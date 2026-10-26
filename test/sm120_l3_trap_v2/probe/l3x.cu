// l3x.cu - cross-process device suspend, the topology cuda-gdb actually uses.
//
// In-process replication is blocked by RM (evidence/live/exp_rmdev.log,
// exp_rmstack_ownclient.log):
//   * a client we allocate ourselves cannot create NV01_DEVICE_0  -> 0x33
//   * RM_DUP_OBJECT of the app's device into that client          -> 0x33
//   * a 0x83de session inside libcuda's own client allocates fine and
//     0x83de0317 {1,0} returns status 0, but the device keeps running.
// cuda-gdb's session lives in a *different process* whose client went through a
// full libcuda initialisation, and it names the target's client + compute object
// in NV83DE_ALLOC_PARAMETERS_DEBUGGER.  This probe reproduces that split:
//
//   ./l3x --role target --shm /sm120_l3x [--spin-ms 30000]
//   ./l3x --role agent  --shm /sm120_l3x [--hold-ms 1000] [--flag 1]
//
// The target publishes {app_client, device, subdev, compute, session, pid} into
// the shm segment together with a monotonically increasing counter written by
// the spinning kernel; the agent reads them, builds its own stack, suspends,
// holds, resumes, and reports the counter rate before/during/after.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <signal.h>
#include <cuda_runtime.h>
#include "arena.h"

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)

// --------------------------------------------------------------- RM ABI
struct NVOS21 { uint32_t hRoot, hParent, hObject, hClass;
                uint64_t pAllocParms, pRightsRequested;
                uint32_t paramsSize, flags; int32_t status; };
struct NVOS54 { uint32_t hClient, hObject, cmd, flags;
                uint64_t params; uint32_t paramsSize; int32_t status; };
struct NVOS00 { uint32_t hRoot, hParent, hObject; int32_t status; };
struct NV0080 { uint32_t hClientShare, hTargetClient, hTargetDevice, deviceId,
                           hVASpace, hInternalDevice; uint64_t vaSpaceSize;
                uint32_t a, b; uint64_t c; uint32_t d; };
static_assert(sizeof(NVOS21) == 48 && sizeof(NVOS54) == 32 && sizeof(NVOS00) == 16, "RM ABI");

static const unsigned long NV_ESC_RM_ALLOC   = 0xc030462bUL;
static const unsigned long NV_ESC_RM_CONTROL = 0xc020462aUL;
static const unsigned long NV_ESC_RM_FREE    = 0xc0104629UL;

static const uint32_t CLASS_ROOT = 0x41, CLASS_DEVICE = 0x80, CLASS_SUBDEV = 0x2080,
                      CLASS_COMPUTE = 0xcec0, CLASS_SMDBGSSN = 0x83de;
static const uint32_t CMD_MODE_ERRBAR = 0x83de031f, CMD_MODE_MMU = 0x83de0307,
                      CMD_MODE_MMUGCC = 0x83de032a, CMD_SUSPEND = 0x83de0317,
                      CMD_RESUME = 0x83de0318;

struct Learned {
    int fd = -1;
    uint32_t device = 0, subdev = 0, compute = 0, app_client = 0, app_session = 0;
    uint32_t ncompute = 0, computes[16] = {};
} L;

static int (*real_ioctl)(int, unsigned long, ...) = nullptr;
static __thread int in_hook = 0;

static int rm_control(int fd, uint32_t client, uint32_t obj, uint32_t cmd, void *p, uint32_t n);

// L3X_POLICY: replicate cuda-gdb's early handshake (evidence/live/r3_ioctl.log.20138).
// As soon as our own client exists, share that client object with the debugger
// client via NV01_ROOT class control 0xd04 (ClientSetInheritedSharePolicy):
//   {own_client, 1, 0x0002}, {own_client, 4, 0x40002}, {agent_client, 1, 0x40003}
// Objects created afterwards inherit the policy, which is what lets the agent
// name our KernelGraphicsObject in a 0x83de session across processes.
struct Shm;
static Shm *g_sh = nullptr;
static uint32_t g_agent_client = 0, g_policy_attempted = 0;
static int32_t g_policy_status[3] = {0x7fffffff, 0x7fffffff, 0x7fffffff};

static void apply_policy(int fd, uint32_t client)
{
    if (g_policy_attempted || !g_agent_client) return;
    g_policy_attempted = 1;
    struct { uint32_t target, rights, type; } p;
    p = { client, 1, 0x0002 };
    g_policy_status[0] = rm_control(fd, client, client, 0xd04, &p, sizeof p);
    p = { client, 4, 0x40002 };
    g_policy_status[1] = rm_control(fd, client, client, 0xd04, &p, sizeof p);
    p = { g_agent_client, 1, 0x40003 };
    g_policy_status[2] = rm_control(fd, client, client, 0xd04, &p, sizeof p);
    fprintf(stderr, "[hook] policy client=%#x agent=%#x -> %#x %#x %#x\n",
            client, g_agent_client, (unsigned)g_policy_status[0],
            (unsigned)g_policy_status[1], (unsigned)g_policy_status[2]);
}

static void apply_policy_now(int fd, uint32_t client)
{
    if (!g_agent_client) { printf("[hook] late policy: no agent client\n"); return; }
    struct { uint32_t target, rights, type; } p;
    p = { client, 1, 0x0002 };
    g_policy_status[0] = rm_control(fd, client, client, 0xd04, &p, sizeof p);
    p = { client, 4, 0x40002 };
    g_policy_status[1] = rm_control(fd, client, client, 0xd04, &p, sizeof p);
    p = { g_agent_client, 1, 0x40003 };
    g_policy_status[2] = rm_control(fd, client, client, 0xd04, &p, sizeof p);
    printf("[hook] late policy client=%#x agent=%#x -> %#x %#x %#x\n", client,
           g_agent_client, (unsigned)g_policy_status[0], (unsigned)g_policy_status[1],
           (unsigned)g_policy_status[2]);
}

static void learn(int fd, void *arg)
{
    NVOS21 a; memcpy(&a, arg, sizeof a);
    if (a.status) return;
    if (getenv("L3X_LOG_ALLOC"))
        fprintf(stderr, "[alloc] class=%#x h=%#x root=%#x parent=%#x\n",
                a.hClass, a.hObject, a.hRoot, a.hParent);
    switch (a.hClass) {
    case CLASS_ROOT:
        if (L.fd < 0) L.fd = fd;
        if (!L.app_client) L.app_client = a.hObject;
        break;
    case CLASS_DEVICE:   if (!L.device) { L.device = a.hObject; L.app_client = a.hRoot;
                                         if (!getenv("L3X_LATE_POLICY")) apply_policy(fd, a.hRoot); }
                         if (L.fd < 0) L.fd = fd; break;
    case CLASS_SUBDEV:   if (!L.subdev) L.subdev = a.hObject; break;
    case CLASS_COMPUTE:
        if (!L.compute) L.compute = a.hObject;
        if (L.ncompute < 16) L.computes[L.ncompute++] = a.hObject;
        break;
    case CLASS_SMDBGSSN: if (!L.app_session) L.app_session = a.hObject; break;
    }
}

extern "C" int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    int r = real_ioctl(fd, req, arg);
    if (getenv("L3X_LOG_ALLOC") && req == 0xc0384627UL && arg && r == 0) {
        uint32_t *w = (uint32_t *)arg;
        fprintf(stderr, "[mem] root=%#x parent=%#x obj=%#x class=%#x size=%#x flags=%#x\n",
                w[0], w[1], w[2], w[3], w[12], w[13]);
    }
    if (in_hook || r || !arg || req != NV_ESC_RM_ALLOC) return r;
    in_hook = 1; learn(fd, arg); in_hook = 0;
    return r;
}

static int rm_alloc(int fd, uint32_t root, uint32_t parent, uint32_t obj, uint32_t cls,
                    void *params, uint32_t psize)
{
    NVOS21 a; memset(&a, 0, sizeof a);
    a.hRoot = root; a.hParent = parent; a.hObject = obj; a.hClass = cls;
    a.pAllocParms = (uint64_t)(uintptr_t)params; a.paramsSize = psize;
    int r = real_ioctl(fd, NV_ESC_RM_ALLOC, &a);
    return r ? (0x1000000 | r) : a.status;
}
static int rm_control(int fd, uint32_t client, uint32_t obj, uint32_t cmd, void *p, uint32_t n)
{
    NVOS54 c; memset(&c, 0, sizeof c);
    c.hClient = client; c.hObject = obj; c.cmd = cmd;
    c.params = (uint64_t)(uintptr_t)p; c.paramsSize = n;
    int r = real_ioctl(fd, NV_ESC_RM_CONTROL, &c);
    return r ? (0x1000000 | r) : c.status;
}
static int rm_free(int fd, uint32_t root, uint32_t parent, uint32_t obj)
{
    NVOS00 f; memset(&f, 0, sizeof f);
    f.hRoot = root; f.hParent = parent; f.hObject = obj;
    int r = real_ioctl(fd, NV_ESC_RM_FREE, &f);
    return r ? (0x1000000 | r) : f.status;
}

// --------------------------------------------------------------- shm layout
struct Shm {
    volatile uint64_t magic;      // 0x4c335831 once the target published
    uint32_t pid, app_client, device, subdev, compute, app_session;
    volatile unsigned long long counter;    // written by the device
    volatile uint32_t done;       // target finished / agent should stop
    // all KernelGraphicsObjects the target created (the running kernel is on
    // one of them; which one is not knowable without the channel map).
    uint32_t ncompute;
    uint32_t computes[16];
    // target-side resume: the app's own 0x83de session releases the hold
    volatile uint32_t resume_req, resume_ack;
    volatile int32_t self_resume_status;
    // live trap-handler patch (v1.5): signature written by the handler itself
    volatile uint32_t sig;           // 0x5aa5 once .text.traphandler really ran
    volatile uint32_t sig_reason;    // trap reason mask computed by do_read_trap_reason
    volatile uint32_t sig2;          // state while parked in the trap handler
    volatile uint32_t trap_resume;   // arm request (host -> helper) / host release
    volatile uint32_t flag_dev_ack;  // helper acked the device flag value
    uint64_t dev_flag;               // device VA of the coherent control word
    uint64_t preempt_buf;
    uint32_t patched;
    // RM_SHARE handshake (see RM_REVERSE_ZH.md §5.2)
    volatile uint32_t agent_client, agent_ready, share_done;
    volatile int32_t  share_status[16];   // 0x7fffffff = not attempted
    volatile int32_t  share_ret[16];
};

static Shm *open_shm(const char *name, bool create)
{
    int fd = shm_open(name, (create ? O_CREAT | O_TRUNC : 0) | O_RDWR, 0666);
    if (fd < 0) { if (create) perror("shm_open"); return nullptr; }
    if (create && ftruncate(fd, sizeof(Shm))) { perror("ftruncate"); return nullptr; }
    void *p = mmap(nullptr, sizeof(Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? nullptr : (Shm *)p;
}

// NV_ESC_RM_SHARE = 0x35, 24-byte NVOS39.  Field offsets inferred from
// Nv04ShareKernel -> _nv04Share@0x339a70 (which forwards (p[0], p[4], &p[8],
// &policy) with status written at 0x14) and from serverShareResourceAccess@0x3bd550
// (reads *(u16*)((p[8])+8) as the share type and rejects type > 7).
struct NVOS39 { uint32_t hClient, hParent, hObject, hShareClient;
                uint16_t shareType; uint16_t pad;
                int32_t status; };
static_assert(sizeof(NVOS39) == 24, "NV_ESC_RM_SHARE param size");

struct NV83DE { uint32_t hDebuggerClient, hAppClient, hClass3dObject; uint32_t pad[5]; };

static int rm_share(int fd, uint32_t client, uint32_t object, uint32_t share_to,
                    uint16_t type)
{
    NVOS39 p; memset(&p, 0, sizeof p);
    p.hClient = client; p.hParent = client; p.hObject = object;
    p.hShareClient = share_to; p.shareType = type;
    int r = real_ioctl(fd, 0xc0184635UL, &p);
    return r ? (0x1000000 | r) : p.status;
}

extern "C" __global__ void guard_param_probe() {}

__global__ void write_code(unsigned long long dst, const unsigned long long *src)
{
    int i = threadIdx.x;
    ((volatile unsigned long long *)dst)[i] = src[i];
    __threadfence_system();
}

__global__ void poke64(unsigned long long va, unsigned long long a, unsigned long long b)
{
    volatile unsigned long long *p = (volatile unsigned long long *)(uintptr_t)va;
    p[0] = a; p[1] = b;
}

// Guardian-equivalent victim: every 256 iterations it does a cache-coherent
// load of the exit flag and breaks out when the trap handler sets it.
__global__ void spin_guard(volatile unsigned *flag, volatile unsigned long long *cnt,
                           unsigned long long cycles)
{
    unsigned long long start = clock64(), i = 0;
    while (clock64() - start < cycles) {
        i++;
        if ((i & 0xff) == 0) {
            unsigned f;
            asm volatile("ld.global.cv.u32 %0, [%1];" : "=r"(f) : "l"(flag));
            if (f) break;
            *cnt = i;
        }
    }
    *cnt = i | (1ULL << 63);
}

__global__ void spin_publish(volatile unsigned long long *c, unsigned long long cycles)
{
    unsigned long long start = clock64(), i = 0;
    while (clock64() - start < cycles) { i++; if ((i & 0xff) == 0) *c = i; }
    *c = i | (1ULL << 63);
}

static double now_s()
{
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ------------------------------------------------- live handler patch (v1.5)
// Patch .text.traphandler +0x390 ("MOV R3,RZ", right after the relocated
// CALL.ABS to do_read_trap_reason) with CALL.ABS into a code cave in the
// zero-filled 0xA00..0xA80 region of the slot-20 object.  The cave repeats the
// displaced instruction, writes 0x5aa5 to a host-mapped word (UVA; the spin
// kernel already publishes to the same kind of mapping), then returns with the
// exact RET.ABS R12 0x20 idiom used by do_read_trap_reason, landing at 0x3a0.
static uint64_t g_h_backup[2], g_cave_backup[64];
static size_t g_cave_size = 0;
static uint64_t g_cave_va = 0, g_handler_va = 0, g_patch_ctx = 0;
static CUdeviceptr g_cave_alloc = 0;
static bool g_cave_instr = false;
static int g_patched = 0;
static uint64_t *g_devflag = nullptr;
static uint64_t g_ret_stub[16], g_guard_stub[16];
static uint64_t g_stub_cave = 0, g_stub_ctx = 0;
static volatile int g_stub_stop = 0;
static Shm *g_sh_ptr = nullptr;
static uint64_t *g_ret_dev = nullptr, *g_armed_dev = nullptr;
static unsigned long long g_cave_dev = 0;
static cudaStream_t g_arm_stream = nullptr;
static volatile int g_devflag_stop = 0;
static Shm *g_devflag_shm = nullptr;

static void mov_imm(int reg, uint32_t imm, uint64_t out[2])
{
    out[0] = ((uint64_t)imm << 32) | ((uint64_t)reg << 16) | 0x00007802ULL;
    out[1] = 0x003fe20000000f00ULL;
}

static void patch_live_handler(struct Shm *sh)
{
    CUcontext ctx = nullptr;
    if (cuCtxGetCurrent(&ctx) || !ctx) { printf("[tgt] no current context\n"); return; }
    sm120arena::Info I = sm120arena::probe(ctx, 0);
    sm120arena::report(I, "[tgt]");
    if (!I.ok || !I.handler_is_memobj || I.handler.size() < 0x3b0 || I.tools_size < 0xb00) {
        printf("[tgt] patch skipped (arena/handler unavailable)\n");
        return;
    }
    for (int i = 0; i < 10; i++) {
        if (sm120arena::opcode_at(&I.handler[i * 16]) != sm120arena::kTrapHandlerFp[i]) {
            printf("[tgt] patch skipped (fingerprint mismatch at +%#x)\n", i * 16);
            return;
        }
    }

    if (getenv("L3X_PATCH_ALLOC_ONLY")) {
        CUdeviceptr b = cuXtraInstrMemBlockAlloc(ctx, 0x1000);
        printf("[tgt] ALLOC_ONLY instr block = %#llx\n", (unsigned long long)b);
        return;
    }
    uint64_t sig_va = (uint64_t)(uintptr_t)&sh->sig;
    bool park = getenv("L3X_PATCH_PARK") != nullptr;
    bool kill = getenv("L3X_PATCH_EXIT") != nullptr;
    uint64_t cave;
    std::vector<uint64_t> code;
    auto put = [&](size_t off, uint64_t a, uint64_t b) {
        code[off / 8] = a; code[off / 8 + 1] = b;
    };
    if (park && sh->dev_flag) {
        // Same loop, but the control word lives in device memory (written by
        // the helper's second context), so release is GPU-coherent.
        cave = I.tools_pc + 0xa00;
        code.assign(16, 0);
        uint64_t dva = sh->dev_flag;
        uint64_t t[2];
        mov_imm(8, (uint32_t)dva, t);            put(0x00, t[0], t[1]);
        mov_imm(9, (uint32_t)(dva >> 32), t);    put(0x10, t[0], t[1]);
        put(0x20, 0x00000000080a7980ULL, 0x000ea20000100900ULL);              // LD.E R10,[R8]
        put(0x30, 0x000000ff0a00720cULL, 0x004fda0003f05270ULL);              // ISETP.NE.AND P0,R10,RZ
        put(0x40, 0xfffffffc00f40947ULL, 0x003ff2000383ffffULL);              // @P0 BRA -0x20
        put(0x50, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);              // IADD R12,R12,0x10
        put(0x60, 0x000000000c087950ULL, 0x003fc00003a00000ULL);              // RET.ABS R12 0x20
        put(0x70, 0x0000000000007918ULL, 0x000fc00000000000ULL);              // NOP
    } else if (park) {
        // Minimal 8-instruction park in the *driver arena* (the only place
        // that is mapped executable in the trap context).  Loop on the host
        // word sh->trap_resume; the scheduler writes 1 to release.
        cave = I.tools_pc + 0xa00;
        code.assign(16, 0);
        uint64_t rva = (uint64_t)(uintptr_t)&sh->trap_resume;
        uint64_t t[2];
        mov_imm(8, (uint32_t)rva, t);            put(0x00, t[0], t[1]);
        mov_imm(9, (uint32_t)(rva >> 32), t);    put(0x10, t[0], t[1]);
        if (getenv("L3X_PATCH_BOUNDED")) {
            // Bounded self-resuming park: spin N iterations inside the trap,
            // then return.  Proves trap-level suspend + resume + context
            // survival without depending on host-memory coherence.
            uint64_t t2[2]; mov_imm(8, 1000000000u, t2); put(0x00, t2[0], t2[1]);
            put(0x10, 0xffffffff08087835ULL, 0x003fe600078e0000ULL);          // IADD R8,R8,-1
            put(0x20, 0x000000ff0800720cULL, 0x004fda0003f05270ULL);          // ISETP.NE P0,R8,RZ
            put(0x30, 0xfffffffc00f40947ULL, 0x003ff2000383ffffULL);          // @P0 BRA -0x20
            put(0x40, 0x000000ff00037202ULL, 0x003ff00000000f00ULL);          // MOV R3,RZ
            put(0x50, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);          // IADD R12
            put(0x60, 0x000000000c087950ULL, 0x003fc00003a00000ULL);          // RET
            put(0x70, 0x0000000000007918ULL, 0x000fc00000000000ULL);          // NOP
        } else {
            put(0x20, 0x00000000080a7980ULL, 0x000ea20000100900ULL);          // LD.E R10,[R8]
            put(0x30, 0x000000ff0a00720cULL, 0x004fda0003f05270ULL);          // ISETP.NE.AND P0,R10,RZ
            put(0x40, 0xfffffffc00f40947ULL, 0x003ff2000383ffffULL);          // @P0 BRA -0x20
            put(0x50, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);          // IADD R12,R12,0x10
            put(0x60, 0x000000000c087950ULL, 0x003fc00003a00000ULL);          // RET.ABS R12 0x20
            put(0x70, 0x0000000000007918ULL, 0x000fc00000000000ULL);          // NOP
        }
    } else {
        cave = I.tools_pc + 0xa00;
        code.assign(16, 0);
        put(0x00, 0x000000ff00037202ULL, 0x003ff00000000f00ULL);               // MOV R3,RZ
        uint64_t t[2]; mov_imm(8, (uint32_t)sig_va, t); put(0x10, t[0], t[1]);
        mov_imm(9, (uint32_t)(sig_va >> 32), t);        put(0x20, t[0], t[1]);
        put(0x30, 0x00005aa5ff0a7431ULL, 0x000fca00000001ffULL);               // HFMA2 R10 = 0x5aa5
        put(0x40, 0x0000000008007385ULL, 0x000fe2000010090aULL);               // ST.E [R8],R10
        put(0x50, 0x0000000408007385ULL, 0x000fe20000100902ULL);               // ST.E [R8+4],R2
        if (getenv("L3X_ARM_PATCH")) {
            // Default: harmless return stub.  A request flips the cave to the
            // GUARD writer (LDC preempt ptr + set global_exit_flag), which the
            // target can do at runtime with cuXtraMemcpyHtoD.
            cave = I.tools_pc + 0xa00;
            code.assign(16, 0);
            for (size_t off = 0; off < 0x80; off += 16) {
                g_ret_stub[off/8] = 0x0000000000007918ULL;
                g_ret_stub[off/8+1] = 0x000fc00000000000ULL;
                g_guard_stub[off/8] = 0x0000000000007918ULL;
                g_guard_stub[off/8+1] = 0x000fc00000000000ULL;
            }
            auto put2 = [](uint64_t *a, size_t off, uint64_t x, uint64_t y) {
                a[off/8] = x; a[off/8+1] = y;
            };
            put2(g_ret_stub, 0x00, 0x000000ff00037202ULL, 0x003ff00000000f00ULL);
            put2(g_ret_stub, 0x10, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);
            put2(g_ret_stub, 0x20, 0x000000000c087950ULL, 0x003fc00003a00000ULL);
            // Armed: redirect the saved TRAP_RETURN_PC to an EXIT trampoline
            // at cave+0x70, so the warp exits *after* the handler restored its
            // trap state (an EXIT inside 0x390 would be unclean).
            uint64_t tramp = cave + 0x70;
            uint64_t m0[2], m1[2];
            mov_imm(0, (uint32_t)tramp, m0);
            mov_imm(1, (uint32_t)(tramp >> 32), m1);
            put2(g_guard_stub, 0x00, m0[0], m0[1]);
            put2(g_guard_stub, 0x10, m1[0], m1[1]);
            put2(g_guard_stub, 0x20, 0xffffa800ff007387ULL, 0x0033e40000100a00ULL); // STL.64 [0xffffa8],R0
            put2(g_guard_stub, 0x30, 0x000000ff00037202ULL, 0x003ff00000000f00ULL);  // MOV R3,RZ
            put2(g_guard_stub, 0x40, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);  // IADD R12
            put2(g_guard_stub, 0x50, 0x000000000c087950ULL, 0x003fc00003a00000ULL);  // RET
            put2(g_guard_stub, 0x60, 0x0000000000007918ULL, 0x000fc00000000000ULL);  // NOP
            put2(g_guard_stub, 0x70, 0x000000000000794dULL, 0x000fea0003800000ULL);  // EXIT trampoline
            for (int i2 = 0; i2 < 16; i2++) code[i2] = g_ret_stub[i2];
            g_stub_cave = cave; g_stub_ctx = (uint64_t)ctx;
        } else if (getenv("L3X_PATCH_LDDIAG")) {
            put(0x00, 0x00005c00ff047b82ULL, 0x000fc00000000800ULL);           // LDC R4,c[0x170]
            put(0x10, 0x00005d00ff057b82ULL, 0x000fc00000000800ULL);           // LDC R5,c[0x174]
            put(0x20, 0x00000000040a7980ULL, 0x000ea20000100900ULL);           // LD.E R10,[R4]
            put(0x30, 0x0000000404007385ULL, 0x000fe2000010090aULL);           // ST.E [R4+4],R10
            put(0x40, 0x00005aa5ff0a7431ULL, 0x000fca00000001ffULL);           // HFMA2 R10=0x5aa5 (positive control)
            put(0x50, 0x0000000804007385ULL, 0x000fe2000010090aULL);           // ST.E [R4+8],R10
            put(0x60, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);           // IADD R12
            put(0x70, 0x000000000c087950ULL, 0x003fc00003a00000ULL);           // RET
        } else if (getenv("L3X_PATCH_REASON")) {
            put(0x00, 0x00005c00ff047b82ULL, 0x000fc00000000800ULL);           // LDC R4, c[0x0][0x170]
            put(0x10, 0x00005d00ff057b82ULL, 0x000fc00000000800ULL);           // LDC R5, c[0x0][0x174]
            put(0x20, 0x0000000004007385ULL, 0x000fe20000100902ULL);           // ST.E [R4],R2
            put(0x30, 0x000000ff00037202ULL, 0x003ff00000000f00ULL);           // MOV R3,RZ
            put(0x40, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);           // IADD R12
            put(0x50, 0x000000000c087950ULL, 0x003fc00003a00000ULL);           // RET
            put(0x60, 0x0000000000007918ULL, 0x000fc00000000000ULL);
            put(0x70, 0x0000000000007918ULL, 0x000fc00000000000ULL);
        } else if (getenv("L3X_PATCH_PAYLOAD")) {
            // The real L3 payload: kill the warp only if the scheduler set the
            // global_exit_flag in the preempt buffer (LDC c[0x0][0x170]).
            put(0x00, 0x00005c00ff047b82ULL, 0x000fc00000000800ULL);           // LDC R4, c[0x0][0x170]
            put(0x10, 0x00005d00ff057b82ULL, 0x000fc00000000800ULL);           // LDC R5, c[0x0][0x174]
            put(0x20, 0x00000000040a7980ULL, 0x000ea20000100900ULL);           // LD.E R10,[R4]
            put(0x30, 0x000000ff0a00720cULL, 0x004fda0003f05270ULL);           // ISETP.NE.AND P0,R10,RZ
            put(0x40, 0x000000000000094dULL, 0x000fea0003800000ULL);           // @P0 EXIT
            put(0x50, 0x000000ff00037202ULL, 0x003ff00000000f00ULL);           // MOV R3,RZ
            put(0x60, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);           // IADD R12
            put(0x70, 0x000000000c087950ULL, 0x003fc00003a00000ULL);           // RET
        } else if (getenv("L3X_PATCH_GUARD")) {
            // sm70/86-style kill: read XSched's preempt buffer pointer from the
            // kernel's constant bank (per-warp, still valid in the trap handler)
            // and set global_exit_flag.  The instrumented guardian then EXITs
            // every block at its next checkpoint.
            put(0x00, 0x00005c00ff047b82ULL, 0x000fc00000000800ULL);           // LDC R4, c[0x0][0x170]
            put(0x10, 0x00005d00ff057b82ULL, 0x000fc00000000800ULL);           // LDC R5, c[0x0][0x174]
            put(0x20, 0x00005aa5ff0a7431ULL, 0x000fca00000001ffULL);           // HFMA2 R10 = 0x5aa5
            put(0x30, 0x0000000004007385ULL, 0x000fe2000010090aULL);           // ST.E [R4],R10
            put(0x40, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);           // IADD R12,R12,0x10
            put(0x50, 0x000000000c087950ULL, 0x003fc00003a00000ULL);           // RET.ABS R12 0x20
            put(0x60, 0x0000000000007918ULL, 0x000fc00000000000ULL);           // NOP
            put(0x70, 0x0000000000007918ULL, 0x000fc00000000000ULL);           // NOP
        } else if (getenv("L3X_PATCH_STRONG")) {
            put(0x20, 0x00000004080a7981ULL, 0x001eac000c1f5900ULL);           // LDG.E.STRONG.SYS R10,desc[UR4][R8.64]
            put(0x30, 0x0000000008007385ULL, 0x000fe2000010090aULL);           // ST.E [R8],R10
            put(0x40, 0x0000000408007385ULL, 0x000fe20000100902ULL);           // ST.E [R8+4],R2
            put(0x50, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);           // IADD R12
            put(0x60, 0x000000000c087950ULL, 0x003fc00003a00000ULL);           // RET
            put(0x70, 0x0000000000007918ULL, 0x000fc00000000000ULL);           // NOP
        } else if (getenv("L3X_PATCH_COUNT")) {
            put(0x20, 0x00000000080a7980ULL, 0x000ea20000100900ULL);           // LD.E R10,[R8] count
            put(0x30, 0x000000010a0a7835ULL, 0x003fe600078e0000ULL);           // IADD R10,R10,1
            put(0x40, 0x0000000008007385ULL, 0x000fe2000010090aULL);           // ST.E [R8],R10
            put(0x50, 0x0000000408007385ULL, 0x000fe20000100902ULL);           // ST.E [R8+4],R2 reason
            put(0x60, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);           // IADD R12
            put(0x70, 0x000000000c087950ULL, 0x003fc00003a00000ULL);           // RET
        } else if (kill) {
            put(0x60, 0x000000000000794dULL, 0x000fea0003800000ULL);           // EXIT
        } else {
            put(0x60, 0x000000100c0c7835ULL, 0x003fe600078e0000ULL);           // IADD R12,R12,0x10
            put(0x70, 0x000000000c087950ULL, 0x003fc00003a00000ULL);           // RET.ABS R12 0x20
        }
    }
    const size_t cave_bytes = code.size() * 8;

    size_t rb0 = cuXtraMemcpyDtoH(g_h_backup, I.handler_va + 0x390, 16, 0);
    size_t rb1 = g_cave_instr ? cuXtraInstrMemcpyDtoH(g_cave_backup, cave, cave_bytes, 0)
                              : cuXtraMemcpyDtoH(g_cave_backup, cave, cave_bytes, 0);
    uint64_t orig0 = 0, orig1 = 0;
    memcpy(&orig0, &I.handler[0x380], 8);
    memcpy(&orig1, &I.handler[0x388], 8);
    // CALL.ABS encodes ((target - 0x100) >> 8); the decoder prints
    // (imm << 8) | 0x100 (verified with `nvdisasm -b SM120a`).
    uint64_t call[2];
    uint64_t enc = (cave - 0x100) >> 8;
    call[0] = ((enc & 0xffffffffULL) << 32) | (orig0 & 0xffffffffULL);
    call[1] = (orig1 & ~0xffULL) | ((enc >> 32) & 0xffULL);

    size_t w0 = g_cave_instr ? cuXtraInstrMemcpyHtoD(cave, code.data(), cave_bytes, 0)
                             : cuXtraMemcpyHtoD(cave, code.data(), cave_bytes, 0);
    size_t w1 = 16;
    if (!getenv("L3X_PATCH_NOPATCH"))
        w1 = cuXtraMemcpyHtoD(I.handler_va + 0x390, call, 16, 0);
    else
        printf("[tgt] NOPATCH: handler left intact\n");
    cuXtraInvalInstrCache(ctx);
    uint64_t readback[2] = {0, 0};
    size_t rr = cuXtraMemcpyDtoH(readback, I.handler_va + 0x390, 16, 0);

    printf("[tgt] patch handler=%#llx cave=%#llx sig_va=%#llx\n",
           (unsigned long long)I.handler_va, (unsigned long long)cave,
           (unsigned long long)sig_va);
    printf("[tgt] write cave=%zu patch=%zu backup=%zu/%zu readback=%zu -> %#llx %#llx (want %#llx %#llx)\n",
           w0, w1, rb0, rb1, rr,
           (unsigned long long)readback[0], (unsigned long long)readback[1],
           (unsigned long long)call[0], (unsigned long long)call[1]);

    g_cave_va = cave; g_handler_va = I.handler_va; g_patch_ctx = (uint64_t)ctx;
    g_cave_size = cave_bytes; g_patched = (w0 == cave_bytes && w1 == 16);
    if (getenv("L3X_ARM_KERNEL") && getenv("L3X_ARM_PATCH")) {
        cudaMalloc((void **)&g_ret_dev, 128);
        cudaMalloc((void **)&g_armed_dev, 128);
        cudaMemcpy(g_ret_dev, g_ret_stub, 128, cudaMemcpyHostToDevice);
        cudaMemcpy(g_armed_dev, g_guard_stub, 128, cudaMemcpyHostToDevice);
        g_cave_dev = cave;
        cudaStreamCreateWithFlags(&g_arm_stream, cudaStreamNonBlocking);
        printf("[tgt] arm-kernel stubs dev=%p/%p stream=%p\n", (void *)g_ret_dev,
               (void *)g_armed_dev, (void *)g_arm_stream);
    }
    sh->preempt_buf = I.preempt_buf;
    sh->patched = (uint32_t)g_patched;
}

static void restore_live_handler()
{
    if (!g_patched) return;
    CUcontext ctx = (CUcontext)(uintptr_t)g_patch_ctx;
    cuXtraMemcpyHtoD(g_handler_va + 0x390, g_h_backup, 16, 0);
    if (g_cave_instr) cuXtraInstrMemcpyHtoD(g_cave_va, g_cave_backup, g_cave_size, 0);
    else              cuXtraMemcpyHtoD(g_cave_va, g_cave_backup, g_cave_size, 0);
    cuXtraInvalInstrCache(ctx);
    uint64_t rb[2] = {0, 0};
    cuXtraMemcpyDtoH(rb, g_handler_va + 0x390, 16, 0);
    printf("[tgt] handler restored (%s)\n",
           (rb[0] == g_h_backup[0] && rb[1] == g_h_backup[1]) ? "verified" : "MISMATCH");
}

// ------------------------------------------------------------------- target
static int run_target(const char *shmname, long spin_ms, long blocks)
{
    if (getenv("L3_ALLOW_PTRACE")) prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    Shm *sh = open_shm(shmname, true);
    if (!sh) return 1;
    memset((void *)sh, 0, sizeof *sh);
    g_sh = sh; g_sh_ptr = sh;

    // In policy mode the debugger client must exist before our own RM client is
    // created, so that the 0xd04 handshake can name it during client creation.
    if (getenv("L3X_POLICY")) {
        int pr_ = prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
        printf("[target] PR_SET_PTRACER -> %d (%s)\n", pr_, pr_ ? strerror(errno) : "ok");
        for (int i = 0; i < 600 && !sh->agent_ready; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!sh->agent_ready) { fprintf(stderr, "[target] agent never published its client\n"); return 1; }
        g_agent_client = sh->agent_client;
        printf("[target] policy mode: agent client=%#x\n", g_agent_client);
    }

    RT_CHECK(cudaFree(0));
    if (getenv("L3X_CUDBG")) {
        void *lc = dlopen("libcuda.so.1", RTLD_NOW | RTLD_NOLOAD);
        int (*init1)(int) = lc ? (int (*)(int))dlsym(lc, "cudbgApiInit") : nullptr;
        int (*attach)() = lc ? (int (*)())dlsym(lc, "cudbgApiAttach") : nullptr;
        printf("[tgt] cudbgApiInit=%p cudbgApiAttach=%p\n", (void *)init1, (void *)attach);
        if (init1) { int r = init1(1); printf("[tgt] cudbgApiInit(1) -> %d\n", r); }
    }
    if (getenv("L3X_LATE_POLICY")) apply_policy_now(L.fd, L.app_client);
    if (getenv("L3X_PATCH")) {
        cudaError_t hr = cudaHostRegister((void *)sh, 4096, cudaHostRegisterMapped);
        printf("[target] cudaHostRegister(shm) -> %s\n", cudaGetErrorName(hr));
    }
    if (getenv("L3X_PATCH_DEVFLAG")) {
        // Coherent control word: normal device memory, updated by an async
        // HtoD copy from a helper thread (copy engine, not the SM path).
        cudaError_t e = cudaMalloc((void **)&g_devflag, 4096);
        if (e == cudaSuccess) e = cudaMemset(g_devflag, 0, 4096);
        printf("[tgt] devflag=%p (%s)\n", (void *)g_devflag, cudaGetErrorName(e));
        if (e == cudaSuccess) {
            sh->dev_flag = (uint64_t)(uintptr_t)g_devflag;
            g_devflag_shm = sh;
            sh->flag_dev_ack = 0;
            std::thread([]() {
                cudaSetDevice(0);
                cudaError_t ce = cudaFree(0);
                cudaStream_t s = nullptr;
                cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking);
                printf("[helper] ctx init=%s stream=%p\n", cudaGetErrorName(ce), (void *)s);
                uint32_t last = 0xffffffff;
                while (!g_devflag_stop) {
                    uint32_t req = g_devflag_shm->trap_resume;
                    if (req != last) {
                        cudaError_t e1 = cudaMemcpyAsync((void *)(uintptr_t)g_devflag_shm->dev_flag,
                                                         &req, 4, cudaMemcpyHostToDevice, s);
                        cudaError_t e2 = cudaStreamSynchronize(s);
                        g_devflag_shm->flag_dev_ack = req;
                        __sync_synchronize();
                        fprintf(stderr, "[helper] flag <- %u (%s/%s)\n", req,
                                cudaGetErrorName(e1), cudaGetErrorName(e2));
                        last = req;
                    }
                    usleep(200);
                }
            }).detach();
        }
    }
    sh->pid = getpid();
    sh->app_client = L.app_client; sh->device = L.device; sh->subdev = L.subdev;
    sh->compute = L.compute; sh->app_session = L.app_session;
    sh->share_status[0] = g_policy_status[0];
    sh->share_status[1] = g_policy_status[1];
    sh->share_status[2] = g_policy_status[2];
    printf("[target] pid=%u client=%#x device=%#x subdev=%#x compute=%#x session=%#x\n",
           sh->pid, sh->app_client, sh->device, sh->subdev, sh->compute, sh->app_session);
    if (getenv("L3X_POLICY"))
        printf("[target] policy statuses: self/1/2=%#x self/4/0x40002=%#x agent/1/0x40003=%#x\n",
               (unsigned)g_policy_status[0], (unsigned)g_policy_status[1],
               (unsigned)g_policy_status[2]);

    if (getenv("L3X_TGTDBG")) {
        // cuda-gdb marks the inferior's libcuda as debugger-managed before any
        // RM session can hold it.  Mirror the flags it sets (see
        // FINDINGS_ZH.md v0.2 for the symbol list).
        void *lc = dlopen("libcuda.so.1", RTLD_NOW | RTLD_NOLOAD);
        const char *names[] = {"cudbgDebuggerInitialized", "cudbgEnablePreemptionDebugging",
                               "cudbgEnableLaunchBlocking"};
        for (const char *n : names) {
            void *p = lc ? dlsym(lc, n) : nullptr;
            if (p) { *(int *)p = 1; printf("[target] %s <- 1\n", n); }
            else    printf("[target] %s not found\n", n);
        }
    }

    if (getenv("L3X_PATCH")) patch_live_handler(sh);
    if (getenv("L3X_DBGWRITE") && g_cave_va) {
        CUcontext ctx = nullptr; cuCtxGetCurrent(&ctx);
        const void *tt_raw = nullptr;
        CUuuid te = sm120arena::uuid_of(sm120arena::kTrapHandlerEtid);
        CUresult er = cuGetExportTable(&tt_raw, &te);
        const void **tt = (const void **)tt_raw;
        struct { uint64_t flags, ssize; void *handle; uint64_t stub, hsize, rsv; } ti;
        memset(&ti, 0, sizeof ti);
        if (er == CUDA_SUCCESS && tt) ((CUresult(*)(CUcontext, void *))tt[20])(ctx, &ti);
        cuxtra::etbl::CUtoolsMemObjHandle_st *oh = nullptr;
        unsigned long long off = 0; unsigned long msz = 0;
        bool found = cuxtra::cuda::FindMemObjHandle(ctx, g_cave_va, &oh, &off, &msz);
        uint64_t pat[2] = {0x0000000000007918ULL, 0x000fc00000000000ULL};
        unsigned *dbuf = nullptr; cudaMalloc((void **)&dbuf, 16);
        cudaMemcpy(dbuf, pat, 16, cudaMemcpyHostToDevice);
        struct { uint32_t hMem, off; uint64_t len, pData; } wp;
        memset(&wp, 0, sizeof wp);
        wp.hMem = (uint32_t)(uintptr_t)ti.handle;
        wp.off = (uint32_t)off; wp.len = 16; wp.pData = (uint64_t)(uintptr_t)dbuf;
        int st = rm_control(L.fd, L.app_client, L.app_session, 0x83de0316, &wp, 0x18);
        uint64_t rb[2] = {0, 0};
        cuXtraMemcpyDtoH(rb, g_cave_va + 0x60, 16, 0);
        printf("[tgt] dbgwrite handle=%p found=%d off=%#llx st=%#x rb=%#llx/%#llx\n",
               ti.handle, (int)found, off, st, (unsigned long long)rb[0],
               (unsigned long long)rb[1]);
        cudaFree(dbuf);
    }
    if (getenv("L3X_ARM_PATCH") && getenv("L3X_GUARD_VICTIM")) {
        cudaMalloc((void **)&g_devflag, 256);
        cudaMemset(g_devflag, 0, 256);
        cudaKernel_t ck = nullptr;
        cudaError_t ce = cudaGetKernel(&ck, (const void *)spin_guard);
        CUfunction f = nullptr;
        CUresult kr = ce == cudaSuccess ? cuKernelGetFunction(&f, (CUkernel)(void *)ck)
                                        : CUDA_ERROR_UNKNOWN;
        uint64_t args[4] = { (uint64_t)(uintptr_t)g_devflag, 0, 0, 0 };
        if (kr == CUDA_SUCCESS) cuXtraSetDebuggerParams(f, args, 28);
        sh->dev_flag = (uint64_t)(uintptr_t)g_devflag;
        printf("[tgt] guard-victim flag=%p get=%s kernel=%#x\n", (void *)g_devflag,
               cudaGetErrorName(ce), kr);
    }
    if (getenv("L3X_ARM_PATCH")) {
        if (getenv("L3X_ARM_ALWAYS")) sh->trap_resume = 1;
        std::thread([]() {
            cuCtxSetCurrent((CUcontext)(uintptr_t)g_stub_ctx);
            int last = 0;   // patch_live_handler already installed the return stub
            while (!g_stub_stop) {
                int req = (int)g_sh_ptr->trap_resume;
                if (req != last) {
                    if (getenv("L3X_ARM_KERNEL") && g_arm_stream) {
                        write_code<<<1, 16, 0, g_arm_stream>>>(g_cave_dev,
                            req ? (const unsigned long long *)g_armed_dev : (const unsigned long long *)g_ret_dev);
                        cudaError_t we = cudaStreamSynchronize(g_arm_stream);
                        // write_code ends with fence.proxy.instruction, so the
                        // host-side (synchronising) invalidation is not needed.
                        g_sh_ptr->flag_dev_ack = req; __sync_synchronize();
                        fprintf(stderr, "[helper] cave <- %s via kernel (%s)\n",
                                req ? "GUARD" : "return", cudaGetErrorName(we));
                    } else {
                        const void *src = req ? (const void *)g_guard_stub : (const void *)g_ret_stub;
                        size_t w = cuXtraMemcpyHtoD(g_stub_cave, src, 128, 0);
                        cuXtraInvalInstrCache((CUcontext)(uintptr_t)g_stub_ctx);
                        g_sh_ptr->flag_dev_ack = req; __sync_synchronize();
                        fprintf(stderr, "[helper] cave <- %s (w=%zu)\n", req ? "GUARD" : "return", w);
                    }
                    last = req;
                }
                usleep(200);
            }
        }).detach();
    }
    if (getenv("L3X_PATCH_GUARD") && getenv("L3X_SETPARAM")) {
        static const char *kPtx =
            ".version 8.7\n.target sm_120\n.address_size 64\n"
            ".visible .entry guard_probe()\n{\n ret;\n}\n";
        CUmodule mod = nullptr; CUfunction f = nullptr;
        CUresult r1 = cuModuleLoadData(&mod, kPtx);
        CUresult r2 = r1 == CUDA_SUCCESS ? cuModuleGetFunction(&f, mod, "guard_probe") : r1;
        uint64_t args[4] = { (uint64_t)(uintptr_t)&sh->sig, 0, 0, 0 };
        if (r2 == CUDA_SUCCESS) cuXtraSetDebuggerParams(f, args, 28);
        printf("[tgt] setparam load=%#x func=%#x f=%p args[0]=%#llx\n", r1, r2, (void *)f,
               (unsigned long long)args[0]);
        if (r2 == CUDA_SUCCESS) {
            for (int i = 0; i < 4; i++) cuLaunchKernel(f, 1,1,1, 1,1,1, 0, nullptr, nullptr, nullptr);
            CUresult sr = cuStreamSynchronize(0);
            printf("[tgt] guard probe sync=%#x sig=%#x\n", sr, sh->sig);
        }
        // Now set the *victim* kernel's debugger params: c[0x0][0x170] = &sig.
        cudaKernel_t ck = nullptr;
        cudaError_t ce3 = cudaGetKernel(&ck, (const void *)spin_publish);
        CUfunction f3 = nullptr;
        CUresult kr3 = ce3 == cudaSuccess ? cuKernelGetFunction(&f3, (CUkernel)(void *)ck)
                                          : CUDA_ERROR_UNKNOWN;
        if (kr3 == CUDA_SUCCESS) cuXtraSetDebuggerParams(f3, args, 28);
        printf("[tgt] victim setparam get=%s kernel=%#x f=%p\n",
               cudaGetErrorName(ce3), kr3, (void *)f3);
    }

    if (getenv("L3X_PATCH_PAYLOAD") && getenv("L3X_PAYLOAD_TEST")) {
        unsigned *devflag = nullptr;
        cudaMalloc((void **)&devflag, 256);
        int fv = getenv("L3X_FLAG1") ? 1 : 0;
        cudaMemset(devflag, 0, 256);
        if (fv) cudaMemset(devflag, 1, 4);
        cudaKernel_t ck = nullptr;
        cudaError_t ce = cudaGetKernel(&ck, (const void *)spin_publish);
        CUfunction f = nullptr;
        CUresult kr = ce == cudaSuccess ? cuKernelGetFunction(&f, (CUkernel)(void *)ck)
                                        : CUDA_ERROR_UNKNOWN;
        uint64_t args[4] = { (uint64_t)(uintptr_t)devflag, 0, 0, 0 };
        if (kr == CUDA_SUCCESS) cuXtraSetDebuggerParams(f, args, 28);
        printf("[tgt] payload-test flag=%d get=%s kernel=%#x f=%p devflag=%p\n",
               fv, cudaGetErrorName(ce), kr, (void *)f, (void *)devflag);
        if (kr == CUDA_SUCCESS) {
            cudaDeviceProp p; cudaGetDeviceProperties(&p, 0);
            unsigned long long cyc = (unsigned long long)p.clockRate * 1000ULL * 5ULL;
            sh->counter = 0;
            struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
            spin_publish<<<1, 32>>>(&sh->counter, cyc);
            std::thread([fv]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                uint64_t zero[2] = {0, 0};
                int st = rm_control(L.fd, L.app_client, L.app_session,
                                    0x83de0317, zero, 8);
                printf("[tgt] self 0317{0,0} -> %#x (sess=%#x)\n", st, L.app_session);
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                int sr = rm_control(L.fd, L.app_client, L.app_session, 0x83de0318, nullptr, 0);
                printf("[tgt] self 0318 -> %#x (flag=%d)\n", sr, fv);
            }).detach();
            cudaError_t e2 = cudaDeviceSynchronize();
            clock_gettime(CLOCK_MONOTONIC, &b);
            double dt = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
            printf("[tgt] payload-test sync=%s elapsed=%.3fs counter=%llu\n",
                   cudaGetErrorName(e2), dt,
                   (unsigned long long)(sh->counter & ~(1ULL << 63)));
        }
        return 0;
    }

    if (getenv("L3X_PATCH_GUARD") && getenv("L3X_GUARD_TEST")) {
        cudaKernel_t ck = nullptr;
        cudaError_t ce = cudaGetKernel(&ck, (const void *)spin_guard);
        CUfunction f = nullptr;
        CUresult kr = ce == cudaSuccess ? cuKernelGetFunction(&f, (CUkernel)(void *)ck)
                                        : CUDA_ERROR_UNKNOWN;
        unsigned *devflag = nullptr;
        cudaMalloc((void **)&devflag, 256);
        cudaMemset(devflag, 0, 256);
        uint64_t args[4] = { (uint64_t)(uintptr_t)devflag, 0, 0, 0 };
        if (kr == CUDA_SUCCESS) cuXtraSetDebuggerParams(f, args, 28);
        printf("[tgt] guard-test get=%s kernel=%#x f=%p devflag=%p\n",
               cudaGetErrorName(ce), kr, (void *)f, (void *)devflag);
        if (kr == CUDA_SUCCESS) {
            cudaDeviceProp p; cudaGetDeviceProperties(&p, 0);
            unsigned long long cyc = (unsigned long long)p.clockRate * 1000ULL * 30ULL;
            sh->sig = 0; sh->counter = 0;
            struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
            spin_guard<<<1, 32>>>(devflag, &sh->counter, cyc);
            cudaError_t e2 = cudaDeviceSynchronize();
            clock_gettime(CLOCK_MONOTONIC, &b);
            double dt = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
            unsigned back = 0; cudaMemcpy(&back, devflag, 4, cudaMemcpyDeviceToHost);
            printf("[tgt] guard-test sync=%s elapsed=%.3fs devflag=%#x sig=%#x counter=%llu\n",
                   cudaGetErrorName(e2), dt, back, sh->sig,
                   (unsigned long long)(sh->counter & ~(1ULL << 63)));
        }
        return 0;
    }

    cudaDeviceProp prop;
    RT_CHECK(cudaGetDeviceProperties(&prop, 0));
    unsigned long long cycles = (unsigned long long)prop.clockRate * 1000ULL * spin_ms / 1000ULL;
    cudaStream_t vs;
    RT_CHECK(cudaStreamCreateWithFlags(&vs, cudaStreamNonBlocking));
    spin_publish<<<1, 32, 0, vs>>>(&sh->counter, 1000);   // warmup
    RT_CHECK(cudaStreamSynchronize(vs));
    sh->counter = 0;

    // Publish now: the warmup above created the channel/KernelGraphicsObject
    // that the victim will actually run on.
    sh->ncompute = L.ncompute;
    for (uint32_t i = 0; i < L.ncompute && i < 16; i++) sh->computes[i] = L.computes[i];
    sh->compute = L.compute;
    __sync_synchronize();
    sh->magic = 0x4c335831;
    printf("[target] compute objects:"); 
    for (uint32_t i = 0; i < L.ncompute; i++) printf(" %#x", L.computes[i]);
    printf("\n");

    if (getenv("L3X_LD_DIAG")) {
        unsigned *dbuf = nullptr;
        cudaMalloc((void **)&dbuf, 256);
        unsigned init[2] = {0xdead, 0};
        cudaMemcpy(dbuf, init, sizeof init, cudaMemcpyHostToDevice);
        cudaKernel_t ck = nullptr;
        cudaError_t ce = cudaGetKernel(&ck, (const void *)spin_publish);
        CUfunction f = nullptr;
        CUresult kr = ce == cudaSuccess ? cuKernelGetFunction(&f, (CUkernel)(void *)ck)
                                        : CUDA_ERROR_UNKNOWN;
        uint64_t args[4] = { (uint64_t)(uintptr_t)dbuf, 0, 0, 0 };
        if (kr == CUDA_SUCCESS) cuXtraSetDebuggerParams(f, args, 28);
        printf("[tgt] ld-diag buf=%p get=%s kernel=%#x\n", (void *)dbuf,
               cudaGetErrorName(ce), kr);
        sh->dev_flag = (uint64_t)(uintptr_t)dbuf;
    }
    if (getenv("L3X_PAYLOAD_AGENT")) {
        unsigned *pflag = nullptr;
        cudaMalloc((void **)&pflag, 256);
        cudaMemset(pflag, 0, 256);
        if (getenv("L3X_FLAG1")) cudaMemset(pflag, 1, 4);
        cudaKernel_t ck = nullptr;
        cudaError_t ce = cudaGetKernel(&ck, (const void *)spin_publish);
        CUfunction f = nullptr;
        CUresult kr = ce == cudaSuccess ? cuKernelGetFunction(&f, (CUkernel)(void *)ck)
                                        : CUDA_ERROR_UNKNOWN;
        uint64_t args[4] = { (uint64_t)(uintptr_t)pflag, 0, 0, 0 };
        if (kr == CUDA_SUCCESS) cuXtraSetDebuggerParams(f, args, 28);
        printf("[tgt] payload-agent flag=%d buf=%p get=%s kernel=%#x\n",
               getenv("L3X_FLAG1") ? 1 : 0, (void *)pflag, cudaGetErrorName(ce), kr);
    }
    if (getenv("L3X_CLOCK_TEST")) {
        cudaKernel_t ck = nullptr;
        cudaError_t ce = cudaGetKernel(&ck, (const void *)spin_publish);
        CUfunction f = nullptr;
        CUresult kr = ce == cudaSuccess ? cuKernelGetFunction(&f, (CUkernel)(void *)ck)
                                        : CUDA_ERROR_UNKNOWN;
        uint64_t args[4] = { (uint64_t)(uintptr_t)&sh->sig, 0, 0, 0 };
        if (kr == CUDA_SUCCESS) cuXtraSetDebuggerParams(f, args, 28);
        printf("[tgt] clock-test get=%s kernel=%#x sig_off=%zu\n",
               cudaGetErrorName(ce), kr, (size_t)((char *)&sh->sig - (char *)sh));
    }
    unsigned *reason_buf = nullptr;
    if (getenv("L3X_REASON_TEST")) {
        cudaMalloc((void **)&reason_buf, 256);
        cudaMemset(reason_buf, 0, 256);
        cudaKernel_t ck = nullptr;
        cudaError_t ce = cudaGetKernel(&ck, (const void *)spin_publish);
        CUfunction f = nullptr;
        CUresult kr = ce == cudaSuccess ? cuKernelGetFunction(&f, (CUkernel)(void *)ck)
                                        : CUDA_ERROR_UNKNOWN;
        uint64_t args[4] = { (uint64_t)(uintptr_t)reason_buf, 0, 0, 0 };
        if (kr == CUDA_SUCCESS) cuXtraSetDebuggerParams(f, args, 28);
        printf("[target] reason-test buf=%p get=%s kernel=%#x f=%p\n",
               (void *)reason_buf, cudaGetErrorName(ce), kr, (void *)f);
    }

    printf("[target] launching victim %ld blocks ~%ld ms\n", blocks, spin_ms);
    if (getenv("L3X_ARM_PATCH") && getenv("L3X_GUARD_VICTIM"))
        spin_guard<<<(int)blocks, 32, 0, vs>>>((volatile unsigned *)g_devflag, &sh->counter, cycles);
    else
        spin_publish<<<(int)blocks, 32, 0, vs>>>(&sh->counter, cycles);
    if (getenv("L3X_POKE") && g_cave_va) {
        unsigned long long va = g_cave_va + 0x60;
        poke64<<<1, 1>>>(va, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);
        cudaError_t pe = cudaDeviceSynchronize();
        unsigned long long rb[2] = {0, 0};
        cuXtraMemcpyDtoH(rb, va, 16, 0);
        printf("[tgt] poke64 sync=%s rb=%#llx/%#llx\n", cudaGetErrorName(pe),
               (unsigned long long)rb[0], (unsigned long long)rb[1]);
    }
    if (getenv("L3X_XTRATRIG")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CUcontext xctx = nullptr; cuCtxGetCurrent(&xctx);
        cuXtraTriggerTrap(xctx);
        printf("[tgt] cuXtraTriggerTrap(%p) issued\n", (void *)xctx);
    }
    RT_CHECK(cudaGetLastError());

    std::thread self_resumer([sh]() {
        uint32_t last_req = 0;
        while (!sh->done) {
            if (sh->resume_req != last_req) {
                last_req = sh->resume_req;
                int st = rm_control(L.fd, L.app_client, L.app_session,
                                    0x83de0318, nullptr, 0);
                sh->self_resume_status = st;
                __sync_synchronize();
                sh->resume_ack = last_req;
                printf("[target] own-session 0318 -> %#x\n", st);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Wait for the agent to publish its own client, then try to grant it debug
    // rights on our KernelGraphicsObject via NV_ESC_RM_SHARE (escape 0x35).
    if (getenv("L3X_SHARE")) {
        for (int i = 0; i < 600 && !sh->agent_ready; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (sh->agent_ready) {
            printf("[target] agent client=%#x; sweeping RM_SHARE types\n", sh->agent_client);
            for (uint16_t t = 0; t <= 15; t++) {
                sh->share_status[t] = 0x7fffffff;
                sh->share_status[t] = rm_share(L.fd, sh->app_client, sh->compute,
                                               sh->agent_client, t);
                sh->share_ret[t] = (int32_t)sh->share_status[t];
                printf("[target] share type=%2u obj=%#x -> client %#x => %#x\n",
                       t, sh->compute, sh->agent_client, sh->share_status[t]);
                fflush(stdout);
            }
        } else {
            printf("[target] agent never published its client\n");
        }
        sh->share_done = 1;
    }

    struct timespec ts0, ts1; clock_gettime(CLOCK_MONOTONIC, &ts0);
    cudaError_t se = cudaStreamSynchronize(vs);
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    double sync_s = (ts1.tv_sec - ts0.tv_sec) + (ts1.tv_nsec - ts0.tv_nsec) * 1e-9;
    printf("[target] victim finished: %s counter=%llu sync=%.3fs\n", cudaGetErrorName(se),
           (unsigned long long)(sh->counter & ~(1ULL << 63)), sync_s);
    if (getenv("L3X_PATCH"))
        printf("[target] trap-handler signature: %#x/%#x reason=%#x (%s)\n", sh->sig, sh->sig2, sh->sig_reason,
               sh->sig == 0x5aa5 ? "LIVE HANDLER EXECUTED <<<" : "not executed");
    if (getenv("L3X_LD_DIAG") && sh->dev_flag) {
        unsigned back[4] = {0, 0, 0, 0};
        cudaMemcpy(back, (void *)(uintptr_t)sh->dev_flag, sizeof back, cudaMemcpyDeviceToHost);
        printf("[tgt] ld-diag in=%#x ldout=%#x ctrl=%#x\n", back[0], back[1], back[2]);
    }
    if (getenv("L3X_REASON_TEST") && reason_buf) {
        unsigned rv = 0;
        cudaMemcpy(&rv, reason_buf, 4, cudaMemcpyDeviceToHost);
        printf("[target] reason-test final reason=%#x\n", rv);
    }
    restore_live_handler();
    sh->done = 1;
    self_resumer.join();
    return 0;
}

// -------------------------------------------------------------------- agent
static int run_agent(const char *shmname, long hold_ms, uint32_t flag, bool modes,
                     const std::string &objkind, bool do_ptrace)
{
    Shm *sh = nullptr;
    for (int i = 0; i < 400 && !sh; i++) {
        sh = open_shm(shmname, false);
        if (!sh) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!sh) { fprintf(stderr, "[agent] start the target first\n"); return 1; }
    printf("[agent] shm ready; initialising our own RM client\n");

    // Attach *before* this process touches CUDA: RM evaluates the caller's
    // rights when its own client/device objects are created.
    bool attached = false;
    if (do_ptrace && sh->pid != 0) {
        long pr = ptrace(PTRACE_ATTACH, (pid_t)sh->pid, nullptr, nullptr);
        if (pr == 0) {
            int wst = 0;
            pid_t w = waitpid((pid_t)sh->pid, &wst, 0);
            printf("[agent] PTRACE_ATTACH pid=%u -> %d (waitpid=%d stopped=%d)\n",
                   sh->pid, (int)pr, (int)w, WIFSTOPPED(wst) ? WSTOPSIG(wst) : -1);
            attached = true;
            long cr = ptrace(PTRACE_CONT, (pid_t)sh->pid, nullptr, nullptr);
            printf("[agent] PTRACE_CONT pid=%u -> %d (%s)\n", sh->pid, (int)cr,
                   cr == 0 ? "target running, still traced" : strerror(errno));
        } else {
            printf("[agent] PTRACE_ATTACH pid=%u failed: %s\n", sh->pid, strerror(errno));
        }
    }

    RT_CHECK(cudaFree(0));       // full libcuda init in *this* process
    printf("[agent] own fd=%d client=%#x device=%#x subdev=%#x compute=%#x\n",
           L.fd, L.app_client, L.device, L.subdev, L.compute);
    if (L.fd < 0 || !L.app_client || !L.device) { fprintf(stderr, "[agent] no own stack\n"); return 1; }

    sh->agent_client = L.app_client;
    sh->agent_ready = 1;
    printf("[agent] published own client=%#x device=%#x\n", L.app_client, L.device);

    for (int i = 0; i < 400 && sh->magic != 0x4c335831; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (sh->magic != 0x4c335831) { fprintf(stderr, "[agent] target never published\n"); return 1; }
    printf("[agent] target pid=%u client=%#x device=%#x subdev=%#x compute=%#x session=%#x\n",
           sh->pid, sh->app_client, sh->device, sh->subdev, sh->compute, sh->app_session);

    if (do_ptrace && !attached) {
        long pr = ptrace(PTRACE_ATTACH, (pid_t)sh->pid, nullptr, nullptr);
        if (pr == 0) {
            int wst = 0;
            pid_t w = waitpid((pid_t)sh->pid, &wst, 0);
            printf("[agent] late PTRACE_ATTACH pid=%u (waitpid=%d stopped=%d)\n",
                   sh->pid, (int)w, WIFSTOPPED(wst) ? WSTOPSIG(wst) : -1);
            long cr = ptrace(PTRACE_CONT, (pid_t)sh->pid, nullptr, nullptr);
            printf("[agent] PTRACE_CONT -> %d (%s)\n", (int)cr,
                   cr == 0 ? "target running, still traced" : strerror(errno));
            attached = (cr == 0);
        } else {
            printf("[agent] late PTRACE_ATTACH pid=%u failed: %s\n", sh->pid, strerror(errno));
        }
    }

    if (getenv("L3X_POLICY")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        printf("[agent] policy sweep: %u target compute objects, ptrace=%d\n",
               sh->ncompute, (int)attached);
        for (uint32_t i = 0; i < sh->ncompute; i++) {
            uint32_t obj = sh->computes[i];
            sh->sig = 0; sh->sig_reason = 0; sh->sig2 = 0;
            sh->trap_resume = 0;
            __sync_synchronize();
            NV83DE p83; memset(&p83, 0, sizeof p83);
            p83.hAppClient = sh->app_client; p83.hClass3dObject = obj;
            const uint32_t H = 0x5c00f00c + i;
            int st = rm_alloc(L.fd, L.app_client, L.device, H, CLASS_SMDBGSSN, &p83, 0);
            if (st) { printf("[pol] compute[%u]=%#x session -> %#x\n", i, obj, st); continue; }
            uint32_t one = 1;
            int s1 = rm_control(L.fd, L.app_client, H, 0x83de031f, &one, 4);
            int s2 = rm_control(L.fd, L.app_client, H, 0x83de0307, &one, 4);
            int s3 = rm_control(L.fd, L.app_client, H, 0x83de032a, &one, 4);
            if (getenv("L3X_HOLD_ONLY")) {
                int hs1 = rm_control(L.fd, L.app_client, H, 0x83de031f, &one, 4);
                int hs2 = rm_control(L.fd, L.app_client, H, 0x83de0307, &one, 4);
                int hs3 = rm_control(L.fd, L.app_client, H, 0x83de032a, &one, 4);
                uint64_t hs[2] = {1, 0};
                int hss = rm_control(L.fd, L.app_client, H, 0x83de0317, hs, 8);
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                uint64_t hv0 = sh->counter, hv1;
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                hv1 = sh->counter;
                int hsr = rm_control(L.fd, L.app_client, H, 0x83de0318, nullptr, 0);
                printf("[hold] compute[%u] modes %#x/%#x/%#x susp %#x resume %#x counter %llu -> %llu (%s)\n",
                       i, hs1, hs2, hs3, hss, hsr, (unsigned long long)hv0,
                       (unsigned long long)hv1, hv1 == hv0 ? "FROZEN" : "running");
                fflush(stdout);
                rm_free(L.fd, L.app_client, L.device, H);
                continue;
            }
            if (getenv("L3X_PATCH_DEVFLAG")) {
                sh->flag_dev_ack = 0xffffffff;
                sh->trap_resume = 1;                              // arm via helper
                __sync_synchronize();
                for (int k = 0; k < 3000 && sh->flag_dev_ack != 1; k++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                printf("[pol]   devflag arm ack=%#x (dev=%#llx)\n", sh->flag_dev_ack,
                       (unsigned long long)sh->dev_flag);
            } else if (getenv("L3X_PATCH_PARK")) {
                sh->trap_resume = 1;   // arm parking
            }
            __sync_synchronize();
            uint64_t poll_susp[2] = {0, 0};
            int s4 = rm_control(L.fd, L.app_client, H, 0x83de0317, poll_susp, 8);
            uint64_t p0 = sh->counter;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            uint64_t p1 = sh->counter;
            if (!getenv("L3X_PATCH_PARK"))
                rm_control(L.fd, L.app_client, H, 0x83de0318, nullptr, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            uint64_t p2 = sh->counter;
            if (getenv("L3X_PATCH_PARK")) {
                printf("[pol] park compute[%u]=%#x sess=%#x modes %#x/%#x/%#x prime %#x "
                       "[%llu->%llu->%llu %s] sig=%#x/%#x reason=%#x\n", i, obj, H, s1, s2, s3, s4,
                       (unsigned long long)p0, (unsigned long long)p1, (unsigned long long)p2,
                       p1 == p0 ? "PARKED" : "running", sh->sig, sh->sig2, sh->sig_reason);
                fflush(stdout);
                if (p1 == p0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    uint64_t v0 = sh->counter, v1 = v0;
                    sh->trap_resume = 0;   // release the parked trap
                    __sync_synchronize();
                    if (getenv("L3X_PATCH_DEVFLAG")) {
                        for (int k = 0; k < 3000 && sh->flag_dev_ack != 0; k++)
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        printf("[pol]   devflag release ack=%#x\n", sh->flag_dev_ack);
                    }
                    for (int k = 0; k < 600 && v1 == v0; k++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        v1 = sh->counter;
                    }
                    rm_control(L.fd, L.app_client, H, 0x83de0318, nullptr, 0);
                    printf("[pol]   release flag -> counter %llu -> %llu (%s), state=%#x\n",
                           (unsigned long long)v0, (unsigned long long)v1,
                           v1 == v0 ? "STILL PARKED" : "RESUMED <<<", sh->sig2);
                    fflush(stdout);
                    rm_free(L.fd, L.app_client, L.device, H);
                    if (v1 != v0) break;
                    continue;
                }
                rm_free(L.fd, L.app_client, L.device, H);
                continue;
            }
            if (getenv("L3X_PRIME_ONLY")) {
                printf("[pol] compute[%u]=%#x prime %#x [%llu->%llu->%llu %s]\n", i, obj, s4,
                       (unsigned long long)p0, (unsigned long long)p1,
                       (unsigned long long)p2, p1 == p0 ? "STOPPED" : "running");
                fflush(stdout);
                rm_free(L.fd, L.app_client, L.device, H);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            uint64_t susp[2] = {1, 0};
            int ss = rm_control(L.fd, L.app_client, H, 0x83de0317, susp, 8);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            uint64_t v0 = sh->counter, v1;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            v1 = sh->counter;
            uint32_t gen = sh->resume_req + 1;
            sh->resume_req = gen;
            for (int k = 0; k < 600 && sh->resume_ack != gen; k++)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            int sr = sh->resume_ack == gen ? sh->self_resume_status : (int32_t)0x7fffffff;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            uint64_t v2 = sh->counter;
            if (sh->resume_ack != gen) {
                int ar = rm_control(L.fd, L.app_client, H, 0x83de0318, nullptr, 0);
                printf("[pol]   self-resume timed out; agent 0318 -> %#x\n", ar);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                v2 = sh->counter;
            }
            printf("[pol] compute[%u]=%#x sess=%#x modes %#x/%#x/%#x prime %#x [%llu->%llu->%llu %s] "
                   "susp %#x hold %llu->%llu (%s) self-resume %#x after %llu (%s)\n",
                   i, obj, H, s1, s2, s3, s4,
                   (unsigned long long)p0, (unsigned long long)p1, (unsigned long long)p2,
                   p1 == p0 ? "STOPPED" : "running",
                   ss, (unsigned long long)v0, (unsigned long long)v1,
                   v1 == v0 ? "FROZEN" : "running", sr, (unsigned long long)v2,
                   v2 == v1 ? "STILL FROZEN" : "RESUMED");
            fflush(stdout);
            if (sh->sig) {
                printf("[pol] trap-handler signature observed: %#x/%#x reason=%#x <<<\n", sh->sig, sh->sig2, sh->sig_reason);
                break;
            }
            rm_free(L.fd, L.app_client, L.device, H);
            if (v1 == v0 && v2 != v1) break;
        }
        if (attached) ptrace(PTRACE_DETACH, (pid_t)sh->pid, nullptr, nullptr);
        return 0;
    }
    if (getenv("L3X_SHARE")) {
        for (int i = 0; i < 600 && !sh->share_done; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        printf("[agent] target share sweep done=%u; shared-client alloc test:\n", sh->share_done);
        int n0 = 0;
        for (int t = 0; t <= 15; t++) {
            if (sh->share_status[t] == 0) n0++;
            if (sh->share_status[t] != (int32_t)0x7fffffff)
                printf("[agent]   target share type %2d -> %#x\n", t,
                       (unsigned)sh->share_status[t]);
        }
        printf("[agent]   %d/16 share calls returned status 0\n", n0);
    }

    // Sweep: which (hAppClient, hClass3dObject) pairs will RM accept from here?
    struct Cand { const char *name; uint32_t h; };
    Cand objs[] = {{"t_compute", sh->compute}, {"t_channel", 0}, {"t_device", sh->device},
                   {"t_subdev", sh->subdev}, {"t_session", sh->app_session},
                   {"a_compute", L.compute}, {"a_device", L.device}, {"zero", 0}};
    Cand clis[] = {{"t_client", sh->app_client}, {"a_client", L.app_client}, {"zero", 0}};
    for (auto &c : clis) for (auto &o : objs) {
        if (!o.h) continue;
        NV83DE p83; memset(&p83, 0, sizeof p83);
        p83.hAppClient = c.h; p83.hClass3dObject = o.h;
        const uint32_t H = 0x5c00f00c;
        int st = rm_alloc(L.fd, L.app_client, L.device, H, CLASS_SMDBGSSN, &p83, 0);
        printf("[sweep] hAppClient=%-9s(%#x) hClass3dObject=%-10s(%#x) -> %#x%s\n",
               c.name, c.h, o.name, o.h, st, st ? "" : "   <<< ACCEPTED");
        if (!st) {
            uint32_t one = 1;
            int s1 = rm_control(L.fd, L.app_client, H, 0x83de031f, &one, 4);
            int s2 = rm_control(L.fd, L.app_client, H, 0x83de0307, &one, 4);
            int s3 = rm_control(L.fd, L.app_client, H, 0x83de032a, &one, 4);
            // cuda-gdb's attach does a polling stop/resume round before the
            // holding suspend (r3_ioctl.log: 0317{0,0} ... 0318 ... 0317{1,0}).
            if (getenv("L3X_PATCH_DEVFLAG")) {
                sh->flag_dev_ack = 0xffffffff;
                sh->trap_resume = 1;                              // arm via helper
                __sync_synchronize();
                for (int k = 0; k < 3000 && sh->flag_dev_ack != 1; k++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                printf("[pol]   devflag arm ack=%#x (dev=%#llx)\n", sh->flag_dev_ack,
                       (unsigned long long)sh->dev_flag);
            } else if (getenv("L3X_PATCH_PARK")) {
                sh->trap_resume = 1;   // arm parking
            }
            __sync_synchronize();
            uint64_t poll_susp[2] = {0, 0};
            int s4 = rm_control(L.fd, L.app_client, H, 0x83de0317, poll_susp, 8);
            int s5 = rm_control(L.fd, L.app_client, H, 0x83de0318, nullptr, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            uint64_t susp[2] = {1, 0};
            int ss = rm_control(L.fd, L.app_client, H, 0x83de0317, susp, 8);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            uint64_t v0 = sh->counter, v1;
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            v1 = sh->counter;
            rm_control(L.fd, L.app_client, H, 0x83de0318, nullptr, 0);
            printf("[sweep]   modes %#x/%#x/%#x prime %#x/%#x suspend=%#x counter %llu -> %llu (%s)\n",
                   s1, s2, s3, s4, s5, ss,
                   (unsigned long long)v0, (unsigned long long)v1,
                   v1 == v0 ? "FROZEN <<<" : "still running");
            rm_free(L.fd, L.app_client, L.device, H);
        }
    }

    if (attached) {
        ptrace(PTRACE_DETACH, (pid_t)sh->pid, nullptr, nullptr);
        printf("[agent] PTRACE_DETACH pid=%u\n", sh->pid);
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string role = "target", shmname = "/sm120_l3x", objkind = "compute";
    long spin_ms = 40000, hold_ms = 1000, blocks = 8;
    uint32_t flag = 1;
    bool modes = true, do_ptrace = true;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--role") role = next();
        else if (a == "--shm") shmname = next();
        else if (a == "--obj") objkind = next();
        else if (a == "--spin-ms") spin_ms = atol(next().c_str());
        else if (a == "--hold-ms") hold_ms = atol(next().c_str());
        else if (a == "--blocks") blocks = atol(next().c_str());
        else if (a == "--flag") flag = (uint32_t)strtoul(next().c_str(), nullptr, 0);
        else if (a == "--no-modes") modes = false;
        else if (a == "--no-ptrace") do_ptrace = false;
    }
    if (role == "target") return run_target(shmname.c_str(), spin_ms, blocks);
    if (role == "agent")  return run_agent(shmname.c_str(), hold_ms, flag, modes, objkind, do_ptrace);

    // "both": fork so that the agent is the *parent* of the target -
    // /proc/sys/kernel/yama/ptrace_scope=1 only lets a process trace its
    // descendants, and RM needs the ptrace relationship to authorise the
    // cross-client 0x83de session.
    pid_t kid = fork();
    if (kid < 0) { perror("fork"); return 1; }
    if (kid == 0) {
        int rc = run_target(shmname.c_str(), spin_ms, blocks);
        fflush(stdout);
        _exit(rc);
    }
    printf("[sup] target child pid=%d\n", (int)kid);
    int rc = run_agent(shmname.c_str(), hold_ms, flag, modes, objkind, do_ptrace);
    kill(kid, SIGTERM);
    int wst = 0;
    waitpid(kid, &wst, 0);
    return rc;
}
