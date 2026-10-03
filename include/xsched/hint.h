#pragma once

#include "xsched/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================================
 * 1. Scheduler Architecture & Global Policy Selection
 * ============================================================================== */

/// @brief Select the scheduler architecture and policy at runtime.
/// @param scheduler [in] Scheduler architecture type (kSchedulerAppManaged,
///                      kSchedulerLocal or kSchedulerGlobal).
/// @param policy    [in] Scheduling policy enum (only specified directly by the
///                      process when scheduler is kSchedulerLocal).
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XHintSetScheduler(XSchedulerType scheduler, XPolicyType policy);

/* ==============================================================================
 * 2. Static / Dynamic Priority Hints
 * ============================================================================== */

/// @brief Inject a priority hint for a specific XQueue (used by the HPF, HHPF,
///        CFS and MLFQ policies).
/// @param xq    [in] Target XQueue handle.
/// @param prio  [in] Priority value in [-255, 255]; the larger the value, the
///              higher the priority:
///     PRIORITY_MIN:        lowest priority (-255)
///     PRIORITY_DEFAULT:    default initial priority (0)
///     PRIORITY_MAX:        highest priority (255)
///     PRIORITY_NO_EXECUTE: special marker (-256) that permanently forbids the
///                          queue from executing (sleeping queue)
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XHintPriority(XQueueHandle xq, Priority prio);

/* ==============================================================================
 * 3. Utilization Quota & Timeslice Hints
 * ============================================================================== */

/// @brief Set the compute utilization quota of a specific XQueue (used by the
///        UP and PUP policies).
/// @param xq   [in] Target XQueue handle.
/// @param util [in] Target utilization percentage [0, 100]; 0 means no compute
///             assigned, 100 means fully saturated.
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XHintUtilization(XQueueHandle xq, Utilization util);

/// @brief Set the base timeslice size of the round-robin scheduling algorithms
///        (used by UP, PUP, CFS and MLFQ).
/// @param ts_us [in] Timeslice length in microseconds (us); default 5000 us
///               (5 ms).
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XHintTimeslice(Timeslice ts_us);

/* ==============================================================================
 * 4. Real-Time Laxity & Deadline Hints
 * ============================================================================== */

/// @brief Set the laxity and the two-phase priorities of an XQueue (used by the
///        LAX policy).
/// @param xq        [in] Target XQueue handle.
/// @param lax_us    [in] Laxity time (us): how much longer the task may be
///                  delayed before it becomes critical.
/// @param lax_prio  [in] Regular priority, effective while the laxity is not
///                  exhausted (relaxed phase).
/// @param crit_prio [in] Critical priority, applied once the laxity is
///                  exhausted (critical phase).
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XHintLaxity(XQueueHandle xq, Laxity lax_us, Priority lax_prio, Priority crit_prio);

/// @brief Set the deadline of an XQueue (used by the KEDF policy).
/// @param xq     [in] Target XQueue handle.
/// @param ddl_us [in] Deadline in microseconds (us); the absolute deadline of
///               the queue = time when the task enters the Ready state + ddl_us.
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XHintDeadline(XQueueHandle xq, Deadline ddl_us);

/// @brief Set the maximum number of concurrent queues k for the K-Earliest
///        Deadline First (K-EDF) policy. The system picks the k XQueues with the
///        earliest deadlines and launches commands to hardware concurrently.
/// @param k [in] Concurrency k.
/// @return kXSchedSuccess on success, otherwise the corresponding error code.
XResult XHintKDeadline(size_t k);

#ifdef __cplusplus
}
#endif
