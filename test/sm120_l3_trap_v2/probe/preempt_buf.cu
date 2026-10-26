// preempt_buf.cu - drive the sm120 CILP preemption buffer directly.
//
// The live trap handler (see FINDINGS_ZH.md v0.1) derives its dispatch mask from
// a driver-internal "CUDA preemption buffer":
//     [buf+4] == 2  -> reason |= SAVE|PAUSE_QUIET   (CILP save, then spin)
//     [buf+4] == 1  -> reason |= RESTORE|PAUSE_QUIET
//     [buf+4] == 0  -> no reason -> handler returns to the trapped PC
// So writing the command word and forcing a trap should suspend a *running*
// kernel without patching a single instruction.  Observation is out-of-band: the
// spin kernel publishes its iteration counter into mapped host memory, so the
// host never needs a CUDA call to see whether the device is still advancing
// (the v1 experiments sampled with cudaMemcpy from the same process, which
// blocks instead of reporting a frozen device).
//
// Modes (cumulative):
//   --info            locate arena / live handler / preemption buffer
//   --read            read the preemption buffer header (may be inaccessible)
//   --cmd N           write N to [buf+4]
//   --trigger         cuXtraTriggerTrap()
//   --spin            run the bounded spin kernel with the host-visible counter
//
// Build: ../build.sh preempt_buf     Run:
//   CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1 \
//   ./preempt_buf --info --read --spin --cmd 2 --trigger

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <cuda_runtime.h>
#include <cuda.h>

extern "C" {
#include <cuxtra/cuxtra.h>
}
extern "C" CUresult cuGetExportTable(const void **ppTable, const CUuuid *pTableId);

namespace cuxtra {
namespace etbl { struct CUtoolsMemObjHandle_st; }
namespace cuda {
bool FindMemObjHandle(CUctx_st *ctx, unsigned long long va,
                      cuxtra::etbl::CUtoolsMemObjHandle_st **obj,
                      unsigned long long *offset, unsigned long *size);
}
}

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)
#define CU_CHECK(x) do { CUresult _r = (x); if (_r != CUDA_SUCCESS) { \
    fprintf(stderr, "[FATAL] %s -> %d @%d\n", #x, (int)_r, __LINE__); exit(1);} } while (0)

static const char *kTrapHandlerEtid = "cc529e945c4e9d46837c9625868334e4";
static const char *kToolsMemoryEtid = "bfdb432dbf3c5a4a945eb34029e81e75";
static const uint64_t kTrapReasonTableOff = 0xc38300; // 595.99.02 arena layout
static const uint64_t kLiveTrapHandlerOff = 0x15f0f00; // .text.traphandler (installed)
static const size_t kPreemptBufRelocOff = 0x250;       // MOV pair holding the buffer VA

// first opcode words of libcuda's .text.traphandler (relocation-free prefix)
static const uint32_t kTrapHandlerFp[10] = {
    0x0000795c, 0x1a007956, 0xff007387, 0xff007387, 0xff007387,
    0x00047202, 0x00057202, 0xff067803, 0x17077355, 0xff007387,
};

// ------------------------------------------------------------------- device side
// Publishes progress straight into mapped host memory: no CUDA call is needed
// to observe a freeze.  Bit 63 marks completion.
__global__ void spin_publish(volatile unsigned long long *hc, unsigned long long cycles)
{
    unsigned long long start = clock64();
    unsigned long long i = 0;
    while (clock64() - start < cycles) {
        i++;
        if ((i & 0xff) == 0) *hc = i;
    }
    *hc = i | (1ULL << 63);
}

static uint32_t opcode_at(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return (uint32_t)w; }
static uint32_t imm32_at(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return (uint32_t)(w >> 32); }

static CUuuid uuid_of(const char *h)
{
    CUuuid u;
    for (int i = 0; i < 16; i++) { unsigned v; sscanf(h + i * 2, "%2x", &v); u.bytes[i] = (char)v; }
    return u;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool do_info = false, do_read = false, do_spin = false, do_trigger = false;
    long cmd = -1;
    long blocks = 64, spin_ms = 30000;
    uint64_t th_off = kLiveTrapHandlerOff, buf_va_override = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--info") do_info = true;
        else if (a == "--read") do_read = true;
        else if (a == "--spin") do_spin = true;
        else if (a == "--trigger") do_trigger = true;
        else if (a == "--cmd") cmd = strtol(next().c_str(), nullptr, 0);
        else if (a == "--blocks") blocks = strtol(next().c_str(), nullptr, 0);
        else if (a == "--spin-ms") spin_ms = strtol(next().c_str(), nullptr, 0);
        else if (a == "--th-off") th_off = strtoull(next().c_str(), nullptr, 0);
        else if (a == "--buf") buf_va_override = strtoull(next().c_str(), nullptr, 0);
        else { fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }

    RT_CHECK(cudaFree(0));
    CUcontext ctx = nullptr;
    CU_CHECK(cuCtxGetCurrent(&ctx));
    CUstream opstream;
    CU_CHECK(cuStreamCreate(&opstream, CU_STREAM_NON_BLOCKING));
    printf("[ctx] %p\n", (void *)ctx);

    // ---- tools image -> arena base
    CUuuid te = uuid_of(kTrapHandlerEtid), me = uuid_of(kToolsMemoryEtid);
    const void **tt = nullptr, **mt = nullptr;
    CU_CHECK(cuGetExportTable((const void **)&tt, &te));
    CU_CHECK(cuGetExportTable((const void **)&mt, &me));
    struct { uint64_t flags, ssize; void *handle; uint64_t stub, hsize, rsv; } info;
    memset(&info, 0, sizeof info);
    CU_CHECK(((CUresult(*)(CUcontext, void *))tt[20])(ctx, &info));
    uint64_t tools_pc = 0, tools_sz = 0;
    CU_CHECK(((CUresult(*)(CUcontext, void *, uint64_t *))mt[24])(ctx, info.handle, &tools_pc));
    CU_CHECK(((CUresult(*)(void *, uint64_t *))mt[25])(info.handle, &tools_sz));
    std::vector<uint8_t> img(tools_sz);
    if (cuXtraMemcpyDtoH(img.data(), tools_pc, tools_sz, opstream) != tools_sz) {
        fprintf(stderr, "tools image read failed\n"); return 1;
    }
    uint64_t scratchpad = (uint64_t)imm32_at(&img[0x010]) | ((uint64_t)imm32_at(&img[0x020]) << 32);
    uint64_t trt        = (uint64_t)imm32_at(&img[0x130]) | ((uint64_t)imm32_at(&img[0x140]) << 32);
    uint64_t base = trt - kTrapReasonTableOff;
    printf("[arena] base=%#llx scratchpad=%#llx tools_image=%#llx (base+%#llx)\n",
           (unsigned long long)base, (unsigned long long)scratchpad,
           (unsigned long long)tools_pc, (unsigned long long)(tools_pc - base));

    // ---- live trap handler
    uint64_t th_va = base + th_off;
    cuxtra::etbl::CUtoolsMemObjHandle_st *th_obj = nullptr;
    unsigned long long th_off_in_obj = 0; unsigned long th_obj_size = 0;
    bool th_known = cuxtra::cuda::FindMemObjHandle(ctx, th_va, &th_obj, &th_off_in_obj, &th_obj_size);
    printf("[handler] va=%#llx (base+%#llx) memobj=%s obj=%p size=%#x\n",
           (unsigned long long)th_va, (unsigned long long)th_off,
           th_known ? "yes" : "NO", (void *)th_obj, th_obj_size);
    std::vector<uint8_t> th(2304);
    if (th_known) {
        size_t got = cuXtraMemcpyDtoH(th.data(), th_va, th.size(), opstream);
        printf("[handler] read %zu bytes\n", got);
        bool ok = got == th.size();
        for (size_t i = 0; ok && i < 10; i++)
            if (opcode_at(&th[i * 16]) != kTrapHandlerFp[i]) { ok = false; printf("[handler] fp mismatch at +%#zx\n", i * 16); }
        printf("[handler] fingerprint %s\n", ok ? "MATCH (.text.traphandler)" : "FAIL");
        uint64_t pbuf = (uint64_t)imm32_at(&th[kPreemptBufRelocOff]) |
                        ((uint64_t)imm32_at(&th[kPreemptBufRelocOff + 16]) << 32);
        printf("[handler] CUDA_PREEMPTION_BUFFER_PATCH_ADDR = %#llx (base%#llx)\n",
               (unsigned long long)pbuf, (long long)(pbuf - base) >= 0 ? (long long)(pbuf - base) : 0);
        if (!buf_va_override) buf_va_override = pbuf;
    }

    uint64_t pbuf = buf_va_override;
    if (!pbuf) { fprintf(stderr, "no preemption buffer address\n"); return 1; }
    printf("[preempt-buf] va=%#llx\n", (unsigned long long)pbuf);

    cuxtra::etbl::CUtoolsMemObjHandle_st *pb_obj = nullptr;
    unsigned long long pb_off = 0; unsigned long pb_size = 0;
    bool pb_known = cuxtra::cuda::FindMemObjHandle(ctx, pbuf, &pb_obj, &pb_off, &pb_size);
    printf("[preempt-buf] memobj=%s obj=%p objbase=%#llx size=%#lx\n", pb_known ? "yes" : "NO",
           (void *)pb_obj, (unsigned long long)pb_off, pb_size);

    // ---- optional read of the buffer header
    uint8_t hdr[256];
    bool hdr_ok = false;
    if (do_read) {
        memset(hdr, 0, sizeof hdr);
        size_t got = cuXtraMemcpyDtoH(hdr, pbuf, sizeof hdr, opstream);
        hdr_ok = got == sizeof hdr;
        printf("[preempt-buf] read %zu/%zu bytes\n", got, sizeof hdr);
        if (hdr_ok) {
            for (size_t r = 0; r < 64; r += 16) {
                printf("  +%#04zx", r);
                for (size_t c = 0; c < 16; c += 4)
                    printf(" %08x", *(uint32_t *)(hdr + r + c));
                printf("\n");
            }
            printf("[preempt-buf] command word [+4] = %u\n", *(uint32_t *)(hdr + 4));
        }
    }

    // ---- spin kernel with host-visible counter
    volatile unsigned long long *hc = nullptr;
    unsigned long long *hc_dev = nullptr;
    cudaStream_t sstream;
    if (do_spin) {
        void *p = nullptr;
        RT_CHECK(cudaHostAlloc(&p, 64, cudaHostAllocMapped));
        memset(p, 0, 64);
        hc = (volatile unsigned long long *)p;
        RT_CHECK(cudaHostGetDevicePointer((void **)&hc_dev, p, 0));
        RT_CHECK(cudaStreamCreateWithFlags(&sstream, cudaStreamNonBlocking));
        int hz = 0;
        cudaDeviceGetAttribute(&hz, cudaDevAttrClockRate, 0); // kHz
        unsigned long long cycles = (unsigned long long)(hz ? hz : 1500000) * 1000ULL * spin_ms / 1000ULL;
        printf("[spin] launching %ld blocks x 32 threads, ~%ld ms (%llu cycles, clock %d kHz)\n",
               blocks, spin_ms, cycles, hz);
        spin_publish<<<(int)blocks, 32, 0, sstream>>>(hc_dev, cycles);
        RT_CHECK(cudaGetLastError());
    }

    // watchdog: never let a frozen GPU hang the session
    std::atomic<bool> done{false};
    std::thread wd([&]() {
        for (int i = 0; i < 120; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (done.load()) return;
        }
        printf("[WATCHDOG] timeout, counter=%llu\n", hc ? *hc : 0ULL);
        _exit(77);
    });
    wd.detach();

    auto sample = [&](const char *tag) {
        unsigned long long v = hc ? *hc : 0ULL;
        printf("[sample] %-22s counter=%llu%s\n", tag, v & ~(1ULL << 63),
               (v >> 63) ? " (COMPLETED)" : "");
        return v;
    };

    if (do_spin) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); sample("pre"); }
    unsigned long long cpre = 0;
    if (do_spin) cpre = sample("just-before-cmd");

    // ---- write the command word
    if (cmd >= 0) {
        uint32_t v = (uint32_t)cmd;
        auto w0 = std::chrono::steady_clock::now();
        size_t put = cuXtraMemcpyHtoD(pbuf + 4, &v, sizeof v, opstream);
        auto w1 = std::chrono::steady_clock::now();
        printf("[cmd] wrote %u to [buf+4] -> %zu bytes in %.3f s\n", v, put,
               std::chrono::duration<double>(w1 - w0).count());
        uint32_t rb = 0;
        auto r0 = std::chrono::steady_clock::now();
        size_t gotrb = cuXtraMemcpyDtoH(&rb, pbuf + 4, 4, opstream);
        auto r1 = std::chrono::steady_clock::now();
        printf("[cmd] readback [+4] = %u (%zu bytes, %.3f s)\n", rb, gotrb,
               std::chrono::duration<double>(r1 - r0).count());
    }

    // ---- trigger
    if (do_trigger) {
        auto t0 = std::chrono::steady_clock::now();
        cuXtraTriggerTrap(ctx);
        auto t1 = std::chrono::steady_clock::now();
        printf("[trigger] cuXtraTriggerTrap returned in %.3f s\n",
               std::chrono::duration<double>(t1 - t0).count());
    }

    unsigned long long c0 = 0, c1 = 0, c2 = 0;
    if (do_spin) {
        c0 = sample("t+0");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        c1 = sample("t+0.4s");
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        c2 = sample("t+1.2s");
    }

    // ---- restore the command word, then look again
    if (cmd >= 0) {
        uint32_t z = 0;
        auto z0 = std::chrono::steady_clock::now();
        cuXtraMemcpyHtoD(pbuf + 4, &z, sizeof z, opstream);
        printf("[cmd] restored [+4] = 0 (%.3f s)\n",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - z0).count());
        if (do_trigger) cuXtraTriggerTrap(ctx);
    }
    unsigned long long c3 = 0, c4 = 0;
    if (do_spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        c3 = sample("after-restore+0.4s");
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        c4 = sample("after-restore+1.2s");
    }

    if (do_spin) {
        printf("[wait] synchronizing spin stream...\n");
        cudaError_t e = cudaStreamSynchronize(sstream);
        printf("[wait] sync=%s counter=%llu\n", cudaGetErrorName(e), sample("final"));
    }

    auto mask = ~(1ULL << 63);
    printf("\n[VERDICT] during-cmd delta=%lld  after-restore delta=%lld  => %s\n",
           (long long)((c2 & mask) - (c0 & mask)), (long long)((c4 & mask) - (c3 & mask)),
           ((c2 & mask) == (c0 & mask) && (c4 & mask) > (c3 & mask))
               ? "FROZEN while cmd set, RESUMED after clear  <<< L3 SUSPEND WORKS"
               : (((c2 & mask) > (c0 & mask)) ? "still running (command word had no effect)"
                                              : "inconclusive"));
    done.store(true);
    return 0;
}
