#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <windows.h>
#include "xsched/cuda/hal/common/cuda.h"

#define CHECK_CUDA(call)                                                 \
    do {                                                                 \
        CUresult err = (call);                                           \
        if (err != CUDA_SUCCESS) {                                       \
            std::cerr << "CUDA call failed with error code " << err      \
                      << " at line " << __LINE__ << std::endl;           \
            return 1;                                                    \
        }                                                                \
    } while (0)

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "[Test] Starting CUDA Driver API Test" << std::endl;
    std::cout << "========================================" << std::endl;

    std::cout << "[Step 1] Initializing CUDA (cuInit)..." << std::endl;
    CHECK_CUDA(cuInit(0));

    int driver_version = 0;
    CHECK_CUDA(cuDriverGetVersion(&driver_version));
    std::cout << "[Step 2] CUDA Driver Version: " << driver_version << std::endl;

    int device_count = 0;
    CHECK_CUDA(cuDeviceGetCount(&device_count));
    std::cout << "[Step 3] Found " << device_count << " CUDA device(s)" << std::endl;
    if (device_count == 0) {
        std::cerr << "No CUDA devices detected!" << std::endl;
        return 1;
    }

    CUdevice dev;
    CHECK_CUDA(cuDeviceGet(&dev, 0));

    char name[256] = {0};
    CHECK_CUDA(cuDeviceGetName(name, sizeof(name), dev));
    std::cout << "[Step 4] Device 0 Name: " << name << std::endl;

    size_t total_mem = 0;
    CHECK_CUDA(cuDeviceTotalMem_v2(&total_mem, dev));
    std::cout << "[Step 5] Device 0 Total Memory: "
              << (total_mem / (1024 * 1024)) << " MB" << std::endl;

    std::cout << "[Step 6] Creating CUDA Context (cuCtxCreate_v2)..." << std::endl;
    CUcontext ctx;
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    std::cout << "[Step 7] Creating CUDA Stream (cuStreamCreate)..." << std::endl;
    CUstream stream;
    CHECK_CUDA(cuStreamCreate(&stream, 0));

    std::cout << "[Step 8] Allocating Device Memory (cuMemAlloc_v2)..." << std::endl;
    CUdeviceptr d_buf = 0;
    size_t buf_size = 16 * 1024 * 1024; // 16 MB
    CHECK_CUDA(cuMemAlloc_v2(&d_buf, buf_size));

    std::cout << "[Step 9] Performing Asynchronous Memset (cuMemsetD8Async)..." << std::endl;
    CHECK_CUDA(cuMemsetD8Async(d_buf, 0xAB, buf_size, stream));

    std::cout << "[Step 10] Synchronizing Stream (cuStreamSynchronize)..." << std::endl;
    CHECK_CUDA(cuStreamSynchronize(stream));

    std::cout << "[Step 11] Verifying Data with DtoH copy..." << std::endl;
    std::vector<unsigned char> h_buf(1024, 0);
    CHECK_CUDA(cuMemcpyDtoH_v2(h_buf.data(), d_buf, h_buf.size()));
    bool valid = true;
    for (size_t i = 0; i < h_buf.size(); ++i) {
        if (h_buf[i] != 0xAB) {
            valid = false;
            break;
        }
    }
    if (valid) {
        std::cout << "  -> Data verification PASSED! (Pattern 0xAB confirmed)" << std::endl;
    } else {
        std::cerr << "  -> Data verification FAILED!" << std::endl;
        return 1;
    }

    std::cout << "[Step 12] Cleaning up resources..." << std::endl;
    CHECK_CUDA(cuMemFree_v2(d_buf));
    CHECK_CUDA(cuStreamDestroy_v2(stream));
    CHECK_CUDA(cuCtxDestroy_v2(ctx));

    std::cout << "========================================" << std::endl;
    std::cout << "[Test Result] ALL CUDA DRIVER TESTS PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
