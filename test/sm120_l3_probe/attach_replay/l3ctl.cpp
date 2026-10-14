// l3ctl.cpp — Linux sm120 Level-3 attach-replay probe (phase B)
//
// 流程：
//   1. 内嵌 ioctl 拦截器学习本进程 RM 句柄（client / device / TSG）
//      —— TSG 通过 cuxtra 的 cuXtraGetTimeslice 的 NVOS54 报文捕获（hObject）
//   2. 分配 class 0x83de 会话对象（参数用捕获模板的最简形式 {0, hClient, hObject, 0}）
//   3. 会话方法：031f=1 / 0307=1 / 032a=1 后做 0317(suspend) / 0318(resume) 计时实验
//      —— 用一个 clock64 定时 3 秒的永旋内核：suspend 2 秒应使内核完成时间 +2s
//
// 用法: sudo/普通:  ./l3ctl [k.ptx]
#define _GNU_SOURCE
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>

extern "C" {
#include <cuxtra/cuxtra.h>
}

// ------------------------- CUDA driver API -------------------------
typedef int CUresult2;
typedef int CUdevice2;
typedef void* CUcontext2;
typedef void* CUmodule2;
typedef void* CUfunction2;
typedef void* CUstream2;
using p_cuInit_t              = CUresult2 (*)(unsigned);
using p_cuDeviceGet_t         = CUresult2 (*)(CUdevice2*, int);
using p_cuCtxCreate_t         = CUresult2 (*)(CUcontext2*, unsigned, CUdevice2);
using p_cuStreamCreate_t      = CUresult2 (*)(CUstream2*, unsigned);
using p_cuModuleLoad_t        = CUresult2 (*)(CUmodule2*, const char*);
using p_cuModuleGetFunction_t = CUresult2 (*)(CUfunction2*, CUmodule2, const char*);
using p_cuLaunchKernel_t      = CUresult2 (*)(CUfunction2, unsigned,unsigned,unsigned, unsigned,unsigned,unsigned,
                                              unsigned, CUstream2, void**, void**);
using p_cuStreamQuery_t       = CUresult2 (*)(CUstream2);
using p_cuStreamSynchronize_t = CUresult2 (*)(CUstream2);
using p_cuDeviceGetAttribute_t= CUresult2 (*)(int*, int, CUdevice2);
using p_cuCtxSynchronize_t    = CUresult2 (*)();

static p_cuInit_t               p_cuInit;
static p_cuDeviceGet_t          p_cuDeviceGet;
static p_cuCtxCreate_t          p_cuCtxCreate;
static p_cuStreamCreate_t       p_cuStreamCreate;
static p_cuModuleLoad_t         p_cuModuleLoad;
static p_cuModuleGetFunction_t  p_cuModuleGetFunction;
static p_cuLaunchKernel_t       p_cuLaunchKernel;
static p_cuStreamQuery_t        p_cuStreamQuery;
static p_cuStreamSynchronize_t  p_cuStreamSynchronize;
static p_cuDeviceGetAttribute_t p_cuDeviceGetAttribute;
static p_cuCtxSynchronize_t     p_cuCtxSynchronize;

// ------------------------- learned handles -------------------------
struct Learned {
    std::mutex mtx;
    int app_fd = -1;
    uint32_t last_memobj = 0;
    uint32_t last_alloc_obj = 0; uint32_t last_alloc_cls = 0;
    struct RA { uint32_t obj; uint32_t cls; } recent[64]; int n_recent = 0;
    uint32_t clients[8] = {}; int n_clients = 0;
    uint32_t client = 0;
    uint32_t device = 0;        // class 0x80
    uint32_t subdevice = 0;     // class 0x2080
    uint32_t tsg = 0;           // hObject seen in timeslice NVOS54
    uint32_t tsg_client = 0;
    uint32_t max_obj_idx = 0;
    uint32_t all_objs[256][2];  // {handle, class}
    int n_objs = 0;
};
static Learned g;

// ------------------------- ioctl interposer -------------------------
static int (*real_ioctl)(int, unsigned long, ...) = nullptr;

static void note_obj(uint32_t h, uint32_t cls) {   // caller must hold g.mtx
    if ((h & 0xFFFFFF00u) == 0x5c000000u && (h & 0xFF) > g.max_obj_idx) g.max_obj_idx = h & 0xFF;
    if (g.n_objs < 256) { g.all_objs[g.n_objs][0] = h; g.all_objs[g.n_objs][1] = cls; g.n_objs++; }
    if (cls == 0x80) g.device = h;
    if (cls == 0x2080) g.subdevice = h;
}

extern "C" int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void* arg = va_arg(ap, void*); va_end(ap);
    if (!real_ioctl) real_ioctl = (int(*)(int,unsigned long,...))dlsym(RTLD_NEXT, "ioctl");

    // classify fd
    bool is_nv = false;
    char path[64], link[128];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, link, sizeof(link)-1);
    if (n > 0) { link[n] = 0; is_nv = strstr(link, "nvidia") != nullptr; }
    if (!is_nv) return real_ioctl(fd, req, arg);

    unsigned char pre[512]; memset(pre, 0, sizeof pre);
    int size = (req>>16)&0x3fff; if (size > (int)sizeof(pre)) size = sizeof(pre);
    if (((req>>30)&3) != 0 && arg && size > 0) memcpy(pre, arg, size);

    int r = real_ioctl(fd, req, arg);

    if (req == 0xc030462b && arg && size >= 48) {   // RM_ALLOC
        uint32_t hClass; memcpy(&hClass, pre+12, 4);
        uint32_t post_hObj; memcpy(&post_hObj, (char*)arg+8, 4);
        uint32_t post_p4;   memcpy(&post_p4,  (char*)arg+4, 4);
        std::lock_guard<std::mutex> lk(g.mtx);
        if (hClass == 0) {
            fprintf(stderr, "[interpose:zero-alloc] r=%d post=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                r, ((unsigned char*)arg)[0], ((unsigned char*)arg)[1], ((unsigned char*)arg)[2], ((unsigned char*)arg)[3],
                ((unsigned char*)arg)[4], ((unsigned char*)arg)[5], ((unsigned char*)arg)[6], ((unsigned char*)arg)[7],
                ((unsigned char*)arg)[8], ((unsigned char*)arg)[9], ((unsigned char*)arg)[10], ((unsigned char*)arg)[11]);
        }
        if (hClass == 0 && r == 0) {
            uint32_t newcli; memcpy(&newcli, (char*)arg+8, 4);
            if (newcli != 0 && g.n_clients < 8) {
                g.clients[g.n_clients++] = newcli;
                if (g.client == 0) g.client = newcli;
                fprintf(stderr, "[interpose] client[%d]=0x%x (zero-alloc; post+12=%#x)\n", g.n_clients-1, newcli, post_hObj);
            }
        } else if (hClass != 0) {
            note_obj(post_hObj, hClass);
            if (hClass == 0x3e) g.last_memobj = post_hObj;
            g.last_alloc_obj = post_hObj; g.last_alloc_cls = hClass;
            if (hClass == 0x40 || hClass == 0x3e) { g.recent[g.n_recent % 64].obj = post_hObj; g.recent[g.n_recent % 64].cls = hClass; g.n_recent++; }
        }
    } else if (req == 0xc020462a && size >= 32) {   // RM_CONTROL
        uint32_t hClient, hObj, cmd;
        memcpy(&hClient, pre+0, 4); memcpy(&hObj, pre+4, 4); memcpy(&cmd, pre+8, 4);
        {
            std::lock_guard<std::mutex> lk(g.mtx);
            g.app_fd = fd;   // libcuda 的 RM 通道 fd
            if ((cmd & 0xFFFF0000u) == 0xA06C0000u) {   // timeslice family -> TSG!
                if (g.tsg == 0) fprintf(stderr, "[interpose] TSG=0x%x (timeslice cmd %#x)\n", hObj, cmd);
                g.tsg = hObj; g.tsg_client = hClient;
            }
        }
    } else if (req == 0xc0104629) {                 // RM_FREE
        // ignore
    }
    return r;
}

// ------------------------- RM helpers -------------------------
#pragma pack(push, 1)
struct NVOS64 { uint32_t hClient, hParent, hObject, hClass; uint64_t pAlloc, pRights;
                uint32_t paramsSize, status, pad0, pad1; };
struct NVOS54 { uint32_t hClient, hObject, cmd, flags; uint64_t params;
                uint32_t paramsSize, status; };
#pragma pack(pop)

static int g_fd = -1;

static int rm_alloc(uint32_t hClient, uint32_t hParent, uint32_t hClass, void* palloc, uint32_t* out_obj, uint32_t hObjHint) {
    NVOS64 req = {};
    req.hClient = hClient; req.hParent = hParent; req.hObject = hObjHint; req.hClass = hClass;
    req.pAlloc = (uint64_t)(uintptr_t)palloc;
    int r = real_ioctl(g_fd, 0xc030462bUL, &req);
    if (out_obj) *out_obj = req.hObject;
    return (r == 0 && req.status == 0) ? 0 : (r != 0 ? r : (int)req.status);
}

static int rm_control(uint32_t hClient, uint32_t hObj, uint32_t cmd, void* params, uint32_t psize) {
    NVOS54 req = {};
    req.hClient = hClient; req.hObject = hObj; req.cmd = cmd;
    req.params = (uint64_t)(uintptr_t)params; req.paramsSize = psize;
    int r = real_ioctl(g_fd, 0xc020462aUL, &req);
    return (r == 0 && req.status == 0) ? 0 : (r != 0 ? r : (int)req.status);
}

static int rm_free(uint32_t hClient, uint32_t hObj) {
    struct { uint32_t hClient, hObject, hParent, pad; } req = { hClient, hObj, hObj, 0 };
    int r = real_ioctl(g_fd, 0xc0104629UL, &req);
    return r;
}

// ------------------------- log -------------------------
static double now_s() {
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
#define LOG(...) do { printf("[%7.3f] ", now_s()); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

// ------------------------- main -------------------------
int main(int argc, char** argv) {
    const char* ptx = argc > 1 ? argv[1] : "k.ptx";
    LOG("=== l3ctl phase-B probe ===");

    void* cuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!cuda) { LOG("dlopen libcuda failed: %s", dlerror()); return 1; }
#define LOADF(n, v) p_##n = (p_##n##_t)dlsym(cuda, v); if (!p_##n) { LOG("dlsym %s missing", v); return 1; }
    LOADF(cuInit, "cuInit")
    LOADF(cuDeviceGet, "cuDeviceGet")
    LOADF(cuCtxCreate, "cuCtxCreate_v2")
    LOADF(cuStreamCreate, "cuStreamCreate")
    LOADF(cuModuleLoad, "cuModuleLoad")
    LOADF(cuModuleGetFunction, "cuModuleGetFunction")
    LOADF(cuLaunchKernel, "cuLaunchKernel")
    LOADF(cuStreamQuery, "cuStreamQuery")
    LOADF(cuStreamSynchronize, "cuStreamSynchronize")
    LOADF(cuDeviceGetAttribute, "cuDeviceGetAttribute")
    LOADF(cuCtxSynchronize, "cuCtxSynchronize")
#undef LOADF

    auto has83de0 = [&]() -> uint32_t {
        std::lock_guard<std::mutex> lk(g.mtx);
        for (int i = 0; i < g.n_objs; i++) if (g.all_objs[i][1] == 0x83de) return g.all_objs[i][0];
        return 0;
    };
    if (p_cuInit(0)) { LOG("cuInit failed"); return 1; }
    LOG("[bisect] after cuInit: has83de=0x%x objs=%d", has83de0(), g.n_objs);
    CUdevice2 dev; if (p_cuDeviceGet(&dev, 0)) { LOG("cuDeviceGet failed"); return 1; }
    CUcontext2 ctx; if (p_cuCtxCreate(&ctx, 0, dev)) { LOG("cuCtxCreate failed"); return 1; }
    LOG("[bisect] after ctxCreate: has83de=0x%x objs=%d", has83de0(), g.n_objs);
    CUstream2 stream; if (p_cuStreamCreate(&stream, 0)) { LOG("cuStreamCreate failed"); return 1; }
    CUmodule2 mod; if (p_cuModuleLoad(&mod, ptx)) { LOG("cuModuleLoad failed (%s)", ptx); return 1; }
    LOG("[bisect] after moduleLoad: has83de=0x%x objs=%d", has83de0(), g.n_objs);
    CUfunction2 spin; if (p_cuModuleGetFunction(&spin, mod, "spin_until")) { LOG("GetFunction failed"); return 1; }
    int kHz = 0; p_cuDeviceGetAttribute(&kHz, 13 /*CU_DEVICE_ATTRIBUTE_CLOCK_RATE*/, dev);
    LOG("ctx=%p stream=%p clk=%d MHz", ctx, stream, kHz/1000);

    auto dump_learn = [&](const char* tag) {
        std::lock_guard<std::mutex> lk(g.mtx);
        LOG("[learn:%s] client=0x%x tsg_client=0x%x tsg=0x%x device=0x%x objs=%d maxidx=0x%x",
            tag, g.client, g.tsg_client, g.tsg, g.device, g.n_objs, g.max_obj_idx);
        for (int i = 0; i < g.n_objs; i++) {
            if (g.all_objs[i][1] == 0x83de || g.all_objs[i][1] == 0x90f1 || g.all_objs[i][1] == 0x50a0)
                LOG("   %s obj[%d] = 0x%08x class=0x%x", tag, i, g.all_objs[i][0], g.all_objs[i][1]);
        }
    };
    auto has83de = [&]() -> uint32_t {
        std::lock_guard<std::mutex> lk(g.mtx);
        for (int i = 0; i < g.n_objs; i++) if (g.all_objs[i][1] == 0x83de) return g.all_objs[i][0];
        return 0;
    };
    dump_learn("before-cuxtra");
    LOG("[has83de] before-cuxtra: 0x%x", has83de());
    if (!getenv("L3CTL_SKIP_CUXTRA")) {
        size_t ts = cuXtraGetTimeslice((CUcontext)ctx);
        LOG("cuXtraGetTimeslice = %zu us", ts);
        LOG("[has83de] after-cuxtra: 0x%x", has83de());
    } else {
        LOG("[skip-cuxtra] skipped cuxtra call");
        LOG("[has83de] after-skip: 0x%x", has83de());
    }

    g_fd = open("/dev/nvidiactl", O_RDWR);
    if (g_fd < 0) { LOG("open /dev/nvidiactl failed: %s", strerror(errno)); return 1; }
    if (g.app_fd >= 0) { g_fd = g.app_fd; LOG("[learn] using libcuda fd=%d for RM calls", g_fd); }

    // 等待学习完成（cuxtra 的调用与 libcuda 初始化都在当前线程完成，这里直接读）
    uint32_t client, tsg, device;
    (void)0;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        // 与 capture 一致：主工作 client = 第二个零分配 client（第一个常用于探测）
        client = (g.n_clients >= 2) ? g.clients[1] : (g.client ? g.client : g.tsg_client);
        tsg = g.tsg; device = g.device;
        LOG("[learn] client=0x%x tsg=0x%x device=0x%x objs=%d maxidx=0x%x", client, tsg, device, g.n_objs, g.max_obj_idx);
        for (int i = 0; i < g.n_objs; i++) LOG("   obj[%d] = 0x%08x class=0x%x", i, g.all_objs[i][0], g.all_objs[i][1]);
    }
    if (!client || !tsg || !device) { LOG("[abort] missing handles (client/tsg/device)"); return 2; }

    // ---- 0xD04 client announce (prelude from captures: {cl,1,0x00040003} + {cl,4,0x00040003}) ----
    {
        struct { uint32_t cl, a, b; } ap;
        ap.cl = client; ap.a = 1; ap.b = 0x00040003;
        int r1 = rm_control(client, client, 0x00000d04, &ap, 12);
        ap.a = 4;
        int r2 = rm_control(client, client, 0x00000d04, &ap, 12);
        LOG("[announce] 0xD04 {cl,1,3/4} rc=%d,%d", r1, r2);
    }

    // ---- session alloc attempts (variants) ----
    struct Variant { const char* name; uint32_t p[16]; };
    uint32_t hObjHint = 0x5c000000u | (g.max_obj_idx + 1);
    LOG("[session] hObjHint=0x%x", hObjHint);
    uint32_t session = 0;
    int rc = -1;
    for (int v = 0; v < 3 && rc != 0; v++) {
        uint32_t p[0x40/4]; memset(p, 0, sizeof p);
        p[0] = 0; p[1] = client; p[2] = tsg; p[3] = (v == 0) ? 0 : 0x7fff;
        rc = rm_alloc(client, device, 0x83de, p, &session, hObjHint);
        LOG("[session] variant%d ({0,client,tsg,%#x}) rc=%d hObj=0x%x", v, p[3], rc, session);
        if (rc != 0) { hObjHint++; }
        else { LOG("[session] *** SESSION CREATED: 0x%x ***", session); }
    }
    if (rc != 0) { LOG("[abort] session alloc failed (rc=%d)", rc); return 3; }

    // ---- session mode prelude (captured agent sequence: 031f=1, 0307=1, 032a=1) ----
    {
        uint32_t one = 1;
        int r1 = rm_control(client, session, 0x83de031f, &one, 4);
        int r2 = rm_control(client, session, 0x83de0307, &one, 4);
        int r3 = rm_control(client, session, 0x83de032a, &one, 4);
        LOG("[prelude] 031f=1 rc=%d  0307=1 rc=%d  032a=1 rc=%d", r1, r2, r3);
    }

    // ---- kernel launch helper (baseline) ----
    auto run_spin = [&](double seconds, const char* tag) -> double {
        unsigned long long cycles = (unsigned long long)kHz * 1000ull * (unsigned long long)(seconds * 1000) / 1000ull;
        void* args[] = { &cycles };
        double t0 = now_s();
        if (p_cuLaunchKernel(spin, 1,1,1, 32,1,1, 0, stream, args, nullptr)) { LOG("%s: launch failed", tag); return -1; }
        int sr = p_cuStreamSynchronize(stream);
        double dt = now_s() - t0;
        LOG("%s: sync=%d elapsed=%.3fs", tag, sr, dt);
        return dt;
    };

    // ---- E10b: counter-based suspend verdict (session 0x73 = cuCtxCreate 自动创建) ----
    {
        using p_cuMemAlloc_t = CUresult2 (*)(unsigned long long*, size_t);
        using p_cuMemsetD8_t = CUresult2 (*)(unsigned long long, unsigned char, size_t);
        auto cuMemAlloc = (p_cuMemAlloc_t)dlsym(cuda, "cuMemAlloc_v2");
        auto cuMemsetD8 = (p_cuMemsetD8_t)dlsym(cuda, "cuMemsetD8_v2");
        CUfunction2 fcounter; p_cuModuleGetFunction(&fcounter, mod, "spin_counter");
        uint32_t memobj = 0;
        {
            std::lock_guard<std::mutex> lk(g.mtx);
            g.last_memobj = 0;
        }
        unsigned long long dptr = 0;
        if (!(cuMemAlloc && cuMemsetD8 && fcounter && cuMemAlloc(&dptr, 4096) == 0)) { LOG("[e10b] setup fail"); }
        else {
            cuMemsetD8(dptr, 0, 4096);
            { std::lock_guard<std::mutex> lk(g.mtx); memobj = g.last_memobj ? g.last_memobj : g.last_alloc_obj;
              LOG("[e10b] last_alloc: obj=0x%x class=0x%x", g.last_alloc_obj, g.last_alloc_cls); }
            uint32_t presess = 0, prec2;
            for (int i = 0; i < g.n_objs; i++) if (g.all_objs[i][1] == 0x83de && g.all_objs[i][0] != session) presess = g.all_objs[i][0];
            { std::lock_guard<std::mutex> lk(g.mtx); prec2 = g.n_clients>=2?g.clients[1]:g.tsg_client; }
            LOG("[e10b] dptr=%#llx memobj=0x%x presess=0x%x cli=0x%x", dptr, memobj, presess, prec2);
            void* args[] = { &dptr };
            int lr = p_cuLaunchKernel(fcounter, 1,1,1, 32,1,1, 0, stream, args, nullptr);
            LOG("[e10b] launch rc=%d", lr);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto read315 = [&](const char* tag) -> unsigned long long {
                unsigned long long val = 0xdead;
                struct { uint32_t hMem; uint32_t size; uint64_t off; uint64_t host; } rp = { memobj, 8, 0, (uint64_t)(uintptr_t)&val };
                int rc = rm_control(prec2, presess, 0x83de0315, &rp, 24);
                printf("[%7.3f] [e10b] %s: 0315 rc=%d val=%llu\n", now_s(), tag, rc, val); fflush(stdout);
                return val;
            };
            unsigned long long v1 = read315("v1");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            unsigned long long v2 = read315("v2");
            uint64_t z = 0;
            int r1 = rm_control(prec2, presess, 0x83de0317, &z, 8);
            printf("[%7.3f] [e10b] SUSPEND(flag=0) rc=%d\n", now_s(), r1); fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            unsigned long long v3 = read315("v3");
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            unsigned long long v4 = read315("v4");
            int r2 = rm_control(prec2, presess, 0x83de0318, nullptr, 0);
            printf("[%7.3f] [e10b] RESUME rc=%d\n", now_s(), r2); fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            unsigned long long v5 = read315("v5");
            LOG("[e10b] v1=%llu v2=%llu v3=%llu v4=%llu v5=%llu", v1, v2, v3, v4, v5);
            LOG("[e10b] VERDICT: running(v2>v1)=%s  suspended(v4==v3&&v3>v1)=%s  resumed(v5>v4)=%s",
                (v2>v1?"YES":"no"), ((v4==v3&&v3>=v1)?"YES":"no"), (v5>v4?"YES":"no"));
            if (!(v4==v3)) {
                // 模式开启序列（captured prelude）后重测
                {
                    struct { uint32_t m; const char* n; } modes[] = {
                        {0x83de031f,"031f ErrbarDebug"},{0x83de0307,"0307 MMUDebug"},
                        {0x83de032a,"032a MMUGccDebug"},{0x83de0301,"0301 SMDebugModeEnable"}};
                    uint32_t one = 1;
                    for (auto& mm : modes) {
                        int rc = rm_control(prec2, presess, mm.m, &one, 4);
                        printf("[%7.3f] [e10b] mode %s rc=%d\n", now_s(), mm.n, rc); fflush(stdout);
                    }
                    uint64_t z2 = 0;
                    int rs = rm_control(prec2, presess, 0x83de0317, &z2, 8);
                    printf("[%7.3f] [e10b] SUSPEND(after modes) rc=%d\n", now_s(), rs); fflush(stdout);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    unsigned long long m1 = read315("m1");
                    std::this_thread::sleep_for(std::chrono::milliseconds(600));
                    unsigned long long m2 = read315("m2");
                    int rr = rm_control(prec2, presess, 0x83de0318, nullptr, 0);
                    LOG("[e10b] after-modes: suspended(m2==m1)=%s (m1=%llu m2=%llu) resume rc=%d",
                        (m2==m1?"YES!":"no"), m1, m2, rr);
                }
                // 再试 flag=1
                uint64_t one = 1;
                int r3 = rm_control(prec2, presess, 0x83de0317, &one, 8);
                printf("[%7.3f] [e10b] SUSPEND(flag=1) rc=%d\n", now_s(), r3); fflush(stdout);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                unsigned long long w1 = read315("w1");
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                unsigned long long w2 = read315("w2");
                int r4 = rm_control(prec2, presess, 0x83de0318, nullptr, 0);
                LOG("[e10b] flag=1 verdict: rc=%d suspended(w2==w1)=%s resume rc=%d", r3, (w2==w1?"YES":"no"), r4);
            }
        }
    }

    // ---- E15: stop-trigger matrix with pinned-memory async sampler ----
    {
        uint32_t presess = 0, prec2;
        for (int i = 0; i < g.n_objs; i++) if (g.all_objs[i][1] == 0x83de && g.all_objs[i][0] != session) presess = g.all_objs[i][0];
        { std::lock_guard<std::mutex> lk(g.mtx); prec2 = g.n_clients>=2?g.clients[1]:g.tsg_client; }
        using p_cuMemAlloc_t = CUresult2 (*)(unsigned long long*, size_t);
        using p_cuMemsetD8_t = CUresult2 (*)(unsigned long long, unsigned char, size_t);
        using p_cuMemHostAlloc_t = CUresult2 (*)(void**, size_t, unsigned);
        using p_cuMemcpyDtoHAsync_t = CUresult2 (*)(void*, unsigned long long, size_t, CUstream2);
        auto cuMemAlloc = (p_cuMemAlloc_t)dlsym(cuda, "cuMemAlloc_v2");
        auto cuMemsetD8 = (p_cuMemsetD8_t)dlsym(cuda, "cuMemsetD8_v2");
        auto cuMemHostAlloc = (p_cuMemHostAlloc_t)dlsym(cuda, "cuMemHostAlloc");
        auto cuMemcpyDtoHAsync = (p_cuMemcpyDtoHAsync_t)dlsym(cuda, "cuMemcpyDtoHAsync_v2");
        CUfunction2 fcounter; p_cuModuleGetFunction(&fcounter, mod, "spin_counter");
        CUstream2 s2; p_cuStreamCreate(&s2, 0);   // kernel stream
        CUstream2 sc; p_cuStreamCreate(&sc, 0);   // copy stream (must be clean!)
        unsigned long long dptr = 0; void* pinned = nullptr;
        if (cuMemAlloc && cuMemHostAlloc && cuMemcpyDtoHAsync && cuMemAlloc(&dptr, 4096) == 0 && cuMemHostAlloc(&pinned, 4096, 0) == 0) {
            *(volatile unsigned long long*)pinned = 111;
            void* args[] = { &dptr };
            p_cuLaunchKernel(fcounter, 1,1,1, 32,1,1, 0, s2, args, nullptr);
            auto sample = [&](const char* tag) -> unsigned long long {
                cuMemcpyDtoHAsync(pinned, dptr, 8, sc);
                p_cuStreamSynchronize(sc);
                unsigned long long v = *(volatile unsigned long long*)pinned;
                printf("[%7.3f] [e15] %s sample=%llu\n", now_s(), tag, v); fflush(stdout);
                return v;
            };
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            unsigned long long a0 = sample("a0");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            unsigned long long a1 = sample("a1");
            LOG("[e15] baseline growing=%s (%llu -> %llu)", (a1>a0?"YES":"no"), a0, a1);
            uint32_t one = 1;
            for (uint32_t m : {0x83de031f, 0x83de0307, 0x83de032a}) rm_control(prec2, presess, m, &one, 4);
            struct T { uint32_t m; uint32_t a; uint32_t b; const char* n; };
            T tests[] = {
                {0x83de0313, 1, 0, "0313 type=1"}, {0x83de0313, 2, 0, "0313 type=2"}, {0x83de0313, 3, 0, "0313 type=3"},
                {0x83de0322, 0, 1, "0322 sm0 on1"}, {0x83de0322, 0xffffffff, 1, "0322 all on1"},
                {0x83de0321, 0, 1, "0321 sm0 on1"}, {0x83de0321, 0xffffffff, 1, "0321 all on1"},
                {0x83de0324, 0, 1, "0324 sm0 skip1"},
            };
            for (auto& t : tests) {
                uint64_t pr = ((uint64_t)t.b << 32) | t.a;
                int rc = rm_control(prec2, presess, t.m, &pr, 8);
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                unsigned long long b0 = sample("b0");
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                unsigned long long b1 = sample("b1");
                bool frozen = (b0 == b1);
                printf("[%7.3f] [e15] %s rc=%d => frozen=%s (%llu -> %llu)\n", now_s(), t.n, rc, (frozen?"*** YES ***":"no"), b0, b1); fflush(stdout);
                if (frozen) {
                    uint64_t r0 = 0;
                    int rr = rm_control(prec2, presess, 0x83de0323, &r0, 8);
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    unsigned long long r1 = sample("resume?");
                    LOG("[e15] FOUND STOP METHOD! %s rc=%d 0323 RunTrigger rc=%d resumed=%s", t.n, rc, rr, (r1>b1?"YES":"no"));
                    break;
                }
            }
            uint64_t z = 0;
            rm_control(prec2, presess, 0x83de0323, &z, 8);
        } else LOG("[e15] setup fail");
    }

    LOG("--- baseline run (3s spin, no suspend) ---");    LOG("--- baseline run (3s spin, no suspend) ---");    LOG("--- baseline run (3s spin, no suspend) ---");
    double base = run_spin(3.0, "baseline");

    // ---- CLEAN suspend/resume test on pre-existing session (0x5c000073) ----
    {
        uint32_t presess = 0, prec1 = 0, prec2 = 0;
        for (int i = 0; i < g.n_objs; i++) if (g.all_objs[i][1] == 0x83de && g.all_objs[i][0] != session) presess = g.all_objs[i][0];
        std::lock_guard<std::mutex> lk(g.mtx);
        prec1 = g.n_clients >= 1 ? g.clients[0] : 0;
        prec2 = g.n_clients >= 2 ? g.clients[1] : g.tsg_client;
        LOG("[e9] pre-session=0x%x clients=0x%x/0x%x", presess, prec1, prec2);
        if (presess) {
            unsigned long long cycles = (unsigned long long)kHz * 1000ull * 3ull;
            void* args[] = { &cycles };
            double t0 = now_s();
            int lr = p_cuLaunchKernel(spin, 1,1,1, 32,1,1, 0, stream, args, nullptr);
            LOG("[e9] launch rc=%d", lr);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            uint64_t z = 0;
            int r1 = rm_control(prec2, presess, 0x83de0317, &z, 8);
            LOG("[e9] t=%.2f SUSPEND rc=%d", now_s()-t0, r1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            int q1 = p_cuStreamQuery(stream);
            LOG("[e9] t=%.2f (after 1s hold) query=%d (%s)", now_s()-t0, q1, q1==1?"NOT_READY(spin仍在跑=未挂起)":(q1==0?"READY(已结束=挂起失败或已完成)":"ERR"));
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            int q2 = p_cuStreamQuery(stream);
            LOG("[e9] t=%.2f (after 2.5s hold) query=%d", now_s()-t0, q2);
            int r2 = rm_control(prec2, presess, 0x83de0318, nullptr, 0);
            LOG("[e9] t=%.2f RESUME rc=%d", now_s()-t0, r2);
            int sr = p_cuStreamSynchronize(stream);
            double dt = now_s() - t0;
            LOG("[e9] sync=%d elapsed=%.3fs (baseline=%.3fs; 预期 ~5.0s => 挂起生效)", sr, dt, base);
        }
    }

    // ---- cleanup: 031f=0 + free session ----
    { uint32_t zz=0, ps=0; for (int i=0;i<g.n_objs;i++) if (g.all_objs[i][1]==0x83de && g.all_objs[i][0]!=session) ps=g.all_objs[i][0];
      if (ps) rm_control(g.n_clients>=2?g.clients[1]:client, ps, 0x83de0318, nullptr, 0); }
    uint32_t off = 0;
    rm_control(client, session, 0x83de031f, &off, 4);
    int rf = rm_free(client, session);
    LOG("cleanup: 031f=0, free rc=%d", rf);
    LOG("=== l3ctl done ===");
    fflush(stdout);
    _exit(0);
}
