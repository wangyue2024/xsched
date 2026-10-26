// correct_ab.cu - L3 correctness on sm86: vector-add launched on a managed
// stream while XQueueSuspend/Resume repeatedly preempts it; verify C == A+B.
//
// Build (3090):
//   nvcc -O2 -std=c++17 -arch=sm_86 -ccbin g++ correct_ab.cu -o correct_ab \
//     -I<xsched-up>/include -I<xsched-up>/platforms/cuda/hal/include \
//     -I/usr/local/cuda/include -L<xsched-up>/output/lib \
//     -lhalcuda -lpreempt -lutils -lcudart -lpthread
// Run: LD_LIBRARY_PATH=<xsched-up>/output/lib ./correct_ab

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>
#include <cuda_runtime.h>
#include "xsched/xsched.h"
#include "xsched/cuda/hal.h"

#define VN (1 << 24)   // 16M ints = 64 MB per buffer

__global__ void vadd(const int *a, const int *b, int *c, int n)
{
    int i = blockDim.x * blockIdx.x + threadIdx.x;
    if (i < n) c[i] = a[i] + b[i];
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    cudaStream_t s;
    cudaStreamCreate(&s);
    HwQueueHandle hwq;
    XQueueHandle xq;
    CudaQueueCreate(&hwq, s);
    // 3 == kPreemptLevelInterrupt in the upstream enum
    XQueueCreate(&xq, hwq, 3, kQueueCreateFlagNone);
    XQueueSetLaunchConfig(xq, 8, 4);

    int *ha = (int *)malloc(sizeof(int) * VN);
    int *hb = (int *)malloc(sizeof(int) * VN);
    int *hc = (int *)malloc(sizeof(int) * VN);
    for (int i = 0; i < VN; i++) { ha[i] = i; hb[i] = 2 * i + 1; hc[i] = -1; }
    int *da, *db, *dc;
    cudaMalloc(&da, sizeof(int) * VN);
    cudaMalloc(&db, sizeof(int) * VN);
    cudaMalloc(&dc, sizeof(int) * VN);
    cudaMemcpy(da, ha, sizeof(int) * VN, cudaMemcpyHostToDevice);
    cudaMemcpy(db, hb, sizeof(int) * VN, cudaMemcpyHostToDevice);
    cudaMemset(dc, 0xff, sizeof(int) * VN);

    std::atomic<bool> stop{false};
    std::thread worker([&]() {
        int bs = 256, gs = (VN + bs - 1) / bs;
        for (int k = 0; k < 400 && !stop.load(); k++) {
            vadd<<<gs, bs, 0, s>>>(da, db, dc, VN);
        }
        cudaStreamSynchronize(s);
    });

    std::this_thread::sleep_for(std::chrono::seconds(1));
    int cycles = 0;
    for (int i = 0; i < 200 && !stop.load(); i++) {
        int64_t t0 = (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        XQueueSuspend(xq, kQueueSuspendFlagSyncHwQueue);
        int64_t t1 = (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        usleep(5000);
        XQueueResume(xq, 0);
        int64_t t2 = (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (cycles < 5) printf("[cycle %d] suspend=%lld us resume=%lld us\n",
                               i, (long long)(t1 - t0), (long long)(t2 - t1));
        cycles++;
        usleep(5000);
    }
    stop = true;
    worker.join();
    cudaError_t ce = cudaStreamSynchronize(s);
    printf("[done] cycles=%d stream=%s\n", cycles, cudaGetErrorString(ce));
    cudaMemcpy(hc, dc, sizeof(int) * VN, cudaMemcpyDeviceToHost);
    int bad = 0;
    for (int i = 0; i < VN; i++) {
        if (hc[i] != ha[i] + hb[i]) {
            if (bad < 5) printf("[MISMATCH] i=%d got=%d want=%d\n", i, hc[i], ha[i] + hb[i]);
            bad++;
        }
    }
    printf("[verify] bad=%d / %d -> %s\n", bad, VN, bad == 0 ? "OK" : "FAIL");
    return bad == 0 ? 0 : 1;
}
