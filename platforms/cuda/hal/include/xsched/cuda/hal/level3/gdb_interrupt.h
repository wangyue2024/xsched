#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>

namespace xsched::cuda
{

/// \brief Level-3 interrupt backend for architectures whose in-context trap path
///        is unavailable (sm120 on driver 595+, see test/sm120_l3_probe/ARCHIVE_ZH.md
///        and test/sm120_l3_trap_v2/FINDINGS_ZH.md).
///
/// It drives an external `cuda-gdb` process in MI mode:
///   -target-attach <pid>  stops the inferior, which suspends the device
///                         mid-kernel (measured 116 ms to quiesce),
///   -target-detach <pid>  resumes it (~115 ms).
///
/// Measured behaviour on RTX 5060 / 595.99.02:
///   * a frozen kernel resumes with a bit-exact result (run_l3work.sh);
///   * freezing a GPU-hogging kernel hands the whole device to another
///     kernel (33.65 s -> 0.74 s, tools/handover_test.sh);
///   * the freeze is per-context: unrelated processes keep running.
///
/// The target process must allow the trace with
///   prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)
/// otherwise /proc/sys/kernel/yama/ptrace_scope=1 makes the attach fail with
/// EPERM.  Call AllowDebuggerAttach() from the target's init path.
///
/// NOTE: this is a *process level* primitive - the whole target stops, including
/// its scheduler thread - so it is meant to be driven from a scheduler process
/// that manages other applications, not from inside the preempted process.
class GdbInterruptContext
{
public:
    enum class State { kRunning, kSuspended };

    static GdbInterruptContext &Instance();

    /// \brief Allow any process to attach to the calling process.
    /// \return true if the prctl succeeded.
    static bool AllowDebuggerAttach();

    /// \brief Whether the cuda-gdb binary this backend needs is usable.
    static bool IsAvailable();

    /// \brief Spawn the agent for \p pid ahead of time, so that a later Suspend()
    ///        only has to issue the attach (saves the cuda-gdb startup cost,
    ///        ~0.3 s of the ~0.53 s scheduler-side path).
    ///        Safe to call repeatedly; the agent stays disconnected until then.
    bool Prepare(pid_t pid);

    /// \brief Freeze \p pid. Blocks until the device is quiesced.
    ///        Uses a pre-spawned agent when Prepare() was called.
    bool Suspend(pid_t pid);

    /// \brief Resume a suspended \p pid.
    bool Resume(pid_t pid);

    State GetState(pid_t pid);
    bool IsSuspended(pid_t pid) { return GetState(pid) == State::kSuspended; }

    /// \brief Detach from everything (used at shutdown).
    void DetachAll();

private:
    GdbInterruptContext() = default;
    ~GdbInterruptContext();
    GdbInterruptContext(const GdbInterruptContext &) = delete;
    GdbInterruptContext &operator=(const GdbInterruptContext &) = delete;

    struct Impl;
    std::mutex mtx_;
    std::map<pid_t, std::shared_ptr<Impl>> agents_;
};

} // namespace xsched::cuda
