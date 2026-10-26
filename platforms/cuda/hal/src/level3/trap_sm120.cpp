// trap_sm120.cpp - XSched level-3 trap backend for Blackwell sm120 (Linux).
//
// Mechanism (validated in test/sm120_l3_trap_v2, FINDINGS_ZH.md v2.0):
//   * the live driver .text.traphandler (arena base + 0x15f0f00) is patched at
//     +0x390 with CALL.ABS to a cave in the slot-20 object (+0xA00);
//   * the cave is a harmless return stub by default.  To preempt, it is
//     rewritten to "redirect the saved TRAP_RETURN_PC to an EXIT trampoline"
//     (cave+0x70).  The handler finishes restoring the trap state first, so the
//     warp exits cleanly and the context survives;
//   * an external agent holds a 0x83de session bound to {app client, KGO} and
//     sends 0x83de0317{0,0} when asked; the app grants it access with the
//     NV01_ROOT 0xd04 inherited-share policy (late grant works).
//
// The app and agent talk over a POSIX shm control block (trap_sm120_shm.h).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <csetjmp>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <time.h>
#include <vector>
#include <string>
#include <link.h>
#include <thread>

#include <cuxtra/cuxtra.h>

#include "xsched/utils/log.h"
#include "xsched/cuda/hal/common/driver.h"
#include "xsched/cuda/hal/level3/trap_sm120.h"
#include "xsched/cuda/hal/level3/trap_sm120_shm.h"

using namespace xsched::cuda;

// ------------------------------------------------------------------ RM ABI
struct NVOS21 { uint32_t hRoot, hParent, hObject, hClass;
                uint64_t pAllocParms, pRightsRequested;
                uint32_t paramsSize, flags; int32_t status; };
struct NVOS54 { uint32_t hClient, hObject, cmd, flags;
                uint64_t params; uint32_t paramsSize; int32_t status; };
static_assert(sizeof(NVOS21) == 48 && sizeof(NVOS54) == 32, "RM ABI");

static const unsigned long NV_ESC_RM_ALLOC   = 0xc030462bUL;
static const unsigned long NV_ESC_RM_CONTROL = 0xc020462aUL;
static const uint32_t CLASS_ROOT = 0x41, CLASS_DEVICE = 0x80, CLASS_SUBDEV = 0x2080,
                      CLASS_COMPUTE = 0xcec0, CLASS_SMDBGSSN = 0x83de;

namespace {
struct Handles {
    int fd = -1;
    uint32_t client = 0, device = 0, subdev = 0, session = 0;
    uint32_t ncompute = 0, computes[L3_TRAP_MAX_KGO] = {};
} H;

int (*real_ioctl)(int, unsigned long, ...) = nullptr;
__thread int in_hook = 0;

void LearnAlloc(int fd, void *arg)
{
    NVOS21 a; memcpy(&a, arg, sizeof a);
    if (a.status != 0) return;
    switch (a.hClass) {
    case CLASS_ROOT:
        if (H.fd < 0) H.fd = fd;
        if (!H.client) H.client = a.hObject;
        break;
    case CLASS_DEVICE:
        if (!H.device) { H.device = a.hObject; H.client = a.hRoot; H.fd = fd; }
        break;
    case CLASS_SUBDEV:
        if (!H.subdev) H.subdev = a.hObject;
        break;
    case CLASS_COMPUTE:
        if (H.ncompute < L3_TRAP_MAX_KGO) H.computes[H.ncompute++] = a.hObject;
        break;
    case CLASS_SMDBGSSN:
        if (!H.session) H.session = a.hObject;
        break;
    }
}
} // namespace

extern "C" int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    const char *backend = getenv("XSCHED_SM120_L3_BACKEND");
    if (backend == nullptr || strcmp(backend, "TRAP") != 0) return real_ioctl(fd, req, arg);
    int r = real_ioctl(fd, req, arg);
    if (in_hook || r || !arg || req != NV_ESC_RM_ALLOC) return r;
    in_hook = 1; LearnAlloc(fd, arg); in_hook = 0;
    return r;
}

// ------------------------------------------------------------ helpers
static int rm_control(int fd, uint32_t client, uint32_t obj, uint32_t cmd, void *p, uint32_t n)
{
    NVOS54 c; memset(&c, 0, sizeof c);
    c.hClient = client; c.hObject = obj; c.cmd = cmd;
    c.params = (uint64_t)(uintptr_t)p; c.paramsSize = n;
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    int r = real_ioctl(fd, NV_ESC_RM_CONTROL, &c);
    return r ? (0x1000000 | r) : c.status;
}

static CUuuid UuidOf(const char *hex)
{
    CUuuid u;
    for (int i = 0; i < 16; i++) { unsigned v; sscanf(hex + i * 2, "%2x", &v); u.bytes[i] = (char)v; }
    return u;
}
static uint32_t OpcodeAt(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return (uint32_t)w; }
static uint32_t Imm32At(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return (uint32_t)(w >> 32); }

namespace {
struct TrapState {
    bool inited = false, patched = false;
    CUcontext ctx = nullptr;
    CUstream stream = nullptr;
    uint64_t handler_va = 0, cave_va = 0, live_base = 0;
    uint64_t ret_stub[16], armed_stub[16];
    uint8_t orig_390[16] = {};
    uint64_t call_words[2] = {};
    int agent_pid = -1;
    L3TrapShm *shm = nullptr;
    char shm_name[64] = {};
    uint64_t drv_preempt_buf = 0;   // driver-patched preemption buffer (handler reads it)
    uint64_t scratch_buf = 0;       // driver-patched scratchpad (host rw-s mapping)
    // device-side cave writer (host instruction writes serialise behind the
    // app's running kernels, a concurrent kernel does not)
    CUmodule wr_mod = nullptr;
    CUfunction wr_fn = nullptr;
    CUstream wr_stream = nullptr;
    int wr_lowpri = 0, wr_highpri = 0;
    CUdeviceptr wr_buf = 0;      // [0,0x80) return stub, [0x80,0x100) armed
    // Host-mapped record page the trap can store into; the CPU reads it
    // directly, so probe results survive a parked/debug-state context.
    volatile uint32_t *rec_host = nullptr;
    CUdeviceptr rec_dev = 0;
};
TrapState T;
} // namespace

// A concurrent kernel that copies one of the two stubs into the cave.  Host
// copies to instruction memory wait for the app's blocking streams; a kernel
// launched on our own non-blocking stream does not.
static const char *kWriterPtx =
    ".version 8.7\n.target sm_120\n.address_size 64\n"
    ".visible .entry l3write(.param .u64 p, .param .u64 src)\n{\n"
    "  .reg .u32 %rtid, %r0;\n"
    "  .reg .u64 %rd1, %rd2, %rd3, %rd4, %rd5;\n"
    "  ld.param.u64 %rd1, [p];\n"
    "  ld.param.u64 %rd2, [src];\n"
    "  mov.u32 %rtid, %tid.x;\n"
    "  mul.wide.u32 %rd3, %rtid, 4;\n"
    "  add.s64 %rd4, %rd1, %rd3;\n"
    "  add.s64 %rd5, %rd2, %rd3;\n"
    "  ld.global.u32 %r0, [%rd5];\n"
    "  st.global.u32 [%rd4], %r0;\n"
    "  membar.sys;\n"
    "  ret;\n}\n";

// cuMemHostAlloc is not part of the generated Driver wrapper table.
static CUresult HostAllocPinned(void **p, size_t n, unsigned flags)
{
    static CUresult (*fn)(void **, size_t, unsigned) = nullptr;
    if (fn == nullptr) {
        void *h = dlopen("libcuda.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (h == nullptr) h = dlopen("libcuda.so", RTLD_NOW | RTLD_NOLOAD);
        if (h != nullptr) {
            fn = (CUresult (*)(void **, size_t, unsigned))dlsym(h, "cuMemHostAlloc");
        }
        if (fn == nullptr) return CUDA_ERROR_NOT_FOUND;
    }
    return fn(p, n, flags);
}

static bool InitWriter()
{
    if (T.wr_fn != nullptr) return true;
    CUresult r;
    if ((r = Driver::ModuleLoadData(&T.wr_mod, kWriterPtx)) != CUDA_SUCCESS) {
        XWARN("sm120 trap: cave writer module load -> %d", (int)r);
        return false;
    }
    if ((r = Driver::ModuleGetFunction(&T.wr_fn, T.wr_mod, "l3write")) != CUDA_SUCCESS) {
        XWARN("sm120 trap: cave writer function -> %d", (int)r);
        T.wr_fn = nullptr;
        return false;
    }
    if ((r = Driver::CtxGetStreamPriorityRange(&T.wr_lowpri, &T.wr_highpri)) != CUDA_SUCCESS)
        T.wr_highpri = 0;
    if ((r = Driver::StreamCreateWithPriority(&T.wr_stream, CU_STREAM_NON_BLOCKING,
                                              T.wr_highpri)) != CUDA_SUCCESS) {
        XWARN("sm120 trap: cave writer stream -> %d", (int)r);
        T.wr_fn = nullptr;
        return false;
    }
    if ((r = Driver::MemAlloc_v2(&T.wr_buf, 0x200)) != CUDA_SUCCESS) {
        XWARN("sm120 trap: cave writer buffer -> %d", (int)r);
        T.wr_fn = nullptr;
        return false;
    }
    {
        void *hp = nullptr;
        if (HostAllocPinned(&hp, 0x1000, CU_MEMHOSTALLOC_DEVICEMAP) == CUDA_SUCCESS) {
            memset(hp, 0, 0x1000);
            T.rec_host = (volatile uint32_t *)hp;
            // UVA: the device address of a host allocation is the host VA.
            T.rec_dev = (CUdeviceptr)(uintptr_t)hp;
            XINFO("sm120 trap: record page host=%p dev=%#llx", hp,
                  (unsigned long long)T.rec_dev);
        } else {
            XWARN("sm120 trap: host record page unavailable");
        }
    }
    r = Driver::MemcpyHtoD_v2_ptds(T.wr_buf, T.ret_stub, sizeof T.ret_stub);
    if (r == CUDA_SUCCESS)
        r = Driver::MemcpyHtoD_v2_ptds(T.wr_buf + 0x80, T.armed_stub, sizeof T.armed_stub);
    if (r != CUDA_SUCCESS) {
        XWARN("sm120 trap: cave writer stub copy -> %d", (int)r);
        T.wr_fn = nullptr;
        return false;
    }
    if (getenv("XSCHED_SM120_TRAP_REASON_PROBE") != nullptr) {
        std::thread([]() {
            sleep(4);
            Driver::CtxSetCurrent(T.ctx);
            uint32_t v[4] = {0, 0, 0, 0};
            CUresult rr = Driver::MemcpyDtoHAsync_v2_ptsz(v, T.wr_buf + 0x100,
                                                          sizeof v, T.wr_stream);
            if (rr == CUDA_SUCCESS) {
                for (int i = 0; i < 500; i++) {
                    if (Driver::StreamQuery_ptsz(T.wr_stream) == CUDA_SUCCESS) break;
                    usleep(10000);
                }
            }
            uint64_t ldg[2] = {0, 0};
            Driver::MemcpyDtoHAsync_v2_ptsz(ldg, T.wr_buf + 0x40, sizeof ldg, T.wr_stream);
            for (int i = 0; i < 200; i++) {
                if (Driver::StreamQuery_ptsz(T.wr_stream) == CUDA_SUCCESS) break;
                usleep(10000);
            }
            XINFO("sm120 trap: reason probe buf %#x %#x %#x %#x ldg=%#llx (r=%d)",
                  v[0], v[1], v[2], v[3], (unsigned long long)ldg[0], (int)rr);
        }).detach();
    }
    return true;
}

static bool LaunchWriter(bool armed)
{
    if (!InitWriter()) return false;
    uint64_t dst = T.cave_va, src = T.wr_buf + (armed ? 0x80 : 0);
    void *args[2] = { &dst, &src };
    CUresult r = Driver::LaunchKernel_ptsz(T.wr_fn, 1, 1, 1, 32, 1, 1, 0,
                                           T.wr_stream, args, nullptr);
    if (r != CUDA_SUCCESS) { XWARN("sm120 trap: cave writer launch -> %d", (int)r); return false; }
    // wait for the tiny kernel without synchronising the app's streams
    CUresult last = CUDA_SUCCESS;
    for (int i = 0; i < 2000; i++) {
        last = Driver::StreamQuery_ptsz(T.wr_stream);
        if (last == CUDA_SUCCESS) return true;
        usleep(10);
    }
    XWARN("sm120 trap: cave writer did not finish (query=%d)", (int)last);
    return false;
}

// Copy a 32-bit value to an arbitrary device VA with the same concurrent
// writer kernel used for the cave (src is prepared in w r_buf+0x1c0).
static bool DeviceWrite32(uint64_t dst_va, uint32_t value)
{
    if (!InitWriter()) return false;
    if (Driver::MemcpyHtoDAsync_v2_ptsz(T.wr_buf + 0x1c0, &value, sizeof value,
                                        T.wr_stream) != CUDA_SUCCESS)
        return false;
    uint64_t dst = dst_va, src = T.wr_buf + 0x1c0;
    void *args[2] = { &dst, &src };
    if (Driver::LaunchKernel_ptsz(T.wr_fn, 1, 1, 1, 1, 1, 1, 0, T.wr_stream,
                                  args, nullptr) != CUDA_SUCCESS)
        return false;
    for (int i = 0; i < 2000; i++) {
        CUresult q = Driver::StreamQuery_ptsz(T.wr_stream);
        if (q == CUDA_SUCCESS) return true;
        if (q != CUDA_ERROR_NOT_READY) { XWARN("sm120 trap: dev write query %d", (int)q); return false; }
        usleep(10);
    }
    XWARN("sm120 trap: dev write timed out");
    return false;
}

// ------------------------------------------------------------ arena + patch
static bool ReadImageSlice(CUdeviceptr pc, size_t size, std::vector<uint8_t> &out)
{
    out.resize(size);
    return cuXtraMemcpyDtoH(out.data(), pc, size, T.stream) == size;
}

// The driver traps kernels at launch once debugger parameters are set.  The
// sm120 handler has no benign path for such a trap, so try to turn the launch
// blocking off and keep only the explicit 0x83de0317 trap.
static void ClearDebuggerLaunchBlocking()
{
    if (getenv("XSCHED_SM120_TRAP_DBGFLAGS") == nullptr) return;
    void *h = nullptr;
    const char *paths[] = {"libcuda.so.1", "libcuda.so"};
    for (const char *p : paths) {
        h = dlopen(p, RTLD_NOW | RTLD_NOLOAD);
        if (h) break;
    }
    if (h == nullptr) { XWARN("sm120 trap: libcuda not found for debug flags"); return; }
    // Launch blocking makes every kernel launch wait for the debugger (the
    // replay saw cuLaunchKernel stall ~4.6 s), so switch it OFF; keep the
    // other two flags on.  Never touch cudbgAttachHandlerAvailable /
    // cudbgDebuggerCapabilities.
    int *lb = (int *)dlsym(h, "cudbgEnableLaunchBlocking");
    if (lb) { XINFO("sm120 trap: cudbgEnableLaunchBlocking = %d -> 0", *lb); *lb = 0; }
    const char *names[] = {"cudbgEnablePreemptionDebugging", "cudbgDebuggerInitialized"};
    for (const char *n : names) {
        int *v = (int *)dlsym(h, n);
        if (v) { XINFO("sm120 trap: %s = %d -> 1", n, *v); *v = 1; }
    }
}

static void InitCudbg()
{
    if (getenv("XSCHED_SM120_TRAP_CUDBG") == nullptr) return;
    void *lc = dlopen("libcuda.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (lc == nullptr) lc = dlopen("libcuda.so", RTLD_NOW | RTLD_NOLOAD);
    if (lc == nullptr) { XWARN("sm120 trap: libcuda not loaded for cudbg init"); return; }
    int (*init1)(int) = (int (*)(int))dlsym(lc, "cudbgApiInit");
    if (init1 == nullptr) { XWARN("sm120 trap: cudbgApiInit not exported"); return; }
    int mode = getenv("XSCHED_SM120_TRAP_CUDBG_MODE") ? atoi(getenv("XSCHED_SM120_TRAP_CUDBG_MODE")) : 1;
    int r = init1(mode);
    XINFO("sm120 trap: cudbgApiInit(%d) -> %d", mode, r);
    if (getenv("XSCHED_SM120_TRAP_CUDBG_ATTACH") != nullptr) {
        // cudbgApiAttach() drives the debugger-state transition cuda-gdb
        // performs on attach (contexts switched to the debug vtbl).  Without
        // it the driver suspends every new launch on a stop nobody answers.
        int (*attach)() = (int (*)())dlsym(lc, "cudbgApiAttach");
        if (attach == nullptr) { XWARN("sm120 trap: cudbgApiAttach not exported"); return; }
        int a = attach();
        XINFO("sm120 trap: cudbgApiAttach() -> %d", a);
    }
}

static bool ArmAtTrigger()
{
    return getenv("XSCHED_SM120_TRAP_ARM_PBUF_PROBE") != nullptr ||
           getenv("XSCHED_SM120_TRAP_ARM_SENTINEL") != nullptr ||
           getenv("XSCHED_SM120_TRAP_ARM_PBUF_WRITE") != nullptr;
}

static bool ProbeAndPatch()
{
    const void *tt_raw = nullptr, *mt_raw = nullptr;
    CUuuid te = UuidOf("cc529e945c4e9d46837c9625868334e4");
    CUuuid me = UuidOf("bfdb432dbf3c5a4a945eb34029e81e75");
    if (Driver::GetExportTable(&tt_raw, &te) != CUDA_SUCCESS ||
        Driver::GetExportTable(&mt_raw, &me) != CUDA_SUCCESS ||
        tt_raw == nullptr || mt_raw == nullptr) {
        XWARN("sm120 trap: export tables unavailable");
        return false;
    }
    const void **tt = (const void **)tt_raw, **mt = (const void **)mt_raw;
    struct { uint64_t flags, ssize; void *handle; uint64_t stub, hsize, rsv; } ti;
    memset(&ti, 0, sizeof ti);
    if (((CUresult (*)(CUcontext, void *))tt[20])(T.ctx, &ti) != CUDA_SUCCESS || !ti.handle) {
        XWARN("sm120 trap: slot20 getter failed");
        return false;
    }
    CUdeviceptr pc = 0; uint64_t sz = 0;
    if (((CUresult (*)(CUcontext, void *, CUdeviceptr *))mt[24])(T.ctx, ti.handle, &pc) != CUDA_SUCCESS ||
        ((CUresult (*)(void *, uint64_t *))mt[25])(ti.handle, &sz) != CUDA_SUCCESS || sz < 0x1300) {
        XWARN("sm120 trap: slot20 image unavailable (pc=%#llx sz=%#llx)",
              (unsigned long long)pc, (unsigned long long)sz);
        return false;
    }
    std::vector<uint8_t> img;
    if (!ReadImageSlice(pc, sz, img)) { XWARN("sm120 trap: image read failed"); return false; }
    uint64_t trt = (uint64_t)Imm32At(&img[0x130]) | ((uint64_t)Imm32At(&img[0x140]) << 32);
    uint64_t base = trt - 0xc38300ULL;
    uint64_t live = base + 0x15f0f00ULL;

    std::vector<uint8_t> hdr;
    if (!ReadImageSlice(live, 0xa0, hdr)) { XWARN("sm120 trap: live handler read failed"); return false; }
    static const uint32_t fp[10] = {
        0x0000795c, 0x1a007956, 0xff007387, 0xff007387, 0xff007387,
        0x00047202, 0x00057202, 0xff067803, 0x17077355, 0xff007387,
    };
    for (int i = 0; i < 10; i++) {
        if (OpcodeAt(&hdr[i * 16]) != fp[i]) {
            XWARN("sm120 trap: live handler fingerprint mismatch at +%#x", i * 16);
            return false;
        }
    }

    uint64_t cave = pc + 0xa00;
    uint64_t tramp = cave + 0x70;

    // return stub: displaced MOV R3,RZ + return to +0x3a0
    for (size_t off = 0; off < 0x80; off += 16) {
        T.ret_stub[off/8] = 0x0000000000007918ULL; T.ret_stub[off/8+1] = 0x000fc00000000000ULL;
        T.armed_stub[off/8] = 0x0000000000007918ULL; T.armed_stub[off/8+1] = 0x000fc00000000000ULL;
    }
    // Keep R2 (the reason mask) intact: zeroing it makes the handler skip
    // the real handling of a trap and re-execute the faulting instruction.
    T.ret_stub[0] = 0x000000ff00037202ULL; T.ret_stub[1] = 0x003ff00000000f00ULL; // MOV R3,RZ
    T.ret_stub[2] = 0x000000100c0c7835ULL; T.ret_stub[3] = 0x003fe600078e0000ULL; // IADD R12
    T.ret_stub[4] = 0x000000000c087950ULL; T.ret_stub[5] = 0x003fc00003a00000ULL; // RET

    // Reason-guarded armed stub (FINDINGS v4.3): only a trap whose reason has
    // bit0 (the explicit 0x83de0317 request; launch-time traps carry 0x8) is
    // redirected to the EXIT trampoline at cave+0x70.  Everything else takes
    // the exact return path of the original handler.  LOP3 encoding is copied
    // verbatim from the live handler (+0x3b0).
    if (getenv("XSCHED_SM120_TRAP_REASON_ARM") != nullptr) {
        T.armed_stub[0] = ((uint64_t)(uint32_t)tramp << 32) | (0ULL << 16) | 0x00007802ULL;
        T.armed_stub[1] = 0x003fe20000000f00ULL;                          // MOV R0, lo
        T.armed_stub[2] = ((uint64_t)(uint32_t)(tramp >> 32) << 32) | (1ULL << 16) | 0x00007802ULL;
        T.armed_stub[3] = 0x003fe20000000f00ULL;                          // MOV R1, hi
        T.armed_stub[4] = 0x0000000102ff7812ULL; T.armed_stub[5] = 0x003fda000780c0ffULL; // LOP3 P0,RZ,R2,1
        T.armed_stub[6] = 0x0000000000048947ULL; T.armed_stub[7] = 0x003ff20003800000ULL; // @!P0 BRA +0x10
        T.armed_stub[8] = 0xffffa800ff007387ULL; T.armed_stub[9] = 0x0033e40000100a00ULL; // STL.64 [0xffffa8],R0
        T.armed_stub[10] = 0x000000100c0c7835ULL; T.armed_stub[11] = 0x003fe600078e0000ULL; // IADD R12
        T.armed_stub[12] = 0x000000000c087950ULL; T.armed_stub[13] = 0x003fc00003a00000ULL; // RET
        T.armed_stub[14] = 0x000000000000794dULL; T.armed_stub[15] = 0x000fea0003800000ULL; // EXIT trampoline
        goto armed_ready;
    }
    // armed stub: read the global_exit_flag from XSched's preempt buffer
    // (debugger param 0, c[0x170]) and EXIT only when it is set.  Deactivate()
    // sets it right before the 0317 trap, so launch-time traps (flag 0) stay
    // benign while the preemption request kills the warp.  Encodings taken
    // verbatim from the validated run_l3kill.sh prototype.
    T.armed_stub[0] = 0x00005c00ff047b82ULL; T.armed_stub[1] = 0x000fc00000000800ULL; // LDC R4,c[0x170]
    T.armed_stub[2] = 0x00005d00ff057b82ULL; T.armed_stub[3] = 0x000fc00000000800ULL; // LDC R5,c[0x174]
    T.armed_stub[4] = 0x00000000040a7980ULL; T.armed_stub[5] = 0x000ea20000100900ULL; // LD.E R10,[R4]
    T.armed_stub[6] = 0x000000ff0a00720cULL; T.armed_stub[7] = 0x004fda0003f05270ULL; // ISETP.NE.AND P0,R10,RZ
    T.armed_stub[8] = 0x000000000000094dULL; T.armed_stub[9] = 0x000fea0003800000ULL; // @P0 EXIT
    T.armed_stub[10] = 0x000000ff00037202ULL; T.armed_stub[11] = 0x003ff00000000f00ULL; // MOV R3,RZ
    T.armed_stub[12] = 0x000000100c0c7835ULL; T.armed_stub[13] = 0x003fe600078e0000ULL; // IADD R12
    T.armed_stub[14] = 0x000000000c087950ULL; T.armed_stub[15] = 0x003fc00003a00000ULL; // RET
armed_ready:;

    // CALL.ABS (T-0x100)>>8 at handler+0x390
    std::vector<uint8_t> orig;
    if (!ReadImageSlice(live + 0x380, 16, orig)) return false;
    uint64_t o0, o1; memcpy(&o0, &orig[0], 8); memcpy(&o1, &orig[8], 8);
    uint64_t enc = (cave - 0x100) >> 8;
    uint64_t call[2] = {
        ((enc & 0xffffffffULL) << 32) | (o0 & 0xffffffffULL),
        (o1 & ~0xffULL) | ((enc >> 32) & 0xffULL),
    };

    {
        std::vector<uint8_t> orig390;
        if (!ReadImageSlice(live + 0x390, 16, orig390)) return false;
        memcpy(T.orig_390, orig390.data(), 16);
    }
    T.call_words[0] = call[0]; T.call_words[1] = call[1];
    T.handler_va = live; T.cave_va = cave; T.live_base = base;
    T.drv_preempt_buf = base - 0x2000000ULL;
    T.scratch_buf = base + 0x1a00000ULL;
    XINFO("sm120 trap: live_base=%#llx driver preempt_buf=%#llx scratch=%#llx (handler reads +4/+8)",
          (unsigned long long)base, (unsigned long long)T.drv_preempt_buf,
          (unsigned long long)T.scratch_buf);
    if (getenv("XSCHED_SM120_TRAP_DUMP_READ") != nullptr) {
        // Dump the live do_read_trap_reason and print every MOV-imm32, so we
        // can recover the driver-patched preemption-buffer address
        // (CUDA_PREEMPTION_BUFFER_PATCH_ADDR).
        uint64_t rd_va = base + 0x15f2100ULL;
        std::vector<uint8_t> rd(0x600);
        if (ReadImageSlice(rd_va, rd.size(), rd)) {
            uint32_t imm120 = 0, imm130 = 0;
            bool have120 = false, have130 = false;
            for (size_t o = 0; o + 16 <= rd.size(); o += 16) {
                uint64_t w0, w1; memcpy(&w0, &rd[o], 8); memcpy(&w1, &rd[o + 8], 8);
                if ((w0 & 0xffffULL) == 0x7802ULL) {
                    uint32_t imm = (uint32_t)(w0 >> 32);
                    uint32_t reg = (uint32_t)((w0 >> 16) & 0xff);
                    if (o == 0x120) { imm120 = imm; have120 = true; }
                    if (o == 0x130) { imm130 = imm; have130 = true; }
                    if (imm != 0) {
                        XINFO("sm120 trap: do_read+%#zx MOV R%u, %#x", o, reg, imm);
                    }
                }
                (void)w1;
            }
            // The driver may relocate the preemption buffer when the debugger
            // session is created.  do_read+0x120/0x130 are MOV low/high of the
            // patched pbuf address; use the post-session value if it changed.
            if (have120 && have130) {
                uint64_t np = (uint64_t)imm120 | ((uint64_t)imm130 << 32);
                if (np != 0 && np != T.drv_preempt_buf) {
                    XINFO("sm120 trap: pbuf relocated by session: %#llx -> %#llx",
                          (unsigned long long)T.drv_preempt_buf,
                          (unsigned long long)np);
                    T.drv_preempt_buf = np;
                } else if (np != 0) {
                    XINFO("sm120 trap: pbuf immediate %#llx (same as base-0x2000000)",
                          (unsigned long long)np);
                }
            }
            if (getenv("XSCHED_SM120_TRAP_DUMP_RAW") != nullptr) {
                for (size_t o = 0x100; o < 0x200; o += 16) {
                    uint64_t w0, w1; memcpy(&w0, &rd[o], 8); memcpy(&w1, &rd[o + 8], 8);
                    XINFO("sm120 trap: do_read raw +%#zx: %016llx %016llx", o,
                          (unsigned long long)w0, (unsigned long long)w1);
                }
            }
        } else {
            XWARN("sm120 trap: cannot read do_read_trap_reason at %#llx",
                  (unsigned long long)rd_va);
        }
    }
    if (getenv("XSCHED_SM120_TRAP_DUMP_ACCESS") != nullptr) {
        uint32_t dv = 0;
        CUresult rr = Driver::MemcpyDtoH_v2(&dv, T.drv_preempt_buf + 4, sizeof dv);
        uint32_t one = 2, back = 0;
        size_t hw = cuXtraMemcpyHtoD(T.drv_preempt_buf + 4, &one, sizeof one, T.stream);
        cuXtraMemcpyDtoH(&back, T.drv_preempt_buf + 4, sizeof back, T.stream);
        XINFO("sm120 trap: drv_pbuf access: dtoh r=%d val=%#x; xtra htod w=%zu back=%#x",
              (int)rr, dv, hw, back);
    }
    if (getenv("XSCHED_SM120_TRAP_NO_PATCH") != nullptr) {
        XINFO("sm120 trap: live=%#llx cave=%#llx tramp=%#llx (no patch)",
              (unsigned long long)live, (unsigned long long)cave,
              (unsigned long long)tramp);
        return true;
    }
    bool always = getenv("XSCHED_SM120_TRAP_ALWAYS_ARMED") != nullptr;
    bool ret_only = getenv("XSCHED_SM120_TRAP_RETURN_ONLY") != nullptr;
    if (always || ret_only) {
        const uint64_t *stub = ret_only ? T.ret_stub : T.armed_stub;
        size_t w0 = cuXtraMemcpyHtoD(cave, stub, sizeof T.ret_stub, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        uint64_t rb[2] = {0, 0};
        cuXtraMemcpyDtoH(rb, live + 0x390, 16, T.stream);
        bool ok = w0 == sizeof T.ret_stub && w1 == 16 && rb[0] == call[0] && rb[1] == call[1];
        XINFO("sm120 trap: live=%#llx cave=%#llx tramp=%#llx always-armed=%s",
              (unsigned long long)live, (unsigned long long)cave,
              (unsigned long long)tramp, ok ? "ok" : "FAILED");
        return ok;
    }
    if (getenv("XSCHED_SM120_TRAP_PROBE_ONLY") != nullptr) {
        XINFO("sm120 trap: live=%#llx cave=%#llx tramp=%#llx (probe only)",
              (unsigned long long)live, (unsigned long long)cave,
              (unsigned long long)tramp);
        return true;
    }
    // Default: install the call site now, before any kernel runs.  Rewriting
    // the call site while kernels run blocks in the driver (it is executing
    // code), while the cave can still be switched at runtime -- this is the
    // same split run_l3kill.sh relies on.
    InitWriter();
    if (ArmAtTrigger()) {
        // Arm() swaps this in *after* the launch (and before the 0x83de
        // session exists), so the only trap it sees is the explicit 0317.
        // The cave reads one 32-bit word of the driver's preemption buffer
        // with the handler's own LD.E.CONSTANT and records it in wr_buf, so
        // the host can read it without touching the driver VA.  The sentinel
        // variant stores R2 (reason mask) instead, proving the cave ran.
        bool sentinel = getenv("XSCHED_SM120_TRAP_ARM_SENTINEL") != nullptr;
        bool wr_probe = getenv("XSCHED_SM120_TRAP_ARM_PBUF_WRITE") != nullptr;
        uint64_t off = getenv("XSCHED_SM120_TRAP_DRVBUF_OFF")
                     ? strtoull(getenv("XSCHED_SM120_TRAP_DRVBUF_OFF"), nullptr, 0) : 4ULL;
        uint64_t tva = T.drv_preempt_buf + off;
        uint64_t buf = T.rec_dev ? T.rec_dev + 0x100 : T.wr_buf + 0x100;
        T.armed_stub[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
        T.armed_stub[1] = 0x003fe20000000f00ULL;                                  // MOV R8, buf lo
        T.armed_stub[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
        T.armed_stub[3] = 0x003fe20000000f00ULL;                                  // MOV R9, buf hi
        if (wr_probe) {
            // Can the trap write the driver's preemption buffer?  With
            // WValue=0 store R2 (the 0317 reason, 0x1); otherwise store the
            // given command word (2 = preempt_save), which the handler's next
            // dispatch-loop iteration would pick up as 0x10|0x20.
            uint32_t wval = getenv("XSCHED_SM120_TRAP_ARM_PBUF_WVAL")
                          ? (uint32_t)strtoul(getenv("XSCHED_SM120_TRAP_ARM_PBUF_WVAL"),
                                              nullptr, 0) : 0;
            T.armed_stub[4] = (((uint64_t)(uint32_t)tva) << 32) | (6ULL << 16) | 0x00007802ULL;
            T.armed_stub[5] = 0x003fe20000000f00ULL;                              // MOV R6, tva lo
            T.armed_stub[6] = (((uint64_t)(uint32_t)(tva >> 32)) << 32) | (7ULL << 16) | 0x00007802ULL;
            T.armed_stub[7] = 0x003fe20000000f00ULL;                              // MOV R7, tva hi
            if (wval == 0) {
                T.armed_stub[8] = 0x0000000006027385ULL; T.armed_stub[9] = 0x000fe20000100902ULL;  // ST.E [R6],R2
                T.armed_stub[10] = 0x0000000008027385ULL; T.armed_stub[11] = 0x000fe20000100902ULL; // ST.E [R8],R2
            } else {
                T.armed_stub[8] = (((uint64_t)wval) << 32) | (10ULL << 16) | 0x00007802ULL;
                T.armed_stub[9] = 0x003fe20000000f00ULL;                          // MOV R10, wval
                T.armed_stub[10] = 0x00000000060a7385ULL; T.armed_stub[11] = 0x000fe2000010090aULL; // ST.E [R6],R10
            }
            T.armed_stub[12] = 0x000000100c0c7835ULL; T.armed_stub[13] = 0x003fe600078e0000ULL; // IADD R12
            T.armed_stub[14] = 0x000000000c087950ULL; T.armed_stub[15] = 0x003fc00003a00000ULL; // RET
        } else if (sentinel) {
            T.armed_stub[4] = 0x0000000008027385ULL; T.armed_stub[5] = 0x000fe20000100902ULL; // ST.E [R8],R2
            T.armed_stub[6] = 0x000000100c0c7835ULL; T.armed_stub[7] = 0x003fe600078e0000ULL; // IADD R12
            T.armed_stub[8] = 0x000000000c087950ULL; T.armed_stub[9] = 0x003fc00003a00000ULL; // RET
        } else {
            T.armed_stub[4] = (((uint64_t)(uint32_t)tva) << 32) | (6ULL << 16) | 0x00007802ULL;
            T.armed_stub[5] = 0x003fe20000000f00ULL;                              // MOV R6, tva lo
            T.armed_stub[6] = (((uint64_t)(uint32_t)(tva >> 32)) << 32) | (7ULL << 16) | 0x00007802ULL;
            T.armed_stub[7] = 0x003fe20000000f00ULL;                              // MOV R7, tva hi
            T.armed_stub[8] = 0x00000000060a7980ULL; T.armed_stub[9] = 0x0032240000108900ULL;  // LD.E.CONSTANT R10,[R6]
            T.armed_stub[10] = 0x0000000008007385ULL; T.armed_stub[11] = 0x000fe2000010090aULL; // ST.E [R8],R10
            T.armed_stub[12] = 0x000000100c0c7835ULL; T.armed_stub[13] = 0x003fe600078e0000ULL; // IADD R12
            T.armed_stub[14] = 0x000000000c087950ULL; T.armed_stub[15] = 0x003fc00003a00000ULL; // RET
        }
        // LaunchWriter(true) copies wr_buf+0x80, so refresh it with the probe.
        CUresult cr = Driver::MemcpyHtoD_v2_ptds(T.wr_buf + 0x80, T.armed_stub,
                                                 sizeof T.armed_stub);
        XINFO("sm120 trap: arm %s off=%llu tva=%#llx buf=%#llx upload=%d",
              wr_probe ? "pbuf write" : (sentinel ? "sentinel" : "pbuf probe"),
              (unsigned long long)off, (unsigned long long)tva,
              (unsigned long long)buf, (int)cr);
        if (getenv("XSCHED_SM120_TRAP_DUMP_READ") == nullptr) {
            XWARN("sm120 trap: arm probe without DUMP_READ: address may be stale (%#llx)",
                  (unsigned long long)T.drv_preempt_buf);
        }
    }
    if (getenv("XSCHED_SM120_TRAP_PBUF_LOAD_PROBE") != nullptr) {
        // Replace the driver's own pbuf load (do_read+0x140) with a jump to a
        // cave that executes the same load, stores R0 (cmd) to wr_buf, and
        // jumps back to +0x150.  Unlike re-reading pbuf at +0x390, this runs
        // where the driver has the pbuf mapped.
        uint64_t rd_va = base + 0x15f2100ULL;
        uint64_t site = rd_va + 0x140ULL;
        std::vector<uint8_t> ld;
        if (!ReadImageSlice(site, 16, ld)) {
            XWARN("sm120 trap: pbuf load probe cannot read site");
            return false;
        }
        uint64_t lw0, lw1; memcpy(&lw0, &ld[0], 8); memcpy(&lw1, &ld[8], 8);
        auto setjmp_instr = [](uint8_t *in, uint64_t addr) {
            uint64_t w0 = 0x345678900000794aULL, w1 = 0x000fea0003807f12ULL;
            memcpy(in, &w0, 8); memcpy(in + 8, &w1, 8);
            in[4] = (uint8_t)(addr & 0xff); in[5] = (uint8_t)((addr >> 8) & 0xff);
            in[6] = (uint8_t)((addr >> 16) & 0xff); in[7] = (uint8_t)((addr >> 24) & 0xff);
            in[8] = (uint8_t)((addr >> 32) & 0xff); in[9] = (uint8_t)((addr >> 40) & 0xff);
        };
        uint64_t rec[16] = {};
        uint64_t buf = T.wr_buf + 0x100;
        bool benign = (strcmp(getenv("XSCHED_SM120_TRAP_PBUF_LOAD_PROBE"), "benign") == 0);
        if (benign) {
            // Control: only re-execute the displaced MOV R3 and jump back, to
            // test the JMP encoding/site without the pbuf load/store.
            rec[0] = 0x000630b000037802ULL; rec[1] = 0x003fec0000000f00ULL; // MOV R3,0x630b0
            setjmp_instr((uint8_t *)&rec[2], site + 16);                 // JMP back +0x150
            for (int i = 4; i < 16; i += 2) {
                rec[i] = 0x0000000000007918ULL; rec[i + 1] = 0x000fc00000000000ULL;
            }
        } else {
            rec[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
            rec[1] = 0x003fe20000000f00ULL;                              // MOV R8, lo
            rec[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
            rec[3] = 0x003fe20000000f00ULL;                              // MOV R9, hi
            rec[4] = lw0; rec[5] = lw1;                                  // driver's pbuf load -> R0
            rec[6] = 0x0000000008007385ULL; rec[7] = 0x000fe20000100900ULL; // ST.E [R8],R0
            setjmp_instr((uint8_t *)&rec[8], site + 16);                 // JMP back +0x150
            for (int i = 10; i < 16; i += 2) {
                rec[i] = 0x0000000000007918ULL; rec[i + 1] = 0x000fc00000000000ULL;
            }
        }
        uint8_t jmp_site[16];
        setjmp_instr(jmp_site, cave);
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(site, jmp_site, sizeof jmp_site, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: pbuf load probe site=%#llx cave=%#llx w=%zu/%zu buf=%#llx ld=%016llx/%016llx",
              (unsigned long long)site, (unsigned long long)cave, w0, w1,
              (unsigned long long)buf, (unsigned long long)lw0, (unsigned long long)lw1);
        return w0 == sizeof rec && w1 == sizeof jmp_site;
    }
    if (getenv("XSCHED_SM120_TRAP_PBUF_READ_PROBE") != nullptr) {
        // Capture the value the DRIVER already loaded from its preemption
        // buffer.  Live do_read:
        //   +0x120 MOV R4, pbuf_lo ; +0x130 MOV R5, pbuf_hi
        //   +0x140 LD.E.CONSTANT R0,[R4+4]   (cmd; 2 on 0317)
        //   +0x150 MOV R3, 0x630b0            (displaced by the call)
        // The pbuf is only mapped on the driver's path; re-reading it from a
        // +0x390 cave faults at launch (700).  So patch +0x150 and store R0.
        uint64_t rd_va = base + 0x15f2100ULL;
        uint64_t site = rd_va + 0x150ULL;
        std::vector<uint8_t> orig;
        if (!ReadImageSlice(site, 16, orig)) {
            XWARN("sm120 trap: pbuf read probe cannot read do_read site");
            return false;
        }
        uint64_t o0, o1; memcpy(&o0, &orig[0], 8); memcpy(&o1, &orig[8], 8);
        uint64_t enc = (cave - 0x100) >> 8;
        uint64_t call2[2] = {
            ((enc & 0xffffffffULL) << 32) | (o0 & 0xffffffffULL),
            (o1 & ~0xffULL) | ((enc >> 32) & 0xffULL),
        };
        uint64_t rec[16] = {};
        uint64_t buf = T.wr_buf + 0x100;
        rec[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;                                  // MOV R8, lo
        rec[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;                                  // MOV R9, hi
        rec[4] = 0x0000000008007385ULL; rec[5] = 0x000fe20000100900ULL;  // ST.E [R8],R0
        rec[6] = 0x000630b000037802ULL; rec[7] = 0x003fec0000000f00ULL;  // MOV R3,0x630b0
        rec[8] = 0x000000100c0c7835ULL; rec[9] = 0x003fe600078e0000ULL;  // IADD R12
        rec[10] = 0x000000000c087950ULL; rec[11] = 0x003fc00003a00000ULL; // RET
        rec[12] = 0x0000000000007918ULL; rec[13] = 0x000fc00000000000ULL; // NOP
        rec[14] = 0x0000000000007918ULL; rec[15] = 0x000fc00000000000ULL; // NOP
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(site, call2, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: pbuf read probe site=%#llx cave=%#llx w=%zu/%zu buf=%#llx",
              (unsigned long long)site, (unsigned long long)cave, w0, w1,
              (unsigned long long)buf);
        return w0 == sizeof rec && w1 == 16;
    }    if (getenv("XSCHED_SM120_TRAP_DRVBUF_RW") != nullptr) {
        // Write R2 (reason, =1) into the driver's preemption buffer +4 and read
        // it back with the handler's own LD.E.CONSTANT; store the readback into
        // our buffer.  If it reads 1 the trap context can write that area.
        uint64_t off = getenv("XSCHED_SM120_TRAP_DRVBUF_OFF")
                     ? strtoull(getenv("XSCHED_SM120_TRAP_DRVBUF_OFF"), nullptr, 0) : 4ULL;
        uint64_t tva = T.drv_preempt_buf + off;
        uint64_t rec[16] = {};
        uint64_t buf = T.wr_buf + 0x100;
        rec[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;                                   // MOV R8, buf lo
        rec[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;                                   // MOV R9, buf hi
        rec[4] = (((uint64_t)(uint32_t)tva) << 32) | (6ULL << 16) | 0x00007802ULL;
        rec[5] = 0x003fe20000000f00ULL;                                   // MOV R6, lo
        rec[6] = (((uint64_t)(uint32_t)(tva >> 32)) << 32) | (7ULL << 16) | 0x00007802ULL;
        rec[7] = 0x003fe20000000f00ULL;                                   // MOV R7, hi
        rec[8] = 0x0000000006027385ULL; rec[9] = 0x000fe20000100902ULL;   // ST.E [R6],R2  (write +4)
        rec[10] = 0x0000000008027385ULL; rec[11] = 0x000fe20000100902ULL; // ST.E [R8],R2  (record R2)
        rec[12] = 0x000000100c0c7835ULL; rec[13] = 0x003fe600078e0000ULL; // IADD R12
        rec[14] = 0x000000000c087950ULL; rec[15] = 0x003fc00003a00000ULL; // RET
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: drvbuf RW probe off=%llu tva=%#llx w=%zu/%zu",
              (unsigned long long)off, (unsigned long long)tva, w0, w1);
        return w0 == sizeof rec && w1 == 16;
    }
    if (getenv("XSCHED_SM120_TRAP_DRVBUF_WRITE") != nullptr) {
        // Can the TRAP context write the driver's preemption buffer?  Write a
        // magic to +4 here; the next experiment reads it back with the
        // read-probe (two-run check, since the host cannot read that VA).
        uint64_t off = getenv("XSCHED_SM120_TRAP_DRVBUF_OFF")
                     ? strtoull(getenv("XSCHED_SM120_TRAP_DRVBUF_OFF"), nullptr, 0) : 4ULL;
        uint64_t tva = T.drv_preempt_buf + off;
        if (getenv("XSCHED_SM120_TRAP_SCRATCH") != nullptr) {
            // trap -> host write test: the cave stores a magic into the
            // scratchpad; the host reads it back through the rw-s mapping.
            tva = T.scratch_buf + off;
        }
        uint32_t magic = 0x5a5a1234;
        uint64_t rec[16] = {};
        rec[0] = (((uint64_t)(uint32_t)tva) << 32) | (6ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;
        rec[2] = (((uint64_t)(uint32_t)(tva >> 32)) << 32) | (7ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;
        rec[4] = (((uint64_t)magic) << 32) | (10ULL << 16) | 0x00007802ULL;
        rec[5] = 0x003fe20000000f00ULL;
        rec[6] = 0x000000000a007385ULL; rec[7] = 0x000fe2000010090aULL;   // ST.E [R6],R10
        rec[8] = 0x000000100c0c7835ULL; rec[9] = 0x003fe600078e0000ULL;   // IADD R12
        rec[10] = 0x000000000c087950ULL; rec[11] = 0x003fc00003a00000ULL; // RET
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: drvbuf WRITE probe off=%llu tva=%#llx magic=%#x w=%zu/%zu",
              (unsigned long long)off, (unsigned long long)tva, magic, w0, w1);
        return w0 == sizeof rec && w1 == 16;
    }
    if (getenv("XSCHED_SM120_TRAP_DRVBUF_PROBE") != nullptr) {
        // Read the DRIVER's preemption buffer (base-0x2000000) with the exact
        // load flavour the handler itself uses (LD.E.CONSTANT).  This makes the
        // driver's writes (exit flag / command word / preempt_idx) observable.
        uint64_t off = getenv("XSCHED_SM120_TRAP_DRVBUF_OFF")
                     ? strtoull(getenv("XSCHED_SM120_TRAP_DRVBUF_OFF"), nullptr, 0) : 4ULL;
        uint64_t tva = T.drv_preempt_buf + off;
        if (getenv("XSCHED_SM120_TRAP_SCRATCH") != nullptr) {
            // host -> trap read test: CPU writes the sentinel through the
            // /dev/nvidiactl rw-s scratchpad mapping, the cave reads it back
            // with LD.E.CONSTANT (the driver's own load flavour).
            tva = T.scratch_buf + off;
            uint64_t sentinel = 0x1122334455667788ULL;
            if (getenv("XSCHED_SM120_TRAP_SCRATCH_NOWRITE") == nullptr) {
                memcpy((void *)tva, &sentinel, sizeof sentinel);
                __sync_synchronize();
                XINFO("sm120 trap: scratch host wrote %#llx to %#llx",
                      (unsigned long long)sentinel, (unsigned long long)tva);
            }
        }
        uint64_t rec[16] = {};
        uint64_t buf = T.wr_buf + 0x100;
        rec[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;
        rec[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;
        rec[4] = (((uint64_t)(uint32_t)tva) << 32) | (6ULL << 16) | 0x00007802ULL;
        rec[5] = 0x003fe20000000f00ULL;
        rec[6] = (((uint64_t)(uint32_t)(tva >> 32)) << 32) | (7ULL << 16) | 0x00007802ULL;
        rec[7] = 0x003fe20000000f00ULL;
        rec[8] = 0x00000000060a7980ULL; rec[9] = 0x0032240000108900ULL;   // LD.E.CONSTANT R10,[R6]
        rec[10] = 0x0000000008007385ULL; rec[11] = 0x000fe2000010090aULL; // ST.E [R8],R10
        rec[12] = 0x000000100c0c7835ULL; rec[13] = 0x003fe600078e0000ULL; // IADD R12
        rec[14] = 0x000000000c087950ULL; rec[15] = 0x003fc00003a00000ULL; // RET
        uint64_t sentinel = 0xdeadbeefcafef00dULL;
        cuXtraMemcpyHtoD(buf, &sentinel, sizeof sentinel, T.stream);
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: drvbuf probe off=%llu tva=%#llx w=%zu/%zu", (unsigned long long)off,
              (unsigned long long)tva, w0, w1);
        return w0 == sizeof rec && w1 == 16;
    }
    if (getenv("XSCHED_SM120_TRAP_CONST_PROBE") != nullptr) {
        // Same as MEM_PROBE but using LD.E.CONSTANT -- the exact load flavour
        // the driver's own do_read_trap_reason uses (bytes taken from the
        // plugin disassembly at +0x220).
        uint64_t off = getenv("XSCHED_SM120_TRAP_MEM_OFF")
                     ? strtoull(getenv("XSCHED_SM120_TRAP_MEM_OFF"), nullptr, 0) : 0x40ULL;
        uint64_t tva = T.wr_buf + off;
        uint64_t magic = 0x1122334455667788ULL;
        size_t mw = cuXtraMemcpyHtoD(tva, &magic, sizeof magic, T.stream);
        uint64_t rec[16] = {};
        uint64_t buf = T.wr_buf + 0x100;
        rec[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;
        rec[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;
        rec[4] = (((uint64_t)(uint32_t)tva) << 32) | (6ULL << 16) | 0x00007802ULL;
        rec[5] = 0x003fe20000000f00ULL;
        rec[6] = (((uint64_t)(uint32_t)(tva >> 32)) << 32) | (7ULL << 16) | 0x00007802ULL;
        rec[7] = 0x003fe20000000f00ULL;
        rec[8] = 0x00000000060a7980ULL; rec[9] = 0x0032240000108900ULL;   // LD.E.CONSTANT R10,[R6]
        rec[10] = 0x0000000008007385ULL; rec[11] = 0x000fe2000010090aULL; // ST.E [R8],R10
        rec[12] = 0x000000100c0c7835ULL; rec[13] = 0x003fe600078e0000ULL; // IADD R12
        rec[14] = 0x000000000c087950ULL; rec[15] = 0x003fc00003a00000ULL; // RET
        uint64_t sentinel = 0xdeadbeefcafef00dULL;
        cuXtraMemcpyHtoD(buf, &sentinel, sizeof sentinel, T.stream);
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: const probe tva=%#llx magic=%#llx w=%zu/%zu (mw=%zu)", (unsigned long long)tva,
              (unsigned long long)magic, w0, w1, mw);
        return w0 == sizeof rec && w1 == 16;
    }
    if (getenv("XSCHED_SM120_TRAP_SRC") != nullptr) {
        // Try every remaining memory type as a host->handler channel:
        //   instr   = cuXtraInstrMemBlockAlloc (instruction memory block)
        //   managed = cuMemAllocManaged
        //   scratch = driver scratchpad (read-only)
        //   pause   = driver pause_reason_table (read-only)
        const char *src = getenv("XSCHED_SM120_TRAP_SRC");
        static CUdeviceptr stva = 0;
        static uint64_t magic = 0x5a5a5a5a12345678ULL;
        static bool ready = false;
        static int alloc_r = -1;
        if (!ready) {
            if (!strcmp(src, "instr")) {
                stva = cuXtraInstrMemBlockAlloc(T.ctx, 0x1000);
                alloc_r = (int)cuXtraInstrMemcpyHtoD(stva, &magic, 8, T.stream);
            } else if (!strcmp(src, "managed")) {
                CUresult r = Driver::MemAllocManaged(&stva, 4096, 1 /* ATTACH_GLOBAL */);
                alloc_r = (int)r;
                if (r == CUDA_SUCCESS) Driver::MemcpyHtoD_v2_ptds(stva, &magic, 8);
            } else if (!strcmp(src, "scratch")) {
                stva = base + 0x1a00000ULL; alloc_r = 0;   // read-only
            } else if (!strcmp(src, "pause")) {
                stva = base + 0x1000000ULL; alloc_r = 0;   // read-only
            } else {
                XWARN("sm120 trap: unknown SRC %s", src);
                return false;
            }
            ready = true;
            uint64_t hv = 0;
            cuXtraMemcpyDtoH(&hv, stva, 8, T.stream);
            XINFO("sm120 trap: src=%s va=%#llx r=%d magic=%#llx hostread=%#llx", src,
                  (unsigned long long)stva, alloc_r, (unsigned long long)magic,
                  (unsigned long long)hv);
        }
        uint64_t rec[16] = {};
        uint64_t buf = T.wr_buf + 0x100;
        rec[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;
        rec[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;
        rec[4] = (((uint64_t)(uint32_t)stva) << 32) | (6ULL << 16) | 0x00007802ULL;
        rec[5] = 0x003fe20000000f00ULL;
        rec[6] = (((uint64_t)(uint32_t)(stva >> 32)) << 32) | (7ULL << 16) | 0x00007802ULL;
        rec[7] = 0x003fe20000000f00ULL;
        rec[8] = 0x00000000060a7980ULL; rec[9] = 0x000ea20000100900ULL;   // LD.E R10,[R6]
        rec[10] = 0x0000000008007385ULL; rec[11] = 0x000fe2000010090aULL; // ST.E [R8],R10
        rec[12] = 0x000000100c0c7835ULL; rec[13] = 0x003fe600078e0000ULL; // IADD R12
        rec[14] = 0x000000000c087950ULL; rec[15] = 0x003fc00003a00000ULL; // RET
        uint64_t sentinel = 0xdeadbeefcafef00dULL;
        cuXtraMemcpyHtoD(buf, &sentinel, sizeof sentinel, T.stream);
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: src probe installed src=%s va=%#llx w=%zu/%zu", src,
              (unsigned long long)stva, w0, w1);
        return w0 == sizeof rec && w1 == 16;
    }
    if (getenv("XSCHED_SM120_TRAP_UVA_PROBE") != nullptr) {
        // Read host-mapped (UVA) memory from the trap context: different
        // aperture than device memory, and a candidate host->handler channel.
        static void *hp = nullptr;
        static CUdeviceptr dp = 0;
        if (hp == nullptr) {
            CUresult r1 = Driver::MemHostAlloc(&hp, 4096,
                CU_MEMHOSTALLOC_DEVICEMAP | CU_MEMHOSTALLOC_PORTABLE);
            CUresult r2 = Driver::MemHostGetDevicePointer_v2(&dp, hp, 0);
            XINFO("sm120 trap: uva alloc host=%p dev=%#llx r=%d/%d", hp,
                  (unsigned long long)dp, (int)r1, (int)r2);
            memset(hp, 0, 4096);
        }
        uint64_t tva = (uint64_t)dp + 0x40;
        uint64_t magic = 0xa5a5c0de12345678ULL;
        memcpy((char *)hp + 0x40, &magic, sizeof magic);
        uint64_t rec[16] = {};
        uint64_t buf = T.wr_buf + 0x100;
        rec[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;
        rec[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;
        rec[4] = (((uint64_t)(uint32_t)tva) << 32) | (6ULL << 16) | 0x00007802ULL;
        rec[5] = 0x003fe20000000f00ULL;
        rec[6] = (((uint64_t)(uint32_t)(tva >> 32)) << 32) | (7ULL << 16) | 0x00007802ULL;
        rec[7] = 0x003fe20000000f00ULL;
        rec[8] = 0x00000000060a7980ULL; rec[9] = 0x000ea20000100900ULL;   // LD.E R10,[R6]
        rec[10] = 0x0000000008007385ULL; rec[11] = 0x000fe2000010090aULL; // ST.E [R8],R10
        rec[12] = 0x000000100c0c7835ULL; rec[13] = 0x003fe600078e0000ULL; // IADD R12
        rec[14] = 0x000000000c087950ULL; rec[15] = 0x003fc00003a00000ULL; // RET
        uint64_t sentinel = 0xdeadbeefcafef00dULL;
        cuXtraMemcpyHtoD(buf, &sentinel, sizeof sentinel, T.stream);
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: uva probe tva=%#llx magic=%#llx w=%zu/%zu", (unsigned long long)tva,
              (unsigned long long)magic, w0, w1);
        return w0 == sizeof rec && w1 == 16;
    }
    if (getenv("XSCHED_SM120_TRAP_LDG_PROBE") != nullptr) {
        // Same as MEM_PROBE, but with the descriptor-based global load the
        // compiler emits on sm_120:  LDCU.64 UR4,c[0x0][0x358] + LDG.E.64.
        uint64_t off = getenv("XSCHED_SM120_TRAP_MEM_OFF")
                     ? strtoull(getenv("XSCHED_SM120_TRAP_MEM_OFF"), nullptr, 0) : 0x40ULL;
        uint64_t tva = T.wr_buf + off;
        uint64_t magic = 0x1122334455667788ULL;
        size_t mw = cuXtraMemcpyHtoD(tva, &magic, sizeof magic, T.stream);
        uint64_t rec[16] = {};
        rec[0] = 0x00006b00ff0477acULL; rec[1] = 0x000e2e0008000a00ULL; // LDCU.64 UR4,c[0x358]
        rec[2] = (((uint64_t)(uint32_t)tva) << 32) | (6ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;                                   // MOV R6, lo
        rec[4] = (((uint64_t)(uint32_t)(tva >> 32)) << 32) | (7ULL << 16) | 0x00007802ULL;
        rec[5] = 0x003fe20000000f00ULL;                                   // MOV R7, hi
        rec[6] = 0x00000004060a7981ULL; rec[7] = 0x001eac000c1e1b00ULL;   // LDG.E.64 R10,desc[UR4][R6]
        rec[8] = 0x000000010a0a7835ULL; rec[9] = 0x003fe600078e0000ULL;   // IADD R10,R10,1
        rec[10] = 0x0000000a06007986ULL; rec[11] = 0x004fe2000c101b04ULL; // STG.E.64 desc[UR4][R6],R10
        rec[12] = 0x000000100c0c7835ULL; rec[13] = 0x003fe600078e0000ULL; // IADD R12
        rec[14] = 0x000000000c087950ULL; rec[15] = 0x003fc00003a00000ULL; // RET
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: ldg probe tva=%#llx magic=%#llx w=%zu/%zu", (unsigned long long)tva,
              (unsigned long long)magic, mw, w1);
        return w0 == sizeof rec && w1 == 16;
    }
    if (getenv("XSCHED_SM120_TRAP_MEM_PROBE") != nullptr) {
        // Read our OWN device allocation (host-initialized magic) from the trap
        // context: tests whether the anomaly is "global reads are dead" or
        // "only foreign (preempt buffer) allocations are invisible".
        uint64_t off = getenv("XSCHED_SM120_TRAP_MEM_OFF")
                     ? strtoull(getenv("XSCHED_SM120_TRAP_MEM_OFF"), nullptr, 0) : 0x40ULL;
        bool code = getenv("XSCHED_SM120_TRAP_MEM_CODE") != nullptr;
        // code variant: read the handler's own code page (known content).
        uint64_t tva = code ? (live + 0x390) : (T.wr_buf + off);
        uint64_t magic = 0x1122334455667788ULL;
        size_t mw = 0;
        if (!code) mw = cuXtraMemcpyHtoD(tva, &magic, sizeof magic, T.stream);
        uint64_t hv = 0;
        cuXtraMemcpyDtoH(&hv, tva, sizeof hv, T.stream);
        uint64_t rec[16] = {};
        uint64_t buf = T.wr_buf + 0x100;
        rec[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;
        rec[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;
        rec[4] = (((uint64_t)(uint32_t)tva) << 32) | (6ULL << 16) | 0x00007802ULL;
        rec[5] = 0x003fe20000000f00ULL;
        rec[6] = (((uint64_t)(uint32_t)(tva >> 32)) << 32) | (7ULL << 16) | 0x00007802ULL;
        rec[7] = 0x003fe20000000f00ULL;
        rec[8] = 0x00000000060a7980ULL; rec[9] = 0x000ea20000100900ULL;   // LD.E R10,[R6]
        rec[10] = 0x0000000008007385ULL; rec[11] = 0x000fe2000010090aULL; // ST.E [R8],R10
        rec[12] = 0x000000100c0c7835ULL; rec[13] = 0x003fe600078e0000ULL; // IADD R12
        rec[14] = 0x000000000c087950ULL; rec[15] = 0x003fc00003a00000ULL; // RET
        uint64_t sentinel = 0xdeadbeefcafef00dULL;   // proves the cave ran
        cuXtraMemcpyHtoD(buf, &sentinel, sizeof sentinel, T.stream);
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: mem probe off=%#llx tva=%#llx code=%d magic=%#llx w=%zu/%zu hostread=%#llx call=%#llx",
              (unsigned long long)off, (unsigned long long)tva, (int)code,
              (unsigned long long)magic, mw, w1, (unsigned long long)hv,
              (unsigned long long)call[0]);
        return w0 == sizeof rec && w1 == 16;
    }
    if (getenv("XSCHED_SM120_TRAP_TABLE_PROBE") != nullptr) {
        // Read a driver-managed trap area (trap_reason_table / scratchpad /
        // pause_reason_table) from inside the trap context.  Those areas are
        // mapped for the driver's own handler, so they are the best candidate
        // for a host->handler channel.  The magic is written at Init; the
        // cave stores whatever it reads back into wr_buf+0x100.
        uint64_t off = getenv("XSCHED_SM120_TRAP_TABLE_OFF")
                     ? strtoull(getenv("XSCHED_SM120_TRAP_TABLE_OFF"), nullptr, 0)
                     : 0xc38300ULL;
        uint64_t tva = base + off;
        // Read-only: capture what the host sees at that address, without
        // writing the driver's area (writing it faulted the context).
        uint64_t magic = 0x5aa5c0de12345678ULL;
        size_t mw = cuXtraMemcpyHtoD(tva, &magic, sizeof magic, T.stream);
        uint64_t hv = 0;
        cuXtraMemcpyDtoH(&hv, tva, sizeof hv, T.stream);
        XINFO("sm120 trap: table magic write w=%zu hostread=%#llx", mw, (unsigned long long)hv);
        uint64_t rec[16] = {};
        uint64_t buf = T.wr_buf + 0x100;
        rec[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;
        rec[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;
        // R6/R7 hold the address, R10 the loaded value: R0-R5 and R12 must be
        // preserved -- the handler writes `STL.64 [R1], R2` right after the
        // cave returns.
        rec[4] = (((uint64_t)(uint32_t)tva) << 32) | (6ULL << 16) | 0x00007802ULL;
        rec[5] = 0x003fe20000000f00ULL;
        rec[6] = (((uint64_t)(uint32_t)(tva >> 32)) << 32) | (7ULL << 16) | 0x00007802ULL;
        rec[7] = 0x003fe20000000f00ULL;
        rec[8] = 0x00000000060a7980ULL; rec[9] = 0x000ea20000100900ULL;   // LD.E R10,[R6]
        rec[10] = 0x0000000008007385ULL; rec[11] = 0x000fe2000010090aULL; // ST.E [R8],R10
        rec[12] = 0x000000100c0c7835ULL; rec[13] = 0x003fe600078e0000ULL; // IADD R12
        rec[14] = 0x000000000c087950ULL; rec[15] = 0x003fc00003a00000ULL; // RET
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: table probe installed off=%#llx tva=%#llx magic=%#llx w=%zu/%zu",
              (unsigned long long)off, (unsigned long long)tva,
              (unsigned long long)magic, w0, w1);
        return w0 == sizeof rec && w1 == 16;
    }
    if (getenv("XSCHED_SM120_TRAP_FLAG_PROBE") != nullptr) {
        // Same as the param probe, but ALSO performs the real LD.E from the
        // preempt buffer pointer and records the value: proves whether the
        // trap context can read the scheduler's flag (XSched Lv3 depends on it).
        uint64_t rec[16] = {};
        uint64_t buf = T.wr_buf + 0x100;
        rec[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;
        rec[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;
        // 64-bit pointer = c[0x170]:c[0x174] -> R4:R5 (the earlier probe loaded
        // only the low half and used R0:R1, so its result was meaningless).
        rec[4] = 0x00005c00ff047b82ULL; rec[5] = 0x000fc00000000800ULL;   // LDC R4,c[0x170]
        rec[6] = 0x00005d00ff057b82ULL; rec[7] = 0x000fc00000000800ULL;   // LDC R5,c[0x174]
        rec[8] = 0x00000000040a7980ULL; rec[9] = 0x000ea20000100900ULL;   // LD.E R10,[R4]
        rec[10] = 0x0000000008007385ULL; rec[11] = 0x000fe2000010090aULL; // ST.E [R8],R10
        rec[12] = 0x000000100c0c7835ULL; rec[13] = 0x003fe600078e0000ULL; // IADD R12
        rec[14] = 0x000000000c087950ULL; rec[15] = 0x003fc00003a00000ULL; // RET
        rec[12] = 0x000000100c0c7835ULL; rec[13] = 0x003fe600078e0000ULL; // IADD R12
        rec[14] = 0x000000000c087950ULL; rec[15] = 0x003fc00003a00000ULL; // RET
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: flag probe installed w=%zu/%zu buf=%#llx", w0, w1,
              (unsigned long long)buf);
        return w0 == sizeof rec && w1 == 16;
    }
    if (const char *p = getenv("XSCHED_SM120_TRAP_DBGPARAM_PROBE")) {
        (void)p;
        // Read one 32-bit debugger-param constant (c[0x0][off]) and store it to
        // wr_buf+0x100 for the reason_probe reader.  Use DBGPARAM_OFF to pick
        // 0x170 (preempt buffer ptr), 0x178 (guardian), 0x180 (kernel_idx?),
        // 0x188 (killable?).  The displaced MOV R3,RZ is restored before RET.
        uint64_t off = 0x180;
        if (const char *po = getenv("XSCHED_SM120_TRAP_DBGPARAM_OFF")) {
            off = strtoull(po, nullptr, 0);
        }
        uint64_t imm = (off & 0xffffffffULL) << 6;   // c[off] encoding: off*64
        uint64_t rec[16] = {};
        uint64_t buf = T.wr_buf + 0x100;
        rec[0] = (((uint64_t)(uint32_t)buf) << 32) | (8ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;                                  // MOV R8, lo
        rec[2] = (((uint64_t)(uint32_t)(buf >> 32)) << 32) | (9ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;                                  // MOV R9, hi
        rec[4] = (imm << 32) | (0xffULL << 24) | (10ULL << 16) | 0x00007b82ULL;
        rec[5] = 0x000fc00000000800ULL;                                  // LDC R10,c[off]
        rec[6] = 0x0000000008007385ULL; rec[7] = 0x000fe2000010090aULL;  // ST.E [R8],R10
        rec[8] = 0x000000ff00037202ULL; rec[9] = 0x003ff00000000f00ULL;  // MOV R3,RZ
        rec[10] = 0x000000100c0c7835ULL; rec[11] = 0x003fe600078e0000ULL; // IADD R12
        rec[12] = 0x000000000c087950ULL; rec[13] = 0x003fc00003a00000ULL; // RET
        rec[14] = 0x0000000000007918ULL; rec[15] = 0x000fc00000000000ULL; // NOP
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: dbgparam probe off=%#llx (c[0x%llx]) installed w=%zu/%zu buf=%#llx",
              (unsigned long long)off, (unsigned long long)off, w0, w1,
              (unsigned long long)buf);
        return w0 == sizeof rec && w1 == 16;
    }
    if (getenv("XSCHED_SM120_TRAP_PARAM_PROBE") != nullptr) {
        uint64_t rec[16] = {};
        uint64_t lo = (uint64_t)(uint32_t)(T.wr_buf + 0x100);
        uint64_t hi = (uint64_t)(uint32_t)((T.wr_buf + 0x100) >> 32);
        rec[0] = (lo << 32) | (8ULL << 16) | 0x00007802ULL; rec[1] = 0x003fe20000000f00ULL;
        rec[2] = (hi << 32) | (9ULL << 16) | 0x00007802ULL; rec[3] = 0x003fe20000000f00ULL;
        rec[4] = 0x00005c00ff007b82ULL; rec[5] = 0x000fc00000000800ULL;   // LDC R0,c[0x170]
        rec[6] = 0x0000000008007385ULL; rec[7] = 0x000fe20000100902ULL;   // ST.E [R8],R2
        rec[8] = 0x0000000408007385ULL; rec[9] = 0x000fe20000100900ULL;   // ST.E [R8+4],R0
        rec[10] = 0x000000ff00037202ULL; rec[11] = 0x003ff00000000f00ULL; // MOV R3,RZ
        rec[12] = 0x000000100c0c7835ULL; rec[13] = 0x003fe600078e0000ULL; // IADD R12
        rec[14] = 0x000000000c087950ULL; rec[15] = 0x003fc00003a00000ULL; // RET
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: param probe installed w=%zu/%zu buf=%#llx", w0, w1,
              (unsigned long long)(T.wr_buf + 0x100));
        return w0 == sizeof rec && w1 == 16;
    }
    if (getenv("XSCHED_SM120_TRAP_REASON_PROBE") != nullptr) {
        // Cave that records the reason mask (R2) the handler computed, so we
        // can tell a launch-time trap from the explicit 0317 trap.
        uint64_t rec[16] = {};
        rec[0] = ((uint64_t)(uint32_t)(T.wr_buf + 0x100) << 32) | (8ULL << 16) | 0x00007802ULL;
        rec[1] = 0x003fe20000000f00ULL;                                  // MOV R8, lo
        rec[2] = ((uint64_t)(uint32_t)((T.wr_buf + 0x100) >> 32) << 32) | (9ULL << 16) | 0x00007802ULL;
        rec[3] = 0x003fe20000000f00ULL;                                  // MOV R9, hi
        rec[4] = 0x0000000008027385ULL;                                  // ST.E [R8], R2
        rec[5] = 0x000fe20000100900ULL;
        rec[6] = 0x000000ff00037202ULL; rec[7] = 0x003ff00000000f00ULL;  // MOV R3,RZ
        rec[8] = 0x000000100c0c7835ULL; rec[9] = 0x003fe600078e0000ULL;  // IADD R12
        rec[10] = 0x000000000c087950ULL; rec[11] = 0x003fc00003a00000ULL; // RET
        size_t w0 = cuXtraMemcpyHtoD(cave, rec, sizeof rec, T.stream);
        size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
        cuXtraInvalInstrCache(T.ctx);
        XINFO("sm120 trap: reason probe installed w=%zu/%zu buf=%#llx",
              w0, w1, (unsigned long long)(T.wr_buf + 0x100));
        return w0 == sizeof rec && w1 == 16;
    }
    // The cave is armed *before* the first launch: at level 3 the guardian
    // spins waiting for the trap, so the GPU is fully occupied and neither a
    // host copy nor a device writer can run once the kernels are up.
    bool pre_arm = getenv("XSCHED_SM120_TRAP_PRE_RETURN") == nullptr && !ArmAtTrigger();
    const uint64_t *stub = pre_arm ? T.armed_stub : T.ret_stub;
    size_t w0 = cuXtraMemcpyHtoD(cave, stub, sizeof T.ret_stub, T.stream);
    size_t w1 = cuXtraMemcpyHtoD(live + 0x390, call, 16, T.stream);
    cuXtraInvalInstrCache(T.ctx);
    uint64_t rb[2] = {0, 0};
    cuXtraMemcpyDtoH(rb, live + 0x390, 16, T.stream);
    bool ok = w0 == sizeof T.ret_stub && w1 == 16 && rb[0] == call[0] && rb[1] == call[1];
    XINFO("sm120 trap: live=%#llx cave=%#llx tramp=%#llx patched=%s (%s)",
          (unsigned long long)live, (unsigned long long)cave,
          (unsigned long long)tramp, ok ? "ok" : "FAILED", pre_arm ? "armed" : "return");
    return ok;
}

// ------------------------------------------------------------ public API
TrapSm120Context &TrapSm120Context::Instance()
{
    static TrapSm120Context inst;
    return inst;
}

bool TrapSm120Context::Init(CUcontext ctx, CUstream stream)
{
    if (T.inited) return T.patched;
    T.inited = true; T.ctx = ctx; T.stream = stream;
    // The agent mirrors cuda-gdb: it attaches with ptrace before building the
    // 0x83de session.  Without the trace relationship the driver faults the
    // instrumented context on the next launch.
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    ClearDebuggerLaunchBlocking();
    InitCudbg();

    const char *name = getenv("XSCHED_SM120_TRAP_SHM");
    const char *agent_cfg = getenv("XSCHED_SM120_TRAP_AGENT");
    if ((name == nullptr || name[0] == '\0') && (agent_cfg == nullptr || agent_cfg[0] == '\0')) {
        XWARN("sm120 trap backend is experimental: set XSCHED_SM120_TRAP_SHM and/or "
              "XSCHED_SM120_TRAP_AGENT to enable it");
        return false;
    }
    snprintf(T.shm_name, sizeof T.shm_name, "%s", name ? name : L3_TRAP_SHM_NAME);
    int fd = shm_open(T.shm_name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { XWARN("sm120 trap: shm_open failed: %s", strerror(errno)); return false; }
    if (ftruncate(fd, sizeof(L3TrapShm)) != 0) { close(fd); return false; }
    T.shm = (L3TrapShm *)mmap(nullptr, sizeof(L3TrapShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (T.shm == MAP_FAILED) { T.shm = nullptr; return false; }
    memset(T.shm, 0, sizeof *T.shm);
    T.shm->pid = (uint32_t)getpid();

    // spawn the agent when a path is provided
    const char *agent = getenv("XSCHED_SM120_TRAP_AGENT");
    if (agent && *agent) {
        pid_t pid = fork();
        if (pid == 0) {
            execl(agent, agent, T.shm_name, (char *)nullptr);
            _exit(127);
        }
        T.agent_pid = (int)pid;
    }

    for (int i = 0; i < 300 && T.shm->agent_ready == 0; i++) usleep(10000);
    if (T.shm->agent_ready == 0) {
        XWARN("sm120 trap: no agent on shm %s (set XSCHED_SM120_TRAP_AGENT)", T.shm_name);
        return false;
    }
    // No 0x83de session is created here on purpose: while one exists every
    // new launch stops on a debugger event nothing answers.  The agent builds
    // it on demand, inside Trigger(), after the kernels are already running.
    XINFO("sm120 trap: agent client %#x ready (lazy sessions)", T.shm->agent_client);

    // handles captured by the ioctl interposer while the app initialised CUDA
    for (int i = 0; i < 300 && (!H.client || !H.device || H.ncompute == 0); i++) usleep(10000);
    if (!H.client || !H.device || !real_ioctl) {
        XWARN("sm120 trap: RM handles not captured (client=%#x device=%#x ioctl=%p)",
              H.client, H.device, (void *)real_ioctl);
        return false;
    }

    // 0xd04 inherited share policy: let the agent's client see our objects
    struct { uint32_t target, rights, type; } p;
    p = { H.client, 1, 0x0002 };
    T.shm->policy_status[0] = rm_control(H.fd, H.client, H.client, 0xd04, &p, sizeof p);
    p = { H.client, 4, 0x40002 };
    T.shm->policy_status[1] = rm_control(H.fd, H.client, H.client, 0xd04, &p, sizeof p);
    p = { T.shm->agent_client, 1, 0x40003 };
    T.shm->policy_status[2] = rm_control(H.fd, H.client, H.client, 0xd04, &p, sizeof p);

    T.shm->app_client = H.client;
    T.shm->device = H.device;
    T.shm->subdev = H.subdev;
    T.shm->app_session = H.session;
    T.shm->ncompute = H.ncompute < L3_TRAP_MAX_KGO ? H.ncompute : L3_TRAP_MAX_KGO;
    for (uint32_t i = 0; i < T.shm->ncompute; i++) T.shm->computes[i] = H.computes[i];
    __sync_synchronize();
    T.shm->magic = L3_TRAP_SHM_MAGIC;
    XINFO("sm120 trap: handles client=%#x device=%#x KGO0=%#x n=%u app_session=%#x agent=%#x policy=%#x/%#x/%#x",
          H.client, H.device, H.computes[0], H.ncompute, H.session, T.shm->agent_client,
          (unsigned)T.shm->policy_status[0], (unsigned)T.shm->policy_status[1],
          (unsigned)T.shm->policy_status[2]);

    if (getenv("L3AGENT_EAGER") != nullptr) {
        for (int i = 0; i < 300 && T.shm->session_ready == 0; i++) usleep(10000);
        XINFO("sm120 trap: eager agent session %#x status=%#x", T.shm->session,
              (unsigned)T.shm->session_status);
        if (T.shm->session_status != 0) return false;
    }
    if (getenv("XSCHED_SM120_TRAP_CAVE") != nullptr) {
        if (getenv("XSCHED_SM120_TRAP_SELF_RESUME_MS") != nullptr && H.session != 0) {
        int ms = atoi(getenv("XSCHED_SM120_TRAP_SELF_RESUME_MS"));
        uint32_t fd = (uint32_t)H.fd, cl = H.client, se = H.session;
        XINFO("sm120 trap: self-resume thread armed (%d ms, session=%#x)", ms, se);
        std::thread([fd, cl, se, ms]() {
            usleep(ms * 1000);
            int st = rm_control((int)fd, cl, se, 0x83de0318, nullptr, 0);
            XINFO("sm120 trap: self 0318 (app session) -> %#x", (unsigned)st);
        }).detach();
    }
    T.patched = ProbeAndPatch();
    } else {
        // Default: TarpHandlerSM120::Instrument() patches the handler return
        // stub (+0x880) with XSched's own trap payload; nothing to install
        // here and no runtime code writes are needed.
        XINFO("sm120 trap: handler payload installed at +0x880 (upstream payload)");
        T.patched = true;
    }
    T.shm->trap_patch_ok = T.patched ? 1 : 0;
    return T.patched;
}

bool TrapSm120Context::IsReady() const { return T.patched; }

void TrapSm120Context::Arm()
{
    if (!T.patched) return;
    if (getenv("XSCHED_SM120_TRAP_CAVE") == nullptr) return;
    // Keep the benign return stub installed at Init; used to test what an
    // unmodified 0x83de0317 does when the handler dispatch is patched.
    if (getenv("XSCHED_SM120_TRAP_KEEP_RET") != nullptr) return;
    if (getenv("XSCHED_SM120_TRAP_NO_PATCH") != nullptr) return;
    if (getenv("XSCHED_SM120_TRAP_PRE_RETURN") == nullptr && !ArmAtTrigger()) {
        // pre-armed in ProbeAndPatch(); nothing can run on the GPU now
        T.shm->arm_count = (uint32_t)(T.shm->arm_count + 1);
        return;
    }
    XINFO("sm120 trap: arm write cave start");
    if (getenv("XSCHED_SM120_TRAP_INSTR_WRITE") != nullptr) {
        size_t a = cuXtraInstrMemcpyHtoD(T.cave_va, T.armed_stub, sizeof T.armed_stub, T.stream);
        XINFO("sm120 trap: arm instr-write done %zu", a);
        cuXtraInvalInstrCache(T.ctx);
    } else if (getenv("XSCHED_SM120_TRAP_HOST_ARM") != nullptr) {
        size_t a = cuXtraMemcpyHtoD(T.cave_va, T.armed_stub, sizeof T.armed_stub, T.stream);
        XINFO("sm120 trap: arm write cave done %zu", a);
        cuXtraInvalInstrCache(T.ctx);
    } else if (!LaunchWriter(true)) {
        XWARN("sm120 trap: arm via device writer failed");
        return;
    }
    XINFO("sm120 trap: arm done");
    T.shm->arm_count = (uint32_t)(T.shm->arm_count + 1);
}

void TrapSm120Context::Disarm()
{
    if (!T.patched) return;
    if (getenv("XSCHED_SM120_TRAP_CAVE") == nullptr) return;
    if (getenv("XSCHED_SM120_TRAP_KEEP_RET") != nullptr) return;
    if (getenv("XSCHED_SM120_TRAP_PRE_RETURN") == nullptr) return;
    if (getenv("XSCHED_SM120_TRAP_HOST_ARM") != nullptr) {
        cuXtraMemcpyHtoD(T.cave_va, T.ret_stub, sizeof T.ret_stub, T.stream);
        cuXtraInvalInstrCache(T.ctx);
    } else {
        LaunchWriter(false);
    }
}

// ------------------------------------------------------------ host poke
// The driver reserves the trap arena and the preemption buffer in anonymous
// PROT_NONE VMAs of this process (see findings v4.9).  If mprotect(WRITE)
// succeeds there, the CPU can poke the command word directly.
static __thread sigjmp_buf g_poke_jmp;
static __thread volatile sig_atomic_t g_poke_armed = 0;
static volatile sig_atomic_t g_poke_sig = 0;

static void PokeFaultHandler(int sig)
{
    if (g_poke_armed) {
        g_poke_sig = sig;
        siglongjmp(g_poke_jmp, 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static std::string VmaLineOf(uint64_t va)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (f == nullptr) return "?";
    char line[512];
    std::string hit = "?";
    while (fgets(line, sizeof line, f) != nullptr) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) != 2) continue;
        if (va >= lo && va < hi) {
            hit = line;
            while (!hit.empty() && (hit.back() == '\n' || hit.back() == ' ')) hit.pop_back();
            break;
        }
    }
    fclose(f);
    return hit;
}

static void HostPoke(uint64_t va, uint32_t val)
{
    long ps = sysconf(_SC_PAGESIZE);
    uint64_t page = va & ~(uint64_t)(ps - 1);
    XINFO("hostpoke: va=%#llx vma=[%s]", (unsigned long long)va, VmaLineOf(va).c_str());

    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = PokeFaultHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);

    g_poke_armed = 1;
    if (sigsetjmp(g_poke_jmp, 1) == 0) {
        int r = mprotect((void *)page, (size_t)ps * 2, PROT_READ | PROT_WRITE);
        int e = errno;
        XINFO("hostpoke: mprotect(%#llx, %#lx) r=%d errno=%d",
              (unsigned long long)page, (unsigned long)ps * 2, r, r ? e : 0);
        if (r == 0) {
            volatile uint32_t *w = (volatile uint32_t *)va;
            uint32_t f0 = w[-1], f4 = w[0], f8 = w[1];
            XINFO("hostpoke: read +0=%#x +4=%#x +8=%#x", f0, f4, f8);
            if (val != 0) {
                w[0] = val;
                XINFO("hostpoke: wrote +4=%#x back=%#x", val, w[0]);
            }
            mprotect((void *)page, (size_t)ps * 2, PROT_NONE);
            XINFO("hostpoke: vma restored to PROT_NONE");
        }
        g_poke_armed = 0;
    } else {
        g_poke_armed = 0;
        XINFO("hostpoke: faulted sig=%d", (int)g_poke_sig);
    }
    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGBUS, &old_bus, nullptr);
    XINFO("hostpoke: done");
}

bool TrapSm120Context::Trigger()
{
    if (!T.patched || T.shm == nullptr) return false;
    if (getenv("XSCHED_SM120_TRAP_NO_TRIGGER")) return true;
    bool no_patch = getenv("XSCHED_SM120_TRAP_NO_PATCH") != nullptr;
    bool always = no_patch || getenv("XSCHED_SM120_TRAP_ALWAYS_ARMED") != nullptr;
    bool ret_only = getenv("XSCHED_SM120_TRAP_RETURN_ONLY") != nullptr;
    if (ret_only) always = true;   // pre-patched return stub, never rewrite
    // 1. arm while no session exists: instruction writes are not serialised
    if (!always) Arm();

    // 1b. write the driver preemption command word with a device-side kernel
    //     (host memcpy to that VA is rejected/crashes while kernels run).
    if (getenv("XSCHED_SM120_TRAP_DEV_CMD") != nullptr && T.drv_preempt_buf != 0) {
        uint32_t cmd = (uint32_t)strtoul(getenv("XSCHED_SM120_TRAP_DEV_CMD"), nullptr, 0);
        bool ok = DeviceWrite32(T.drv_preempt_buf + 4, cmd);
        XINFO("sm120 trap: dev cmd %u -> drv_pbuf+4 %s", cmd, ok ? "ok" : "FAILED");
    }

    // 2. have the agent build the 0x83de session now that kernels are running
    bool lazy = getenv("L3AGENT_EAGER") == nullptr;
    if (lazy && T.shm->session != 0) lazy = false;   // reuse the live session
    if (lazy) {
        uint32_t sg = T.shm->session_req + 1;
        T.shm->session_req = sg;
        for (int i = 0; i < 400 && T.shm->session_ready != sg; i++) usleep(5000);
        if (T.shm->session_ready != sg || T.shm->session_status != 0) {
            XWARN("sm120 trap: lazy session %u ready=%u status=%#x", sg,
                  (unsigned)T.shm->session_ready, (unsigned)T.shm->session_status);
            if (!always) Disarm();
            return false;
        }
    }

    if (getenv("XSCHED_SM120_TRAP_XTRA") != nullptr) {
        // cuXtraTriggerTrap returns immediately; with the payload installed
        // at handler+0x880 the trap now has something to run.
        cuXtraTriggerTrap(T.ctx);
        XINFO("sm120 trap: cuXtraTriggerTrap issued");
        return true;
    }
    if (getenv("XSCHED_SM120_TRAP_CMD") != nullptr && T.drv_preempt_buf != 0) {
        // Write the driver preemption command word the handler actually reads:
        // 2 = preempt_save+do_pause (park), 1 = preempt_restore (resume).
        uint32_t cmd = (uint32_t)strtoul(getenv("XSCHED_SM120_TRAP_CMD"), nullptr, 0);
        // Synchronous cuxtra copies are rejected while kernels run; use an
        // async copy on our own stream and poll instead of syncing.
        CUresult cr = Driver::MemcpyHtoDAsync_v2_ptsz(T.drv_preempt_buf + 4, &cmd,
                                                     sizeof cmd, T.wr_stream);
        int polls = 0;
        if (cr == CUDA_SUCCESS) {
            for (; polls < 200; polls++) {
                if (Driver::StreamQuery_ptsz(T.wr_stream) == CUDA_SUCCESS) break;
                usleep(50);
            }
        }
        XINFO("sm120 trap: cmd %u -> drv_pbuf+4 async r=%d polls=%d", cmd, (int)cr, polls);
    }
    uint32_t gen = T.shm->trigger_req + 1;
    XINFO("sm120 trap: request #%u (agent_ready=%u session=%#x)", gen,
          (unsigned)T.shm->agent_ready, T.shm->session);
    T.shm->trigger_req = gen;
    // The RM 0317 call blocks until the context is resumed; do not wait for
    // the agent's acknowledgement, the trap itself is what matters.  The
    // queue's Synchronize() decides whether the preemption worked.
    for (int i = 0; i < 40 && T.shm->trigger_ack != gen; i++) usleep(1000);
    bool ok = true;
    XINFO("sm120 trap: trigger sent (ack=%u status=%#x, not waited)",
          (unsigned)T.shm->trigger_ack, (unsigned)T.shm->trigger_status);
    if (ArmAtTrigger() && T.rec_host != nullptr) {
        std::thread([]() {
            for (int i = 0; i < 6; i++) {
                usleep(200000);
                XINFO("sm120 trap: rec +0x100 = %#x %#x %#x %#x (t=%d ms)",
                      T.rec_host[0x40], T.rec_host[0x41], T.rec_host[0x42],
                      T.rec_host[0x43], (i + 1) * 200);
            }
        }).detach();
    }
    if (!always) usleep(20000);   // let the trap land before restoring the handler

    // 2a. read the scratchpad back through its /dev/nvidiactl mapping (the cave
    //     may have stored a value there)
    if (getenv("XSCHED_SM120_TRAP_SCRATCH_READBACK") != nullptr && T.scratch_buf != 0) {
        uint64_t off = getenv("XSCHED_SM120_TRAP_DRVBUF_OFF")
                     ? strtoull(getenv("XSCHED_SM120_TRAP_DRVBUF_OFF"), nullptr, 0) : 4ULL;
        uint64_t rv = *(volatile uint64_t *)(T.scratch_buf + off);
        XINFO("sm120 trap: scratch readback +%#llx = %#llx",
              (unsigned long long)off, (unsigned long long)rv);
    }

    // 2b. optional host poke of the driver preemption command word (diagnostic)
    if (const char *pq = getenv("XSCHED_SM120_TRAP_HOSTPOKE_MS");
        pq != nullptr && T.drv_preempt_buf != 0) {
        long ms = strtol(pq, nullptr, 0);
        uint32_t val = 1;
        if (const char *pv = getenv("XSCHED_SM120_TRAP_HOSTPOKE_VAL")) {
            val = (uint32_t)strtoul(pv, nullptr, 0);
        }
        uint64_t va = T.drv_preempt_buf + 4;
        std::thread([va, ms, val]() {
            if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            HostPoke(va, val);
        }).detach();
        XINFO("sm120 trap: host poke armed in %ld ms (val=%u)", ms, val);
    }

    // 3. drop the session again so later launches do not stop on a debugger
    //    event, then restore the handler.  A holding suspend keeps it: the
    //    resume (0x83de0318) needs the session to still exist.
    bool hold = getenv("L3AGENT_HOLD") != nullptr ||
                getenv("XSCHED_SM120_TRAP_KEEP_SESSION") != nullptr;
    if (lazy && !hold) {
        uint32_t fg = T.shm->session_free_req + 1;
        T.shm->session_free_req = fg;
        // free is best-effort: the agent may still be blocked inside 0317
        for (int i = 0; i < 20 && T.shm->session_free_ack != fg; i++) usleep(1000);
    }
    if (!always) Disarm();
    return ok;
}

bool TrapSm120Context::Resume()
{
    if (T.shm == nullptr) return false;
    uint32_t gen = T.shm->resume_req + 1;
    T.shm->resume_req = gen;
    for (int i = 0; i < 400 && T.shm->resume_ack != gen; i++) usleep(5000);
    XINFO("sm120 trap: resume ack=%u status=%#x", (unsigned)T.shm->resume_ack,
          (unsigned)T.shm->resume_status);
    bool ok = T.shm->resume_ack == gen;
    if (ok && getenv("XSCHED_SM120_TRAP_FREE_AFTER_RESUME") != nullptr) {
        // A live debugger session keeps the context in debugger-managed mode,
        // which kills newly launched kernels; drop it once the resume landed.
        uint32_t fg = T.shm->session_free_req + 1;
        T.shm->session_free_req = fg;
        for (int i = 0; i < 100 && T.shm->session_free_ack != fg; i++) usleep(1000);
        XINFO("sm120 trap: session released after resume (ack=%u)",
              (unsigned)T.shm->session_free_ack);
    }
    return ok;
}
