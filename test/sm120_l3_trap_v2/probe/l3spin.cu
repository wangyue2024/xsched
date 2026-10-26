// l3spin.cu - direct evidence for the sm120 trap backend inside XSched.
//
// One long running kernel publishes its iteration count to host-mapped memory.
// A sampler thread reads that counter (no CUDA call, so it keeps working while
// the GPU is stopped) while the main thread suspends and resumes the queue.
//
// Expected with a working trap preemption:
//   run -> counter rises
//   XQueueSuspend returns after the trap killed the kernel -> counter frozen
//   XQueueResume re-launches from the command log -> counter rises again

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <atomic>
#include <thread>
#include <chrono>
#include <cuda_runtime.h>
#include <cstring>
#include <ctime>

#include "xsched/xqueue.h"
#include "xsched/cuda/hal.h"

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)

__global__ void spin_publish(volatile unsigned long long *c, unsigned long long cycles)
{
    unsigned long long start = clock64(), i = 0;
    while (clock64() - start < cycles) { i++; if ((i & 0xff) == 0) *c = i; }
    *c = i | (1ULL << 63);
}

static double Now()
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    long spin_ms = argc > 1 ? atol(argv[1]) : 4000;
    long hold_ms = argc > 2 ? atol(argv[2]) : 1500;

    cudaStream_t stream;
    RT_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));

    unsigned long long *host_cnt = nullptr;
    RT_CHECK(cudaHostAlloc((void **)&host_cnt, 4096, cudaHostAllocMapped));
    memset(host_cnt, 0, 4096);
    unsigned long long *dev_cnt = nullptr;
    RT_CHECK(cudaHostGetDevicePointer((void **)&dev_cnt, host_cnt, 0));

    XQueueHandle xq;
    HwQueueHandle hwq;
    CudaQueueCreate(&hwq, stream);
    XQueueCreate(&xq, hwq, 3, kQueueCreateFlagNone);
    XQueueSetLaunchConfig(xq, 8, 4);

    cudaDeviceProp prop;
    RT_CHECK(cudaGetDeviceProperties(&prop, 0));
    unsigned long long cycles = (unsigned long long)prop.clockRate * 1000ULL * spin_ms / 1000ULL;

    std::atomic<bool> done(false);
    std::thread sampler([&]() {
        unsigned long long last = 0;
        while (!done.load()) {
            unsigned long long v = *host_cnt;
            printf("[sample] %.3f counter=%llu\n", Now(), v & ~(1ULL << 63));
            (void)last; last = v;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    printf("[l3spin] launch: %ld ms, hold %ld ms, blocks 8\n", spin_ms, hold_ms);
    spin_publish<<<8, 32, 0, stream>>>(dev_cnt, cycles);
    printf("[l3spin] launch -> %s\n", cudaGetErrorName(cudaGetLastError()));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    double t0 = Now();
    XQueueSuspend(xq, kQueueSuspendFlagSyncHwQueue);
    double t1 = Now();
    printf("[l3spin] suspend returned in %.3f s, counter=%llu\n", t1 - t0, *host_cnt & ~(1ULL << 63));

    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    printf("[l3spin] after hold, counter=%llu\n", *host_cnt & ~(1ULL << 63));

    double t2 = Now();
    XQueueResume(xq, kQueueResumeFlagNone);
    double t3 = Now();
    printf("[l3spin] resume returned in %.3f s\n", t3 - t2);

    cudaError_t se = cudaStreamSynchronize(stream);
    printf("[l3spin] final sync -> %s counter=%llu\n",
           cudaGetErrorName(se), *host_cnt & ~(1ULL << 63));

    done = true;
    sampler.join();
    return 0;
}
