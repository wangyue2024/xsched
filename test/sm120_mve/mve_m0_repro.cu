// =====================================================================
//  mve_m0_repro.cu - bisect the M0 "missing markers" phenomenon
//
//  Re-runs the M0 scenario (2048-block ~50 ms kernel, driver API) in
//  phases, each adding one piece of the MVE environment:
//
//    phase P0 : cuModuleLoad(mve_kernel.cubin) + cuLaunchKernel loop
//               (plain driver path, no cuxtra state changes)
//    phase P1 : + cuXtraSetLocalRegsPerThread(32) / SetBarrierCnt(1)
//    phase P2 : + cuXtraInvalInstrCache
//
//  10 rounds each of [memset 0xCC -> launch -> sync -> count != 0xCC].
//  A phase that starts losing markers pinpoints the culprit.
//
//    phase P3 : + full instrument history (guardian splice + resume block
//               + debugger params + entry redirect once, then restore)
//    phase P4 : + 8 guarded launches (mveLaunch-style), then plain again
//
//  Build: see build section in the header of run loop (see README).
// =====================================================================

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>

#include "cuxtra/cuxtra.h"
#include "mve_arrays.h"   /* mve_guardian_instructions / mve_resume_instructions */

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st  *CUcontext;
typedef struct CUmod_st  *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef struct CUstream_st *CUstream;
typedef unsigned long long CUdeviceptr;

typedef CUresult (*pfn_cuInit)(unsigned);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice *, int);
typedef CUresult (*pfn_cuCtxCreate)(CUcontext *, unsigned, CUdevice);
typedef CUresult (*pfn_cuStreamCreate)(CUstream *, unsigned);
typedef CUresult (*pfn_cuStreamSynchronize)(CUstream);
typedef CUresult (*pfn_cuModuleLoad)(CUmodule *, const char *);
typedef CUresult (*pfn_cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
typedef CUresult (*pfn_cuMemAlloc)(CUdeviceptr *, size_t);
typedef CUresult (*pfn_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
typedef CUresult (*pfn_cuMemsetD8)(CUdeviceptr, unsigned char, size_t);
typedef CUresult (*pfn_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                       unsigned, unsigned, unsigned, unsigned,
                                       CUstream, void **, void **);

static HMODULE g_cuda;
template <typename T> static T sym(const char *n)
{
    void *p = (void *)GetProcAddress(g_cuda, n);
    if (!p) { std::printf("FATAL: %s missing\n", n); std::exit(2); }
    return (T)p;
}

#define BLOCKS 2048
#define FILL   0xCC

static double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static int runRound(CUfunction fn, CUstream stream, CUdeviceptr d_out,
                    long long spin, unsigned blocks, bool (*inv)(CUcontext), CUcontext ctx)
{
    auto cuMemsetD8  = sym<pfn_cuMemsetD8>("cuMemsetD8_v2");
    auto cuMemcpyDtoH= sym<pfn_cuMemcpyDtoH>("cuMemcpyDtoH_v2");
    auto cuLaunch    = sym<pfn_cuLaunchKernel>("cuLaunchKernel");
    auto cuSync      = sym<pfn_cuStreamSynchronize>("cuStreamSynchronize");

    cuMemsetD8(d_out, FILL, blocks * 8);
    void *params[2] = { &d_out, &spin };
    cuLaunch(fn, blocks, 1, 1, 128, 1, 1, 0, stream, params, nullptr);
    cuSync(stream);
    if (inv) inv(ctx);
    static std::vector<unsigned long long> h(BLOCKS);
    cuMemcpyDtoH(h.data(), d_out, blocks * 8);
    int done = 0;
    for (unsigned i = 0; i < blocks; ++i)
        if (h[i] != 0xCCCCCCCCCCCCCCCCull) ++done;
    return done;
}

int main(int argc, char **argv)
{
    const char *cubin = (argc > 1) ? argv[1] : "mve_kernel.cubin";
    int rounds = (argc > 2) ? atoi(argv[2]) : 10;

    g_cuda = LoadLibraryA("C:\\Windows\\System32\\nvcuda.dll");
    if (!g_cuda) { std::printf("FATAL: nvcuda\n"); return 2; }

    auto cuInit     = sym<pfn_cuInit>("cuInit");
    auto cuDeviceGet= sym<pfn_cuDeviceGet>("cuDeviceGet");
    auto cuCtxCreate= sym<pfn_cuCtxCreate>("cuCtxCreate_v2");
    auto cuStreamCreate = sym<pfn_cuStreamCreate>("cuStreamCreate");
    auto cuModuleLoad   = sym<pfn_cuModuleLoad>("cuModuleLoad");
    auto cuModuleGetFunction = sym<pfn_cuModuleGetFunction>("cuModuleGetFunction");
    auto cuMemAlloc = sym<pfn_cuMemAlloc>("cuMemAlloc_v2");

    cuInit(0);
    CUdevice dev; cuDeviceGet(&dev, 0);
    CUcontext ctx; cuCtxCreate(&ctx, 0, dev);
    CUstream stream; cuStreamCreate(&stream, 1 /* NON_BLOCKING */);
    CUdeviceptr d_out = 0; cuMemAlloc(&d_out, 4096 * 8); /* 32KB like mve_main */

    CUmodule mod; CUfunction fn;
    if (cuModuleLoad(&mod, cubin) != 0) { std::printf("FATAL: load cubin\n"); return 2; }
    if (cuModuleGetFunction(&fn, mod, "mve_kernel") != 0) { std::printf("FATAL: symbol\n"); return 2; }

    /* calibrate ~50 ms per block with a single wave (32 blocks) -- exactly
     * like the MVE calibrateSpin (the previous 2048-block calibration both
     * included wave-stacking and was polluted by foreign GPU load) */
    long long spin = 1 << 20;
    runRound(fn, stream, d_out, spin, 32, nullptr, ctx);
    double t0 = nowMs();
    runRound(fn, stream, d_out, spin, 32, nullptr, ctx);
    double ms = nowMs() - t0;
    if (ms < 1) ms = 1;
    spin = (long long)((double)spin * 50.0 / ms);
    std::printf("[repro] spin=%lld (32-block calib %.1f ms -> ~50 ms/block)\n", spin, ms);

    /* ---- P0: plain driver path ---- */
    std::printf("[P0] plain driver (no cuxtra state changes)\n");
    for (int r = 0; r < rounds; ++r) {
        double t = nowMs();
        int done = runRound(fn, stream, d_out, spin, BLOCKS, nullptr, ctx);
        std::printf("  round %2d: done=%4d/%d wall=%8.1f ms %s\n",
                    r, done, BLOCKS, nowMs() - t, done == BLOCKS ? "OK" : "*** MISSING ***");
        std::fflush(stdout);
    }

    /* ---- P1: + regs/barrier floors (exactly like Instrument()) ---- */
    std::printf("[P1] + SetLocalRegs(32) / SetBarrierCnt(1)\n");
    cuXtraSetLocalRegsPerThread(fn, 32);
    cuXtraSetBarrierCnt(fn, 1);
    for (int r = 0; r < rounds; ++r) {
        double t = nowMs();
        int done = runRound(fn, stream, d_out, spin, BLOCKS, nullptr, ctx);
        std::printf("  round %2d: done=%4d/%d wall=%8.1f ms %s\n",
                    r, done, BLOCKS, nowMs() - t, done == BLOCKS ? "OK" : "*** MISSING ***");
        std::fflush(stdout);
    }

    /* ---- P2: + InvalInstrCache after every launch ---- */
    std::printf("[P2] + cuXtraInvalInstrCache after each launch\n");
    for (int r = 0; r < rounds; ++r) {
        double t = nowMs();
        int done = runRound(fn, stream, d_out, spin, BLOCKS, nullptr, ctx);
        cuXtraInvalInstrCache(ctx);
        std::printf("  round %2d: done=%4d/%d wall=%8.1f ms %s\n",
                    r, done, BLOCKS, nowMs() - t, done == BLOCKS ? "OK" : "*** MISSING ***");
        std::fflush(stdout);
    }

    /* ---- P3: full instrument history, then plain launches ---- */
    std::printf("[P3] instrument history (guardian splice + params + entry redirect)\n");
    auto cuSyncFn = sym<pfn_cuStreamSynchronize>("cuStreamSynchronize");
    auto cuMemset8 = sym<pfn_cuMemsetD8>("cuMemsetD8_v2");
    CUdeviceptr d_buf = 0; cuMemAlloc(&d_buf, 16 + 8 * BLOCKS);
    cuMemset8(d_buf, 0, 16 + 8 * BLOCKS);
    const void *kimg = nullptr; size_t ksize = 0;
    cuXtraGetBinary(ctx, fn, &kimg, &ksize, false);
    std::printf("  [info] kernel image %zu bytes\n", ksize);
    CUdeviceptr gblk = cuXtraInstrMemBlockAlloc(ctx, mve_guardian_instructions_size + ksize);
    cuXtraInstrMemcpyHtoD(gblk, mve_guardian_instructions, mve_guardian_instructions_size, stream);
    cuXtraInstrMemcpyHtoD(gblk + mve_guardian_instructions_size, kimg, ksize, stream);
    CUdeviceptr rblk = cuXtraInstrMemBlockAlloc(ctx, mve_resume_instructions_size);
    cuXtraInstrMemcpyHtoD(rblk, mve_resume_instructions, mve_resume_instructions_size, stream);
    cuXtraInvalInstrCache(ctx);
    CUdeviceptr ep_orig = cuXtraGetEntryPoint(fn);

    /* one guarded launch with debugger params (like MVE calibrate) */
    char args[28];
    std::memset(args, 0, sizeof(args));
    *(uint64_t *)(args + 0) = d_buf;
    *(uint64_t *)(args + 8) = gblk;
    *(int64_t  *)(args + 16) = 99;
    cuXtraSetDebuggerParams(fn, args, sizeof(args));
    cuXtraSetEntryPoint(fn, gblk);
    {void *params[2] = { &d_out, &spin };
     sym<pfn_cuLaunchKernel>("cuLaunchKernel")(fn, 32, 1, 1, 128, 1, 1, 0, stream, params, nullptr);}
    cuSyncFn(stream);
    cuXtraSetEntryPoint(fn, ep_orig);
    std::printf("  [info] guarded launch done, entry restored\n");

    for (int r = 0; r < rounds; ++r) {
        double t = nowMs();
        int done = runRound(fn, stream, d_out, spin, BLOCKS, nullptr, ctx);
        std::printf("  round %2d: done=%4d/%d wall=%8.1f ms %s\n",
                    r, done, BLOCKS, nowMs() - t, done == BLOCKS ? "OK" : "*** MISSING ***");
        std::fflush(stdout);
    }

    /* ---- P4: several guarded launches (mveLaunch-style), then plain ---- */
    std::printf("[P4] 8 guarded launches then plain\n");
    for (int r = 0; r < 8; ++r) {
        cuXtraSetDebuggerParams(fn, args, sizeof(args));
        cuXtraSetEntryPoint(fn, gblk);
        {void *params[2] = { &d_out, &spin };
         sym<pfn_cuLaunchKernel>("cuLaunchKernel")(fn, BLOCKS, 1, 1, 128, 1, 1, 0, stream, params, nullptr);}
        cuXtraSetEntryPoint(fn, ep_orig);      /* restore immediately (async) */
        cuSyncFn(stream);
    }
    for (int r = 0; r < rounds; ++r) {
        double t = nowMs();
        int done = runRound(fn, stream, d_out, spin, BLOCKS, nullptr, ctx);
        std::printf("  round %2d: done=%4d/%d wall=%8.1f ms %s\n",
                    r, done, BLOCKS, nowMs() - t, done == BLOCKS ? "OK" : "*** MISSING ***");
        std::fflush(stdout);
    }
    return 0;
}
