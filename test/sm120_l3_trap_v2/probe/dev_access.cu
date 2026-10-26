// dev_access.cu - can device code read/write the driver's CUDA preemption buffer?
//
// The host-side path is unusable while a kernel runs: cuXtraMemcpyHtoD into the
// arena blocked for 17.8 s (exactly until the running kernel finished), i.e. it
// synchronizes the context.  If a kernel can touch the buffer itself, suspend
// and resume can both be driven from the device, and the host only has to fire
// the (non-blocking) RM trap trigger.
//
// Modes: 0 = read [+4], 1 = write 2 then read back, 2 = dump the first 16 words,
//        3 = write 0 (clear).
//
// Build: ../build.sh dev_access    Run:
//   CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1 ./dev_access [--mode N]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <cuda_runtime.h>
#include "arena.h"

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)

// out[0] = status/first value, out[1..] = dumped words
__global__ void pbuf_test(volatile uint32_t *cmd, volatile unsigned long long *out, int mode)
{
    if (mode == 0) {
        out[0] = *cmd;
    } else if (mode == 1) {
        *cmd = 2u;
        __threadfence_system();
        out[0] = *cmd;
    } else if (mode == 2) {
        for (int i = 0; i < 16; i++) out[i] = cmd[i];

    } else if (mode == 3) {
        *cmd = 0u;
        __threadfence_system();
        out[0] = *cmd;
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    int mode = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--mode") mode = atoi(argv[++i]);
    }

    RT_CHECK(cudaFree(0));
    CUcontext ctx = nullptr;
    if (cuCtxGetCurrent(&ctx) || !ctx) { fprintf(stderr, "no current context\n"); return 1; }
    CUstream opstream;
    if (cuStreamCreate(&opstream, CU_STREAM_NON_BLOCKING)) return 1;

    auto I = sm120arena::probe(ctx, opstream);
    sm120arena::report(I);
    if (!I.ok || !I.preempt_buf) { fprintf(stderr, "preemption buffer not resolved\n"); return 1; }

    unsigned long long *out = nullptr;
    RT_CHECK(cudaMalloc(&out, 32 * sizeof(unsigned long long)));
    RT_CHECK(cudaMemset(out, 0, 32 * sizeof(unsigned long long)));
    RT_CHECK(cudaDeviceSynchronize());

    volatile uint32_t *cmd = (volatile uint32_t *)(uintptr_t)(I.preempt_buf + 4);
    printf("[test] mode=%d cmd word device VA = %p\n", mode, (void *)cmd);

    pbuf_test<<<1, 1, 0, 0>>>((volatile uint32_t *)(uintptr_t)cmd, out, mode);
    cudaError_t le = cudaGetLastError();
    cudaError_t sy = cudaDeviceSynchronize();
    printf("[test] launch=%s sync=%s\n", cudaGetErrorName(le), cudaGetErrorName(sy));

    unsigned long long h[32] = {0};
    if (sy == cudaSuccess) {
        RT_CHECK(cudaMemcpy(h, out, sizeof h, cudaMemcpyDeviceToHost));
        if (mode == 2) {
            printf("[dump] first 16 words of the preemption buffer:\n  ");
            for (int i = 0; i < 16; i++) printf("%08llx ", h[i]);
            printf("\n");
        } else {
            printf("[result] out[0] = %#llx (%llu)\n", h[0], h[0]);
        }
    }
    printf("[verdict] device-side %s of the preemption buffer %s\n",
           mode == 0 || mode == 2 ? "read" : "write",
           sy == cudaSuccess ? "WORKS" : "FAULTS (protected)");
    return sy == cudaSuccess ? 0 : 2;
}
