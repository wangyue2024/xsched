/*
 * =====================================================================
 *  mve_main.cpp - T6 Minimum Viable Experiment (sm120 Level-2)
 *
 *  Validates, directly on sm120 hardware and with driver-API calls only
 *  (no shim / no preempt framework), the complete three-step loop the
 *  XSched Level-2 mechanism depends on:
 *
 *      guardian splice  ->  Deactivate (block)  ->  Reactivate (resume)
 *
 *  It mirrors InstrumentContext::Instrument / Launch and
 *  InstrumentManager::Deactivate / Reactivate from
 *  platforms/cuda/hal/src/level2/instrument.cpp one-to-one:
 *
 *    instrument : cuXtraGetEntryPoint / cuXtraGetBinary / InstrMemBlockAlloc
 *                 + [guardian][kernel] splice + regs>=32 + barrier>=1
 *                 + cuXtraInvalInstrCache
 *    launch     : 28-byte args (buf, guardian ep, kernel idx, killable)
 *                 -> cuXtraSetDebuggerParams -> cuXtraSetEntryPoint
 *                 -> cuLaunchKernel -> restore entry point
 *    deactivate : global_exit_flag = 1
 *    reactivate : read preempt_idx, clear 16-byte header, relaunch the
 *                 blocked kernel through the resume entry
 *
 *  The instruction arrays come from the reviewed sm120.cpp via
 *  gen_arrays.py (mve_arrays.h) -- never hand-copied.
 *
 *  Cases (see DESIGN.md for the full matrix and pass criteria):
 *    M1  normal path        (guarded, flag=0)            -> bit-exact
 *    M2  full block         (flag=1 before launch)       -> all blocked
 *    M3  resume after block                              -> bit-exact
 *    M4  partial in-flight  (flag set 10ms into flight)  -> 0<done<total
 *    M5  resume after partial                            -> bit-exact
 *    M6  stress loop        (A->B->C1, 1000 rounds)
 *    M7  three-branch K13   (K1 blocked, K2 later, K1..K2 resumed)
 *    M8  edge shapes        (1x1x1 / 2x2x2 / 1024x256t)
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
#include "mve_arrays.h"     /* generated from platforms/cuda/hal/src/arch/sm120.cpp */

/* ---------------- minimal dynamic CUDA driver API ---------------- */

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st  *CUcontext;
typedef struct CUmod_st  *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef struct CUstream_st *CUstream;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR 75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR 76

typedef CUresult (*pfn_cuInit)(unsigned);
typedef CUresult (*pfn_cuDriverGetVersion)(int *);
typedef CUresult (*pfn_cuDeviceGetCount)(int *);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice *, int);
typedef CUresult (*pfn_cuDeviceGetName)(char *, int, CUdevice);
typedef CUresult (*pfn_cuDeviceGetAttribute)(int *, int, CUdevice);
typedef CUresult (*pfn_cuCtxCreate)(CUcontext *, unsigned, CUdevice);
typedef CUresult (*pfn_cuModuleLoad)(CUmodule *, const char *);
typedef CUresult (*pfn_cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
typedef CUresult (*pfn_cuMemAlloc)(CUdeviceptr *, size_t);
typedef CUresult (*pfn_cuMemFree)(CUdeviceptr);
typedef CUresult (*pfn_cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
typedef CUresult (*pfn_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
typedef CUresult (*pfn_cuMemsetD8)(CUdeviceptr, unsigned char, size_t);
typedef CUresult (*pfn_cuMemsetD32)(CUdeviceptr, unsigned int, size_t);
typedef CUresult (*pfn_cuStreamCreate)(CUstream *, unsigned);
typedef CUresult (*pfn_cuStreamSynchronize)(CUstream);
typedef CUresult (*pfn_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                       unsigned, unsigned, unsigned, unsigned,
                                       CUstream, void **, void **);
typedef CUresult (*pfn_cuGetErrorName)(CUresult, const char **);

static pfn_cuInit               drvInit;
static pfn_cuDriverGetVersion   drvGetVersion;
static pfn_cuDeviceGetCount     drvGetCount;
static pfn_cuDeviceGet          drvGet;
static pfn_cuDeviceGetName      drvGetName;
static pfn_cuDeviceGetAttribute drvGetAttr;
static pfn_cuCtxCreate          drvCtxCreate;
static pfn_cuModuleLoad         drvModuleLoad;
static pfn_cuModuleGetFunction  drvModuleGetFunction;
static pfn_cuMemAlloc           drvMemAlloc;
static pfn_cuMemFree            drvMemFree;
static pfn_cuMemcpyHtoD         drvMemcpyHtoD;
static pfn_cuMemcpyDtoH         drvMemcpyDtoH;
static pfn_cuMemsetD8           drvMemsetD8;
static pfn_cuMemsetD32          drvMemsetD32;
static pfn_cuStreamCreate       drvStreamCreate;
static pfn_cuStreamSynchronize drvStreamSync;
static pfn_cuLaunchKernel       drvLaunchKernel;
static pfn_cuGetErrorName       drvGetErrorName;

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
    drvInit              = cudaSym<pfn_cuInit>("cuInit");
    drvGetVersion        = cudaSym<pfn_cuDriverGetVersion>("cuDriverGetVersion");
    drvGetCount          = cudaSym<pfn_cuDeviceGetCount>("cuDeviceGetCount");
    drvGet               = cudaSym<pfn_cuDeviceGet>("cuDeviceGet");
    drvGetName           = cudaSym<pfn_cuDeviceGetName>("cuDeviceGetName");
    drvGetAttr           = cudaSym<pfn_cuDeviceGetAttribute>("cuDeviceGetAttribute");
    drvCtxCreate         = cudaSym<pfn_cuCtxCreate>("cuCtxCreate_v2");
    drvModuleLoad        = cudaSym<pfn_cuModuleLoad>("cuModuleLoad");
    drvModuleGetFunction = cudaSym<pfn_cuModuleGetFunction>("cuModuleGetFunction");
    drvMemAlloc          = cudaSym<pfn_cuMemAlloc>("cuMemAlloc_v2");
    drvMemFree           = cudaSym<pfn_cuMemFree>("cuMemFree_v2");
    drvMemcpyHtoD        = cudaSym<pfn_cuMemcpyHtoD>("cuMemcpyHtoD_v2");
    drvMemcpyDtoH        = cudaSym<pfn_cuMemcpyDtoH>("cuMemcpyDtoH_v2");
    drvMemsetD8          = cudaSym<pfn_cuMemsetD8>("cuMemsetD8_v2");
    drvMemsetD32         = cudaSym<pfn_cuMemsetD32>("cuMemsetD32_v2");
    drvStreamCreate      = cudaSym<pfn_cuStreamCreate>("cuStreamCreate");
    drvStreamSync        = cudaSym<pfn_cuStreamSynchronize>("cuStreamSynchronize");
    drvLaunchKernel      = cudaSym<pfn_cuLaunchKernel>("cuLaunchKernel");
    drvGetErrorName      = cudaSym<pfn_cuGetErrorName>("cuGetErrorName");
}

/* ------------------------------ MVE state ------------------------------ */

#define MAX_BLOCKS        4096
#define OUT_FILL          0xCCCCCCCCCCCCCCCCull
#define DEF_BLOCKS        2048
#define DEF_SPIN_MS       50
#define PARTIAL_DELAY_MS  10

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

static CUcontext  ctx = nullptr;
static CUstream   stream = nullptr;
static CUmodule   mod = nullptr;
static CUfunction fn = nullptr;

static CUdeviceptr d_out = 0;    /* per-block completion markers */
static CUdeviceptr d_buf = 0;    /* preempt buffer (16 + 8*MAX_BLOCKS)   */

static CUdeviceptr ep_orig = 0;      /* original kernel entry            */
static CUdeviceptr ep_guardian = 0;  /* spliced guardian entry           */
static CUdeviceptr ep_resume = 0;    /* standalone resume entry          */

static size_t kernel_size = 0;
static const void *kernel_image = nullptr;

static uint64_t h_out[MAX_BLOCKS];
static std::vector<uint8_t> h_buf(16 + 8 * MAX_BLOCKS);
static std::vector<uint64_t> golden(DEF_BLOCKS);

static bool g_deep = true;         /* full 1000-round stress by default */

static double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/* ------------------------- launch configuration ------------------------ */

struct Cfg
{
    unsigned gx, gy, gz;    /* grid  */
    unsigned bx, by, bz;    /* block */
    int spin;
};

static Cfg cfgOf(unsigned gx, int spin)
{
    return Cfg{gx, 1, 1, 128, 1, 1, spin};
}

/* ------------------------------ helpers -------------------------------- */

static size_t bufBytes(unsigned nblocks) { return 16 + 8 * (size_t)nblocks; }

struct State
{
    uint32_t global_exit_flag;
    uint64_t preempt_idx;
    std::vector<uint32_t> exit_flags;
    std::vector<uint32_t> restore_flags;
};

static State readState(unsigned nblocks)
{
    CHECK(drvMemcpyDtoH(h_buf.data(), d_buf, bufBytes(nblocks)));
    State s;
    std::memcpy(&s.global_exit_flag, h_buf.data() + 0, 4);
    std::memcpy(&s.preempt_idx, h_buf.data() + 8, 8);
    s.exit_flags.resize(nblocks);
    s.restore_flags.resize(nblocks);
    for (unsigned i = 0; i < nblocks; ++i) {
        std::memcpy(&s.exit_flags[i], h_buf.data() + 16 + 8 * i, 4);
        std::memcpy(&s.restore_flags[i], h_buf.data() + 16 + 8 * i + 4, 4);
    }
    return s;
}

static void resetBuffers(unsigned nblocks)
{
    CHECK(drvMemsetD8(d_buf, 0, bufBytes(nblocks)));          /* header+flags */
    CHECK(drvMemsetD8(d_out, 0xCC, 8 * (size_t)nblocks));     /* out markers  */
    CHECK(drvStreamSync(stream));
}

static void clearHeader()
{
    /* exactly like InstrumentManager::Reactivate(): zero the first 16B */
    CHECK(drvMemsetD8(d_buf, 0, 16));
    CHECK(drvStreamSync(stream));
}

static int countCompleted(unsigned nblocks)
{
    CHECK(drvMemcpyDtoH(h_out, d_out, 8 * (size_t)nblocks));
    int c = 0;
    for (unsigned i = 0; i < nblocks; ++i)
        if (h_out[i] != OUT_FILL) ++c;
    return c;
}

static bool compareOut(const std::vector<uint64_t> &ref, unsigned nblocks,
                       int *mismatch_at)
{
    CHECK(drvMemcpyDtoH(h_out, d_out, 8 * (size_t)nblocks));
    for (unsigned i = 0; i < nblocks; ++i) {
        if (h_out[i] != ref[i]) {
            if (mismatch_at) *mismatch_at = (int)i;
            return false;
        }
    }
    return true;
}

static const char *launchTypeName(int t) { return t == 1 ? "resume" : "guardian"; }

/* Mirrors InstrumentContext::Launch(kKernelLaunchGuardian / Resume). */
static double mveLaunch(int type, int64_t idx, const Cfg &cfg, bool sync = true)
{
    char args_buf[28];
    uint64_t *preempt_buf = (uint64_t *)(args_buf +  0);
    uint64_t *guardian    = (uint64_t *)(args_buf +  8);
    int64_t  *kernel_idx  = (int64_t  *)(args_buf + 16);
    uint32_t *killable    = (uint32_t *)(args_buf + 24);

    *preempt_buf = d_buf;
    *guardian    = ep_guardian;
    *kernel_idx  = idx;
    *killable    = 0;

    CUdeviceptr ep = (type == 1) ? ep_resume : ep_guardian;

    cuXtraSetDebuggerParams(fn, args_buf, sizeof(args_buf));
    cuXtraSetEntryPoint(fn, ep);

    void *params[2];
    int spin_local = cfg.spin;
    params[0] = &d_out;
    params[1] = &spin_local;
    double t0 = nowMs();
    CHECK(drvLaunchKernel(fn, cfg.gx, cfg.gy, cfg.gz,
                          cfg.bx, cfg.by, cfg.bz, 0, stream, params, nullptr));
    if (sync) CHECK(drvStreamSync(stream));
    double dt = nowMs() - t0;

    cuXtraSetEntryPoint(fn, ep_orig);
    return dt;
}

/* ---------------------- instrument (InstrumentContext) ------------------ */

static void instrumentKernel()
{
    std::printf("[instrument] guardian splice (mirrors InstrumentContext::Instrument)\n");

    ep_orig = cuXtraGetEntryPoint(fn);
    std::printf("  [info] original entry: 0x%llx\n", (unsigned long long)ep_orig);

    cuXtraGetBinary(ctx, fn, &kernel_image, &kernel_size, false);
    REPORT("cuXtraGetBinary returned a kernel image",
           kernel_image != nullptr && kernel_size > 0);
    std::printf("  [info] kernel image: %llu bytes, first 16 bytes:",
                (unsigned long long)kernel_size);
    for (int i = 0; i < 16 && (size_t)i < kernel_size; ++i)
        std::printf(" %02x", ((const unsigned char *)kernel_image)[i]);
    std::printf("\n");

    /* [guardian][kernel body] -- guardian falls through into the kernel */
    CUdeviceptr blk = cuXtraInstrMemBlockAlloc(ctx,
                        mve_guardian_instructions_size + kernel_size);
    REPORT("instruction memory block allocated", blk != 0);
    size_t n1 = cuXtraInstrMemcpyHtoD(blk, mve_guardian_instructions,
                                      mve_guardian_instructions_size, stream);
    size_t n2 = cuXtraInstrMemcpyHtoD(blk + mve_guardian_instructions_size,
                                      kernel_image, kernel_size, stream);
    REPORT("guardian + kernel image uploaded",
           n1 == mve_guardian_instructions_size && n2 == kernel_size);
    ep_guardian = blk;
    std::printf("  [info] guardian entry: 0x%llx\n", (unsigned long long)ep_guardian);

    /* standalone resume entry (InstrumentContext constructor) */
    CUdeviceptr rblk = cuXtraInstrMemBlockAlloc(ctx, mve_resume_instructions_size);
    size_t n3 = cuXtraInstrMemcpyHtoD(rblk, mve_resume_instructions,
                                      mve_resume_instructions_size, stream);
    REPORT("resume instructions uploaded", n3 == mve_resume_instructions_size);
    ep_resume = rblk;
    std::printf("  [info] resume entry:   0x%llx\n", (unsigned long long)ep_resume);

    /* resource floors (mirrors Instrument()) */
    size_t regs = cuXtraGetLocalRegsPerThread(fn);
    if (regs < 32) cuXtraSetLocalRegsPerThread(fn, 32);
    size_t regs2 = cuXtraGetLocalRegsPerThread(fn);
    std::printf("  [info] regs/thread: %llu -> %llu\n",
                (unsigned long long)regs, (unsigned long long)regs2);
    REPORT("regs/thread floor >= 32", regs2 >= 32);

    size_t bars = cuXtraGetBarrierCnt(fn);
    if (bars < 1) cuXtraSetBarrierCnt(fn, 1);
    size_t bars2 = cuXtraGetBarrierCnt(fn);
    std::printf("  [info] barrier count: %llu -> %llu\n",
                (unsigned long long)bars, (unsigned long long)bars2);
    REPORT("barrier count floor >= 1", bars2 >= 1);

    cuXtraInvalInstrCache(ctx);
    std::printf("  [info] instruction cache invalidated\n");
}

/* ------------------------------- cases ---------------------------------- */

/* M1: guarded launch, no preemption -> must be bit-exact vs baseline. */
static void caseNormal(unsigned gx, int spin, unsigned nblocks, int64_t idx,
                       const std::vector<uint64_t> &ref)
{
    std::printf("[M1] normal path (guarded, flag=0, grid=%u, spin=%d)\n", gx, spin);
    resetBuffers(nblocks);
    double dt = mveLaunch(0, idx, cfgOf(gx, spin));
    int at = -1;
    REPORT("output bit-exact vs baseline", compareOut(ref, nblocks, &at));
    if (at >= 0) std::printf("  [info] first mismatch at block %d\n", at);
    State s = readState(nblocks);
    REPORT("preempt_idx == 0 (nothing preempted)", s.preempt_idx == 0);
    bool flags_clean = true;
    for (unsigned i = 0; i < nblocks; ++i)
        if (s.exit_flags[i] || s.restore_flags[i]) { flags_clean = false; break; }
    REPORT("all block flags clean (exit=0, restore=0)", flags_clean);
    std::printf("  [info] wall time: %.1f ms\n", dt);
    std::printf("  [info] global_exit_flag at end: %u\n", s.global_exit_flag);
}

/* M2: Deactivate then launch -> every block exits at the checkpoint. */
static void caseBlockAll(unsigned gx, int spin, unsigned nblocks, int64_t idx)
{
    std::printf("[M2] full block (flag=1 before launch, grid=%u)\n", gx);
    resetBuffers(nblocks);
    uint32_t one = 1;
    CHECK(drvMemcpyHtoD(d_buf, &one, 4));   /* global_exit_flag = 1 */
    CHECK(drvStreamSync(stream));
    double dt = mveLaunch(0, idx, cfgOf(gx, spin));
    int done = countCompleted(nblocks);
    REPORT("no block executed the body (out untouched)", done == 0);
    State s = readState(nblocks);
    REPORT("preempt_idx recorded == kernel idx", s.preempt_idx == (uint64_t)idx);
    int exit_set = 0, restore_set = 0;
    for (unsigned i = 0; i < nblocks; ++i) {
        if (s.exit_flags[i]) ++exit_set;
        if (s.restore_flags[i]) ++restore_set;
    }
    REPORT("every block marked exit_flag=1", exit_set == (int)nblocks);
    REPORT("every block marked restore_flag=1", restore_set == (int)nblocks);
    char note[128];
    std::snprintf(note, sizeof(note),
                  "blocked launch returned fast (%.1f ms)", dt);
    REPORT(note, dt < 500.0);
}

/* M3/M5: Reactivate -> resume entry -> bit-exact completion. */
static void caseResume(unsigned gx, int spin, unsigned nblocks, int64_t idx,
                       const std::vector<uint64_t> &ref, const char *tag)
{
    std::printf("%s resume (reactivate + relaunch, grid=%u)\n", tag, gx);
    clearHeader();                          /* Reactivate() */
    double dt = mveLaunch(1, idx, cfgOf(gx, spin));
    int at = -1;
    REPORT("output bit-exact vs baseline", compareOut(ref, nblocks, &at));
    if (at >= 0) std::printf("  [info] first mismatch at block %d\n", at);
    State s = readState(nblocks);
    REPORT("preempt_idx cleared", s.preempt_idx == 0);
    int restore_set = 0, exit_set = 0;
    for (unsigned i = 0; i < nblocks; ++i) {
        if (s.restore_flags[i]) ++restore_set;
        if (s.exit_flags[i]) ++exit_set;
    }
    REPORT("restore flags cleared by resume", restore_set == 0);
    REPORT("exit flags cleared (guarded body ran again)", exit_set == 0);
    std::printf("  [info] resume wall time: %.1f ms\n", dt);
}

/* M4/M5: partial in-flight -- flag set while first wave is running. */
static void casePartial(unsigned gx, int spin, unsigned nblocks, int64_t idx,
                        const std::vector<uint64_t> &ref)
{
    std::printf("[M4] partial in-flight (flag set %d ms into flight, grid=%u)\n",
                PARTIAL_DELAY_MS, gx);
    resetBuffers(nblocks);

    /* asynchronous guarded launch */
    mveLaunch(0, idx, cfgOf(gx, spin), /*sync=*/false);
    std::this_thread::sleep_for(std::chrono::milliseconds(PARTIAL_DELAY_MS));
    uint32_t one = 1;
    CHECK(drvMemcpyHtoD(d_buf, &one, 4));   /* mid-flight flag write (blocking API) */
    double t0 = nowMs();
    CHECK(drvStreamSync(stream));
    double total = nowMs() - t0 + PARTIAL_DELAY_MS;

    int done = countCompleted(nblocks);
    std::printf("  [info] completed blocks: %d / %u (%.1f ms total)\n",
                done, nblocks, total);
    REPORT("some blocks completed (first wave passed the checkpoint)",
           done > 0);
    REPORT("some blocks were blocked (flag took effect mid-flight)",
           done < (int)nblocks);

    State s = readState(nblocks);
    REPORT("preempt_idx recorded == kernel idx", s.preempt_idx == (uint64_t)idx);
    int restore_set = 0;
    for (unsigned i = 0; i < nblocks; ++i)
        if (s.restore_flags[i]) ++restore_set;
    char note[128];
    std::snprintf(note, sizeof(note),
                  "restore flags == blocked blocks (%d)", (int)nblocks - done);
    REPORT(note, restore_set == (int)nblocks - done);

    /* M5: resume completes exactly the missing blocks */
    std::printf("[M5] resume after partial\n");
    clearHeader();
    double dt = mveLaunch(1, idx, cfgOf(gx, spin));
    int at = -1;
    REPORT("output bit-exact vs baseline after resume",
           compareOut(ref, nblocks, &at));
    if (at >= 0) std::printf("  [info] first mismatch at block %d\n", at);
    State s2 = readState(nblocks);
    int restore_left = 0;
    for (unsigned i = 0; i < nblocks; ++i)
        if (s2.restore_flags[i]) ++restore_left;
    REPORT("restore flags cleared", restore_left == 0);
    std::printf("  [info] resume wall time: %.1f ms\n", dt);
}

/* M7: K13 three-branch semantics, verified in isolation by injecting
 * the branch-entry state directly (flag=1, chosen preempt_idx, clean
 * restore flags) and observing what a blocked launch writes back. */
static void forceState(uint64_t preempt_idx_val, unsigned nblocks)
{
    std::vector<uint8_t> b(bufBytes(nblocks), 0);
    uint32_t one = 1;
    std::memcpy(&b[0], &one, 4);            /* global_exit_flag = 1 */
    std::memcpy(&b[8], &preempt_idx_val, 8); /* preempt_idx        */
    CHECK(drvMemcpyHtoD(d_buf, b.data(), b.size()));
    CHECK(drvStreamSync(stream));
}

static void caseThreeBranch(unsigned gx, int spin, unsigned nblocks)
{
    std::printf("[M7] three-branch K13 semantics (isolated state injection)\n");
    const int64_t k1 = 101, k2 = 102;

    /* reference output for this shape */
    resetBuffers(nblocks);
    mveLaunch(0, k1, cfgOf(gx, spin));
    std::vector<uint64_t> ref(nblocks);
    CHECK(drvMemcpyDtoH(ref.data(), d_out, 8 * (size_t)nblocks));

    /* branch 1: preempt_idx==0 -> record idx AND restore flags */
    resetBuffers(nblocks);
    forceState(0, nblocks);
    mveLaunch(0, k1, cfgOf(gx, spin));
    State s1 = readState(nblocks);
    REPORT("branch1 (preempt_idx==0): records kernel idx",
           s1.preempt_idx == (uint64_t)k1);
    int r1 = 0; for (unsigned i = 0; i < nblocks; ++i) if (s1.restore_flags[i]) ++r1;
    REPORT("branch1: records restore flags (all set)", r1 == (int)nblocks);

    /* branch 2: preempt_idx==kernel_idx -> record restore flags only */
    forceState((uint64_t)k1, nblocks);
    mveLaunch(0, k1, cfgOf(gx, spin));
    State s2 = readState(nblocks);
    REPORT("branch2 (idx==preempt_idx): idx unchanged",
           s2.preempt_idx == (uint64_t)k1);
    int r2 = 0; for (unsigned i = 0; i < nblocks; ++i) if (s2.restore_flags[i]) ++r2;
    REPORT("branch2: records restore flags (all set)", r2 == (int)nblocks);

    /* branch 3: later kernel -> records nothing */
    forceState((uint64_t)k1, nblocks);
    mveLaunch(0, k2, cfgOf(gx, spin));
    State s3 = readState(nblocks);
    REPORT("branch3 (later idx): preempt_idx unchanged",
           s3.preempt_idx == (uint64_t)k1);
    int r3 = 0; for (unsigned i = 0; i < nblocks; ++i) if (s3.restore_flags[i]) ++r3;
    REPORT("branch3: does NOT record restore flags", r3 == 0);

    /* end-to-end: K1 blocked -> resume -> bit-exact */
    resetBuffers(nblocks);
    forceState(0, nblocks);
    mveLaunch(0, k1, cfgOf(gx, spin));
    clearHeader();
    mveLaunch(1, k1, cfgOf(gx, spin));
    int at = -1;
    REPORT("end-to-end: resumed K1 bit-exact after block",
           compareOut(ref, nblocks, &at));
}

/* M6: A -> B -> C1 stress loop on a small grid. */
static void stressLoop(int rounds, unsigned gx, int spin, unsigned nblocks)
{
    std::printf("[M6] stress loop x%d (A->B->C1, grid=%u, spin=%d)\n",
                rounds, gx, spin);
    std::vector<uint64_t> ref(nblocks);

    /* reference */
    resetBuffers(nblocks);
    mveLaunch(0, 1000, cfgOf(gx, spin));
    CHECK(drvMemcpyDtoH(ref.data(), d_out, 8 * (size_t)nblocks));

    int bad = 0;
    double t0 = nowMs();
    for (int round = 0; round < rounds; ++round) {
        const int64_t idx = 2000 + round;

        /* A: normal */
        resetBuffers(nblocks);
        mveLaunch(0, idx, cfgOf(gx, spin));
        int at = -1;
        if (!compareOut(ref, nblocks, &at)) { ++bad; break; }

        /* B: blocked */
        uint32_t one = 1;
        CHECK(drvMemcpyHtoD(d_buf, &one, 4));
        CHECK(drvMemsetD8(d_out, 0xCC, 8 * (size_t)nblocks)); /* fresh markers */
        mveLaunch(0, idx, cfgOf(gx, spin));
        State sb = readState(nblocks);
        if (sb.preempt_idx != (uint64_t)idx) { ++bad; break; }
        if (countCompleted(nblocks) != 0) { ++bad; break; }

        /* C1: resume */
        clearHeader();
        mveLaunch(1, idx, cfgOf(gx, spin));
        at = -1;
        if (!compareOut(ref, nblocks, &at)) { ++bad; break; }

        if ((round + 1) % 200 == 0)
            std::printf("  [info] %d/%d rounds ok (%.1f s elapsed)\n",
                        round + 1, rounds, (nowMs() - t0) / 1000.0);
    }
    double secs = (nowMs() - t0) / 1000.0;
    char note[160];
    std::snprintf(note, sizeof(note),
                  "%d rounds A->B->C1 converged (%.1f s)", rounds, secs);
    REPORT(note, bad == 0);
}

/* M8: edge shapes. */
static void caseShape(const char *name, Cfg cfg, unsigned nblocks)
{
    std::printf("[M8] shape %s (grid %ux%ux%u, block %ux%ux%u)\n", name,
                cfg.gx, cfg.gy, cfg.gz, cfg.bx, cfg.by, cfg.bz);
    std::vector<uint64_t> ref(nblocks);

    resetBuffers(nblocks);
    mveLaunch(0, 7, cfg);
    CHECK(drvMemcpyDtoH(ref.data(), d_out, 8 * (size_t)nblocks));
    int done = countCompleted(nblocks);
    REPORT("baseline-complete (all blocks ran)", done == (int)nblocks);

    /* blocked -> resume */
    uint32_t one = 1;
    CHECK(drvMemcpyHtoD(d_buf, &one, 4));
    CHECK(drvMemsetD8(d_out, 0xCC, 8 * (size_t)nblocks)); /* fresh markers */
    mveLaunch(0, 7, cfg);
    int done2 = countCompleted(nblocks);
    REPORT("full block works for this shape", done2 == 0);
    clearHeader();
    mveLaunch(1, 7, cfg);
    int at = -1;
    REPORT("resume bit-exact for this shape", compareOut(ref, nblocks, &at));
}

/* ------------------------------ calibration ----------------------------- */

static int calibrateSpin(int target_ms, unsigned gx)
{
    /* measure with a few candidates and interpolate (LCG is linear in iters) */
    int spin = 1 << 20;
    Cfg cfg = cfgOf(gx, spin);
    resetBuffers(gx);
    double t = mveLaunch(0, 1, cfg);
    if (t <= 0.01) t = 0.01;
    double per_block = t; /* grid gx blocks run concurrently; wall ~= block time */
    long long want = (long long)((double)spin * target_ms / per_block);
    if (want < (1 << 16)) want = 1 << 16;
    if (want > (1 << 30)) want = 1 << 30;
    std::printf("[calibrate] spin=%d -> %.2f ms wall (%u blocks); target %d ms -> spin=%lld\n",
                spin, t, gx, target_ms, want);
    return (int)want;
}

/* -------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    const char *cubin_path = nullptr;
    unsigned gx = DEF_BLOCKS;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) {
            g_deep = false;
        } else if (std::strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) {
            gx = (unsigned)std::strtoul(argv[++i], nullptr, 0);
        } else if (argv[i][0] != '-') {
            cubin_path = argv[i];
        } else {
            std::printf("FATAL: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }
    if (cubin_path == nullptr) cubin_path = "mve_kernel.cubin";
    if (gx > MAX_BLOCKS) gx = MAX_BLOCKS;

    std::printf("==================================================\n");
    std::printf("  T6 MVE - sm120 Level-2 guardian splice / block / resume\n");
    std::printf("  cubin: %s   blocks: %u   mode: %s\n",
                cubin_path, gx, g_deep ? "full" : "quick");
    std::printf("  arrays: guardian %d instrs / resume %d instrs (from sm120.cpp)\n",
                (int)(mve_guardian_instructions_size / 16),
                (int)(mve_resume_instructions_size / 16));
    std::printf("==================================================\n");

    g_cuda = LoadLibraryA("C:\\Windows\\System32\\nvcuda.dll");
    if (g_cuda == nullptr) {
        std::printf("FATAL: cannot load C:\\Windows\\System32\\nvcuda.dll\n");
        return 2;
    }
    loadDriverFns();

    /* ---- environment ---- */
    CHECK(drvInit(0));
    CUdevice dev;
    CHECK(drvGet(&dev, 0));
    int ccMaj = 0, ccMin = 0;
    CHECK(drvGetAttr(&ccMaj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev));
    CHECK(drvGetAttr(&ccMin, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev));
    char name[256] = {0};
    CHECK(drvGetName(name, sizeof(name), dev));
    std::printf("[env] %s, CC %d.%d\n", name, ccMaj, ccMin);
    REPORT("sm_120 device", ccMaj == 12 && ccMin == 0);

    CHECK(drvCtxCreate(&ctx, 0, dev));
    /* CU_STREAM_NON_BLOCKING: the host-side flag poke in M4 must run
     * CONCURRENTLY with the kernel (InstrumentContext uses the same
     * flag).  With the default (blocking) stream, a synchronous
     * cuMemcpyHtoD would implicitly wait for the running kernel. */
    CHECK(drvStreamCreate(&stream, 1 /* CU_STREAM_NON_BLOCKING */));
    CHECK(drvMemAlloc(&d_out, 8 * (size_t)MAX_BLOCKS));
    CHECK(drvMemAlloc(&d_buf, bufBytes(MAX_BLOCKS)));

    CHECK(drvModuleLoad(&mod, cubin_path));
    REPORT("mve_kernel.cubin loaded", mod != nullptr);
    CHECK(drvModuleGetFunction(&fn, mod, "mve_kernel"));
    REPORT("mve_kernel symbol resolved", fn != nullptr);

    /* ---- instrument ---- */
    instrumentKernel();

    /* ---- calibration ---- */
    int spin = calibrateSpin(DEF_SPIN_MS, 32);

    /* ---- baseline (golden) ---- */
    std::printf("[M0] baseline (no instrumentation, grid=%u)\n", gx);
    resetBuffers(gx);
    {
        void *params[2] = { &d_out, &spin };
        double t0 = nowMs();
        CHECK(drvLaunchKernel(fn, gx, 1, 1, 128, 1, 1, 0, stream, params, nullptr));
        CHECK(drvStreamSync(stream));
        std::printf("  [info] baseline wall time: %.1f ms\n", nowMs() - t0);
    }
    CHECK(drvMemcpyDtoH(h_out, d_out, 8 * (size_t)gx));
    golden.assign(h_out, h_out + gx);
    REPORT("baseline produced all completion markers",
           countCompleted(gx) == (int)gx);

    /* ---- M1..M5 on the main grid ---- */
    caseNormal(gx, spin, gx, 1, golden);
    caseBlockAll(gx, spin, gx, 2);
    caseResume(gx, spin, gx, 2, golden, "[M3]");
    casePartial(gx, spin, gx, 3, golden);

    /* ---- M7 three-branch (small grid) ---- */
    caseThreeBranch(64, spin, 64);

    /* ---- M8 edge shapes ---- */
    caseShape("1x1x1 / 1 thread", Cfg{1, 1, 1, 1, 1, 1, spin}, 1);
    {
        Cfg c = Cfg{2, 2, 2, 128, 1, 1, spin};
        caseShape("2x2x2 / 128 threads", c, 8);
    }
    {
        Cfg c = Cfg{1024, 1, 1, 256, 1, 1, spin};
        caseShape("1024 / 256 threads", c, 1024);
    }

    /* ---- M6 stress loop ---- */
    if (g_deep) {
        int spin_small = calibrateSpin(2, 32);
        stressLoop(1000, 64, spin_small, 64);
    } else {
        std::printf("[M6] stress loop skipped (--quick)\n");
    }

    std::printf("==================================================\n");
    std::printf("  RESULT: %d failed check(s)\n", g_fail);
    std::printf("==================================================\n");
    return g_fail;
}
