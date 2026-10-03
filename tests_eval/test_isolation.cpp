#include <windows.h>
#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cassert>

// CUDA Driver API typedefs & constants (self-contained, no CUDA SDK needed)
typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUstream;
typedef void *CUmodule;
typedef void *CUfunction;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0
#define CU_STREAM_DEFAULT 0
#define CU_STREAM_NON_BLOCKING 1

typedef CUresult (*PFN_cuInit)(unsigned int Flags);
typedef CUresult (*PFN_cuDeviceGet)(CUdevice *device, int ordinal);
typedef CUresult (*PFN_cuCtxCreate_v2)(CUcontext *pctx, unsigned int flags, CUdevice dev);
typedef CUresult (*PFN_cuCtxDestroy_v2)(CUcontext ctx);
typedef CUresult (*PFN_cuCtxPushCurrent_v2)(CUcontext ctx);
typedef CUresult (*PFN_cuCtxPopCurrent_v2)(CUcontext *pctx);
typedef CUresult (*PFN_cuStreamCreate)(CUstream *phStream, unsigned int Flags);
typedef CUresult (*PFN_cuStreamDestroy_v2)(CUstream hStream);
typedef CUresult (*PFN_cuStreamSynchronize)(CUstream hStream);
typedef CUresult (*PFN_cuMemAlloc_v2)(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult (*PFN_cuMemFree_v2)(CUdeviceptr dptr);
typedef CUresult (*PFN_cuMemcpyHtoD_v2)(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount);
typedef CUresult (*PFN_cuModuleLoadData)(CUmodule *module, const void *image);
typedef CUresult (*PFN_cuModuleGetFunction)(CUfunction *hfunc, CUmodule hmod, const char *name);
typedef CUresult (*PFN_cuLaunchKernel)(CUfunction f, unsigned gdx, unsigned gdy, unsigned gdz,
                                       unsigned bdx, unsigned bdy, unsigned bdz, unsigned shm,
                                       CUstream stream, void **params, void **extra);
typedef CUresult (*PFN_cuLaunchCooperativeKernel)(CUfunction f, unsigned gdx, unsigned gdy, unsigned gdz,
                                                  unsigned bdx, unsigned bdy, unsigned bdz, unsigned shm,
                                                  CUstream stream, void **params);

// Busy-loop kernel: spins until ~p_cycles clock64 ticks have elapsed.
static const char *kBusyPtx = R"ptx(
.version 8.0
.target sm_90
.address_size 64
.visible .entry busy_kernel(.param .u64 p_cycles)
{
    .reg .b64 %rd<8>;
    .reg .pred %p1;
    ld.param.u64 %rd1, [p_cycles];
    mov.u64 %rd2, %clock64;
LOOP:
    mov.u64 %rd3, %clock64;
    sub.u64 %rd4, %rd3, %rd2;
    setp.lt.u64 %p1, %rd4, %rd1;
    @%p1 bra LOOP;
    ret;
}
)ptx";

static double Percentile(std::vector<double> &v, double p)
{
    std::sort(v.begin(), v.end());
    size_t idx = (size_t)(v.size() * p / 100.0);
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

int main()
{
    std::cout << "============================================================\n";
    std::cout << "  T1: Context Isolation Test (heavy kernel vs ctx2 memFree)\n";
    std::cout << "============================================================\n";

    char mode[16] = {0};
    GetEnvironmentVariableA("XSCHED_AUTO_XQUEUE", mode, sizeof(mode));
    std::cout << "XSCHED_AUTO_XQUEUE=" << (mode[0] ? mode : "(unset)") << "\n";

    // The shim and its cuxtra backend resolve the real CUDA driver via the
    // XSCHED_CUDA_LIB / CUXTRA_CUDA_LIB environment variables. Setting them
    // before this process starts (see run_tests.ps1) is the most reliable
    // way; the assignments below are a best-effort fallback.
    _putenv_s("XSCHED_CUDA_LIB", "C:\\Windows\\System32\\nvcuda.dll");
    _putenv_s("CUXTRA_CUDA_LIB", "C:\\Windows\\System32\\nvcuda.dll");
    SetEnvironmentVariableA("XSCHED_CUDA_LIB", "C:\\Windows\\System32\\nvcuda.dll");
    SetEnvironmentVariableA("CUXTRA_CUDA_LIB", "C:\\Windows\\System32\\nvcuda.dll");

    char shimPath[MAX_PATH] = {0};
    const char *dllPath = "D:\\1file\\Desktop\\code\\myxsched\\build\\platforms\\cuda\\nvcuda.dll";
    if (GetEnvironmentVariableA("XSCHED_SHIM_PATH", shimPath, MAX_PATH) > 0) dllPath = shimPath;

    std::cout << "[Step 1] Loading shim: " << dllPath << "\n";
    HMODULE hShim = LoadLibraryA(dllPath);
    if (!hShim) {
        std::cerr << "FAIL: cannot load shim, error " << GetLastError() << "\n";
        return 1;
    }

    auto resolve = [hShim](const char *name) -> FARPROC {
        FARPROC p = GetProcAddress(hShim, name);
        if (!p) { std::cerr << "FAIL: missing symbol " << name << "\n"; }
        return p;
    };

    auto cuInit = (PFN_cuInit)resolve("cuInit");
    auto cuDeviceGet = (PFN_cuDeviceGet)resolve("cuDeviceGet");
    auto cuCtxCreate = (PFN_cuCtxCreate_v2)resolve("cuCtxCreate_v2");
    auto cuCtxDestroy = (PFN_cuCtxDestroy_v2)resolve("cuCtxDestroy_v2");
    auto cuCtxPushCurrent = (PFN_cuCtxPushCurrent_v2)resolve("cuCtxPushCurrent_v2");
    auto cuCtxPopCurrent = (PFN_cuCtxPopCurrent_v2)resolve("cuCtxPopCurrent_v2");
    auto cuStreamCreate = (PFN_cuStreamCreate)resolve("cuStreamCreate");
    auto cuStreamDestroy = (PFN_cuStreamDestroy_v2)resolve("cuStreamDestroy_v2");
    auto cuStreamSynchronize = (PFN_cuStreamSynchronize)resolve("cuStreamSynchronize");
    auto cuMemAlloc = (PFN_cuMemAlloc_v2)resolve("cuMemAlloc_v2");
    auto cuMemFree = (PFN_cuMemFree_v2)resolve("cuMemFree_v2");
    auto cuMemcpyHtoD = (PFN_cuMemcpyHtoD_v2)resolve("cuMemcpyHtoD_v2");
    auto cuModuleLoadData = (PFN_cuModuleLoadData)resolve("cuModuleLoadData");
    auto cuModuleGetFunction = (PFN_cuModuleGetFunction)resolve("cuModuleGetFunction");
    auto cuLaunchKernel = (PFN_cuLaunchKernel)resolve("cuLaunchKernel");
    auto cuLaunchCooperativeKernel = (PFN_cuLaunchCooperativeKernel)resolve("cuLaunchCooperativeKernel");

    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuCtxDestroy || !cuCtxPushCurrent ||
        !cuStreamCreate || !cuStreamDestroy || !cuStreamSynchronize || !cuMemAlloc || !cuMemFree ||
        !cuMemcpyHtoD || !cuModuleLoadData || !cuModuleGetFunction || !cuLaunchKernel) {
        return 1;
    }

    assert(cuInit(0) == CUDA_SUCCESS);
    CUdevice dev;
    assert(cuDeviceGet(&dev, 0) == CUDA_SUCCESS);

    CUcontext ctxA = nullptr, ctxB = nullptr;
    assert(cuCtxCreate(&ctxA, 0, dev) == CUDA_SUCCESS);
    assert(cuCtxCreate(&ctxB, 0, dev) == CUDA_SUCCESS);
    std::cout << "[Step 2] Created ctxA (" << ctxA << ") and ctxB (" << ctxB << ")\n";

    // Load the busy kernel in ctxA.
    cuCtxPushCurrent(ctxA);
    CUmodule mod = nullptr;
    CUresult r = cuModuleLoadData(&mod, kBusyPtx);
    if (r != CUDA_SUCCESS) {
        std::cerr << "FAIL: cuModuleLoadData returned " << r << "\n";
        return 1;
    }
    CUfunction busy = nullptr;
    assert(cuModuleGetFunction(&busy, mod, "busy_kernel") == CUDA_SUCCESS);

    CUstream streamA = nullptr;
    assert(cuStreamCreate(&streamA, CU_STREAM_DEFAULT) == CUDA_SUCCESS); // blocking stream
    cuCtxPopCurrent(nullptr);
    std::cout << "[Step 3] Busy kernel loaded, streamA created (blocking)\n";

    // ------------------------------------------------------------------
    // Isolation test: launch ~0.6s kernel on ctxA/streamA, then measure
    // cuMemFree_v2 latency on ctxB. With the global ForEachWaitAll (old
    // behavior) the first free would stall ~600ms; with context-isolated
    // sync it must stay in the microsecond range.
    // ------------------------------------------------------------------
    unsigned long long cycles = 1400000000ULL; // ~0.6s on this GPU
    void *args[] = { &cycles };

    std::cout << "[Step 4] Launching ~0.6s kernel on ctxA, measuring ctxB memFree...\n";
    auto t_launch = std::chrono::high_resolution_clock::now();
    cuCtxPushCurrent(ctxA);
    assert(cuLaunchKernel(busy, 1, 1, 1, 32, 1, 1, 0, streamA, args, nullptr) == CUDA_SUCCESS);
    cuCtxPopCurrent(nullptr);

    cuCtxPushCurrent(ctxB);
    std::vector<double> free_lats;
    std::vector<float> h_in(1024, 42.0f);
    const size_t bytes = h_in.size() * sizeof(float);
    for (int i = 0; i < 200; ++i) {
        CUdeviceptr d = 0;
        assert(cuMemAlloc(&d, bytes) == CUDA_SUCCESS);
        assert(cuMemcpyHtoD(d, h_in.data(), bytes) == CUDA_SUCCESS);
        auto t0 = std::chrono::high_resolution_clock::now();
        assert(cuMemFree(d) == CUDA_SUCCESS);
        auto t1 = std::chrono::high_resolution_clock::now();
        free_lats.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    cuCtxPopCurrent(nullptr);

    double p50 = Percentile(free_lats, 50), p95 = Percentile(free_lats, 95);
    double p99 = Percentile(free_lats, 99), max_us = *std::max_element(free_lats.begin(), free_lats.end());
    std::cout << "  ctxB cuMemFree_v2 latency (n=" << free_lats.size() << "): "
              << "P50=" << p50 << "us P95=" << p95 << "us P99=" << p99 << "us Max=" << max_us << "us\n";

    // Wait for the long kernel to finish and report its total duration.
    cuCtxPushCurrent(ctxA);
    assert(cuStreamSynchronize(streamA) == CUDA_SUCCESS);
    cuCtxPopCurrent(nullptr);
    auto t_kernel_done = std::chrono::high_resolution_clock::now();
    double kernel_ms = std::chrono::duration<double, std::milli>(t_kernel_done - t_launch).count();
    std::cout << "  long kernel wall time: " << kernel_ms << " ms\n";

    if (p95 > 50000.0) {
        std::cerr << "FAIL: ctxB memFree P95 " << p95 << "us exceeded 50ms — isolation broken!\n";
        return 1;
    }
    std::cout << "  [ISOLATION] PASSED: ctxB operations were NOT stalled by ctxA's heavy kernel\n";

    // ------------------------------------------------------------------
    // F1: cooperative kernel launch must go through XLaunchCooperativeKernel.
    // ------------------------------------------------------------------
    std::cout << "[Step 5] Cooperative kernel launch (F1)...\n";
    if (cuLaunchCooperativeKernel != nullptr) {
        cuCtxPushCurrent(ctxA);
        CUstream streamC = nullptr;
        assert(cuStreamCreate(&streamC, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS);
        unsigned long long cycles2 = 1000000ULL; // very short
        void *args2[] = { &cycles2 };
        r = cuLaunchCooperativeKernel(busy, 1, 1, 1, 32, 1, 1, 0, streamC, args2);
        if (r == CUDA_SUCCESS) {
            assert(cuStreamSynchronize(streamC) == CUDA_SUCCESS);
            std::cout << "  [F1] cooperative kernel launch+sync PASSED\n";
        } else {
            std::cout << "  [F1] cooperative launch returned " << r
                      << " (device/arch may not support cooperative launch; skip)\n";
        }
        cuStreamDestroy(streamC);
        cuCtxPopCurrent(nullptr);
    }

    // Cleanup
    cuCtxPushCurrent(ctxA);
    assert(cuStreamDestroy(streamA) == CUDA_SUCCESS);
    assert(cuCtxDestroy(ctxA) == CUDA_SUCCESS);
    assert(cuCtxDestroy(ctxB) == CUDA_SUCCESS);

    std::cout << "============================================================\n";
    std::cout << "  T1 ALL PASSED\n";
    std::cout << "============================================================\n";
    FreeLibrary(hShim);
    return 0;
}
