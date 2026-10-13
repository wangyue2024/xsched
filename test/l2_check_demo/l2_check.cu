/*
 * =====================================================================
 *  l2_check.cu - minimal Level-2 preemption verification demo
 *
 *  WHAT IT VERIFIES (the three observable invariants of Level-2):
 *    I1  "withdraw":  suspending mid-flight must leave the kernel
 *        PARTIALLY done (some blocks completed, some blocked at the
 *        guardian entry checkpoint) -> 0 < done < total
 *    I2  "frozen":    while suspended, no further block may complete
 *    I3  "correct":   after resume, results are bit-exact vs the
 *        baseline AND every block executed exactly once (no loss,
 *        no double execution)
 *
 *  HOW IT WORKS (full XSched stack, application-managed mode):
 *    - links the nvcuda shim import lib (output/lib/nvcuda.lib)
 *    - creates an explicit Level-2 XQueue on the stream (NON-BLOCKING,
 *      so the synchronous observation copies on the legacy stream do
 *      NOT implicitly wait for the kernel)
 *    - launches one 2048-block kernel (~hundreds of ms) through the shim
 *    - sleeps ~10 ms, XQueueSuspend(sync) -> count markers -> sleep ->
 *      count again -> XQueueResume -> synchronized -> verify
 *
 *  RUN: see run.ps1 (stages the shim nvcuda.dll next to the exe and
 *  sets XSCHED_CUDA_LIB / CUXTRA_CUDA_LIB / XSCHED_SCHEDULER=APP).
 *  Set XLOG_LEVEL=DEBG to watch the intercept/suspend/resume logs.
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

#include <cuda_runtime.h>

#include "xsched/xsched.h"
#include "xsched/cuda/hal.h"

/* synchronous / asynchronous (passthrough) driver calls for observation */
extern "C" {
typedef int cu_result;
typedef unsigned long long cu_devptr;
cu_result cuMemAlloc_v2(cu_devptr *dptr, size_t bytesize);
cu_result cuMemcpyDtoHAsync_v2(void *dstHost, cu_devptr srcDevice, size_t ByteCount, void *hStream);
cu_result cuMemsetD8_v2(cu_devptr dstDevice, unsigned char uc, size_t N);
}

#define GRID_BLOCKS   2048
#define NTHREADS      128
#define CHUNK_BLOCKS  256                          /* blocks per kernel */
#define N_CHUNKS      (GRID_BLOCKS / CHUNK_BLOCKS) /* 8 sequential kernels */
#define FILL_BYTE     0xCC
#define DONE_MAGIC    0xA5A5000000000000ull

static int g_fail = 0;

#define REPORT(name, ok)                                                  \
    do {                                                                  \
        std::printf("  [%s] %s\n", (ok) ? "PASS" : "FAIL", name);         \
        if (!(ok)) ++g_fail;                                              \
        std::fflush(stdout);                                              \
    } while (0)

#define RUNTIME_CHECK(call)                                               \
    do {                                                                  \
        cudaError_t _e = (call);                                          \
        if (_e != cudaSuccess) {                                          \
            std::printf("  [ERR ] %s -> %s\n", #call,                     \
                        cudaGetErrorString(_e));                          \
            ++g_fail;                                                     \
            return g_fail;                                                \
        }                                                                 \
    } while (0)

static double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/* observation buffers (declared EARLY: launch_workload() below uses them) */
static unsigned long long *g_done = nullptr;   /* GRID_BLOCKS * 8 bytes */
static unsigned int       *g_exe  = nullptr;   /* GRID_BLOCKS * 4 bytes */

/* ------------------------------------------------------------------ */
/*  device kernel: deterministic work + two observation arrays.        */
/*  The workload is launched as N_CHUNKS sequential kernels: the       */
/*  Deactivate flag's GPU-side landing time under WDDM is a lottery    */
/*  (us to hundreds of ms); with one giant kernel the "blockable tail" */
/*  is too narrow, while with several kernels any landing time still   */
/*  leaves un-started kernels that get blocked at the guardian.        */
/* ------------------------------------------------------------------ */
__global__ void l2_check_kernel(unsigned long long *done,   /* completion marker  */
                                unsigned int *exe_cnt,      /* body-exec counter  */
                                int seed, long long spin, int base)
{
    unsigned long long acc = 0x9E3779B97F4A7C15ull
                           ^ (unsigned long long)seed
                           ^ (unsigned long long)(base + blockIdx.x);
    for (long long i = 0; i < spin; ++i) {
        acc = acc * 6364136223846793005ull + 1442695040888963407ull;
        acc ^= (acc >> 29);
    }
    if ((threadIdx.x | threadIdx.y | threadIdx.z) == 0) {
        atomicAdd(&exe_cnt[base + blockIdx.x], 1u);   /* exactly-once detector */
        done[base + blockIdx.x] = DONE_MAGIC | (acc & 0xFFFFFFFFull);
    }
}

static void launch_workload(cudaStream_t s, int seed, long long spin)
{
    for (int k = 0; k < N_CHUNKS; ++k)
        l2_check_kernel<<<CHUNK_BLOCKS, NTHREADS, 0, s>>>(g_done, g_exe, seed, spin,
                                                          k * CHUNK_BLOCKS);
}

/* reset the observation arrays ON THE SAME STREAM (via the XQueue), so the
 * zeroing is strictly ordered BEFORE the next kernel -- host-side sync
 * memsets showed a rare WDDM submission race that could wipe counters of
 * blocks that had already run. */
__global__ void reset_buffers_kernel(unsigned long long *done, unsigned int *exe, int n)
{
    int stride = gridDim.x * blockDim.x;
    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        done[i] = 0xCCCCCCCCCCCCCCCCull;
        exe[i] = 0u;
    }
}

/* ------------------------------------------------------------------ */
/*  observation helpers.                                               */
/*  Snapshots use an ASYNC copy on a dedicated observation stream and  */
/*  synchronize only that stream -- a synchronous cuMemcpyDtoH on the  */
/*  legacy stream can (and on WDDM sometimes does) wait for the kernel */
/*  to finish, which would destroy the mid-flight sampling.            */
/* ------------------------------------------------------------------ */
static std::vector<unsigned long long> h_done(GRID_BLOCKS);
static std::vector<unsigned int> h_exe(GRID_BLOCKS);
static cudaStream_t s_obs = nullptr;           /* observation stream (no XQueue) */

static void fillBuffers()
{
    /* Reset on the PLAIN observation stream (no XQueue) and sync host-side:
     * the reset completes before the workload is even submitted, so ordering
     * is hard-guaranteed.  NOTE: resetting via a kernel INSIDE the XQueue
     * was measured to break the subsequent Deactivate (the extra instrumented
     * kernel's instruction-upload batch on the op-stream delays the exit-flag
     * memset until after the workload ends -- a WDDM batching artifact). */
    reset_buffers_kernel<<<16, 256, 0, s_obs>>>(g_done, g_exe, GRID_BLOCKS);
    cudaStreamSynchronize(s_obs);
}

static bool snapDone()
{
    if (cuMemcpyDtoHAsync_v2(h_done.data(), (cu_devptr)g_done, GRID_BLOCKS * 8,
                             (void *)s_obs) != 0) return false;
    return cudaStreamSynchronize(s_obs) == cudaSuccess;
}

static bool snapExe()
{
    if (cuMemcpyDtoHAsync_v2(h_exe.data(), (cu_devptr)g_exe, GRID_BLOCKS * 4,
                             (void *)s_obs) != 0) return false;
    return cudaStreamSynchronize(s_obs) == cudaSuccess;
}

static int countDone()
{
    if (!snapDone()) return -1;
    int c = 0;
    for (int i = 0; i < GRID_BLOCKS; ++i)
        if (h_done[i] != 0xCCCCCCCCCCCCCCCCull) ++c;
    return c;
}

/* ------------------------------------------------------------------ */
/*  calibration: pick `spin` so one block runs ~15 ms.                 */
/*  IMPORTANT: run it on a stream WITHOUT an XQueue (pure passthrough),*/
/*  otherwise the WDDM/queue round-trip overhead pollutes the timing.  */
/* ------------------------------------------------------------------ */
static long long calibrate(cudaStream_t s_cal)
{
    long long spin = 1LL << 24;
    for (int round = 0; round < 2; ++round) {
        double t0 = nowMs();
        l2_check_kernel<<<64, NTHREADS, 0, s_cal>>>(g_done, g_exe, 99, spin, 0);
        cudaError_t e = cudaStreamSynchronize(s_cal);
        double dt = nowMs() - t0;
        if (e != cudaSuccess || dt <= 0.1) dt = 100.0;
        long long next = (long long)((double)spin * 15.0 / dt);
        if (next < (1LL << 20)) next = 1LL << 20;
        if (next > (1LL << 26)) next = 1LL << 26;
        std::printf("[calibrate] round %d: spin=%lld -> %.1f ms; target 15 ms/block -> spin=%lld\n",
                    round, spin, dt, next);
        if (next == spin) break;
        spin = next;
    }
    return spin;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    std::printf("==================================================\n");
    std::printf("  L2 check: does Level-2 preemption work correctly?\n");
    std::printf("  grid=%d blocks x %d threads\n", GRID_BLOCKS, NTHREADS);
    std::printf("==================================================\n");

    cudaDeviceProp prop;
    if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) {
        std::printf("FATAL: no CUDA device\n");
        return 2;
    }
    std::printf("[env] %s (CC %d.%d)\n", prop.name, prop.major, prop.minor);

    cudaFree(0);  /* init runtime + primary context */
    if (cuMemAlloc_v2((cu_devptr *)&g_done, GRID_BLOCKS * 8) != 0 || g_done == nullptr ||
        cuMemAlloc_v2((cu_devptr *)&g_exe, GRID_BLOCKS * 4) != 0 || g_exe == nullptr) {
        std::printf("FATAL: observation buffer alloc failed\n");
        return 2;
    }

    /* ---- create a Level-2 XQueue on our stream ---- */
    /* NON-BLOCKING: without this, legacy-stream observation copies
     * (cuMemcpyDtoH) implicitly wait for the kernel to finish. */
    cudaStream_t stream = nullptr;
    RUNTIME_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));

    /* a plain calibration stream: NO XQueue -> pure passthrough timing */
    cudaStream_t s_cal = nullptr;
    RUNTIME_CHECK(cudaStreamCreateWithFlags(&s_cal, cudaStreamNonBlocking));

    /* observation stream: async snapshots that never wait for the kernel */
    RUNTIME_CHECK(cudaStreamCreateWithFlags(&s_obs, cudaStreamNonBlocking));

    HwQueueHandle hwq = 0;
    XQueueHandle  xq = 0;
    XResult r1 = CudaQueueCreate(&hwq, stream);
    XResult r2 = XQueueCreate(&xq, hwq, kPreemptLevelDeactivate, kQueueCreateFlagNone);
    REPORT("Level-2 XQueue created (kPreemptLevelDeactivate)", r1 == 0 && r2 == 0);
    XQueueSetLaunchConfig(xq, 8, 4);

    long long spin = calibrate(s_cal);

    /* ---- baseline: no preemption, record golden ---- */
    std::printf("[base] baseline run (no suspend)\n");
    fillBuffers();
    double t0 = nowMs();
    launch_workload(stream, 7, spin);
    RUNTIME_CHECK(cudaStreamSynchronize(stream));
    double t_base = nowMs() - t0;

    std::vector<unsigned long long> golden(GRID_BLOCKS);
    snapDone();  /* h_done now holds the baseline snapshot */
    std::memcpy(golden.data(), h_done.data(), GRID_BLOCKS * 8);
    int done_base = countDone();
    REPORT("baseline: all blocks completed", done_base == GRID_BLOCKS);
    bool magic_ok = true;
    for (int i = 0; i < GRID_BLOCKS; ++i)
        if (golden[i] == 0xCCCCCCCCCCCCCCCCull) { magic_ok = false; break; }
    REPORT("baseline: golden recorded", magic_ok);
    std::printf("  [info] baseline wall time: %.1f ms\n", t_base);

    /* ---- preemption run: suspend mid-flight ---- */
    std::printf("[L2] preemption run: %d kernels x %d blocks -> sleep 10 ms -> Suspend -> Resume\n",
                N_CHUNKS, CHUNK_BLOCKS);
    fillBuffers();
    t0 = nowMs();
    launch_workload(stream, 7, spin);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    int done_mid = countDone();
    std::printf("  [info] t=%.1f ms (mid-flight): %d/%d blocks done\n",
                nowMs() - t0, done_mid, GRID_BLOCKS);

    double ts = nowMs();
    XResult rs = XQueueSuspend(xq, kQueueSuspendFlagSyncHwQueue);
    double suspend_ms = nowMs() - ts;
    int done_susp = countDone();
    std::printf("  [info] Suspend(sync) took %.1f ms; %d/%d blocks done\n",
                suspend_ms, done_susp, GRID_BLOCKS);
    REPORT("I1 withdraw: suspend left the kernel partially done (0 < done < N)",
           rs == 0 && done_susp > 0 && done_susp < GRID_BLOCKS);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int done_frozen = countDone();
    std::printf("  [info] +20 ms while suspended: %d/%d blocks done\n",
                done_frozen, GRID_BLOCKS);
    REPORT("I2 frozen: no block completed while suspended",
           done_frozen == done_susp);

    ts = nowMs();
    XResult rr = XQueueResume(xq, kQueueResumeFlagNone);
    RUNTIME_CHECK(cudaStreamSynchronize(stream));
    double resume_ms = nowMs() - ts;
    int done_all = countDone();
    std::printf("  [info] Resume -> sync took %.1f ms; total run %.1f ms\n",
                resume_ms, nowMs() - t0);
    REPORT("resume: all blocks completed", rr == 0 && done_all == GRID_BLOCKS);

    snapDone();  /* h_done holds the final snapshot */
    bool bitexact = true;
    for (int i = 0; i < GRID_BLOCKS; ++i)
        if (h_done[i] != golden[i]) { bitexact = false; break; }
    REPORT("I3a correct: results bit-exact vs baseline", bitexact);

    snapExe();
    int dup = 0, miss = 0;
    for (int i = 0; i < GRID_BLOCKS; ++i) {
        if (h_exe[i] == 0) ++miss;
        else if (h_exe[i] > 1) ++dup;
    }
    std::printf("  [info] per-block body executions: miss=%d duplicate=%d\n", miss, dup);
    REPORT("I3b exactly-once: every block executed exactly once",
           miss == 0 && dup == 0);

    std::printf("==================================================\n");
    std::printf("  RESULT: %d failed check(s)\n", g_fail);
    std::printf("==================================================\n");
    return g_fail;
}
