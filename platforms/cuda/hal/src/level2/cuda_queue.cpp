#include "xsched/utils/xassert.h"
#include "xsched/cuda/hal/level2/cuda_queue.h"
#include "xsched/cuda/hal/common/cuda_assert.h"

using namespace xsched::cuda;
using namespace xsched::preempt;

CudaQueueLv2::CudaQueueLv2(CUstream stream): CudaQueueLv1(stream)
{
    instrument_manager_ = std::make_unique<InstrumentManager>(context_, cudevice_);
}

void CudaQueueLv2::Launch(std::shared_ptr<HwCommand> hw_cmd)
{
    // Diagnostic (sm120 trap backend): launch with the debugger-parameter
    // window populated (preempt buffer pointer) but through the *original*
    // entry point, i.e. no guardian replacement.  This keeps the trap handler
    // reachable while still giving the cave a flag to discriminate on.
    if (getenv("XSCHED_SM120_TRAP_PLAIN_LAUNCH") != nullptr) {
        auto kernel = std::dynamic_pointer_cast<CudaKernelCommand>(hw_cmd);
        if (kernel != nullptr) {
            CUDA_ASSERT(CudaQueueLv2::DirectLaunch(kernel, context_, kStream));
            return;
        }
    }
    // Experimental: the sm120 trap backend can kill raw kernels (trap +
    // command-log replay); guardian instrumentation changes the entry point
    // and is incompatible with an external RM debugger session.
    if (getenv("XSCHED_SM120_TRAP_NO_INSTR")) {
        CudaQueueLv1::Launch(hw_cmd);
        return;
    }
    auto kernel = std::dynamic_pointer_cast<CudaKernelCommand>(hw_cmd);
    if (kernel != nullptr) return instrument_manager_->Launch(kernel, kStream, level_);
    
    auto cuda_cmd = std::dynamic_pointer_cast<CudaCommand>(hw_cmd);
    XASSERT(cuda_cmd != nullptr, "hw_cmd is not a CudaCommand");
    CUDA_ASSERT(cuda_cmd->LaunchWrapper(kStream));
}

void CudaQueueLv2::Deactivate()
{
    XASSERT(level_ >= kPreemptLevelDeactivate, "Deactivate() not supported on level-%d", level_);
    // The sm120 trap backend needs this flag too: the armed trap stub
    // redirects the trapped warp to the guardian entry, which exits the block
    // only when the exit flag is set.
    if (getenv("XSCHED_SM120_TRAP_NO_DEACT") != nullptr) return;
    instrument_manager_->Deactivate();
}

void CudaQueueLv2::Reactivate(const preempt::CommandLog &log)
{
    XASSERT(level_ >= kPreemptLevelDeactivate, "Reactivate() not supported on level-%d", level_);
    this->Synchronize();

    uint64_t resume_cmd_idx = instrument_manager_->Reactivate();
    XINFO("Reactivate: resume_cmd_idx=%lu log=%zu", (unsigned long)resume_cmd_idx, log.size());
    if (resume_cmd_idx == 0) return;

    for (auto cmd : log) {
        if (cmd->GetIdx() < (int64_t)resume_cmd_idx) continue;
        this->Launch(cmd);
    }
}

void CudaQueueLv2::OnPreemptLevelChange(XPreemptLevel level)
{
    XASSERT(level <= kPreemptLevelDeactivate, "unsupported level: %d", level);
    level_ = level;
}

void CudaQueueLv2::OnHwCommandSubmit(std::shared_ptr<preempt::HwCommand> hw_cmd)
{
    this->CudaQueueLv1::OnHwCommandSubmit(hw_cmd);
    if (getenv("XSCHED_SM120_TRAP_NO_INSTR")) return;
    if (level_ < kPreemptLevelDeactivate) return;

    if (std::dynamic_pointer_cast<CudaGraphCommand>(hw_cmd) != nullptr) {
        // do nothing here, will automatically fallback to wait-based preemption
        static bool warned = false;
        if (!warned) {
            warned = true;
            XWARN("CUDA graph cannot support level-2 preemption, falling back to level-1");
        }
        return;
    }
    auto kernel = std::dynamic_pointer_cast<CudaKernelCommand>(hw_cmd);
    if (kernel != nullptr) instrument_manager_->Instrument(kernel);
}

CUresult CudaQueueLv2::DirectLaunch(std::shared_ptr<CudaKernelCommand> kernel,
                                    CUcontext ctx, CUstream stream)
{
    auto instrument_ctx = InstrumentContext::Instance(ctx);
    return instrument_ctx->Launch(kernel, stream, kKernelLaunchOriginal);
}
