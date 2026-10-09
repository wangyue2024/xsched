// mini13: two-context (two-TSG) preemption test for the TSG path.
// ctx1: 64 eternal spin blocks.  ctx2: measure a 300ms spin, with fair
// timeslice vs after SetTimeslice(ctx1,0).  Also verifies GetTimeslice.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <atomic>
#include <dlfcn.h>
#include <cuda.h>
extern "C" {
#include <cuxtra/cuxtra.h>
}
static double now_ms() {
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
static double measure(CUcontext ctx, CUfunction spin, CUstream s, int units)
{
    cuCtxSetCurrent(ctx);
    cuStreamSynchronize(s);
    double t0 = now_ms();
    long v = units;
    void *a[] = { &v };
    CUresult r = cuLaunchKernel(spin, 2,1,1, 64,1,1, 0, s, a, nullptr);
    if (r) printf("[measure] launch rc=%d\n", r);
    cuStreamSynchronize(s);
    return now_ms() - t0;
}
int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    cuInit(0);
    CUdevice dev; cuDeviceGet(&dev, 0);
    CUcontext ctx1, ctx2;
    cuCtxCreate(&ctx1, 0, dev);
    cuCtxCreate(&ctx2, 0, dev);

    // load module in both contexts
    FILE *f = fopen("/tmp/l3work/mk.cubin", "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *img = malloc(sz); fread(img, 1, sz, f); fclose(f);
    CUmodule m1, m2;
    cuCtxSetCurrent(ctx1); cuModuleLoadData(&m1, img);
    cuCtxSetCurrent(ctx2); cuModuleLoadData(&m2, img);
    CUfunction spin1, spin2, wait2;
    cuCtxSetCurrent(ctx1); cuModuleGetFunction(&spin1, m1, "SpinK"); cuModuleGetFunction(&wait2, m1, "WaitFlagK");
    cuCtxSetCurrent(ctx2); cuModuleGetFunction(&spin2, m2, "SpinK");

    CUstream s1, s2;
    cuCtxSetCurrent(ctx1); cuStreamCreate(&s1, 1);
    cuCtxSetCurrent(ctx2); cuStreamCreate(&s2, 1);

    CUdeviceptr flag;
    cuCtxSetCurrent(ctx1); cuMemAlloc(&flag, 4); cuMemsetD32(flag, 0, 1);

    printf("[mini13] ts ctx1=%zu ctx2=%zu\n", cuXtraGetTimeslice(ctx1), cuXtraGetTimeslice(ctx2));
    long u = 300000000; // 300ms spin units
    long w = 10000000000; // 10s
    // baseline: ctx2 alone
    double base = measure(ctx2, spin2, s2, u);
    printf("[mini13] baseline (alone)      : %.1f ms\n", base);

    // launch victim on ctx1 (64 blocks, eternal)
    cuCtxSetCurrent(ctx1);
    void *fa[] = { &flag };
    CUresult vr = cuLaunchKernel(wait2, 2048,1,1, 256,1,1, 0, s1, fa, nullptr);
    CUresult qr = cuStreamQuery(s1);
    printf("[mini13] victim launch rc=%d query=%d (0=done,600=busy)\n", vr, qr);
    (void)w;
    printf("[mini13] victim launched; settling 600ms\n");
    static std::atomic<bool> done_wd{false};
    std::thread wd([]() {
        for (int i = 0; i < 80; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); if (done_wd.load()) return; }
        printf("[WATCHDOG] timed out (40s)\n"); _exit(77);
    });
    wd.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // contended with fair timeslice
    double fair = measure(ctx2, spin2, s2, u);
    printf("[mini13] contended (fair ts)   : %.1f ms\n", fair);

    // suspend victim via timeslice 0
    size_t r0 = cuXtraGetTimeslice(ctx1);
    cuXtraSetTimeslice(ctx1, 0);
    printf("[mini13] SetTimeslice(ctx1,0); readback=%zu us\n", cuXtraGetTimeslice(ctx1));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    double pre = measure(ctx2, spin2, s2, u);
    printf("[mini13] contended (ts1=0)     : %.1f ms\n", pre);

    printf("[RESULT] base=%.1f fair=%.1f preempted=%.1f | speedup_vs_fair=%.2fx\n",
           base, fair, pre, fair / (pre > 0 ? pre : 1));

    // release
    cuCtxSetCurrent(ctx1);
    cuMemsetD32(flag, 1, 1);
    cuStreamSynchronize(s1);
    cuXtraSetTimeslice(ctx1, r0);
    done_wd.store(true);
    printf("[mini13] DONE\n");
    return 0;
}
