// mini9: does a GENUINE trap (BPT.TRAP in a kernel) execute the patched
// tools trap handler? Independent of the RM-trigger path.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cuda_runtime.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <chrono>
#include "xsched/xsched.h"
#include "xsched/cuda/hal.h"

__global__ void BrkptK(volatile int *out)
{
    out[0] = 0x1111;
    asm volatile("brkpt;");
    out[1] = 0x2222;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    int off = (argc > 1 && strcmp(argv[1], "none") != 0) ? (int)strtoul(argv[1], nullptr, 0) : -1;

    cudaFree(0);
    volatile int *out;
    cudaMallocManaged((void**)&out, 8 * sizeof(int));
    out[0] = out[1] = 0;
    volatile int *flag;
    cudaMallocManaged((void**)&flag, sizeof(int));
    *flag = 0;

    if (off >= 0) {
        char ea[64], eo[32];
        snprintf(ea, sizeof ea, "0x%llx", (unsigned long long)flag);
        snprintf(eo, sizeof eo, "0x%x", off);
        setenv("XSCHED_SM120_TRAP_TEST_ADDR", ea, 1);
        setenv("XSCHED_SM120_TRAP_TEST_OFFSET", eo, 1);
        printf("[mini9] payload at offset %#x -> flag %p\n", off, (void*)flag);
    } else {
        printf("[mini9] baseline (no patch)\n");
    }

    // create a level-3 XQueue (patches the handler when enabled)
    cudaStream_t qstream; cudaStreamCreate(&qstream);
    XQueueHandle xq; HwQueueHandle hwq;
    CudaQueueCreate(&hwq, qstream);
    XQueueCreate(&xq, hwq, 3, kQueueCreateFlagNone);

    // launch the breakpoint kernel directly (not through the XQueue)
    cudaStream_t s2; cudaStreamCreate(&s2);
    BrkptK<<<1, 32, 0, s2>>>(out);
    printf("[mini9] brkpt kernel launched, err=%d\n", (int)cudaGetLastError());

    std::atomic<bool> done{false};
    std::thread watchdog([&]() {
        for (int i = 0; i < 16; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (done.load()) return;
        }
        printf("[WATCHDOG] sync stuck; out=%#x,%#x flag=%#x\n", out[0], out[1], *flag);
        _exit(77);
    });

    cudaError_t e = cudaStreamSynchronize(s2);
    done.store(true);
    printf("[mini9] sync err=%d (%s)\n", (int)e, cudaGetErrorName(e));
    printf("[mini9] out=%#x,%#x flag=%#x (0x5AA5 => handler payload RAN)\n",
           out[0], out[1], *flag);
    watchdog.join();
    return 0;
}
