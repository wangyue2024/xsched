#include "xsched/cuda/hal/level2/guardian.h"
#include "xsched/cuda/hal/level3/trap.h"
#include "xsched/cuda/hal/level3/cuda_queue.h"
#include "xsched/cuda/hal/common/levels.h"
#include "xsched/cuda/hal/common/driver.h"
#include "xsched/cuda/hal/common/options.h"
#include "xsched/cuda/hal/common/cuda_assert.h"
#include "xsched/cuda/hal/arch/sm35.h"
#include "xsched/cuda/hal/arch/sm70.h"
#include "xsched/cuda/hal/arch/sm86.h"
#include "xsched/cuda/hal/arch/sm120.h"
// NEW_CUDA_ARCH: New CUDA architecture support goes here

using namespace xsched::cuda;
using namespace xsched::preempt;

static int32_t GetArch(CUdevice dev)
{
    // see https://developer.nvidia.com/cuda-legacy-gpus
    // see https://developer.nvidia.com/cuda-gpus
    int major, minor;
    CUDA_ASSERT(Driver::DeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev));
    CUDA_ASSERT(Driver::DeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev));
    return major * 10 + minor;
}

std::shared_ptr<Guardian> Guardian::Instance(CUdevice dev)
{
    switch (GetArch(dev)) {
    case 35:
        return std::make_shared<GuardianSM35>();
    case 70:
        return std::make_shared<GuardianSM70>();
    case 86:
        return std::make_shared<GuardianSM86>();
    case 120: // Blackwell: RTX 50 series
        return std::make_shared<GuardianSM120>();
    // NEW_CUDA_ARCH: New CUDA architecture support goes here
    default:
        return nullptr;
    }
}

std::shared_ptr<TarpHandler> TarpHandler::Instance(CUdevice dev)
{
    switch (GetArch(dev)) {
    case 70:
        return std::make_shared<TarpHandlerSM70>();
    case 86:
        return std::make_shared<TarpHandlerSM86>();
    case 120: // Blackwell: RTX 50 series (Linux; Windows falls back to Lv2)
        return std::make_shared<TarpHandlerSM120>();
    // NEW_CUDA_ARCH: New CUDA architecture support goes here
    default:
        return nullptr;
    }
}

std::shared_ptr<HwQueue> xsched::cuda::CudaQueueCreate(CUstream stream)
{
    CUdevice dev;
    CUcontext stream_ctx;
    CUcontext current_ctx;
    CUDA_ASSERT(Driver::StreamGetCtx(stream, &stream_ctx));
    CUDA_ASSERT(Driver::CtxGetCurrent(&current_ctx));
    XASSERT(current_ctx == stream_ctx,
            "create CudaQueue failed: current context (%p) does not match stream context (%p)",
            current_ctx, stream_ctx);
    CUDA_ASSERT(Driver::CtxGetDevice(&dev));

    const int32_t arch = GetArch(dev);

#if defined(_WIN32)
    // Windows: only the Blackwell (sm120) level-2 path is hardware-verified
    // on the standard driver (test/sm120_mve, T6).  The trap (L3) path is
    // unavailable there (cuXtraGetTrapHandlerInfo aborts, T1-P6) and sm120
    // has no TarpHandlerSM120, so sm120 must NOT fall into CudaQueueLv3Trap.
    // Other architectures keep the legacy level-1 fallback.
    if (arch == 120) return std::make_shared<CudaQueueLv2>(stream);
    return std::make_shared<CudaQueueLv1>(stream);
#endif

    if (GetCudaLv3Implementation() == kCudaLv3ImplementationTsg) {
        return std::make_shared<CudaQueueLv3Tsg>(stream);
    }

    switch (arch) {
    case 35: // Kepler: K20, K40, GTX TITAN
        return std::make_shared<CudaQueueLv2>(stream);
    case 70: // Volta: V100, GV100
    case 86: // Ampere: RTX3050 - RTX 3090 Ti
    case 120: // Blackwell: RTX 50 series -- trap path via TarpHandlerSM120
        return std::make_shared<CudaQueueLv3Trap>(stream);
    // NEW_CUDA_ARCH: New CUDA architecture support goes here
    default:
        return std::make_shared<CudaQueueLv1>(stream);
    }
}

CUresult xsched::cuda::DirectLaunch(std::shared_ptr<CudaKernelCommand> kernel, CUstream stream)
{
    CUdevice dev;
    CUcontext stream_ctx;
    CUcontext current_ctx;
    CUDA_ASSERT(Driver::StreamGetCtx(stream, &stream_ctx));
    CUDA_ASSERT(Driver::CtxGetCurrent(&current_ctx));
    XASSERT(current_ctx == stream_ctx,
            "direct launch kernel failed: current context (%p) does not match stream context (%p)",
            current_ctx, stream_ctx);
    CUDA_ASSERT(Driver::CtxGetDevice(&dev));

    const int32_t arch = GetArch(dev);

#if defined(_WIN32)
    // Windows: sm120 uses the level-2 direct path so that an already
    // instrumented kernel stays safe when launched on an unmanaged stream
    // (the preempt-buffer field of the debugger window is populated before
    // the passthrough launch).  Other architectures keep the plain
    // level-1 launch.
    if (arch == 120) return CudaQueueLv2::DirectLaunch(kernel, current_ctx, stream);
    return CudaQueueLv1::DirectLaunch(kernel, stream);
#endif

    if (GetCudaLv3Implementation() == kCudaLv3ImplementationTsg) {
        return CudaQueueLv3Tsg::DirectLaunch(kernel, stream);
    }

    switch (arch) {
    case 35:
        return CudaQueueLv2::DirectLaunch(kernel, current_ctx, stream);
    case 70:
    case 86:
    case 120:
        return CudaQueueLv3Trap::DirectLaunch(kernel, current_ctx, stream);
    // NEW_CUDA_ARCH: New CUDA architecture support goes here
    default:
        return CudaQueueLv1::DirectLaunch(kernel, stream);
    }
}
