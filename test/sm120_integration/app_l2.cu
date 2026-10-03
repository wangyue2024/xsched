// =====================================================================
//  app_l2.cu - T7 integration test: sm120 Level-2 through the FULL
//              XSched stack (DLL-proxy interception on Windows)
//
//  Run layout (see run.ps1):
//    work/  app_l2.exe + nvcuda.dll (shim from output/bin)
//    env:   XSCHED_CUDA_LIB / CUXTRA_CUDA_LIB -> System32\ nvcuda.dll
//           XSCHED_SCHEDULER=APP  (application-managed scheduling)
//
//  The app links output/lib/nvcuda.lib, so BOTH the CUDA runtime calls
//  (cudart -> nvcuda.dll -> shim) AND the XSched management API calls
//  (CudaQueueCreate / XQueue*) land in the same shim instance -- one
//  consistent XSched state, exactly like a real user application.
//
//  Observation channel: the SYNCHRONOUS driver copies (cuMemcpyDtoH_v2
//  / cuMemsetD8_v2 etc.) are passed through to the real driver by the
//  shim (intercept.cpp), so they never enter a queue and never block on
//  a suspended XQueue -- the test can inspect device buffers at ANY time.
//
//  Cases (all must PASS):
//    I1  functional   : level-2 queue, 32 kernels, no preemption -> correct
//    I2  block/resume : long stream, suspend mid-flight -> freeze verified,
//                       resume -> every command exactly once, correct
//    I3  cycle        : 20x (submit / suspend / resume) rounds
//    I4  mixed        : kernel + memcpyAsync + memsetAsync across suspend
//    I5  unmanaged    : plain stream (no XQueue) works alongside
//    I6  multi-queue  : two level-2 queues; suspend one, the other keeps
//                       running; both correct after resume
//
//  Exit code = number of failed checks (0 = all pass).
// =====================================================================

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

// ---------------------------------------------------------------------
//  minimal driver declarations for the synchronous (passthrough) APIs
//  we use for observation; x64 C ABI, resolved against nvcuda.lib
// ---------------------------------------------------------------------
extern "C" {
typedef int cu_result;
typedef unsigned long long cu_devptr;
cu_result cuMemAlloc_v2(cu_devptr *dptr, size_t bytesize);
cu_result cuMemcpyHtoD_v2(cu_devptr dstDevice, const void *srcHost, size_t ByteCount);
cu_result cuMemcpyDtoH_v2(void *dstHost, cu_devptr srcDevice, size_t ByteCount);
cu_result cuMemsetD8_v2(cu_devptr dstDevice, unsigned char uc, size_t N);
}

#define MAGIC_BASE   0xA250000000000000ull
#define FILL_BYTE    0xCC
#define OUT_SLOTS    128
#define BLOCKS       16
#define THREADS      128

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
            return;                                                       \
        }                                                                 \
    } while (0)

static double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------
//  device kernels
// ---------------------------------------------------------------------
__global__ void l2_probe_kernel(unsigned long long *out, unsigned long long *sink,
                               int idx, long long spin)
{
    unsigned long long acc = 0x9E3779B97F4A7C15ull
                           ^ (unsigned long long)idx
                           ^ (unsigned long long)blockIdx.x;
    for (long long i = 0; i < spin; ++i) {
        acc = acc * 6364136223846793005ull + 1442695040888963407ull;
        acc ^= (acc >> 29);
    }
    if ((threadIdx.x | threadIdx.y | threadIdx.z) == 0 && blockIdx.x == 0) {
        out[idx] = MAGIC_BASE + (unsigned long long)idx;
        if (sink != nullptr) sink[idx] = acc;  /* make the busy chain
                                                  observable (non-removable) */
    }
}

/* reads src[0]; writes the magic iff src[0] carries the expected payload */
__global__ void l2_read_kernel(const unsigned long long *src,
                               unsigned long long *out, int idx)
{
    if ((threadIdx.x | threadIdx.y | threadIdx.z) == 0 && blockIdx.x == 0) {
        out[idx] = (src[0] == 0x5A5A5A5A5A5A5A5Aull)
                 ? (MAGIC_BASE + (unsigned long long)idx)
                 : 0xDEADDEADDEADDEADull;
    }
}

// ---------------------------------------------------------------------
//  observation helpers (synchronous passthrough driver calls)
// ---------------------------------------------------------------------
static unsigned long long *g_dout = nullptr;
static unsigned long long *g_sink = nullptr;
static std::vector<unsigned long long> h_snap;

static bool fillOut(unsigned char v)
{
    return cuMemsetD8_v2((cu_devptr)g_dout, v, OUT_SLOTS * 8) == 0;
}

static int countCompleted()
{
    if (cuMemcpyDtoH_v2(h_snap.data(), (cu_devptr)g_dout, OUT_SLOTS * 8) != 0) return -1;
    int c = 0;
    for (int i = 0; i < OUT_SLOTS; ++i)
        if (h_snap[i] != 0xCCCCCCCCCCCCCCCCull) ++c;
    return c;
}

static bool verifyOut(int n)
{
    if (cuMemcpyDtoH_v2(h_snap.data(), (cu_devptr)g_dout, OUT_SLOTS * 8) != 0) return false;
    for (int i = 0; i < n; ++i)
        if (h_snap[i] != MAGIC_BASE + (unsigned long long)i) return false;
    return true;
}

// ---------------------------------------------------------------------
//  calibration: spin value making one kernel take ~5 ms
//  (wall-clock based: CUDA events are queued through the XQueue and do
//   not give clean GPU timestamps in the application-managed mode)
// ---------------------------------------------------------------------
static long long g_spin5 = 1 << 20;

static void calibrate(cudaStream_t stream)
{
    long long spin = 1 << 20;
    l2_probe_kernel<<<BLOCKS, THREADS, 0, stream>>>(g_dout, g_sink, 127, spin);  // warmup
    cudaStreamSynchronize(stream);

    double t0 = nowMs();
    l2_probe_kernel<<<BLOCKS, THREADS, 0, stream>>>(g_dout, g_sink, 127, spin);
    cudaStreamSynchronize(stream);
    double ms = nowMs() - t0;
    if (ms < 0.5) ms = 0.5;
    double scaled = (double)spin * 5.0 / ms;
    if (scaled < (1 << 16)) scaled = 1 << 16;
    if (scaled > 4e9) scaled = 4e9;
    g_spin5 = (long long)scaled;
    std::printf("[calibrate] spin=%lld -> %.2f ms/kernel (wall incl. ~launch); "
                "target 5 ms -> spin=%lld\n", spin, ms, g_spin5);
    std::fflush(stdout);

    /* sanity: verify the calibrated kernel really takes a while */
    t0 = nowMs();
    l2_probe_kernel<<<BLOCKS, THREADS, 0, stream>>>(g_dout, g_sink, 127, g_spin5);
    cudaStreamSynchronize(stream);
    std::printf("[calibrate] verify: calibrated kernel wall = %.2f ms\n",
                nowMs() - t0);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------
//  I1: functional correctness on a level-2 managed queue
// ---------------------------------------------------------------------
static void caseI1(cudaStream_t stream, int n)
{
    std::printf("[I1] functional: %d kernels through the level-2 queue\n", n);
    fillOut(FILL_BYTE);
    for (int i = 0; i < n; ++i)
        l2_probe_kernel<<<BLOCKS, THREADS, 0, stream>>>(g_dout, g_sink, i, g_spin5 / 4);
    cudaError_t e = cudaStreamSynchronize(stream);
    REPORT("stream synchronized without error", e == cudaSuccess);
    REPORT("all 32 kernels completed", countCompleted() == n);
    REPORT("all outputs correct (bit-exact)", verifyOut(n));
}

// ---------------------------------------------------------------------
//  I2: suspend mid-flight -> freeze -> resume -> all correct
// ---------------------------------------------------------------------
static void caseI2(cudaStream_t stream, XQueueHandle xq, int n_pre, int n_mid)
{
    std::printf("[I2] block/resume: %d pre + %d mid-flight kernels\n", n_pre, n_mid);
    fillOut(FILL_BYTE);

    for (int i = 0; i < n_pre; ++i)
        l2_probe_kernel<<<BLOCKS, THREADS, 0, stream>>>(g_dout, g_sink, i, g_spin5);
    std::this_thread::sleep_for(std::chrono::milliseconds(18));

    double t0 = nowMs();
    XQueueSuspend(xq, kQueueSuspendFlagSyncHwQueue);
    double suspend_ms = nowMs() - t0;
    int c1 = countCompleted();
    std::printf("  [info] after suspend: %d/%d completed (suspend took %.1f ms)\n",
                c1, n_pre, suspend_ms);
    REPORT("mid-flight: some completed, some blocked (0 < done < n_pre)",
           c1 > 0 && c1 < n_pre);

    for (int i = n_pre; i < n_pre + n_mid; ++i)
        l2_probe_kernel<<<BLOCKS, THREADS, 0, stream>>>(g_dout, g_sink, i, g_spin5);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    int c2 = countCompleted();
    REPORT("queue frozen while suspended (no further completions)", c2 == c1);

    t0 = nowMs();
    XQueueResume(xq, kQueueResumeFlagNone);
    cudaError_t e = cudaStreamSynchronize(stream);
    double resume_ms = nowMs() - t0;
    int total = n_pre + n_mid;
    REPORT("all commands completed after resume", e == cudaSuccess &&
           countCompleted() == total);
    REPORT("all outputs correct (exactly-once semantics)", verifyOut(total));
    std::printf("  [info] resume->sync: %.1f ms\n", resume_ms);
}

// ---------------------------------------------------------------------
//  I3: suspend/resume cycles
// ---------------------------------------------------------------------
static void caseI3(cudaStream_t stream, XQueueHandle xq, int rounds)
{
    std::printf("[I3] %d suspend/resume cycles (8 pre + 4 mid kernels each)\n", rounds);
    int bad = 0;
    for (int r = 0; r < rounds; ++r) {
        fillOut(FILL_BYTE);
        for (int i = 0; i < 8; ++i)
            l2_probe_kernel<<<BLOCKS, THREADS, 0, stream>>>(g_dout, g_sink, i, g_spin5);
        std::this_thread::sleep_for(std::chrono::milliseconds(6));
        XQueueSuspend(xq, kQueueSuspendFlagSyncHwQueue);
        for (int i = 8; i < 12; ++i)
            l2_probe_kernel<<<BLOCKS, THREADS, 0, stream>>>(g_dout, g_sink, i, g_spin5);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        XQueueResume(xq, kQueueResumeFlagNone);
        if (cudaStreamSynchronize(stream) != cudaSuccess) { bad = r + 1; break; }
        if (countCompleted() != 12 || !verifyOut(12)) { bad = r + 1; break; }
    }
    char note[96];
    std::snprintf(note, sizeof(note), "%d cycles converged", rounds);
    REPORT(note, bad == 0);
    if (bad) std::printf("  [info] failed at round %d\n", bad);
}

// ---------------------------------------------------------------------
//  I4: mixed command stream (kernel + memcpyAsync + memsetAsync)
// ---------------------------------------------------------------------
static void caseI4(cudaStream_t stream, XQueueHandle xq)
{
    std::printf("[I4] mixed stream: kernel + memcpyAsync + memsetAsync across suspend\n");
    fillOut(FILL_BYTE);

    const size_t kBytes  = 1 << 20;   // 1 MB payload
    const size_t kMidOff = 64 << 10;  // memset window: [64K, 576K)
    const size_t kMidLen = 512 << 10;

    void *d_buf = nullptr;
    cudaError_t e = cudaMalloc(&d_buf, kBytes);
    REPORT("device buffer allocated", e == cudaSuccess && d_buf != nullptr);
    std::vector<unsigned char> h_src(kBytes, 0x5A);
    std::vector<unsigned char> h_dst(kBytes, 0);

    /* strictly ordered single stream: kernel, copy, memset-window, read, kernel */
    l2_probe_kernel<<<BLOCKS, THREADS, 0, stream>>>(g_dout, g_sink, 0, g_spin5);
    cudaMemcpyAsync(d_buf, h_src.data(), kBytes, cudaMemcpyHostToDevice, stream);
    cudaMemsetAsync((unsigned char *)d_buf + kMidOff, 0x77, kMidLen, stream);
    l2_read_kernel<<<1, 32, 0, stream>>>((const unsigned long long *)d_buf, g_dout, 2);
    l2_probe_kernel<<<BLOCKS, THREADS, 0, stream>>>(g_dout, g_sink, 1, g_spin5);
    l2_probe_kernel<<<BLOCKS, THREADS, 0, stream>>>(g_dout, g_sink, 3, g_spin5);

    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    XQueueSuspend(xq, kQueueSuspendFlagSyncHwQueue);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    XQueueResume(xq, kQueueResumeFlagNone);
    e = cudaStreamSynchronize(stream);
    REPORT("mixed stream synchronized", e == cudaSuccess);

    REPORT("kernels around the memory ops all completed", countCompleted() == 4);
    REPORT("kernel outputs correct", verifyOut(4));

    cuMemcpyDtoH_v2(h_dst.data(), (cu_devptr)d_buf, kBytes);
    bool head_ok = true, mid_ok = true, tail_ok = true;
    for (size_t i = 0; i < kMidOff; ++i)
        if (h_dst[i] != 0x5A) { head_ok = false; break; }
    for (size_t i = kMidOff; i < kMidOff + kMidLen; ++i)
        if (h_dst[i] != 0x77) { mid_ok = false; break; }
    for (size_t i = kMidOff + kMidLen; i < kBytes; ++i)
        if (h_dst[i] != 0x5A) { tail_ok = false; break; }
    REPORT("memcpy payload intact (head region 0x5A)", head_ok);
    REPORT("memset window intact (mid region 0x77)", mid_ok);
    REPORT("memcpy payload intact (tail region 0x5A)", tail_ok);
    REPORT("read kernel observed the copied head (out[2])",
           h_snap[2] == MAGIC_BASE + 2);

    cudaFree(d_buf);
}

// ---------------------------------------------------------------------
//  I5: unmanaged stream (no XQueue) must keep working (DirectLaunch)
// ---------------------------------------------------------------------
static void caseI5(cudaStream_t managed, cudaStream_t probe)
{
    std::printf("[I5] unmanaged stream alongside the managed level-2 queue\n");
    fillOut(FILL_BYTE);

    for (int i = 0; i < 4; ++i)
        l2_probe_kernel<<<BLOCKS, THREADS, 0, managed>>>(g_dout, g_sink, i, g_spin5 / 2);
    for (int i = 16; i < 20; ++i)
        l2_probe_kernel<<<BLOCKS, THREADS, 0, probe>>>(g_dout, g_sink, i, g_spin5 / 2);

    cudaError_t e1 = cudaStreamSynchronize(probe);
    cudaError_t e2 = cudaStreamSynchronize(managed);
    REPORT("both streams synchronized", e1 == cudaSuccess && e2 == cudaSuccess);
    REPORT("managed + unmanaged kernels all completed (8)", countCompleted() == 8);

    cuMemcpyDtoH_v2(h_snap.data(), (cu_devptr)g_dout, OUT_SLOTS * 8);
    bool ok = true;
    for (int i = 0; i < 4; ++i)
        if (h_snap[i] != MAGIC_BASE + (unsigned long long)i) ok = false;
    for (int i = 16; i < 20; ++i)
        if (h_snap[i] != MAGIC_BASE + (unsigned long long)i) ok = false;
    REPORT("all outputs correct (both paths)", ok);
}

// ---------------------------------------------------------------------
//  I6: two managed queues -- independent suspend/resume
// ---------------------------------------------------------------------
static void caseI6(cudaStream_t s1, XQueueHandle q1, cudaStream_t s2, XQueueHandle q2)
{
    std::printf("[I6] two level-2 queues: suspend q1, q2 unaffected\n");
    fillOut(FILL_BYTE);

    for (int i = 0; i < 8; ++i)                                   // q1: idx 0..7
        l2_probe_kernel<<<BLOCKS, THREADS, 0, s1>>>(g_dout, g_sink, i, g_spin5);
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    XQueueSuspend(q1, kQueueSuspendFlagSyncHwQueue);
    int c1 = countCompleted();

    for (int i = 64; i < 72; ++i)                                 // q2: idx 64..71
        l2_probe_kernel<<<BLOCKS, THREADS, 0, s2>>>(g_dout, g_sink, i, g_spin5);
    cudaStreamSynchronize(s2);                                    // q2 must flow
    int c2 = countCompleted();
    REPORT("q2 completed while q1 suspended (+8)", c2 == c1 + 8);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REPORT("q1 stayed frozen", countCompleted() == c2);

    XQueueResume(q1, kQueueResumeFlagNone);
    cudaStreamSynchronize(s1);
    REPORT("q1 resumed: all 16 commands completed exactly once",
           countCompleted() == 16);

    cuMemcpyDtoH_v2(h_snap.data(), (cu_devptr)g_dout, OUT_SLOTS * 8);
    bool ok = true;
    for (int i = 0; i < 8; ++i)
        if (h_snap[i] != MAGIC_BASE + (unsigned long long)i) ok = false;
    for (int i = 64; i < 72; ++i)
        if (h_snap[i] != MAGIC_BASE + (unsigned long long)i) ok = false;
    REPORT("all outputs correct", ok);
}

// ---------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    std::printf("==================================================\n");
    std::printf("  T7 integration: sm120 Level-2 via DLL-proxy stack\n");
    std::printf("==================================================\n");

    cudaDeviceProp prop;
    if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) {
        std::printf("FATAL: cannot query device\n");
        return 2;
    }
    std::printf("[env] %s, CC %d.%d\n", prop.name, prop.major, prop.minor);
    REPORT("sm_120 device", prop.major == 12 && prop.minor == 0);

    h_snap.assign(OUT_SLOTS, 0);
    cudaFree(0);  /* initialize the runtime + primary context */
    if (cuMemAlloc_v2((cu_devptr *)&g_dout, OUT_SLOTS * 8) != 0 || g_dout == nullptr ||
        cuMemAlloc_v2((cu_devptr *)&g_sink, OUT_SLOTS * 8) != 0 || g_sink == nullptr) {
        std::printf("FATAL: cannot allocate observation buffers\n");
        return 2;
    }

    cudaStream_t stream_main, stream_probe, stream2;
    cudaStreamCreate(&stream_main);
    cudaStreamCreate(&stream_probe);
    cudaStreamCreate(&stream2);

    HwQueueHandle hwq = 0, hwq2 = 0;
    XQueueHandle  xq = 0, xq2 = 0;
    XResult r1 = CudaQueueCreate(&hwq, stream_main);
    XResult r2 = XQueueCreate(&xq, hwq, kPreemptLevelDeactivate, kQueueCreateFlagNone);
    XQueueSetLaunchConfig(xq, 8, 4);
    std::printf("[queue] CudaQueueCreate res=%d, XQueueCreate(level-2) res=%d\n",
                (int)r1, (int)r2);
    REPORT("level-2 XQueue created (sm120 -> CudaQueueLv2)", r1 == 0 && r2 == 0);

    XResult r3 = CudaQueueCreate(&hwq2, stream2);
    XResult r4 = XQueueCreate(&xq2, hwq2, kPreemptLevelDeactivate, kQueueCreateFlagNone);
    XQueueSetLaunchConfig(xq2, 8, 4);
    REPORT("second level-2 XQueue created", r3 == 0 && r4 == 0);

    calibrate(stream_main);

    caseI1(stream_main, 32);
    caseI2(stream_main, xq, 24, 8);
    caseI3(stream_main, xq, 20);
    caseI4(stream_main, xq);
    caseI5(stream_main, stream_probe);
    caseI6(stream_main, xq, stream2, xq2);

    std::printf("==================================================\n");
    std::printf("  RESULT: %d failed check(s)\n", g_fail);
    std::printf("==================================================\n");
    return g_fail;
}
