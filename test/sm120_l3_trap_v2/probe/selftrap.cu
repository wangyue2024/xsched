// selftrap.cu - drive the live trap handler from inside a running kernel.
//
// The host-side RM trigger (cuXtraTriggerTrap -> GlobalRegsWrite32 0x419e84
// bit31) demonstrably does not deliver a trap to running warps on sm120+595
// (freeze.cu: the victim kept counting at 37 M iter/s with the CILP command word
// armed at 2).  What *does* enter the handler is a real trap instruction, so
// this probe lets the victim trap itself while the command word is armed:
//
//   block 0 (victim)    : publish counter; at --arm-at iterations write
//                         [preempt_buf+4] = cmd, fence, publish, execute
//                         brkpt/trap, then publish "returned" and keep spinning
//   block 1 (controller): after --clear-ms, write [preempt_buf+4] = 0 and publish
//   other blocks        : plain spin (controls)
//
// Expected if the decoded mechanism is right: with cmd=2 the victim's reason
// mask becomes PREEMPT_SAVE|PAUSE_QUIET, the handler loops on it, so block 0's
// counter stalls until block 1 clears the word.
//
// Build: ../build.sh selftrap    Run:
//   CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1 \
//   ./selftrap [--cmd 2] [--inst brkpt|trap] [--arm-at 20000000] [--clear-ms 3000]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <dlfcn.h>
#include <cuda_runtime.h>
#include "arena.h"

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)

// hc layout: [0..7] per-block counters, [10] armed iteration, [11] returned-from-trap,
// [12] controller cleared, [13] trap instruction executed count
__global__ void victim_kernel(volatile unsigned long long *hc, volatile unsigned int *cmd,
                              unsigned long long cycles, unsigned long long arm_at,
                              unsigned int cmd_val, unsigned long long clear_cycles,
                              int use_trap_inst)
{
    unsigned bid = blockIdx.x;
    unsigned long long start = clock64(), i = 0;

    if (bid == 1) { // controller: clear the command word after a delay
        unsigned long long t0 = clock64();
        while (clock64() - t0 < clear_cycles) { i++; if ((i & 0xff) == 0) hc[bid] = i; }
        *cmd = 0u;
        __threadfence_system();
        hc[12] = i;
        while (clock64() - start < cycles) { i++; if ((i & 0xff) == 0) hc[bid] = i; }
        hc[bid] = i | (1ULL << 62);
        return;
    }

    while (clock64() - start < cycles) {
        i++;
        if ((i & 0xff) == 0) hc[bid] = i;
        if (bid == 0 && i >= arm_at) {
            *cmd = cmd_val;
            __threadfence_system();
            hc[10] = i;
            hc[13] += 1;
            if (use_trap_inst) asm volatile("trap;");
            else               asm volatile("brkpt;");
            hc[11] = i;   // only reached if the handler returned to us
            arm_at = (unsigned long long)-1; // trap once
        }
    }
    hc[bid] = i | (1ULL << 63);
}

static double now_s()
{
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    long blocks = 8, spin_ms = 15000, clear_ms = 3000;
    unsigned cmd_val = 2;
    unsigned long long arm_at = 20000000;
    std::string inst = "brkpt";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--blocks") blocks = atol(next().c_str());
        else if (a == "--spin-ms") spin_ms = atol(next().c_str());
        else if (a == "--clear-ms") clear_ms = atol(next().c_str());
        else if (a == "--cmd") cmd_val = (unsigned)strtoul(next().c_str(), nullptr, 0);
        else if (a == "--arm-at") arm_at = strtoull(next().c_str(), nullptr, 0);
        else if (a == "--inst") inst = next();
    }
    int use_trap = inst == "trap" ? 1 : 0;

    RT_CHECK(cudaFree(0));
    CUcontext ctx = nullptr;
    if (cuCtxGetCurrent(&ctx) || !ctx) { fprintf(stderr, "no context\n"); return 1; }
    CUstream opstream;
    if (cuStreamCreate(&opstream, CU_STREAM_NON_BLOCKING)) return 1;

    auto I = sm120arena::probe(ctx, opstream);
    sm120arena::report(I);
    if (!I.ok || !I.preempt_buf) { fprintf(stderr, "preemption buffer unresolved\n"); return 1; }

    void *hp = nullptr;
    RT_CHECK(cudaHostAlloc(&hp, 256, cudaHostAllocMapped));
    memset(hp, 0, 256);
    volatile unsigned long long *hc = (volatile unsigned long long *)hp;
    unsigned long long *hc_dev = nullptr;
    RT_CHECK(cudaHostGetDevicePointer((void **)&hc_dev, hp, 0));

    cudaStream_t vs;
    RT_CHECK(cudaStreamCreateWithFlags(&vs, cudaStreamNonBlocking));
    int khz = 0;
    cudaDeviceGetAttribute(&khz, cudaDevAttrClockRate, 0);
    unsigned long long cycles = (unsigned long long)khz * 1000ULL * spin_ms / 1000ULL;
    unsigned long long clear_cycles = (unsigned long long)khz * 1000ULL * clear_ms / 1000ULL;

    printf("[setup] blocks=%ld spin=%ldms clear=%ldms cmd=%u arm_at=%llu inst=%s preempt_buf=%#llx\n",
           blocks, spin_ms, clear_ms, cmd_val, arm_at, inst.c_str(),
           (unsigned long long)I.preempt_buf);

    std::atomic<bool> done{false};
    std::thread wd([&]() {
        for (int i = 0; i < 120 && !done.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (done.load()) return;
        printf("[WATCHDOG] 60 s timeout: blk0=%llu armed=%llu returned=%llu cleared=%llu\n",
               hc[0] & ~(3ULL << 62), hc[10], hc[11], hc[12]);
        _exit(77);
    });
    wd.detach();

    // warmup so the module is resident (lazy loading would serialize the launch)
    victim_kernel<<<2, 32, 0, vs>>>(hc_dev, (volatile unsigned int *)(uintptr_t)(I.preempt_buf + 4),
                                    1000, (unsigned long long)-1, 0, 1000, use_trap);
    RT_CHECK(cudaStreamSynchronize(vs));
    memset(hp, 0, 256);

    printf("\n--- launch ---\n");
    double t0 = now_s();
    victim_kernel<<<(int)blocks, 32, 0, vs>>>(hc_dev, (volatile unsigned int *)(uintptr_t)(I.preempt_buf + 4),
                                              cycles, arm_at, cmd_val, clear_cycles, use_trap);
    RT_CHECK(cudaGetLastError());

    unsigned long long last = 0, frozen_at = 0, resumed_at = 0;
    int stall = 0;
    for (int k = 0; k < 200; k++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        unsigned long long v = hc[0] & ~(3ULL << 62);
        bool completed = (hc[0] >> 63) & 1;
        if (k % 5 == 0 || hc[10] || hc[11] || hc[12])
            printf("[t=%6.2f] blk0=%-12llu blk1=%-12llu armed=%-10llu returned=%-10llu cleared=%-10llu traps=%llu%s\n",
                   now_s() - t0, v, hc[1] & ~(3ULL << 62), hc[10], hc[11], hc[12], hc[13],
                   completed ? "  (COMPLETED)" : "");
        if (hc[10] && !hc[11]) {           // armed and not returned yet
            if (v == last) { stall++; if (stall == 3 && !frozen_at) frozen_at = now_s() - t0; }
            else stall = 0;
        }
        if (frozen_at && v > last && !resumed_at) resumed_at = now_s() - t0;
        last = v;
        if (completed) break;
    }

    printf("\n--- sync ---\n");
    double tw = now_s();
    cudaError_t se = cudaStreamSynchronize(vs);
    printf("[t=%6.2f] sync=%s after %.3f s\n", now_s() - t0, cudaGetErrorName(se), now_s() - tw);
    printf("[final] blk0=%llu armed=%llu returned=%llu cleared=%llu traps=%llu\n",
           hc[0], hc[10], hc[11], hc[12], hc[13]);
    done = true;

    printf("\n================ VERDICT ================\n");
    printf("  trap instruction executed : %s (traps=%llu)\n", hc[13] ? "YES" : "no", hc[13]);
    printf("  handler returned to kernel: %s\n", hc[11] ? "YES" : "NO");
    printf("  victim frozen in handler  : %s%s\n", frozen_at ? "YES" : "no",
           frozen_at ? (resumed_at ? " and RESUMED" : " and never resumed") : "");
    if (frozen_at) printf("  frozen at t=%.2f s, resumed at t=%.2f s (clear at blk1 mark=%llu)\n",
                          frozen_at, resumed_at, hc[12]);
    printf("  stream sync               : %s\n", cudaGetErrorName(se));
    printf("=========================================\n");
    return 0;
}
