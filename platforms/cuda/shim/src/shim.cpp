/**
 * @file shim.cpp
 * @brief XSched CUDA 驱动 API 拦截层核心实现 (CUDA Driver Shim Implementation)
 * 
 * 本文件是 XSched 对 NVIDIA CUDA Driver API 进行透明劫持的核心枢纽。
 * 它伪装成官方的 nvcuda.dll / libcuda.so，截获应用程序（如 PyTorch、TensorFlow）
 * 对 CUDA 驱动 API 的调用，将其转化为 XSched 内部的软件命令（HwCommand），
 * 并根据调度队列（XQueue）的状态机控制其实际物理下发。
 * 
 * 核心架构划分：
 *   1. 【全局事件管理】: 追踪 CUevent 与其对应的录制命令关系 (g_events)
 *   2. 【默认流与阻塞流处理】: 每线程默认流 (PTDS) 创建与 Legacy Default Stream 阻塞同步
 *   3. 【CUDA Graph 捕获处理】: 图捕获期间的安全性保证 (CaptureBegin / CaptureEnd / Bypass)
 *   4. 【Kernel 发射拦截】: cuLaunchKernel / cuLaunchKernelEx / cuLaunchHostFunc 拦截与入队
 *   5. 【显存释放安全同步】: cuMemFree_v2 强制全局排空，防止 UAF (Use-After-Free)
 *   6. 【Event 事件系统拦截】: cuEventRecord / Query / Synchronize / StreamWaitEvent / Destroy
 *   7. 【流同步与状态查询】: cuStreamSynchronize / StreamQuery / CtxSynchronize
 *   8. 【流创建与销毁】: cuStreamCreate / Destroy 及单进程单物理流 (Single-Stream) 复用优化
 */

#include <list>
#include <mutex>
#include <atomic>
#include <functional>

#include "xsched/utils/log.h"
#include "xsched/utils/map.h"
#include "xsched/preempt/hal/hw_queue.h"
#include "xsched/preempt/xqueue/xqueue.h"
#include "xsched/cuda/hal.h"
#include "xsched/cuda/shim/shim.h"
#include "xsched/cuda/hal/common/cuda.h"
#include "xsched/cuda/hal/common/levels.h"
#include "xsched/cuda/hal/common/options.h"
#include "xsched/cuda/hal/level1/cuda_queue.h"
#include "xsched/cuda/hal/common/cuda_command.h"

using namespace xsched::preempt;

namespace xsched::cuda
{

// ============================================================================
// 1. 全局事件映射表 (Global Event Map)
// ============================================================================

/**
 * @brief 全局事件映射表：CUevent -> CudaEventRecordCommand
 * 
 * 在 CUDA 中，cuEventRecord 会在一个 stream 上打下一个时间点标记。
 * 由于 XSched 将命令放入软件队列异步下发，因此需要用该表记录每个 CUevent
 * 绑定的录制命令对象，以支持后续的：
 *   - 跨流等待 (cuStreamWaitEvent)
 *   - 主机端事件查询 (cuEventQuery)
 *   - 主机端事件同步 (cuEventSynchronize)
 *   - 延迟安全销毁 (cuEventDestroy)
 */
static utils::ObjectMap<CUevent, std::shared_ptr<CudaEventRecordCommand>> g_events;

// ============================================================================
// 2. 默认流与阻塞流处理 (Stream Management & PTDS)
// ============================================================================

/**
 * @brief 获取当前线程专属的 PTDS (Per-Thread Default Stream)
 * 
 * CUDA 支持两种默认流语义：
 *   1. 传统的 Legacy Default Stream (NULL / CU_STREAM_LEGACY): 全局阻塞流。
 *   2. Per-Thread Default Stream (CU_STREAM_PER_THREAD): 每个线程独立的隐式非阻塞流。
 * 
 * XSched 为每个线程分配一个显式的非阻塞物理流作为其 PTDS，并自动为其挂载一个 XQueue，
 * 从而把隐式默认流纳入调度器的管控范围中。
 * 
 * @return 当前线程绑定的 CUstream
 */
CUstream GetPTDS()
{
    /// FIXME: Here we assume that the thread will only use one single CUDA context.
    /// However, each CUDA context should have its own per-thread default stream.
    /// TODO: Destory the stream when the thread exits.
    static thread_local CUstream per_thread_default_stream = 0;
    if (per_thread_default_stream != 0) return per_thread_default_stream;

    CUstream stream = nullptr;
    // 创建底层非阻塞物理流
    CUDA_ASSERT(Driver::StreamCreate(&stream, CU_STREAM_NON_BLOCKING));

    /// FIXME: If XSCHED_AUTO_XQUEUE is not turned on,
    /// there is no meaning creating new per-thread default streams.
    // 自动为该流注册 HwQueue 并创建软件调度队列 XQueue
    XQueueManager::AutoCreate([&](HwQueueHandle *hwq) {return CudaQueueCreate(hwq, stream);});
    per_thread_default_stream = stream;
    return stream;
}

/**
 * @brief 等待所有阻塞型 XQueue（非 CU_STREAM_NON_BLOCKING）排空完成
 * 
 * 根据 CUDA 官方规范：当在 Legacy Default Stream (NULL) 上发射操作时，
 * 必须隐式等待设备上所有其他阻塞流中的历史任务全部执行完毕；
 * 同时后序所有流上的任务也必须等待该默认流执行完毕。
 * 本函数向所有阻塞型 XQueue 提交 WaitAll 并同步等待，以严格保证此语义。
 */
void WaitBlockingXQueues()
{
    std::list<std::shared_ptr<XQueueWaitAllCommand>> wait_cmds;
    // 遍历当前进程中所有受管的 XQueue
    XResult res = XQueueManager::ForEach([&](std::shared_ptr<XQueue> xq)->XResult {
        auto hwq = xq->GetHwQueue();
        auto cuda_q = std::dynamic_pointer_cast<CudaQueueLv1>(hwq);
        if (cuda_q == nullptr) return kXSchedErrorUnknown;
        // 若流本身带有 CU_STREAM_NON_BLOCKING 标记，则无需等待
        if (cuda_q->GetStreamFlags() & CU_STREAM_NON_BLOCKING) return kXSchedSuccess;
        
        // 提交排空命令并收集等待句柄
        auto wait_cmd = xq->SubmitWaitAll();
        if (wait_cmd == nullptr) return kXSchedErrorUnknown;
        wait_cmds.push_back(wait_cmd);
        return kXSchedSuccess;
    });
    XASSERT(res == kXSchedSuccess, "Fail to submit wait all commands");
    // 阻塞等待所有阻塞流命令执行结束
    for (auto &cmd : wait_cmds) cmd->Wait();
}

// ============================================================================
// 3. CUDA Graph 捕获处理 (CUDA Graph Capture Handling)
// ============================================================================

static std::mutex g_capture_mutex;
static std::atomic<int64_t> g_capture_counter {0};

/**
 * @brief 进入 CUDA Graph 捕获状态
 * 
 * 当应用程序调用 cuStreamBeginCapture 时触发。
 * 在捕获期间，所有后续的 Kernel 发射和数据传输操作都不能进入 XSched 队列，
 * 而是必须直通（Bypass）到底层驱动以被记录进 CUDA Graph 节点结构中。
 * 
 * 关键不变式（Invariants）：
 *   1. 捕获安全性：在驱动正式开始捕获之前，必须先调用 XCtxSynchronize()
 *      将之前所有 XQueue 中在排队或正在下发的工作彻底清空，防止意外被录入图或破坏捕获流状态。
 *   2. 时序正确性：确保在切换到直通模式之前，先前的异步调度任务已经全部完成，避免乱序执行。
 */
void CaptureBegin()
{
    /// @note: We assume that when a thread starts a CUDA graph capture,
    /// no other threads will submit commands concurrently.
    /// Explanation:
    /// While GetCaptureCounter() > 0, every shim entry point bypasses the
    /// XQueue and launch it command directly to the driver (see CHECK_STREAM*
    /// in shim.h). CaptureBegin / CaptureEnd maintain that counter around
    /// each cuStreamBeginCapture / cuStreamEndCapture pair.
    ///
    /// Two invariants we want to hold for any thread that observes
    /// GetCaptureCounter() > 0:
    ///
    ///   (1) Capture safety. When the driver actually starts capturing a
    ///       stream, no XQueue launch worker is still about to launch
    ///       any commands onto that stream or do synchronization
    ///       — otherwise the command would be silently recorded into the graph
    ///       or cause CUDA graph invalidation if synchronization is called.
    ///
    ///   (2) Ordering safety. Any thread that observes counter > 0 and
    ///       therefore switches to "direct launch mode" must have its
    ///       previously-submitted XQueue commands already completed.
    ///       Otherwise its direct launches can race ahead of commands it
    ///       submitted earlier on the same XQueue.
    ///
    /// Here, we do XCtxSynchronize first, then increase counter.
    /// So that (2) can be guaranteed even when there is a concurrent
    /// command submission.
    /// Corner case: two concurrent threads are submitting to the same XQueue
    /// thread A check count == 0 => thread X ctx sync => thread X count = 1
    /// => thread B check count == 1 => thread A submit command A to XQueue A
    /// => thread B launch command B directly to CUDA => XQueue A launch command A
    /// in this case command A will be executed after command B.
    /// However, in CUDA semantics, command A and B can also be reordered.
    /// But (1) still depends on the application-level assumption that
    /// no other thread submits to an XQueue while CaptureBegin is running.
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    // 首次进入捕获时，必须同步等待整个上下文排空
    if (g_capture_counter.load() == 0) XCtxSynchronize();
    g_capture_counter.fetch_add(1);
}

/**
 * @brief 结束 CUDA Graph 捕获状态
 */
void CaptureEnd()
{
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    int64_t counter = g_capture_counter.load();
    if (counter == 0) {
        XWARN("CaptureEnd() called but capture counter is 0");
        return;
    }
    g_capture_counter.store(counter - 1);
}

/**
 * @brief 获取当前全局正在进行的 Graph Capture 计数
 * @return 计数大于 0 表示当前系统正处于图捕获流程中
 */
int64_t GetCaptureCounter()
{
    return g_capture_counter.load();
}

// ============================================================================
// 4. Kernel 发射拦截 (Kernel Launch Interception)
// ============================================================================

/**
 * @brief Kernel 发射核心模版实现函数
 * 
 * 这是所有 Kernel 拦截入口（cuLaunchKernel、cuLaunchKernelEx 等）的公共落脚点：
 *   1. 检查传入的 CUstream 是否被 XSched 管理（转换为 HwQueueHandle 查询绑定的 XQueue）。
 *   2. 如果该流未被 XSched 接管（xq == nullptr），则调用 DirectLaunch 直接向硬件驱动下发。
 *   3. 如果受 XSched 管理，则将 Kernel 及其全部发射参数打包成 CmdT 命令对象，
 *      调用 xq->Submit(kernel) 放入队列缓冲区，并立即返回 CUDA_SUCCESS。
 * 
 * @tparam CmdT 命令类类型（如 CudaKernelLaunchCommand）
 * @param stream 目标 CUDA 流
 * @param args 构造 CmdT 所需的可变参数包
 * @return CUresult 执行结果
 */
template <typename CmdT, typename... Args>
CUresult XLaunchKernelImpl(CUstream stream, Args&&... args)
{
    auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    auto kernel = std::make_shared<CmdT>(std::forward<Args>(args)..., xq != nullptr);
    if (xq == nullptr) return DirectLaunch(kernel, stream);
    xq->Submit(kernel);
    return CUDA_SUCCESS;
}

/**
 * @brief 拦截标准的 cuLaunchKernel
 */
CUresult XLaunchKernel(CUfunction f,
                       unsigned int gdx, unsigned int gdy, unsigned int gdz,
                       unsigned int bdx, unsigned int bdy, unsigned int bdz,
                       unsigned int shmem, CUstream stream, void **params, void **extra)
{
    XDEBG("XLaunchKernel(func: %p, stream: %p, grid: [%u, %u, %u], block: [%u, %u, %u], "
          "shm: %u, params: %p, extra: %p)", f, stream, gdx, gdy, gdz, bdx, bdy, bdz,
          shmem, params, extra);
    // 检查流转换（处理 NULL/Legacy 流、图捕获等特殊情况）
    CHECK_STREAM(stream, DirectLaunch(std::make_shared<CudaKernelLaunchCommand>(
        f, gdx, gdy, gdz, bdx, bdy, bdz, shmem, params, extra, false), stream));
    return XLaunchKernelImpl<CudaKernelLaunchCommand>(
        stream, f, gdx, gdy, gdz, bdx, bdy, bdz, shmem, params, extra);
}

/**
 * @brief 拦截 per-thread 默认流版本的 cuLaunchKernel_ptsz
 */
CUresult XLaunchKernel_ptsz(CUfunction f,
                            unsigned int gdx, unsigned int gdy, unsigned int gdz,
                            unsigned int bdx, unsigned int bdy, unsigned int bdz,
                            unsigned int shmem, CUstream stream, void **params, void **extra)
{
    XDEBG("XLaunchKernel_ptsz(func: %p, stream: %p, grid: [%u, %u, %u], block: [%u, %u, %u], "
          "shm: %u, params: %p, extra: %p)", f, stream, gdx, gdy, gdz, bdx, bdy, bdz,
          shmem, params, extra);
    CHECK_STREAM_PTSZ(stream, DirectLaunch(std::make_shared<CudaKernelLaunchCommand>(
        f, gdx, gdy, gdz, bdx, bdy, bdz, shmem, params, extra, false), stream));
    return XLaunchKernelImpl<CudaKernelLaunchCommand>(
        stream, f, gdx, gdy, gdz, bdx, bdy, bdz, shmem, params, extra);
}

/**
 * @brief 拦截扩展版 cuLaunchKernelEx (支持带有动态集群配置、属性配置的 Kernel)
 */
CUresult XLaunchKernelEx(const CUlaunchConfig *config, CUfunction f, void **params, void **extra)
{
    if (config == nullptr) {
        XDEBG("XLaunchKernelEx(cfg: %p, func: %p, params: %p, extra: %p)",
              config, f, params, extra);
        return Driver::LaunchKernelEx(config, f, params, extra);
    }
    XDEBG("XLaunchKernelEx(cfg: %p, func: %p, params: %p, extra: %p, stream: %p)",
          config, f, params, extra, config->hStream);

    CUlaunchConfig cfg = *config;
    CHECK_STREAM(cfg.hStream, DirectLaunch(std::make_shared<CudaKernelLaunchExCommand>(
        &cfg, f, params, extra, false), cfg.hStream));
    return XLaunchKernelImpl<CudaKernelLaunchExCommand>(cfg.hStream, &cfg, f, params, extra);
}

/**
 * @brief 拦截 per-thread 默认流版本的 cuLaunchKernelEx_ptsz
 */
CUresult XLaunchKernelEx_ptsz(const CUlaunchConfig *config, CUfunction f, void **params, void **extra)
{
    if (config == nullptr) {
        XDEBG("XLaunchKernelEx_ptsz(cfg: %p, func: %p, params: %p, extra: %p)",
              config, f, params, extra);
        return Driver::LaunchKernelEx(config, f, params, extra);
    }
    XDEBG("XLaunchKernelEx_ptsz(cfg: %p, func: %p, params: %p, extra: %p, stream: %p)",
          config, f, params, extra, config->hStream);

    CUlaunchConfig cfg = *config;
    CHECK_STREAM_PTSZ(cfg.hStream, DirectLaunch(std::make_shared<CudaKernelLaunchExCommand>(
        &cfg, f, params, extra, false), cfg.hStream));
    return XLaunchKernelImpl<CudaKernelLaunchExCommand>(cfg.hStream, &cfg, f, params, extra);
}

/**
 * @brief 拦截向 CUDA 流中提交 CPU 主机回调函数 (cuLaunchHostFunc) 的实现
 */
static inline CUresult XLaunchHostFuncImpl(CUstream stream, CUhostFn fn, void *data)
{
    // 注意：cuLaunchHostFunc 的第一个参数是 stream
    auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    if (xq == nullptr) return Driver::LaunchHostFunc(stream, fn, data);
    auto hw_cmd = std::make_shared<CudaHostFuncCommand>(fn, data);
    xq->Submit(hw_cmd);
    return CUDA_SUCCESS;
}

CUresult XLaunchHostFunc(CUstream stream, CUhostFn fn, void *data)
{
    XDEBG("XLaunchHostFunc(stream: %p, fn: %p, data: %p)", stream, fn, data);
    CHECK_STREAM(stream, Driver::LaunchHostFunc(stream, fn, data));
    return XLaunchHostFuncImpl(stream, fn, data);
}

CUresult XLaunchHostFunc_ptsz(CUstream stream, CUhostFn fn, void *data)
{
    XDEBG("XLaunchHostFunc_ptsz(stream: %p, fn: %p, data: %p)", stream, fn, data);
    CHECK_STREAM_PTSZ(stream, Driver::LaunchHostFunc(stream, fn, data));
    return XLaunchHostFuncImpl(stream, fn, data);
}

/**
 * @brief 拦截 cuStreamEndCapture，安全退出图捕获模式
 */
static inline CUresult XStreamEndCaptureImpl(CUstream stream, CUgraph *graph)
{
    CUstreamCaptureStatus before = CU_STREAM_CAPTURE_STATUS_NONE;
    CUresult qres = Driver::StreamIsCapturing(stream, &before);
    CUresult res = Driver::StreamEndCapture(stream, graph);
    if (res != CUDA_ERROR_STREAM_CAPTURE_UNMATCHED && // 在正确的流上结束捕获
        qres == CUDA_SUCCESS && before != CU_STREAM_CAPTURE_STATUS_NONE) {
        // 捕获前流处于捕获态，检查调用后是否已真正退出
        CUstreamCaptureStatus after = CU_STREAM_CAPTURE_STATUS_NONE;
        qres = Driver::StreamIsCapturing(stream, &after);
        if (qres == CUDA_SUCCESS && after == CU_STREAM_CAPTURE_STATUS_NONE) {
            CaptureEnd();
        }
    }
    return res;
}

CUresult XStreamEndCapture(CUstream stream, CUgraph *graph)
{
    XDEBG("XStreamEndCapture(stream: %p, graph: %p)", stream, graph);
    CONVERT_STREAM(stream);
    return XStreamEndCaptureImpl(stream, graph);
}

CUresult XStreamEndCapture_ptsz(CUstream stream, CUgraph *graph)
{
    XDEBG("XStreamEndCapture_ptsz(stream: %p, graph: %p)", stream, graph);
    CONVERT_STREAM_PTSZ(stream);
    return XStreamEndCaptureImpl(stream, graph);
}

// ============================================================================
// 5. 显存释放安全同步 (Memory Free Safety)
// ============================================================================

/**
 * @brief 拦截 cuMemFree_v2
 * 
 * 在原生 CUDA 语义中，cuMemFree 必须确保所有引用该地址的流上异步操作完成后才物理释放。
 * XSched 在此采用保守策略：释放显存前触发所有 XQueue 的 WaitAll 同步排空，
 * 杜绝在异步调度下发过程中出现 UAF (Use-After-Free) 悬挂野指针崩溃。
 */
CUresult XMemFree_v2(CUdeviceptr dptr)
{
    /// TODO: Optimize this.
    /// In CUDA semantics, cuMemFree only waits for commands who use this memory.
    XQueueManager::ForEachWaitAll();
    return Driver::MemFree_v2(dptr);
}

// ============================================================================
// 6. Event 事件系统拦截 (CUDA Event Interception)
// ============================================================================

/**
 * @brief 拦截 cuEventRecord 的核心实现
 * 
 * 将录制事件包装为 CudaEventRecordCommand 命令对象：
 *   - 若在图捕获中或流未受管：直接穿透到底层驱动 `LaunchWrapper`。
 *   - 若受 XQueue 管辖：提交到队列等待被 LaunchWorker 触发，并登记入全局 `g_events`。
 */
static CUresult XEventRecordImpl(std::shared_ptr<CudaEventRecordCommand> xevent,
                                 CUevent event, CUstream stream)
{
    CUresult result;
    if (GetCaptureCounter() > 0) {
        result = xevent->LaunchWrapper(stream);
    } else if (stream == CU_STREAM_LEGACY) {
        WaitBlockingXQueues();
        result = xevent->LaunchWrapper(stream);
    } else {
        auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
        if (xq == nullptr) {
            result = xevent->LaunchWrapper(stream);
        } else {
            xq->Submit(xevent);
            result = CUDA_SUCCESS;
        }
    }
    // 登记至全局映射表，供后续 EventQuery / WaitEvent 查询关联
    g_events.Add(event, xevent);
    return result;
}

CUresult XEventRecord(CUevent event, CUstream stream)
{
    XDEBG("XEventRecord(event: %p, stream: %p)", event, stream);
    CONVERT_STREAM(stream);
    if (event == nullptr) return Driver::EventRecord(event, stream);
    auto xevent = std::make_shared<CudaEventRecordCommand>(event);
    return XEventRecordImpl(xevent, event, stream);
}

CUresult XEventRecord_ptsz(CUevent event, CUstream stream)
{
    XDEBG("XEventRecord_ptsz(event: %p, stream: %p)", event, stream);
    CONVERT_STREAM_PTSZ(stream);
    if (event == nullptr) return Driver::EventRecord(event, stream);
    auto xevent = std::make_shared<CudaEventRecordCommand>(event);
    return XEventRecordImpl(xevent, event, stream);
}

CUresult XEventRecordWithFlags(CUevent event, CUstream stream, unsigned int flags)
{
    XDEBG("XEventRecordWithFlags(event: %p, stream: %p, flags: %u)", event, stream, flags);
    CONVERT_STREAM(stream);
    if (event == nullptr) return Driver::EventRecordWithFlags(event, stream, flags);
    auto xevent = std::make_shared<CudaEventRecordWithFlagsCommand>(event, flags);
    return XEventRecordImpl(xevent, event, stream);
}

CUresult XEventRecordWithFlags_ptsz(CUevent event, CUstream stream, unsigned int flags)
{
    XDEBG("XEventRecordWithFlags_ptsz(event: %p, stream: %p, flags: %u)", event, stream, flags);
    CONVERT_STREAM_PTSZ(stream);
    if (event == nullptr) return Driver::EventRecordWithFlags(event, stream, flags);
    auto xevent = std::make_shared<CudaEventRecordWithFlagsCommand>(event, flags);
    return XEventRecordImpl(xevent, event, stream);
}

/**
 * @brief 拦截 cuEventQuery (主机端非阻塞查询事件是否已完成)
 * 
 * 逻辑：
 *   1. 查表看该事件是否关联了 XSched 的 CudaEventRecordCommand。
 *   2. 如果该事件不在 XQueue 中，穿透到底层物理驱动查询。
 *   3. 如果在 XQueue 中，检查命令状态机：
 *      - 若 >= kCommandStateCompleted（物理执行完毕），返回 CUDA_SUCCESS；
 *      - 否则返回 CUDA_ERROR_NOT_READY。
 */
CUresult XEventQuery(CUevent event)
{
    XDEBG("XEventQuery(event: %p)", event);
    if (event == nullptr) return Driver::EventQuery(event);
    auto xevent = g_events.Get(event, nullptr);
    if (xevent == nullptr) return Driver::EventQuery(event);
    // If the event is not recorded on an XQueue, directly query from CUDA driver.
    if (xevent->GetXQueueHandle() == 0) return Driver::EventQuery(event);

    auto state = xevent->GetState();
    if (state >= kCommandStateCompleted) return CUDA_SUCCESS;
    return CUDA_ERROR_NOT_READY;
}

/**
 * @brief 拦截 cuEventSynchronize (主机端阻塞等待事件完成)
 */
CUresult XEventSynchronize(CUevent event)
{
    XDEBG("XEventSynchronize(event: %p)", event);
    if (event == nullptr) return Driver::EventSynchronize(event);

    auto xevent = g_events.Get(event, nullptr);
    if (xevent == nullptr) return Driver::EventSynchronize(event);

    // 阻塞等待直到该事件命令在物理硬件上执行结束
    xevent->Wait();
    return CUDA_SUCCESS;
}

/**
 * @brief 拦截 cuStreamWaitEvent (流跨流等待事件)
 * 
 * 典型跨流依赖场景（如数据拷贝流完成后，计算流开始执行）：
 *   1. 若目标流是 Legacy Stream，先排空所有阻塞队列并同步等待事件；
 *   2. 若目标流由 XQueue 管理，则构造 CudaEventWaitCommand 并压入目标队列，
 *      让下发工作线程在下发后续 Kernel 之前自动遵守跨流同步等待；
 *   3. 若目标流未被管辖，则调用驱动的 StreamWaitEvent。
 */
static CUresult XStreamWaitEventImpl(CUstream stream, CUevent event, unsigned int flags)
{
    if (event == nullptr) return Driver::StreamWaitEvent(stream, event, flags);
    CHECK_STREAM_CAPTURE(stream, Driver::StreamWaitEvent(stream, event, flags));

    auto xevent = g_events.Get(event, nullptr);
    // 尚未在任何流上 record 过，直接透传驱动
    if (xevent == nullptr) return Driver::StreamWaitEvent(stream, event, flags);

    if (stream == CU_STREAM_LEGACY) {
        // 在 Legacy 默认流上等待事件
        WaitBlockingXQueues();
        xevent->Wait();
        return Driver::StreamWaitEvent(stream, event, flags);
    }

    auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    if (xq == nullptr) {
        // 等待方流并非由 XQueue 管理
        if (xevent->GetXQueueHandle() == 0) {
            // 被等待的事件也未在 XQueue 上录制
            return Driver::StreamWaitEvent(stream, event, flags);
        }
        xevent->Wait();
        return CUDA_SUCCESS;
    }

    // 目标流受 XQueue 管理：生成等待命令压入目标流的队列中
    auto cmd = std::make_shared<CudaEventWaitCommand>(xevent, flags);
    xq->Submit(cmd);
    return CUDA_SUCCESS;
}

CUresult XStreamWaitEvent(CUstream stream, CUevent event, unsigned int flags)
{
    XDEBG("XStreamWaitEvent(stream: %p, event: %p, flags: %u)", stream, event, flags);
    CONVERT_STREAM(stream);
    return XStreamWaitEventImpl(stream, event, flags);
}

CUresult XStreamWaitEvent_ptsz(CUstream stream, CUevent event, unsigned int flags)
{
    XDEBG("XStreamWaitEvent_ptsz(stream: %p, event: %p, flags: %u)", stream, event, flags);
    CONVERT_STREAM_PTSZ(stream);
    return XStreamWaitEventImpl(stream, event, flags);
}

/**
 * @brief 拦截 cuEventDestroy
 * 
 * 依据 CUDA 官方规范：如果被销毁的事件当前仍被某个流在 StreamWaitEvent 中依赖，
 * 不能立即从显卡驱动中硬销毁该事件，否则会引发严重硬件未定义行为。
 * XSched 的做法是：从全局 g_events 移除，若该命令仍在排队，标记其在析构时才物理释放。
 */
CUresult XEventDestroy(CUevent event)
{
    XDEBG("XEventDestroy(event: %p)", event);
    if (event == nullptr) return Driver::EventDestroy(event);

    auto xevent = g_events.DoThenDel(event, nullptr, [](auto xevent) {
        // https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__EVENT.html#group__CUDA__EVENT_1g593ec73a8ec5a5fc031311d3e4dca1ef
        // According to CUDA driver API documentation, if the event is waiting in XQueues,
        // we should not destroy it immediately. Instead, we shall set a flag to destroy
        // the CUevent in the destructor of the xevent.
        xevent->DestroyEvent();
    });
    if (xevent == nullptr) return Driver::EventDestroy(event);
    return CUDA_SUCCESS;
}

CUresult XEventDestroy_v2(CUevent event)
{
    XDEBG("XEventDestroy_v2(event: %p)", event);
    if (event == nullptr) return Driver::EventDestroy_v2(event);

    auto xevent = g_events.DoThenDel(event, nullptr, [](auto xevent) {
        // Same as XEventDestroy.
        xevent->DestroyEvent();
    });
    if (xevent == nullptr) return Driver::EventDestroy_v2(event);
    return CUDA_SUCCESS;
}

// ============================================================================
// 7. 流同步与状态查询 (Stream Synchronization & Query)
// ============================================================================

/**
 * @brief 拦截 cuStreamSynchronize 的核心实现
 * 
 * 调用 xq->WaitAll()，阻塞等待该队列缓冲中的所有命令全部完成硬件下发与执行。
 */
static inline CUresult XStreamSynchronizeImpl(CUstream stream)
{
    auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    if (xq == nullptr) return Driver::StreamSynchronize(stream);
    xq->WaitAll();
    return CUDA_SUCCESS;
}

CUresult XStreamSynchronize(CUstream stream)
{
    XDEBG("XStreamSynchronize(stream: %p)", stream);
    CHECK_STREAM(stream, Driver::StreamSynchronize(stream));
    return XStreamSynchronizeImpl(stream);
}

CUresult XStreamSynchronize_ptsz(CUstream stream)
{
    XDEBG("XStreamSynchronize_ptsz(stream: %p)", stream);
    CHECK_STREAM_PTSZ(stream, Driver::StreamSynchronize(stream));
    return XStreamSynchronizeImpl(stream);
}

/**
 * @brief 拦截 cuStreamQuery (主机端非阻塞查询流是否已经空闲)
 * 
 * 映射 XQueue 内部状态：
 *   - kQueueStateIdle: 队列已空闲，返回 CUDA_SUCCESS
 *   - kQueueStateReady: 队列仍有积压任务，返回 CUDA_ERROR_NOT_READY
 *   - 其他情况：回退到底层驱动查询
 */
static inline CUresult XStreamQueryImpl(CUstream stream)
{
    auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    if (xq == nullptr) return Driver::StreamQuery(stream);

    switch (xq->Query())
    {
    case kQueueStateIdle:
        return CUDA_SUCCESS;
    case kQueueStateReady:
        return CUDA_ERROR_NOT_READY;
    default:
        return Driver::StreamQuery(stream);
    }
}

CUresult XStreamQuery(CUstream stream)
{
    XDEBG("XStreamQuery(stream: %p)", stream);
    CHECK_STREAM(stream, Driver::StreamQuery(stream));
    return XStreamQueryImpl(stream);
}

CUresult XStreamQuery_ptsz(CUstream stream)
{
    XDEBG("XStreamQuery_ptsz(stream: %p)", stream);
    CHECK_STREAM_PTSZ(stream, Driver::StreamQuery(stream));
    return XStreamQueryImpl(stream);
}

/**
 * @brief 拦截 cuCtxSynchronize (全 Context 阻塞等待)
 * 
 * 调用 XQueueManager::ForEachWaitAll()，等待进程中所有 XQueue 的全部命令完成，
 * 再调用真实的 Driver::CtxSynchronize()。
 */
CUresult XCtxSynchronize()
{
    XDEBG("XCtxSynchronize()");
    XQueueManager::ForEachWaitAll();
    return Driver::CtxSynchronize();
}

// ============================================================================
// 8. 流创建与销毁 (Stream Lifecycle & Single-Stream Mode)
// ============================================================================

// 单物理流复用模式 (Single Stream Per Process) 的全局管理变量
static std::mutex g_single_stream_mutex;
static CUstream g_single_stream = nullptr;
static int64_t g_single_stream_ref_cnt = 0;

/**
 * @brief 拦截 cuStreamCreate
 * 
 * 1. 默认模式：
 *    - 调用 Driver::StreamCreate 创建真实物理流；
 *    - 调用 XQueueManager::AutoCreate 自动为此流注册 HwQueue 并建立对应的 XQueue。
 * 2. 单流模式 (GetCudaSingleStreamPerProcessEnabled):
 *    - 无论上层创建多少个流，底层只创建 1 个物理 Stream 并维护引用计数，
 *      让多流串行化/集中排队（在特定单卡切片或排错调试场景非常有效）。
 */
CUresult XStreamCreate(CUstream *stream, unsigned int flags)
{
    if (!GetCudaSingleStreamPerProcessEnabled()) {
        CUresult res = Driver::StreamCreate(stream, flags);
        if (res != CUDA_SUCCESS) return res;
        // 自动绑定创建 XQueue
        XQueueManager::AutoCreate([&](HwQueueHandle *hwq) {return CudaQueueCreate(hwq, *stream);});
        XDEBG("XStreamCreate(stream: %p, flags: 0x%x) = %d", *stream, flags, res);
        return res;
    }

    std::lock_guard<std::mutex> lock(g_single_stream_mutex);
    if (g_single_stream_ref_cnt == 0) {
        CUresult res = Driver::StreamCreate(stream, flags);
        if (res != CUDA_SUCCESS) return res;
        XQueueManager::AutoCreate([&](HwQueueHandle *hwq) {return CudaQueueCreate(hwq, *stream);});
        g_single_stream = *stream;
    }

    g_single_stream_ref_cnt++;
    *stream = g_single_stream;
    XDEBG("XStreamCreate(single stream: %p (ref: %ld), flags: 0x%x)",
          *stream, (long)g_single_stream_ref_cnt, flags);
    return CUDA_SUCCESS;
}

/**
 * @brief 拦截 cuStreamCreateWithPriority (带优先级的流创建)
 */
CUresult XStreamCreateWithPriority(CUstream *stream, unsigned int flags, int priority)
{
    if (!GetCudaSingleStreamPerProcessEnabled()) {
        CUresult res = Driver::StreamCreateWithPriority(stream, flags, priority);
        if (res != CUDA_SUCCESS) return res;
        XQueueManager::AutoCreate([&](HwQueueHandle *hwq) {return CudaQueueCreate(hwq, *stream);});
        XDEBG("XStreamCreateWithPriority(stream: %p, flags: 0x%x, priority: %d) = %d",
              *stream, flags, priority, res);
        return res;
    }

    std::lock_guard<std::mutex> lock(g_single_stream_mutex);
    if (g_single_stream_ref_cnt == 0) {
        CUresult res = Driver::StreamCreateWithPriority(stream, flags, priority);
        if (res != CUDA_SUCCESS) return res;
        XQueueManager::AutoCreate([&](HwQueueHandle *hwq) {return CudaQueueCreate(hwq, *stream);});
        g_single_stream = *stream;
    }

    g_single_stream_ref_cnt++;
    *stream = g_single_stream;
    XDEBG("XStreamCreateWithPriority(single stream: %p (ref: %ld), flags: 0x%x, priority: %d)",
          *stream, (long)g_single_stream_ref_cnt, flags, priority);
    return CUDA_SUCCESS;
}

/**
 * @brief 拦截 cuStreamDestroy
 * 
 * 1. 自动销毁绑定的软件调度队列 XQueue (AutoDestroy)；
 * 2. 调用 Driver::StreamDestroy 销毁物理硬件流。
 */
CUresult XStreamDestroy(CUstream stream)
{
    if (!GetCudaSingleStreamPerProcessEnabled()) {
        XDEBG("XStreamDestroy(stream: %p)", stream);
        XQueueManager::AutoDestroy(GetHwQueueHandle(stream));
        return Driver::StreamDestroy(stream);
    }

    CUresult res = CUDA_SUCCESS;
    std::lock_guard<std::mutex> lock(g_single_stream_mutex);
    g_single_stream_ref_cnt--;
    if (g_single_stream_ref_cnt == 0) {
        XQueueManager::AutoDestroy(GetHwQueueHandle(stream));
        res = Driver::StreamDestroy(g_single_stream);
        g_single_stream = nullptr;
    }
    XDEBG("XStreamDestroy(single stream: %p (ref: %ld)) = %d",
          stream, (long)g_single_stream_ref_cnt, res);
    return res;
}

/**
 * @brief 拦截 cuStreamDestroy_v2
 */
CUresult XStreamDestroy_v2(CUstream stream)
{
    if (!GetCudaSingleStreamPerProcessEnabled()) {
        XDEBG("XStreamDestroy_v2(stream: %p)", stream);
        XQueueManager::AutoDestroy(GetHwQueueHandle(stream));
        return Driver::StreamDestroy_v2(stream);
    }

    CUresult res = CUDA_SUCCESS;
    std::lock_guard<std::mutex> lock(g_single_stream_mutex);
    g_single_stream_ref_cnt--;
    if (g_single_stream_ref_cnt == 0) {
        XQueueManager::AutoDestroy(GetHwQueueHandle(stream));
        res = Driver::StreamDestroy_v2(g_single_stream);
        g_single_stream = nullptr;
    }
    XDEBG("XStreamDestroy_v2(single stream: %p (ref: %ld)) = %d",
          stream, (long)g_single_stream_ref_cnt, res);
    return res;
}

} // namespace xsched::cuda

