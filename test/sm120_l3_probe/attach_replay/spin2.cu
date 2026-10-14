// counter kernel on its own stream; host prints counter every 0.5s (no queue-behind)
#include <cstdio>
#include <unistd.h>
#include <cuda_runtime.h>
__global__ void spin_counter(volatile unsigned long long *c) { while (true) { *c = *c + 1; } }
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    cudaFree(0);
    unsigned long long *d; cudaMalloc((void**)&d, 8); cudaMemset((void*)d, 0, 8);
    cudaStream_t s; cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking);
    spin_counter<<<1, 32, 0, s>>>(d);
    printf("[s2] launched err=%d\n", (int)cudaGetLastError());
    unsigned long long h = 0;
    for (int i = 0; i < 40; i++) {
        usleep(500000);
        cudaMemcpy(&h, (void*)d, 8, cudaMemcpyDeviceToHost);
        printf("[s2] t=%.1f counter=%llu\n", (i+1)*0.5, h);
    }
    _exit(0);
}
