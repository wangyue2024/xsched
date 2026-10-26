// spin_ab.cu - long-running target for tracing cuda-gdb's interrupt on Ampere.
// Prints a host-mapped counter once per second; allows ptrace from any process.
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/prctl.h>
#include <cuda_runtime.h>

__global__ void spin_kernel(volatile unsigned long long *cnt, volatile int *stop,
                            unsigned long long limit)
{
    for (unsigned long long i = 0; i < limit && *stop == 0; i++) *cnt = i;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    unsigned long long limit = ~0ULL;
    if (argc > 2) limit = strtoull(argv[2], nullptr, 0);

    cudaError_t ce = cudaSetDevice(0);
    if (ce != cudaSuccess) { fprintf(stderr, "setDevice: %s\n", cudaGetErrorString(ce)); return 1; }
    unsigned long long *h = nullptr, *d = nullptr;
    int *hs = nullptr, *ds = nullptr;
    cudaHostAlloc((void **)&h, sizeof(*h), cudaHostAllocMapped);
    cudaHostGetDevicePointer((void **)&d, h, 0);
    cudaHostAlloc((void **)&hs, sizeof(*hs), cudaHostAllocMapped);
    cudaHostGetDevicePointer((void **)&ds, hs, 0);
    *h = 0; *hs = 0;
    spin_kernel<<<1, 1>>>(d, ds, limit);
    printf("[spin] pid=%d secs=%d err=%s\n", (int)getpid(), secs,
           cudaGetErrorString(cudaGetLastError()));
    for (int i = 0; i < secs; i++) {
        sleep(1);
        printf("[spin] t=%ds counter=%llu\n", i + 1, *h);
    }
    *hs = 1;
    cudaDeviceSynchronize();
    printf("[spin] done counter=%llu\n", *h);
    return 0;
}
