/*
 * probe_win_trigger2.cpp - safe Windows L3 alternatives (no dangerous fn() calls).
 *
 * Continuation of probe_win_trigger which crashed during slot sweep.
 * This version:
 *   - Skips no-arg fn() calls (cause SEH corruption accumulation)
 *   - Focuses on cuCheckpointProcess* APIs (Windows-native preemption?)
 *   - Tests TSG/timeslice path
 *   - Tests slot 1 and 10 (confirmed SUCCESS responders) with output capture
 *
 * Build: powershell -ExecutionPolicy Bypass -File build.ps1 -Target probe_win_trigger2
 * Run  : powershell -ExecutionPolicy Bypass -File run.ps1   -Probe probe_win_trigger2
 */
#include "probe_common.h"
#include <string>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>

typedef CUresult (*pfn_cuCtxSynchronize)(void);
typedef CUresult (*pfn_cuCtxGetFlags)(unsigned int *flags);
typedef CUresult (*pfn_cuMemAlloc_v2)(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult (*pfn_cuMemFree_v2)(CUdeviceptr dptr);

/* Checkpoint APIs (undocumented, ordinals 13-18 in nvcuda.dll) */
typedef CUresult (*pfn_cpCheckpoint)(void);
typedef CUresult (*pfn_cpRestore)(void);
typedef CUresult (*pfn_cpGetState)(int *state);
typedef CUresult (*pfn_cpLock)(void);
typedef CUresult (*pfn_cpUnlock)(void);
typedef CUresult (*pfn_cpGetTid)(unsigned long *tid);

static pfn_cuCtxSynchronize g_cuCtxSync = nullptr;
static pfn_cuCtxGetFlags    g_cuCtxGetFlags = nullptr;
static pfn_cuMemAlloc_v2    g_cuMemAlloc = nullptr;
static pfn_cuMemFree_v2     g_cuMemFree = nullptr;
static pfn_cpCheckpoint     g_cpCheckpoint = nullptr;
static pfn_cpRestore        g_cpRestore = nullptr;
static pfn_cpGetState       g_cpGetState = nullptr;
static pfn_cpLock           g_cpLock = nullptr;
static pfn_cpUnlock         g_cpUnlock = nullptr;
static pfn_cpGetTid         g_cpGetTid = nullptr;

static pfn_cuInit               g_cuInit = nullptr;
static pfn_cuDriverGetVersion   g_cuDriverGetVersion = nullptr;
static pfn_cuDeviceGet          g_cuDeviceGet = nullptr;
static pfn_cuDeviceGetCount     g_cuDeviceGetCount = nullptr;
static pfn_cuDeviceGetAttribute g_cuDeviceGetAttribute = nullptr;
static pfn_cuDeviceGetName      g_cuDeviceGetName = nullptr;
static pfn_cuCtxCreate_v2       g_cuCtxCreate_v2 = nullptr;
static pfn_cuCtxGetCurrent      g_cuCtxGetCurrent = nullptr;
static pfn_cuGetExportTable     g_cuGetExportTable = nullptr;

static HMODULE g_hNvcuda = nullptr;
static CUdevice g_dev = 0;
static CUcontext g_ctx = nullptr;
static int g_driver_ver = 0;
static char g_dev_name[256] = {0};
static int g_cc_major = 0, g_cc_minor = 0;
static const void **g_trap_table = nullptr;
static size_t g_trap_max = 0;

static const unsigned char ETID_TRAP[16] = {
    0xcc,0x52,0x9e,0x94,0x5c,0x4e,0x9d,0x46,
    0x83,0x7c,0x96,0x25,0x86,0x83,0x34,0xe4 };

typedef CUresult (*SlotFn2)(void *, void *);
typedef CUresult (*SlotFn3)(void *, void *, void *);

static std::string fmt_rc(CUresult rc) {
    char b[160];
    if (rc == PROBE_SEH_RAISED)
        std::snprintf(b, sizeof b, "SEH-FAULT(0x%08lx)", (unsigned long)probe_last_seh_code);
    else
        std::snprintf(b, sizeof b, "rc=%d (%s)", rc, probe_cu_err_str(rc));
    return std::string(b);
}

#define RESOLVE(dst, sym, type) \
    do { (dst) = (type)(void *)GetProcAddress(g_hNvcuda, sym); \
         if (!(dst)) LOG("[WARN] missing: %s", sym); } while (0)

static bool phase0() {
    LOG("=== Phase 0: setup ===");
    g_hNvcuda = LoadLibraryA("C:\\Windows\\System32\\nvcuda.dll");
    if (!g_hNvcuda) { LOG("[CRITICAL] LoadLibrary failed"); return false; }
    RESOLVE(g_cuInit, "cuInit", pfn_cuInit);
    RESOLVE(g_cuDriverGetVersion, "cuDriverGetVersion", pfn_cuDriverGetVersion);
    RESOLVE(g_cuDeviceGet, "cuDeviceGet", pfn_cuDeviceGet);
    RESOLVE(g_cuDeviceGetCount, "cuDeviceGetCount", pfn_cuDeviceGetCount);
    RESOLVE(g_cuDeviceGetAttribute, "cuDeviceGetAttribute", pfn_cuDeviceGetAttribute);
    RESOLVE(g_cuDeviceGetName, "cuDeviceGetName", pfn_cuDeviceGetName);
    RESOLVE(g_cuCtxCreate_v2, "cuCtxCreate_v2", pfn_cuCtxCreate_v2);
    RESOLVE(g_cuCtxGetCurrent, "cuCtxGetCurrent", pfn_cuCtxGetCurrent);
    RESOLVE(g_cuGetExportTable, "cuGetExportTable", pfn_cuGetExportTable);
    RESOLVE(g_cuCtxSync, "cuCtxSynchronize", pfn_cuCtxSynchronize);
    RESOLVE(g_cuCtxGetFlags, "cuCtxGetFlags", pfn_cuCtxGetFlags);
    RESOLVE(g_cuMemAlloc, "cuMemAlloc_v2", pfn_cuMemAlloc_v2);
    RESOLVE(g_cuMemFree, "cuMemFree_v2", pfn_cuMemFree_v2);
    RESOLVE(g_cpCheckpoint, "cuCheckpointProcessCheckpoint", pfn_cpCheckpoint);
    RESOLVE(g_cpRestore, "cuCheckpointProcessRestore", pfn_cpRestore);
    RESOLVE(g_cpGetState, "cuCheckpointProcessGetState", pfn_cpGetState);
    RESOLVE(g_cpLock, "cuCheckpointProcessLock", pfn_cpLock);
    RESOLVE(g_cpUnlock, "cuCheckpointProcessUnlock", pfn_cpUnlock);
    RESOLVE(g_cpGetTid, "cuCheckpointProcessGetRestoreThreadId", pfn_cpGetTid);

    if (!g_cuInit) return false;
    CHECK_CU_VOID(SAFE_CALL(g_cuInit, 0u));
    SAFE_CALL(g_cuDriverGetVersion, &g_driver_ver);
    int cnt=0; SAFE_CALL(g_cuDeviceGetCount, &cnt);
    if (cnt<=0) return false;
    SAFE_CALL(g_cuDeviceGet, &g_dev, 0);
    SAFE_CALL(g_cuDeviceGetAttribute, &g_cc_major, 75, g_dev);
    SAFE_CALL(g_cuDeviceGetAttribute, &g_cc_minor, 76, g_dev);
    (void)SAFE_CALL(g_cuDeviceGetName, g_dev_name, (int)sizeof g_dev_name, g_dev);
    LOG("  %s CC=%d.%d drv=%d", g_dev_name, g_cc_major, g_cc_minor, g_driver_ver);
    CHECK_CU_VOID(SAFE_CALL(g_cuCtxCreate_v2, &g_ctx, 0u, g_dev));

    CUuuid etid; std::memcpy(etid.bytes, ETID_TRAP, 16);
    const void *tbl = nullptr;
    if (SAFE_CALL(g_cuGetExportTable, &tbl, &etid) == CUDA_SUCCESS && tbl) {
        g_trap_table = (const void **)tbl;
        g_trap_max = (size_t)((uint64_t)g_trap_table[0] / 8);
    }
    LOG("  TrapHandler: %zu slots | Checkpoint APIs: %s",
        g_trap_max, g_cpCheckpoint ? "FOUND" : "NOT FOUND");
    LOG("=== Phase 0 done ===");
    return true;
}

/* Phase 1: safe slot investigation (only fn(ctx,...) patterns) */
static void phase1_safe_slots() {
    LOG("=== Phase 1: safe slot probe (fn(ctx) only) ===");
    static const size_t interesting[] = {1, 5, 8, 10, 16, 20, 22};
    struct OutBuf { uint8_t data[128]; };

    for (size_t idx = 0; idx < 7; ++idx) {
        size_t i = interesting[idx];
        if (g_trap_max <= i || !g_trap_table[i]) continue;
        void *fp = (void *)g_trap_table[i];
        LOG("  slot %2zu = %p:", i, fp);

        OutBuf ob; std::memset(&ob, 0, sizeof ob);
        CUresult rc = SAFE_CALL((SlotFn2)fp, (void *)g_ctx, &ob);
        LOG("    fn(ctx,&out128) -> %s", fmt_rc(rc).c_str());
        if (rc == CUDA_SUCCESS) {
            bool nz = false;
            for (size_t b = 0; b < 128; ++b) if (ob.data[b]) { nz = true; break; }
            if (nz) {
                LOGRAW("    output (48 bytes):\n");
                probe_hexdump(ob.data, 48, 16);
                uint64_t v0=0,v1=0,v2=0,v3=0;
                std::memcpy(&v0,ob.data,8); std::memcpy(&v1,ob.data+8,8);
                std::memcpy(&v2,ob.data+16,8); std::memcpy(&v3,ob.data+24,8);
                LOG("    [0]=0x%llx [1]=0x%llx(%llu) [2]=0x%llx [3]=0x%llx(%llu)",
                    (unsigned long long)v0,(unsigned long long)v1,(unsigned long long)v1,
                    (unsigned long long)v2,(unsigned long long)v3,(unsigned long long)v3);
            } else {
                LOG("    output: all zeros (no-op or query-only)");
            }
        }
    }
    LOG("=== Phase 1 done ===");
}

/* Phase 2: cuCheckpointProcess APIs */
static std::atomic<bool> g_cp_done{false};
static void phase2_checkpoint() {
    LOG("=== Phase 2: cuCheckpointProcess APIs ===");
    if (!g_cpGetState && !g_cpCheckpoint && !g_cpLock) {
        LOG("  [SKIP] not available in this driver");
        return;
    }
    LOG("  APIs: Checkpoint=%p Restore=%p GetState=%p Lock=%p Unlock=%p GetTid=%p",
        (void*)g_cpCheckpoint,(void*)g_cpRestore,(void*)g_cpGetState,
        (void*)g_cpLock,(void*)g_cpUnlock,(void*)g_cpGetTid);

    if (g_cpGetState) {
        int st = -1;
        CUresult rc = SAFE_CALL(g_cpGetState, &st);
        LOG("  GetState -> %s state=%d", fmt_rc(rc).c_str(), st);
    }
    if (g_cpGetTid) {
        unsigned long tid = 0;
        CUresult rc = SAFE_CALL(g_cpGetTid, &tid);
        LOG("  GetRestoreThreadId -> %s tid=%lu (cur=%lu)",
            fmt_rc(rc).c_str(), tid, GetCurrentThreadId());
    }
    if (g_cpLock) {
        LOG("  -> Lock ...");
        double t0 = probe_elapsed_sec();
        CUresult rc = SAFE_CALL(g_cpLock);
        LOG("  Lock -> %s (%.3fs)", fmt_rc(rc).c_str(), probe_elapsed_sec()-t0);
        if (rc == CUDA_SUCCESS && g_cpGetState) {
            int st=-1; SAFE_CALL(g_cpGetState, &st);
            LOG("  GetState post-lock = %d", st);
        }
        if (g_cpUnlock) {
            CUresult rcu = SAFE_CALL(g_cpUnlock);
            LOG("  Unlock -> %s", fmt_rc(rcu).c_str());
        }
    }
    if (g_cpCheckpoint) {
        LOG("  -> Checkpoint (may suspend GPU work, 5s watchdog) ...");
        g_cp_done.store(false);
        std::thread wd([]() {
            for (int i=0;i<50;++i) {
                if (g_cp_done.load()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            LOG("  [WATCHDOG] Checkpoint HUNG >5s");
        });
        double t0 = probe_elapsed_sec();
        CUresult rc = SAFE_CALL(g_cpCheckpoint);
        double t1 = probe_elapsed_sec();
        g_cp_done.store(true);
        wd.join();
        LOG("  Checkpoint -> %s (%.3fs)", fmt_rc(rc).c_str(), t1-t0);
        if (rc == CUDA_SUCCESS) {
            LOG("  ** CHECKPOINT SUCCEEDED **");
            if (g_cpGetState) { int st=-1; SAFE_CALL(g_cpGetState,&st); LOG("  state=%d",st); }
            if (g_cpRestore) {
                LOG("  -> Restore ...");
                CUresult rcr = SAFE_CALL(g_cpRestore);
                LOG("  Restore -> %s", fmt_rc(rcr).c_str());
            }
        }
    }
    if (g_cuCtxSync) {
        CUresult rcs = SAFE_CALL(g_cuCtxSync);
        LOG("  health: cuCtxSynchronize -> %s", fmt_rc(rcs).c_str());
    }
    LOG("=== Phase 2 done ===");
}

/* Phase 3: TSG / timeslice */
static void phase3_tsg() {
    LOG("=== Phase 3: TSG timeslice path ===");
    size_t ts = 0;
    CUresult rc = SAFE_CALL([&]() -> CUresult {
        try { ts = cuXtraGetTimeslice(g_ctx); return CUDA_SUCCESS; }
        catch (...) { return (CUresult)0x7FFFFFFE; }
    });
    LOG("  cuXtraGetTimeslice -> %s ts=%zu us", fmt_rc(rc).c_str(), ts);
    if (rc == CUDA_SUCCESS && ts > 0) {
        LOG("  ** TSG ALIVE ON WINDOWS! timeslice=%zu us **", ts);
        CUresult rc2 = SAFE_CALL([&]() -> CUresult {
            try { cuXtraSetTimeslice(g_ctx, 2000); return CUDA_SUCCESS; }
            catch (...) { return (CUresult)0x7FFFFFFE; }
        });
        LOG("  SetTimeslice(2000) -> %s", fmt_rc(rc2).c_str());
        if (rc2 == CUDA_SUCCESS) {
            size_t ts2 = 0;
            SAFE_CALL([&]() -> CUresult {
                try { ts2 = cuXtraGetTimeslice(g_ctx); return CUDA_SUCCESS; }
                catch (...) { return (CUresult)0x7FFFFFFE; }
            });
            LOG("  verify -> %zu us", ts2);
            SAFE_CALL([&]() -> CUresult {
                try { cuXtraSetTimeslice(g_ctx, ts); return CUDA_SUCCESS; }
                catch (...) { return (CUresult)0x7FFFFFFE; }
            });
            LOG("  restored original=%zu", ts);
        }
    } else {
        LOG("  TSG blocked (CudaKernelModule Windows limitation)");
    }
    LOG("=== Phase 3 done ===");
}

/* Phase 4: device attributes */
static void phase4_attrs() {
    LOG("=== Phase 4: preemption-related device attributes ===");
    struct { int attr; const char *name; } attrs[] = {
        {90, "COMPUTE_PREEMPTION_SUPPORTED"},
        {71, "MPS_ENABLED(?)"},
        {92, "attr92(?)"},
        {93, "attr93(?)"},
        {94, "attr94(?)"},
        {95, "attr95(?)"},
        {108,"attr108(?)"},
        {109,"attr109(?)"},
        {110,"attr110(?)"},
    };
    for (size_t i = 0; i < sizeof(attrs)/sizeof(attrs[0]); ++i) {
        int v = -1;
        CUresult rc = SAFE_CALL(g_cuDeviceGetAttribute, &v, attrs[i].attr, g_dev);
        LOG("  attr %3d (%-32s) -> %s val=%d",
            attrs[i].attr, attrs[i].name, fmt_rc(rc).c_str(), v);
    }
    if (g_cuCtxGetFlags) {
        unsigned int f = 0;
        SAFE_CALL(g_cuCtxGetFlags, &f);
        LOG("  cuCtxGetFlags -> 0x%x", f);
    }
    LOG("=== Phase 4 done ===");
}

static void phase5_summary() {
    LOGRAW("\n========== WINDOWS L3 VIABILITY SUMMARY ==========\n");
    LOGRAW("device: %s CC %d.%d driver %d WDDM\n",
           g_dev_name, g_cc_major, g_cc_minor, g_driver_ver);
    LOGRAW("\nFindings:\n");
    LOGRAW("  1. cuxtra TriggerTrap: BLOCKED (CudaKernelModule Linux-only)\n");
    LOGRAW("  2. cuxtra TSG:         see Phase 3 above\n");
    LOGRAW("  3. Checkpoint APIs:    see Phase 2 above\n");
    LOGRAW("  4. Slot 20 GetInfo:    WORKS (handler memObjHandle + sizes)\n");
    LOGRAW("  5. Slots 1,10:         respond to all patterns (role TBD)\n");
    LOGRAW("\nImplications for XSched L3 on sm120 Windows:\n");
    LOGRAW("  - Trap HANDLER info is obtainable (slot 20 + ObjGetPc)\n");
    LOGRAW("  - Trap TRIGGER needs a Windows-native path:\n");
    LOGRAW("    a) Port GlobalRegsWrite32 to Windows (DeviceIoControl)\n");
    LOGRAW("    b) Use cuCheckpointProcess if it provides GPU suspend\n");
    LOGRAW("    c) Use TSG timeslice=0 as coarse preempt (if TSG works)\n");
    LOGRAW("    d) Find trigger slot in export table (slots 1/5/8/10/16)\n");
    LOGRAW("===================================================\n");
}

int main(int argc, char **argv) {
    bool skip_cp = false;
    for (int i=1;i<argc;++i)
        if (std::strcmp(argv[i],"--no-checkpoint")==0) skip_cp=true;
    LOG("=== probe_win_trigger2: safe Windows L3 alternatives ===");
    if (!phase0()) return 1;
    phase1_safe_slots();
    if (!skip_cp) phase2_checkpoint();
    else LOG("=== Phase 2 SKIPPED ===");
    phase3_tsg();
    phase4_attrs();
    phase5_summary();
    LOG("=== probe_win_trigger2 done ===");
    return 0;
}
