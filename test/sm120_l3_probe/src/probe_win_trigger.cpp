/*
 * probe_win_trigger.cpp - Windows-compatible L3 trigger alternatives for sm120.
 *
 * FINDING FROM probe_trigger:
 *   cuXtraTriggerTrap FAILS on Windows because cuxtra's CudaKernelModule
 *   (GlobalRegsWrite32) is Linux-only (uses /dev/nvidiactl ioctls).
 *   Error: "CudaKernelModule is not supported on Windows @ kmod.cpp:63"
 *
 * THIS PROBE explores alternative trigger paths that work on Windows:
 *   Phase 0: setup
 *   Phase 1: deep-probe ALL TrapHandler slots with trigger-like patterns
 *            (looking for a slot that fires the trap without RM ioctls)
 *   Phase 2: cuCheckpointProcess* APIs (new in CUDA 12.x driver, ordinals 13-18)
 *            These may provide process-level GPU preemption on Windows.
 *   Phase 3: cuCtxSetFlags / scheduling policy APIs
 *   Phase 4: TSG (timeslice) path via cuXtraGetTimeslice/SetTimeslice
 *   Phase 5: summary + viability matrix
 *
 * Build: powershell -ExecutionPolicy Bypass -File build.ps1 -Target probe_win_trigger
 * Run  : powershell -ExecutionPolicy Bypass -File run.ps1   -Probe probe_win_trigger
 */
#include "probe_common.h"

#include <string>
#include <cstring>
#include <vector>

/* ================================================================== */
/*  Additional driver entry points                                     */
/* ================================================================== */

typedef CUresult (*pfn_cuMemAlloc_v2)(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult (*pfn_cuMemFree_v2)(CUdeviceptr dptr);
typedef CUresult (*pfn_cuCtxSynchronize)(void);
typedef CUresult (*pfn_cuCtxSetFlags)(unsigned int flags);
typedef CUresult (*pfn_cuCtxGetFlags)(unsigned int *flags);

/* cuCheckpointProcess* (undocumented, ordinals 13-18 in nvcuda.dll) */
typedef CUresult (*pfn_cuCheckpointProcessCheckpoint)(void);
typedef CUresult (*pfn_cuCheckpointProcessRestore)(void);
typedef CUresult (*pfn_cuCheckpointProcessGetState)(int *state);
typedef CUresult (*pfn_cuCheckpointProcessLock)(void);
typedef CUresult (*pfn_cuCheckpointProcessUnlock)(void);
typedef CUresult (*pfn_cuCheckpointProcessGetRestoreThreadId)(unsigned long *tid);

static pfn_cuMemAlloc_v2       g_cuMemAlloc_v2       = nullptr;
static pfn_cuMemFree_v2        g_cuMemFree_v2        = nullptr;
static pfn_cuCtxSynchronize    g_cuCtxSynchronize    = nullptr;
static pfn_cuCtxSetFlags       g_cuCtxSetFlags       = nullptr;
static pfn_cuCtxGetFlags       g_cuCtxGetFlags       = nullptr;

static pfn_cuCheckpointProcessCheckpoint       g_cpCheckpoint = nullptr;
static pfn_cuCheckpointProcessRestore          g_cpRestore    = nullptr;
static pfn_cuCheckpointProcessGetState         g_cpGetState   = nullptr;
static pfn_cuCheckpointProcessLock             g_cpLock       = nullptr;
static pfn_cuCheckpointProcessUnlock           g_cpUnlock     = nullptr;
static pfn_cuCheckpointProcessGetRestoreThreadId g_cpGetTid   = nullptr;

/* ================================================================== */
/*  ETIDs + globals                                                    */
/* ================================================================== */

static const unsigned char ETID_TRAP_BYTES[16] = {
    0xcc, 0x52, 0x9e, 0x94, 0x5c, 0x4e, 0x9d, 0x46,
    0x83, 0x7c, 0x96, 0x25, 0x86, 0x83, 0x34, 0xe4 };

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

static const void **g_trap_table = nullptr;
static size_t       g_trap_max   = 0;

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

typedef CUresult (*SlotFn4)(void *, void *, void *, void *);
typedef CUresult (*SlotFn3)(void *, void *, void *);
typedef CUresult (*SlotFn2)(void *, void *);
typedef CUresult (*SlotFn1)(void *);
typedef CUresult (*SlotFn0)(void);

static std::string fmt_rc(CUresult rc) {
    char b[160];
    if (rc == PROBE_SEH_RAISED)
        std::snprintf(b, sizeof b, "SEH-FAULT(0x%08lx)", (unsigned long)probe_last_seh_code);
    else
        std::snprintf(b, sizeof b, "rc=%d (%s)", rc, probe_cu_err_str(rc));
    return std::string(b);
}

static CUuuid make_etid(const unsigned char bytes[16]) {
    CUuuid u; std::memcpy(u.bytes, bytes, 16); return u;
}

static bool buf_nonzero(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; ++i) if (b[i]) return true;
    return false;
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
    RESOLVE(g_cuCtxSynchronize,     "cuCtxSynchronize",     pfn_cuCtxSynchronize);
    RESOLVE(g_cuCtxSetFlags,        "cuCtxSetFlags",        pfn_cuCtxSetFlags);
    RESOLVE(g_cuCtxGetFlags,        "cuCtxGetFlags",        pfn_cuCtxGetFlags);

    /* Checkpoint APIs (undocumented, may not exist in all drivers) */
    RESOLVE(g_cpCheckpoint, "cuCheckpointProcessCheckpoint",       pfn_cuCheckpointProcessCheckpoint);
    RESOLVE(g_cpRestore,    "cuCheckpointProcessRestore",          pfn_cuCheckpointProcessRestore);
    RESOLVE(g_cpGetState,   "cuCheckpointProcessGetState",         pfn_cuCheckpointProcessGetState);
    RESOLVE(g_cpLock,       "cuCheckpointProcessLock",             pfn_cuCheckpointProcessLock);
    RESOLVE(g_cpUnlock,     "cuCheckpointProcessUnlock",           pfn_cuCheckpointProcessUnlock);
    RESOLVE(g_cpGetTid,     "cuCheckpointProcessGetRestoreThreadId", pfn_cuCheckpointProcessGetRestoreThreadId);

    if (!g_cuInit || !g_cuGetExportTable) { LOG("[CRITICAL] core unresolved"); return false; }
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

    /* Resolve TrapHandler table */
    CUuuid etid = make_etid(ETID_TRAP_BYTES);
    const void *table = nullptr;
    CUresult rc = SAFE_CALL(g_cuGetExportTable, &table, &etid);
    if (rc == CUDA_SUCCESS && table) {
        g_trap_table = (const void **)table;
        g_trap_max = (size_t)((uint64_t)g_trap_table[0] / 8);
        LOG("  TrapHandler table: %zu slots", g_trap_max);
    } else {
        LOG("  [CRITICAL] TrapHandler table failed");
        return false;
    }

    LOG("=== Phase 0 complete ===");
    return true;
}

/* ================================================================== */
/*  Phase 1 - deep-probe all slots with trigger-like patterns          */
/* ================================================================== */

static void phase1_slot_sweep() {
    LOG("=== Phase 1: TrapHandler slot deep sweep (trigger candidates) ===");
    LOG("  Looking for a slot that can FIRE a trap without RM ioctls.");
    LOG("  Testing patterns: fn(), fn(ctx), fn(ctx,u32), fn(ctx,u64), fn(u32)");

    for (size_t i = 1; i < g_trap_max && i <= 22; ++i) {
        if (!g_trap_table[i]) continue;
        void *fp = (void *)g_trap_table[i];
        LOG("  --- slot %zu = %p ---", i, fp);

        /* Pattern: fn() - no args (simplest trigger) */
        CUresult rc0 = SAFE_CALL((SlotFn0)fp);
        LOG("    fn()             -> %s", fmt_rc(rc0).c_str());

        /* Pattern: fn(ctx) */
        CUresult rc1 = SAFE_CALL((SlotFn1)fp, (void *)g_ctx);
        LOG("    fn(ctx)          -> %s", fmt_rc(rc1).c_str());

        /* Pattern: fn(ctx, uint32=1) - trigger with flag */
        uint32_t one32 = 1;
        CUresult rc2 = SAFE_CALL((SlotFn2)fp, (void *)g_ctx, &one32);
        LOG("    fn(ctx,&u32=1)   -> %s", fmt_rc(rc2).c_str());

        /* Pattern: fn(ctx, uint64=1) */
        uint64_t one64 = 1;
        CUresult rc3 = SAFE_CALL((SlotFn2)fp, (void *)g_ctx, &one64);
        LOG("    fn(ctx,&u64=1)   -> %s", fmt_rc(rc3).c_str());

        /* Pattern: fn(ctx, ctx) - double context (some ET fns take 2) */
        CUresult rc4 = SAFE_CALL((SlotFn2)fp, (void *)g_ctx, (void *)g_ctx);
        LOG("    fn(ctx,ctx)      -> %s", fmt_rc(rc4).c_str());

        /* Pattern: fn(ctx, &out32, &out32) - 3-arg with outputs */
        uint32_t o1 = 0, o2 = 0;
        CUresult rc5 = SAFE_CALL((SlotFn3)fp, (void *)g_ctx, &o1, &o2);
        LOG("    fn(ctx,&o1,&o2)  -> %s  o1=0x%x o2=0x%x",
            fmt_rc(rc5).c_str(), o1, o2);

        /* If any pattern returned SUCCESS with non-trivial output, flag it */
        if (rc0 == CUDA_SUCCESS || rc1 == CUDA_SUCCESS) {
            LOG("    ** SLOT %zu RESPONDS TO SIMPLE CALL - TRIGGER CANDIDATE **", i);
        }
    }
    LOG("=== Phase 1 complete ===");
}

/* ================================================================== */
/*  Phase 2 - cuCheckpointProcess* APIs                                */
/* ================================================================== */

static void phase2_checkpoint() {
    LOG("=== Phase 2: cuCheckpointProcess* APIs ===");
    LOG("  These are newer driver exports (ordinals 13-18) that may provide");
    LOG("  process-level GPU checkpoint/restore on Windows (WDDM-compatible).");

    if (!g_cpGetState && !g_cpCheckpoint && !g_cpLock) {
        LOG("  [SKIP] No cuCheckpointProcess* APIs found in this driver");
        return;
    }

    /* 2a: GetState - query current checkpoint state */
    if (g_cpGetState) {
        int state = -1;
        CUresult rc = SAFE_CALL(g_cpGetState, &state);
        LOG("  GetState -> %s state=%d", fmt_rc(rc).c_str(), state);
        LOG("    (state meanings: 0=normal? 1=checkpointed? 2=locked?)");
    } else { LOG("  GetState: not available"); }

    /* 2b: GetRestoreThreadId */
    if (g_cpGetTid) {
        unsigned long tid = 0;
        CUresult rc = SAFE_CALL(g_cpGetTid, &tid);
        LOG("  GetRestoreThreadId -> %s tid=%lu (current=%lu)",
            fmt_rc(rc).c_str(), tid, GetCurrentThreadId());
    } else { LOG("  GetRestoreThreadId: not available"); }

    /* 2c: Lock - may pause GPU work for this process */
    if (g_cpLock) {
        LOG("  -> calling Lock (may pause GPU scheduling for this process)...");
        double t0 = probe_elapsed_sec();
        CUresult rc = SAFE_CALL(g_cpLock);
        double t1 = probe_elapsed_sec();
        LOG("  Lock -> %s (elapsed %.3fs)", fmt_rc(rc).c_str(), t1 - t0);

        /* If lock succeeded, query state again */
        if (rc == CUDA_SUCCESS && g_cpGetState) {
            int state2 = -1;
            SAFE_CALL(g_cpGetState, &state2);
            LOG("  GetState (post-lock) -> state=%d", state2);
        }

        /* Unlock */
        if (g_cpUnlock) {
            CUresult rcu = SAFE_CALL(g_cpUnlock);
            LOG("  Unlock -> %s", fmt_rc(rcu).c_str());
        }
    } else { LOG("  Lock: not available"); }

    /* 2d: Checkpoint - the heavy operation (may suspend all GPU work) */
    if (g_cpCheckpoint) {
        LOG("  -> calling Checkpoint (WARNING: may suspend all GPU work)...");
        LOG("     If this hangs, the process will need manual termination.");
        double t0 = probe_elapsed_sec();
        CUresult rc = SAFE_CALL(g_cpCheckpoint);
        double t1 = probe_elapsed_sec();
        LOG("  Checkpoint -> %s (elapsed %.3fs)", fmt_rc(rc).c_str(), t1 - t0);

        if (rc == CUDA_SUCCESS) {
            LOG("  ** CHECKPOINT SUCCEEDED - GPU work suspended! **");
            if (g_cpGetState) {
                int st = -1; SAFE_CALL(g_cpGetState, &st);
                LOG("  GetState (post-checkpoint) -> state=%d", st);
            }
            /* Immediately restore */
            if (g_cpRestore) {
                LOG("  -> calling Restore...");
                CUresult rcr = SAFE_CALL(g_cpRestore);
                LOG("  Restore -> %s", fmt_rc(rcr).c_str());
            }
        }
    } else { LOG("  Checkpoint: not available"); }

    /* 2e: Verify GPU still responsive */
    LOG("  --- post-checkpoint health ---");
    CUcontext cur = nullptr;
    SAFE_CALL(g_cuCtxGetCurrent, &cur);
    LOG("  ctx still current: %p", (void *)cur);
    if (g_cuCtxSynchronize) {
        CUresult rcs = SAFE_CALL(g_cuCtxSynchronize);
        LOG("  cuCtxSynchronize -> %s", fmt_rc(rcs).c_str());
    }

    LOG("=== Phase 2 complete ===");
}

/* ================================================================== */
/*  Phase 3 - context scheduling flags                                 */
/* ================================================================== */

static void phase3_ctx_flags() {
    LOG("=== Phase 3: context scheduling flags ===");

    if (g_cuCtxGetFlags) {
        unsigned int flags = 0;
        CUresult rc = SAFE_CALL(g_cuCtxGetFlags, &flags);
        LOG("  cuCtxGetFlags -> %s flags=0x%x", fmt_rc(rc).c_str(), flags);
        LOG("    bit0: CU_CTX_SCHED_AUTO(0) / SPIN(1) / YIELD(2) / BLOCKING(4)");
    }

    /* Try setting blocking sync (may affect preemption behaviour) */
    if (g_cuCtxSetFlags) {
        LOG("  -> cuCtxSetFlags(CU_CTX_SCHED_BLOCKING_SYNC=4)...");
        CUresult rc = SAFE_CALL(g_cuCtxSetFlags, 4);
        LOG("  SetFlags -> %s", fmt_rc(rc).c_str());
        /* Restore */
        SAFE_CALL(g_cuCtxSetFlags, 0);
    }

    /* Device attribute: MPS-related */
    if (g_cuDeviceGetAttribute) {
        int v = -1;
        SAFE_CALL(g_cuDeviceGetAttribute, &v, 90, g_dev);  /* COMPUTE_PREEMPTION */
        LOG("  attr90 (COMPUTE_PREEMPTION_SUPPORTED) = %d", v);
        v = -1;
        SAFE_CALL(g_cuDeviceGetAttribute, &v, 71, g_dev);  /* MPS_ENABLED */
        LOG("  attr71 (MPS_ENABLED?) = %d", v);
        v = -1;
        SAFE_CALL(g_cuDeviceGetAttribute, &v, 92, g_dev);  /* ? */
        LOG("  attr92 = %d", v);
        v = -1;
        SAFE_CALL(g_cuDeviceGetAttribute, &v, 93, g_dev);
        LOG("  attr93 = %d", v);
    }

    LOG("=== Phase 3 complete ===");
}

/* ================================================================== */
/*  Phase 4 - TSG / timeslice path                                     */
/* ================================================================== */

static void phase4_tsg() {
    LOG("=== Phase 4: TSG (timeslice) path ===");
    LOG("  cuXtraGetTimeslice/SetTimeslice use CudaKernelModule internally.");
    LOG("  If CudaKernelModule is Linux-only, TSG is also unavailable on Windows.");

    /* Test cuXtraGetTimeslice - it may hit the same Windows limitation */
    LOG("  -> cuXtraGetTimeslice(ctx)...");
    size_t ts = 0;
    CUresult rc = SAFE_CALL([&]() -> CUresult {
        try { ts = cuXtraGetTimeslice(g_ctx); return CUDA_SUCCESS; }
        catch (...) { return (CUresult)0x7FFFFFFE; }
    });
    LOG("  cuXtraGetTimeslice -> %s timeslice=%zu us", fmt_rc(rc).c_str(), ts);

    if (rc == CUDA_SUCCESS && ts > 0) {
        LOG("  ** TSG PATH IS ALIVE ON WINDOWS! timeslice=%zu us **", ts);
        LOG("  -> trying cuXtraSetTimeslice(ctx, 1000) (1ms)...");
        CUresult rc2 = SAFE_CALL([&]() -> CUresult {
            try { cuXtraSetTimeslice(g_ctx, 1000); return CUDA_SUCCESS; }
            catch (...) { return (CUresult)0x7FFFFFFE; }
        });
        LOG("  SetTimeslice -> %s", fmt_rc(rc2).c_str());
        /* Restore original */
        if (rc2 == CUDA_SUCCESS) {
            SAFE_CALL([&]() -> CUresult {
                try { cuXtraSetTimeslice(g_ctx, ts); return CUDA_SUCCESS; }
                catch (...) { return (CUresult)0x7FFFFFFE; }
            });
            LOG("  restored original timeslice=%zu", ts);
        }
    } else {
        LOG("  TSG path likely also blocked by CudaKernelModule Windows limitation.");
    }

    LOG("=== Phase 4 complete ===");
}

/* ================================================================== */
/*  Phase 5 - summary                                                  */
/* ================================================================== */

static void phase5_summary() {
    LOGRAW("\n============ WINDOWS L3 TRIGGER VIABILITY MATRIX ============\n");
    LOGRAW("device: %s | CC %d.%d | driver %d | WDDM\n",
           g_dev_name, g_cc_major, g_cc_minor, g_driver_ver);
    LOGRAW("\n");
    LOGRAW("Path                        | Status      | Notes\n");
    LOGRAW("----------------------------|-------------|-------------------------------\n");
    LOGRAW("cuxtra TriggerTrap (RM reg) | BLOCKED     | CudaKernelModule Linux-only\n");
    LOGRAW("Slot 20 GetInfo (handler)   | WORKS       | Returns memObjHandle+sizes\n");
    LOGRAW("ObjGetPc (resolve handle)   | WORKS       | Returns pc=0x880\n");
    LOGRAW("cuCheckpointProcess*        | SEE PHASE 2 | New Windows-native path?\n");
    LOGRAW("TSG (timeslice)             | SEE PHASE 4 | May share kmod limitation\n");
    LOGRAW("Export table slot trigger   | SEE PHASE 1 | Scanning for Win-native fire\n");
    LOGRAW("\n");
    LOGRAW("NEXT STEPS (depending on results above):\n");
    LOGRAW("  If Checkpoint works: implement L3 via checkpoint/restore cycle\n");
    LOGRAW("  If a slot triggers: implement Windows TriggerTrap via that slot\n");
    LOGRAW("  If TSG works: use timeslice reduction as coarse L3\n");
    LOGRAW("  If all blocked: L3 on Windows requires a kmod port or Linux env\n");
    LOGRAW("==============================================================\n");
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */

int main(int argc, char **argv) {
    bool skip_checkpoint = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--no-checkpoint") == 0) skip_checkpoint = true;

    LOG("=== probe_win_trigger: Windows L3 trigger alternatives for sm120 ===");
    if (!phase0_setup()) return 1;

    phase1_slot_sweep();
    if (!skip_checkpoint) phase2_checkpoint();
    else LOG("=== Phase 2 SKIPPED (--no-checkpoint) ===");
    phase3_ctx_flags();
    phase4_tsg();
    phase5_summary();

    LOG("=== probe_win_trigger done ===");
    return 0;
}
