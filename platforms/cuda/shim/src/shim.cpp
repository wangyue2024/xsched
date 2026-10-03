/**
 * @file shim.cpp
 * @brief XSched CUDA driver API interception layer (CUDA Driver Shim Implementation)
 *
 * This file is the core hub that transparently hijacks the NVIDIA CUDA Driver
 * API. It impersonates the official nvcuda.dll / libcuda.so, intercepts the
 * driver API calls of applications (e.g., PyTorch, TensorFlow), converts them
 * into XSched software commands (HwCommand), and controls their actual physical
 * dispatch according to the XQueue state machine.
 *
 * Structural overview:
 *   1. [Global event map]:      tracks the relationship between CUevent and the
 *                               corresponding record command (g_events).
 *   2. [Default & blocking streams]: per-thread default stream (PTDS) creation
 *                               and Legacy Default Stream blocking semantics.
 *   3. [CUDA Graph capture]:    safety guarantees during capture
 *                               (CaptureBegin / CaptureEnd / Bypass).
 *   4. [Kernel launch]:         cuLaunchKernel / cuLaunchKernelEx /
 *                               cuLaunchHostFunc interception and enqueue.
 *   5. [Memory free safety]:    cuMemFree_v2 forces a global drain to prevent
 *                               use-after-free (UAF).
 *   6. [Event interception]:    cuEventRecord / Query / Synchronize /
 *                               StreamWaitEvent / Destroy.
 *   7. [Stream sync & query]:   cuStreamSynchronize / StreamQuery /
 *                               CtxSynchronize.
 *   8. [Stream lifecycle]:      cuStreamCreate / Destroy plus the
 *                               single-stream-per-process reuse optimization.
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
// 1. Global Event Map
// ============================================================================

/**
 * @brief Global event map: CUevent -> CudaEventRecordCommand
 *
 * In CUDA, cuEventRecord places a timing marker on a stream. Since XSched
 * dispatches commands asynchronously through software queues, this map records
 * the record command object bound to each CUevent so that the following can be
 * supported:
 *   - cross-stream waiting (cuStreamWaitEvent)
 *   - host-side event query (cuEventQuery)
 *   - host-side event synchronize (cuEventSynchronize)
 *   - deferred safe destruction (cuEventDestroy)
 */
static utils::ObjectMap<CUevent, std::shared_ptr<CudaEventRecordCommand>> g_events;

// ============================================================================
// 2. Stream Management & PTDS
// ============================================================================

/**
 * @brief Get the PTDS (Per-Thread Default Stream) dedicated to the current thread.
 *
 * CUDA supports two default-stream semantics:
 *   1. Legacy Default Stream (NULL / CU_STREAM_LEGACY): global blocking stream.
 *   2. Per-Thread Default Stream (CU_STREAM_PER_THREAD): an implicit
 *      non-blocking stream per thread.
 *
 * XSched allocates an explicit non-blocking physical stream per thread as its
 * PTDS and attaches an XQueue to it automatically, bringing the implicit
 * default stream under the management of the scheduler.
 *
 * @return The CUstream bound to the current thread.
 */
CUstream GetPTDS()
{
    /// FIXME: Here we assume that the thread will only use one single CUDA context.
    /// However, each CUDA context should have its own per-thread default stream.
    /// TODO: Destory the stream when the thread exits.
    static thread_local CUstream per_thread_default_stream = 0;
    if (per_thread_default_stream != 0) return per_thread_default_stream;

    CUstream stream = nullptr;
    // Create the underlying non-blocking physical stream.
    CUDA_ASSERT(Driver::StreamCreate(&stream, CU_STREAM_NON_BLOCKING));

    /// FIXME: If XSCHED_AUTO_XQUEUE is not turned on,
    /// there is no meaning creating new per-thread default streams.
    // Automatically register a HwQueue for the stream and create the software
    // scheduling queue (XQueue).
    XQueueManager::AutoCreate([&](HwQueueHandle *hwq) {return CudaQueueCreate(hwq, stream);});
    per_thread_default_stream = stream;
    return stream;
}

/**
 * @brief Wait until all blocking XQueues (non CU_STREAM_NON_BLOCKING) drain.
 *
 * Per the official CUDA semantics: when an operation is launched on the Legacy
 * Default Stream (NULL), it must implicitly wait for all previously issued
 * work on every other blocking stream of the device to complete; conversely,
 * subsequent work on all streams must wait for the default stream. This
 * function submits WaitAll to every blocking XQueue and synchronously waits so
 * that this semantic is strictly preserved.
 */
void WaitBlockingXQueues()
{
    std::list<std::shared_ptr<XQueueWaitAllCommand>> wait_cmds;
    // Iterate over every managed XQueue in the current process.
    XResult res = XQueueManager::ForEach([&](std::shared_ptr<XQueue> xq)->XResult {
        auto hwq = xq->GetHwQueue();
        auto cuda_q = std::dynamic_pointer_cast<CudaQueueLv1>(hwq);
        if (cuda_q == nullptr) return kXSchedErrorUnknown;
        // A stream carrying CU_STREAM_NON_BLOCKING does not need to be waited.
        if (cuda_q->GetStreamFlags() & CU_STREAM_NON_BLOCKING) return kXSchedSuccess;

        // Submit the drain command and collect the wait handle.
        auto wait_cmd = xq->SubmitWaitAll();
        if (wait_cmd == nullptr) return kXSchedErrorUnknown;
        wait_cmds.push_back(wait_cmd);
        return kXSchedSuccess;
    });
    XASSERT(res == kXSchedSuccess, "Fail to submit wait all commands");
    // Block until all commands of the blocking streams have completed.
    for (auto &cmd : wait_cmds) cmd->Wait();
}

// ============================================================================
// 3. CUDA Graph Capture Handling
// ============================================================================

static std::mutex g_capture_mutex;
static std::atomic<int64_t> g_capture_counter {0};

/**
 * @brief Enter the CUDA Graph capture state.
 *
 * Triggered when the application calls cuStreamBeginCapture. During capture,
 * all subsequent kernel launches and data transfers must NOT enter the XSched
 * queues; they must bypass the shim down to the driver so that they are
 * recorded into the CUDA Graph node structure.
 *
 * Key invariants:
 *   1. Capture safety: before the driver actually starts capturing, call
 *      XCtxSynchronize() to completely drain all previously queued or in-flight
 *      XQueue work, preventing accidental recording into the graph or capture
 *      stream state corruption.
 *   2. Ordering safety: ensure that previously submitted asynchronous work has
 *      completed before switching to bypass mode to avoid out-of-order execution.
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
    // On the first entry into capture, the whole context must be drained first.
    if (g_capture_counter.load() == 0) XCtxSynchronize();
    g_capture_counter.fetch_add(1);
}

/**
 * @brief Exit the CUDA Graph capture state.
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
 * @brief Get the current global in-progress graph capture counter.
 * @return A value greater than 0 means the system is currently capturing a graph.
 */
int64_t GetCaptureCounter()
{
    return g_capture_counter.load();
}

// ============================================================================
// 4. Kernel Launch Interception
// ============================================================================

/**
 * @brief Core template implementation for kernel launch interception.
 *
 * This is the common landing point of all kernel interception entry points
 * (cuLaunchKernel, cuLaunchKernelEx, ...):
 *   1. Check whether the given CUstream is managed by XSched (resolve the bound
 *      XQueue through the HwQueueHandle).
 *   2. If the stream is not managed (xq == nullptr), call DirectLaunch to issue
 *      the command straight to the hardware driver.
 *   3. If managed, pack the kernel and all of its launch parameters into a CmdT
 *      command object, call xq->Submit(kernel) to place it into the queue
 *      buffer, and return CUDA_SUCCESS immediately.
 *
 * @tparam CmdT The command class type (e.g., CudaKernelLaunchCommand).
 * @param stream The target CUDA stream.
 * @param args The variadic argument pack required to construct CmdT.
 * @return CUresult The execution result.
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
 * @brief Intercept the standard cuLaunchKernel.
 */
CUresult XLaunchKernel(CUfunction f,
                       unsigned int gdx, unsigned int gdy, unsigned int gdz,
                       unsigned int bdx, unsigned int bdy, unsigned int bdz,
                       unsigned int shmem, CUstream stream, void **params, void **extra)
{
    XDEBG("XLaunchKernel(func: %p, stream: %p, grid: [%u, %u, %u], block: [%u, %u, %u], "
          "shm: %u, params: %p, extra: %p)", f, stream, gdx, gdy, gdz, bdx, bdy, bdz,
          shmem, params, extra);
    // Convert the stream (handles NULL/Legacy streams, graph capture, etc.).
    CHECK_STREAM(stream, DirectLaunch(std::make_shared<CudaKernelLaunchCommand>(
        f, gdx, gdy, gdz, bdx, bdy, bdz, shmem, params, extra, false), stream));
    return XLaunchKernelImpl<CudaKernelLaunchCommand>(
        stream, f, gdx, gdy, gdz, bdx, bdy, bdz, shmem, params, extra);
}

/**
 * @brief Intercept the per-thread default stream variant cuLaunchKernel_ptsz.
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
 * @brief Intercept the extended variant cuLaunchKernelEx (kernels with dynamic
 *        cluster configuration, attributes, etc.).
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
 * @brief Intercept the per-thread default stream variant cuLaunchKernelEx_ptsz.
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
 * @brief Intercept cuLaunchHostFunc (submitting a CPU host callback into a
 *        CUDA stream).
 */
static inline CUresult XLaunchHostFuncImpl(CUstream stream, CUhostFn fn, void *data)
{
    // Note: the first argument of cuLaunchHostFunc is the stream.
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
 * @brief Intercept cuStreamEndCapture and safely leave graph capture mode.
 */
static inline CUresult XStreamEndCaptureImpl(CUstream stream, CUgraph *graph)
{
    CUstreamCaptureStatus before = CU_STREAM_CAPTURE_STATUS_NONE;
    CUresult qres = Driver::StreamIsCapturing(stream, &before);
    CUresult res = Driver::StreamEndCapture(stream, graph);
    if (res != CUDA_ERROR_STREAM_CAPTURE_UNMATCHED && // ended capture on the right stream
        qres == CUDA_SUCCESS && before != CU_STREAM_CAPTURE_STATUS_NONE) {
        // The stream was capturing before; check whether it really exited.
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
// 5. Memory Free Safety
// ============================================================================

/**
 * @brief Intercept cuMemFree_v2.
 *
 * In native CUDA semantics, cuMemFree must ensure that all asynchronous
 * operations on every stream referencing this address have completed before the
 * memory is physically released. XSched takes a conservative approach here:
 * trigger a WaitAll drain of all XQueues before freeing so that use-after-free
 * (UAF) crashes cannot happen while asynchronous commands are being dispatched.
 */
CUresult XMemFree_v2(CUdeviceptr dptr)
{
    /// TODO: Optimize this.
    /// In CUDA semantics, cuMemFree only waits for commands who use this memory.
    XQueueManager::ForEachWaitAll();
    return Driver::MemFree_v2(dptr);
}

// ============================================================================
// 6. Event Interception
// ============================================================================

/**
 * @brief Core implementation of cuEventRecord interception.
 *
 * Wrap the recorded event as a CudaEventRecordCommand object:
 *   - During graph capture or for unmanaged streams: pass through directly to
 *     the driver via LaunchWrapper.
 *   - For XQueue-managed streams: submit to the queue (to be launched by the
 *     LaunchWorker) and register into the global `g_events` table.
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
    // Register into the global map for later EventQuery / WaitEvent lookups.
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
 * @brief Intercept cuEventQuery (host-side non-blocking completion check).
 *
 * Logic:
 *   1. Look up whether the event is associated with an XSched
 *      CudaEventRecordCommand.
 *   2. If not in an XQueue, pass through to the physical driver query.
 *   3. If in an XQueue, check the command state machine:
 *      - >= kCommandStateCompleted (physically finished): return CUDA_SUCCESS;
 *      - otherwise return CUDA_ERROR_NOT_READY.
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
 * @brief Intercept cuEventSynchronize (host-side blocking wait for event completion).
 */
CUresult XEventSynchronize(CUevent event)
{
    XDEBG("XEventSynchronize(event: %p)", event);
    if (event == nullptr) return Driver::EventSynchronize(event);

    auto xevent = g_events.Get(event, nullptr);
    if (xevent == nullptr) return Driver::EventSynchronize(event);

    // Block until the event command has actually completed on the hardware.
    xevent->Wait();
    return CUDA_SUCCESS;
}

/**
 * @brief Intercept cuStreamWaitEvent (cross-stream event wait).
 *
 * Typical cross-stream dependency scenarios (e.g., a compute stream starts
 * after a copy stream completes):
 *   1. If the target stream is the Legacy Stream, drain all blocking queues
 *      first and synchronously wait for the event;
 *   2. If the target stream is managed by an XQueue, construct a
 *      CudaEventWaitCommand and push it into the target queue so that the
 *      launch worker honors the cross-stream wait before launching subsequent
 *      kernels;
 *   3. If the target stream is unmanaged, call the driver's StreamWaitEvent.
 */
static CUresult XStreamWaitEventImpl(CUstream stream, CUevent event, unsigned int flags)
{
    if (event == nullptr) return Driver::StreamWaitEvent(stream, event, flags);
    CHECK_STREAM_CAPTURE(stream, Driver::StreamWaitEvent(stream, event, flags));

    auto xevent = g_events.Get(event, nullptr);
    // Never recorded on any stream; pass through to the driver.
    if (xevent == nullptr) return Driver::StreamWaitEvent(stream, event, flags);

    if (stream == CU_STREAM_LEGACY) {
        // Waiting for the event on the Legacy default stream.
        WaitBlockingXQueues();
        xevent->Wait();
        return Driver::StreamWaitEvent(stream, event, flags);
    }

    auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    if (xq == nullptr) {
        // The waiting stream is not managed by an XQueue.
        if (xevent->GetXQueueHandle() == 0) {
            // The awaited event was not recorded on an XQueue either.
            return Driver::StreamWaitEvent(stream, event, flags);
        }
        xevent->Wait();
        return CUDA_SUCCESS;
    }

    // The target stream is XQueue-managed: push a wait command into its queue.
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
 * @brief Intercept cuEventDestroy.
 *
 * Per the official CUDA semantics: if the event being destroyed is still being
 * awaited by some stream via StreamWaitEvent, it must not be hard-destroyed in
 * the driver immediately, otherwise undefined hardware behavior may occur.
 * XSched handles this by removing the event from the global g_events and, if
 * the command is still queued, marking it for physical release only at
 * destruction time.
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
// 7. Stream Synchronization & Query
// ============================================================================

/**
 * @brief Core implementation of cuStreamSynchronize interception.
 *
 * Calls xq->WaitAll() to block until all commands in the queue buffer have been
 * launched to and executed by the hardware.
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
 * @brief Intercept cuStreamQuery (host-side non-blocking idle check).
 *
 * Maps the internal XQueue state:
 *   - kQueueStateIdle:  queue idle, return CUDA_SUCCESS
 *   - kQueueStateReady: backlog remains, return CUDA_ERROR_NOT_READY
 *   - otherwise: fall back to the driver query
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
 * @brief Intercept cuCtxSynchronize (whole-context blocking wait).
 *
 * Calls XQueueManager::ForEachWaitAll() to wait for all commands of every
 * XQueue in the process, then calls the real Driver::CtxSynchronize().
 */
CUresult XCtxSynchronize()
{
    XDEBG("XCtxSynchronize()");
    XQueueManager::ForEachWaitAll();
    return Driver::CtxSynchronize();
}

// ============================================================================
// 8. Stream Lifecycle & Single-Stream Mode
// ============================================================================

// Global state of the single physical stream reuse mode (single stream per process).
static std::mutex g_single_stream_mutex;
static CUstream g_single_stream = nullptr;
static int64_t g_single_stream_ref_cnt = 0;

/**
 * @brief Intercept cuStreamCreate.
 *
 * 1. Default mode:
 *    - Call Driver::StreamCreate to create the real physical stream;
 *    - Call XQueueManager::AutoCreate to register a HwQueue for this stream and
 *      establish the corresponding XQueue.
 * 2. Single-stream mode (GetCudaSingleStreamPerProcessEnabled):
 *    - No matter how many streams the application creates, only one physical
 *      stream is created and reference-counted, serializing/centralizing the
 *      queues (useful for single-device slicing or debugging scenarios).
 */
CUresult XStreamCreate(CUstream *stream, unsigned int flags)
{
    if (!GetCudaSingleStreamPerProcessEnabled()) {
        CUresult res = Driver::StreamCreate(stream, flags);
        if (res != CUDA_SUCCESS) return res;
        // Automatically create and bind the XQueue.
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
 * @brief Intercept cuStreamCreateWithPriority (stream creation with priority).
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
 * @brief Intercept cuStreamDestroy.
 *
 * 1. Automatically destroy the bound software scheduling queue XQueue
 *    (AutoDestroy);
 * 2. Call Driver::StreamDestroy to destroy the physical hardware stream.
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
 * @brief Intercept cuStreamDestroy_v2.
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
