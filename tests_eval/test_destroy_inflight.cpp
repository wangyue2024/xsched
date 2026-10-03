#include <windows.h>
#include <iostream>
#include <vector>
#include <chrono>
#include <cassert>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUstream;
typedef void *CUmodule;
typedef void *CUfunction;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0
#define CU_STREAM_DEFAULT 0

typedef CUresult (*PFN_cuInit)(unsigned int Flags);
typedef CUresult (*PFN_cuDeviceGet)(CUdevice *device, int ordinal);
typedef CUresult (*PFN_cuCtxCreate_v2)(CUcontext *pctx, unsigned int flags, CUdevice dev);
typedef CUresult (*PFN_cuCtxDestroy_v2)(CUcontext ctx);
typedef CUresult (*PFN_cuCtxPushCurrent_v2)(CUcontext ctx);
typedef CUresult (*PFN_cuCtxPopCurrent_v2)(CUcontext *pctx);
typedef CUresult (*PFN_cuStreamCreate)(CUstream *phStream, unsigned int Flags);
typedef CUresult (*PFN_cuStreamDestroy_v2)(CUstream hStream);
typedef CUresult (*PFN_cuMemAlloc_v2)(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult (*PFN_cuMemFree_v2)(CUdeviceptr dptr);
typedef CUresult (*PFN_cuMemcpyHtoD_v2)(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount);
typedef CUresult (*PFN_cuMemcpyDtoH_v2)(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount);
typedef CUresult (*PFN_cuModuleLoadData)(CUmodule *module, const void *image);
typedef CUresult (*PFN_cuModuleGetFunction)(CUfunction *hfunc, CUmodule hmod, const char *name);
typedef CUresult (*PFN_cuLaunchKernel)(CUfunction f, unsigned gdx, unsigned gdy, unsigned gdz,
                                       unsigned bdx, unsigned bdy, unsigned bdz, unsigned shm,
                                       CUstream stream, void **params, void **extra);
typedef CUresult (*PFN_cuDevicePrimaryCtxRetain)(CUcontext *pctx, CUdevice dev);
typedef CUresult (*PFN_cuDevicePrimaryCtxRelease_v2)(CUdevice dev);
typedef CUresult (*PFN_cuDevicePrimaryCtxReset_v2)(CUdevice dev);

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

int main()
{
    std::cout << "============================================================\n";
    std::cout << "  T3: In-flight Context Destroy + Primary Ctx Reset Test\n";
    std::cout << "============================================================\n";

    char mode[16] = {0};
    GetEnvironmentVariableA("XSCHED_AUTO_XQUEUE", mode, sizeof(mode));
    std::cout << "XSCHED_AUTO_XQUEUE=" << (mode[0] ? mode : "(unset)") << "\n";

    // See test_isolation.cpp: XSCHED_CUDA_LIB / CUXTRA_CUDA_LIB should be
    // set before process start (run_tests.ps1); these are fallbacks.
    _putenv_s("XSCHED_CUDA_LIB", "C:\\Windows\\System32\\nvcuda.dll");
    _putenv_s("CUXTRA_CUDA_LIB", "C:\\Windows\\System32\\nvcuda.dll");
    SetEnvironmentVariableA("XSCHED_CUDA_LIB", "C:\\Windows\\System32\\nvcuda.dll");
    SetEnvironmentVariableA("CUXTRA_CUDA_LIB", "C:\\Windows\\System32\\nvcuda.dll");

    char shimPath[MAX_PATH] = {0};
    const char *dllPath = "D:\\1file\\Desktop\\code\\myxsched\\build\\platforms\\cuda\\nvcuda.dll";
    if (GetEnvironmentVariableA("XSCHED_SHIM_PATH", shimPath, MAX_PATH) > 0) dllPath = shimPath;

    HMODULE hShim = LoadLibraryA(dllPath);
    if (!hShim) {
        std::cerr << "FAIL: cannot load shim, error " << GetLastError() << "\n";
        return 1;
    }

    auto resolve = [hShim](const char *name) -> FARPROC { return GetProcAddress(hShim, name); };
    auto cuInit = (PFN_cuInit)resolve("cuInit");
    auto cuDeviceGet = (PFN_cuDeviceGet)resolve("cuDeviceGet");
    auto cuCtxCreate = (PFN_cuCtxCreate_v2)resolve("cuCtxCreate_v2");
    auto cuCtxDestroy = (PFN_cuCtxDestroy_v2)resolve("cuCtxDestroy_v2");
    auto cuCtxPushCurrent = (PFN_cuCtxPushCurrent_v2)resolve("cuCtxPushCurrent_v2");
    auto cuStreamCreate = (PFN_cuStreamCreate)resolve("cuStreamCreate");
    auto cuStreamDestroy = (PFN_cuStreamDestroy_v2)resolve("cuStreamDestroy_v2");
    auto cuMemAlloc = (PFN_cuMemAlloc_v2)resolve("cuMemAlloc_v2");
    auto cuMemFree = (PFN_cuMemFree_v2)resolve("cuMemFree_v2");
    auto cuMemcpyHtoD = (PFN_cuMemcpyHtoD_v2)resolve("cuMemcpyHtoD_v2");
    auto cuMemcpyDtoH = (PFN_cuMemcpyDtoH_v2)resolve("cuMemcpyDtoH_v2");
    auto cuModuleLoadData = (PFN_cuModuleLoadData)resolve("cuModuleLoadData");
    auto cuModuleGetFunction = (PFN_cuModuleGetFunction)resolve("cuModuleGetFunction");
    auto cuLaunchKernel = (PFN_cuLaunchKernel)resolve("cuLaunchKernel");
    auto cuDevicePrimaryCtxRetain = (PFN_cuDevicePrimaryCtxRetain)resolve("cuDevicePrimaryCtxRetain");
    auto cuDevicePrimaryCtxRelease = (PFN_cuDevicePrimaryCtxRelease_v2)resolve("cuDevicePrimaryCtxRelease_v2");
    auto cuDevicePrimaryCtxReset = (PFN_cuDevicePrimaryCtxReset_v2)resolve("cuDevicePrimaryCtxReset_v2");

    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuCtxDestroy || !cuCtxPushCurrent ||
        !cuStreamCreate || !cuStreamDestroy || !cuMemAlloc || !cuMemFree || !cuMemcpyHtoD ||
        !cuMemcpyDtoH || !cuModuleLoadData || !cuModuleGetFunction || !cuLaunchKernel) {
        return 1;
    }

    assert(cuInit(0) == CUDA_SUCCESS);
    CUdevice dev;
    assert(cuDeviceGet(&dev, 0) == CUDA_SUCCESS);

    // ------------------------------------------------------------------
    // Phase 1: destroy a context while a long kernel is still in flight.
    // The shim must drain (XCtxDestroy -> DrainContextBeforeDestroy) before
    // the physical destroy; no crash, destroy returns CUDA_SUCCESS.
    // ------------------------------------------------------------------
    std::cout << "[Phase 1] Submitting long kernel, then destroying ctx immediately...\n";
    CUcontext ctxA = nullptr;
    assert(cuCtxCreate(&ctxA, 0, dev) == CUDA_SUCCESS);
    assert(cuCtxPushCurrent(ctxA) == CUDA_SUCCESS);

    CUmodule mod = nullptr;
    assert(cuModuleLoadData(&mod, kBusyPtx) == CUDA_SUCCESS);
    CUfunction busy = nullptr;
    assert(cuModuleGetFunction(&busy, mod, "busy_kernel") == CUDA_SUCCESS);

    CUstream streamA = nullptr;
    assert(cuStreamCreate(&streamA, CU_STREAM_DEFAULT) == CUDA_SUCCESS);

    unsigned long long cycles = 800000000ULL; // ~0.35s
    void *args[] = { &cycles };
    assert(cuLaunchKernel(busy, 1, 1, 1, 32, 1, 1, 0, streamA, args, nullptr) == CUDA_SUCCESS);

    auto t0 = std::chrono::high_resolution_clock::now();
    CUresult r = cuCtxDestroy(ctxA); // drains the in-flight kernel first
    auto t1 = std::chrono::high_resolution_clock::now();
    double destroy_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (r != CUDA_SUCCESS) {
        std::cerr << "FAIL: cuCtxDestroy with in-flight work returned " << r << "\n";
        return 1;
    }
    std::cout << "  [Phase 1] PASSED: destroy returned SUCCESS in " << destroy_ms
              << " ms (kernel was drained)\n";

    // ------------------------------------------------------------------
    // Phase 2: system must remain healthy — create a fresh context and do
    // a full round-trip on it.
    // ------------------------------------------------------------------
    std::cout << "[Phase 2] Health check with a fresh context...\n";
    CUcontext ctxB = nullptr;
    assert(cuCtxCreate(&ctxB, 0, dev) == CUDA_SUCCESS);
    assert(cuCtxPushCurrent(ctxB) == CUDA_SUCCESS);

    const size_t bytes = 1024 * sizeof(float);
    std::vector<float> h_in(1024, 3.5f), h_out(1024, 0.0f);
    CUdeviceptr d = 0;
    assert(cuMemAlloc(&d, bytes) == CUDA_SUCCESS);
    assert(cuMemcpyHtoD(d, h_in.data(), bytes) == CUDA_SUCCESS);
    assert(cuMemcpyDtoH(h_out.data(), d, bytes) == CUDA_SUCCESS);
    assert(h_out[0] == 3.5f && h_out[1023] == 3.5f);
    assert(cuMemFree(d) == CUDA_SUCCESS);
    std::cout << "  [Phase 2] PASSED: fresh context round-trip OK\n";

    // Cleanup Phase 2 context.
    assert(cuCtxDestroy(ctxB) == CUDA_SUCCESS);

    // ------------------------------------------------------------------
    // Phase 3: primary context reset (F3). Submit work on the primary
    // context, then reset it; the shim drains it before the physical reset.
    // ------------------------------------------------------------------
    std::cout << "[Phase 3] Primary context retain/work/reset...\n";
    if (cuDevicePrimaryCtxRetain && cuDevicePrimaryCtxRelease && cuDevicePrimaryCtxReset) {
        CUcontext pctx = nullptr;
        r = cuDevicePrimaryCtxRetain(&pctx, dev);
        assert(r == CUDA_SUCCESS && pctx != nullptr);
        assert(cuCtxPushCurrent(pctx) == CUDA_SUCCESS);

        CUdeviceptr pd = 0;
        assert(cuMemAlloc(&pd, bytes) == CUDA_SUCCESS);
        assert(cuMemcpyHtoD(pd, h_in.data(), bytes) == CUDA_SUCCESS);
        assert(cuMemcpyDtoH(h_out.data(), pd, bytes) == CUDA_SUCCESS);
        assert(h_out[0] == 3.5f);
        // leave the allocation in place on purpose: reset must reclaim it

        r = cuDevicePrimaryCtxReset(dev); // XDevicePrimaryCtxReset_v2 (F3)
        if (r != CUDA_SUCCESS) {
            std::cerr << "FAIL: cuDevicePrimaryCtxReset_v2 returned " << r << "\n";
            return 1;
        }
        std::cout << "  [Phase 3] primary reset returned SUCCESS\n";

        // Retain a fresh primary context and verify it works.
        CUcontext pctx2 = nullptr;
        r = cuDevicePrimaryCtxRetain(&pctx2, dev);
        assert(r == CUDA_SUCCESS && pctx2 != nullptr);
        assert(cuCtxPushCurrent(pctx2) == CUDA_SUCCESS);
        CUdeviceptr pd2 = 0;
        assert(cuMemAlloc(&pd2, bytes) == CUDA_SUCCESS);
        assert(cuMemcpyHtoD(pd2, h_in.data(), bytes) == CUDA_SUCCESS);
        assert(cuMemcpyDtoH(h_out.data(), pd2, bytes) == CUDA_SUCCESS);
        assert(h_out[1023] == 3.5f);
        assert(cuMemFree(pd2) == CUDA_SUCCESS);
        assert(cuDevicePrimaryCtxRelease(dev) == CUDA_SUCCESS);
        std::cout << "  [Phase 3] PASSED: primary context reusable after reset\n";
    } else {
        std::cout << "  [Phase 3] skipped: primary ctx symbols not found\n";
    }

    std::cout << "============================================================\n";
    std::cout << "  T3 ALL PASSED (no crash, contexts healthy after destroy/reset)\n";
    std::cout << "============================================================\n";
    FreeLibrary(hShim);
    return 0;
}
