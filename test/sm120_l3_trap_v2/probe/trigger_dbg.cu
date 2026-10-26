// trigger_dbg.cu - Ampere trap-path probe:
//   1. report the driver trap handler (cuXtraGetTrapHandlerInfo)
//   2. optionally install XSched-style debugger params on the kernel function
//   3. run a spin kernel and pulse 0x419e84 bit31 (or call cuXtraTriggerTrap)
//   4. sample a host-mapped counter to see whether the kernel is stopped
//
// Build (A100): nvcc -O2 -std=c++17 -arch=sm_80 -ccbin g++ trigger_dbg.cu -o trigger_dbg \
//   -I<cuxtra>/include <cuxtra>/lib/libcuxtra_linux_x86_64.a -lcuda -ldl -lpthread -lrt
// Build (3090): ... -arch=sm_86 ...

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
#include <cuda_runtime.h>
#include <cuxtra/cuxtra.h>

struct NVOS54 { uint32_t hClient, hObject, cmd, flags;
                uint64_t params; uint32_t paramsSize; int32_t status; };
struct NVOS21 { uint32_t hRoot, hParent, hObject, hClass;
                uint64_t pAllocParms, pRightsRequested;
                uint32_t paramsSize, flags; int32_t status; };
static const unsigned long NV_ESC_RM_ALLOC = 0xc030462bUL;
static const unsigned long NV_ESC_RM_CONTROL = 0xc020462aUL;

static int (*real_ioctl)(int, unsigned long, ...) = nullptr;
static __thread int in_hook = 0;
static uint32_t g_client = 0, g_subdev = 0;
static int g_fd = -1;

extern "C" int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    int r = real_ioctl(fd, req, arg);
    if (in_hook || r || !arg || req != NV_ESC_RM_ALLOC) return r;
    in_hook = 1;
    NVOS21 a; memcpy(&a, arg, sizeof a);
    if (a.status == 0) {
        if (a.hClass == 0x80 && !g_client) { g_client = a.hRoot; g_fd = fd; }
        if (a.hClass == 0x2080 && !g_subdev) g_subdev = a.hObject;
    }
    in_hook = 0;
    return r;
}

struct RegOp32 {
    uint8_t  regOp, regType, regStatus, regQuad;
    uint32_t regGroupMask, regSubGroupMask, regOffset;
    uint32_t regValueHi, regValueLo, regAndNMaskHi, regAndNMaskLo;
};
static_assert(sizeof(RegOp32) == 32, "32-byte stride");

static int reg_pulse(uint32_t reg, uint32_t value, uint32_t mask)
{
    uint8_t param[0x200];
    RegOp32 ops[2];
    memset(param, 0, sizeof param);
    memset(ops, 0, sizeof ops);
    for (int i = 0; i < 2; i++) {
        ops[i].regOp = 1; ops[i].regType = 0; ops[i].regOffset = reg;
        ops[i].regValueLo = (i == 0) ? value : 0;
        ops[i].regAndNMaskLo = mask; ops[i].regAndNMaskHi = mask;
    }
    uint32_t hc = 0, ch = 0, nt = 0, cnt = 2;
    memcpy(param + 0x00, &hc, 4); memcpy(param + 0x04, &ch, 4);
    memcpy(param + 0x08, &nt, 4); memcpy(param + 0x14, &cnt, 4);
    uint64_t p = (uint64_t)(uintptr_t)ops;
    memcpy(param + 0x18, &p, 8);
    NVOS54 c; memset(&c, 0, sizeof c);
    c.hClient = g_client; c.hObject = g_subdev; c.cmd = 0x20800122;
    c.params = (uint64_t)(uintptr_t)param; c.paramsSize = 0x30;
    int r = real_ioctl(g_fd, NV_ESC_RM_CONTROL, &c);
    int st = r ? (0x1000000 | r) : c.status;
    printf("[trigger] pulse %#x -> st=%#x s0=%#x s1=%#x\n",
           reg, st, ops[0].regStatus, ops[1].regStatus);
    return st;
}

__global__ void spin_kernel(volatile unsigned long long *cnt, volatile int *stop,
                            unsigned long long limit)
{
    for (unsigned long long i = 0; i < limit && *stop == 0; i++) *cnt = i;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned long long limit = 3000000000ULL;
    int delay_ms = 1000;
    bool use_cuxtra = false, use_dbg = false;
    if (argc > 1) limit = strtoull(argv[1], nullptr, 0);
    if (argc > 2) delay_ms = atoi(argv[2]);
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--cuxtra")) use_cuxtra = true;
        if (!strcmp(argv[i], "--dbg")) use_dbg = true;
    }

    cudaError_t ce = cudaSetDevice(0);
    if (ce != cudaSuccess) { fprintf(stderr, "cudaSetDevice: %s\n", cudaGetErrorString(ce)); return 1; }
    cuInit(0);
    CUcontext ctx = nullptr;
    cuCtxGetCurrent(&ctx);
    printf("[init] fd=%d client=%#x subdev=%#x ctx=%p\n", g_fd, g_client, g_subdev, (void *)ctx);

    CUdeviceptr handler = 0;
    size_t hsize = 0;
    cuXtraGetTrapHandlerInfo(ctx, &handler, &hsize);
    printf("[handler] dev=%#llx size=%#zx\n", (unsigned long long)handler, hsize);

    unsigned long long *h_cnt = nullptr, *d_cnt = nullptr;
    int *h_stop = nullptr, *d_stop = nullptr;
    cudaHostAlloc((void **)&h_cnt, sizeof(unsigned long long), cudaHostAllocMapped);
    cudaHostGetDevicePointer((void **)&d_cnt, h_cnt, 0);
    cudaHostAlloc((void **)&h_stop, sizeof(int), cudaHostAllocMapped);
    cudaHostGetDevicePointer((void **)&d_stop, h_stop, 0);
    *h_cnt = 0; *h_stop = 0;

    cudaFunction_t sym = nullptr;
    cudaError_t ge = cudaGetFuncBySymbol(&sym, (const void *)spin_kernel);
    printf("[func] getFuncBySymbol=%s sym=%p\n", cudaGetErrorString(ge), (void *)sym);
    CUfunction func = (CUfunction)sym;

    CUdeviceptr pbuf = 0;
    cuMemAlloc(&pbuf, 0x1000);
    cuMemsetD8(pbuf, 0, 0x1000);
    if (use_dbg && func) {
        struct { uint64_t preempt; uint64_t guardian; int64_t idx; uint32_t killable; } args;
        args.preempt = pbuf; args.guardian = 0; args.idx = 1; args.killable = 1;
        cuXtraSetDebuggerParams(func, &args, sizeof args);
        printf("[dbg] set debugger params preempt=%#llx guardian=0 idx=1 killable=1\n",
               (unsigned long long)pbuf);
    }

    void *kargs[3] = { &d_cnt, &d_stop, &limit };
    cudaStream_t s; cudaStreamCreate(&s);
    if (func) {
        CUresult lr = cuLaunchKernel(func, 1, 1, 1, 1, 1, 1, 0, (CUstream)s, kargs, nullptr);
        printf("[launch] cuLaunchKernel=%d\n", (int)lr);
    } else {
        spin_kernel<<<1, 1, 0, s>>>(d_cnt, d_stop, limit);
        printf("[launch] runtime err=%s\n", cudaGetErrorString(cudaGetLastError()));
    }

    usleep(delay_ms * 1000);
    unsigned long long before = *h_cnt;
    printf("[before] counter=%llu\n", before);

    if (use_cuxtra) {
        cuXtraTriggerTrap(ctx);
        printf("[trigger] cuXtraTriggerTrap issued\n");
    } else {
        reg_pulse(0x00419e84, 0x80000000u, 0x80000000u);
    }

    unsigned long long last = before, frozen = 0;
    for (int i = 0; i < 600; i++) {
        usleep(5000);
        unsigned long long v = *h_cnt;
        if (v == last) {
            frozen++;
            if (frozen == 3 || frozen == 20 || frozen == 100)
                printf("[sample] frozen x%llu at counter=%llu (t=%dms)\n",
                       frozen, v, (i + 1) * 5);
        } else {
            if (frozen) printf("[sample] resumed after %llu samples, counter=%llu\n", frozen, v);
            frozen = 0;
        }
        last = v;
        if ((i % 100) == 99) printf("[sample] t=%dms counter=%llu\n", (i + 1) * 5, v);
    }
    printf("[after] counter=%llu frozen_last=%llu\n", last, frozen);

    *h_stop = 1;
    cudaError_t q = cudaErrorNotReady;
    int polls = 0;
    for (; polls < 200; polls++) {
        q = cudaStreamQuery(s);
        if (q != cudaErrorNotReady) break;
        usleep(10000);
    }
    printf("[stop] polls=%d stream=%s last=%s\n", polls, cudaGetErrorString(q),
           cudaGetErrorString(cudaGetLastError()));
    return 0;
}
