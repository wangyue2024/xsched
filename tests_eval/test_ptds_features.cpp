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
typedef void *CUevent;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0
#define CU_STREAM_NON_BLOCKING 1

typedef CUresult (*PFN_cuInit)(unsigned int Flags);
typedef CUresult (*PFN_cuDeviceGet)(CUdevice *device, int ordinal);
typedef CUresult (*PFN_cuCtxCreate_v2)(CUcontext *pctx, unsigned int flags, CUdevice dev);
typedef CUresult (*PFN_cuCtxDestroy_v2)(CUcontext ctx);
typedef CUresult (*PFN_cuCtxPushCurrent_v2)(CUcontext ctx);
typedef CUresult (*PFN_cuCtxSetCurrent)(CUcontext ctx);
typedef CUresult (*PFN_cuStreamCreate)(CUstream *phStream, unsigned int Flags);
typedef CUresult (*PFN_cuStreamDestroy_v2)(CUstream hStream);
typedef CUresult (*PFN_cuMemAlloc_v2)(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult (*PFN_cuMemFree_v2)(CUdeviceptr dptr);
typedef CUresult (*PFN_cuMemcpyHtoD_v2)(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount);
typedef CUresult (*PFN_cuMemcpyDtoH_v2)(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount);
typedef CUresult (*PFN_cuMemcpyHtoD_v2_ptds)(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount);
typedef CUresult (*PFN_cuMemcpyDtoH_v2_ptds)(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount);
typedef CUresult (*PFN_cuMemcpyDtoD_v2_ptds)(CUdeviceptr dstDevice, CUdeviceptr srcDevice, size_t ByteCount);
typedef CUresult (*PFN_cuEventCreate)(CUevent *phEvent, unsigned int Flags);
typedef CUresult (*PFN_cuEventRecord)(CUevent hEvent, CUstream hStream);
typedef CUresult (*PFN_cuEventElapsedTime)(float *pMilliseconds, CUevent hStart, CUevent hEnd);
typedef CUresult (*PFN_cuEventElapsedTime_v2)(float *pMilliseconds, CUevent hStart, CUevent hEnd);
typedef CUresult (*PFN_cuEventDestroy_v2)(CUevent hEvent);

static PFN_cuCtxSetCurrent cuCtxSetCurrent = nullptr;
static PFN_cuMemAlloc_v2 cuMemAlloc = nullptr;
static PFN_cuMemFree_v2 cuMemFree = nullptr;
static PFN_cuMemcpyHtoD_v2_ptds cuMemcpyHtoD_ptds = nullptr;
static PFN_cuMemcpyDtoH_v2_ptds cuMemcpyDtoH_ptds = nullptr;

// Thread body for the F2 test: uses PTDS APIs so that a per-thread default
// stream is created inside the thread, then exits (triggering the PtdsMap
// destructor cleanup path).
static void PtdsUserThread(CUcontext ctx, int tid, std::atomic<int> *errors)
{
    if (cuCtxSetCurrent(ctx) != CUDA_SUCCESS) { (*errors)++; return; }
    const size_t bytes = 1024 * sizeof(float);
    std::vector<float> h_in(1024, (float)(tid + 1)), h_out(1024, 0.0f);
    CUdeviceptr d = 0;
    if (cuMemAlloc(&d, bytes) != CUDA_SUCCESS) { (*errors)++; return; }
    if (cuMemcpyHtoD_ptds(d, h_in.data(), bytes) != CUDA_SUCCESS) { (*errors)++; return; }
    if (cuMemcpyDtoH_ptds(h_out.data(), d, bytes) != CUDA_SUCCESS) { (*errors)++; return; }
    if (h_out[0] != h_in[0]) { (*errors)++; return; }
    if (cuMemFree(d) != CUDA_SUCCESS) { (*errors)++; return; }
}

int main()
{
    std::cout << "============================================================\n";
    std::cout << "  Feature Test: F5 (EventElapsedTime) + F6 (PTDS memcpy)\n";
    std::cout << "                F2 (PTDS thread-exit cleanup) + F7 (gen)\n";
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
    cuCtxSetCurrent = (PFN_cuCtxSetCurrent)resolve("cuCtxSetCurrent");
    auto cuStreamCreate = (PFN_cuStreamCreate)resolve("cuStreamCreate");
    auto cuStreamDestroy = (PFN_cuStreamDestroy_v2)resolve("cuStreamDestroy_v2");
    cuMemAlloc = (PFN_cuMemAlloc_v2)resolve("cuMemAlloc_v2");
    cuMemFree = (PFN_cuMemFree_v2)resolve("cuMemFree_v2");
    auto cuMemcpyHtoD = (PFN_cuMemcpyHtoD_v2)resolve("cuMemcpyHtoD_v2");
    auto cuMemcpyDtoH = (PFN_cuMemcpyDtoH_v2)resolve("cuMemcpyDtoH_v2");
    cuMemcpyHtoD_ptds = (PFN_cuMemcpyHtoD_v2_ptds)resolve("cuMemcpyHtoD_v2_ptds");
    cuMemcpyDtoH_ptds = (PFN_cuMemcpyDtoH_v2_ptds)resolve("cuMemcpyDtoH_v2_ptds");
    auto cuMemcpyDtoD_ptds = (PFN_cuMemcpyDtoD_v2_ptds)resolve("cuMemcpyDtoD_v2_ptds");
    auto cuEventCreate = (PFN_cuEventCreate)resolve("cuEventCreate");
    auto cuEventRecord = (PFN_cuEventRecord)resolve("cuEventRecord");
    auto cuEventElapsedTime = (PFN_cuEventElapsedTime)resolve("cuEventElapsedTime");
    auto cuEventElapsedTime_v2 = (PFN_cuEventElapsedTime_v2)resolve("cuEventElapsedTime_v2");
    auto cuEventDestroy = (PFN_cuEventDestroy_v2)resolve("cuEventDestroy_v2");

    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuCtxDestroy || !cuCtxPushCurrent ||
        !cuCtxSetCurrent || !cuStreamCreate || !cuStreamDestroy || !cuMemAlloc || !cuMemFree ||
        !cuMemcpyHtoD || !cuMemcpyDtoH) {
        std::cerr << "FAIL: missing essential symbols\n";
        return 1;
    }

    assert(cuInit(0) == CUDA_SUCCESS);
    CUdevice dev;
    assert(cuDeviceGet(&dev, 0) == CUDA_SUCCESS);

    CUcontext ctxA = nullptr;
    assert(cuCtxCreate(&ctxA, 0, dev) == CUDA_SUCCESS);
    assert(cuCtxPushCurrent(ctxA) == CUDA_SUCCESS);

    const size_t bytes = 1024 * sizeof(float);
    std::vector<float> h_in(1024, 5.5f), h_out(1024, 0.0f);

    // ------------------------------------------------------------------
    // F6: synchronous memcpy with PTDS variants.
    // ------------------------------------------------------------------
    std::cout << "[F6] PTDS synchronous memcpy...\n";
    if (cuMemcpyHtoD_ptds && cuMemcpyDtoH_ptds && cuMemcpyDtoD_ptds) {
        CUdeviceptr d1 = 0, d2 = 0;
        assert(cuMemAlloc(&d1, bytes) == CUDA_SUCCESS);
        assert(cuMemAlloc(&d2, bytes) == CUDA_SUCCESS);
        CUresult r = cuMemcpyHtoD_ptds(d1, h_in.data(), bytes);
        assert(r == CUDA_SUCCESS);
        r = cuMemcpyDtoD_ptds(d2, d1, bytes);
        assert(r == CUDA_SUCCESS);
        std::fill(h_out.begin(), h_out.end(), 0.0f);
        r = cuMemcpyDtoH_ptds(h_out.data(), d2, bytes);
        assert(r == CUDA_SUCCESS);
        assert(h_out[0] == 5.5f && h_out[1023] == 5.5f);
        assert(cuMemFree(d1) == CUDA_SUCCESS);
        assert(cuMemFree(d2) == CUDA_SUCCESS);
        std::cout << "  [F6] PASSED: ptds HtoD + DtoD + DtoH round-trip verified\n";
    } else {
        std::cout << "  [F6] skipped: ptds symbols not found\n";
    }

    // ------------------------------------------------------------------
    // F5: cuEventElapsedTime (v1 and v2) on events recorded via the shim.
    // ------------------------------------------------------------------
    std::cout << "[F5] cuEventElapsedTime / _v2...\n";
    if (cuEventCreate && cuEventRecord && cuEventElapsedTime && cuEventElapsedTime_v2) {
        CUevent e0 = nullptr, e1 = nullptr;
        assert(cuEventCreate(&e0, 0) == CUDA_SUCCESS);
        assert(cuEventCreate(&e1, 0) == CUDA_SUCCESS);
        CUstream s = nullptr;
        assert(cuStreamCreate(&s, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS);
        assert(cuEventRecord(e0, s) == CUDA_SUCCESS);
        assert(cuEventRecord(e1, s) == CUDA_SUCCESS);
        assert(cuStreamDestroy(s) == CUDA_SUCCESS);

        float ms1 = -1.0f, ms2 = -1.0f;
        CUresult r = cuEventElapsedTime(&ms1, e0, e1);
        assert(r == CUDA_SUCCESS && ms1 >= 0.0f);
        r = cuEventElapsedTime_v2(&ms2, e0, e1);
        assert(r == CUDA_SUCCESS && ms2 >= 0.0f);
        std::cout << "  [F5] PASSED: v1=" << ms1 << " ms, v2=" << ms2 << " ms\n";
        if (cuEventDestroy) {
            assert(cuEventDestroy(e0) == CUDA_SUCCESS);
            assert(cuEventDestroy(e1) == CUDA_SUCCESS);
        }
    } else {
        std::cout << "  [F5] skipped: event symbols not found\n";
    }

    // ------------------------------------------------------------------
    // F2: threads create PTDS streams and exit; the PtdsMap destructor must
    // tear them down cleanly (exercised path, visible in XSCHED debug log).
    // ------------------------------------------------------------------
    std::cout << "[F2] Thread create/exit with PTDS usage...\n";
    {
        std::atomic<int> errors{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back(PtdsUserThread, ctxA, t, &errors);
        }
        for (auto &th : threads) th.join();
        assert(errors.load() == 0);
        // Main thread still healthy after workers exited.
        CUdeviceptr d = 0;
        assert(cuMemAlloc(&d, bytes) == CUDA_SUCCESS);
        assert(cuMemcpyHtoD(d, h_in.data(), bytes) == CUDA_SUCCESS);
        assert(cuMemFree(d) == CUDA_SUCCESS);
        std::cout << "  [F2] PASSED: 4 threads used+exited PTDS, main thread healthy\n";
    }

    // ------------------------------------------------------------------
    // F7: destroy another context (bump destroy generation), then use PTDS
    // again on ctxA — the generation check must revalidate, not crash.
    // ------------------------------------------------------------------
    std::cout << "[F7] PTDS after foreign context destroy...\n";
    {
        CUcontext ctxB = nullptr;
        assert(cuCtxCreate(&ctxB, 0, dev) == CUDA_SUCCESS);
        assert(cuCtxDestroy(ctxB) == CUDA_SUCCESS); // -> DestroyGeneration()++

        CUdeviceptr d = 0;
        assert(cuMemAlloc(&d, bytes) == CUDA_SUCCESS);
        CUresult r = cuMemcpyHtoD_ptds(d, h_in.data(), bytes);
        assert(r == CUDA_SUCCESS);
        std::fill(h_out.begin(), h_out.end(), 0.0f);
        r = cuMemcpyDtoH_ptds(h_out.data(), d, bytes);
        assert(r == CUDA_SUCCESS);
        assert(h_out[0] == 5.5f);
        assert(cuMemFree(d) == CUDA_SUCCESS);
        std::cout << "  [F7] PASSED: PTDS reuse after destroy-generation bump OK\n";
    }

    assert(cuCtxDestroy(ctxA) == CUDA_SUCCESS);

    std::cout << "============================================================\n";
    std::cout << "  Feature tests ALL PASSED\n";
    std::cout << "============================================================\n";
    FreeLibrary(hShim);
    return 0;
}
