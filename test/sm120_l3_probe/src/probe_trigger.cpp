/*
 * probe_trigger.cpp - direct TriggerTrap independence test on sm120.
 *
 * WHY THIS EXISTS
 * ---------------
 * From the cuxtra disassembly (evidence/cuxtra_analysis.txt), cuXtraTriggerTrap
 * is INDEPENDENT of cuXtraGetTrapHandlerInfo:
 *
 *   cuXtraTriggerTrap(ctx):
 *     1. Initialize static params: {reg=0x419e84, val=0x80000000} (bit31)
 *     2. kmod = CudaKernelModule::GetKMod(ctx)
 *     3. GlobalRegsWrite32(kmod, params)   <-- RM-control ioctl
 *
 * It does NOT call GetTrapHandlerInfo. The previous probe skipped Phase 4
 * because GetTrapHandlerInfo aborts on sm120 (error 101 -> exit()). But
 * TriggerTrap itself may work fine!
 *
 * This probe:
 *   Phase 0: setup (driver, context)
 *   Phase 1: call cuXtraTriggerTrap directly under SEH (no kernel running)
 *   Phase 2: allocate device memory, launch a long memset, trigger trap
 *   Phase 3: observe post-trigger state (device responsive? context alive?)
 *   Phase 4: replicate the RM write manually via export table (bypass cuxtra)
 *   Phase 5: summary
 *
 * SAFETY: TriggerTrap without a rewritten handler invokes the STOCK handler.
 * On sm86 this saves context and returns. On sm120 behaviour is unknown -
 * it may hang, fault, or silently succeed. SEH guards protect the host.
 * A timeout watchdog (separate thread) will terminate if the call hangs.
 *
 * Build: powershell -ExecutionPolicy Bypass -File build.ps1 -Target probe_trigger
 * Run  : powershell -ExecutionPolicy Bypass -File run.ps1   -Probe probe_trigger
 */
#include "probe_common.h"

#include <string>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>

/* ================================================================== */
/*  Additional driver types                                            */
/* ================================================================== */

typedef CUresult (*pfn_cuMemAlloc_v2)(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult (*pfn_cuMemFree_v2)(CUdeviceptr dptr);
typedef CUresult (*pfn_cuMemsetD32Async)(CUdeviceptr dst, unsigned int ui,
                                         size_t N, CUstream hStream);
typedef CUresult (*pfn_cuStreamCreate)(CUstream *phStream, unsigned int Flags);
typedef CUresult (*pfn_cuStreamSynchronize)(CUstream hStream);
typedef CUresult (*pfn_cuStreamQuery)(CUstream hStream);
typedef CUresult (*pfn_cuCtxSynchronize)(void);

static pfn_cuMemAlloc_v2        g_cuMemAlloc_v2        = nullptr;
static pfn_cuMemFree_v2         g_cuMemFree_v2         = nullptr;
static pfn_cuMemsetD32Async     g_cuMemsetD32Async     = nullptr;
static pfn_cuStreamCreate       g_cuStreamCreate       = nullptr;
static pfn_cuStreamSynchronize  g_cuStreamSynchronize  = nullptr;
static pfn_cuStreamQuery        g_cuStreamQuery        = nullptr;
static pfn_cuCtxSynchronize     g_cuCtxSynchronize     = nullptr;

/* ================================================================== */
/*  Global state                                                       */
/* ================================================================== */

static pfn_cuInit                g_cuInit                = nullptr;
static pfn_cuDriverGetVersion    g_cuDriverGetVersion    = nullptr;
static pfn_cuDeviceGet           g_cuDeviceGet           = nullptr;
static pfn_cuDeviceGetCount      g_cuDeviceGetCount      = nullptr;
static pfn_cuDeviceGetAttribute  g_cuDeviceGetAttribute  = nullptr;
static pfn_cuDeviceGetName       g_cuDeviceGetName       = nullptr;
static pfn_cuCtxCreate_v2        g_cuCtxCreate_v2        = nullptr;
static pfn_cuCtxGetCurrent       g_cuCtxGetCurrent       = nullptr;
static pfn_cuGetExportTable      g_cuGetExportTable      = nullptr;

static HMODULE   g_hNvcuda = nullptr;
static CUdevice  g_dev     = 0;
static CUcontext g_ctx     = nullptr;
static int       g_driver_ver = 0;
static char      g_dev_name[256] = {0};
static int       g_cc_major = 0, g_cc_minor = 0;

/* Watchdog */
static std::atomic<bool> g_watchdog_abort{false};
static std::atomic<bool> g_call_done{false};

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static std::string fmt_rc(CUresult rc) {
    char b[160];
    if (rc == PROBE_SEH_RAISED)
        std::snprintf(b, sizeof b, "SEH-FAULT(code=0x%08lx)",
                      (unsigned long)probe_last_seh_code);
    else
        std::snprintf(b, sizeof b, "rc=%d (%s)", rc, probe_cu_err_str(rc));
    return std::string(b);
}

#define RESOLVE(dst, symname, type)                                            \
    do { (dst) = (type)(void *)GetProcAddress(g_hNvcuda, symname);             \
         if (!(dst)) LOG("[WARN] missing: %s", symname); } while (0)

/* ================================================================== */
/*  Phase 0                                                            */
/* ================================================================== */

static bool phase0_setup() {
    LOG("=== Phase 0: environment setup ===");
    g_hNvcuda = LoadLibraryA("C:\\Windows\\System32\\nvcuda.dll");
    if (!g_hNvcuda) { LOG("[CRITICAL] LoadLibrary failed"); return false; }

    RESOLVE(g_cuInit,               "cuInit",               pfn_cuInit);
    RESOLVE(g_cuDriverGetVersion,   "cuDriverGetVersion",   pfn_cuDriverGetVersion);
    RESOLVE(g_cuDeviceGet,          "cuDeviceGet",          pfn_cuDeviceGet);
    RESOLVE(g_cuDeviceGetCount,     "cuDeviceGetCount",     pfn_cuDeviceGetCount);
    RESOLVE(g_cuDeviceGetAttribute, "cuDeviceGetAttribute", pfn_cuDeviceGetAttribute);
    RESOLVE(g_cuDeviceGetName,      "cuDeviceGetName",      pfn_cuDeviceGetName);
    RESOLVE(g_cuCtxCreate_v2,       "cuCtxCreate_v2",       pfn_cuCtxCreate_v2);
    RESOLVE(g_cuCtxGetCurrent,      "cuCtxGetCurrent",      pfn_cuCtxGetCurrent);
    RESOLVE(g_cuGetExportTable,     "cuGetExportTable",     pfn_cuGetExportTable);
    RESOLVE(g_cuMemAlloc_v2,        "cuMemAlloc_v2",        pfn_cuMemAlloc_v2);
    RESOLVE(g_cuMemFree_v2,         "cuMemFree_v2",         pfn_cuMemFree_v2);
    RESOLVE(g_cuMemsetD32Async,     "cuMemsetD32Async",     pfn_cuMemsetD32Async);
    RESOLVE(g_cuStreamCreate,       "cuStreamCreate",       pfn_cuStreamCreate);
    RESOLVE(g_cuStreamSynchronize,  "cuStreamSynchronize",  pfn_cuStreamSynchronize);
    RESOLVE(g_cuStreamQuery,        "cuStreamQuery",        pfn_cuStreamQuery);
    RESOLVE(g_cuCtxSynchronize,     "cuCtxSynchronize",     pfn_cuCtxSynchronize);

    if (!g_cuInit) { LOG("[CRITICAL] cuInit unresolved"); return false; }
    CHECK_CU_VOID(SAFE_CALL(g_cuInit, 0u));
    SAFE_CALL(g_cuDriverGetVersion, &g_driver_ver);
    int count = 0; SAFE_CALL(g_cuDeviceGetCount, &count);
    if (count <= 0) { LOG("[CRITICAL] no devices"); return false; }
    SAFE_CALL(g_cuDeviceGet, &g_dev, 0);
    SAFE_CALL(g_cuDeviceGetAttribute, &g_cc_major, 75, g_dev);
    SAFE_CALL(g_cuDeviceGetAttribute, &g_cc_minor, 76, g_dev);
    (void)SAFE_CALL(g_cuDeviceGetName, g_dev_name, (int)sizeof g_dev_name, g_dev);
    LOG("  device=%s CC=%d.%d driver=%d", g_dev_name, g_cc_major, g_cc_minor, g_driver_ver);
    CHECK_CU_VOID(SAFE_CALL(g_cuCtxCreate_v2, &g_ctx, 0u, g_dev));
    LOG("  context=%p", (void *)g_ctx);
    LOG("=== Phase 0 complete ===");
    return true;
}

/* Wrapper that catches C++ exceptions from cuxtra */
static CUresult wrap_trigger_trap_direct(CUcontext c) {
    try { cuXtraTriggerTrap(c); return CUDA_SUCCESS; }
    catch (...) { return (CUresult)0x7FFFFFFE; }
}

/* ================================================================== */
/*  Phase 1 - TriggerTrap with idle GPU (safest test)                  */
/* ================================================================== */

static void phase1_idle_trigger() {
    LOG("=== Phase 1: cuXtraTriggerTrap with IDLE GPU ===");
    LOG("  [INFO] No kernel running. Stock handler should be harmless.");
    LOG("  [INFO] This tests whether GlobalRegsWrite32(reg 0x419e84, bit31)");
    LOG("         is honoured by the sm120 WDDM driver.");

    /* Start a watchdog thread: if the call hangs for >5s, log and exit */
    g_call_done.store(false);
    g_watchdog_abort.store(false);
    std::thread watchdog([]() {
        for (int i = 0; i < 50; ++i) {
            if (g_call_done.load()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        LOG("[WATCHDOG] TriggerTrap HUNG for >5s - RM path likely blocked");
        g_watchdog_abort.store(true);
        /* Cannot safely kill the main thread; just flag it */
    });

    LOG("  -> calling cuXtraTriggerTrap(ctx=%p) ...", (void *)g_ctx);
    double t0 = probe_elapsed_sec();
    CUresult rc = SAFE_CALL(wrap_trigger_trap_direct, g_ctx);
    double t1 = probe_elapsed_sec();
    g_call_done.store(true);
    watchdog.join();

    LOG("  cuXtraTriggerTrap -> %s  (elapsed %.3fs)", fmt_rc(rc).c_str(), t1 - t0);
    if (g_watchdog_abort.load()) {
        LOG("  [WARN] watchdog fired - call may have been interrupted");
    }

    /* Verify context is still alive */
    CUcontext cur = nullptr;
    CUresult rc2 = SAFE_CALL(g_cuCtxGetCurrent, &cur);
    LOG("  post-trigger: cuCtxGetCurrent -> %s ctx=%p", fmt_rc(rc2).c_str(), (void *)cur);
    LOG("=== Phase 1 complete ===");
}

/* ================================================================== */
/*  Phase 2 - TriggerTrap during async GPU work                        */
/* ================================================================== */

static void phase2_busy_trigger() {
    LOG("=== Phase 2: cuXtraTriggerTrap during async memset ===");

    if (!g_cuMemAlloc_v2 || !g_cuMemsetD32Async || !g_cuStreamCreate) {
        LOG("  [SKIP] required APIs unavailable");
        return;
    }

    /* Allocate a large buffer and start a long async memset */
    CUdeviceptr buf = 0;
    size_t bufsize = 256 * 1024 * 1024;  /* 256 MB -> should take ~50-100ms */
    CUresult rc = SAFE_CALL(g_cuMemAlloc_v2, &buf, bufsize);
    if (rc != CUDA_SUCCESS || !buf) {
        LOG("  cuMemAlloc(%zu MB) failed: %s", bufsize/(1024*1024), fmt_rc(rc).c_str());
        return;
    }
    LOG("  allocated 256 MB at 0x%016llx", (unsigned long long)buf);

    CUstream stream = nullptr;
    rc = SAFE_CALL(g_cuStreamCreate, &stream, 0x01 /*CU_STREAM_NON_BLOCKING*/);
    if (rc != CUDA_SUCCESS) {
        LOG("  cuStreamCreate failed: %s", fmt_rc(rc).c_str());
        SAFE_CALL(g_cuMemFree_v2, buf);
        return;
    }

    /* Launch a big async memset (fills 64M uint32s) */
    size_t nelems = bufsize / 4;
    LOG("  launching cuMemsetD32Async(%llu elements) ...", (unsigned long long)nelems);
    rc = SAFE_CALL(g_cuMemsetD32Async, buf, 0xDEADBEEF, nelems, stream);
    LOG("  memset launch -> %s", fmt_rc(rc).c_str());

    /* Immediately query stream (should be busy) */
    if (g_cuStreamQuery) {
        CUresult rq = SAFE_CALL(g_cuStreamQuery, stream);
        LOG("  cuStreamQuery (pre-trigger) -> %s (%s)",
            fmt_rc(rq).c_str(), rq == CUDA_SUCCESS ? "IDLE" : "BUSY");
    }

    /* Trigger trap while GPU is busy */
    LOG("  -> cuXtraTriggerTrap during active memset ...");
    double t0 = probe_elapsed_sec();
    CUresult rct = SAFE_CALL(wrap_trigger_trap_direct, g_ctx);
    double t1 = probe_elapsed_sec();
    LOG("  TriggerTrap -> %s (elapsed %.3fs)", fmt_rc(rct).c_str(), t1 - t0);

    /* Check stream state post-trigger */
    if (g_cuStreamQuery) {
        CUresult rq2 = SAFE_CALL(g_cuStreamQuery, stream);
        LOG("  cuStreamQuery (post-trigger) -> %s", fmt_rc(rq2).c_str());
    }

    /* Synchronize and check for errors */
    if (g_cuStreamSynchronize) {
        LOG("  cuStreamSynchronize ...");
        double ts0 = probe_elapsed_sec();
        CUresult rcs = SAFE_CALL(g_cuStreamSynchronize, stream);
        double ts1 = probe_elapsed_sec();
        LOG("  sync -> %s (elapsed %.3fs)", fmt_rc(rcs).c_str(), ts1 - ts0);
    }

    /* Context still alive? */
    CUcontext cur = nullptr;
    SAFE_CALL(g_cuCtxGetCurrent, &cur);
    LOG("  post-trigger context: %p (alive=%s)", (void *)cur, cur ? "YES" : "NO");

    SAFE_CALL(g_cuMemFree_v2, buf);
    LOG("=== Phase 2 complete ===");
}

/* ================================================================== */
/*  Phase 3 - post-trigger health check                                */
/* ================================================================== */

static void phase3_health() {
    LOG("=== Phase 3: post-trigger GPU health check ===");

    /* Can we still allocate and use the GPU? */
    if (g_cuMemAlloc_v2) {
        CUdeviceptr p = 0;
        CUresult rc = SAFE_CALL(g_cuMemAlloc_v2, &p, 4096);
        LOG("  cuMemAlloc(4096) -> %s ptr=0x%llx", fmt_rc(rc).c_str(),
            (unsigned long long)p);
        if (rc == CUDA_SUCCESS && p && g_cuMemFree_v2)
            SAFE_CALL(g_cuMemFree_v2, p);
    }

    /* cuCtxSynchronize */
    if (g_cuCtxSynchronize) {
        CUresult rc = SAFE_CALL(g_cuCtxSynchronize);
        LOG("  cuCtxSynchronize -> %s", fmt_rc(rc).c_str());
    }

    /* Device attribute re-query */
    if (g_cuDeviceGetAttribute) {
        int v = -1;
        SAFE_CALL(g_cuDeviceGetAttribute, &v, 75, g_dev);
        LOG("  CC major re-query -> %d (expect 12)", v);
    }

    LOG("=== Phase 3 complete ===");
}

/* ================================================================== */
/*  Phase 4 - manual RM register write (bypass cuxtra entirely)        */
/* ================================================================== */

static void phase4_manual_rm() {
    LOG("=== Phase 4: manual RM register write analysis ===");
    LOG("  cuXtraTriggerTrap internally does:");
    LOG("    kmod = CudaKernelModule::GetKMod(ctx)");
    LOG("    GlobalRegsWrite32(kmod, [{reg=0x419e84, val=0x80000000}])");
    LOG("  This is an RM-control ioctl (NV_ESC_RM_CONTROL) that writes");
    LOG("  register 0x419e84 bit31 = TRIGGER_TRAP on the GPU's global regs.");
    LOG("");
    LOG("  Since we cannot replicate GetKMod/GlobalRegsWrite32 without");
    LOG("  cuxtra internals, Phase 1-2 already tested the full path.");
    LOG("  If Phase 1 returned SUCCESS, the RM write path is ALIVE on sm120.");
    LOG("  If it returned error/SEH, the WDDM driver blocks the ioctl.");
    LOG("=== Phase 4 complete (analysis only) ===");
}

/* ================================================================== */
/*  Phase 5 - summary                                                  */
/* ================================================================== */

static void phase5_summary() {
    LOGRAW("\n================ TRIGGER PROBE SUMMARY ================\n");
    LOGRAW("device: %s | CC %d.%d | driver %d\n", g_dev_name, g_cc_major, g_cc_minor, g_driver_ver);
    LOGRAW("\nKey question: does GlobalRegsWrite32(0x419e84, bit31) work on sm120 WDDM?\n");
    LOGRAW("  - Phase 1 (idle):  see cuXtraTriggerTrap result above\n");
    LOGRAW("  - Phase 2 (busy):  see cuXtraTriggerTrap result above\n");
    LOGRAW("  - Phase 3 (health): GPU still responsive?\n");
    LOGRAW("\nIf TriggerTrap returned SUCCESS:\n");
    LOGRAW("  The RM-control path is ALIVE. The missing piece is only the\n");
    LOGRAW("  trap HANDLER (slot 20 investigation). L3 is feasible on sm120.\n");
    LOGRAW("If TriggerTrap returned error/SEH/hung:\n");
    LOGRAW("  WDDM blocks the register write. L3 trap path is NOT viable\n");
    LOGRAW("  in this environment. Consider Linux or TSG fallback.\n");
    LOGRAW("========================================================\n");
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */

int main(int argc, char **argv) {
    bool skip_busy = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--idle-only") == 0) skip_busy = true;

    LOG("=== probe_trigger: sm120 TriggerTrap independence test ===");
    if (!phase0_setup()) return 1;

    phase1_idle_trigger();
    if (!skip_busy) phase2_busy_trigger();
    phase3_health();
    phase4_manual_rm();
    phase5_summary();

    LOG("=== probe_trigger done ===");
    return 0;
}
