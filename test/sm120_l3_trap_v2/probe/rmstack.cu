// rmstack.cu - build a private RM client stack (client -> device -> subdevice ->
// KernelSMDebuggerSession) inside the application process and use it to suspend
// a *running* kernel, exactly the way cuda-gdb's debug agent does.
//
// What made v1 / l3dbg fail is now known from evidence/live/r3_ioctl.log:
//
//   app (client 0xc1d00150):
//     0x5c000002 cls=0x80    device      params {hClientShare=0, hTargetClient=<own client>, ...}
//     0x5c000003 cls=0x2080  subdevice
//     0x5c000013 cls=0xa06c  TSG
//     0x5c00001a cls=0xc96f  channel
//     0x5c00001b cls=0xcec0  compute object   <-- "hClass3dObject"
//     0x5c000073 cls=0x83de  session params {0, 0xc1d00150, 0x5c00001b}
//   cuda-gdb (client 0xc1d0014c, its own fd/device/subdevice):
//     0x5c00000c cls=0x83de  session params {0, 0xc1d00150, 0x5c00001b}   <-- same triplet
//     then 031f=1 0307=1 032a=1, then 0317 params {1,0}  => device FREEZES
//     and  0318 params {}                                => device RESUMES
//
// Two corrections over the previous attempt:
//   * NV0080_ALLOC_PARAMETERS.hTargetClient must be our own client handle
//     (passing an all-zero blob gives NV_ERR_INVALID_OBJECT_HANDLE 0x33).
//   * the 0x83de session params are {hDebuggerClient=0, hAppClient, hClass3dObject}
//     naming the *application's* client and compute object, not zeros.
//
// Build: ../build.sh rmstack     Run:
//   CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1 ./rmstack [opts]
//     --parent device|subdev      parent of the 0x83de session (default device)
//     --app-device                use libcuda's device object as parent instead
//                                 of building our own stack
//     --obj channel|compute|tsg   which app object to name in hClass3dObject
//     --no-modes / --flag N / --hold-ms N / --spin-ms N / --blocks N

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <cuda_runtime.h>

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)

// --------------------------------------------------------------- RM ABI
struct NVOS21 { uint32_t hRoot, hParent, hObject, hClass;
                uint64_t pAllocParms, pRightsRequested;
                uint32_t paramsSize, flags; int32_t status; };          // 48 B
struct NVOS54 { uint32_t hClient, hObject, cmd, flags;
                uint64_t params; uint32_t paramsSize; int32_t status; }; // 32 B
struct NVOS00 { uint32_t hRoot, hParent, hObject; int32_t status; };     // 16 B
struct NVOS55 { uint32_t hRoot, hParent, hObject, hClientSrc, hObjSrc, flags;
                int32_t status; };                                       // 28 B
struct NV0080 { uint32_t hClientShare, hTargetClient, hTargetDevice, deviceId,
                           hVASpace, hInternalDevice;
                uint64_t vaSpaceSize;
                uint32_t internaldevicehandle, vaStartInternal;
                uint64_t vaLimitInternal; uint32_t vaMode; };            // NV0080_ALLOC_PARAMETERS
struct NV83DE { uint32_t hDebuggerClient, hAppClient, hClass3dObject;
                uint32_t pad[5]; };                                      // captured: 3 handles
static_assert(sizeof(NVOS21) == 48 && sizeof(NVOS54) == 32 && sizeof(NVOS00) == 16 &&
                     sizeof(NVOS55) == 28,
              "RM struct layout must match the captured ioctl sizes");

static const unsigned long NV_ESC_RM_ALLOC    = 0xc030462bUL;
static const unsigned long NV_ESC_RM_CONTROL  = 0xc020462aUL;
static const unsigned long NV_ESC_RM_FREE     = 0xc0104629UL;
static const unsigned long NV_ESC_RM_DUP      = 0xc01c4634UL;
static const unsigned long NV_ESC_CARD_INFO   = 0xc90046c8UL;

static const uint32_t CLASS_ROOT = 0x41, CLASS_DEVICE = 0x80, CLASS_SUBDEV = 0x2080,
                      CLASS_TSG = 0xa06c, CLASS_CHANNEL = 0xc96f, CLASS_COMPUTE = 0xcec0,
                      CLASS_SMDBGSSN = 0x83de;
static const uint32_t CMD_MODE_ERRBAR = 0x83de031f, CMD_MODE_MMU = 0x83de0307,
                      CMD_MODE_MMUGCC = 0x83de032a, CMD_SUSPEND = 0x83de0317,
                      CMD_RESUME = 0x83de0318;
// Names recovered from __nvoc_exported_method_def_KernelSMDebuggerSession in
// nv-kernel.o_binary (see FINDINGS R3 table).
static const uint32_t CMD_SM_DEBUG_MODE_ENABLE  = 0x83de0301;  // psize 0
static const uint32_t CMD_SM_DEBUG_MODE_DISABLE = 0x83de0302;  // psize 0
static const uint32_t CMD_SET_EXC_MASK2         = 0x83de0309;  // psize 4
static const uint32_t CMD_READ_ALL_SM_ESR       = 0x83de030c;  // psize 4824
static const uint32_t CMD_SET_STOP_TRIGGER      = 0x83de0313;  // psize 4
static const uint32_t CMD_GET_MAPPINGS          = 0x83de031c;  // psize 1048
static const uint32_t CMD_EXEC_REG_OPS          = 0x83de031d;  // psize 3208

// ------------------------------------------------- learned libcuda handles
struct Learned {
    int fd = -1;
    std::vector<uint32_t> clients;
    uint32_t device = 0, subdev = 0, tsg = 0, channel = 0, compute = 0, app_session = 0;
    uint32_t app_client = 0;      // client that owns the device object
    std::vector<uint32_t> computes, channels, tsgs, subdevs;
} L;

static int (*real_ioctl)(int, unsigned long, ...) = nullptr;
static __thread int in_hook = 0;

static void learn(int fd, void *arg)
{
    NVOS21 a;
    memcpy(&a, arg, sizeof a);
    if (a.status) return;
    switch (a.hClass) {
    case CLASS_ROOT:    if (L.fd < 0) L.fd = fd; L.clients.push_back(a.hObject); break;
    case CLASS_DEVICE:  if (!L.device) { L.device = a.hObject; L.app_client = a.hRoot; }
                        if (L.fd < 0) L.fd = fd; break;
    case CLASS_SUBDEV:  if (!L.subdev) L.subdev = a.hObject; break;
    case CLASS_TSG:     if (!L.tsg) L.tsg = a.hObject; L.tsgs.push_back(a.hObject); break;
    case CLASS_CHANNEL: if (!L.channel) L.channel = a.hObject; L.channels.push_back(a.hObject); break;
    case CLASS_COMPUTE: if (!L.compute) L.compute = a.hObject; L.computes.push_back(a.hObject); break;
    case CLASS_SMDBGSSN:if (!L.app_session) L.app_session = a.hObject; break;
    }
}

extern "C" int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    int r = real_ioctl(fd, req, arg);
    if (in_hook || r || !arg || req != NV_ESC_RM_ALLOC) return r;
    in_hook = 1;
    learn(fd, arg);
    in_hook = 0;
    return r;
}

// --------------------------------------------------------------- RM helpers
static int rm_alloc(int fd, uint32_t root, uint32_t parent, uint32_t obj, uint32_t cls,
                    void *params, uint32_t psize)
{
    NVOS21 a;
    memset(&a, 0, sizeof a);
    a.hRoot = root; a.hParent = parent; a.hObject = obj; a.hClass = cls;
    a.pAllocParms = (uint64_t)(uintptr_t)params; a.paramsSize = psize;
    int r = real_ioctl(fd, NV_ESC_RM_ALLOC, &a);
    return r ? (0x1000000 | r) : a.status;
}
static int rm_dup(int fd, uint32_t root, uint32_t parent, uint32_t obj,
                  uint32_t src_client, uint32_t src_obj)
{
    NVOS55 d;
    memset(&d, 0, sizeof d);
    d.hRoot = root; d.hParent = parent; d.hObject = obj;
    d.hClientSrc = src_client; d.hObjSrc = src_obj;
    int r = real_ioctl(fd, NV_ESC_RM_DUP, &d);
    return r ? (0x1000000 | r) : d.status;
}
static int rm_control(int fd, uint32_t client, uint32_t obj, uint32_t cmd, void *params, uint32_t psize)
{
    NVOS54 c;
    memset(&c, 0, sizeof c);
    c.hClient = client; c.hObject = obj; c.cmd = cmd;
    c.params = (uint64_t)(uintptr_t)params; c.paramsSize = psize;
    int r = real_ioctl(fd, NV_ESC_RM_CONTROL, &c);
    return r ? (0x1000000 | r) : c.status;
}
static int rm_free(int fd, uint32_t root, uint32_t parent, uint32_t obj)
{
    NVOS00 f;
    memset(&f, 0, sizeof f);
    f.hRoot = root; f.hParent = parent; f.hObject = obj;
    int r = real_ioctl(fd, NV_ESC_RM_FREE, &f);
    return r ? (0x1000000 | r) : f.status;
}

// ------------------------------------------------- NV83DE_CTRL_CMD_DEBUGGER_EXEC_REG_OPS
// Layout recovered from ksmdbgssnCtrlCmdDebugExecRegOps_IMPL@0x21a990:
//   regOpCount = *(NvU32*)(params+4), regOps = params+8, count <= 0x64,
//   total param size 0xc88 -> each op is 32 bytes.
// The IMPL also bails out with NV_OK (doing nothing) when a bool at
// KernelGraphics+0x911 is false, so a zero regStatus is NOT proof of success:
// the read-back value has to be sane too.
struct RegOp { uint8_t regOp; uint8_t regStatus; uint16_t regQuad;
               uint32_t regAndNackIndex, regSubIndex, regValue, regMask, regShift,
               regGrpMask, pad; };
struct ExecRegOps { uint8_t flags; uint8_t reserved[3]; uint32_t regOpCount;
                    RegOp ops[100]; };
static_assert(sizeof(ExecRegOps) == 0xc88, "must match the 0x83de031d param size");

// regOp field: type in the high bits, READ=0 / WRITE=1 in bit0
enum { REGOP_GLOBAL = 0x00, REGOP_GR_CTX = 0x01, REGOP_GR_CTX_SM = 0x04,
       REGOP_FB = 0x20, REGOP_WRITE = 0x01 };

static const uint32_t NV_PMC_BOOT_0     = 0x00000000;
static const uint32_t NV_PTIMER_TIME_0  = 0x00200510;
static const uint32_t NV_PTIMER_TIME_1  = 0x00200514;

static void do_regops(int fd, uint32_t client, uint32_t session, bool write_trigger)
{
    // Probe the parameter encoding: gpuValidateRegOps(pGpu, count, ops, flagsByte,
    // 0, 0) walks ops with a 32-byte stride and only returns NV_ERR_INVALID_ARGUMENT
    // when flagsByte != 0, so a 0x1f means the count field itself was misread.
    for (int flags = 0; flags <= 1; flags++) {
        for (uint32_t n = 0; n <= 3; n++) {
            ExecRegOps p; memset(&p, 0, sizeof p);
            p.flags = (uint8_t)flags;
            p.regOpCount = n;
            for (uint32_t i = 0; i < n; i++)
                p.ops[i] = RegOp{REGOP_GLOBAL, 0, 0, NV_PMC_BOOT_0 + i * 4, 0, 0,
                                 0xffffffff, 0, 0, 0};
            int st = rm_control(fd, client, session, CMD_EXEC_REG_OPS, &p, sizeof p);
            printf("[regops] flags=%d count=%u -> %#x", flags, n, st);
            for (uint32_t i = 0; i < n; i++)
                printf("  op[%u] reg=%#x status=%#x val=%#x", i, p.ops[i].regAndNackIndex,
                       p.ops[i].regStatus, p.ops[i].regValue);
            printf("\n");
        }
    }
    if (write_trigger) {
        ExecRegOps p; memset(&p, 0, sizeof p);
        p.regOpCount = 1;
        p.ops[0] = RegOp{(uint8_t)(REGOP_GLOBAL | REGOP_WRITE), 0, 0, 0x419e84,
                         0, 1u << 31, 0xffffffff, 0, 0, 0};
        int st = rm_control(fd, client, session, CMD_EXEC_REG_OPS, &p, sizeof p);
        printf("[regops] write 0x419e84 |= bit31 -> %#x status=%#04x\n", st, p.ops[0].regStatus);
    }
}

// ---------------------------------------------------------------- device side
__global__ void spin_publish(volatile unsigned long long *hc, unsigned long long cycles)
{
    unsigned long long start = clock64(), i = 0;
    while (clock64() - start < cycles) {
        i++;
        if ((i & 0xff) == 0) *hc = i;
    }
    *hc = i | (1ULL << 63);
}

static double now_s()
{
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    long blocks = 8, spin_ms = 12000, hold_ms = 1000;
    uint32_t flag = 1;
    bool modes = true, app_device = false, parent_subdev = false,
         probe_ctl = false, dbgmode = false, regops_write = false, sweep = false;
    std::string objkind = "compute";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--blocks") blocks = atol(next().c_str());
        else if (a == "--spin-ms") spin_ms = atol(next().c_str());
        else if (a == "--hold-ms") hold_ms = atol(next().c_str());
        else if (a == "--flag") flag = (uint32_t)strtoul(next().c_str(), nullptr, 0);
        else if (a == "--no-modes") modes = false;
        else if (a == "--app-device") app_device = true;
        else if (a == "--obj") objkind = next();
        else if (a == "--probe-ctl") probe_ctl = true;
        else if (a == "--dbgmode") dbgmode = true;
        else if (a == "--regops-write") regops_write = true;
        else if (a == "--sweep") sweep = true;
        else if (a == "--parent") { std::string p = next(); parent_subdev = (p == "subdev"); }
    }

    RT_CHECK(cudaFree(0));
    printf("[learn] computes="); for (uint32_t h : L.computes) printf(" %#x", h);
    printf("  channels="); for (uint32_t h : L.channels) printf(" %#x", h);
    printf("  tsgs="); for (uint32_t h : L.tsgs) printf(" %#x", h);
    printf("\n");
    printf("[learn] fd=%d app_client=%#x device=%#x subdev=%#x tsg=%#x channel=%#x compute=%#x app_session=%#x clients=",
           L.fd, L.app_client, L.device, L.subdev, L.tsg, L.channel, L.compute, L.app_session);
    for (uint32_t c : L.clients) printf(" %#x", c);
    printf("\n");
    if (L.fd < 0 || !L.app_client || !L.device) { fprintf(stderr, "[learn] incomplete\n"); return 1; }

    uint32_t target_obj = objkind == "channel" ? L.channel
                        : objkind == "tsg"     ? L.tsg : L.compute;
    if (!target_obj) { fprintf(stderr, "[learn] no %s object captured\n", objkind.c_str()); return 1; }

    // ---------------------------------------------------- our own RM stack
    int fd = L.fd;
    uint32_t client = 0, device = 0, subdev = 0, session = 0;
    const uint32_t H_DEV = 0x5c00f002, H_SUB = 0x5c00f003, H_SESS = 0x5c00f00c,
                   H_SESS2 = 0x5c00f00e;

    // Strategy set for obtaining a KernelSMDebuggerSession that RM will honour.
    // cuda-gdb's session lives in a *separate* RM client from the application's
    // and names the app's client + compute object in its alloc params; a session
    // allocated inside libcuda's own client returns status 0 for 0x83de0317 but
    // does not freeze anything (evidence/live/exp_rmstack_2nddev.log).
    const uint32_t H_CL2 = 0xc1d0f001, H_DUP = 0x5c00f010;
    if (!app_device) {
        int st = rm_alloc(fd, 0, 0, H_CL2, CLASS_ROOT, nullptr, 0);
        printf("[own] client 0x41 -> %#x\n", st);
        if (!st) {
            client = H_CL2;
            st = rm_dup(fd, client, client, H_DUP, L.app_client, L.device);
            printf("[own] DUP device %#x from client %#x -> %#x\n", L.device, L.app_client, st);
            if (!st) {
                device = H_DUP;
                uint32_t sd = 0;
                st = rm_alloc(fd, client, device, H_SUB, CLASS_SUBDEV, &sd, 0);
                printf("[own] subdev 0x2080 under dup'd device -> %#x\n", st);
                if (!st) subdev = H_SUB;
            }
        }
        if (!device) {
            // fall back: second device object inside libcuda's client
            client = L.app_client;
            NV0080 dp; memset(&dp, 0, sizeof dp);
            dp.hTargetClient = client; dp.deviceId = 0;
            int st2 = rm_alloc(fd, client, client, H_DEV, CLASS_DEVICE, &dp, 0);
            printf("[own] fallback 2nd device under libcuda client -> %#x\n", st2);
            if (st2) return 1;
            device = H_DEV;
            uint32_t sd = 0;
            if (!rm_alloc(fd, client, device, H_SUB, CLASS_SUBDEV, &sd, 0)) subdev = H_SUB;
        }
    } else {
        client = L.app_client; device = L.device; subdev = L.subdev;
        printf("[own] reusing libcuda stack: client=%#x device=%#x subdev=%#x\n", client, device, subdev);
    }

    // ---------------------------------------------------- the 0x83de session
    NV83DE sp83; memset(&sp83, 0, sizeof sp83);
    sp83.hDebuggerClient = 0;
    sp83.hAppClient = L.app_client;
    sp83.hClass3dObject = target_obj;
    uint32_t parent = parent_subdev ? subdev : device;
    if (!parent) { fprintf(stderr, "[session] no parent object\n"); return 1; }

    int st = rm_alloc(fd, client, parent, H_SESS, CLASS_SMDBGSSN, &sp83, 0);
    printf("[session] 0x83de client=%#x parent=%#x params={%#x,%#x,%#x} -> %#x\n",
           client, parent, sp83.hDebuggerClient, sp83.hAppClient, sp83.hClass3dObject, st);
    if (st) {
        // fall back to libcuda's own auto-created session
        if (L.app_session) {
            session = L.app_session; client = L.app_client; fd = L.fd;
            printf("[session] falling back to libcuda's %#x\n", session);
        } else return 1;
    } else session = H_SESS;

    if (probe_ctl) {
        // Is the session actually bound to the GPU / to the app's context?
        static unsigned char esr[4824], maps[1048];
        memset(esr, 0, sizeof esr); memset(maps, 0, sizeof maps);
        int st1 = rm_control(fd, client, session, CMD_READ_ALL_SM_ESR, esr, sizeof esr);
        size_t nz = 0; for (size_t i = 0; i < sizeof esr; i++) nz += esr[i] != 0;
        uint32_t *e32 = (uint32_t *)esr;
        printf("[probe] 030c ReadAllSmErrorStates -> %#x  nonzero_bytes=%zu  words[0..7]=", st1, nz);
        for (int i = 0; i < 8; i++) printf(" %08x", e32[i]);
        printf("\n");
        int st2 = rm_control(fd, client, session, CMD_GET_MAPPINGS, maps, sizeof maps);
        nz = 0; for (size_t i = 0; i < sizeof maps; i++) nz += maps[i] != 0;
        uint32_t *m32 = (uint32_t *)maps;
        printf("[probe] 031c GetMappings -> %#x  nonzero_bytes=%zu  words[0..15]=", st2, nz);
        for (int i = 0; i < 16; i++) printf(" %08x", m32[i]);
        printf("\n");
        int st3 = rm_control(fd, client, session, CMD_SM_DEBUG_MODE_ENABLE, nullptr, 0);
        printf("[probe] 0301 SMDebugModeEnable -> %#x\n", st3);
        do_regops(fd, client, session, regops_write);
    }
    if (modes) {
        uint32_t one = 1;
        printf("[modes] 031f=%#x 0307=%#x 032a=%#x\n",
               rm_control(fd, client, session, CMD_MODE_ERRBAR, &one, 4),
               rm_control(fd, client, session, CMD_MODE_MMU, &one, 4),
               rm_control(fd, client, session, CMD_MODE_MMUGCC, &one, 4));
    }

    // ---- victim kernel with an out-of-band counter
    void *hp = nullptr;
    RT_CHECK(cudaHostAlloc(&hp, 64, cudaHostAllocMapped));
    memset(hp, 0, 64);
    volatile unsigned long long *hc = (volatile unsigned long long *)hp;
    unsigned long long *hc_dev = nullptr;
    RT_CHECK(cudaHostGetDevicePointer((void **)&hc_dev, hp, 0));
    cudaStream_t vs;
    RT_CHECK(cudaStreamCreateWithFlags(&vs, cudaStreamNonBlocking));
    cudaDeviceProp prop;
    RT_CHECK(cudaGetDeviceProperties(&prop, 0));
    unsigned long long cycles = (unsigned long long)prop.clockRate * 1000ULL * spin_ms / 1000ULL;

    spin_publish<<<1, 32, 0, vs>>>(hc_dev, 1000);   // warmup (lazy module loading)
    RT_CHECK(cudaStreamSynchronize(vs));
    memset(hp, 0, 64);

    std::atomic<bool> done{false};
    std::thread wd([&]() {
        for (int i = 0; i < 120 && !done.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (done.load()) return;
        printf("[WATCHDOG] timeout - resuming\n");
        rm_control(fd, client, session, CMD_RESUME, nullptr, 0);
        _exit(77);
    });
    wd.detach();

    auto sample = [&](const char *tag) {
        unsigned long long v = *hc;
        printf("[t=%7.3f] %-20s counter=%-14llu %s\n", now_s(), tag, v & ~(1ULL << 63),
               (v >> 63) ? "(COMPLETED)" : "");
        return v & ~(1ULL << 63);
    };

    printf("\n--- launch victim (%ld blocks, ~%ld ms) ---\n", blocks, spin_ms);
    spin_publish<<<(int)blocks, 32, 0, vs>>>(hc_dev, cycles);
    RT_CHECK(cudaGetLastError());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    unsigned long long a0 = sample("running");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    unsigned long long a1 = sample("running 2");
    double rate = (a1 - a0) / 1e6 / 0.5;
    printf("[baseline] %.2f M iter/s\n", rate);

    if (sweep) {
        // The session only binds to a KernelGraphicsObject; libcuda creates one
        // per channel, so try every candidate and report which one freezes.
        std::vector<std::pair<const char *, uint32_t>> cands;
        for (uint32_t h : L.computes) cands.push_back({"compute", h});
        for (uint32_t h : L.channels) cands.push_back({"channel", h});
        for (uint32_t h : L.tsgs)     cands.push_back({"tsg", h});
        for (auto &cd : cands) {
            NV83DE sp; memset(&sp, 0, sizeof sp);
            sp.hAppClient = L.app_client; sp.hClass3dObject = cd.second;
            int a = rm_alloc(fd, L.app_client, L.device, H_SESS2, CLASS_SMDBGSSN, &sp, 0);
            if (a) { printf("[sweep] %-8s %#x alloc -> %#x\n", cd.first, cd.second, a); continue; }
            uint32_t one = 1;
            rm_control(fd, L.app_client, H_SESS2, CMD_MODE_ERRBAR, &one, 4);
            rm_control(fd, L.app_client, H_SESS2, CMD_MODE_MMU, &one, 4);
            uint64_t sw[2] = {1, 0};
            int ss = rm_control(fd, L.app_client, H_SESS2, CMD_SUSPEND, sw, 8);
            unsigned long long v0 = *hc & ~(1ULL << 63);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            unsigned long long v1 = *hc & ~(1ULL << 63);
            rm_control(fd, L.app_client, H_SESS2, CMD_RESUME, nullptr, 0);
            printf("[sweep] %-8s %#x suspend=%#x counter %llu -> %llu  %s\n",
                   cd.first, cd.second, ss, v0, v1,
                   v1 == v0 ? "FROZEN <<<" : "still running");
            rm_free(fd, L.app_client, L.device, H_SESS2);
        }
        done = true;
        return 0;
    }

    uint64_t susp[2] = { flag, 0 };
    double t0 = now_s();
    int sst = rm_control(fd, client, session, CMD_SUSPEND, susp, 8);
    printf("\n[SUSPEND] 0x83de0317 {flag=%u} -> status=%#x in %.4f s\n", flag, sst, now_s() - t0);
    unsigned long long b0 = sample("suspend+0.0s");
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms / 2));
    unsigned long long b1 = sample("suspend+hold/2");
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms / 2));
    unsigned long long b2 = sample("suspend+hold");

    t0 = now_s();
    int rst = rm_control(fd, client, session, CMD_RESUME, nullptr, 0);
    printf("\n[RESUME] 0x83de0318 -> status=%#x in %.4f s\n", rst, now_s() - t0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    unsigned long long c0 = sample("resume+0.3s");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    unsigned long long c1 = sample("resume+1.0s");

    printf("\n--- wait for victim ---\n");
    double tw = now_s();
    cudaError_t se = cudaStreamSynchronize(vs);
    printf("[sync] %s after %.3f s\n", cudaGetErrorName(se), now_s() - tw);
    unsigned long long fin = sample("final");
    done = true;

    double during = (b2 - b0) / 1e6 / (hold_ms / 1000.0);
    double after = (c1 - c0) / 1e6;
    printf("\n================ VERDICT ================\n");
    printf("  baseline rate       : %.2f M iter/s\n", rate);
    printf("  rate during suspend : %.3f M iter/s -> %s\n", during,
           during < rate * 0.1 ? "FROZEN  <<< mid-kernel suspend" : "still running");
    printf("  rate after resume   : %.2f M iter/s -> %s\n", after,
           after > rate * 0.5 ? "RESUMED" : "did not resume");
    printf("  suspend=%#x resume=%#x sync=%s\n", sst, rst, cudaGetErrorName(se));
    printf("=========================================\n");

    if (modes) { uint32_t zero = 0; rm_control(fd, client, session, CMD_MODE_ERRBAR, &zero, 4); }
    if (session == H_SESS)
        printf("[cleanup] free session -> %#x\n", rm_free(fd, client, parent, session));
    return 0;
}
