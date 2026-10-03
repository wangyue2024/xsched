#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================================
 * 1. Handle Abstractions
 * ------------------------------------------------------------------------------
 * XSched fully decouples low-level hardware concepts (e.g., CUDA CUstream,
 * CUdevice) from the upper-layer abstract queues:
 * - XQueueHandle:    handle of an XSched preemptible command queue (the upper
 *                    scheduling unit, analogous to a thread)
 * - HwQueueHandle:   handle of a low-level physical/driver queue
 *                    (e.g., CUstream, ze_command_queue)
 * - HwCommandHandle: handle of a hardware command that encapsulates a concrete
 *                    driver API call (e.g., a kernel launch)
 * ============================================================================== */
typedef uint64_t XDevice;
typedef uint32_t XDeviceId; // It is recommended to use the 32-bit PCI address
                            // as the physical device identifier.
typedef uint64_t XQueueHandle;
typedef uint64_t HwQueueHandle;
typedef uint64_t HwCommandHandle;

/* ==============================================================================
 * 2. Error Codes & Return Values
 * ============================================================================== */
typedef enum {
    kXSchedSuccess           = 0,   // Operation succeeded.
    kXSchedErrorHardware     = 1,   // Low-level hardware or driver error.
    kXSchedErrorInvalidValue = 2,   // Invalid argument.
    kXSchedErrorNotFound     = 3,   // Target handle or resource not found.
    kXSchedErrorNotAllowed   = 4,   // The operation is not allowed in the current state.
    kXSchedErrorNotSupported = 5,   // The feature is not supported by this platform or arch.
    kXSchedErrorBadResponse  = 6,   // Abnormal response from IPC or the daemon.
    kXSchedErrorUnknown      = 999, // Unknown error.
} XResult;

/* ==============================================================================
 * 3. Compute Platform & Accelerator Device Types
 * ============================================================================== */
typedef enum {
    kPlatformUnknown   = 0,
    kPlatformVPI       = 1, // NVIDIA Vision Programming Interface (OFA/PVA)
    kPlatformCUDA      = 2, // NVIDIA CUDA
    kPlatformCUDLA     = 3, // NVIDIA Deep Learning Accelerator
    kPlatformHIP       = 4, // AMD ROCm HIP
    kPlatformAscend    = 5, // Huawei AscendCL
    kPlatformOpenCL    = 6, // Generic OpenCL
    kPlatformLevelZero = 7, // Intel OneAPI Level-Zero (GPU / NPU)
} XPlatform;

typedef enum {
    kDeviceTypeUnknown = 0,
    kDeviceTypeCPU     = 1,
    kDeviceTypeGPU     = 2,
    kDeviceTypeNPU     = 3,
    kDeviceTypeFPGA    = 4,
    kDeviceTypeASIC    = 5,
    kDeviceTypeMCA     = 6, // Memory Copy Accelerator (DMA copy engine)
} XDeviceType;

/* ==============================================================================
 * 4. Three-Level Preemption Capability Model
 * ------------------------------------------------------------------------------
 * A core theoretical contribution of the XSched paper: the widely different
 * XPU hardware capabilities are classified into three tiers:
 * - Level 1 (Block):      preemption at submission time. Commands are
 *                         intercepted in the software buffer and the timing of
 *                         their physical dispatch is controlled.
 * - Level 2 (Deactivate): preemption of submitted-but-not-yet-executed work.
 *                         Hardware/driver mechanisms withdraw pending commands.
 * - Level 3 (Interrupt):  preemption during execution. Microsecond-scale
 *                         hardware interrupt/context switch of a running kernel.
 * ============================================================================== */
typedef enum {
    kPreemptLevelUnknown    = 0,
    kPreemptLevelBlock      = 1, // Level 1: gate the launch path; low cost and
                                 // universally compatible with all devices.
    kPreemptLevelDeactivate = 2, // Level 2: deactivate not-yet-executed
                                 // commands; millisecond-scale response.
    kPreemptLevelInterrupt  = 3, // Level 3: interrupt and restore kernels in
                                 // hardware; microsecond-scale hard real-time.
    kPreemptLevelMax,
} XPreemptLevel;

/* ==============================================================================
 * 5. XQueue State Machine & Feature Flags
 * ------------------------------------------------------------------------------
 * SchedAgent produces scheduling events (Event) by observing XQueueState
 * transitions:
 * - Idle:  no pending commands in the queue; compute occupancy released.
 * - Ready: new work arrived in the queue; the scheduler must allocate compute.
 * ============================================================================== */
typedef enum {
    kQueueStateUnknown = 0,
    kQueueStateIdle    = 1, // Idle state: no command pending execution.
    kQueueStateReady   = 2, // Ready state: commands are waiting to be executed.
} XQueueState;

typedef enum {
    kQueueFeatureNone               = 0x0000,
    kQueueFeatureAsyncSubmit        = 0x0001, // Asynchronous command submission.
    kQueueFeatureDynamicLevel       = 0x0002, // Runtime dynamic preempt level change.
    kQueueFeatureDynamicThreshold   = 0x0004, // Runtime dynamic in-flight threshold change.
    kQueueFeatureDynamicBatchSize   = 0x0008, // Runtime dynamic batch size change.
    kQueueFeatureSyncSuspend        = 0x0010, // Force hardware queue sync on suspend.
    kQueueFeatureResumeDropCommands = 0x0020, // Drop backlogged commands on resume.
    kQueueFeatureMaskAll            = -1,
} XQueueFeature;

typedef enum {
    kQueueCreateFlagNone           = 0x0000,
    kQueueCreateFlagBlockingSubmit = 0x0001, // Block the submitting thread when full.
    kQueueCreateFlagMaskAll        = -1,
} XQueueCreateFlag;

typedef enum {
    kQueueSuspendFlagNone        = 0x0000,
    kQueueSuspendFlagSyncHwQueue = 0x0001, // Also synchronize with the hardware for
                                           // the currently executing instructions.
    kQueueSuspendFlagWaitAll     = 0x0002, // Wait until all queued tasks complete.
    kQueueSuspendFlagWaitIdle    = 0x0004, // Wait until the queue becomes Idle.
    kQueueSuspendFlagMaskAll     = -1,
} XQueueSuspendFlag;

typedef enum {
    kQueueResumeFlagNone         = 0x0000,
    kQueueResumeFlagDropCommands = 0x0001, // Drop all commands backlogged during the
                                           // suspension (e.g., for real-time video
                                           // frame sampling).
    kQueueResumeFlagMaskAll      = -1,
} XQueueResumeFlag;

/* ==============================================================================
 * 6. Scheduler Architecture Types
 * ------------------------------------------------------------------------------
 * - AppManaged: the scheduler is disabled; the application explicitly calls
 *               Suspend/Resume from its own code.
 * - Local:      in-process scheduler, suitable for exclusive management of
 *               multiple streams within a single process; very low overhead.
 * - Global:     cross-process global scheduler; all processes are centrally
 *               scheduled by xserver through IPC.
 * ============================================================================== */
typedef enum {
    kSchedulerUnknown    = 0,
    kSchedulerAppManaged = 1, // Application-managed mode (manual control).
    kSchedulerLocal      = 2, // In-process local scheduling mode.
    kSchedulerGlobal     = 3, // Centralized global scheduling mode (XServer).
    kSchedulerMax,
} XSchedulerType;

/* ==============================================================================
 * 7. Scheduling Policy Family
 * ------------------------------------------------------------------------------
 * XSched provides a rich family of policies aligned with Linux kernel /
 * real-time systems; the upper-layer policies are fully decoupled from the
 * underlying hardware:
 * ============================================================================== */
// NEW_POLICY: A new XPolicyType should be added here when creating a new policy.
typedef enum {
    kPolicyUnknown                           = 0,
    kPolicyHighestPriorityFirst              = 1,  // HPF: strict highest-priority-first.
    kPolicyHeterogeneousHighestPriorityFirst = 2,  // HHPF: HPF across heterogeneous devices.
    kPolicyCPUHighestPriorityFirst           = 3,  // CHPF: OS nice value based CPU-GPU priority.
    kPolicyUtilizationPartition              = 4,  // UP: per-queue utilization quota + timeslice.
    kPolicyProcessUtilizationPartition       = 5,  // PUP: per-process utilization quota.
    kPolicyStrictProcessUtilizationPartition = 6,  // SPUP: strict process quota (no overcommit).
    kPolicyKEarliestDeadlineFirst            = 7,  // KEDF: K earliest-deadline-first queues.
    kPolicyLaxity                            = 8,  // LAX: least-laxity-first (real-time classic).
    kPolicyActiveWindowFirst                 = 9,  // AWF: prioritize foreground active windows.
    kPolicyCompletelyFairScheduler           = 10, // CFS: Linux-like red-black tree fair scheduling.
    kPolicyMultiLevelFeedbackQueue           = 11, // MLFQ: multi-level feedback queue with
                                                   // anti-starvation recovery.
    // NEW_POLICY: New XPolicyTypes go here.

    kPolicyMax,
} XPolicyType;

typedef XResult (*LaunchCallback)(HwQueueHandle, void *);

/* ==============================================================================
 * 8. Policy Tuning Parameters & Constraints
 * ============================================================================== */
/// @brief Priority value (signed integer); the larger the value, the higher
///        the priority.
typedef int32_t Priority;
#define PRIORITY_NO_EXECUTE -256 // Forbid execution (put a queue to sleep).
#define PRIORITY_MIN        -255 // Lowest priority.
#define PRIORITY_DEFAULT     000 // Default priority.
#define PRIORITY_MAX         255 // Highest priority.

/// @brief Compute utilization percentage (0 ~ 100).
typedef int32_t Utilization;
#define UTILIZATION_MIN      0
#define UTILIZATION_DEFAULT  100
#define UTILIZATION_MAX      100

/// @brief Round-robin timeslice (microseconds, us).
typedef int64_t Timeslice;
#define TIMESLICE_MIN        100      // 0.1 ms (minimum timeslice).
#define TIMESLICE_DEFAULT    5000     // 5 ms (default timeslice).
#define TIMESLICE_MAX        10000000 // 10 s (maximum timeslice).
#define TIMESLICE_UNLIMITED  UINT64_MAX

/// @brief Laxity (microseconds, us): how long the task may be postponed at most.
typedef int64_t Laxity;
#define NO_LAXITY           -1

/// @brief Deadline (microseconds, us): task ready timestamp + deadline period.
typedef int64_t Deadline;
#define NO_DEADLINE         -1

#ifdef __cplusplus
}
#endif
