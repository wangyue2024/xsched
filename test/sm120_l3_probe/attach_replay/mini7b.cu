// mini7b: trigger-based L3 verdict — eternal spin kernel + cuXtraTriggerTrap + signature payload
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include "xsched/xsched.h"
#include "xsched/cuda/hal.h"
extern "C" {
#include <cuxtra/cuxtra.h>
int cuCtxGetCurrent(void **ctx);
}
__global__ void spin_k(volatile unsigned long long *c) { while (true) { *c = *c + 1; } }
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int off = (argc > 1 && strcmp(argv[1], "none") != 0) ? (int)strtoul(argv[1], nullptr, 0) : -1;
    cudaFree(0);
    volatile unsigned long long *cnt;
    cudaMallocManaged((void**)&cnt, sizeof(*cnt));
    *cnt = 0;
    __attribute__((unused)) volatile int *flag;
    cudaMallocManaged((void**)&flag, sizeof(int)); *flag = 0;
    if (off >= 0) {
        char ea[64], eo[32];
        snprintf(ea, sizeof ea, "0x%llx", (unsigned long long)flag);
        snprintf(eo, sizeof eo, "0x%x", off);
        setenv("XSCHED_SM120_TRAP_TEST_ADDR", ea, 1);
        setenv("XSCHED_SM120_TRAP_TEST_OFFSET", eo, 1);
        printf("[m7b] payload@%#x -> flag %p\n", off, (void*)flag);
    } else printf("[m7b] baseline (no patch)\n");
    cudaStream_t qs; cudaStreamCreate(&qs);
    XQueueHandle xq; HwQueueHandle hwq;
    CudaQueueCreate(&hwq, qs);
    XQueueCreate(&xq, hwq, 3, kQueueCreateFlagNone);
    // launch eternal spin on a non-blocking stream
    cudaStream_t s; cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking);
    spin_k<<<1, 32, 0, s>>>(cnt);
    printf("[m7b] spin launched err=%d\n", (int)cudaGetLastError());
    usleep(500000);
    // pinned sampler
    unsigned long long *pin; cudaHostAlloc((void**)&pin, 8, cudaHostAllocDefault);
    cudaStream_t cs; cudaStreamCreateWithFlags(&cs, cudaStreamNonBlocking);
    auto sample = [&](const char *tag) {
        cudaMemcpyAsync((void*)pin, (void*)(const unsigned long long*)cnt, 8, cudaMemcpyDeviceToHost, cs);
        cudaStreamSynchronize(cs);
        printf("[m7b] %s: cnt=%llu flag=%d\n", tag, *pin, *flag);
        return *pin;
    };
    unsigned long long c0 = sample("before-trigger");
    // TRIGGER
    void *ctx = nullptr; cuCtxGetCurrent(&ctx);
    printf("[m7b] triggering trap on ctx=%p ...\n", (void*)ctx);
    cuXtraTriggerTrap((CUcontext)ctx);
    usleep(300000);
    unsigned long long c1 = sample("after-trigger+0.3s");
    usleep(500000);
    unsigned long long c2 = sample("after-trigger+0.8s");
    printf("[m7b] VERDICT: counter [%llu -> %llu -> %llu] %s | payload_ran=%s\n",
        c0, c1, c2,
        (c2 > c1 ? "STILL SPINNING (trigger no effect)" : "FROZEN/KILLED (trigger works!!)"),
        (*flag == 0x5AA5 ? "YES!!!" : "no"));
    fflush(stdout);
    _exit(0);
}
