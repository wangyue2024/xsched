// spin.cu — eternal-spin CUDA app (L3 attach capture target / suspend testbed)
// 运行 ~20 秒并每 0.5s 打印一次设备计数器；内核会一直旋转直到进程退出。
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cuda_runtime.h>

__global__ void spin_kernel(volatile unsigned long long *c) {
    while (true) { *c = *c + 1; }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    cudaFree(0);
    unsigned long long *d = nullptr;
    if (cudaMalloc((void**)&d, 8) != cudaSuccess) { printf("malloc fail\n"); return 1; }
    cudaMemset((void*)d, 0, 8);
    spin_kernel<<<4, 256>>>(d);
    cudaError_t e = cudaGetLastError();
    printf("[spin] launched err=%d (%s) dev=%s\n", (int)e, cudaGetErrorName(e), [&]{ cudaDeviceProp p; cudaGetDeviceProperties(&p, 0); static char n[256]; snprintf(n, 256, "%s", p.name); return n; }());
    unsigned long long h = 0;
    for (int i = 0; i < 40; i++) {
        usleep(500000);
        if (cudaMemcpy((void*)&h, (void*)d, 8, cudaMemcpyDeviceToHost) != cudaSuccess) { printf("[spin] memcpy err\n"); break; }
        printf("[spin] t=%.1fs counter=%llu\n", (i+1)*0.5, h);
    }
    printf("[spin] exiting (kernel left running)\n");
    _exit(0);
}
