#pragma once

#include "xsched/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================================
 * 1. 调度器体系与全局策略设定 (Scheduler & Policy Selection)
 * ============================================================================== */

/// @brief 运行时动态选择调度器类型及调度策略。
/// @param scheduler [in] 调度器架构类型（kSchedulerAppManaged, kSchedulerLocal, kSchedulerGlobal）。
/// @param policy    [in] 调度策略枚举（仅在 scheduler 为 kSchedulerLocal 时由进程直接指定）。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XHintSetScheduler(XSchedulerType scheduler, XPolicyType policy);

/* ==============================================================================
 * 2. 静态/动态优先级提示 (Priority Hint)
 * ============================================================================== */

/// @brief 为特定 XQueue 注入优先级提示（用于 HPF、HHPF、CFS、MLFQ 策略）。
/// @param xq    [in] 目标 XQueue 句柄。
/// @param prio  [in] 优先级数值（[-255, 255]），数值越大优先级越高：
///     PRIORITY_MIN:        最低优先级 (-255)
///     PRIORITY_DEFAULT:    默认初始优先级 (0)
///     PRIORITY_MAX:        最高优先级 (255)
///     PRIORITY_NO_EXECUTE: 特殊标记 (-256)，该队列被永久禁止执行（进入休眠）
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XHintPriority(XQueueHandle xq, Priority prio);

/* ==============================================================================
 * 3. 算力配额与时间片提示 (Utilization & Timeslice Hints)
 * ============================================================================== */

/// @brief 为特定 XQueue 设置算力利用率配额比例（用于 UP、PUP 策略）。
/// @param xq   [in] 目标 XQueue 句柄。
/// @param util [in] 目标利用率百分比整数 [0, 100]，0 表示不分配算力，100 表示占满满载。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XHintUtilization(XQueueHandle xq, Utilization util);

/// @brief 设置轮转类调度算法的单次基础时间片大小（用于 UP、PUP、CFS、MLFQ）。
/// @param ts_us [in] 时间片长度，单位为微秒 (us)，默认 5000 us (5 ms)。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XHintTimeslice(Timeslice ts_us);

/* ==============================================================================
 * 4. 实时性与松弛度调度提示 (Real-Time Laxity & Deadline Hints)
 * ============================================================================== */

/// @brief 为 XQueue 设置松弛度（Laxity）以及双阶段优先级（用于 LAX 策略）。
/// @param xq        [in] 目标 XQueue 句柄。
/// @param lax_us    [in] 松弛度时间（微秒 us），表示任务距离成为紧迫任务还可容忍延迟多久。
/// @param lax_prio  [in] 常规优先级：当松弛度尚未耗尽（处于宽裕期）时生效的优先级。
/// @param crit_prio [in] 临界优先级：当松弛度耗尽（进入临界期）时触发的提升优先级。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XHintLaxity(XQueueHandle xq, Laxity lax_us, Priority lax_prio, Priority crit_prio);

/// @brief 为 XQueue 设置截止期限（Deadline）（用于 KEDF 策略）。
/// @param xq     [in] 目标 XQueue 句柄。
/// @param ddl_us [in] 截止时间（微秒 us），队列绝对截止时间戳 = 任务进入 Ready 态时刻 + ddl_us。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XHintDeadline(XQueueHandle xq, Deadline ddl_us);

/// @brief 设置 K-Earliest Deadline First (K-EDF) 策略的最大并发队列数 k。
/// 系统将挑选截止时间最早的 k 个 XQueue 并发向硬件发射命令。
/// @param k [in] 并发数 k。
/// @return kXSchedSuccess 表示成功，否则返回相应错误码。
XResult XHintKDeadline(size_t k);

#ifdef __cplusplus
}
#endif
