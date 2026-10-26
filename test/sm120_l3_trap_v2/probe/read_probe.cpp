// read_probe.cpp - which device-read primitive can reach the driver's internal
// trap-handler arena, and which failures are survivable?
//
// cuxtra's cuXtraMemcpyDtoH calls exit(1) on any error, so it cannot be used to
// probe an address range.  This probe measures the alternatives:
//   - public cuMemcpyDtoH_v2 / cuMemGetAddressRange / cuPointerGetAttribute on
//     the tools image, the derived arena base, several intra-arena offsets and
//     a wild address
//   - context health after each attempt
//   - fatality of a cuxtra read of a wild address, isolated in a fork()ed child

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

#include <cuda.h>
extern "C" {
#include <cuxtra/cuxtra.h>
}
extern "C" CUresult cuGetExportTable(const void **ppTable, const CUuuid *pTableId);

#define CU_CHECK(x) do { CUresult _r = (x); if (_r != CUDA_SUCCESS) { \
    fprintf(stderr, "[FATAL] %s -> %d\n", #x, (int)_r); exit(1);} } while (0)

static const char *kTrapHandlerEtid = "cc529e945c4e9d46837c9625868334e4";
static const char *kToolsMemoryEtid = "bfdb432dbf3c5a4a945eb34029e81e75";

static CUuuid uuid_of(const char *h)
{
    CUuuid u;
    for (int i = 0; i < 16; i++) { unsigned v; sscanf(h + i * 2, "%2x", &v); u.bytes[i] = (char)v; }
    return u;
}

struct BlackwellTrapInfo { uint64_t flags, struct_size; void *handle; uint64_t stub, hsize, rsv; };

static const char *res(CUresult r)
{
    const char *s = nullptr;
    cuGetErrorName(r, &s);
    return s ? s : "?";
}

static uint32_t imm32(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return (uint32_t)(w >> 32); }

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    uint64_t wild = argc > 1 ? strtoull(argv[1], nullptr, 0) : 0x700000000000ull;
    CU_CHECK(cuInit(0));
    CUdevice dev; CU_CHECK(cuDeviceGet(&dev, 0));
    CUcontext ctx; CU_CHECK(cuCtxCreate(&ctx, 0, dev));
    CUstream s; CU_CHECK(cuStreamCreate(&s, CU_STREAM_NON_BLOCKING));

    CUuuid t = uuid_of(kTrapHandlerEtid), m = uuid_of(kToolsMemoryEtid);
    const void **tt = nullptr, **mt = nullptr;
    CU_CHECK(cuGetExportTable((const void **)&tt, &t));
    CU_CHECK(cuGetExportTable((const void **)&mt, &m));
    BlackwellTrapInfo info; memset(&info, 0, sizeof info);
    CU_CHECK(((CUresult(*)(CUcontext, void*))tt[20])(ctx, &info));
    uint64_t pc = 0, sz = 0;
    CU_CHECK(((CUresult(*)(CUcontext, void*, uint64_t*))mt[24])(ctx, info.handle, &pc));
    CU_CHECK(((CUresult(*)(void*, uint64_t*))mt[25])(info.handle, &sz));
    printf("[info] tools image pc=%#llx size=%#llx\n", (unsigned long long)pc, (unsigned long long)sz);

    std::vector<uint8_t> img(sz), buf(4096);
    size_t got = cuXtraMemcpyDtoH(img.data(), pc, sz, s);
    printf("[cuxtra] tools image read %zu/%llu\n", got, (unsigned long long)sz);

    // arena layout on 595.99.02: trap_reason_table sits at base + 0xc38300
    uint64_t trap_reason_table = (uint64_t)imm32(&img[0x130]) | ((uint64_t)imm32(&img[0x140]) << 32);
    uint64_t scratchpad        = (uint64_t)imm32(&img[0x010]) | ((uint64_t)imm32(&img[0x020]) << 32);
    uint64_t base = trap_reason_table - 0xc38300;
    printf("[arena] trap_reason_table=%#llx scratchpad=%#llx -> base=%#llx span=%#llx\n",
           (unsigned long long)trap_reason_table, (unsigned long long)scratchpad,
           (unsigned long long)base, (unsigned long long)(scratchpad - base));

    struct T { const char *name; uint64_t va; };
    T tests[] = {
        {"tools image", pc},
        {"arena base", base},
        {"base+0x0c12600", base + 0x0c12600},
        {"base+0x12b8f00", base + 0x12b8f00},
        {"base+0x15f5300", base + 0x15f5300},
        {"scratchpad", scratchpad},
        {"scratchpad+1M", scratchpad + 0x100000},
        {"wild", wild},
    };
    for (auto &x : tests) {
        CUresult r = cuMemcpyDtoH_v2(buf.data(), x.va, 64);
        CUdeviceptr b = 0; size_t bsz = 0;
        CUresult r2 = cuMemGetAddressRange(&b, &bsz, x.va);
        unsigned memtype = 0xdead;
        CUresult r3 = cuPointerGetAttribute(&memtype, CU_POINTER_ATTRIBUTE_MEMORY_TYPE, x.va);
        CUdevice d2;
        CUresult rh = cuCtxGetDevice(&d2);
        printf("[pub ] %-16s va=%#014llx memcpy=%-26s range=%-14s(%#llx+%#zx) ptrattr=%-22s memtype=%#x ctx=%s\n",
               x.name, (unsigned long long)x.va, res(r), res(r2),
               (unsigned long long)b, bsz, res(r3), memtype, res(rh));
    }

    CUdeviceptr p = 0;
    printf("[health] cuMemAlloc(4K) = %s\n", res(cuMemAlloc(&p, 4096)));
    if (p) cuMemFree(p);

    pid_t pid = fork();
    if (pid == 0) {
        fflush(nullptr);
        size_t g2 = cuXtraMemcpyDtoH(buf.data(), wild, 64, s);
        printf("[child] cuxtra wild read -> %zu (survived)\n", g2);
        fflush(nullptr);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    printf("[fork ] child exited=%d code=%d signaled=%d sig=%d\n",
           WIFEXITED(st), WEXITSTATUS(st), WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    printf("[health] post-fork cuCtxGetDevice = %s\n", res(cuCtxGetDevice(&dev)));
    return 0;
}
