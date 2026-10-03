#pragma once

#include <memory>

#include "xsched/cuda/hal/common/cuda.h"

namespace xsched::cuda
{

class Guardian
{
public:
    Guardian() = default;
    virtual ~Guardian() = default;
    static std::shared_ptr<Guardian> Instance(CUdevice dev);

    virtual void GetGuardianInstructions(const void **guardian_instr, size_t *size) = 0;
    virtual void GetResumeInstructions(const void **resume_instr, size_t *size) = 0;

    /// @brief Per-thread register floor the spliced guardian/resume code needs.
    /// The instrumented kernel function will be raised to at least this many
    /// registers before the guardian is spliced in front of it.
    /// 32 covers the sm86 arrays (max live reg R21) and was re-verified for
    /// sm120 in test/sm120_mve/T5_REVIEW.md (guardian R11 / resume R21).
    virtual size_t RequiredRegs() { return 32; }

    /// @brief Barrier floor the spliced guardian/resume code needs (each
    /// array contains one BAR.SYNC).
    virtual size_t RequiredBarriers() { return 1; }
};

} // namespace xsched::cuda
