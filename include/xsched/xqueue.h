#pragma once

#include "xsched/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================================
 * 1. XQueue Lifecycle Management
 * ============================================================================== */

/// @brief Create a preemptible command queue (XQueue).
/// @param xq    [out] Returns the created XQueue handle.
/// @param hwq   [in]  The bound low-level hardware queue handle (e.g., the
///               return value of CudaQueueCreate for CUDA).
/// @param level [in]  Initial preemption level, see XPreemptLevel (Level 1/2/3).
/// @param flags [in]  Queue creation flags, see XQueueCreateFlag.
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueCreate(XQueueHandle *xq, HwQueueHandle hwq, int64_t level, int64_t flags);

/// @brief Destroy the given XQueue and release its command buffer pool and
///        scheduling listeners.
/// @param xq [in] The XQueue handle to destroy.
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueDestroy(XQueueHandle xq);

/// @brief Look up the XQueue handle bound to a low-level HwQueue handle.
/// @param xq    [out] Outputs the corresponding XQueue handle.
/// @param hwq   [in]  The bound HwQueue handle.
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueGet(XQueueHandle *xq, HwQueueHandle hwq);

/* ==============================================================================
 * 2. XQueue Runtime Dynamic Configuration
 * ============================================================================== */

/// @brief Dynamically adjust the preemption level of an XQueue (supports
///        upgrading/downgrading between Level-1 and Level-3).
/// @param xq    [in] XQueue handle.
/// @param level [in] The new target preemption level.
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueSetPreemptLevel(XQueueHandle xq, XPreemptLevel level);

/// @brief Set the launch pipeline threshold and batch size of an XQueue
///        (Level-1 core tuning parameters).
/// @param xq         [in] XQueue handle.
/// @param threshold  [in] Maximum number of commands allowed to be in flight in
///                   the physical queue (in-flight limit).
/// @param batch_size [in] Number of commands dequeued and launched to hardware
///                   per batch by the LaunchWorker.
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueSetLaunchConfig(XQueueHandle xq, int64_t threshold, int64_t batch_size);

/* ==============================================================================
 * 3. Command Submission & Synchronization
 * ============================================================================== */

/// @brief Submit an encapsulated hardware command to the XQueue (placed into the
///        software buffer, waiting for the scheduler to let it through).
/// @param xq     [in] XQueue handle.
/// @param hw_cmd [in] Hardware command handle (e.g., kernel launch, async copy).
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueSubmit(XQueueHandle xq, HwCommandHandle hw_cmd);

/// @brief Wait until a specific HwCommand in the XQueue completes (blocks the
///        calling thread).
/// @param xq     [in] XQueue handle.
/// @param hw_cmd [in] Handle of the target command to wait for.
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueWait(XQueueHandle xq, HwCommandHandle hw_cmd);

/// @brief Wait until all commands currently queued in the XQueue complete
///        (drain the queue).
/// @param xq [in] XQueue handle.
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueWaitAll(XQueueHandle xq);

/// @brief Query the current state of the XQueue (e.g., Idle or Ready).
/// @param xq    [in]  XQueue handle.
/// @param state [out] Outputs the queue state (kQueueStateIdle or
///               kQueueStateReady).
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueQuery(XQueueHandle xq, XQueueState *state);

/* ==============================================================================
 * 4. Scheduling Preemption & Execution Control
 * ------------------------------------------------------------------------------
 * Suspend and Resume are the core interfaces invoked by the scheduler when
 * switching tasks (re-entrant with reference counting):
 * - Suspend: stop launching new commands to hardware (or interrupt running ones)
 * - Resume:  wake up the worker thread and continue launching backlogged commands
 * ============================================================================== */

/// @brief Suspend the XQueue and pause its task launching to the physical GPU.
/// @param xq    [in] XQueue handle.
/// @param flags [in] Suspend control flags (e.g., synchronously wait for the
///               hardware to drain; see XQueueSuspendFlag).
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueSuspend(XQueueHandle xq, int64_t flags);

/// @brief Resume the XQueue and allow it to execute tasks again.
/// @param xq    [in] XQueue handle.
/// @param flags [in] Resume control flags (e.g., drop commands backlogged during
///               the suspension; see XQueueResumeFlag).
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueResume(XQueueHandle xq, int64_t flags);

/// @brief Get the total number of commands ever submitted to this XQueue (for
///        performance analysis and runtime metrics).
/// @param xq    [in]  XQueue handle.
/// @param count [out] Outputs the cumulative number of submitted commands.
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XQueueProfileHwCommandCount(XQueueHandle xq, int64_t *count);

/* ==============================================================================
 * 5. Low-Level Hardware Queue & Generic Command APIs
 * ------------------------------------------------------------------------------
 * Bridges for the low-level HAL and non-transparent integration scenarios to
 * directly operate the hardware:
 * ============================================================================== */

/// @brief Destroy a low-level HwQueue resource.
XResult HwQueueDestroy(HwQueueHandle hwq);

/// @brief Launch a HwCommand directly onto the HwQueue (bypassing the XQueue
///        buffer).
XResult HwQueueLaunch(HwQueueHandle hwq, HwCommandHandle hw_cmd);

/// @brief Force-synchronize the underlying physical hardware queue.
XResult HwQueueSynchronize(HwQueueHandle hwq);

/// @brief Create a generic custom callback command (used to wrap the execution
///        logic of any non-standard hardware).
/// @param hw_cmd [out] Outputs the created command handle.
/// @param launch [in]  Execution callback function pointer.
/// @param data   [in]  Callback argument pointer.
XResult HwCommandCreateCallback(HwCommandHandle *hw_cmd, LaunchCallback launch, void *data);

/// @brief Destroy the given HwCommand object.
XResult HwCommandDestroy(HwCommandHandle hw_cmd);

#ifdef __cplusplus
}
#endif
