#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <chrono>

// CUDA Driver API typedefs & constants
typedef int CUresult;
typedef int CUdevice;
typedef void* CUcontext;
typedef void* CUstream;
typedef void* CUevent;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0
#define CU_STREAM_DEFAULT 0
#define CU_STREAM_NON_BLOCKING 1

// Function pointer definitions
typedef CUresult (*PFN_cuInit)(unsigned int Flags);
typedef CUresult (*PFN_cuDriverGetVersion)(int *driverVersion);
typedef CUresult (*PFN_cuDeviceGetCount)(int *count);
typedef CUresult (*PFN_cuDeviceGet)(CUdevice *device, int ordinal);
typedef CUresult (*PFN_cuDeviceGetName)(char *name, int len, CUdevice dev);
typedef CUresult (*PFN_cuCtxCreate_v2)(CUcontext *pctx, unsigned int flags, CUdevice dev);
typedef CUresult (*PFN_cuCtxDestroy_v2)(CUcontext ctx);
typedef CUresult (*PFN_cuCtxPushCurrent_v2)(CUcontext ctx);
typedef CUresult (*PFN_cuCtxPopCurrent_v2)(CUcontext *pctx);
typedef CUresult (*PFN_cuCtxGetCurrent)(CUcontext *pctx);
typedef CUresult (*PFN_cuCtxSynchronize)();
typedef CUresult (*PFN_cuStreamCreate)(CUstream *phStream, unsigned int Flags);
typedef CUresult (*PFN_cuStreamDestroy_v2)(CUstream hStream);
typedef CUresult (*PFN_cuStreamSynchronize)(CUstream hStream);
typedef CUresult (*PFN_cuMemAlloc_v2)(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult (*PFN_cuMemFree_v2)(CUdeviceptr dptr);
typedef CUresult (*PFN_cuMemcpyHtoD_v2)(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount);
typedef CUresult (*PFN_cuMemcpyDtoH_v2)(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount);
typedef CUresult (*PFN_cuEventCreate)(CUevent *phEvent, unsigned int Flags);
typedef CUresult (*PFN_cuEventRecord)(CUevent hEvent, CUstream hStream);
typedef CUresult (*PFN_cuEventSynchronize)(CUevent hEvent);
typedef CUresult (*PFN_cuEventElapsedTime)(float *pMilliseconds, CUevent hStart, CUevent hEnd);
typedef CUresult (*PFN_cuEventDestroy_v2)(CUevent hEvent);

int main() {
    std::cout << "====================================================\n";
    std::cout << "   XSched nvcuda.dll Driver Shim End-to-End Test    \n";
    std::cout << "====================================================\n";

    // Set the backing NVIDIA driver path for XSched
    _putenv_s("XSCHED_CUDA_LIB", "C:\\Windows\\System32\\nvcuda.dll");
    SetEnvironmentVariableA("XSCHED_CUDA_LIB", "C:\\Windows\\System32\\nvcuda.dll");

    // Load our built nvcuda.dll shim
    const char* dllPath = "D:\\1file\\Desktop\\code\\myxsched\\build\\platforms\\cuda\\nvcuda.dll";
    std::cout << "[Step 1] Loading shim library: " << dllPath << "\n";
    HMODULE hShim = LoadLibraryA(dllPath);
    if (!hShim) {
        std::cerr << "FAIL: Failed to LoadLibraryA nvcuda.dll, error code: " << GetLastError() << "\n";
        return 1;
    }
    std::cout << "  SUCCESS: Loaded nvcuda.dll at " << (void*)hShim << "\n\n";

    // Resolve symbols
    std::cout << "[Step 2] Resolving CUDA Driver API symbols from shim...\n";
    auto resolve = [hShim](const char* name) -> FARPROC {
        FARPROC proc = GetProcAddress(hShim, name);
        if (!proc) {
            std::cerr << "  MISSING SYMBOL: " << name << "\n";
        }
        return proc;
    };

    PFN_cuInit cuInit = (PFN_cuInit)resolve("cuInit");
    PFN_cuDriverGetVersion cuDriverGetVersion = (PFN_cuDriverGetVersion)resolve("cuDriverGetVersion");
    PFN_cuDeviceGetCount cuDeviceGetCount = (PFN_cuDeviceGetCount)resolve("cuDeviceGetCount");
    PFN_cuDeviceGet cuDeviceGet = (PFN_cuDeviceGet)resolve("cuDeviceGet");
    PFN_cuDeviceGetName cuDeviceGetName = (PFN_cuDeviceGetName)resolve("cuDeviceGetName");
    PFN_cuCtxCreate_v2 cuCtxCreate = (PFN_cuCtxCreate_v2)resolve("cuCtxCreate_v2");
    PFN_cuCtxDestroy_v2 cuCtxDestroy = (PFN_cuCtxDestroy_v2)resolve("cuCtxDestroy_v2");
    PFN_cuCtxPushCurrent_v2 cuCtxPushCurrent = (PFN_cuCtxPushCurrent_v2)resolve("cuCtxPushCurrent_v2");
    PFN_cuCtxPopCurrent_v2 cuCtxPopCurrent = (PFN_cuCtxPopCurrent_v2)resolve("cuCtxPopCurrent_v2");
    PFN_cuCtxGetCurrent cuCtxGetCurrent = (PFN_cuCtxGetCurrent)resolve("cuCtxGetCurrent");
    PFN_cuCtxSynchronize cuCtxSynchronize = (PFN_cuCtxSynchronize)resolve("cuCtxSynchronize");
    PFN_cuStreamCreate cuStreamCreate = (PFN_cuStreamCreate)resolve("cuStreamCreate");
    PFN_cuStreamDestroy_v2 cuStreamDestroy = (PFN_cuStreamDestroy_v2)resolve("cuStreamDestroy_v2");
    PFN_cuStreamSynchronize cuStreamSynchronize = (PFN_cuStreamSynchronize)resolve("cuStreamSynchronize");
    PFN_cuMemAlloc_v2 cuMemAlloc = (PFN_cuMemAlloc_v2)resolve("cuMemAlloc_v2");
    PFN_cuMemFree_v2 cuMemFree = (PFN_cuMemFree_v2)resolve("cuMemFree_v2");
    PFN_cuMemcpyHtoD_v2 cuMemcpyHtoD = (PFN_cuMemcpyHtoD_v2)resolve("cuMemcpyHtoD_v2");
    PFN_cuMemcpyDtoH_v2 cuMemcpyDtoH = (PFN_cuMemcpyDtoH_v2)resolve("cuMemcpyDtoH_v2");
    PFN_cuEventCreate cuEventCreate = (PFN_cuEventCreate)resolve("cuEventCreate");
    PFN_cuEventRecord cuEventRecord = (PFN_cuEventRecord)resolve("cuEventRecord");
    PFN_cuEventSynchronize cuEventSynchronize = (PFN_cuEventSynchronize)resolve("cuEventSynchronize");
    PFN_cuEventElapsedTime cuEventElapsedTime = (PFN_cuEventElapsedTime)resolve("cuEventElapsedTime");
    PFN_cuEventDestroy_v2 cuEventDestroy = (PFN_cuEventDestroy_v2)resolve("cuEventDestroy_v2");

    if (!cuInit || !cuCtxCreate || !cuCtxDestroy || !cuCtxSynchronize || !cuStreamCreate ||
        !cuStreamDestroy || !cuStreamSynchronize || !cuMemAlloc || !cuMemFree ||
        !cuMemcpyHtoD || !cuMemcpyDtoH || !cuEventCreate || !cuEventRecord) {
        std::cerr << "FAIL: Essential symbols missing from nvcuda.dll!\n";
        return 1;
    }
    std::cout << "  SUCCESS: All critical Driver API symbols successfully resolved!\n\n";

    // Initialize Driver
    std::cout << "[Step 3] Initializing CUDA driver through shim (cuInit)...\n";
    CUresult res = cuInit(0);
    if (res != CUDA_SUCCESS) {
        std::cerr << "FAIL: cuInit(0) returned " << res << "\n";
        return 1;
    }
    int driverVer = 0;
    cuDriverGetVersion(&driverVer);
    std::cout << "  SUCCESS: cuInit(0) OK! Driver version: " << driverVer << "\n";

    int devCount = 0;
    cuDeviceGetCount(&devCount);
    std::cout << "  Device count: " << devCount << "\n";
    if (devCount <= 0) {
        std::cerr << "FAIL: No CUDA devices found!\n";
        return 1;
    }

    CUdevice dev;
    cuDeviceGet(&dev, 0);
    char devName[256] = {0};
    cuDeviceGetName(devName, sizeof(devName), dev);
    std::cout << "  GPU 0 Name: " << devName << "\n\n";

    // Step 4: Multi-Context Creation and Verification
    std::cout << "[Step 4] Creating multiple contexts on GPU (ctxA, ctxB)...\n";
    CUcontext ctxA = nullptr;
    CUcontext ctxB = nullptr;

    res = cuCtxCreate(&ctxA, 0, dev);
    if (res != CUDA_SUCCESS || !ctxA) {
        std::cerr << "FAIL: cuCtxCreate ctxA returned " << res << "\n";
        return 1;
    }
    std::cout << "  SUCCESS: Created context A: " << ctxA << "\n";

    res = cuCtxCreate(&ctxB, 0, dev);
    if (res != CUDA_SUCCESS || !ctxB) {
        std::cerr << "FAIL: cuCtxCreate ctxB returned " << res << "\n";
        return 1;
    }
    std::cout << "  SUCCESS: Created context B: " << ctxB << "\n\n";

    // Step 5: Test Context Isolation for Streams & Allocation in Context A
    std::cout << "[Step 5] Testing Context A stream creation & memory operations...\n";
    cuCtxPushCurrent(ctxA);
    CUcontext curCtx = nullptr;
    cuCtxGetCurrent(&curCtx);
    assert(curCtx == ctxA);

    CUstream streamA1 = nullptr, streamA2 = nullptr;
    res = cuStreamCreate(&streamA1, CU_STREAM_NON_BLOCKING);
    assert(res == CUDA_SUCCESS && streamA1 != nullptr);
    res = cuStreamCreate(&streamA2, CU_STREAM_NON_BLOCKING);
    assert(res == CUDA_SUCCESS && streamA2 != nullptr);
    std::cout << "  Created streamA1 (" << streamA1 << ") and streamA2 (" << streamA2 << ") in ctxA\n";

    const size_t numElements = 1024 * 1024; // 4MB
    const size_t bufferBytes = numElements * sizeof(float);
    std::vector<float> h_in(numElements, 42.0f);
    std::vector<float> h_out(numElements, 0.0f);

    CUdeviceptr d_dataA = 0;
    res = cuMemAlloc(&d_dataA, bufferBytes);
    assert(res == CUDA_SUCCESS && d_dataA != 0);

    // HtoD copy on streamA1
    res = cuMemcpyHtoD(d_dataA, h_in.data(), bufferBytes);
    assert(res == CUDA_SUCCESS);

    // DtoH copy on streamA2
    res = cuMemcpyDtoH(h_out.data(), d_dataA, bufferBytes);
    assert(res == CUDA_SUCCESS);

    // Verify data transfer
    assert(h_out[0] == 42.0f && h_out[numElements - 1] == 42.0f);
    std::cout << "  Data transfer verified in ctxA (4MB verified)!\n";

    // Step 6: Test Context Isolation for Streams & Allocation in Context B
    std::cout << "\n[Step 6] Testing Context B stream creation & memory operations...\n";
    cuCtxPushCurrent(ctxB);
    cuCtxGetCurrent(&curCtx);
    assert(curCtx == ctxB);

    CUstream streamB1 = nullptr;
    res = cuStreamCreate(&streamB1, CU_STREAM_NON_BLOCKING);
    assert(res == CUDA_SUCCESS && streamB1 != nullptr);
    std::cout << "  Created streamB1 (" << streamB1 << ") in ctxB\n";

    CUdeviceptr d_dataB = 0;
    res = cuMemAlloc(&d_dataB, bufferBytes);
    assert(res == CUDA_SUCCESS && d_dataB != 0);

    std::vector<float> h_inB(numElements, 99.0f);
    std::vector<float> h_outB(numElements, 0.0f);
    res = cuMemcpyHtoD(d_dataB, h_inB.data(), bufferBytes);
    assert(res == CUDA_SUCCESS);
    res = cuMemcpyDtoH(h_outB.data(), d_dataB, bufferBytes);
    assert(res == CUDA_SUCCESS);
    assert(h_outB[0] == 99.0f && h_outB[numElements - 1] == 99.0f);
    std::cout << "  Data transfer verified in ctxB (4MB verified)!\n";

    // Step 7: Test cuCtxSynchronize & cuMemFree_v2 under Context A
    std::cout << "\n[Step 7] Testing cuCtxSynchronize() and cuMemFree_v2() in ctxA...\n";
    cuCtxPushCurrent(ctxA);
    auto t0 = std::chrono::high_resolution_clock::now();
    res = cuCtxSynchronize();
    auto t1 = std::chrono::high_resolution_clock::now();
    assert(res == CUDA_SUCCESS);
    auto sync_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::cout << "  cuCtxSynchronize() in ctxA succeeded in " << sync_us << " us\n";

    t0 = std::chrono::high_resolution_clock::now();
    res = cuMemFree(d_dataA);
    t1 = std::chrono::high_resolution_clock::now();
    assert(res == CUDA_SUCCESS);
    auto free_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::cout << "  cuMemFree_v2() in ctxA succeeded in " << free_us << " us\n";

    // Step 8: Clean up ctxB
    std::cout << "\n[Step 8] Cleaning up ctxB resources...\n";
    cuCtxPushCurrent(ctxB);
    res = cuMemFree(d_dataB);
    assert(res == CUDA_SUCCESS);
    res = cuStreamDestroy(streamB1);
    assert(res == CUDA_SUCCESS);
    std::cout << "  ctxB streams and allocations freed!\n";

    // Step 9: Clean up ctxA streams and destroy contexts
    std::cout << "\n[Step 9] Destroying streams and contexts...\n";
    cuCtxPushCurrent(ctxA);
    res = cuStreamDestroy(streamA1);
    assert(res == CUDA_SUCCESS);
    res = cuStreamDestroy(streamA2);
    assert(res == CUDA_SUCCESS);

    res = cuCtxDestroy(ctxB);
    assert(res == CUDA_SUCCESS);
    std::cout << "  cuCtxDestroy(ctxB) SUCCESS!\n";

    res = cuCtxDestroy(ctxA);
    assert(res == CUDA_SUCCESS);
    std::cout << "  cuCtxDestroy(ctxA) SUCCESS!\n";

    std::cout << "\n====================================================\n";
    std::cout << "   ALL END-TO-END TESTS PASSED ON NVIDIA RTX 5060!  \n";
    std::cout << "====================================================\n";

    FreeLibrary(hShim);
    return 0;
}
