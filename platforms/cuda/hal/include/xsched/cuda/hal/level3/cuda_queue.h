#pragma once

#include "xsched/cuda/hal/level2/cuda_queue.h"
#include "xsched/cuda/hal/level3/interrupt.h"
#include "xsched/cuda/hal/level3/tsg.h"

namespace xsched::cuda
{

class CudaQueueLv3Trap : public CudaQueueLv2
{
public:
    CudaQueueLv3Trap(CUstream stream);
    virtual ~CudaQueueLv3Trap() = default;

    virtual void Launch(std::shared_ptr<preempt::HwCommand> hw_cmd) override;
    virtual void Interrupt() override;
    virtual void Restore(const preempt::CommandLog &log) override;
    virtual void Reactivate(const preempt::CommandLog &log) override;
    virtual void OnPreemptLevelChange(XPreemptLevel level) override;
    virtual void OnHwCommandSubmit(std::shared_ptr<preempt::HwCommand> hw_cmd) override;

    virtual bool          SupportDynamicLevel()  override { return true; }
    virtual XPreemptLevel GetMaxSupportedLevel() override { return kPreemptLevelInterrupt; }

protected:
    std::shared_ptr<InterruptContext> interrupt_context_ = nullptr;
    // The trap backend kills the running kernel instead of pausing it in
    // place, so it has to replay that kernel itself: the launch worker's
    // command log is already empty by the time Resume() runs.
    std::shared_ptr<preempt::HwCommand> last_kernel_ = nullptr;
    bool relaunched_ = false;   // Reactivate relaunched a kernel this round
};

class CudaQueueLv3Tsg : public CudaQueueLv1
{
public:
    CudaQueueLv3Tsg(CUstream stream);
    virtual ~CudaQueueLv3Tsg() = default;

    virtual void Interrupt() override;
    virtual void Restore(const preempt::CommandLog &log) override;
    virtual void OnPreemptLevelChange(XPreemptLevel level) override;

    virtual bool          SupportDynamicLevel()  override { return true; }
    virtual XPreemptLevel GetMaxSupportedLevel() override { return kPreemptLevelInterrupt; }

private:
    std::shared_ptr<TsgContext> tsg_context_ = nullptr;
};

} // namespace xsched::cuda
