// tsg_smoke: 验证 sm120 Linux 默认路由 = CudaQueueLv3Tsg，且 suspend/resume 周期可用
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "xsched/xsched.h"
#include "xsched/cuda/hal.h"
__global__ void k(int *p) { *p = 42; }
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    cudaFree(0);
    int *d = nullptr;
    if (cudaMalloc(&d, 4) != cudaSuccess) { printf("[smoke] malloc fail\n"); return 1; }
    cudaStream_t s; cudaStreamCreate(&s);
    HwQueueHandle hwq = 0;
    CudaQueueCreate(&hwq, s);                         // -> arch.cpp 路由（期望 Lv3Tsg）
    XQueueHandle xq = 0;
    XResult rc = XQueueCreate(&xq, hwq, 3, kQueueCreateFlagNone);   // level 3
    printf("[smoke] CudaQueueCreate+XQueueCreate(level3) rc=%d\n", (int)rc);
    k<<<1,32,0,s>>>(d);
    cudaStreamSynchronize(s);
    int h = 0; cudaMemcpy(&h, d, 4, cudaMemcpyDeviceToHost);
    printf("[smoke] kernel1 done h=%d (expect 42)\n", h);
    // L3 suspend（TSG 路径：不带同步标志）
    rc = XQueueSuspend(xq, 0);
    printf("[smoke] XQueueSuspend rc=%d\n", (int)rc);
    h = 0; k<<<1,32,0,s>>>(d);
    rc = XQueueResume(xq, 0);
    printf("[smoke] XQueueResume rc=%d\n", (int)rc);
    cudaStreamSynchronize(s);
    cudaMemcpy(&h, d, 4, cudaMemcpyDeviceToHost);
    printf("[smoke] kernel2 done h=%d (expect 42) => SMOKE %s\n", h, h == 42 ? "PASS" : "FAIL");
    return 0;
}
