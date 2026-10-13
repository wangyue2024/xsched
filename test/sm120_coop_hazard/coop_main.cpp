/*
 * =====================================================================
 *  coop_main.cpp - Cooperative-groups x XSched Level-2 hazard demo
 *
 *  Demonstrates, on real hardware and with driver-API calls only, that
 *  an L2-instrumented cooperative kernel can enter a PARTIAL state:
 *
 *    - some blocks exit at the guardian entry checkpoint (blocked), and
 *    - the remaining blocks enter the body and wait at a grid barrier
 *      for peers that will never arrive.
 *
 *  A real cooperative_groups grid.sync() has no timeout, so that state
 *  is a permanent DEADLOCK.  This test replaces the real sync with a
 *  BOUNDED barrier (coop_kernel.cu) so the condition is reported as
 *  data and the GPU can never hang.
 *
 *  Two experiments (mirroring InstrumentContext / InstrumentManager):
 *
 *   [E1] Deterministic: inject the exact post-race state by hand.
 *        Craft the preempt buffer as if K of N blocks had been blocked
 *        at the guardian (exit=1,restore=1 for K blocks) and relaunch
 *        through the RESUME entry exactly like Reactivate() does.
 *        Expect: K blocks enter the body, all K time out on the missing
 *        N-K peers  ->  hazard condition reproduced, bounded, reported.
 *        Control: with ALL restore flags set, the full grid re-enters
 *        and the barrier passes (the verified safe resume path).
 *
 *   [E2] Race amplifier: launch the guarded cooperative kernel and let
 *        a companion kernel (second stream) set the Deactivate flag
 *        DURING the entry window, timed precisely from GPU-side:
 *        companion waits for the first body-entry marker, dwells a
 *        swept amount of time, then writes global_exit_flag = 1.
 *        Partial iterations (0 < entered < N) are counted as hits.
 *
 *  Exit code = number of failed checks (0 = all pass).
 * =====================================================================
 */

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "cuxtra/cuxtra.h"
#include "mve_arrays.h" /* guardian/resume arrays, generated from sm120.cpp */

/* ---------------- minimal dynamic CUDA driver API ---------------- */

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st  *CUcontext;
typedef struct CUmod_st  *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef struct CUstream_st *CUstream;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR  75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR  76
#define CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT      16
#define CU_DEVICE_ATTRIBUTE_COOPERATIVE_LAUNCH        95

#define CU_LAUNCH_ATTRIBUTE_COOPERATIVE 2

typedef struct CUlaunchAttribute_st {
    int id;
    int pad;
    unsigned char value[64];
} CUlaunchAttribute;

typedef struct CUlaunchConfig_st {
    unsigned gridDimX, gridDimY, gridDimZ;
    unsigned blockDimX, blockDimY, blockDimZ;
    unsigned sharedMemBytes;
    CUstream hStream;
    CUlaunchAttribute *attrs;
    unsigned numAttrs;
} CUlaunchConfig;

typedef CUresult (*pfn_cuInit)(unsigned);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice *, int);
typedef CUresult (*pfn_cuDeviceGetName)(char *, int, CUdevice);
typedef CUresult (*pfn_cuDeviceGetAttribute)(int *, int, CUdevice);
typedef CUresult (*pfn_cuCtxCreate)(CUcontext *, unsigned, CUdevice);
typedef CUresult (*pfn_cuModuleLoad)(CUmodule *, const char *);
typedef CUresult (*pfn_cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
typedef CUresult (*pfn_cuMemAlloc)(CUdeviceptr *, size_t);
typedef CUresult (*pfn_cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
typedef CUresult (*pfn_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
typedef CUresult (*pfn_cuMemsetD8)(CUdeviceptr, unsigned char, size_t);
typedef CUresult (*pfn_cuStreamCreate)(CUstream *, unsigned);
typedef CUresult (*pfn_cuStreamSynchronize)(CUstream);
typedef CUresult (*pfn_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                       unsigned, unsigned, unsigned, unsigned,
                                       CUstream, void **, void **);
typedef CUresult (*pfn_cuLaunchKernelEx)(const CUlaunchConfig *, CUfunction,
                                         void **, void **);
typedef CUresult (*pfn_cuOccupancy)(int *, CUfunction, int, size_t);
typedef CUresult (*pfn_cuEventCreate)(void **, unsigned);
typedef CUresult (*pfn_cuEventRecord)(void *, CUstream);
typedef CUresult (*pfn_cuStreamWaitEvent)(CUstream, void *, unsigned);
typedef CUresult (*pfn_cuGetErrorName)(CUresult, const char **);

static pfn_cuInit         drvInit;
static pfn_cuDeviceGet    drvGet;
static pfn_cuDeviceGetName drvGetName;
static pfn_cuDeviceGetAttribute drvGetAttr;
static pfn_cuCtxCreate    drvCtxCreate;
static pfn_cuModuleLoad   drvModuleLoad;
static pfn_cuModuleGetFunction drvModuleGetFunction;
static pfn_cuMemAlloc     drvMemAlloc;
static pfn_cuMemcpyHtoD   drvMemcpyHtoD;
static pfn_cuMemcpyDtoH   drvMemcpyDtoH;
static pfn_cuMemsetD8     drvMemsetD8;
static pfn_cuStreamCreate drvStreamCreate;
static pfn_cuStreamSynchronize drvStreamSync;
static pfn_cuLaunchKernel drvLaunchKernel;
static pfn_cuLaunchKernelEx drvLaunchKernelEx;
static pfn_cuOccupancy    drvOccupancy;
static pfn_cuEventCreate  drvEventCreate;
static pfn_cuEventRecord  drvEventRecord;
static pfn_cuStreamWaitEvent drvStreamWaitEvent;
static pfn_cuGetErrorName drvGetErrorName;

static HMODULE g_cuda = nullptr;

template <typename T>
static T cudaSym(const char *name)
{
    void *p = reinterpret_cast<void *>(GetProcAddress(g_cuda, name));
    if (p == nullptr) {
        std::printf("FATAL: nvcuda.dll export '%s' not found\n", name);
        std::exit(2);
    }
    return reinterpret_cast<T>(p);
}

static void loadDriverFns()
{
    drvInit            = cudaSym<pfn_cuInit>("cuInit");
    drvGet             = cudaSym<pfn_cuDeviceGet>("cuDeviceGet");
    drvGetName         = cudaSym<pfn_cuDeviceGetName>("cuDeviceGetName");
    drvGetAttr         = cudaSym<pfn_cuDeviceGetAttribute>("cuDeviceGetAttribute");
    drvCtxCreate       = cudaSym<pfn_cuCtxCreate>("cuCtxCreate_v2");
    drvModuleLoad      = cudaSym<pfn_cuModuleLoad>("cuModuleLoad");
    drvModuleGetFunction = cudaSym<pfn_cuModuleGetFunction>("cuModuleGetFunction");
    drvMemAlloc        = cudaSym<pfn_cuMemAlloc>("cuMemAlloc_v2");
    drvMemcpyHtoD      = cudaSym<pfn_cuMemcpyHtoD>("cuMemcpyHtoD_v2");
    drvMemcpyDtoH      = cudaSym<pfn_cuMemcpyDtoH>("cuMemcpyDtoH_v2");
    drvMemsetD8        = cudaSym<pfn_cuMemsetD8>("cuMemsetD8_v2");
    drvStreamCreate    = cudaSym<pfn_cuStreamCreate>("cuStreamCreate");
    drvStreamSync      = cudaSym<pfn_cuStreamSynchronize>("cuStreamSynchronize");
    drvLaunchKernel    = cudaSym<pfn_cuLaunchKernel>("cuLaunchKernel");
    drvEventCreate     = cudaSym<pfn_cuEventCreate>("cuEventCreate");
    drvEventRecord     = cudaSym<pfn_cuEventRecord>("cuEventRecord");
    drvStreamWaitEvent = cudaSym<pfn_cuStreamWaitEvent>("cuStreamWaitEvent");
    drvGetErrorName    = cudaSym<pfn_cuGetErrorName>("cuGetErrorName");

    void *p = reinterpret_cast<void *>(GetProcAddress(g_cuda, "cuLaunchKernelEx"));
    drvLaunchKernelEx = reinterpret_cast<pfn_cuLaunchKernelEx>(p); /* optional */
    p = reinterpret_cast<void *>(
        GetProcAddress(g_cuda, "cuOccupancyMaxActiveBlocksPerMultiprocessor"));
    drvOccupancy = reinterpret_cast<pfn_cuOccupancy>(p);           /* optional */
}

/* ------------------------------ state ------------------------------ */

#define MAX_BLOCKS       4096
#define OUT_FILL         0xCCCCCCCCCCCCCCCCull
#define BLOCK_THREADS    128
#define DEF_TIMEOUT_NS   100000000ull   /* barrier deadline: 100 ms */

static int g_fail = 0;

#define REPORT(name, ok)                                                  \
    do {                                                                  \
        std::printf("  [%s] %s\n", (ok) ? "PASS" : "FAIL", name);         \
        if (!(ok)) ++g_fail;                                              \
    } while (0)

#define CHECK(call)                                                       \
    do {                                                                  \
        CUresult _e = (call);                                             \
        if (_e != CUDA_SUCCESS) {                                         \
            const char *_s = "?";                                         \
            drvGetErrorName(_e, &_s);                                     \
            std::printf("  [ERR ] %s -> CUresult %d (%s)\n",              \
                        #call, (int)_e, _s);                              \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

static CUcontext ctx = nullptr;
static CUstream  streamA = nullptr;  /* cooperative kernel            */
static CUstream  streamB = nullptr;  /* companion (setflag) kernel    */
static CUmodule  mod = nullptr;
static CUfunction fn_coop = nullptr;
static CUfunction fn_setflag = nullptr;

static CUdeviceptr d_out = 0;      /* per-block completion markers      */
static CUdeviceptr d_buf = 0;      /* preempt buffer (16 + 8*MAX)       */
static CUdeviceptr d_barrier = 0;  /* grid barrier state (16 B)         */
static CUdeviceptr d_diag = 0;     /* diagnostics (32 B)                */
static CUdeviceptr d_entry = 0;    /* per-block entry timestamps        */
static CUdeviceptr d_setns = 0;    /* companion flag-set timestamp      */
static void *ev_reset = nullptr;   /* orders the reset with both streams */

static CUdeviceptr ep_orig = 0;
static CUdeviceptr ep_guardian = 0;
static CUdeviceptr ep_resume = 0;

static bool g_use_attr = true;     /* cooperative launch attribute      */
static uint64_t g_spread_ns = 0;   /* entry-window width measured in S1 */
static unsigned g_threads = BLOCK_THREADS; /* threads per block (--threads) */

static uint64_t h_out[MAX_BLOCKS];
static std::vector<uint8_t> h_buf(16 + 8 * MAX_BLOCKS);

static double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static size_t bufBytes(unsigned nblocks) { return 16 + 8 * (size_t)nblocks; }

/* ------------------------------ helpers ---------------------------- */

struct Diag
{
    uint32_t entered;   /* diag[0] blocks that reached the body          */
    uint32_t timedout;  /* diag[1] blocks whose barrier wait expired     */
    uint32_t synced;    /* diag[2] blocks released by a full barrier     */
    uint32_t blocked;   /* exit flags set in the preempt buffer          */
    uint32_t restore;   /* restore flags set in the preempt buffer       */
    uint32_t markers;   /* out[] slots written                           */
    uint64_t first_ns;  /* first body-entry timestamp                    */
    uint64_t last_ns;   /* last body-entry timestamp                     */
};

static Diag readDiag(unsigned nblocks)
{
    uint32_t hdiag[8] = {0};
    CHECK(drvMemcpyDtoH(hdiag, d_diag, sizeof(hdiag)));
    CHECK(drvMemcpyDtoH(h_out, d_out, 8 * (size_t)nblocks));
    CHECK(drvMemcpyDtoH(h_buf.data(), d_buf, bufBytes(nblocks)));

    Diag d;
    d.entered  = hdiag[0];
    d.timedout = hdiag[1];
    d.synced   = hdiag[2];
    d.blocked  = 0;
    d.restore  = 0;
    for (unsigned i = 0; i < nblocks; ++i) {
        uint32_t ex = 0, rs = 0;
        std::memcpy(&ex, h_buf.data() + 16 + 8 * i, 4);
        std::memcpy(&rs, h_buf.data() + 16 + 8 * i + 4, 4);
        if (ex) ++d.blocked;
        if (rs) ++d.restore;
    }
    d.markers = 0;
    for (unsigned i = 0; i < nblocks; ++i)
        if (h_out[i] != OUT_FILL) ++d.markers;

    /* entry timestamps */
    static uint64_t entry[MAX_BLOCKS];
    CHECK(drvMemcpyDtoH(entry, d_entry, 8 * (size_t)nblocks));
    d.first_ns = 0;
    d.last_ns = 0;
    for (unsigned i = 0; i < nblocks; ++i) {
        if (entry[i] == 0) continue;
        if (d.first_ns == 0 || entry[i] < d.first_ns) d.first_ns = entry[i];
        if (entry[i] > d.last_ns) d.last_ns = entry[i];
    }
    return d;
}

static void resetBuffers(unsigned nblocks)
{
    CHECK(drvMemsetD8(d_buf, 0, bufBytes(nblocks)));
    CHECK(drvMemsetD8(d_out, 0xCC, 8 * (size_t)nblocks));
    CHECK(drvMemsetD8(d_barrier, 0, 16));
    CHECK(drvMemsetD8(d_diag, 0, 32));
    CHECK(drvMemsetD8(d_entry, 0, 8 * (size_t)nblocks));
    CHECK(drvMemsetD8(d_setns, 0, 8));
    CHECK(drvStreamSync(streamA));
    /* The memsets run on the legacy stream; streamA/streamB are
     * NON-BLOCKING (no implicit ordering).  Record an event after the
     * resets and make BOTH streams wait on it -- otherwise the companion
     * kernel can read a STALE marker from the previous iteration. */
    CHECK(drvEventRecord(ev_reset, nullptr));
    CHECK(drvStreamWaitEvent(streamA, ev_reset, 0));
    CHECK(drvStreamWaitEvent(streamB, ev_reset, 0));
}

/* Craft "after-suspend" preempt-buffer state, then relaunch via resume.
 * Semantics mirror InstrumentManager::Reactivate()'s read-back state:
 *   header: global_exit_flag=0, preempt_idx
 *   block range [restore_begin, restore_begin+restore_cnt):
 *       exit_flag=1, restore_flag=1  (these blocks were BLOCKED at the
 *       guardian and must re-enter on resume)
 *   all other blocks: clean (they had entered the body; resume skips them) */
static void craftResumeState(unsigned nblocks, int64_t idx,
                             unsigned restore_begin, unsigned restore_cnt)
{
    std::vector<uint8_t> b(bufBytes(nblocks), 0);
    uint64_t preempt_idx = (uint64_t)idx;
    std::memcpy(&b[8], &preempt_idx, 8);
    for (unsigned i = restore_begin; i < restore_begin + restore_cnt; ++i) {
        uint32_t one = 1;
        std::memcpy(&b[16 + 8 * i], &one, 4);       /* exit_flag    */
        std::memcpy(&b[16 + 8 * i + 4], &one, 4);   /* restore_flag */
    }
    CHECK(drvMemcpyHtoD(d_buf, b.data(), b.size()));
    CHECK(drvStreamSync(streamA));
}

/* ------------------------- kernel launches -------------------------- */

/* Launch the instrumented coop kernel.
 *   type: 0 = guardian entry, 1 = resume entry  (mirrors LaunchType)
 *   uses the same 28-byte debugger params as InstrumentContext::Launch */
static CUresult coopLaunch(int type, int64_t idx, unsigned nblocks, int spin,
                           bool sync)
{
    char args_buf[28];
    uint64_t *preempt_buf = (uint64_t *)(args_buf +  0);
    uint64_t *guardian    = (uint64_t *)(args_buf +  8);
    int64_t  *kernel_idx  = (int64_t  *)(args_buf + 16);
    uint32_t *killable    = (uint32_t *)(args_buf + 24);

    *preempt_buf = d_buf;
    *guardian    = ep_guardian;   /* resume jumps back to the guardian */
    *kernel_idx  = idx;
    *killable    = 0;

    CUdeviceptr ep = (type == 1) ? ep_resume : ep_guardian;

    int spin_local = spin;
    uint64_t timeout_ns = DEF_TIMEOUT_NS;
    void *params[6];
    params[0] = &d_out;
    params[1] = &spin_local;
    params[2] = &d_barrier;
    params[3] = &d_diag;
    params[4] = &d_entry;
    params[5] = &timeout_ns;

    cuXtraSetDebuggerParams(fn_coop, args_buf, sizeof(args_buf));
    cuXtraSetEntryPoint(fn_coop, ep);

    CUresult ret;
    if (g_use_attr && drvLaunchKernelEx != nullptr) {
        CUlaunchAttribute attr;
        std::memset(&attr, 0, sizeof(attr));
        attr.id = CU_LAUNCH_ATTRIBUTE_COOPERATIVE;
        *(int *)attr.value = 1;
        CUlaunchConfig cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.gridDimX = nblocks;
        cfg.gridDimY = 1;
        cfg.gridDimZ = 1;
        cfg.blockDimX = g_threads;
        cfg.blockDimY = 1;
        cfg.blockDimZ = 1;
        cfg.sharedMemBytes = 0;
        cfg.hStream = streamA;
        cfg.attrs = &attr;
        cfg.numAttrs = 1;
        ret = drvLaunchKernelEx(&cfg, fn_coop, params, nullptr);
    } else {
        ret = drvLaunchKernel(fn_coop, nblocks, 1, 1, g_threads, 1, 1, 0,
                              streamA, params, nullptr);
    }

    cuXtraSetEntryPoint(fn_coop, ep_orig);
    if (sync) CHECK(drvStreamSync(streamA));
    return ret;
}

/* Companion kernel: waits for the first body-entry marker, dwells, sets flag */
static void companionLaunch(uint64_t dwell_ns)
{
    uint64_t wait_cap_ns = 2000000000ull; /* 2 s safety, keeps iterations short */
    void *params[5];
    params[0] = &d_entry;
    params[1] = &d_buf;   /* &global_exit_flag (word 0 of the preempt buffer) */
    params[2] = &dwell_ns;
    params[3] = &d_setns;
    params[4] = &wait_cap_ns;
    CHECK(drvLaunchKernel(fn_setflag, 1, 1, 1, 32, 1, 1, 0, streamB,
                          params, nullptr));
}

/* ---------------------- instrument (InstrumentContext) -------------- */

static void instrumentKernel()
{
    std::printf("[instrument] guardian splice (mirrors InstrumentContext::Instrument)\n");
    ep_orig = cuXtraGetEntryPoint(fn_coop);

    const void *kernel_image = nullptr;
    size_t kernel_size = 0;
    cuXtraGetBinary(ctx, fn_coop, &kernel_image, &kernel_size, false);
    REPORT("cuXtraGetBinary returned a kernel image",
           kernel_image != nullptr && kernel_size > 0);

    CUdeviceptr blk = cuXtraInstrMemBlockAlloc(ctx,
                        mve_guardian_instructions_size + kernel_size);
    REPORT("instruction memory block allocated", blk != 0);
    size_t n1 = cuXtraInstrMemcpyHtoD(blk, mve_guardian_instructions,
                                      mve_guardian_instructions_size, streamA);
    size_t n2 = cuXtraInstrMemcpyHtoD(blk + mve_guardian_instructions_size,
                                      kernel_image, kernel_size, streamA);
    REPORT("guardian + kernel image uploaded",
           n1 == mve_guardian_instructions_size && n2 == kernel_size);
    ep_guardian = blk;

    CUdeviceptr rblk = cuXtraInstrMemBlockAlloc(ctx, mve_resume_instructions_size);
    size_t n3 = cuXtraInstrMemcpyHtoD(rblk, mve_resume_instructions,
                                      mve_resume_instructions_size, streamA);
    REPORT("resume instructions uploaded", n3 == mve_resume_instructions_size);
    ep_resume = rblk;

    size_t regs = cuXtraGetLocalRegsPerThread(fn_coop);
    if (regs < 32) cuXtraSetLocalRegsPerThread(fn_coop, 32);
    size_t bars = cuXtraGetBarrierCnt(fn_coop);
    if (bars < 1) cuXtraSetBarrierCnt(fn_coop, 1);

    cuXtraInvalInstrCache(ctx);
    std::printf("  [info] entries: orig=0x%llx guardian=0x%llx resume=0x%llx\n",
                (unsigned long long)ep_orig, (unsigned long long)ep_guardian,
                (unsigned long long)ep_resume);
}

/* ------------------------------ S1 sanity --------------------------- */

static void sanityGuarded(unsigned nblocks, int spin)
{
    std::printf("[S1] guarded full run (flag=0, no interference, grid=%u)\n", nblocks);
    resetBuffers(nblocks);
    CHECK(coopLaunch(0, 1, nblocks, spin, true));
    Diag d = readDiag(nblocks);
    std::printf("  [info] entered=%u synced=%u timedout=%u blocked=%u markers=%u\n",
                d.entered, d.synced, d.timedout, d.blocked, d.markers);
    if (d.first_ns != 0 && d.last_ns >= d.first_ns) {
        g_spread_ns = d.last_ns - d.first_ns;
        std::printf("  [measure] entry spread over %u blocks: %.1f us\n",
                    nblocks, (double)g_spread_ns / 1000.0);
    }
    REPORT("all blocks entered the body", d.entered == nblocks);
    REPORT("barrier released the full grid (synced == N)", d.synced == nblocks);
    REPORT("no timeout (no missing peers in clean run)", d.timedout == 0);
    REPORT("all blocks completed", d.markers == nblocks);
    REPORT("no block was blocked at the guardian", d.blocked == 0);
}

/* ---------------------- E1: deterministic partial ------------------- */

static void expPartialResume(unsigned nblocks, int spin)
{
    const unsigned K = nblocks / 2;
    std::printf("[E1] partial grid via crafted resume state (N=%u, K=%u)\n", nblocks, K);

    /* --- E1a: the K blocked (later) blocks re-enter; the earlier ones
     *     had passed the checkpoint and are skipped -> missing peers --- */
    resetBuffers(nblocks);
    craftResumeState(nblocks, 3, nblocks - K, K);
    REPORT("resume-entry launch accepted", coopLaunch(1, 3, nblocks, spin, true) == CUDA_SUCCESS);
    Diag d = readDiag(nblocks);
    std::printf("  [info] entered=%u synced=%u timedout=%u blocked=%u markers=%u\n",
                d.entered, d.synced, d.timedout, d.blocked, d.markers);
    REPORT("exactly K blocks entered the body", d.entered == K);
    REPORT("all K entered blocks timed out on the missing N-K peers",
           d.timedout == K && d.synced == 0);
    bool skipped_clean = true;
    for (unsigned i = 0; i < nblocks - K; ++i)
        if (h_out[i] != OUT_FILL) { skipped_clean = false; break; }
    REPORT("the N-K skipped blocks (restore=0) never ran: out[i] untouched",
           skipped_clean);
    REPORT("kernel completed (bounded barrier prevented a GPU hang)",
           d.markers == K);

    /* --- E1b: control -- full re-entry passes the barrier --- */
    resetBuffers(nblocks);
    craftResumeState(nblocks, 3, 0, nblocks);
    REPORT("control launch accepted", coopLaunch(1, 3, nblocks, spin, true) == CUDA_SUCCESS);
    Diag c = readDiag(nblocks);
    std::printf("  [control] entered=%u synced=%u timedout=%u markers=%u\n",
                c.entered, c.synced, c.timedout, c.markers);
    REPORT("control: full grid entered", c.entered == nblocks);
    REPORT("control: barrier passed (no timeout)", c.synced == nblocks && c.timedout == 0);
    REPORT("control: all blocks completed", c.markers == nblocks);
}

/* ---------------------- E2: race amplifier (fuzz) ------------------- */

static void expRaceSweep(unsigned nblocks, int spin, unsigned max_hits,
                         unsigned max_iters, uint64_t sweep_max_ns, unsigned repeats)
{
    std::printf("[E2] race amplifier: flag set during the entry window\n");
    std::printf("  [info] N=%u, sweep 0..%llu ns x %u repeats (cap %u iters)\n",
                nblocks, (unsigned long long)sweep_max_ns, repeats, max_iters);

    unsigned hits = 0, all_entered = 0, all_blocked = 0, errors = 0, iters = 0;
    unsigned no_flag = 0;
    double t_start = nowMs();
    unsigned steps = 48;
    unsigned reps = repeats;
    if (reps < 1) reps = 1;
    if ((unsigned long long)steps * reps > max_iters) reps = max_iters / steps;
    if (reps < 1) reps = 1;

    for (unsigned r = 0; r < reps && iters < max_iters && hits < max_hits; ++r) {
        for (unsigned s = 0; s < steps && iters < max_iters && hits < max_hits; ++s) {
            uint64_t dwell = (uint64_t)((double)sweep_max_ns * s / steps);
            ++iters;
            resetBuffers(nblocks);

            /* verify the reset really zeroed the marker + flag (visibility probe) */
            {
                uint64_t probe = 1;
                CHECK(drvMemcpyDtoH(&probe, d_entry, 8));
                CHECK(drvMemcpyDtoH(h_buf.data(), d_buf, 4));
                if (probe != 0 || h_buf[0] != 0) {
                    std::printf("  [RESET-BUG] iter=%u marker=%llu flag=%u\n",
                                iters, (unsigned long long)probe, (unsigned)h_buf[0]);
                }
            }

            companionLaunch(dwell);              /* stream B: waits+dwells+sets */
            CUresult ret = coopLaunch(0, 7, nblocks, spin, false); /* stream A */
            CHECK(drvStreamSync(streamA));
            CHECK(drvStreamSync(streamB));

            if (ret != CUDA_SUCCESS) { ++errors; continue; }

            Diag d = readDiag(nblocks);
            uint32_t flag_seen = 0;
            std::memcpy(&flag_seen, h_buf.data(), 4); /* h_buf was just reloaded */
            if (flag_seen == 0) { ++no_flag; continue; } /* companion did not land */
            if (d.entered == 0) {
                ++all_blocked;                    /* flag landed before any entry */
                /* anomaly diagnostics */
                uint64_t setns = 0;
                CHECK(drvMemcpyDtoH(&setns, d_setns, 8));
                static uint64_t e2[MAX_BLOCKS];
                CHECK(drvMemcpyDtoH(e2, d_entry, 8 * (size_t)nblocks));
                unsigned entries_nz = 0;
                for (unsigned i = 0; i < nblocks; ++i) if (e2[i]) ++entries_nz;
                std::printf("  [ODD] iter=%u dwell=%.1f us entered=0 blocked=%u "
                            "restore=%u markers=%u entries_nz=%u setns=%llu "
                            "first_ns=%llu\n",
                            iters, (double)dwell / 1000.0, d.blocked, d.restore,
                            d.markers, entries_nz, (unsigned long long)setns,
                            (unsigned long long)d.first_ns);
                std::fflush(stdout);
            } else if (d.entered == nblocks) {
                ++all_entered;                    /* flag landed after all entries */
            } else {
                ++hits;
                uint64_t setns = 0;
                CHECK(drvMemcpyDtoH(&setns, d_setns, 8));
                uint64_t set_after_first =
                    (setns > d.first_ns && d.first_ns != 0) ? setns - d.first_ns : 0;
                std::printf("  [HIT] iter=%u dwell=%.1f us entered=%u blocked=%u "
                            "timedout=%u synced=%u markers=%u  flag_set=%.1f us "
                            "after first entry (spread=%.1f us)\n",
                            iters, (double)dwell / 1000.0, d.entered, d.blocked,
                            d.timedout, d.synced, d.markers,
                            (double)set_after_first / 1000.0,
                            (double)(d.last_ns - d.first_ns) / 1000.0);
                std::fflush(stdout);
            }
        }
    }

    std::printf("  [E2] outcomes: partial_hits=%u all_entered=%u all_blocked=%u "
                "no_flag=%u errors=%u (iters=%u, %.1f s)\n",
                hits, all_entered, all_blocked, no_flag, errors, iters,
                (nowMs() - t_start) / 1000.0);
    if (hits > 0) {
        std::printf("  [E2] HAZARD REPRODUCED: %u iteration(s) left a PARTIAL grid "
                    "waiting on peers that exited at the guardian checkpoint.\n", hits);
        std::printf("  [E2] With a real (unbounded) grid.sync() these iterations "
                    "would deadlock permanently; the bounded barrier reported them.\n");
    } else {
        std::printf("  [E2] no partial window hit in %u iterations (see spread "
                    "measurement in S1; the window exists but the landing time "
                    "missed it this run)\n", iters);
    }
    if (hits == 0) {
        std::printf("  [E2] (soft) window not hit this run; E1 already proves "
                    "the condition deterministically\n");
    }
}

/* -------------------------------- main ------------------------------ */

int main(int argc, char **argv)
{
    const char *cubin_path = nullptr;
    unsigned nblocks_override = 0;
    unsigned max_hits = 5, max_iters = 240, repeats = 6;
    int spin = 1 << 20;
    bool quick = false, run_s1 = true, run_e1 = true, run_e2 = true;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) quick = true;
        else if (std::strcmp(argv[i], "--no-attr") == 0) g_use_attr = false;
        else if (std::strcmp(argv[i], "--only-s1") == 0) { run_e1 = run_e2 = false; }
        else if (std::strcmp(argv[i], "--only-e1") == 0) { run_s1 = run_e2 = false; }
        else if (std::strcmp(argv[i], "--only-e2") == 0) { run_s1 = run_e1 = false; }
        else if (std::strcmp(argv[i], "--nblocks") == 0 && i + 1 < argc)
            nblocks_override = (unsigned)std::strtoul(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            g_threads = (unsigned)std::strtoul(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
            max_iters = (unsigned)std::strtoul(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--max-hits") == 0 && i + 1 < argc)
            max_hits = (unsigned)std::strtoul(argv[++i], nullptr, 0);
        else if (argv[i][0] != '-') cubin_path = argv[i];
    }
    if (cubin_path == nullptr) cubin_path = "coop_kernel.cubin";
    if (quick) { max_iters = 64; max_hits = 3; spin = 1 << 18; }

    std::printf("==================================================\n");
    std::printf("  Cooperative-groups x XSched Level-2 hazard demo (sm120)\n");
    std::printf("  cubin: %s\n", cubin_path);
    std::printf("  arrays: guardian %d instrs / resume %d instrs (from sm120.cpp)\n",
                (int)(mve_guardian_instructions_size / 16),
                (int)(mve_resume_instructions_size / 16));
    std::printf("==================================================\n");

    g_cuda = LoadLibraryA("C:\\Windows\\System32\\nvcuda.dll");
    if (g_cuda == nullptr) {
        std::printf("FATAL: cannot load nvcuda.dll\n");
        return 2;
    }
    loadDriverFns();

    CHECK(drvInit(0));
    CUdevice dev;
    CHECK(drvGet(&dev, 0));
    int ccMaj = 0, ccMin = 0, smCount = 0, coopLa = 0;
    CHECK(drvGetAttr(&ccMaj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev));
    CHECK(drvGetAttr(&ccMin, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev));
    CHECK(drvGetAttr(&smCount, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev));
    CHECK(drvGetAttr(&coopLa, CU_DEVICE_ATTRIBUTE_COOPERATIVE_LAUNCH, dev));
    char name[256] = {0};
    CHECK(drvGetName(name, sizeof(name), dev));
    std::printf("[env] %s, CC %d.%d, SMs=%d, cooperative-launch=%d, "
                "cuLaunchKernelEx=%s\n",
                name, ccMaj, ccMin, smCount, coopLa,
                drvLaunchKernelEx ? "yes" : "no");

    CHECK(drvCtxCreate(&ctx, 0, dev));
    CHECK(drvStreamCreate(&streamA, 1 /* CU_STREAM_NON_BLOCKING */));
    CHECK(drvStreamCreate(&streamB, 1 /* CU_STREAM_NON_BLOCKING */));
    CHECK(drvEventCreate(&ev_reset, 2 /* CU_EVENT_DISABLE_TIMING */));

    CHECK(drvMemAlloc(&d_out, 8 * (size_t)MAX_BLOCKS));
    CHECK(drvMemAlloc(&d_buf, bufBytes(MAX_BLOCKS)));
    CHECK(drvMemAlloc(&d_barrier, 16));
    CHECK(drvMemAlloc(&d_diag, 32));
    CHECK(drvMemAlloc(&d_entry, 8 * (size_t)MAX_BLOCKS));
    CHECK(drvMemAlloc(&d_setns, 8));

    CHECK(drvModuleLoad(&mod, cubin_path));
    CHECK(drvModuleGetFunction(&fn_coop, mod, "coop_kernel"));
    CHECK(drvModuleGetFunction(&fn_setflag, mod, "setflag_kernel"));
    REPORT("coop_kernel + setflag_kernel resolved", fn_coop && fn_setflag);

    instrumentKernel();

    /* grid sizing: stay within the co-resident limit (cooperative launch),
     * and leave headroom for the companion kernel in E2. */
    unsigned nblocks = 1024;
    if (drvOccupancy != nullptr && fn_coop != nullptr) {
        int per_sm = 0;
        if (drvOccupancy(&per_sm, fn_coop, (int)g_threads, 0) == CUDA_SUCCESS && per_sm > 0) {
            unsigned cap = (unsigned)per_sm * (unsigned)smCount;
            if (cap >= 2) nblocks = cap - cap / 5;    /* 80% headroom */
            std::printf("[info] occupancy: %d blocks/SM x %d SMs -> using %u blocks\n",
                        per_sm, smCount, nblocks);
        }
    }
    if (nblocks_override > 0) nblocks = nblocks_override;
    if (nblocks > MAX_BLOCKS) nblocks = MAX_BLOCKS;

    /* validate the cooperative attribute once, before the experiments */
    if (g_use_attr && drvLaunchKernelEx != nullptr) {
        resetBuffers(nblocks);
        CUresult r = coopLaunch(0, 1, nblocks, spin, true);
        if (r != CUDA_SUCCESS) {
            const char *s = "?";
            drvGetErrorName(r, &s);
            std::printf("[warn] cooperative-attribute launch failed (%s); "
                        "falling back to plain launch\n", s);
            g_use_attr = false;
        } else {
            std::printf("[info] cooperative-attribute launch accepted\n");
        }
    }

    if (run_s1) sanityGuarded(nblocks, spin);
    if (run_e1) expPartialResume(nblocks, spin);
    if (run_e2) {
        uint64_t sweep_max = g_spread_ns ? g_spread_ns * 3 : 3000; /* 3x spread */
        if (sweep_max < 2000) sweep_max = 2000; /* sub-us window: fine steps */
        expRaceSweep(nblocks, spin, max_hits, max_iters, sweep_max, repeats);
    }

    std::printf("==================================================\n");
    std::printf("  RESULT: %d failed check(s)\n", g_fail);
    std::printf("==================================================\n");
    return g_fail;
}
