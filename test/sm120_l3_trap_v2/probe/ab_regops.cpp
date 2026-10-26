// ab_regops.cpp - NV2080_CTRL_CMD_GPU_EXEC_REG_OPS with the *real* 550/570
// parameter layout (from open-gpu-kernel-modules 570.86.16):
//
//   struct NV2080_CTRL_GPU_EXEC_REG_OPS_PARAMS {   // 0x30
//     NvHandle hClientTarget;      // +0x00
//     NvHandle hChannelTarget;     // +0x04
//     NvU32    bNonTransactional;  // +0x08
//     NvU32    reserved00[2];      // +0x0c
//     NvU32    regOpCount;         // +0x14
//     NvP64    regOps;             // +0x18
//     NV2080_CTRL_GR_ROUTE_INFO grRouteInfo; // +0x20 (16 B)
//   };
//
//   struct NV2080_CTRL_GPU_REG_OP {                // 0x20
//     NvU8  regOp;        // +0x00  0=READ_32 1=WRITE_32
//     NvU8  regType;      // +0x01  0=GLOBAL
//     NvU8  regStatus;    // +0x02
//     NvU8  regQuad;      // +0x03
//     NvU32 regGroupMask; // +0x04
//     NvU32 regSubGroupMask; // +0x08
//     NvU32 regOffset;    // +0x0c
//     NvU32 regValueHi;   // +0x10
//     NvU32 regValueLo;   // +0x14
//     NvU32 regAndNMaskHi;// +0x18
//     NvU32 regAndNMaskLo;// +0x1c
//   };
//
// The 570 source requires (hClientTarget == 0) == (hChannelTarget == 0),
// otherwise NV_ERR_INVALID_PARAM_STRUCT (0x3a).  For a user client the
// "both zero" form is the only usable one.
//
// Build: g++ -O2 -std=c++17 -I/usr/local/cuda/include ab_regops.cpp -o ab_regops -lcuda -ldl -lpthread

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <cuda.h>

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

extern "C" int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    int r = real_ioctl(fd, req, arg);
    if (in_hook || r || !arg || req != NV_ESC_RM_ALLOC) return r;
    in_hook = 1;
    NVOS21 a; memcpy(&a, arg, sizeof a);
    if (a.status == 0) {
        switch (a.hClass) {
        case 0x41:   if (L.fd < 0) L.fd = fd; break;
        case 0x80:   if (!L.device) { L.device = a.hObject; L.client = a.hRoot; } if (L.fd < 0) L.fd = fd; break;
        case 0x2080: if (!L.subdev) L.subdev = a.hObject; break;
        case 0xc96f: if (!L.channel) L.channel = a.hObject; break;
        case 0xcec0: if (!L.compute) L.compute = a.hObject; break;
        }
    }
    in_hook = 0;
    return r;
}

static int rm_control(int fd, uint32_t client, uint32_t obj, uint32_t cmd, void *p, uint32_t n, uint32_t flags)
{
    NVOS54 c; memset(&c, 0, sizeof c);
    c.hClient = client; c.hObject = obj; c.cmd = cmd;
    c.params = (uint64_t)(uintptr_t)p; c.paramsSize = n; c.flags = flags;
    int r = real_ioctl(fd, NV_ESC_RM_CONTROL, &c);
    return r ? (0x1000000 | r) : c.status;
}

struct RegOp32 {
    uint8_t  regOp, regType, regStatus, regQuad;
    uint32_t regGroupMask, regSubGroupMask;
    uint32_t regOffset;
    uint32_t regValueHi, regValueLo;
    uint32_t regAndNMaskHi, regAndNMaskLo;
};
static_assert(sizeof(RegOp32) == 32, "32-byte stride");

static uint8_t g_param[0x200];
static RegOp32 g_ops[8];

static void reset_ops(uint32_t reg, uint32_t value, uint32_t mask, uint8_t op)
{
    memset(g_ops, 0, sizeof g_ops);
    g_ops[0].regOp = op;          // 0 READ_32, 1 WRITE_32
    g_ops[0].regType = 0;         // GLOBAL
    g_ops[0].regOffset = reg;
    g_ops[0].regValueLo = value;
    g_ops[0].regAndNMaskLo = mask;
    g_ops[0].regAndNMaskHi = mask;
}

static int send(uint32_t psize, uint32_t hclient, uint32_t channel, uint32_t nontrans,
                uint32_t f0c, uint32_t f10, uint32_t count, uint32_t flags, uint32_t reg,
                uint32_t value, uint32_t mask, uint8_t op)
{
    memset(g_param, 0, sizeof g_param);
    reset_ops(reg, value, mask, op);
    memcpy(g_param + 0x00, &hclient, 4);
    memcpy(g_param + 0x04, &channel, 4);
    memcpy(g_param + 0x08, &nontrans, 4);
    memcpy(g_param + 0x0c, &f0c, 4);
    memcpy(g_param + 0x10, &f10, 4);
    memcpy(g_param + 0x14, &count, 4);
    uint64_t p = (uint64_t)(uintptr_t)g_ops;
    memcpy(g_param + 0x18, &p, 8);
    int st = rm_control(L.fd, L.client, L.subdev, 0x20800122, g_param, psize, flags);
    if (getenv("AB_VERBOSE")) {
        printf("  psize=%#x hc=%#x ch=%#x nt=%u n=%u fl=%#x -> st=%#x "
               "op[op=%u type=%u status=%#x off=%#x valLo=%#x maskLo=%#x]\n",
               psize, hclient, channel, nontrans, count, flags, st,
               g_ops[0].regOp, g_ops[0].regType, g_ops[0].regStatus,
               g_ops[0].regOffset, g_ops[0].regValueLo, g_ops[0].regAndNMaskLo);
    }
    return st;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool do_scan = false;
    uint32_t hclient = 0, channel = 0, nontrans = 0, f0c = 0, f10 = 0;
    uint32_t one_psize = 0x30, flags = 0;
    uint32_t reg = 0, value = 0, mask = 0xffffffffu;
    uint8_t op = 0;
    bool have_psize = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--write")) op = 1;
        else if (!strcmp(argv[i], "--scan")) do_scan = true;
        else if (!strcmp(argv[i], "--psize") && i + 1 < argc) { one_psize = (uint32_t)strtoul(argv[++i], nullptr, 0); have_psize = true; }
        else if (!strcmp(argv[i], "--hclient") && i + 1 < argc) hclient = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--channel") && i + 1 < argc) channel = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--nontrans") && i + 1 < argc) nontrans = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--f0c") && i + 1 < argc) f0c = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--f10") && i + 1 < argc) f10 = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--flags") && i + 1 < argc) flags = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--reg") && i + 1 < argc) reg = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--value") && i + 1 < argc) value = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--mask") && i + 1 < argc) mask = (uint32_t)strtoul(argv[++i], nullptr, 0);
    }

    CUresult r = cuInit(0);
    if (r != CUDA_SUCCESS) { fprintf(stderr, "cuInit %d\n", (int)r); return 1; }
    CUdevice dev; CUcontext ctx;
    cuDeviceGet(&dev, 0);
    cuDevicePrimaryCtxRetain(&ctx, dev);
    cuCtxSetCurrent(ctx);
    printf("[learn] fd=%d client=%#x device=%#x subdev=%#x channel=%#x compute=%#x\n",
           L.fd, L.client, L.device, L.subdev, L.channel, L.compute);
    if (!L.subdev) { fprintf(stderr, "no subdevice\n"); return 1; }

    if (do_scan) {
        printf("== psize scan (hClient=0, channel=0, reg=%#x op=%u) ==\n", reg, op);
        for (uint32_t ps = 0x20; ps <= 0x80; ps += 8) {
            int st = send(ps, hclient, channel, nontrans, f0c, f10, 1, flags, reg, value, mask, op);
            printf("psize=%#04x -> status=%#x%s\n", ps, st, st == 0 ? "  <-- accepted" : "");
        }
    } else {
        int st = send(one_psize, hclient, channel, nontrans, f0c, f10, 1, flags, reg, value, mask, op);
        printf("psize=%#x hc=%#x ch=%#x nt=%u op=%u reg=%#x val=%#x mask=%#x -> "
               "status=%#x regStatus=%#x valueLo=%#x\n",
               one_psize, hclient, channel, nontrans, op, reg, value, mask,
               st, g_ops[0].regStatus, g_ops[0].regValueLo);
        (void)have_psize;
    }
    return 0;
}
