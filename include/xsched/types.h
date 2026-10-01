#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================================
 * 1. 基础句柄抽象 (Handles)
 * ------------------------------------------------------------------------------
 * XSched 将底层硬件概念（如 CUDA 的 CUstream、CUdevice）与上层抽象队列彻底解耦：
 * - XQueueHandle:     XSched 的可抢占命令队列句柄（上层调度单元，类比线程）
 * - HwQueueHandle:    底层物理/驱动队列句柄（如 CUstream、ze_command_queue）
 * - HwCommandHandle:  封装具体 Driver API 调用的硬件命令句柄（如 Kernel Launch）
 * ============================================================================== */
typedef uint64_t XDevice;
typedef uint32_t XDeviceId; // 推荐使用 32 位 PCI 地址作为设备物理标识
typedef uint64_t XQueueHandle;
typedef uint64_t HwQueueHandle;
typedef uint64_t HwCommandHandle;

/* ==============================================================================
 * 2. 错误码与返回值 (Error Codes)
 * ============================================================================== */
typedef enum {
    kXSchedSuccess           = 0,   // 操作成功
    kXSchedErrorHardware     = 1,   // 底层硬件或驱动报错
    kXSchedErrorInvalidValue = 2,   // 传入参数无效
    kXSchedErrorNotFound     = 3,   // 目标句柄或资源未找到
    kXSchedErrorNotAllowed   = 4,   // 当前状态不允许执行该操作
    kXSchedErrorNotSupported = 5,   // 当前平台或硬件架构不支持该特性
    kXSchedErrorBadResponse  = 6,   // IPC 或守护进程通信返回异常响应
    kXSchedErrorUnknown      = 999, // 未知错误
} XResult;

/* ==============================================================================
 * 3. 计算平台与加速器设备分类 (Platform & Device Types)
 * ============================================================================== */
typedef enum {
    kPlatformUnknown   = 0,
    kPlatformVPI       = 1, // NVIDIA Vision Programming Interface (OFA/PVA)
    kPlatformCUDA      = 2, // NVIDIA CUDA
    kPlatformCUDLA     = 3, // NVIDIA Deep Learning Accelerator
    kPlatformHIP       = 4, // AMD ROCm HIP
    kPlatformAscend    = 5, // 华为昇腾 AscendCL
    kPlatformOpenCL    = 6, // 通用 OpenCL
    kPlatformLevelZero = 7, // Intel OneAPI Level-Zero (GPU / NPU)
} XPlatform;

typedef enum {
    kDeviceTypeUnknown = 0,
    kDeviceTypeCPU     = 1,
    kDeviceTypeGPU     = 2,
    kDeviceTypeNPU     = 3,
    kDeviceTypeFPGA    = 4,
    kDeviceTypeASIC    = 5,
    kDeviceTypeMCA     = 6, // Memory Copy Accelerator (DMA 拷贝引擎)
} XDeviceType;

/* ==============================================================================
 * 4. 三级抢占能力模型 (Three-Level Preemption Model)
 * ------------------------------------------------------------------------------
 * 这是 XSched 论文的核心理论贡献：将千差万别的 XPU 硬件能力划分为三级阶梯：
 * - Level 1 (Block):      待提交级抢占。在软件缓冲区拦截，控制下发硬件的时机。
 * - Level 2 (Deactivate): 已提交未执行级抢占。利用硬件/驱动机制撤回未执行的任务。
 * - Level 3 (Interrupt):  执行中级抢占。微秒级硬件中断/切换，中断正在计算的 Kernel。
 * ============================================================================== */
typedef enum {
    kPreemptLevelUnknown    = 0,
    kPreemptLevelBlock      = 1, // 级别1：控制发射门禁，低成本通用兼容所有设备
    kPreemptLevelDeactivate = 2, // 级别2：停用未执行命令，毫秒级快速响应
    kPreemptLevelInterrupt  = 3, // 级别3：内核硬件打断与恢复，微秒级硬实时抢占
    kPreemptLevelMax,
} XPreemptLevel;

/* ==============================================================================
 * 5. XQueue 队列状态机与特性标志 (Queue State & Flags)
 * ------------------------------------------------------------------------------
 * SchedAgent 通过监听 XQueueState 的迁移来产生调度事件（Event）：
 * - Idle: 队列中已无待执行命令，释放算力占用
 * - Ready: 队列中有新任务到达，需要调度器介入分配算力
 * ============================================================================== */
typedef enum {
    kQueueStateUnknown = 0,
    kQueueStateIdle    = 1, // 空闲状态：无命令待执行
    kQueueStateReady   = 2, // 就绪状态：有命令等待被硬件执行
} XQueueState;

typedef enum {
    kQueueFeatureNone               = 0x0000,
    kQueueFeatureAsyncSubmit        = 0x0001, // 异步提交命令
    kQueueFeatureDynamicLevel       = 0x0002, // 支持运行时动态调整抢占级别
    kQueueFeatureDynamicThreshold   = 0x0004, // 支持运行时动态调整流水线飞行阈值
    kQueueFeatureDynamicBatchSize   = 0x0008, // 支持运行时动态调整批处理大小
    kQueueFeatureSyncSuspend        = 0x0010, // 挂起时强制同步底层硬件队列
    kQueueFeatureResumeDropCommands = 0x0020, // 恢复时丢弃堆积的历史命令
    kQueueFeatureMaskAll            = -1,
} XQueueFeature;

typedef enum {
    kQueueCreateFlagNone           = 0x0000,
    kQueueCreateFlagBlockingSubmit = 0x0001, // 队列满时阻塞提交线程
    kQueueCreateFlagMaskAll        = -1,
} XQueueCreateFlag;

typedef enum {
    kQueueSuspendFlagNone        = 0x0000,
    kQueueSuspendFlagSyncHwQueue = 0x0001, // 挂起操作同时同步等待硬件完成当前正在执行的指令
    kQueueSuspendFlagWaitAll     = 0x0002, // 等待队列内全部任务执行完再挂起
    kQueueSuspendFlagWaitIdle    = 0x0004, // 等待队列迁移至 Idle 状态
    kQueueSuspendFlagMaskAll     = -1,
} XQueueSuspendFlag;

typedef enum {
    kQueueResumeFlagNone         = 0x0000,
    kQueueResumeFlagDropCommands = 0x0001, // 恢复时丢弃挂起期间积压的所有待执行命令（适用于视频/实时流抽帧）
    kQueueResumeFlagMaskAll      = -1,
} XQueueResumeFlag;

/* ==============================================================================
 * 6. 调度器架构分类 (Scheduler Architectures)
 * ------------------------------------------------------------------------------
 * - AppManaged: 禁用调度器，由应用程序通过代码显式调用 Suspend/Resume 进行手动控制
 * - Local:      进程内本地调度器，适用于单进程多流的独占管理，开销极低
 * - Global:     跨进程全局调度器，所有进程通过 IPC 连接到 xserver 集中调度
 * ============================================================================== */
typedef enum {
    kSchedulerUnknown    = 0,
    kSchedulerAppManaged = 1, // 应用自管模式（手动控制）
    kSchedulerLocal      = 2, // 进程内本地调度模式
    kSchedulerGlobal     = 3, // 集中式全局调度模式 (XServer)
    kSchedulerMax,
} XSchedulerType;

/* ==============================================================================
 * 7. 调度算法策略家族 (Scheduling Policies)
 * ------------------------------------------------------------------------------
 * XSched 提供了与 Linux 内核/实时系统对齐的丰富策略族，上层策略算法与底层硬件完全解耦：
 * ============================================================================== */
// NEW_POLICY: A new XPolicyType should be added here when creating a new policy.
typedef enum {
    kPolicyUnknown                           = 0,
    kPolicyHighestPriorityFirst              = 1,  // HPF: 严格高优先级优先
    kPolicyHeterogeneousHighestPriorityFirst = 2,  // HHPF: 跨异构多设备的高优先级优先
    kPolicyCPUHighestPriorityFirst           = 3,  // CHPF: 读取操作系统 nice 值的 CPU-GPU 联动优先级
    kPolicyUtilizationPartition              = 4,  // UP: 按队列划分算力利用率配额与时间片
    kPolicyProcessUtilizationPartition       = 5,  // PUP: 按进程划分算力配额
    kPolicyStrictProcessUtilizationPartition = 6,  // SPUP: 严格进程配额（无超额借调）
    kPolicyKEarliestDeadlineFirst            = 7,  // KEDF: K 个最早截止时间优先并发
    kPolicyLaxity                            = 8,  // LAX: 最小松弛度优先（实时系统经典算法）
    kPolicyActiveWindowFirst                 = 9,  // AWF: 前台活跃窗口（GUI/终端）优先响应
    kPolicyCompletelyFairScheduler           = 10, // CFS: 类似 Linux 的红黑树完全公平调度 (vruntime)
    kPolicyMultiLevelFeedbackQueue           = 11, // MLFQ: 多级反馈队列（支持交互任务防饥饿恢复）
    // NEW_POLICY: New XPolicyTypes go here.

    kPolicyMax,
} XPolicyType;

typedef XResult (*LaunchCallback)(HwQueueHandle, void *);

/* ==============================================================================
 * 8. 调度参数量纲与约束定义 (Policy Tuning Parameters)
 * ============================================================================== */
/// @brief 优先级数值（有符号整数），数值越大优先级越高。
typedef int32_t Priority;
#define PRIORITY_NO_EXECUTE -256 // 禁止执行（用于休眠某个任务队列）
#define PRIORITY_MIN        -255 // 最低优先级
#define PRIORITY_DEFAULT     000 // 默认优先级
#define PRIORITY_MAX         255 // 最高优先级

/// @brief 算力利用率百分比（0 ~ 100）
typedef int32_t Utilization;
#define UTILIZATION_MIN      0
#define UTILIZATION_DEFAULT  100
#define UTILIZATION_MAX      100

/// @brief 轮转时间片（微秒 us）
typedef int64_t Timeslice;
#define TIMESLICE_MIN        100      // 0.1 ms (最小时间片)
#define TIMESLICE_DEFAULT    5000     // 5 ms (默认时间片)
#define TIMESLICE_MAX        10000000 // 10 s (最大时间片)
#define TIMESLICE_UNLIMITED  UINT64_MAX

/// @brief 松弛度 Laxity（微秒 us），表示任务最多还可以推迟多久执行
typedef int64_t Laxity;
#define NO_LAXITY           -1

/// @brief 截止时间 Deadline（微秒 us），即任务就绪时间戳 + 期限
typedef int64_t Deadline;
#define NO_DEADLINE         -1

#ifdef __cplusplus
}
#endif
