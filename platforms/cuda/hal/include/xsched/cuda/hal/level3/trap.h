#pragma once

#include <memory>
#include <cuxtra/cuxtra.h>

#include "xsched/cuda/hal/common/cuda.h"
#include "xsched/cuda/hal/level2/mm.h"

namespace xsched::cuda
{

class TarpHandler
{
public:
    TarpHandler() = default;
    virtual ~TarpHandler() = default;
    static std::shared_ptr<TarpHandler> Instance(CUdevice dev);

    /// @brief Get the trap handler device address and size for this context.
    /// The default is cuxtra's Pascal-era path; architectures whose cuxtra
    /// getter is unavailable (e.g., sm120/Blackwell) override this with a
    /// direct export-table implementation.
    virtual void GetTrapHandlerInfo(CUcontext ctx, CUdeviceptr *handler, size_t *size)
    {
        cuXtraGetTrapHandlerInfo(ctx, handler, size);
    }

    virtual size_t GetInjectSize() = 0;
    virtual void Instrument(void *handler_host, CUdeviceptr handler_dev, size_t size,
                            void *inject_host , CUdeviceptr inject_dev) = 0;
};

} // namespace xsched::cuda
