#pragma once

#include "xsched/cuda/hal/common/cuda.h"

namespace xsched::cuda
{

/// @brief Level-3 trap backend for Blackwell sm120 (driver 595+).
///
/// The live driver trap handler is patched once per process; preemption is
/// armed by rewriting the handler cave and requesting a 0x83de0317 trap from an
/// external agent.  See trap_sm120.cpp and test/sm120_l3_trap_v2/FINDINGS_ZH.md
/// (v2.0) for the validated mechanism.
class TrapSm120Context
{
public:
    static TrapSm120Context &Instance();

    /// @brief Locate and patch the live trap handler, handshake with the agent.
    /// @return true when the handler patch is installed.
    bool Init(CUcontext ctx, CUstream stream);
    bool IsReady() const;

    /// @brief Ask the agent for a 0317 trap with the kill cave armed.
    bool Trigger();
    /// @brief Ask the agent for 0318.
    bool Resume();

private:
    TrapSm120Context() = default;
    void Arm();
    void Disarm();
};

} // namespace xsched::cuda
