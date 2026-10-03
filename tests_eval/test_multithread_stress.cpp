#include <windows.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cassert>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUstream;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0
#define CU_STREAM_NON_BLOCKING 1

typedef CUresult (*PFN_cuInit)(unsigned int Flags);
typedef CUresult (*PFN_cuDeviceGet)(CUdevice *device, int ordinal);
typedef CUresult (*PFN_cuCtxCreate_v2)(CUcontext *pctx, unsigned int flags, CUdevice dev);
typedef CUresult (*PFN_cuCtxDestroy_v2)(CUcontext ctx);
typedef CUresult (*PFN_cuCtxSetCurrent)(CUcontext ctx);
typedef CUresult (*PFN_cuStreamCreate)(CUstream *phStream, unsigned int Flags);
typedef CUresult (*PFN_cuStreamDestroy_v2)(CUstream hStream);
typedef CUresult (*PFN_cuStreamSynchronize)(CUstream hStream);
typedef CUresult (*PFN_cuMemAlloc_v2)(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult (*PFN_cuMemFree_v2)(CUdeviceptr dptr);
typedef CUresult (*PFN_cuMemcpyHtoD_v2)(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount);
typedef CUresult (*PFN_cuMemcpyDtoH_v2)(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount);
typedef CUresult (*PFN_cuMemGetInfo_v2)(size_t *free, size_t *total);

static PFN_cuStreamCreate cuStreamCreate = nullptr;
static PFN_cuStreamDestroy_v2 cuStreamDestroy = nullptr;
static PFN_cuStreamSynchronize cuStreamSynchronize = nullptr;
static PFN_cuMemAlloc_v2 cuMemAlloc = nullptr;
static PFN_cuMemFree_v2 cuMemFree = nullptr;
static PFN_cuMemcpyHtoD_v2 cuMemcpyHtoD = nullptr;
static PFN_cuMemcpyDtoH_v2 cuMemcpyDtoH = nullptr;
static PFN_cuCtxSetCurrent cuCtxSetCurrent = nullptr;

static constexpr int kThreads = 16;
static constexpr int kIters = 100;

static void Worker(CUcontext ctx, int tid, std::atomic<int> *errors, std::atomic<int> *completed)
{
    if (cuCtxSetCurrent(ctx) != CUDA_SUCCESS) {
        (*errors)++;
        return;
    }

    const size_t bytes = 1024 * sizeof(float);
    std::vector<float> h_in(1024, (float)(tid + 100));
    std::vector<float> h_out(1024, 0.0f);

    for (int i = 0; i < kIters; ++i) {
        CUstream s = nullptr;
        if (cuStreamCreate(&s, CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS) { (*errors)++; return; }

        CUdeviceptr d = 0;
        if (cuMemAlloc(&d, bytes) != CUDA_SUCCESS) { (*errors)++; return; }
        if (cuMemcpyHtoD(d, h_in.data(), bytes) != CUDA_SUCCESS) { (*errors)++; return; }
        if (cuMemcpyDtoH(h_out.data(), d, bytes) != CUDA_SUCCESS) { (*errors)++; return; }
        if (h_out[0] != h_in[0] || h_out[1023] != h_in[1023]) {
            std::cerr << "FAIL: data mismatch in thread " << tid << " iter " << i << "\n";
            (*errors)++;
            return;
        }
        if (cuStreamSynchronize(s) != CUDA_SUCCESS) { (*errors)++; return; }
        if (cuStreamDestroy(s) != CUDA_SUCCESS) { (*errors)++; return; }
        if (cuMemFree(d) != CUDA_SUCCESS) { (*errors)++; return; }
        (*completed)++;
    }
}

int main()
{
    std::cout << "============================================================\n";
    std::cout << "  T2: Multi-thread Stress Test (16 threads x " << kIters << " iters)\n";
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
    auto cuMemGetInfo = (PFN_cuMemGetInfo_v2)resolve("cuMemGetInfo_v2");
    cuCtxSetCurrent = (PFN_cuCtxSetCurrent)resolve("cuCtxSetCurrent");
    cuStreamCreate = (PFN_cuStreamCreate)resolve("cuStreamCreate");
    cuStreamDestroy = (PFN_cuStreamDestroy_v2)resolve("cuStreamDestroy_v2");
    cuStreamSynchronize = (PFN_cuStreamSynchronize)resolve("cuStreamSynchronize");
    cuMemAlloc = (PFN_cuMemAlloc_v2)resolve("cuMemAlloc_v2");
    cuMemFree = (PFN_cuMemFree_v2)resolve("cuMemFree_v2");
    cuMemcpyHtoD = (PFN_cuMemcpyHtoD_v2)resolve("cuMemcpyHtoD_v2");
    cuMemcpyDtoH = (PFN_cuMemcpyDtoH_v2)resolve("cuMemcpyDtoH_v2");

    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuCtxDestroy || !cuCtxSetCurrent ||
        !cuStreamCreate || !cuStreamDestroy || !cuStreamSynchronize || !cuMemAlloc ||
        !cuMemFree || !cuMemcpyHtoD || !cuMemcpyDtoH || !cuMemGetInfo) {
        std::cerr << "FAIL: missing symbols\n";
        return 1;
    }

    assert(cuInit(0) == CUDA_SUCCESS);
    CUdevice dev;
    assert(cuDeviceGet(&dev, 0) == CUDA_SUCCESS);

    CUcontext ctx = nullptr;
    assert(cuCtxCreate(&ctx, 0, dev) == CUDA_SUCCESS);
    assert(cuCtxSetCurrent(ctx) == CUDA_SUCCESS);

    size_t free_before = 0, total = 0;
    assert(cuMemGetInfo(&free_before, &total) == CUDA_SUCCESS);
    std::cout << "[Step 1] GPU free memory before: " << (free_before >> 20) << " MB\n";

    std::atomic<int> errors{0};
    std::atomic<int> completed{0};
    std::vector<std::thread> workers;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back(Worker, ctx, t, &errors, &completed);
    }
    for (auto &w : workers) w.join();
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

    std::cout << "[Step 2] " << completed.load() << "/" << (kThreads * kIters)
              << " iterations completed in " << elapsed_s << " s, errors: " << errors.load() << "\n";

    size_t free_after = 0;
    assert(cuMemGetInfo(&free_after, &total) == CUDA_SUCCESS);
    long long leak_mb = ((long long)free_before - (long long)free_after) >> 20;
    std::cout << "[Step 3] GPU free memory after: " << (free_after >> 20) << " MB (delta: "
              << leak_mb << " MB)\n";

    assert(cuCtxDestroy(ctx) == CUDA_SUCCESS);

    if (errors.load() != 0) {
        std::cerr << "FAIL: " << errors.load() << " worker errors\n";
        return 1;
    }
    if (leak_mb > 64) {
        std::cerr << "FAIL: possible leak of " << leak_mb << " MB\n";
        return 1;
    }

    std::cout << "============================================================\n";
    std::cout << "  T2 ALL PASSED (no deadlock, no leak, no crash)\n";
    std::cout << "============================================================\n";
    FreeLibrary(hShim);
    return 0;
}
