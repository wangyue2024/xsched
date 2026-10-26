// l3dbg.cu - reproduce the CUDA debug agent's device suspend *inside* the
// application process, with no debugger and no ptrace.
//
// Recipe recovered from evidence/live/r1_ioctl.log + r2_ioctl.log:
//   1. libcuda auto-creates a KernelSMDebuggerSession (class 0x83de) in the app
//      client at cuCtxCreate (obj 0x5c000073).  v1 drove that one and saw no
//      freeze - the debug agent does NOT use it.
//   2. the debug agent creates its own RM client on its own /dev/nvidiactl fd,
//      with its own device object, and allocates a SECOND 0x83de session with
//      pAllocParms != NULL but **psize = 0** (v1 passed an explicit param blob
//      and got rc=87 on GSP forwarding).
//   3. it sets 0x83de031f / 0307 / 032a = 1, then
//          0x83de0317 params = {1, 0}   (8 bytes)  -> the device FREEZES
//          0x83de0318 params = {}       (0 bytes)  -> the device resumes
//      The attach-phase polling loop uses 0317 params = {0, 0}, which does not
//      stop a running kernel.  The flag is the whole difference.
//
// RM authorizes ioctls by fd ownership, so this probe interposes ioctl() in the
// executable (which wins over libc for every DSO, including libcuda) to learn
// which fd/client/device/subdevice/TSG handles libcuda actually uses, and then
// allocates its own session on that fd.
//
// Build: ../build.sh l3dbg     Run:
//   CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1 \
//   ./l3dbg [--flag 1] [--hold-ms 800] [--no-modes] [--blocks 8] [--spin-ms 12000]

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
#include <cuda.h>

extern "C" {
#include <cuxtra/cuxtra.h>
}

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)

// --------------------------------------------------------------- RM ABI
struct NVOS21 { uint32_t hRoot, hParent, hObject, hClass;
                uint64_t pAllocParms, pRightsRequested;
                uint32_t paramsSize, flags; int32_t status; };          // 48 B
struct NVOS54 { uint32_t hClient, hObject, cmd, flags;
                uint64_t params; uint32_t paramsSize; int32_t status; }; // 32 B
struct NVOS00 { uint32_t hRoot, hParent, hObject; int32_t status; };     // 16 B
static_assert(sizeof(NVOS21) == 48 && sizeof(NVOS54) == 32 && sizeof(NVOS00) == 16,
              "RM struct layout must match the captured ioctl sizes");

static const unsigned long NV_ESC_RM_ALLOC   = 0xc030462bUL;
static const unsigned long NV_ESC_RM_CONTROL = 0xc020462aUL;
static const unsigned long NV_ESC_RM_FREE    = 0xc0104629UL;

static const uint32_t CLASS_ROOT = 0x41, CLASS_DEVICE = 0x80, CLASS_SUBDEV = 0x2080,
                      CLASS_TSG = 0xa06c, CLASS_SMDBGSSN = 0x83de, CLASS_CHANNEL = 0xc96f;
static const uint32_t CMD_MODE_ERRBAR = 0x83de031f, CMD_MODE_MMU = 0x83de0307,
                      CMD_MODE_MMUGCC = 0x83de032a, CMD_SUSPEND = 0x83de0317,
                      CMD_RESUME = 0x83de0318, CMD_SET_EXC_MASK = 0x83de0309;

// ------------------------------------------------- learned libcuda handles
struct Learned {
    int fd = -1;
    std::vector<uint32_t> clients, devices;
    uint32_t device = 0, subdev = 0, tsg = 0, app_session = 0, channel = 0;
} L;

static int (*real_ioctl)(int, unsigned long, ...) = nullptr;
static __thread int in_hook = 0;
static bool verbose_hook = false;

static void learn(int fd, unsigned long req, void *arg, int r)
{
    if (r || !arg || req != NV_ESC_RM_ALLOC) return;
    NVOS21 a;
    memcpy(&a, arg, sizeof a);
    if (verbose_hook)
        printf("[learn] ALLOC fd=%d root=%#x parent=%#x obj=%#x class=%#x psize=%u status=%#x\n",
               fd, a.hRoot, a.hParent, a.hObject, a.hClass, a.paramsSize, a.status);
    if (a.status) return;
    switch (a.hClass) {
    case CLASS_ROOT:    if (L.fd < 0) L.fd = fd; L.clients.push_back(a.hObject); break;
    case CLASS_DEVICE:  L.device = a.hObject; L.devices.push_back(a.hObject);
                        if (L.fd < 0) L.fd = fd; break;
    case CLASS_SUBDEV:  L.subdev = a.hObject; break;
    case CLASS_TSG:     L.tsg = a.hObject; break;
    case CLASS_CHANNEL: L.channel = a.hObject; break;
    case CLASS_SMDBGSSN:L.app_session = a.hObject; break;
    }
}

// Interposed in the executable so every DSO (libcuda included) resolves to it.
extern "C" int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    int r = real_ioctl(fd, req, arg);
    if (in_hook || req != NV_ESC_RM_ALLOC) return r;
    in_hook = 1;
    learn(fd, req, arg, r);
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
    long blocks = 8, spin_ms = 12000, hold_ms = 800;
    uint32_t flag = 1;
    bool modes = true, use_app_session = false, exc_mask = false, scan_parents = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--blocks") blocks = atol(next().c_str());
        else if (a == "--spin-ms") spin_ms = atol(next().c_str());
        else if (a == "--hold-ms") hold_ms = atol(next().c_str());
        else if (a == "--flag") flag = (uint32_t)strtoul(next().c_str(), nullptr, 0);
        else if (a == "--no-modes") modes = false;
        else if (a == "--app-session") use_app_session = true;
        else if (a == "--exc-mask") exc_mask = true;
        else if (a == "--verbose-hook") verbose_hook = true;
        else if (a == "--scan-parents") scan_parents = true;
    }

    RT_CHECK(cudaFree(0));   // triggers cuInit -> the hook learns libcuda's handles
    printf("[learn] fd=%d device=%#x subdev=%#x tsg=%#x channel=%#x app_session=%#x clients=",
           L.fd, L.device, L.subdev, L.tsg, L.channel, L.app_session);
    for (uint32_t c : L.clients) printf(" %#x", c);
    printf("\n");
    if (L.fd < 0 || L.clients.empty() || !L.device) {
        fprintf(stderr, "[learn] incomplete - cannot proceed\n");
        return 1;
    }

    if (scan_parents) {
        // Which (client, parent) pairs will RM accept for a KernelSMDebuggerSession?
        // The captured debug agent used parent=0x5c000002 inside *its own* client,
        // but that handle may be a different class there, so scan instead of guessing.
        uint8_t zp[64] = {0};
        const uint32_t H = 0x5c00f00c;
        for (uint32_t c : L.clients) {
            for (uint32_t par = 0x5c000001; par <= 0x5c0000ff; par++) {
                int st = rm_alloc(L.fd, c, par, H, CLASS_SMDBGSSN, zp, 0);
                if (st == 0) {
                    printf("[scan] SUCCESS client=%#x parent=%#x -> session %#x\n", c, par, H);
                    rm_free(L.fd, c, par, H);
                } else if (par <= 0x5c000010 || st != 0x36) {
                    printf("[scan] client=%#x parent=%#x -> status=%#x\n", c, par, st);
                }
            }
        }
        printf("[scan] done\n");
        return 0;
    }

    uint32_t client = L.clients.back();
    int fd = L.fd;
    uint32_t session = 0;
    const uint32_t H_SESSION = 0x5c00f00c;
    uint8_t params[64] = {0};

    if (use_app_session && L.app_session) {
        session = L.app_session;
        printf("[session] using libcuda's auto-created session %#x\n", session);
    } else {
        for (uint32_t c : L.clients) {
            int st = rm_alloc(fd, c, L.device, H_SESSION, CLASS_SMDBGSSN, params, 0);
            printf("[session] alloc 0x83de client=%#x device=%#x -> status=%#x\n", c, L.device, st);
            if (st == 0) { session = H_SESSION; client = c; break; }
        }
        if (!session && L.subdev) {
            int st = rm_alloc(fd, client, L.subdev, H_SESSION, CLASS_SMDBGSSN, params, 0);
            printf("[session] retry parent=subdev -> status=%#x\n", st);
            if (st == 0) session = H_SESSION;
        }
    }
    if (!session) { fprintf(stderr, "[session] FAILED\n"); return 1; }

    if (modes) {
        uint32_t one = 1;
        printf("[modes] 031f=%#x 0307=%#x 032a=%#x\n",
               rm_control(fd, client, session, CMD_MODE_ERRBAR, &one, 4),
               rm_control(fd, client, session, CMD_MODE_MMU, &one, 4),
               rm_control(fd, client, session, CMD_MODE_MMUGCC, &one, 4));
    }
    if (exc_mask) {
        uint32_t mask = 0xffffffff;
        printf("[modes] 0309 exception mask -> %#x\n",
               rm_control(fd, client, session, CMD_SET_EXC_MASK, &mask, 4));
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

    spin_publish<<<1, 32, 0, vs>>>(hc_dev, 1000);   // warmup: lazy module loading
    RT_CHECK(cudaStreamSynchronize(vs));
    memset(hp, 0, 64);

    std::atomic<bool> done{false};
    std::thread wd([&]() {
        for (int i = 0; i < 120 && !done.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (done.load()) return;
        printf("[WATCHDOG] timeout - resuming (counter=%llu)\n", *hc & ~(1ULL << 63));
        rm_control(fd, client, session, CMD_RESUME, nullptr, 0);
        _exit(77);
    });
    wd.detach();

    auto sample = [&](const char *tag) {
        unsigned long long v = *hc;
        printf("[t=%7.3f] %-22s counter=%-14llu %s\n", now_s(), tag, v & ~(1ULL << 63),
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

    uint64_t sp[2] = { flag, 0 };
    double t0 = now_s();
    int st = rm_control(fd, client, session, CMD_SUSPEND, sp, 8);
    printf("\n[SUSPEND] 0x83de0317 {flag=%u} -> status=%#x in %.4f s\n", flag, st, now_s() - t0);
    unsigned long long b0 = sample("suspend+0.0s");
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms / 2));
    unsigned long long b1 = sample("suspend+hold/2");
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms / 2));
    unsigned long long b2 = sample("suspend+hold");

    t0 = now_s();
    int rt = rm_control(fd, client, session, CMD_RESUME, nullptr, 0);
    printf("\n[RESUME] 0x83de0318 -> status=%#x in %.4f s\n", rt, now_s() - t0);
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
    printf("  suspend=%#x resume=%#x sync=%s\n", st, rt, cudaGetErrorName(se));
    printf("=========================================\n");

    uint32_t zero = 0;
    if (modes) rm_control(fd, client, session, CMD_MODE_ERRBAR, &zero, 4);
    if (!use_app_session)
        printf("[cleanup] free session -> %#x\n", rm_free(fd, client, L.device, session));
    return 0;
}
