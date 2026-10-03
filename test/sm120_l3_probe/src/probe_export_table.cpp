/*
 * probe_export_table.cpp - comprehensive cuGetExportTable probe for the
 *                          sm120 (Blackwell) Level-3 Trap-Handler path.
 *
 * WHY THIS EXISTS
 * ---------------
 * XSched L3 preemption ("trap path") rewrites the CUDA driver's trap handler
 * so an in-flight kernel can be interrupted and safely exited. cuxtra reaches
 * that handler through the *undocumented* driver hook:
 *
 *      CUresult cuGetExportTable(const void **ppTable, const CUuuid *pETID);
 *
 * Static RE of libcuxtra_windows_amd64.a (see evidence/cuxtra_analysis.txt)
 * recovered the table identifier (ETID) GUIDs and the slot layout cuxtra uses:
 *
 *      CU_ETID_ToolsTrapHandler = cc529e94-5c4e-9d46-837c-9625868334e4
 *          slot 19 -> EtblTrapHandler::GetInfoPascal(ctx, params*, info*)
 *      CU_ETID_ToolsMemory      = bfdb432d-bf3c-5a4a-945e-b34029e81e75
 *          slot 24 -> EtblMemory::ObjGetPc(ctx, handle*, u64*)
 *          slot 25 -> EtblMemory::ObjGetSize(handle*, u64*)
 *
 * Export-table layout convention:
 *      table[0] (read as uint64) = total byte size of the table
 *      table[N] (read as void* ) = function pointer for slot N
 *
 * EMPIRICAL NOTE (from the first run of this probe on RTX 5060 / CC 12.0):
 *   - The TrapHandler ETID *does* resolve (184-byte table, 22 slots), so the
 *     export table is NOT removed from the sm120 driver.
 *   - BUT slot 19 (GetInfoPascal) returns error 101 for every version/index
 *     combo: the Pascal-era getter has no Blackwell case.
 *   - cuxtra's cuXtraGetTrapHandlerInfo treats that 101 as FATAL and aborts
 *     the process. We therefore print the full summary BEFORE running the
 *     cuxtra trap API (Phase 4), and run Phase 4 last so its evidence is
 *     banked incrementally even if the process is torn down.
 *
 * SAFETY
 * ------
 * Every undocumented call is wrapped in SAFE_CALL() (VEH + longjmp guard from
 * probe_common.h) AND, for cuxtra entry points that may unwind via a C++
 * exception, an inner try/catch. A SIGABRT handler logs a last-gasp message
 * if cuxtra calls abort(). One bad pointer / bogus ETID cannot silently kill
 * the run without leaving evidence.
 *
 * EXIT CODE = number of *critical* failures (0 = every probe ran to completion
 * without an unrecoverable problem). A missing/failed TrapHandler export table
 * is counted as critical because it would mean the ET path is gone entirely.
 *
 * Build: powershell -ExecutionPolicy Bypass -File build.ps1 -Target probe_export_table
 * Run  : powershell -ExecutionPolicy Bypass -File run.ps1   -Probe probe_export_table
 */
#include "probe_common.h"

#include <vector>
#include <string>
#include <map>
#include <csignal>

/* ================================================================== */
/*  Known ETIDs recovered from the cuxtra static archive               */
/* ================================================================== */

/* CU_ETID_ToolsTrapHandler (trap.cpp.obj) */
static const unsigned char ETID_TRAP_BYTES[16] = {
    0xcc, 0x52, 0x9e, 0x94, 0x5c, 0x4e, 0x9d, 0x46,
    0x83, 0x7c, 0x96, 0x25, 0x86, 0x83, 0x34, 0xe4 };

/* CU_ETID_ToolsMemory (instrmem.cpp.obj) - comparison baseline: L2 works,
 * so this table is expected to resolve cleanly on sm120. */
static const unsigned char ETID_MEM_BYTES[16] = {
    0xbf, 0xdb, 0x43, 0x2d, 0xbf, 0x3c, 0x5a, 0x4a,
    0x94, 0x5e, 0xb3, 0x40, 0x29, 0xe8, 0x1e, 0x75 };

/* cuxtra slot assignments we care about by name. */
static const size_t SLOT_GetInfoPascal = 19;
static const size_t SLOT_ObjGetPc      = 24;
static const size_t SLOT_ObjGetSize    = 25;

/* Upper bound on how many slots we enumerate / probe. Real tables are ~30-60
 * slots; the cap only guards against a corrupt table[0] size field. */
static const size_t MAX_ENUM_SLOTS = 512;
static const size_t MAX_PROBE_SLOT = 40;   /* per spec: probe non-NULL 1..40 */

/* Sentinel returned by a wrapper when cuxtra unwinds via a C++ exception
 * instead of yielding a normal CUresult. Distinct from PROBE_SEH_RAISED
 * (which signals a Win32/SEH fault caught by the VEH guard). */
#define PROBE_CXX_THREW ((CUresult)0x7FFFFFFE)

/* ================================================================== */
/*  Pascal-style GetInfo parameter / result structures                 */
/* ================================================================== */

/* 16-byte input struct cuxtra hands to GetInfoPascal. */
struct GetInfoParams {
    uint32_t version;    /* 0x10 for the Pascal getter                     */
    uint32_t index;      /* 0x01                                            */
    uint64_t reserved;   /* 0                                               */
};

/* Oversized capture buffer - we do not know the true sm120 result size, so
 * we over-allocate and hexdump whatever the callee writes. GetInfoPascal's
 * documented output places memObjHandle (a device pointer) at offset +0x28
 * from the result-struct base. */
struct GetInfoResult {
    uint8_t data[128];
};

/* Generic slot signatures. Undocumented slots have unknown arity; we try the
 * shapes cuxtra itself uses (ctx, in*, out*) and (a*, b*). SEH guards the
 * mismatch cases. */
typedef CUresult (*SlotFn3)(void *, void *, void *);
typedef CUresult (*SlotFn2)(void *, void *);

/* ================================================================== */
/*  Resolved driver entry points + device state (Phase 0 fills these)  */
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

static HMODULE   g_hNvcuda     = nullptr;
static uintptr_t g_nvcuda_base = 0;
static uintptr_t g_nvcuda_size = 0;

static CUdevice  g_dev         = 0;
static CUcontext g_ctx         = nullptr;
static int       g_driver_ver  = 0;
static int       g_cc_major    = 0;
static int       g_cc_minor    = 0;
static char      g_dev_name[256] = {0};

static int       g_critical    = 0;   /* exit code accumulator */

/* ---- summary accumulators (Phase 6 prints these) -------------------- */
static bool     g_trap_ok       = false;
static uint64_t g_trap_size     = 0;
static size_t   g_trap_maxslots = 0;
static bool     g_mem_ok        = false;
static uint64_t g_mem_size      = 0;
static size_t   g_mem_maxslots  = 0;

static std::vector<std::string> g_slot19_lines;      /* per-combo results   */
static std::map<size_t, std::string> g_slot_result;   /* slot -> one-liner   */
/* Phase 4 runs AFTER the summary (cuxtra trap API may abort on sm120), so its
 * lines start out "deferred" and are only filled if the process survives. */
static std::string g_getinfo_line =
    "DEFERRED (runs post-summary; cuxtra trap API may abort on sm120)";
static std::string g_trigger_line =
    "DEFERRED (runs post-summary; cuxtra trap API may abort on sm120)";
static std::vector<std::string> g_alt_lines;         /* Phase 5 ETID scans  */
static std::string g_preempt_line  = "not run";      /* Phase 7 attr 90     */

/* ================================================================== */
/*  Small helpers                                                      */
/* ================================================================== */

/* Last-gasp logger if cuxtra calls abort() (SIGABRT). The handler runs once;
 * abort() then re-raises and terminates, but the evidence reaches the log. */
static void probe_sigabrt_handler(int) {
    LOG("[FATAL] SIGABRT received - cuxtra aborted the process "
        "(trap API is fatal on sm120: GetInfoPascal error 101)");
    std::fflush(stdout);
}

/* Classify a function pointer relative to the loaded nvcuda.dll image. */
static const char *classify_fn_ptr(uintptr_t p) {
    if (p == 0) return "NULL";
    if (g_nvcuda_base && p >= g_nvcuda_base && p < g_nvcuda_base + g_nvcuda_size)
        return "-> nvcuda.dll";
    return "-> ELSEWHERE (not nvcuda)";
}

/* True if any byte in the buffer is non-zero (i.e. the callee wrote output). */
static bool buf_nonzero(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; ++i) if (b[i]) return true;
    return false;
}

/* Format a SAFE_CALL outcome. Must be called *immediately* after the guard so
 * probe_last_seh_code still describes that specific call. */
static std::string fmt_rc(CUresult rc) {
    char b[160];
    if (rc == PROBE_SEH_RAISED)
        std::snprintf(b, sizeof b, "SEH-FAULT(code=0x%08lx)",
                      (unsigned long)probe_last_seh_code);
    else if (rc == PROBE_CXX_THREW)
        std::snprintf(b, sizeof b, "C++-EXCEPTION-THROWN(caught)");
    else
        std::snprintf(b, sizeof b, "rc=%d (%s)", rc, probe_cu_err_str(rc));
    return std::string(b);
}

static CUuuid make_etid(const unsigned char bytes[16]) {
    CUuuid u;
    std::memcpy(u.bytes, bytes, 16);
    return u;
}

/* ================================================================== */
/*  Phase 0 - environment setup                                        */
/* ================================================================== */

#define RESOLVE(dst, symname, type)                                            \
    do {                                                                       \
        (dst) = (type)(void *)GetProcAddress(g_hNvcuda, symname);              \
        LOG("  GetProcAddress(%-22s) = %p", symname, (void *)(dst));           \
        if (!(dst)) { LOG("[CRITICAL] nvcuda.dll missing export: %s", symname);\
                      ++g_critical; }                                          \
    } while (0)

static bool phase0_setup() {
    LOG("=== Phase 0: environment setup ===");

    /* (1) Load the driver explicitly so the module under test is unambiguous
     *     (matches CUXTRA_CUDA_LIB set by run.ps1). */
    const char *lib = "C:\\Windows\\System32\\nvcuda.dll";
    g_hNvcuda = LoadLibraryA(lib);
    if (!g_hNvcuda) {
        LOG("[CRITICAL] LoadLibraryA(%s) failed, gle=%lu", lib, GetLastError());
        ++g_critical;
        return false;
    }
    LOG("  LoadLibraryA(%s) = %p", lib, (void *)g_hNvcuda);

    /* Derive the image base/size from the PE headers so slot pointers can be
     * classified as inside/outside nvcuda.dll. */
    g_nvcuda_base = (uintptr_t)g_hNvcuda;
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_nvcuda_base;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            IMAGE_NT_HEADERS *nt =
                (IMAGE_NT_HEADERS *)(g_nvcuda_base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                g_nvcuda_size = nt->OptionalHeader.SizeOfImage;
        }
    }
    LOG("  nvcuda.dll image base = 0x%016llx size = 0x%llx (%llu bytes)",
        (unsigned long long)g_nvcuda_base,
        (unsigned long long)g_nvcuda_size,
        (unsigned long long)g_nvcuda_size);

    /* (2) Resolve the entry points we need. */
    RESOLVE(g_cuInit,               "cuInit",                pfn_cuInit);
    RESOLVE(g_cuDriverGetVersion,   "cuDriverGetVersion",    pfn_cuDriverGetVersion);
    RESOLVE(g_cuDeviceGet,          "cuDeviceGet",           pfn_cuDeviceGet);
    RESOLVE(g_cuDeviceGetCount,     "cuDeviceGetCount",      pfn_cuDeviceGetCount);
    RESOLVE(g_cuDeviceGetAttribute, "cuDeviceGetAttribute",  pfn_cuDeviceGetAttribute);
    RESOLVE(g_cuDeviceGetName,      "cuDeviceGetName",       pfn_cuDeviceGetName);
    RESOLVE(g_cuCtxCreate_v2,       "cuCtxCreate_v2",        pfn_cuCtxCreate_v2);
    RESOLVE(g_cuCtxGetCurrent,      "cuCtxGetCurrent",       pfn_cuCtxGetCurrent);
    RESOLVE(g_cuGetExportTable,     "cuGetExportTable",      pfn_cuGetExportTable);

    if (!g_cuInit || !g_cuGetExportTable) {
        LOG("[CRITICAL] core entry points unresolved - cannot continue");
        return false;
    }

    /* (3) Init + device enumeration. */
    CHECK_CU_VOID(SAFE_CALL(g_cuInit, 0u));
    CHECK_CU_VOID(SAFE_CALL(g_cuDriverGetVersion, &g_driver_ver));
    LOG("  driver version = %d", g_driver_ver);

    int count = 0;
    CHECK_CU_VOID(SAFE_CALL(g_cuDeviceGetCount, &count));
    LOG("  device count   = %d", count);
    if (count <= 0) { LOG("[CRITICAL] no CUDA devices"); ++g_critical; return false; }

    CHECK_CU_VOID(SAFE_CALL(g_cuDeviceGet, &g_dev, 0));

    /* (4) Compute capability - confirm sm120 (CC 12.0). */
    CHECK_CU_VOID(SAFE_CALL(g_cuDeviceGetAttribute, &g_cc_major,
                            CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, g_dev));
    CHECK_CU_VOID(SAFE_CALL(g_cuDeviceGetAttribute, &g_cc_minor,
                            CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, g_dev));
    (void)SAFE_CALL(g_cuDeviceGetName, g_dev_name, (int)sizeof g_dev_name, g_dev);
    LOG("  device name    = %s", g_dev_name);
    LOG("  compute cap    = %d.%d (sm%d%d)",
        g_cc_major, g_cc_minor, g_cc_major, g_cc_minor);
    if (g_cc_major != 12 || g_cc_minor != 0)
        LOG("[WARN] expected CC 12.0 (sm120); running on %d.%d - results may "
            "not reflect Blackwell behaviour", g_cc_major, g_cc_minor);

    /* (5) Create + confirm a current context (export tables are ctx-scoped). */
    CHECK_CU_VOID(SAFE_CALL(g_cuCtxCreate_v2, &g_ctx, 0u, g_dev));
    CUcontext cur = nullptr;
    CHECK_CU_VOID(SAFE_CALL(g_cuCtxGetCurrent, &cur));
    LOG("  context created = %p, current = %p", (void *)g_ctx, (void *)cur);
    if (!g_ctx) { LOG("[CRITICAL] no context"); ++g_critical; return false; }

    LOG("=== Phase 0 complete: driver=%d, %s, CC %d.%d ===",
        g_driver_ver, g_dev_name, g_cc_major, g_cc_minor);
    return true;
}

/* ================================================================== */
/*  Table discovery + enumeration (shared by Phase 1 and Phase 3)      */
/* ================================================================== */

/* Call cuGetExportTable under SEH; on success read table[0] (byte size) and
 * derive the slot count. Returns true when a usable table came back.
 * NOTE: cuGetExportTable's out-param is a `const void **` (address of a single
 * pointer that receives the table base). We therefore hold a `const void *`
 * and pass its address, then reinterpret the base as an array of slots. */
static bool discover_table(const CUuuid *etid, const char *label,
                           const void ***out_table, uint64_t *out_size,
                           size_t *out_max) {
    char guid[37];
    probe_uuid_to_str(etid, guid);
    LOG("--- cuGetExportTable(%s) ETID=%s ---", label, guid);
    LOGRAW("    ETID bytes:\n");
    probe_hexdump(etid->bytes, 16, 16);

    const void *table = nullptr;
    CUresult rc = SAFE_CALL(g_cuGetExportTable, &table, etid);
    LOG("    -> %s | table=%p | seh=0x%08lx",
        fmt_rc(rc).c_str(), (void *)table, (unsigned long)probe_last_seh_code);

    *out_table = nullptr; *out_size = 0; *out_max = 0;
    if (rc != CUDA_SUCCESS || table == nullptr) return false;

    const void **tbl = (const void **)table;   /* base == array of slots */
    uint64_t size = (uint64_t)tbl[0];          /* table[0] == byte size  */
    size_t   max  = (size_t)(size / 8);        /* each slot is a pointer */
    LOG("    table[0] (byte size) = %llu -> max_slots = %zu (size/8)",
        (unsigned long long)size, max);
    if (max == 0 || max > MAX_ENUM_SLOTS) {
        LOG("    [WARN] slot count %zu out of sane range (1..%zu); clamping",
            max, MAX_ENUM_SLOTS);
        if (max > MAX_ENUM_SLOTS) max = MAX_ENUM_SLOTS;
    }
    *out_table = tbl; *out_size = size; *out_max = max;
    return true;
}

/* Enumerate slots 1..max-1: print index + pointer + classification. Collects
 * the non-NULL indices into *nonnull when provided. */
static void enumerate_table(const void **table, size_t max, const char *label,
                            std::vector<size_t> *nonnull) {
    LOG("    [%s] enumerating slots 1..%zu:", label, max - 1);
    size_t nonnull_n = 0;
    for (size_t i = 1; i < max; ++i) {
        void *fp = (void *)table[i];
        LOG("      slot %3zu = %p  %s", i, fp, classify_fn_ptr((uintptr_t)fp));
        if (fp) { ++nonnull_n; if (nonnull) nonnull->push_back(i); }
    }
    LOG("    [%s] %zu non-NULL slot(s) of %zu total", label, nonnull_n, max - 1);
}

/* ================================================================== */
/*  Phase 2 - slot probing on the TrapHandler table                    */
/* ================================================================== */

/* 2a: hit slot 19 (GetInfoPascal) with several version/index combos. */
static void phase2_slot19(const void **table, size_t max) {
    LOG("=== Phase 2a: slot 19 (GetInfoPascal) parameter sweep ===");
    if (max <= SLOT_GetInfoPascal || table[SLOT_GetInfoPascal] == nullptr) {
        LOG("  slot 19 absent or NULL - cannot probe GetInfoPascal");
        g_slot19_lines.push_back("slot 19 absent/NULL");
        return;
    }
    void *fp = (void *)table[SLOT_GetInfoPascal];
    LOG("  slot 19 fn = %p (%s)", fp, classify_fn_ptr((uintptr_t)fp));

    struct Combo { uint32_t version; uint32_t index; const char *note; };
    static const Combo combos[] = {
        { 0x10, 0x1, "Pascal style (v0x10,idx1)" },
        { 0x20, 0x1, "possible newer (v0x20,idx1)" },
        { 0x30, 0x1, "possible newer (v0x30,idx1)" },
        { 0x10, 0x0, "index 0 (v0x10,idx0)" },
        { 0x10, 0x2, "index 2 (v0x10,idx2)" },
    };

    for (size_t c = 0; c < sizeof(combos) / sizeof(combos[0]); ++c) {
        GetInfoParams p; p.version = combos[c].version;
        p.index = combos[c].index; p.reserved = 0;
        GetInfoResult r; std::memset(&r, 0, sizeof r);

        CUresult rc = SAFE_CALL((SlotFn3)fp, (void *)g_ctx, &p, &r);
        std::string res = fmt_rc(rc);
        LOG("  combo[%zu] %-30s -> %s", c, combos[c].note, res.c_str());

        if (rc == CUDA_SUCCESS && buf_nonzero(&r, sizeof r)) {
            LOGRAW("    non-zero result buffer (hexdump):\n");
            probe_hexdump(&r, sizeof r, 16);
            uint64_t handle = 0;
            std::memcpy(&handle, r.data + 0x28, sizeof handle);
            LOG("    offset +0x28 (memObjHandle?) = 0x%016llx  [%s]",
                (unsigned long long)handle,
                probe_addr_class_str(probe_addr_classify(handle)));
            char hb[64];
            std::snprintf(hb, sizeof hb, " +output(handle=0x%016llx)",
                          (unsigned long long)handle);
            res += hb;
        }
        char line[256];
        std::snprintf(line, sizeof line, "combo[%zu] %-28s -> %s",
                      c, combos[c].note, res.c_str());
        g_slot19_lines.push_back(line);
    }
}

/* 2b: generic arity sweep across every non-NULL slot 1..40. */
static void phase2_generic(const void **table, size_t max) {
    LOG("=== Phase 2b: generic slot sweep (non-NULL slots 1..%zu) ===",
        MAX_PROBE_SLOT);
    size_t hi = max - 1;
    if (hi > MAX_PROBE_SLOT) hi = MAX_PROBE_SLOT;

    for (size_t i = 1; i <= hi; ++i) {
        void *fp = (void *)table[i];
        if (!fp) continue;

        GetInfoParams p; p.version = 0x10; p.index = 0x1; p.reserved = 0;

        /* Pattern A: fn(ctx, params*, result*) -- cuxtra GetInfoPascal shape */
        GetInfoResult rA; std::memset(&rA, 0, sizeof rA);
        CUresult rcA = SAFE_CALL((SlotFn3)fp, (void *)g_ctx, &p, &rA);
        std::string sA = fmt_rc(rcA);

        /* Pattern B: fn(ctx, result*) */
        GetInfoResult rB; std::memset(&rB, 0, sizeof rB);
        CUresult rcB = SAFE_CALL((SlotFn2)fp, (void *)g_ctx, &rB);
        std::string sB = fmt_rc(rcB);

        /* Pattern C: fn(params*, result*) */
        GetInfoResult rC; std::memset(&rC, 0, sizeof rC);
        CUresult rcC = SAFE_CALL((SlotFn2)fp, &p, &rC);
        std::string sC = fmt_rc(rcC);

        const char *role = "";
        if (i == SLOT_GetInfoPascal)   role = " [GetInfoPascal]";
        else if (i == SLOT_ObjGetPc)   role = " [ObjGetPc?]";
        else if (i == SLOT_ObjGetSize) role = " [ObjGetSize?]";

        LOG("  slot %3zu = %p%s", i, fp, role);
        LOG("      A(ctx,p,r)   -> %s", sA.c_str());
        LOG("      B(ctx,r)     -> %s", sB.c_str());
        LOG("      C(p,r)       -> %s", sC.c_str());

        bool anyOut = false;
        if (rcA == CUDA_SUCCESS && buf_nonzero(&rA, sizeof rA)) {
            LOGRAW("      A output hexdump:\n"); probe_hexdump(&rA, sizeof rA, 16);
            anyOut = true;
        }
        if (rcB == CUDA_SUCCESS && buf_nonzero(&rB, sizeof rB)) {
            LOGRAW("      B output hexdump:\n"); probe_hexdump(&rB, sizeof rB, 16);
            anyOut = true;
        }
        if (rcC == CUDA_SUCCESS && buf_nonzero(&rC, sizeof rC)) {
            LOGRAW("      C output hexdump:\n"); probe_hexdump(&rC, sizeof rC, 16);
            anyOut = true;
        }

        char line[320];
        std::snprintf(line, sizeof line,
                      "slot %2zu%s: A=%s | B=%s | C=%s%s",
                      i, role, sA.c_str(), sB.c_str(), sC.c_str(),
                      anyOut ? " [+output]" : "");
        g_slot_result[i] = line;
    }
}

/* ================================================================== */
/*  Phase 4 - cuxtra TriggerTrap / GetTrapHandlerInfo independence      */
/* ================================================================== */

/* Thin void->CUresult wrappers so SAFE_CALL (which expects a CUresult return)
 * can guard the void-returning cuxtra entry points. The inner try/catch also
 * captures the case where cuxtra reports its fatal sm120 error by *throwing*
 * a C++ exception (error 101 from GetInfoPascal). If cuxtra instead calls
 * abort(), the SIGABRT handler logs and the process still dies - which is why
 * Phase 4 is scheduled AFTER the summary. */
static CUresult wrap_get_trap_info(CUcontext c, CUdeviceptr *h, size_t *s) {
    try { cuXtraGetTrapHandlerInfo(c, h, s); return CUDA_SUCCESS; }
    catch (...) { return PROBE_CXX_THREW; }
}
static CUresult wrap_trigger_trap(CUcontext c) {
    try { cuXtraTriggerTrap(c); return CUDA_SUCCESS; }
    catch (...) { return PROBE_CXX_THREW; }
}

static void phase4_trigger() {
    LOGRAW("\n");
    LOG("=== Phase 4: TriggerTrap / GetTrapHandlerInfo independence test ===");
    LOG("  [WARN] cuxtra's trap API is empirically PROCESS-FATAL on sm120:");
    LOG("         GetInfoPascal returns error 101 (INVALID_DEVICE) and cuxtra");
    LOG("         tears the process down. The full summary was printed above");
    LOG("         first; this phase runs last so its evidence is banked.");

    /* Cross-check: cuxtra's own GetTrapHandlerInfo funnels through slot 19.
     * If slot 19 is broken on sm120 this surfaces the same failure - or kills
     * us, which is itself the finding. */
    LOG("  -> calling cuXtraGetTrapHandlerInfo ...");
    CUdeviceptr handler = 0; size_t hsize = 0;
    CUresult rcGI = SAFE_CALL(wrap_get_trap_info, g_ctx, &handler, &hsize);
    LOG("  cuXtraGetTrapHandlerInfo -> %s | handler=0x%016llx size=%zu [%s]",
        fmt_rc(rcGI).c_str(), (unsigned long long)handler, hsize,
        probe_addr_class_str(probe_addr_classify(handler)));
    {
        char b[256];
        std::snprintf(b, sizeof b,
                      "cuXtraGetTrapHandlerInfo: %s (handler=0x%llx,size=%zu)",
                      fmt_rc(rcGI).c_str(), (unsigned long long)handler, hsize);
        g_getinfo_line = b;
    }

    /* The actual trigger. Without a rewritten handler this should be harmless
     * (the stock handler saves context and returns) - we only observe whether
     * it faults, throws, or errors, which tells us the RM-control register
     * write path (GlobalRegsWrite32, reg 0x00419e84 bit31) is alive on sm120. */
    LOG("  -> calling cuXtraTriggerTrap ...");
    CUresult rcTT = SAFE_CALL(wrap_trigger_trap, g_ctx);
    LOG("  cuXtraTriggerTrap        -> %s", fmt_rc(rcTT).c_str());
    {
        char b[160];
        std::snprintf(b, sizeof b, "cuXtraTriggerTrap: %s", fmt_rc(rcTT).c_str());
        g_trigger_line = b;
    }

    LOGRAW("\n---- PHASE 4 ADDENDUM (process survived the cuxtra trap API) ----\n");
    LOGRAW("  %s\n", g_getinfo_line.c_str());
    LOGRAW("  %s\n", g_trigger_line.c_str());
    LOGRAW("----------------------------------------------------------------\n");
}

/* ================================================================== */
/*  Phase 5 - alternative / mutated ETID scanning                      */
/* ================================================================== */

static void try_etid(const std::string &label, const CUuuid &etid) {
    char guid[37];
    probe_uuid_to_str(&etid, guid);
    const void *table = nullptr;
    CUresult rc = SAFE_CALL(g_cuGetExportTable, &table, &etid);
    LOG("  %-44s ETID=%s -> %s table=%p",
        label.c_str(), guid, fmt_rc(rc).c_str(), (void *)table);
    char line[320];
    std::snprintf(line, sizeof line, "%-42s %s -> %s",
                  label.c_str(), guid, fmt_rc(rc).c_str());
    g_alt_lines.push_back(line);
}

static void phase5_alt_etids() {
    LOG("=== Phase 5: alternative / mutated ETID scan ===");

    CUuuid base = make_etid(ETID_TRAP_BYTES);

    /* (1) flip the last byte */
    { CUuuid u = base; u.bytes[15] = (char)(u.bytes[15] ^ 0xFF);
      try_etid("TrapHandler ^ last-byte flipped", u); }

    /* (2) increment the first byte */
    { CUuuid u = base; u.bytes[0] = (char)(u.bytes[0] + 1);
      try_etid("TrapHandler ^ first-byte +1", u); }

    /* (3) all-zeros ETID */
    { CUuuid u; std::memset(u.bytes, 0, 16);
      try_etid("all-zeros ETID", u); }

    /* (4) first 4 bytes = 0x949e52cc in various byte orders.
     *     Note: the original first four bytes ARE cc 52 9e 94, which read as a
     *     little-endian uint32 is exactly 0x949e52cc. Try both orders with
     *     (a) the original tail and (b) a zeroed tail. */
    const unsigned char tail[12] = { 0x5c,0x4e,0x9d,0x46,0x83,0x7c,
                                     0x96,0x25,0x86,0x83,0x34,0xe4 };
    { CUuuid u; std::memset(&u, 0, 16);
      u.bytes[0]=(char)0xcc; u.bytes[1]=(char)0x52;
      u.bytes[2]=(char)0x9e; u.bytes[3]=(char)0x94;
      std::memcpy(u.bytes + 4, tail, 12);
      try_etid("head 0x949e52cc LE + orig tail (control)", u); }
    { CUuuid u; std::memset(&u, 0, 16);
      u.bytes[0]=(char)0x94; u.bytes[1]=(char)0x9e;
      u.bytes[2]=(char)0x52; u.bytes[3]=(char)0xcc;
      std::memcpy(u.bytes + 4, tail, 12);
      try_etid("head 0x949e52cc BE + orig tail", u); }
    { CUuuid u; std::memset(&u, 0, 16);
      u.bytes[0]=(char)0xcc; u.bytes[1]=(char)0x52;
      u.bytes[2]=(char)0x9e; u.bytes[3]=(char)0x94;
      try_etid("head 0x949e52cc LE + zero tail", u); }
    { CUuuid u; std::memset(&u, 0, 16);
      u.bytes[0]=(char)0x94; u.bytes[1]=(char)0x9e;
      u.bytes[2]=(char)0x52; u.bytes[3]=(char)0xcc;
      try_etid("head 0x949e52cc BE + zero tail", u); }
}

/* ================================================================== */
/*  Phase 7 - COMPUTE_PREEMPTION_SUPPORTED (device attribute 90)       */
/* ================================================================== */

static void phase7_preemption_attr() {
    LOG("=== Phase 7: CU_DEVICE_ATTRIBUTE_COMPUTE_PREEMPTION_SUPPORTED (90) ===");
    int v = -1;
    CUresult rc = SAFE_CALL(g_cuDeviceGetAttribute, &v, 90, g_dev);
    LOG("  attr 90 -> %s | value = %d (%s)",
        fmt_rc(rc).c_str(), v,
        (rc == CUDA_SUCCESS) ? (v ? "ILP/compute-preemption SUPPORTED"
                                  : "compute-preemption NOT supported")
                             : "query failed");
    char b[160];
    std::snprintf(b, sizeof b, "attr90 COMPUTE_PREEMPTION_SUPPORTED: %s value=%d",
                  fmt_rc(rc).c_str(), v);
    g_preempt_line = b;
}

/* ================================================================== */
/*  Phase 6 - structured summary report                               */
/* ================================================================== */

static std::string slot_line_or(size_t slot, const char *fallback) {
    std::map<size_t, std::string>::iterator it = g_slot_result.find(slot);
    return (it != g_slot_result.end()) ? it->second : std::string(fallback);
}

static void phase6_summary() {
    LOGRAW("\n");
    LOGRAW("================ EXPORT TABLE PROBE SUMMARY ================\n");
    LOGRAW("device: %s | CC %d.%d | driver %d | ctx %p\n",
           g_dev_name, g_cc_major, g_cc_minor, g_driver_ver, (void *)g_ctx);
    LOGRAW("nvcuda.dll: base 0x%016llx size 0x%llx\n",
           (unsigned long long)g_nvcuda_base, (unsigned long long)g_nvcuda_size);

    LOGRAW("\nTrapHandler ETID (cc529e94-...-9625868334e4): %s\n",
           g_trap_ok ? "SUCCESS" : "FAILURE");
    if (g_trap_ok) {
        LOGRAW("  table size = %llu bytes, %zu slots\n",
               (unsigned long long)g_trap_size, g_trap_maxslots);
        LOGRAW("  Slot 19 (GetInfoPascal):\n");
        if (g_slot19_lines.empty()) LOGRAW("    (not probed)\n");
        for (size_t i = 0; i < g_slot19_lines.size(); ++i)
            LOGRAW("    %s\n", g_slot19_lines[i].c_str());
        LOGRAW("  Slot 24 (ObjGetPc):   %s\n",
               slot_line_or(SLOT_ObjGetPc, "(absent/NULL)").c_str());
        LOGRAW("  Slot 25 (ObjGetSize): %s\n",
               slot_line_or(SLOT_ObjGetSize, "(absent/NULL)").c_str());
        LOGRAW("  Other non-NULL slots (1..%zu):\n", MAX_PROBE_SLOT);
        bool any = false;
        for (std::map<size_t, std::string>::iterator kv = g_slot_result.begin();
             kv != g_slot_result.end(); ++kv) {
            if (kv->first == SLOT_GetInfoPascal || kv->first == SLOT_ObjGetPc ||
                kv->first == SLOT_ObjGetSize) continue;
            LOGRAW("    %s\n", kv->second.c_str());
            any = true;
        }
        if (!any) LOGRAW("    (none)\n");
    }

    LOGRAW("\nToolsMemory ETID (bfdb432d-...-b34029e81e75): %s\n",
           g_mem_ok ? "SUCCESS" : "FAILURE");
    if (g_mem_ok)
        LOGRAW("  table size = %llu bytes, %zu slots\n",
               (unsigned long long)g_mem_size, g_mem_maxslots);

    LOGRAW("\ncuxtra GetTrapHandlerInfo: %s\n", g_getinfo_line.c_str());
    LOGRAW("cuxtra TriggerTrap:        %s\n", g_trigger_line.c_str());
    LOGRAW("%s\n", g_preempt_line.c_str());

    LOGRAW("\nAlternative ETIDs:\n");
    if (g_alt_lines.empty()) LOGRAW("  (none tried)\n");
    for (size_t i = 0; i < g_alt_lines.size(); ++i)
        LOGRAW("  %s\n", g_alt_lines[i].c_str());

    /* ---- conclusion heuristic ---- */
    LOGRAW("\nCONCLUSION: ");
    if (!g_trap_ok) {
        LOGRAW("Trap path NOT VIABLE via export table - the TrapHandler ETID "
               "did not resolve on this driver (ET likely removed/unregistered "
               "for sm120). Investigate the TSG path or a Blackwell-specific "
               "ETID.\n");
    } else {
        bool slot19_useful = false;
        for (size_t i = 0; i < g_slot19_lines.size(); ++i)
            if (g_slot19_lines[i].find("rc=0") != std::string::npos) {
                slot19_useful = true; break;
            }
        if (slot19_useful)
            LOGRAW("Trap path MAY BE VIABLE - TrapHandler table resolved and "
                   "slot 19 (GetInfoPascal) returned CUDA_SUCCESS. Inspect the "
                   "hexdump for a Blackwell handler address before relying on "
                   "it.\n");
        else
            LOGRAW("Trap path NEEDS FURTHER INVESTIGATION - the table resolved "
                   "but slot 19 (GetInfoPascal) never returned success "
                   "(error 101 for every combo: the Pascal-era getter has no "
                   "sm120 case). A GetInfoBlackwell equivalent must be located "
                   "or the TSG path used.\n");
    }
    LOGRAW("============================================================\n");
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */

int main(int argc, char **argv) {
    /* --no-phase4 skips the (empirically process-fatal) cuxtra trap call so
     * the probe can return a clean exit code once the summary is banked. */
    bool run_phase4 = true;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--no-phase4") == 0) run_phase4 = false;
    /* Bank a last-gasp message if cuxtra aborts during the Phase 4 trap API. */
    std::signal(SIGABRT, probe_sigabrt_handler);

    LOG("=== probe_export_table: sm120 L3 Trap-Handler export-table probe ===");

    if (!phase0_setup()) {
        LOG("=== aborting: environment setup failed (critical=%d) ===", g_critical);
        return g_critical ? g_critical : 1;
    }

    /* ---- Phase 1: TrapHandler export-table discovery ---- */
    LOG("=== Phase 1: Export Table discovery (TrapHandler ETID) ===");
    CUuuid etid_trap = make_etid(ETID_TRAP_BYTES);
    const void **trap_table = nullptr;
    g_trap_ok = discover_table(&etid_trap, "TrapHandler",
                               &trap_table, &g_trap_size, &g_trap_maxslots);
    if (g_trap_ok) {
        std::vector<size_t> nonnull;
        enumerate_table(trap_table, g_trap_maxslots, "TrapHandler", &nonnull);

        /* ---- Phase 2: slot probing (only meaningful with a table) ---- */
        phase2_slot19(trap_table, g_trap_maxslots);
        phase2_generic(trap_table, g_trap_maxslots);
    } else {
        LOG("[CRITICAL] cuGetExportTable FAILED for the TrapHandler ETID - the "
            "entire export table appears removed/unregistered in this driver.");
        ++g_critical;
        g_slot19_lines.push_back("table unavailable (cuGetExportTable failed)");
    }

    /* ---- Phase 3: ToolsMemory export-table discovery (baseline) ---- */
    LOG("=== Phase 3: Export Table discovery (ToolsMemory ETID) ===");
    CUuuid etid_mem = make_etid(ETID_MEM_BYTES);
    const void **mem_table = nullptr;
    g_mem_ok = discover_table(&etid_mem, "ToolsMemory",
                              &mem_table, &g_mem_size, &g_mem_maxslots);
    if (g_mem_ok) {
        enumerate_table(mem_table, g_mem_maxslots, "ToolsMemory", nullptr);
    } else {
        LOG("[WARN] ToolsMemory ETID did not resolve - unexpected, since L2 "
            "InstrMem APIs rely on it.");
    }

    /* ---- Phase 5: alternative ETIDs ---- */
    phase5_alt_etids();

    /* ---- Phase 7: compute-preemption attribute ---- */
    phase7_preemption_attr();

    /* ---- Phase 6: summary ----
     * Printed BEFORE Phase 4 on purpose. cuxtra's trap API is empirically
     * process-fatal on sm120 (GetInfoPascal error 101 -> teardown), so we bank
     * every other result first. Phase 4 then runs last and appends its own
     * addendum if - and only if - the process survives. */
    phase6_summary();

    /* ---- Phase 4: TriggerTrap / GetTrapHandlerInfo (may be fatal) ---- */
    if (run_phase4) {
        phase4_trigger();
    } else {
        LOG("=== Phase 4 SKIPPED (--no-phase4): cuxtra trap API is process-fatal on sm120 ===");
    }

    LOG("=== probe_export_table done: critical failures = %d ===", g_critical);
    return g_critical;
}
