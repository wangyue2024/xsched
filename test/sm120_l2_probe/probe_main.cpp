/*
 * probe_main.cpp - host-side driver for the T1 cuxtra probe (sm120 L2 study).
 *
 * Linked artifacts:
 *   probe_kernels_patched.cubin   (kernel_read_params patched 15 slots)
 *   libcuxtra_windows_amd64.a     (static; locate the real driver through
 *                                  the CUXTRA_CUDA_LIB environment variable)
 *
 * The probe talks to the CUDA driver ONLY through a private, locally
 * loaded nvcuda.dll (GetProcAddress table) so that the driver under test
 * is unambiguous, and reaches the cuxtra layer through the static library.
 *
 * Phases (each check prints [PASS]/[FAIL]):
 *   [P0] environment: driver/device/CC, module + function lookup,
 *        ref_use_param runtime sanity (parameter passing works)
 *   [P1] debugger-parameters window (VERIFIED: c[0x0][0x170..0x188] on
 *        this sm120 driver; the sm86 ABI used 0x1880):
 *        cuXtraSetDebuggerParams -> cuXtraGetDebuggerParams read-back ->
 *        kernel_read_params samples the four window cells + one pre-window
 *        negative control into `out` (patched LDC/LDC.64/ST.E.64 slots)
 *   [P2] entry-point redirect: run kernel_marker_b through the entry
 *        point of kernel_marker_a, then restore and re-verify
 *   [P3] instruction memory: allocate, upload a bare EXIT stub, redirect
 *        kernel_marker_a to it (out must stay untouched), restore
 *   [P4] binary & resource APIs: GetBinary / LocalRegs / BarrierCnt /
 *        ParamCount / ParamInfo
 *   [P5] entry-point stability
 *   [P6] OPT-IN ONLY (--p6): dump the driver trap handler via
 *        cuXtraGetTrapHandlerInfo.  KNOWN ISSUE: the call aborts with
 *        "invalid device ordinal" inside cuxtra on this driver, so P6 is
 *        skipped by default and kept for the record.
 *
 * Modes:
 *   probe_main.exe <cubin>                 full check sequence (P0..P5)
 *   probe_main.exe <cubin> --p6            also attempt P6 (may abort)
 *   probe_main.exe <cubin> --scan B S N    exhaustive cbank scan: set the
 *        four sentinel debugger params, then dump N consecutive u64
 *        samples at c[0x0][B + i*S] and flag sentinel matches + the
 *        kernel-parameter self-check (out pointer @0x380).
 *
 * Exit code = number of failed checks (0 = all pass).
 */

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "cuxtra/cuxtra.h"

/* ---------------- minimal dynamic CUDA driver API ---------------- */

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st  *CUcontext;
typedef struct CUmod_st  *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef struct CUstream_st *CUstream;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR 75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR 76

typedef CUresult (*pfn_cuInit)(unsigned);
typedef CUresult (*pfn_cuDriverGetVersion)(int *);
typedef CUresult (*pfn_cuDeviceGetCount)(int *);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice *, int);
typedef CUresult (*pfn_cuDeviceGetName)(char *, int, CUdevice);
typedef CUresult (*pfn_cuDeviceGetAttribute)(int *, int, CUdevice);
typedef CUresult (*pfn_cuCtxCreate)(CUcontext *, unsigned, CUdevice);
typedef CUresult (*pfn_cuModuleLoad)(CUmodule *, const char *);
typedef CUresult (*pfn_cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
typedef CUresult (*pfn_cuMemAlloc)(CUdeviceptr *, size_t);
typedef CUresult (*pfn_cuMemFree)(CUdeviceptr);
typedef CUresult (*pfn_cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
typedef CUresult (*pfn_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
typedef CUresult (*pfn_cuStreamCreate)(CUstream *, unsigned);
typedef CUresult (*pfn_cuStreamSynchronize)(CUstream);
typedef CUresult (*pfn_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                       unsigned, unsigned, unsigned, unsigned,
                                       CUstream, void **, void **);
typedef CUresult (*pfn_cuGetErrorName)(CUresult, const char **);

static pfn_cuInit                  drvInit;
static pfn_cuDriverGetVersion      drvGetVersion;
static pfn_cuDeviceGetCount        drvGetCount;
static pfn_cuDeviceGet             drvGet;
static pfn_cuDeviceGetName         drvGetName;
static pfn_cuDeviceGetAttribute    drvGetAttr;
static pfn_cuCtxCreate             drvCtxCreate;
static pfn_cuModuleLoad            drvModuleLoad;
static pfn_cuModuleGetFunction     drvModuleGetFunction;
static pfn_cuMemAlloc              drvMemAlloc;
static pfn_cuMemFree               drvMemFree;
static pfn_cuMemcpyHtoD            drvMemcpyHtoD;
static pfn_cuMemcpyDtoH            drvMemcpyDtoH;
static pfn_cuStreamCreate          drvStreamCreate;
static pfn_cuStreamSynchronize     drvStreamSync;
static pfn_cuLaunchKernel          drvLaunchKernel;
static pfn_cuGetErrorName          drvGetErrorName;

static HMODULE g_cuda = nullptr;

template <typename T>
static T cudaSym(const char *name)
{
    void *p = reinterpret_cast<void *>(GetProcAddress(g_cuda, name));
    if (p == nullptr) {
        std::printf("FATAL: nvcuda.dll export '%s' not found\n", name);
        std::exit(2);
    }
    return reinterpret_cast<T>(p);
}

static void loadDriverFns()
{
    drvInit             = cudaSym<pfn_cuInit>("cuInit");
    drvGetVersion       = cudaSym<pfn_cuDriverGetVersion>("cuDriverGetVersion");
    drvGetCount         = cudaSym<pfn_cuDeviceGetCount>("cuDeviceGetCount");
    drvGet              = cudaSym<pfn_cuDeviceGet>("cuDeviceGet");
    drvGetName          = cudaSym<pfn_cuDeviceGetName>("cuDeviceGetName");
    drvGetAttr          = cudaSym<pfn_cuDeviceGetAttribute>("cuDeviceGetAttribute");
    drvCtxCreate        = cudaSym<pfn_cuCtxCreate>("cuCtxCreate_v2");
    drvModuleLoad       = cudaSym<pfn_cuModuleLoad>("cuModuleLoad");
    drvModuleGetFunction = cudaSym<pfn_cuModuleGetFunction>("cuModuleGetFunction");
    drvMemAlloc         = cudaSym<pfn_cuMemAlloc>("cuMemAlloc_v2");
    drvMemFree          = cudaSym<pfn_cuMemFree>("cuMemFree_v2");
    drvMemcpyHtoD       = cudaSym<pfn_cuMemcpyHtoD>("cuMemcpyHtoD_v2");
    drvMemcpyDtoH       = cudaSym<pfn_cuMemcpyDtoH>("cuMemcpyDtoH_v2");
    drvStreamCreate      = cudaSym<pfn_cuStreamCreate>("cuStreamCreate");
    drvStreamSync       = cudaSym<pfn_cuStreamSynchronize>("cuStreamSynchronize");
    drvLaunchKernel     = cudaSym<pfn_cuLaunchKernel>("cuLaunchKernel");
    drvGetErrorName     = cudaSym<pfn_cuGetErrorName>("cuGetErrorName");
}

/* ------------------------- probe state --------------------------- */

static int g_fail = 0;

/* command-line modes (see main) */
static bool     g_run_p6     = false;
static uint64_t g_scan_base  = 0;
static uint64_t g_scan_step  = 0;
static int      g_scan_count = 0;

static CUcontext   ctx = nullptr;
static CUstream    stream = nullptr;
static CUmodule    mod = nullptr;
static CUfunction  fn_a = nullptr;      /* kernel_marker_a  */
static CUfunction  fn_b = nullptr;      /* kernel_marker_b  */
static CUfunction  fn_read = nullptr;   /* kernel_read_params (patched) */
static CUfunction  fn_ref = nullptr;    /* ref_use_param    */
static CUdeviceptr d_out = 0;

#define OUT_WORDS 64      /* >= 62 scan points x 8 bytes (one batch) */
static uint64_t h_out[OUT_WORDS];

static void report(const char *name, bool ok)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

static const char *errName(CUresult e)
{
    const char *s = "?";
    if (drvGetErrorName) drvGetErrorName(e, &s);
    return s ? s : "?";
}

#define CHECK(call)                                                      \
    do {                                                                 \
        CUresult _e = (call);                                            \
        if (_e != CUDA_SUCCESS) {                                        \
            std::printf("  [ERR ] %s -> CUresult %d (%s)\n",             \
                        #call, (int)_e, errName(_e));                    \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

static void fillOut(uint64_t v)
{
    for (int i = 0; i < OUT_WORDS; ++i) h_out[i] = v;
    CHECK(drvMemcpyHtoD(d_out, h_out, sizeof(h_out)));
}

static void readOut()
{
    CHECK(drvMemcpyDtoH(h_out, d_out, sizeof(h_out)));
}

static void launch1(CUfunction f)
{
    void *args[] = { &d_out };
    CHECK(drvLaunchKernel(f, 1, 1, 1, 1, 1, 1, 0, stream, args, nullptr));
    CHECK(drvStreamSync(stream));
}

static void dumpU32(const char *tag, const uint32_t *v, int n)
{
    std::printf("  [info] %s:", tag);
    for (int i = 0; i < n; ++i) std::printf(" u32[%d]=0x%08x", i, v[i]);
    std::printf("\n");
}

/* --------------------------- phases ------------------------------ */

static void phase0(const char *cubin_path)
{
    std::printf("[P0] environment & module load\n");

    CHECK(drvInit(0));
    int dver = 0;
    CHECK(drvGetVersion(&dver));
    std::printf("  [info] driver version: %d (%d.%d)\n",
                dver, dver / 1000, (dver % 1000) / 10);

    int ndev = 0;
    CHECK(drvGetCount(&ndev));
    report("at least one CUDA device", ndev > 0);
    if (ndev <= 0) return;

    CUdevice dev;
    CHECK(drvGet(&dev, 0));
    char name[256] = {0};
    CHECK(drvGetName(name, sizeof(name), dev));
    int ccMaj = 0, ccMin = 0;
    CHECK(drvGetAttr(&ccMaj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev));
    CHECK(drvGetAttr(&ccMin, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev));
    std::printf("  [info] device 0: %s, compute capability %d.%d\n",
                name, ccMaj, ccMin);
    report("compute capability 12.0 (sm_120)", ccMaj == 12 && ccMin == 0);

    const char *env = std::getenv("CUXTRA_CUDA_LIB");
    std::printf("  [info] CUXTRA_CUDA_LIB=%s\n", env ? env : "(unset)");

    CHECK(drvCtxCreate(&ctx, 0, dev));
    CHECK(drvStreamCreate(&stream, 0));
    CHECK(drvMemAlloc(&d_out, sizeof(h_out)));

    CHECK(drvModuleLoad(&mod, cubin_path));
    if (mod == nullptr) return;
    CHECK(drvModuleGetFunction(&fn_a, mod, "kernel_marker_a"));
    CHECK(drvModuleGetFunction(&fn_b, mod, "kernel_marker_b"));
    CHECK(drvModuleGetFunction(&fn_read, mod, "kernel_read_params"));
    CHECK(drvModuleGetFunction(&fn_ref, mod, "ref_use_param"));
    report("4 probe kernels resolved", fn_a && fn_b && fn_read && fn_ref);

    /* runtime sanity: parameter passing and DtoH round trip */
    fillOut(0xCCCCCCCCCCCCCCCCull);
    launch1(fn_ref);
    readOut();
    report("ref_use_param wrote out[1] = 0x1234567890abcdef",
           h_out[1] == 0x1234567890ABCDEFull);
}

static void phase1()
{
    std::printf("[P1] debugger-parameters window (c[0x0][0x170..0x188], sm120)\n");

    /* Fresh sentinel set (deliberately different from the one used by the
     * exhaustive scan) so a P1 pass additionally proves that the window
     * content follows cuXtraSetDebuggerParams - a sensitivity check. */
    uint64_t v1 = 0xA1A2A3A4A5A6A7A8ull;  /* preempt buffer addr slot */
    uint64_t v2 = 0xB1B2B3B4B5B6B7B8ull;  /* guardian entry slot       */
    uint64_t v3 = 0xC1C2C3C4C5C6C7C8ull;  /* kernel idx slot           */
    uint32_t v4 = 0xE1E2E3E4u;            /* killable slot             */

    uint8_t win[28];
    std::memcpy(win + 0,  &v1, 8);
    std::memcpy(win + 8,  &v2, 8);
    std::memcpy(win + 16, &v3, 8);
    std::memcpy(win + 24, &v4, 4);

    cuXtraSetDebuggerParams(fn_read, win, sizeof(win));
    uint8_t rb[28];
    std::memset(rb, 0, sizeof(rb));
    cuXtraGetDebuggerParams(fn_read, rb, 0, sizeof(rb));
    report("SetDebuggerParams -> GetDebuggerParams read-back identical",
           std::memcmp(rb, win, sizeof(win)) == 0);
    if (std::memcmp(rb, win, sizeof(win)) != 0) {
        std::printf("  [info] read-back:");
        for (int i = 0; i < 28; ++i) std::printf(" %02x", rb[i]);
        std::printf("\n");
    }

    fillOut(0xCCCCCCCCCCCCCCCCull);
    launch1(fn_read);
    readOut();

    const uint64_t *w64 = h_out;
    const uint32_t *w32 = reinterpret_cast<const uint32_t *>(h_out);
    dumpU32("kernel_read_params out (u32 view)", w32, 10);

    report("out u64[0] (@0x170 preempt-buf slot) == 0xa1a2a3a4a5a6a7a8",
           w64[0] == v1);
    report("out u64[1] (@0x178 guardian slot)   == 0xb1b2b3b4b5b6b7b8",
           w64[1] == v2);
    report("out u64[2] (@0x180 kernel-idx slot) == 0xc1c2c3c4c5c6c7c8",
           w64[2] == v3);
    report("out u32[6] (@0x188 killable slot)   == 0xe1e2e3e4",
           w32[6] == v4);

    /* negative control: the cell right before the window must not carry
     * any sentinel (SetDebuggerParams must not leak outside its 28-byte
     * window) */
    bool neg_ok = w64[4] != v1 && w64[4] != v2 && w64[4] != v3 &&
                  (uint32_t)w64[4] != v4 && (uint32_t)(w64[4] >> 32) != v4;
    std::printf("  [info] out u64[4] (@0x168 pre-window neg. control) = 0x%016llx\n",
                (unsigned long long)w64[4]);
    report("out u64[4] (@0x168 pre-window) contains NO sentinel", neg_ok);
}

static void phase2()
{
    std::printf("[P2] entry-point redirect\n");

    CUdeviceptr ep_a = cuXtraGetEntryPoint(fn_a);
    CUdeviceptr ep_b = cuXtraGetEntryPoint(fn_b);
    std::printf("  [info] entry(kernel_marker_a)=0x%llx entry(kernel_marker_b)=0x%llx\n",
                (unsigned long long)ep_a, (unsigned long long)ep_b);
    report("entry points distinct and non-zero", ep_a && ep_b && ep_a != ep_b);

    fillOut(0xCCCCCCCCCCCCCCCCull);
    cuXtraSetEntryPoint(fn_b, ep_a);
    launch1(fn_b);
    cuXtraSetEntryPoint(fn_b, ep_b);
    readOut();
    report("kernel_marker_b re-routed to kernel_marker_a body (out[0]=0xA...)",
           h_out[0] == 0xAAAAAAAAAAAAAAAAull);

    fillOut(0xCCCCCCCCCCCCCCCCull);
    launch1(fn_b);
    readOut();
    report("entry point restored, kernel_marker_b body runs again (out[0]=0xB...)",
           h_out[0] == 0xBBBBBBBBBBBBBBBBull);
}

static void phase3()
{
    std::printf("[P3] instruction memory (EXIT stub) & redirect\n");

    uint64_t exit_words[2] = { 0x000000000000794Dull, 0x000fea0003800000ull };

    CUdeviceptr blk = cuXtraInstrMemBlockAlloc(ctx, 256);
    report("cuXtraInstrMemBlockAlloc returns a device pointer", blk != 0);
    if (blk == 0) return;

    size_t n = cuXtraInstrMemcpyHtoD(blk, exit_words, sizeof(exit_words), stream);
    report("cuXtraInstrMemcpyHtoD uploaded the 16-byte EXIT stub", n == 16);
    cuXtraInvalInstrCache(ctx);

    CUdeviceptr ep_a = cuXtraGetEntryPoint(fn_a);
    fillOut(0xCCCCCCCCCCCCCCCCull);
    cuXtraSetEntryPoint(fn_a, blk);
    launch1(fn_a);
    cuXtraSetEntryPoint(fn_a, ep_a);
    readOut();
    report("kernel_marker_a ran the EXIT stub (out[0] untouched)",
           h_out[0] == 0xCCCCCCCCCCCCCCCCull);

    fillOut(0xCCCCCCCCCCCCCCCCull);
    launch1(fn_a);
    readOut();
    report("entry point restored, kernel_marker_a body runs again (out[0]=0xA...)",
           h_out[0] == 0xAAAAAAAAAAAAAAAAull);

    cuXtraInstrMemBlockFree(ctx, blk);
}

static void phase4()
{
    std::printf("[P4] binary & resource APIs\n");

    const void *bin = nullptr;
    size_t bsz = 0;
    cuXtraGetBinary(ctx, fn_read, &bin, &bsz, false);
    report("cuXtraGetBinary returns non-empty host image", bin != nullptr && bsz > 0);
    if (bin && bsz > 0) {
        const unsigned char *p = static_cast<const unsigned char *>(bin);
        std::printf("  [info] kernel_read_params image: %llu bytes, first 16:",
                    (unsigned long long)bsz);
        for (int i = 0; i < 16 && (size_t)i < bsz; ++i) std::printf(" %02x", p[i]);
        std::printf("\n");
    }

    size_t regs0 = cuXtraGetLocalRegsPerThread(fn_read);
    std::printf("  [info] LocalRegsPerThread before: %llu\n",
                (unsigned long long)regs0);
    cuXtraSetLocalRegsPerThread(fn_read, 32);
    size_t regs1 = cuXtraGetLocalRegsPerThread(fn_read);
    report("SetLocalRegsPerThread(32) read-back == 32", regs1 == 32);

    size_t bar0 = cuXtraGetBarrierCnt(fn_read);
    std::printf("  [info] BarrierCnt before: %llu\n", (unsigned long long)bar0);
    cuXtraSetBarrierCnt(fn_read, 1);
    size_t bar1 = cuXtraGetBarrierCnt(fn_read);
    report("SetBarrierCnt(1) read-back == 1", bar1 == 1);

    size_t pc = cuXtraGetParamCount(fn_read);
    std::printf("  [info] ParamCount: %llu\n", (unsigned long long)pc);
    report("ParamCount == 1", pc == 1);

    size_t off = 0, sz = 0;
    bool in_shm = false;
    cuXtraGetParamInfo(fn_read, 0, &off, &sz, &in_shm);
    std::printf("  [info] ParamInfo(0): offset=0x%llx size=%llu in_shm=%d"
                "  (shadow of c[0x0][0x380] under test)\n",
                (unsigned long long)off, (unsigned long long)sz, (int)in_shm);
    report("ParamInfo(0) size == 8", sz == 8);
}

static void phase5()
{
    std::printf("[P5] entry-point stability\n");
    CUdeviceptr ep_a1 = cuXtraGetEntryPoint(fn_a);
    CUdeviceptr ep_a2 = cuXtraGetEntryPoint(fn_a);
    report("repeated GetEntryPoint returns the same address", ep_a1 == ep_a2);
}

/* [P6] dump the driver trap handler from the device and report every
 * constant-bank offset it consumes (sm120 debugger-window recon).
 * The machine code is saved as trap_handler_dump_sm120.bin and scanned
 * for LDC / LDC.64 word0 (low 16 bits == 0x7b82); the offset field is
 * bits [40,64) holding (off >> 2). */
static void phase6()
{
    std::printf("[P6] driver trap-handler dump (sm120 cbank reconnaissance)\n");

    CUdeviceptr handler = 0;
    size_t hsize = 0;
    cuXtraGetTrapHandlerInfo(ctx, &handler, &hsize);
    std::printf("  [info] trap handler: addr=0x%llx size=%llu\n",
                (unsigned long long)handler, (unsigned long long)hsize);
    report("GetTrapHandlerInfo returned a handler", handler != 0 && hsize > 0);
    if (handler == 0 || hsize == 0) return;

    if (hsize > (size_t)(1u << 20)) hsize = (size_t)(1u << 20); /* cap */
    std::vector<unsigned char> buf(hsize, 0);
    size_t got = cuXtraInstrMemcpyDtoH(buf.data(), handler, hsize, stream);
    std::printf("  [info] InstrMemcpyDtoH: %llu of %llu bytes\n",
                (unsigned long long)got, (unsigned long long)hsize);
    report("InstrMemcpyDtoH dumped the trap handler", got == hsize);
    if (got != hsize) return;

    FILE *f = std::fopen("trap_handler_dump_sm120.bin", "wb");
    if (f) {
        std::fwrite(buf.data(), 1, got, f);
        std::fclose(f);
        std::printf("  [info] saved trap_handler_dump_sm120.bin\n");
    }

    /* scan 16-byte SASS slots for LDC / LDC.64 */
    int total = 0, high = 0;
    for (size_t i = 0; i + 16 <= got; i += 16) {
        uint64_t w0 = 0;
        std::memcpy(&w0, buf.data() + i, 8);
        if ((w0 & 0xffffull) != 0x7b82ull) continue;
        uint32_t off = (uint32_t)(((w0 >> 40) & 0xffffffull) << 2);
        ++total;
        if (off >= 0x200) {
            ++high;
            std::printf("  [info] LDC slot +0x%04llx -> c[0x0][0x%x]"
                        "  w0=0x%016llx\n",
                        (unsigned long long)i, off, (unsigned long long)w0);
        }
    }
    std::printf("  [info] LDC slots found: %d (of which >= 0x200: %d)\n",
                total, high);
    report("trap handler contains at least one LDC", total > 0);
}

/* [--scan] exhaustive constant-bank scan: set the four sentinel debugger
 * params, launch the patched kernel once and dump `g_scan_count`
 * consecutive u64 samples at c[0x0][base + i*step].  Sentinel matches
 * (the SetDebuggerParams values landing at a probed offset), the `out`
 * pointer self-check (@0x380) and unwritten slots are flagged inline;
 * a RESULT-SCAN summary line is emitted for the run_scan.ps1 driver. */
static void scanMode()
{
    std::printf("[SCAN] base=0x%llx step=0x%llx count=%d (span 0x%llx bytes)\n",
                (unsigned long long)g_scan_base,
                (unsigned long long)g_scan_step, g_scan_count,
                (unsigned long long)(g_scan_step * (uint64_t)g_scan_count));

    const uint64_t v1 = 0x1111222233334444ull;  /* preempt buffer addr */
    const uint64_t v2 = 0x5555666677778888ull;  /* guardian entry      */
    const uint64_t v3 = 0x9999AAAABBBBCCCCull;  /* kernel idx          */
    const uint32_t v4 = 0xDDDDEEEEu;            /* killable            */

    uint8_t win[28];
    std::memcpy(win + 0,  &v1, 8);
    std::memcpy(win + 8,  &v2, 8);
    std::memcpy(win + 16, &v3, 8);
    std::memcpy(win + 24, &v4, 4);
    cuXtraSetDebuggerParams(fn_read, win, sizeof(win));

    fillOut(0xCCCCCCCCCCCCCCCCull);
    launch1(fn_read);
    readOut();

    int nonzero = 0, matches = 0, unwritten = 0;
    for (int i = 0; i < g_scan_count; ++i) {
        uint64_t off = g_scan_base + (uint64_t)i * g_scan_step;
        uint64_t v = h_out[i];
        const char *tag = "";
        if (v == 0xCCCCCCCCCCCCCCCCull) {
            tag = "  <== NOT WRITTEN (sentinel intact)";
            ++unwritten;
        } else if (v == v1) {
            tag = "  <== MATCH preempt-buf sentinel"; ++matches;
        } else if (v == v2) {
            tag = "  <== MATCH guardian sentinel"; ++matches;
        } else if (v == v3) {
            tag = "  <== MATCH kernel-idx sentinel"; ++matches;
        } else if ((v & 0xffffffffull) == v4 || (v >> 32) == v4) {
            tag = "  <== MATCH killable sentinel"; ++matches;
        } else if (v == d_out) {
            tag = "  <== == out pointer (@0x380 self-check)";
        }
        if (v != 0 && v != 0xCCCCCCCCCCCCCCCCull) ++nonzero;
        std::printf("[SCAN] 0x%04llx: %016llx%s\n",
                    (unsigned long long)off, (unsigned long long)v, tag);
    }
    std::printf("[SCAN] RESULT-SCAN: points=%d nonzero=%d matches=%d"
                " unwritten=%d\n", g_scan_count, nonzero, matches, unwritten);
}

/* ------------------------------ main ------------------------------ */

int main(int argc, char **argv)
{
    const char *cubin_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--p6") == 0) {
            g_run_p6 = true;
        } else if (std::strcmp(argv[i], "--scan") == 0 && i + 3 < argc) {
            g_scan_base  = std::strtoull(argv[++i], nullptr, 0);
            g_scan_step  = std::strtoull(argv[++i], nullptr, 0);
            g_scan_count = (int)std::strtoul(argv[++i], nullptr, 0);
        } else if (argv[i][0] != '-') {
            cubin_path = argv[i];
        } else {
            std::printf("FATAL: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }
    if (cubin_path == nullptr) cubin_path = "probe_kernels_patched.cubin";

    std::printf("==================================================\n");
    std::printf("  T1 cuxtra probe (sm120 L2 study)\n");
    std::printf("  cubin: %s\n", cubin_path);
    if (g_scan_count > 0) {
        std::printf("  mode : SCAN base=0x%llx step=0x%llx count=%d\n",
                    (unsigned long long)g_scan_base,
                    (unsigned long long)g_scan_step, g_scan_count);
    }
    std::printf("==================================================\n");

    g_cuda = LoadLibraryA("C:\\Windows\\System32\\nvcuda.dll");
    if (g_cuda == nullptr) {
        std::printf("FATAL: cannot load C:\\Windows\\System32\\nvcuda.dll\n");
        return 2;
    }
    std::printf("  [info] nvcuda.dll loaded from System32\n");
    loadDriverFns();

    phase0(cubin_path);
    if (ctx && d_out && fn_read) {
        if (g_scan_count > 0) {
            scanMode();
        } else {
            phase1();
            phase2();
            phase3();
            phase4();
            phase5();
            if (g_run_p6) {
                phase6();
            } else {
                std::printf("[P6] skipped (known abort on this driver;"
                            " pass --p6 to attempt anyway)\n");
            }
        }
    } else {
        std::printf("  [ERR ] environment incomplete, later phases skipped\n");
        ++g_fail;
    }

    std::printf("==================================================\n");
    std::printf("  RESULT: %d failed check(s)\n", g_fail);
    std::printf("==================================================\n");
    return g_fail;
}
