#pragma once

#include "xsched/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================================
 * 1. XQueue 队列生命周期管理 (Lifecycle Management)
 * ============================================================================== */

/// @brief 创建一个可抢占命令队列（XQueue）。
/// @param xq    [out] 返回创建好的 XQueue 句柄。
/// @param hwq   [in]  绑定的底层硬件队列句柄（如 CUDA 的 CudaQueueCreate 返回值）。
/// @param level [in]  初始抢占级别，参见 XPreemptLevel（Level 1/2/3）。
/// @param flags [in]  队列创建标志，参见 XQueueCreateFlag。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueCreate(XQueueHandle *xq, HwQueueHandle hwq, int64_t level, int64_t flags);

/// @brief 销毁指定的 XQueue 并释放其命令缓冲池和调度监听器。
/// @param xq [in] 待销毁的 XQueue 句柄。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueDestroy(XQueueHandle xq);

/// @brief 根据底层 HwQueue 句柄查找对应的 XQueue 句柄（反查映射）。
/// @param xq    [out] 输出对应的 XQueue 句柄。
/// @param hwq   [in]  已绑定的 HwQueue 句柄。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueGet(XQueueHandle *xq, HwQueueHandle hwq);

/* ==============================================================================
 * 2. XQueue 运行时动态调优配置 (Runtime Dynamic Configuration)
 * ============================================================================== */

/// @brief 动态调整 XQueue 的抢占级别（支持从 Level-1 到 Level-3 升降级）。
/// @param xq    [in] XQueue 句柄。
/// @param level [in] 新的目标抢占级别。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueSetPreemptLevel(XQueueHandle xq, XPreemptLevel level);

/// @brief 设置 XQueue 的发射流水线阈值与批处理大小（Level-1 核心调优参数）。
/// @param xq         [in] XQueue 句柄。
/// @param threshold  [in] 流水线允许在物理队列中并行的最大命令数（In-Flight 限制）。
/// @param batch_size [in] 每次由 LaunchWorker 批量出队并发射到硬件的命令个数。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueSetLaunchConfig(XQueueHandle xq, int64_t threshold, int64_t batch_size);

/* ==============================================================================
 * 3. 命令提交与同步等待 (Command Enqueue & Synchronization)
 * ============================================================================== */

/// @brief 向 XQueue 提交一条封装好的硬件命令（放入软件缓冲区，等待调度器放行）。
/// @param xq     [in] XQueue 句柄。
/// @param hw_cmd [in] 硬件命令句柄（如 Kernel 启动、异步显存拷贝等）。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueSubmit(XQueueHandle xq, HwCommandHandle hw_cmd);

/// @brief 等待 XQueue 中某条特定的 HwCommand 执行完毕（阻塞当前线程）。
/// @param xq     [in] XQueue 句柄。
/// @param hw_cmd [in] 待等待的目标命令句柄。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueWait(XQueueHandle xq, HwCommandHandle hw_cmd);

/// @brief 等待 XQueue 中当前排队的所有命令全部执行完毕（排空队列）。
/// @param xq [in] XQueue 句柄。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueWaitAll(XQueueHandle xq);

/// @brief 查询 XQueue 当前的状态（如判断是 Idle 还是 Ready）。
/// @param xq    [in]  XQueue 句柄。
/// @param state [out] 返回队列状态（kQueueStateIdle 或 kQueueStateReady）。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueQuery(XQueueHandle xq, XQueueState *state);

/* ==============================================================================
 * 4. 调度抢占与执行流控制 (Preemption & Execution Control)
 * ------------------------------------------------------------------------------
 * Suspend 与 Resume 是调度器进行任务切换时调用的核心接口（支持可重入引用计数）：
 * - Suspend: 停止向硬件发射新命令（或中断正在运行的命令）
 * - Resume:  唤醒工作线程，继续发射队列中的积压命令
 * ============================================================================== */

/// @brief 挂起 XQueue，暂停其任务向物理显卡发射。
/// @param xq    [in] XQueue 句柄。
/// @param flags [in] 挂起控制标志（如同步等待硬件排空，见 XQueueSuspendFlag）。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueSuspend(XQueueHandle xq, int64_t flags);

/// @brief 恢复 XQueue，允许其继续执行任务。
/// @param xq    [in] XQueue 句柄。
/// @param flags [in] 恢复控制标志（如丢弃挂起期间积压命令，见 XQueueResumeFlag）。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueResume(XQueueHandle xq, int64_t flags);

/// @brief 获取该 XQueue 累计已提交的命令总数（用于性能分析与运行时度量）。
/// @param xq    [in]  XQueue 句柄。
/// @param count [out] 输出累计提交的命令数量。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XQueueProfileHwCommandCount(XQueueHandle xq, int64_t *count);

/* ==============================================================================
 * 5. 底层物理队列与原生命令接口 (Hardware Queue & Generic Command APIs)
 * ------------------------------------------------------------------------------
 * 提供给底层 HAL 以及非透明接入场景直接操作硬件的桥梁接口：
 * ============================================================================== */

/// @brief 销毁底层 HwQueue 资源。
XResult HwQueueDestroy(HwQueueHandle hwq);

/// @brief 直接将 HwCommand 发射到 HwQueue（绕过 XQueue 缓冲）。
XResult HwQueueLaunch(HwQueueHandle hwq, HwCommandHandle hw_cmd);

/// @brief 强制同步底层的物理硬件队列。
XResult HwQueueSynchronize(HwQueueHandle hwq);

/// @brief 创建一个通用的自定义回调命令（用于包装任何非标准硬件的执行逻辑）。
/// @param hw_cmd [out] 输出创建好的命令句柄。
/// @param launch [in]  执行回调函数指针。
/// @param data   [in]  回调函数入参指针。
XResult HwCommandCreateCallback(HwCommandHandle *hw_cmd, LaunchCallback launch, void *data);

/// @brief 销毁指定的 HwCommand 对象。
XResult HwCommandDestroy(HwCommandHandle hw_cmd);

#ifdef __cplusplus
}
#endif
