// regops.cpp - NV2080_CTRL_CMD_GPU_EXEC_REG_OPS (0x20800122) from userspace.
//
// Recovered from nv-kernel.o_binary:
//   subdeviceCtrlCmdGpuExecRegOps_IMPL @0x2dd410, table entry
//   cmd=0x20800122 psize=0x30 flags=0x10118.
//   The IMPL forwards (pGpu, p[0], p[4], p[8], p[0x18], 0, p[0x14]) to the
//   reg-op executor, so the 48-byte param blob is
//     {NvHandle hClient; NvHandle hChannel; NvHandle hObjectContext;
//      NvU32 regOpCount; NvU32 flags; NV2080_CTRL_GPU_REG_OP regOps[1]}
//   with NV2080_CTRL_GPU_REG_OP = {u8 regOp; u8 regStatus; u16 regQuad;
//      u32 regAndNackIndex; u32 regSubIndex; u32 regValue; u32 regMask;
//      u32 regShift} = 28 bytes -> 20 + 28 = 48.  Exactly the table's psize.
//
// If this works on a subdevice we own, we get privileged GR/global register
// access without any debugger session - which is the missing "deliver a trap
// to a running warp" primitive for sm120 L3.
//
// Build: ../build.sh regops    Run: CUXTRA_CUDA_LIB=... ./bin/regops

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <cuda.h>

#define CU_CHECK(x) do { CUresult _r = (x); if (_r != CUDA_SUCCESS) { \
    const char *s = nullptr; cuGetErrorName(_r, &s); \
    fprintf(stderr, "[FATAL] %s -> %d %s @%d\n", #x, (int)_r, s ? s : "?", __LINE__); exit(1);} } while (0)

struct NVOS54 { uint32_t hClient, hObject, cmd, flags;
                uint64_t params; uint32_t paramsSize; int32_t status; };
static_assert(sizeof(NVOS54) == 32, "RM ABI");
struct NVOS21 { uint32_t hRoot, hParent, hObject, hClass;
                uint64_t pAllocParms, pRightsRequested;
                uint32_t paramsSize, flags; int32_t status; };

static const unsigned long NV_ESC_RM_ALLOC   = 0xc030462bUL;
static const unsigned long NV_ESC_RM_CONTROL = 0xc020462aUL;

struct Learned { int fd = -1; uint32_t client = 0, device = 0, subdev = 0, compute = 0, channel = 0; } L;
static int (*real_ioctl)(int, unsigned long, ...) = nullptr;
static __thread int in_hook = 0;

static void learn(int fd, void *arg)
{
    NVOS21 a; memcpy(&a, arg, sizeof a);
    if (a.status) return;
    switch (a.hClass) {
    case 0x41:   if (L.fd < 0) L.fd = fd; break;
    case 0x80:   if (!L.device) { L.device = a.hObject; L.client = a.hRoot; } if (L.fd < 0) L.fd = fd; break;
    case 0x2080: if (!L.subdev) L.subdev = a.hObject; break;
    case 0xc96f: if (!L.channel) L.channel = a.hObject; break;
    case 0xcec0: if (!L.compute) L.compute = a.hObject; break;
    }
}

extern "C" int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    int r = real_ioctl(fd, req, arg);
    if (in_hook || r || !arg || req != NV_ESC_RM_ALLOC) return r;
    in_hook = 1; learn(fd, arg); in_hook = 0;
    return r;
}

static int rm_control(int fd, uint32_t client, uint32_t obj, uint32_t cmd, void *p, uint32_t n)
{
    NVOS54 c; memset(&c, 0, sizeof c);
    c.hClient = client; c.hObject = obj; c.cmd = cmd;
    c.params = (uint64_t)(uintptr_t)p; c.paramsSize = n;
    int r = real_ioctl(fd, NV_ESC_RM_CONTROL, &c);
    return r ? (0x1000000 | r) : c.status;
}

// subdeviceCtrlCmdGpuExecRegOps_cmn@0x2dcfc0 reads:
//   arg2 = p[0x00] hClient, arg3 = p[0x04] hChannel, arg4 = p[0x08] hObjectContext,
//   arg7 = p[0x14] regOpCount (must be != 0, else NV_ERR_INVALID_PARAM_STRUCT),
//   arg5 = p[0x18] NvP64 pRegOps, walked with a 32-byte stride, validating
//          *(NvU16*)(op+0x0a) via gpuValidateRegOffset.
// hChannel and hObjectContext must both be zero or both non-zero.
struct RegOp32 { uint8_t regOp; uint8_t regStatus; uint16_t regQuad;
                 uint32_t regAndNackIndex, regSubIndex;
                 uint16_t regOffset16; uint16_t pad16;
                 uint32_t regValue, regMask, regShift, regGrpMask; };
static_assert(sizeof(RegOp32) == 32, "reg ops have a 32-byte stride");
struct ExecParams { uint32_t hClient, hChannel, hObjectContext, field0c, field10,
                               regOpCount;
                    uint64_t pRegOps; uint32_t tail[4]; };
static_assert(sizeof(ExecParams) == 48, "must match the 0x20800122 psize");

enum { OP_GLOBAL = 0x00, OP_GR_CTX = 0x01, OP_GR_CTX_SM = 0x04, OP_WRITE = 0x01 };

static RegOp32 g_ops[4];

static void try_op(const char *tag, uint32_t hChannel, uint32_t hObjCtx, uint32_t field10,
                   uint8_t op, uint32_t reg, uint32_t value)
{
    ExecParams p; memset(&p, 0, sizeof p);
    memset(g_ops, 0, sizeof g_ops);
    p.hClient = L.client; p.hChannel = hChannel; p.hObjectContext = hObjCtx;
    p.field10 = field10; p.regOpCount = 1;
    p.pRegOps = (uint64_t)(uintptr_t)g_ops;
    g_ops[0].regOp = op;
    g_ops[0].regAndNackIndex = reg;
    g_ops[0].regOffset16 = (uint16_t)(reg >> 2);   // what gpuValidateRegOffset checks
    g_ops[0].regValue = value;
    g_ops[0].regMask = 0xffffffff;
    int st = rm_control(L.fd, L.client, L.subdev, 0x20800122, &p, sizeof p);
    printf("[%-28s] f10=%#x op=%#x reg=%#010x -> status=%#x regStatus=%#x value=%#010x\n",
           tag, field10, op, reg, st, g_ops[0].regStatus, g_ops[0].regValue);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool do_write = false;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--write")) do_write = true;

    CU_CHECK(cuInit(0));
    CUcontext ctx; CUdevice dev;
    CU_CHECK(cuDeviceGet(&dev, 0));
    CU_CHECK(cuDevicePrimaryCtxRetain(&ctx, dev));
    CU_CHECK(cuCtxSetCurrent(ctx));
    printf("[learn] fd=%d client=%#x device=%#x subdev=%#x channel=%#x compute=%#x\n",
           L.fd, L.client, L.device, L.subdev, L.channel, L.compute);
    if (!L.subdev) { fprintf(stderr, "no subdevice\n"); return 1; }

    const uint32_t PMC_BOOT_0 = 0x00000000, PTIMER_TIME_0 = 0x00200510, PTIMER_TIME_1 = 0x00200514;
    try_op("read PMC_BOOT_0",     0, 0, 0, OP_GLOBAL, PMC_BOOT_0, 0);
    try_op("read PTIMER_TIME_0",  0, 0, 0, OP_GLOBAL, PTIMER_TIME_0, 0);
    try_op("read PTIMER_TIME_1",  0, 0, 0, OP_GLOBAL, PTIMER_TIME_1, 0);
    try_op("read PMC_BOOT_0 f=1", 0, 0, 1, OP_GLOBAL, PMC_BOOT_0, 0);
    try_op("read GR_CTX",         0, 0, 0, OP_GR_CTX, PMC_BOOT_0, 0);
    try_op("read w/ channel",     L.channel, L.compute, 0, OP_GLOBAL, PTIMER_TIME_0, 0);

    if (do_write) {
        // 0x419e84 bit31 is the trap-trigger register cuXtraTriggerTrap pokes.
        try_op("WRITE 0x419e84 bit31", 0, 0, 0, (uint8_t)(OP_GLOBAL | OP_WRITE), 0x00419e84, 1u << 31);
    }
    return 0;
}
