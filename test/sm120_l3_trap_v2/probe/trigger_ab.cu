// trigger_ab.cu - write the in-band trigger register (0x419e84 bit31) while a
// kernel is running, and observe whether the kernel is stopped by the trap.
//
// Uses the corrected 550/570 NV2080_CTRL_GPU_EXEC_REG_OPS layout (see
// ab_regops.cpp): hClientTarget == hChannelTarget == 0 for a user client.
//
// Build (A100): nvcc -O2 -std=c++17 -arch=sm_80 -ccbin g++ trigger_ab.cu -o trigger_ab -lcuda -ldl -lpthread
// Build (3090): nvcc -O2 -std=c++17 -arch=sm_86 -ccbin g++ trigger_ab.cu -o trigger_ab -lcuda -ldl -lpthread
// Run: ./trigger_ab [limit] [delay_ms]

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

static uint8_t g_param[0x200];
static RegOp32 g_ops[4];

static int reg_pulse(uint32_t reg, uint32_t value, uint32_t mask)
{
    memset(g_param, 0, sizeof g_param);
    memset(g_ops, 0, sizeof g_ops);
    // cuxtra's GlobalRegsWrite32 sends both writes in ONE control call:
    // set bit31, then clear bit31 (a pulse).
    for (int i = 0; i < 2; i++) {
        g_ops[i].regOp = 1;          // WRITE_32
        g_ops[i].regType = 0;        // GLOBAL
        g_ops[i].regOffset = reg;
        g_ops[i].regValueLo = (i == 0) ? value : 0;
        g_ops[i].regAndNMaskLo = mask;
        g_ops[i].regAndNMaskHi = mask;
    }
    uint32_t hc = 0, ch = 0, nt = 0, cnt = 2;
    memcpy(g_param + 0x00, &hc, 4);
    memcpy(g_param + 0x04, &ch, 4);
    memcpy(g_param + 0x08, &nt, 4);
    memcpy(g_param + 0x14, &cnt, 4);
    uint64_t p = (uint64_t)(uintptr_t)g_ops;
    memcpy(g_param + 0x18, &p, 8);
    NVOS54 c; memset(&c, 0, sizeof c);
    c.hClient = g_client; c.hObject = g_subdev; c.cmd = 0x20800122;
    c.params = (uint64_t)(uintptr_t)g_param; c.paramsSize = 0x30;
    int r = real_ioctl(g_fd, NV_ESC_RM_CONTROL, &c);
    int st = r ? (0x1000000 | r) : c.status;
    printf("[trigger] pulse reg=%#x val=%#x mask=%#x -> st=%#x status0=%#x status1=%#x\n",
           reg, value, mask, st, g_ops[0].regStatus, g_ops[1].regStatus);
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
    if (argc > 1) limit = strtoull(argv[1], nullptr, 0);
    if (argc > 2) delay_ms = atoi(argv[2]);

    cudaError_t ce = cudaSetDevice(0);
    if (ce != cudaSuccess) { fprintf(stderr, "cudaSetDevice: %s\n", cudaGetErrorString(ce)); return 1; }
    cuInit(0);
    printf("[init] fd=%d client=%#x subdev=%#x\n", g_fd, g_client, g_subdev);

    unsigned long long *h_cnt = nullptr;
    unsigned long long *d_cnt = nullptr;
    int *h_stop = nullptr, *d_stop = nullptr;
    ce = cudaHostAlloc((void **)&h_cnt, sizeof(unsigned long long), cudaHostAllocMapped);
    if (ce != cudaSuccess) { fprintf(stderr, "hostAlloc: %s\n", cudaGetErrorString(ce)); return 1; }
    ce = cudaHostGetDevicePointer((void **)&d_cnt, h_cnt, 0);
    if (ce != cudaSuccess) { fprintf(stderr, "getDevPtr: %s\n", cudaGetErrorString(ce)); return 1; }
    ce = cudaHostAlloc((void **)&h_stop, sizeof(int), cudaHostAllocMapped);
    if (ce != cudaSuccess) { fprintf(stderr, "hostAlloc stop: %s\n", cudaGetErrorString(ce)); return 1; }
    ce = cudaHostGetDevicePointer((void **)&d_stop, h_stop, 0);
    if (ce != cudaSuccess) { fprintf(stderr, "getDevPtr stop: %s\n", cudaGetErrorString(ce)); return 1; }
    *h_cnt = 0;
    *h_stop = 0;

    cudaStream_t s; cudaStreamCreate(&s);
    spin_kernel<<<1, 1, 0, s>>>(d_cnt, d_stop, limit);
    ce = cudaGetLastError();
    printf("[launch] limit=%llu err=%s\n", limit, cudaGetErrorString(ce));

    usleep(delay_ms * 1000);
    unsigned long long before = *h_cnt;
    printf("[before] counter=%llu\n", before);

    int st1 = reg_pulse(0x00419e84, 0x80000000u, 0x80000000u);
    printf("[pulse] st=%#x\n", st1);

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
    printf("[stop] polls=%d stream=%s\n", polls, cudaGetErrorString(q));
    cudaError_t le = cudaGetLastError();
    printf("[last] %s\n", cudaGetErrorString(le));
    return 0;
}
