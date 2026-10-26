#include <mutex>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cuxtra/cuxtra.h>

#include "xsched/cuda/hal/level3/interrupt.h"
#include "xsched/cuda/hal/level2/instrument.h"
#include "xsched/cuda/hal/common/cuda_assert.h"
#include "xsched/cuda/hal/level3/trap_sm120.h"

using namespace xsched::cuda;

InterruptContext::InterruptContext(CUcontext ctx)
    : kCtx(ctx), kOpStream(InstrumentContext::Instance(ctx)->OpStream())
{
    // check current context
    CUcontext current_ctx;
    CUDA_ASSERT(Driver::CtxGetCurrent(&current_ctx));
    XASSERT(current_ctx == ctx,
            "create InterruptContext failed: current context (%p) does not match context (%p)",
            current_ctx, ctx);

    CUdevice dev;
    CUDA_ASSERT(Driver::CtxGetDevice(&dev));
    trap_handler_ = TarpHandler::Instance(dev);
    XASSERT(trap_handler_ != nullptr, "no trap handler implementation for this architecture");
    trap_handler_->GetTrapHandlerInfo(kCtx, &trap_handler_dev_, &trap_handler_size_);
    // Diagnostic bisect (FINDINGS §24/§26): the instruction-memory allocator
    // may be what turns 0x83de0317 from "trap to the handler" into "cancel
    // in-flight work" on a managed context.
    if (getenv("XSCHED_SM120_TRAP_MIN_CTX") == nullptr &&
        getenv("XSCHED_SM120_TRAP_NO_ICTX_INSTRMEM") == nullptr)
        instrument_mem_ = std::make_unique<InstrMemAllocator>(kCtx, dev);

    int major = 0, minor = 0;
    Driver::DeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
    Driver::DeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
    sm120_trap_ = (major * 10 + minor) == 120;
    if (sm120_trap_) {
        // Handshake/authorisation must happen before kernels run: the agent
        // creates its 0x83de session and sets the debug modes here.
        if (!TrapSm120Context::Instance().Init(kCtx, kOpStream)) {
            XWARN("sm120 trap backend is not ready");
        }
        // Install XSched's trap payload at the handler return stub (+0x880)
        // before any kernel runs.  The payload itself decides between a
        // benign return and an EXIT using the preempt buffer + killable flag;
        // the working trigger is the RM 0x83de0317 trap, not cuXtraTriggerTrap.
        if (getenv("XSCHED_SM120_TRAP_NO_PAYLOAD") == nullptr) InstrumentTrapHandler();
        else XINFO("sm120 trap: payload disabled (NO_PAYLOAD), using the driver handler only");
    }
}

void InterruptContext::InstrumentTrapHandler()
{
    static std::mutex init_mtx;
    static bool initialized = false;
    std::lock_guard<std::mutex> lock(init_mtx);
    if (initialized) return;
    initialized = true;

    // used for debugging
    // DumpTrapHandler();

    char *trap_handler_host = (char *)malloc(trap_handler_size_);
    // alloc host and device memory for injected instructions
    size_t inject_size = trap_handler_->GetInjectSize();
    char *inject_host = (char *)malloc(inject_size);
    if (instrument_mem_ == nullptr) {
        XWARN("trap handler instrumentation skipped (no instrument memory)");
        free(inject_host);
        return;
    }
    CUdeviceptr inject_dev = instrument_mem_->Alloc(inject_size);

    // copy the original trap handler instructions to host
    cuXtraMemcpyDtoH(trap_handler_host, trap_handler_dev_, trap_handler_size_, kOpStream);

    // get the instrumented trap handler instructions and injected instructions
    trap_handler_->Instrument(trap_handler_host, trap_handler_dev_, trap_handler_size_,
                              inject_host, inject_dev);

    // copy the injected instructions to device
    cuXtraInstrMemcpyHtoD(inject_dev, inject_host, inject_size, kOpStream);
    // copy the instrumented trap handler instructions to device
    cuXtraMemcpyHtoD(trap_handler_dev_, trap_handler_host, trap_handler_size_, kOpStream);

    cuXtraInvalInstrCache(kCtx);
    XINFO("trap handler instrumented: dev %#llx size %zu inject %#llx size %zu",
          (unsigned long long)trap_handler_dev_, trap_handler_size_,
          (unsigned long long)inject_dev, inject_size);
    free(trap_handler_host);
    free(inject_host);
}

void InterruptContext::Interrupt()
{
    XINFO("triggering trap (ctx %p)", kCtx);
    if (sm120_trap_) {
        bool ok = TrapSm120Context::Instance().Trigger();
        XINFO("sm120 trap triggered: %s", ok ? "ok" : "FAILED");
        return;
    }
    cuXtraTriggerTrap(kCtx);
    XINFO("trap triggered");
}

void InterruptContext::DumpTrapHandler()
{
    char *trap_handler_host = (char *)malloc(trap_handler_size_);
    // copy trap handler instructions to host
    cuXtraMemcpyDtoH(trap_handler_host, trap_handler_dev_, trap_handler_size_, kOpStream);

    std::stringstream filename;
    filename << "trap_handler_0x" << std::hex << trap_handler_dev_ << ".bin";
    std::ofstream out_file(filename.str(), std::ios::binary);
    out_file.write(trap_handler_host, trap_handler_size_);
    out_file.close();
    free(trap_handler_host);
    XINFO("dumped trap handler in %s", filename.str().c_str());
}

std::shared_ptr<InterruptContext> InterruptContext::Instance(CUcontext ctx)
{
    static std::mutex ctx_mtx;
    static std::map<CUcontext, std::shared_ptr<InterruptContext>> ctx_map;

    std::lock_guard<std::mutex> lock(ctx_mtx);
    auto it = ctx_map.find(ctx);
    if (it != ctx_map.end()) return it->second;

    auto interrupt_ctx = std::make_shared<InterruptContext>(ctx);
    ctx_map[ctx] = interrupt_ctx;
    return interrupt_ctx;
}
