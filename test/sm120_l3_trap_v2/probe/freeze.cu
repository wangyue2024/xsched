// freeze.cu - can we suspend and resume a *running* kernel through the driver's
// own CILP preemption buffer?
//
// Mechanism under test (decoded from the live sm120 trap handler, see
// FINDINGS_ZH.md v0.1):
//     do_read_trap_reason reads [CUDA_PREEMPTION_BUFFER_PATCH_ADDR + 4]
//         == 2 -> reason |= PREEMPT_SAVE|PAUSE_QUIET  (CILP save, then spin)
//         == 1 -> reason |= PREEMPT_RESTORE|PAUSE_QUIET
//         == 0 -> no reason -> handler returns to the trapped PC
// The host cannot write that word while a kernel runs (cuXtraMemcpyHtoD blocked
// 17.8 s until the kernel finished), but *device code can* (dev_access), and
// cuXtraTriggerTrap is non-blocking.  So: a helper kernel arms the command word,
// the host fires the RM trap, and the victim kernel should freeze inside the
// handler until the word is cleared.
//
// Phases isolate whether the trap trigger is needed at all:
//   A arm(2), no trigger   B trigger   C clear(0), no trigger   D trigger
//
// Observation is out-of-band: the victim publishes its iteration counter into
// mapped host memory, so the host never blocks on a CUDA call to see a freeze.
//
// Build: ../build.sh freeze    Run:
//   CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1 \
//   ./freeze [--blocks 8] [--spin-ms 20000] [--no-trigger]

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

__global__ void spin_publish(volatile unsigned long long *hc, unsigned long long cycles)
{
    unsigned long long start = clock64();
    unsigned long long i = 0;
    while (clock64() - start < cycles) {
        i++;
        if ((i & 0xff) == 0) *hc = i;
    }
    *hc = i | (1ULL << 63);
}

__global__ void set_cmd(volatile uint32_t *cmd, unsigned int val)
{
    *cmd = val;
    __threadfence_system();
}

static double now_s()
{
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    long blocks = 8, spin_ms = 20000;
    bool trigger = true, arm_only = false;
    std::string dbgflags;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--blocks") blocks = atol(argv[++i]);
        else if (a == "--spin-ms") spin_ms = atol(argv[++i]);
        else if (a == "--no-trigger") trigger = false;
        else if (a == "--arm-only") arm_only = true;
        else if (a == "--dbgflags") dbgflags = argv[++i];
    }

    // libcuda exports its debugger state as data symbols; setting them before
    // the context is created is the cheapest way to ask for the preemption /
    // attach code paths to be enabled.
    if (!dbgflags.empty()) {
        size_t pos = 0;
        while (pos < dbgflags.size()) {
            size_t comma = dbgflags.find(',', pos);
            std::string name = dbgflags.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? dbgflags.size() : comma + 1;
            if (name.empty()) continue;
            void *sym = dlsym(RTLD_DEFAULT, name.c_str());
            if (!sym) { void *h = dlopen("libcuda.so.1", RTLD_NOLOAD | RTLD_GLOBAL); if (h) sym = dlsym(h, name.c_str()); }
            if (!sym) { printf("[dbgflag] %s NOT FOUND\n", name.c_str()); continue; }
            int *iv = (int *)sym;
            printf("[dbgflag] %s @ %p : %d -> 1\n", name.c_str(), sym, *iv);
            *iv = 1;
        }
    }

    RT_CHECK(cudaFree(0));
    CUcontext ctx = nullptr;
    if (cuCtxGetCurrent(&ctx) || !ctx) { fprintf(stderr, "no context\n"); return 1; }
    CUstream opstream;
    if (cuStreamCreate(&opstream, CU_STREAM_NON_BLOCKING)) return 1;

    auto I = sm120arena::probe(ctx, opstream);
    sm120arena::report(I);
    if (!I.ok || !I.preempt_buf) { fprintf(stderr, "preemption buffer unresolved\n"); return 1; }

    // host-visible observation channel
    void *hp = nullptr;
    RT_CHECK(cudaHostAlloc(&hp, 64, cudaHostAllocMapped));
    memset(hp, 0, 64);
    volatile unsigned long long *hc = (volatile unsigned long long *)hp;
    unsigned long long *hc_dev = nullptr;
    RT_CHECK(cudaHostGetDevicePointer((void **)&hc_dev, hp, 0));

    cudaStream_t victim, helper;
    RT_CHECK(cudaStreamCreateWithFlags(&victim, cudaStreamNonBlocking));
    RT_CHECK(cudaStreamCreateWithFlags(&helper, cudaStreamNonBlocking));

    int khz = 0;
    cudaDeviceGetAttribute(&khz, cudaDevAttrClockRate, 0);
    unsigned long long cycles = (unsigned long long)(khz ? khz : 1500000) * 1000ULL * spin_ms / 1000ULL;

    volatile uint32_t *cmd = (volatile uint32_t *)(uintptr_t)(I.preempt_buf + 4);
    printf("[setup] victim: %ld blocks x 32 threads, ~%ld ms; cmd word VA = %p; trigger=%d\n",
           blocks, spin_ms, (void *)cmd, (int)trigger);

    std::atomic<bool> done{false};
    std::thread wd([&]() {
        for (int i = 0; i < 160 && !done.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (done.load()) return;
        printf("[WATCHDOG] 80 s timeout, counter=%llu - exiting (context may stay stuck)\n",
               *hc & ~(1ULL << 63));
        _exit(77);
    });
    wd.detach();

    auto sample = [&](const char *tag) {
        unsigned long long v = *hc;
        printf("[t=%7.3f] %-26s counter=%-12llu %s\n", now_s(), tag, v & ~(1ULL << 63),
               (v >> 63) ? "(COMPLETED)" : "");
        return v & ~(1ULL << 63);
    };
    auto arm = [&](unsigned val, const char *tag) {
        double t0 = now_s();
        set_cmd<<<1, 1, 0, helper>>>((volatile uint32_t *)(uintptr_t)cmd, val);
        cudaError_t e = cudaStreamSynchronize(helper);
        printf("[t=%7.3f] arm(%u) %s -> %s (%.3f s)\n", now_s(), val, tag,
               cudaGetErrorName(e), now_s() - t0);
    };
    auto fire = [&](const char *tag) {
        if (!trigger) { printf("[t=%7.3f] trigger skipped (%s)\n", now_s(), tag); return; }
        double t0 = now_s();
        cuXtraTriggerTrap(ctx);
        printf("[t=%7.3f] TRIGGER %s (%.4f s)\n", now_s(), tag, now_s() - t0);
    };

    // Warm up every kernel module first: CUDA's lazy module loading serializes
    // a helper kernel's *first* launch behind whatever is already running
    // (measured: 17.09 s in freeze run 1, 2.42 s in concurrency test 1).
    printf("\n--- warmup (avoid lazy module load during the experiment) ---\n");
    set_cmd<<<1, 1, 0, helper>>>((volatile uint32_t *)(uintptr_t)cmd, 0u);
    RT_CHECK(cudaStreamSynchronize(helper));
    spin_publish<<<1, 32, 0, helper>>>(hc_dev, 1000);
    RT_CHECK(cudaStreamSynchronize(helper));
    *hc = 0;
    printf("[warmup] done\n");

    printf("\n--- launch victim ---\n");
    spin_publish<<<(int)blocks, 32, 0, victim>>>(hc_dev, cycles);
    RT_CHECK(cudaGetLastError());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    unsigned long long a0 = sample("running (baseline)");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    unsigned long long a1 = sample("running (baseline 2)");
    printf("[baseline] rate = %.1f M iter/s\n", (a1 - a0) / 1e6 / 0.5);

    printf("\n--- phase A: arm cmd=2, no trigger ---\n");
    arm(2, "SAVE|PAUSE");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    unsigned long long b0 = sample("A: armed, no trigger");
    if (arm_only) { printf("[arm-only] stopping here\n"); arm(0, "clear"); done = true; return 0; }

    printf("\n--- phase B: trigger ---\n");
    fire("with cmd=2");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    unsigned long long c0 = sample("B: +0.3s");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    unsigned long long c1 = sample("B: +1.0s");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    unsigned long long c2 = sample("B: +2.0s");

    printf("\n--- phase C: clear cmd=0, no trigger ---\n");
    arm(0, "clear");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    unsigned long long d0 = sample("C: cleared, no trigger");

    printf("\n--- phase D: trigger again ---\n");
    fire("with cmd=0");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    unsigned long long e0 = sample("D: +0.5s");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    unsigned long long e1 = sample("D: +1.5s");

    printf("\n--- wait for victim ---\n");
    double tw = now_s();
    cudaError_t se = cudaStreamSynchronize(victim);
    printf("[t=%7.3f] victim sync=%s after %.3f s\n", now_s(), cudaGetErrorName(se), now_s() - tw);
    unsigned long long f0 = sample("final");
    done = true;

    bool frozen_in_B = (c2 == c1 && c1 == c0);
    bool frozen_in_A = (b0 == a1);
    bool resumed = (e1 > e0) || (f0 > e1);
    printf("\n================ VERDICT ================\n");
    printf("  baseline rate      : %.1f M iter/s\n", (a1 - a0) / 1e6 / 0.5);
    printf("  frozen by arm only : %s\n", frozen_in_A ? "YES" : "no");
    printf("  frozen after trigger: %s\n", frozen_in_B ? "YES  <<< mid-kernel suspend" : "no");
    printf("  resumed after clear : %s\n", resumed ? "YES" : "no");
    printf("  victim total        : %llu iterations (expected ~%llu)\n", f0,
           (unsigned long long)((a1 - a0) * 2.0 * spin_ms / 1000.0));
    printf("=========================================\n");
    return 0;
}
