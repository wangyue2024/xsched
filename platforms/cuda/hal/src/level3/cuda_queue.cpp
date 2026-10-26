#include "xsched/utils/env.h"
#include "xsched/utils/xassert.h"
#include "xsched/cuda/hal/level3/cuda_queue.h"
#include "xsched/cuda/hal/level3/trap_sm120.h"
#include "xsched/cuda/hal/common/cuda_assert.h"

using namespace xsched::cuda;
using namespace xsched::preempt;

CudaQueueLv3Trap::CudaQueueLv3Trap(CUstream stream): CudaQueueLv2(stream)
{
    interrupt_context_ = InterruptContext::Instance(context_);
}

void CudaQueueLv3Trap::Launch(std::shared_ptr<HwCommand> hw_cmd)
{
    auto kernel = std::dynamic_pointer_cast<CudaKernelCommand>(hw_cmd);
    if (kernel != nullptr) {
        last_kernel_ = hw_cmd;
        relaunched_ = true;
        XINFO("trap queue: remembered kernel cmd idx=%ld", (long)hw_cmd->GetIdx());
    }
    CudaQueueLv2::Launch(hw_cmd);
}

void CudaQueueLv3Trap::Interrupt()
{
    XASSERT(level_ >= kPreemptLevelInterrupt, "Interrupt() not supported on level-%d", level_);
    // wait until the preempt flag is set
    XINFO("Interrupt: syncing op stream");
    CUDA_ASSERT(Driver::StreamSynchronize(instrument_manager_->OpStream()));
    XINFO("Interrupt: op stream idle, triggering");
    // FIXME: what if multiple threads call Interrupt()?
    interrupt_context_->Interrupt();
}

void CudaQueueLv3Trap::Restore(const CommandLog &)
{
    XASSERT(level_ >= kPreemptLevelInterrupt, "Restore() not supported on level-%d", level_);
    // Freeze-style preemption: the RM trap suspended the context, ask the
    // agent to resume it (0x83de0318) so the kernel continues in place.
    TrapSm120Context::Instance().Resume();
}

void CudaQueueLv3Trap::Reactivate(const CommandLog &log)
{
    relaunched_ = false;
    CudaQueueLv2::Reactivate(log);
    if (getenv("XSCHED_SM120_TRAP_CMD_WORD") != nullptr) {
        // Park semantics: the suspend trap ran do_preempt_save/do_pause for the
        // running warps.  Restore them in place: command word 1 + another trap
        // (0x83de0317) makes do_read_trap_reason request preempt_restore.
        instrument_manager_->SetTrapCommand(1);
        XINFO("trap reactivate: command word 1, sending the restore trap");
        TrapSm120Context::Instance().Trigger();
        return;
    }
    XINFO("trap reactivate: log=%zu last_kernel=%p relaunched=%d", log.size(),
          (void *)last_kernel_.get(), (int)relaunched_);
    if (relaunched_ || last_kernel_ == nullptr) return;
    XINFO("trap reactivate: replaying the killed kernel");
    auto kernel = std::dynamic_pointer_cast<CudaKernelCommand>(last_kernel_);
    if (kernel != nullptr &&
        getenv("XSCHED_SM120_TRAP_REPLAY_ORIGINAL") != nullptr) {
        // Replay through the original entry point: the guardian/resume entry
        // still carries the previous preempt state and stops the replayed
        // kernel early (FINDINGS v2.8 §12).
        CUDA_ASSERT(CudaQueueLv2::DirectLaunch(kernel, context_, kStream));
        XINFO("trap reactivate: replayed through the original entry");
        return;
    }
    this->Launch(last_kernel_);
}

void CudaQueueLv3Trap::OnPreemptLevelChange(XPreemptLevel level)
{
    XASSERT(level <= kPreemptLevelInterrupt, "unsupported level: %d", level);
    if (level == kPreemptLevelInterrupt) {
        instrument_manager_->NotifyTrapInstrumented();
        interrupt_context_->InstrumentTrapHandler();
    }
    level_ = level;
}

void CudaQueueLv3Trap::OnHwCommandSubmit(std::shared_ptr<preempt::HwCommand> hw_cmd)
{
    this->CudaQueueLv2::OnHwCommandSubmit(hw_cmd);
    if (level_ < kPreemptLevelInterrupt) return;

    if (std::dynamic_pointer_cast<CudaGraphCommand>(hw_cmd) != nullptr) {
        // do nothing here, will automatically fallback to wait-based preemption
        static bool warned = false;
        if (!warned) {
            warned = true;
            XWARN("CUDA graph cannot support trap-based level-3 preemption, "
                  "falling back to level-1");
        }
        return;
    }
    auto kernel = std::dynamic_pointer_cast<CudaKernelCommand>(hw_cmd);
    // TODO: assign kernel_command->killable
    if (kernel != nullptr) {
        kernel->killable = true;
        last_kernel_ = hw_cmd;
        XINFO("trap queue: remembered kernel cmd idx=%ld", (long)hw_cmd->GetIdx());
    }
}

CudaQueueLv3Tsg::CudaQueueLv3Tsg(CUstream stream): CudaQueueLv1(stream)
{
    tsg_context_ = TsgContext::Instance(context_);
}

void CudaQueueLv3Tsg::Interrupt()
{
    XASSERT(level_ >= kPreemptLevelInterrupt, "Interrupt() not supported on level-%d", level_);
    tsg_context_->Interrupt();
}

void CudaQueueLv3Tsg::Restore(const CommandLog &)
{
    XASSERT(level_ >= kPreemptLevelInterrupt, "Restore() not supported on level-%d", level_);
    tsg_context_->Restore();
}

void CudaQueueLv3Tsg::OnPreemptLevelChange(XPreemptLevel level)
{
    XASSERT(level <= kPreemptLevelInterrupt, "unsupported level: %d", level);
    level_ = level;
}
