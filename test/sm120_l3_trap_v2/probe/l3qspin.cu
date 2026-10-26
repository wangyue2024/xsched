// l3qspin.cu - managed-kernel trap test for the sm120 L3 backend.
//
// cudart resolves driver entry points through dlopen/dlsym, so LD_PRELOAD of
// XSched's driver shim does not intercept the normal <<<>>> path in this
// build.  The shim does export XLaunchKernel(), the sanctioned interception
// entry point; calling it explicitly puts the kernel into the XQueue, which
// is what the trap backend needs (instrumentation, command log, replay).

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <atomic>
#include <thread>
#include <chrono>
#include <cuda_runtime.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <cuda.h>
#include <cuxtra/cuxtra.h>

#include "xsched/xqueue.h"
#include "xsched/cuda/hal.h"

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)

typedef CUresult (*LaunchFn)(CUfunction, unsigned int, unsigned int, unsigned int,
                             unsigned int, unsigned int, unsigned int,
                             unsigned int, CUstream, void **, void **);
#include <dlfcn.h>

__global__ void spin_publish(volatile unsigned long long *c, unsigned long long cycles,
                            unsigned long long *done)
{
    unsigned long long start = clock64(), i = 0;
    while (clock64() - start < cycles) { i++; if ((i & 0xff) == 0) *c = i; }
    *c = i | (1ULL << 63);
    *done = i | (1ULL << 63);   // device-side completion word
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    long spin_ms = argc > 1 ? atol(argv[1]) : 4000;
    long hold_ms = argc > 2 ? atol(argv[2]) : 700;

    if (getenv("L3Q_ALLOW_PTRACE")) prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    if (getenv("L3Q_START_DELAY")) {
        printf("[l3qspin] start delay %s s (pid=%d)\n", getenv("L3Q_START_DELAY"), (int)getpid());
        sleep(atoi(getenv("L3Q_START_DELAY")));
    }
    int dev = 0;
    RT_CHECK(cudaSetDevice(dev));
    RT_CHECK(cudaFree(0));
    cudaStream_t stream;
    RT_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));

    unsigned long long *hcnt = nullptr;
    RT_CHECK(cudaHostAlloc((void **)&hcnt, 4096, cudaHostAllocMapped));
    memset((void *)hcnt, 0, 4096);
    unsigned long long *dcnt = nullptr;
    RT_CHECK(cudaHostGetDevicePointer((void **)&dcnt, hcnt, 0));

    XQueueHandle xq; HwQueueHandle hwq;
    CudaQueueCreate(&hwq, (CUstream)stream);
    XQueueCreate(&xq, hwq, 3, kQueueCreateFlagNone);
    XQueueSetLaunchConfig(xq, 8, 4);

    cudaDeviceProp prop;
    RT_CHECK(cudaGetDeviceProperties(&prop, 0));
    unsigned long long cycles = (unsigned long long)prop.clockRate * 1000ULL * spin_ms / 1000ULL;

    cudaKernel_t ck = nullptr;
    RT_CHECK(cudaGetKernel(&ck, (const void *)spin_publish));
    CUfunction f = nullptr;
    CUresult kr = cuKernelGetFunction(&f, (CUkernel)(void *)ck);
    printf("[l3qspin] func=%p kernel=%d\n", (void *)f, (int)kr);
    if (kr != CUDA_SUCCESS) return 1;

    unsigned long long *ddone = nullptr;
    RT_CHECK(cudaMalloc((void **)&ddone, 8));
    RT_CHECK(cudaMemset(ddone, 0, 8));

    std::atomic<bool> stop(false);
    std::thread sampler([&]() {
        while (!stop.load()) {
            printf("[sample] counter=%llu\n", *hcnt & ~(1ULL << 63));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    void *shim = dlopen("libshimcuda.so", RTLD_NOW | RTLD_NOLOAD);
    LaunchFn launch = shim ? (LaunchFn)dlsym(shim, "cuLaunchKernel") : nullptr;
    printf("[l3qspin] shim=%p launch=%p\n", shim, (void *)launch);
    if (launch == nullptr) return 1;
    void *params[3] = { &dcnt, &cycles, &ddone };
    LaunchFn real_launch = nullptr;
    if (getenv("L3Q_DIRECT")) {
        void *lc = dlopen("libcuda.so.1", RTLD_NOW);
        real_launch = lc ? (LaunchFn)dlsym(lc, "cuLaunchKernel") : nullptr;
        printf("[l3qspin] direct launch via libcuda: %p\n", (void *)real_launch);
    }
    CUresult lr = real_launch
        ? real_launch(f, 8, 1, 1, 32, 1, 1, 0, (CUstream)stream, params, nullptr)
        : launch(f, 8, 1, 1, 32, 1, 1, 0, (CUstream)stream, params, nullptr);
    printf("[l3qspin] XLaunchKernel -> %d\n", (int)lr);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // Optional fine-grained timeline: detect the exact moment the device
    // counter stops (us resolution) and compare it with the host request /
    // Suspend() return times.  Separates "trap takes effect" from
    // "queue Synchronize costs".
    std::atomic<int> tl_req(0);
    std::atomic<double> tl_freeze(0.0), tl_req_t(0.0);
    std::atomic<bool> tl_stop(false);
    double tl_thresh = getenv("L3Q_TL_THRESH_US") ? atol(getenv("L3Q_TL_THRESH_US")) * 1e-6 : 200e-6;
    std::thread timeline;
    auto now_d = []() {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
    };
    if (getenv("L3Q_TIMELINE")) {
        timeline = std::thread([&]() {
            while (tl_req.load() == 0) { }
            tl_req_t = now_d();
            unsigned long long last = *hcnt; double tlast = now_d();
            while (!tl_stop.load()) {
                unsigned long long v = *hcnt; double t = now_d();
                if (v != last) { last = v; tlast = t; }
                else if (t - tlast > tl_thresh) { tl_freeze = tlast; break; }
            }
        });
    }
    if (getenv("L3Q_TIMELINE")) tl_req = 1;
    int ncyc = getenv("L3Q_CYCLES") ? atoi(getenv("L3Q_CYCLES")) : 1;
    for (int cyc = 0; cyc < ncyc; cyc++) {
    auto t0 = std::chrono::steady_clock::now();
    XQueueSuspend(xq, kQueueSuspendFlagSyncHwQueue);
    auto t1 = std::chrono::steady_clock::now();
    printf("[l3qspin] cycle %d suspend %.1f ms counter=%llu\n", cyc,
           std::chrono::duration<double, std::milli>(t1 - t0).count(), *hcnt & ~(1ULL << 63));
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    XQueueResume(xq, kQueueResumeFlagNone);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (getenv("L3Q_TIMELINE")) {
        tl_stop = true;
        double t_end = now_d();
        printf("[timeline] req=%.6f freeze=%.6f suspend_end=%.6f -> freeze-req=%.3f ms, end-req=%.3f ms\n",
               tl_req_t.load(), tl_freeze.load(), t_end,
               (tl_freeze.load() - tl_req_t.load()) * 1e3, (t_end - tl_req_t.load()) * 1e3);
        if (timeline.joinable()) timeline.join();
    }
    printf("[l3qspin] after %d cycles counter=%llu\n", ncyc, *hcnt & ~(1ULL << 63));
    if (getenv("L3Q_RELAUNCH")) {
        // Fresh managed launch after the resume: distinguishes "context is
        // damaged by the trap" from "replaying the old command is unsafe".
        *hcnt = 0;
        CUresult r2 = launch(f, 8, 1, 1, 32, 1, 1, 0, (CUstream)stream, params, nullptr);
        printf("[l3qspin] fresh XLaunchKernel -> %d\n", (int)r2);
    }
    for (int i = 0; i < 6; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        printf("[l3qspin] post-resume %d counter=%llu\n", i, *hcnt & ~(1ULL << 63));
    }
    cudaError_t se = cudaStreamSynchronize(stream);
    unsigned long long dv = 0;
    cudaError_t de = cudaMemcpy(&dv, ddone, 8, cudaMemcpyDeviceToHost);
    printf("[l3qspin] final sync -> %s counter=%llu raw=%#llx done=%d dev=%#llx (r=%s)\n",
           cudaGetErrorName(se), *hcnt & ~(1ULL << 63), *hcnt,
           (int)((*hcnt >> 63) & 1), dv, cudaGetErrorName(de));

    if (getenv("L3Q_HOLD_END")) {
        printf("[l3qspin] holding %s s for probes\n", getenv("L3Q_HOLD_END"));
        std::this_thread::sleep_for(std::chrono::seconds(atoi(getenv("L3Q_HOLD_END"))));
    }
    stop = true; sampler.join();
    return 0;
}
