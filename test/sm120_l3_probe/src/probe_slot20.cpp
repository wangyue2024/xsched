/*
 * probe_slot20.cpp - deep investigation of TrapHandler export-table slot 20
 *                    on sm120 (Blackwell).
 *
 * BACKGROUND
 * ----------
 * probe_export_table discovered that slot 19 (GetInfoPascal) returns error 101
 * on sm120, BUT slot 20 returns CUDA_SUCCESS and writes output data when called
 * as fn(ctx, result*).  The output contained:
 *     +0x10 = 0x00000236_0637ca90  (device-looking pointer)
 *     +0x18 = 0x880 (2176 bytes)
 *     +0x20 = 0x940 (2368 bytes)
 *
 * This probe performs a structured investigation:
 *   Phase 0: environment setup (driver, context, device memory baseline)
 *   Phase 1: resolve TrapHandler + ToolsMemory export tables
 *   Phase 2: slot 20 deep probe (repeatability, parameter sensitivity)
 *   Phase 3: cross-reference with ToolsMemory slots (ObjGetPc / ObjGetSize)
 *   Phase 4: probe other SUCCESS slots (1, 5, 8, 10, 16, 22) for comparison
 *   Phase 5: error-101 slots (11, 12) version sweep
 *   Phase 6: structured summary + conclusion
 *
 * Build: powershell -ExecutionPolicy Bypass -File build.ps1 -Target probe_slot20
 * Run  : powershell -ExecutionPolicy Bypass -File run.ps1   -Probe probe_slot20
 */
#include "probe_common.h"

#include <vector>
#include <string>
#include <cstring>

/* ================================================================== */
/*  Additional driver entry points needed for this probe               */
/* ================================================================== */

typedef CUresult (*pfn_cuMemAlloc_v2)(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult (*pfn_cuMemFree_v2)(CUdeviceptr dptr);
typedef CUresult (*pfn_cuMemcpyDtoH_v2)(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount);
typedef CUresult (*pfn_cuPointerGetAttribute)(void *data, int attribute, CUdeviceptr ptr);

#define CU_POINTER_ATTRIBUTE_CONTEXT        1
#define CU_POINTER_ATTRIBUTE_MEMORY_TYPE    2
#define CU_POINTER_ATTRIBUTE_DEVICE_POINTER 3
#define CU_POINTER_ATTRIBUTE_HOST_POINTER   4
#define CU_POINTER_ATTRIBUTE_IS_MANAGED     13
#define CU_POINTER_ATTRIBUTE_RANGE_START_ADDR 15
#define CU_POINTER_ATTRIBUTE_RANGE_SIZE     16

static pfn_cuMemAlloc_v2         g_cuMemAlloc_v2         = nullptr;
static pfn_cuMemFree_v2          g_cuMemFree_v2          = nullptr;
static pfn_cuMemcpyDtoH_v2       g_cuMemcpyDtoH_v2       = nullptr;
static pfn_cuPointerGetAttribute g_cuPointerGetAttribute = nullptr;

/* ================================================================== */
/*  Known ETIDs                                                        */
/* ================================================================== */

static const unsigned char ETID_TRAP_BYTES[16] = {
    0xcc, 0x52, 0x9e, 0x94, 0x5c, 0x4e, 0x9d, 0x46,
    0x83, 0x7c, 0x96, 0x25, 0x86, 0x83, 0x34, 0xe4 };

static const unsigned char ETID_MEM_BYTES[16] = {
    0xbf, 0xdb, 0x43, 0x2d, 0xbf, 0x3c, 0x5a, 0x4a,
    0x94, 0x5e, 0xb3, 0x40, 0x29, 0xe8, 0x1e, 0x75 };

static const size_t MEM_SLOT_ObjGetPc   = 24;
static const size_t MEM_SLOT_ObjGetSize = 25;

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

static HMODULE   g_hNvcuda     = nullptr;
static uintptr_t g_nvcuda_base = 0;
static uintptr_t g_nvcuda_size = 0;

static CUdevice  g_dev         = 0;
static CUcontext g_ctx         = nullptr;
static int       g_driver_ver  = 0;
static int       g_cc_major    = 0;
static int       g_cc_minor    = 0;
static char      g_dev_name[256] = {0};

static const void **g_trap_table = nullptr;
static uint64_t     g_trap_size  = 0;
static size_t       g_trap_max   = 0;

static const void **g_mem_table  = nullptr;
static uint64_t     g_mem_size   = 0;
static size_t       g_mem_max    = 0;

static CUdeviceptr  g_baseline_dev = 0;
static const size_t BASELINE_SIZE  = 4096;

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

typedef CUresult (*SlotFn3)(void *, void *, void *);
typedef CUresult (*SlotFn2)(void *, void *);
typedef CUresult (*SlotFn1)(void *);

static std::string fmt_rc(CUresult rc) {
    char b[160];
    if (rc == PROBE_SEH_RAISED)
        std::snprintf(b, sizeof b, "SEH-FAULT(code=0x%08lx)",
                      (unsigned long)probe_last_seh_code);
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

/* ================================================================== */
/*  Phase 0                                                            */
/* ================================================================== */

#define RESOLVE(dst, symname, type)                                            \
    do {                                                                       \
        (dst) = (type)(void *)GetProcAddress(g_hNvcuda, symname);              \
        if (!(dst)) LOG("[WARN] missing export: %s", symname);                 \
    } while (0)

static bool phase0_setup() {
    LOG("=== Phase 0: environment setup ===");
    const char *lib = "C:\\Windows\\System32\\nvcuda.dll";
    g_hNvcuda = LoadLibraryA(lib);
    if (!g_hNvcuda) { LOG("[CRITICAL] LoadLibrary failed gle=%lu", GetLastError()); return false; }
    g_nvcuda_base = (uintptr_t)g_hNvcuda;
    { IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_nvcuda_base;
      if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_nvcuda_base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) g_nvcuda_size = nt->OptionalHeader.SizeOfImage;
      } }
    LOG("  nvcuda base=0x%016llx size=0x%llx",
        (unsigned long long)g_nvcuda_base, (unsigned long long)g_nvcuda_size);

    RESOLVE(g_cuInit,               "cuInit",                pfn_cuInit);
    RESOLVE(g_cuDriverGetVersion,   "cuDriverGetVersion",    pfn_cuDriverGetVersion);
    RESOLVE(g_cuDeviceGet,          "cuDeviceGet",           pfn_cuDeviceGet);
    RESOLVE(g_cuDeviceGetCount,     "cuDeviceGetCount",      pfn_cuDeviceGetCount);
    RESOLVE(g_cuDeviceGetAttribute, "cuDeviceGetAttribute",  pfn_cuDeviceGetAttribute);
    RESOLVE(g_cuDeviceGetName,      "cuDeviceGetName",       pfn_cuDeviceGetName);
    RESOLVE(g_cuCtxCreate_v2,       "cuCtxCreate_v2",        pfn_cuCtxCreate_v2);
    RESOLVE(g_cuCtxGetCurrent,      "cuCtxGetCurrent",       pfn_cuCtxGetCurrent);
    RESOLVE(g_cuGetExportTable,     "cuGetExportTable",      pfn_cuGetExportTable);
    RESOLVE(g_cuMemAlloc_v2,        "cuMemAlloc_v2",         pfn_cuMemAlloc_v2);
    RESOLVE(g_cuMemFree_v2,         "cuMemFree_v2",          pfn_cuMemFree_v2);
    RESOLVE(g_cuMemcpyDtoH_v2,      "cuMemcpyDtoH_v2",       pfn_cuMemcpyDtoH_v2);
    RESOLVE(g_cuPointerGetAttribute,"cuPointerGetAttribute",  pfn_cuPointerGetAttribute);

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

    if (g_cuMemAlloc_v2) {
        CUresult rc = SAFE_CALL(g_cuMemAlloc_v2, &g_baseline_dev, BASELINE_SIZE);
        LOG("  baseline alloc -> %s ptr=0x%016llx", fmt_rc(rc).c_str(),
            (unsigned long long)g_baseline_dev);
        if (rc == CUDA_SUCCESS && g_baseline_dev)
            probe_range_observe_device(g_baseline_dev, BASELINE_SIZE);
    }
    LOG("=== Phase 0 complete ===");
    return true;
}

/* ================================================================== */
/*  Phase 1                                                            */
/* ================================================================== */

static bool phase1_tables() {
    LOG("=== Phase 1: resolve export tables ===");
    CUuuid etid = make_etid(ETID_TRAP_BYTES);
    const void *table = nullptr;
    CUresult rc = SAFE_CALL(g_cuGetExportTable, &table, &etid);
    if (rc != CUDA_SUCCESS || !table) { LOG("[CRITICAL] TrapHandler failed"); return false; }
    g_trap_table = (const void **)table;
    g_trap_size = (uint64_t)g_trap_table[0];
    g_trap_max = (size_t)(g_trap_size / 8);
    LOG("  TrapHandler: %llu bytes, %zu slots", (unsigned long long)g_trap_size, g_trap_max);

    etid = make_etid(ETID_MEM_BYTES);
    table = nullptr;
    rc = SAFE_CALL(g_cuGetExportTable, &table, &etid);
    if (rc == CUDA_SUCCESS && table) {
        g_mem_table = (const void **)table;
        g_mem_size = (uint64_t)g_mem_table[0];
        g_mem_max = (size_t)(g_mem_size / 8);
        LOG("  ToolsMemory: %llu bytes, %zu slots", (unsigned long long)g_mem_size, g_mem_max);
    } else { LOG("  [WARN] ToolsMemory failed"); }
    LOG("=== Phase 1 complete ===");
    return true;
}

/* ================================================================== */
/*  Phase 2 - slot 20 deep probe                                       */
/* ================================================================== */

struct BigResult { uint8_t data[256]; };

static void phase2_slot20() {
    LOG("=== Phase 2: slot 20 deep investigation ===");
    if (g_trap_max <= 20 || !g_trap_table[20]) { LOG("  slot 20 absent"); return; }
    void *fp = (void *)g_trap_table[20];
    LOG("  slot 20 fn = %p", fp);

    /* 2a: repeatability */
    LOG("  --- 2a: repeatability (3 calls, pattern B) ---");
    BigResult res[3];
    for (int i = 0; i < 3; ++i) {
        std::memset(&res[i], 0, sizeof(BigResult));
        CUresult rc = SAFE_CALL((SlotFn2)fp, (void *)g_ctx, &res[i]);
        LOG("    call[%d] -> %s", i, fmt_rc(rc).c_str());
        if (rc == CUDA_SUCCESS && buf_nonzero(&res[i], 64)) {
            LOGRAW("    hexdump(48):\n"); probe_hexdump(res[i].data, 48, 16);
        }
    }
    bool same = (std::memcmp(res[0].data, res[1].data, 64) == 0) &&
                (std::memcmp(res[1].data, res[2].data, 64) == 0);
    LOG("  repeatability: %s", same ? "IDENTICAL" : "DIFFERS");

    /* 2b: pattern A version sweep */
    LOG("  --- 2b: pattern A version/index sweep ---");
    struct Params { uint32_t version; uint32_t index; uint64_t reserved; };
    static const uint32_t vers[] = { 0x00, 0x01, 0x10, 0x20, 0x30, 0x40, 0x100 };
    static const uint32_t idxs[] = { 0x00, 0x01, 0x02 };
    for (size_t vi = 0; vi < sizeof(vers)/sizeof(vers[0]); ++vi) {
        for (size_t ii = 0; ii < sizeof(idxs)/sizeof(idxs[0]); ++ii) {
            Params p = { vers[vi], idxs[ii], 0 };
            BigResult r; std::memset(&r, 0, sizeof r);
            CUresult rc = SAFE_CALL((SlotFn3)fp, (void *)g_ctx, &p, &r);
            if (rc == CUDA_SUCCESS && buf_nonzero(&r, 48)) {
                LOG("    A(v=0x%02x,i=%u) -> SUCCESS +output", vers[vi], idxs[ii]);
                probe_hexdump(r.data, 48, 16);
            } else if (rc != CUDA_SUCCESS) {
                LOG("    A(v=0x%02x,i=%u) -> %s", vers[vi], idxs[ii], fmt_rc(rc).c_str());
            }
        }
    }

    /* 2c: full output dump + field interpretation */
    LOG("  --- 2c: output field interpretation ---");
    {
        BigResult r; std::memset(&r, 0, sizeof r);
        CUresult rc = SAFE_CALL((SlotFn2)fp, (void *)g_ctx, &r);
        if (rc == CUDA_SUCCESS) {
            LOGRAW("  full 128-byte output:\n");
            probe_hexdump(r.data, 128, 16);

            uint64_t v[8];
            for (int i = 0; i < 8; ++i) std::memcpy(&v[i], r.data + i*8, 8);
            for (int i = 0; i < 8; ++i)
                LOG("  +0x%02x = 0x%016llx (%llu)", i*8,
                    (unsigned long long)v[i], (unsigned long long)v[i]);

            /* Classify pointer candidates */
            for (int i = 0; i < 8; ++i) {
                if (v[i] > 0x10000 && v[i] < 0x00007FFFFFFFFFFFull) {
                    ProbeAddrClass cls = probe_addr_classify((CUdeviceptr)v[i]);
                    LOG("  +0x%02x looks like a pointer -> %s", i*8,
                        probe_addr_class_str(cls));

                    if (g_cuPointerGetAttribute && cls != PROBE_ADDR_RESERVED) {
                        int mem_type = -1; uint64_t range_sz = 0;
                        CUdeviceptr range_start = 0;
                        SAFE_CALL(g_cuPointerGetAttribute, &mem_type,
                                  CU_POINTER_ATTRIBUTE_MEMORY_TYPE, (CUdeviceptr)v[i]);
                        SAFE_CALL(g_cuPointerGetAttribute, &range_start,
                                  CU_POINTER_ATTRIBUTE_RANGE_START_ADDR, (CUdeviceptr)v[i]);
                        SAFE_CALL(g_cuPointerGetAttribute, &range_sz,
                                  CU_POINTER_ATTRIBUTE_RANGE_SIZE, (CUdeviceptr)v[i]);
                        LOG("    mem_type=%d(%s) range=[0x%llx, +%llu]",
                            mem_type, mem_type==1?"HOST":mem_type==2?"DEVICE":"?",
                            (unsigned long long)range_start, (unsigned long long)range_sz);
                    }

                    /* Try reading 64 bytes */
                    if (g_cuMemcpyDtoH_v2) {
                        uint8_t buf[64] = {0};
                        CUresult rcp = SAFE_CALL(g_cuMemcpyDtoH_v2, buf, (CUdeviceptr)v[i], 64);
                        LOG("    cuMemcpyDtoH -> %s", fmt_rc(rcp).c_str());
                        if (rcp == CUDA_SUCCESS && buf_nonzero(buf, 64)) {
                            LOGRAW("    content:\n"); probe_hexdump(buf, 64, 16);
                        }
                    }
                }
            }
        }
    }
    LOG("=== Phase 2 complete ===");
}

/* ================================================================== */
/*  Phase 3 - ToolsMemory cross-reference                              */
/* ================================================================== */

static void phase3_memobj() {
    LOG("=== Phase 3: ToolsMemory cross-reference ===");
    if (!g_mem_table || g_mem_max <= MEM_SLOT_ObjGetSize) { LOG("  unavailable"); return; }
    if (g_trap_max <= 20 || !g_trap_table[20]) { LOG("  slot 20 unavailable"); return; }

    BigResult r; std::memset(&r, 0, sizeof r);
    SAFE_CALL((SlotFn2)(void *)g_trap_table[20], (void *)g_ctx, &r);
    uint64_t v10 = 0, v28 = 0;
    std::memcpy(&v10, r.data + 0x10, 8);
    std::memcpy(&v28, r.data + 0x28, 8);

    void *fn_pc = (void *)g_mem_table[MEM_SLOT_ObjGetPc];
    void *fn_sz = (void *)g_mem_table[MEM_SLOT_ObjGetSize];
    LOG("  ObjGetPc=%p ObjGetSize=%p", fn_pc, fn_sz);

    /* Try both values as memObjHandle */
    uint64_t candidates[] = { v10, v28 };
    for (int c = 0; c < 2; ++c) {
        if (!candidates[c]) continue;
        LOG("  candidate[%d] = 0x%016llx:", c, (unsigned long long)candidates[c]);
        if (fn_pc) {
            uint64_t h = candidates[c], pc = 0;
            CUresult rc = SAFE_CALL((SlotFn3)fn_pc, (void *)g_ctx, &h, &pc);
            LOG("    ObjGetPc -> %s pc=0x%016llx", fmt_rc(rc).c_str(), (unsigned long long)pc);
        }
        if (fn_sz) {
            uint64_t h = candidates[c], sz = 0;
            CUresult rc = SAFE_CALL((SlotFn2)fn_sz, &h, &sz);
            LOG("    ObjGetSize -> %s sz=%llu", fmt_rc(rc).c_str(), (unsigned long long)sz);
        }
    }
    LOG("=== Phase 3 complete ===");
}

/* ================================================================== */
/*  Phase 4 - other SUCCESS slots                                      */
/* ================================================================== */

static void phase4_other_slots() {
    LOG("=== Phase 4: other SUCCESS slots ===");
    static const size_t slots[] = { 1, 5, 8, 10, 16, 22 };
    for (size_t si = 0; si < sizeof(slots)/sizeof(slots[0]); ++si) {
        size_t i = slots[si];
        if (g_trap_max <= i || !g_trap_table[i]) continue;
        void *fp = (void *)g_trap_table[i];
        LOG("  slot %zu = %p:", i, fp);
        BigResult r; std::memset(&r, 0, sizeof r);
        CUresult rc = SAFE_CALL((SlotFn2)fp, (void *)g_ctx, &r);
        LOG("    B(ctx,r) -> %s", fmt_rc(rc).c_str());
        if (rc == CUDA_SUCCESS && buf_nonzero(&r, 48)) {
            probe_hexdump(r.data, 48, 16);
            uint64_t v0 = 0, v1 = 0, v2 = 0, v3 = 0;
            std::memcpy(&v0, r.data, 8); std::memcpy(&v1, r.data+8, 8);
            std::memcpy(&v2, r.data+16, 8); std::memcpy(&v3, r.data+24, 8);
            LOG("    [0]=0x%llx [1]=0x%llx [2]=0x%llx [3]=0x%llx",
                (unsigned long long)v0, (unsigned long long)v1,
                (unsigned long long)v2, (unsigned long long)v3);
        }
    }
    LOG("=== Phase 4 complete ===");
}

/* ================================================================== */
/*  Phase 5 - error-101 slots version sweep                            */
/* ================================================================== */

static void phase5_err101() {
    LOG("=== Phase 5: error-101 slots (11,12,19) version sweep ===");
    static const size_t err_slots[] = { 11, 12, 19 };
    struct Params { uint32_t version; uint32_t index; uint64_t reserved; };
    static const uint32_t vers[] = { 0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80,0x90,0xA0,0xB0,0xC0 };

    for (size_t si = 0; si < 3; ++si) {
        size_t slot = err_slots[si];
        if (g_trap_max <= slot || !g_trap_table[slot]) continue;
        void *fp = (void *)g_trap_table[slot];
        LOG("  slot %zu = %p:", slot, fp);
        bool any = false;
        for (size_t vi = 0; vi < sizeof(vers)/sizeof(vers[0]); ++vi) {
            Params p = { vers[vi], 0x01, 0 };
            BigResult r; std::memset(&r, 0, sizeof r);
            CUresult rc = SAFE_CALL((SlotFn3)fp, (void *)g_ctx, &p, &r);
            if (rc == CUDA_SUCCESS) {
                LOG("    v=0x%02x -> SUCCESS!", vers[vi]);
                if (buf_nonzero(&r, 48)) probe_hexdump(r.data, 48, 16);
                any = true;
            }
        }
        if (!any) LOG("    all versions -> error (arch mismatch confirmed)");
    }
    LOG("=== Phase 5 complete ===");
}

/* ================================================================== */
/*  Phase 6 - summary                                                  */
/* ================================================================== */

static void phase6_summary() {
    LOGRAW("\n================ SLOT 20 PROBE SUMMARY ================\n");
    LOGRAW("device: %s | CC %d.%d | driver %d\n", g_dev_name, g_cc_major, g_cc_minor, g_driver_ver);
    LOGRAW("baseline dev alloc: 0x%016llx\n", (unsigned long long)g_baseline_dev);

    if (g_trap_max > 20 && g_trap_table[20]) {
        BigResult r; std::memset(&r, 0, sizeof r);
        CUresult rc = SAFE_CALL((SlotFn2)(void *)g_trap_table[20], (void *)g_ctx, &r);
        if (rc == CUDA_SUCCESS) {
            uint64_t v[6]; for (int i=0;i<6;++i) std::memcpy(&v[i], r.data+i*8, 8);
            LOGRAW("\nSlot 20 output (Blackwell GetInfo candidate):\n");
            for (int i=0;i<6;++i)
                LOGRAW("  +0x%02x = 0x%016llx (%llu)\n", i*8,
                       (unsigned long long)v[i], (unsigned long long)v[i]);
        }
    }
    LOGRAW("\nCONCLUSION: see Phase 2d field interpretation above.\n");
    LOGRAW("========================================================\n");
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */

int main(void) {
    LOG("=== probe_slot20: sm120 TrapHandler slot 20 deep investigation ===");
    if (!phase0_setup()) return 1;
    if (!phase1_tables()) return 2;
    phase2_slot20();
    phase3_memobj();
    phase4_other_slots();
    phase5_err101();
    phase6_summary();
    if (g_baseline_dev && g_cuMemFree_v2) SAFE_CALL(g_cuMemFree_v2, g_baseline_dev);
    LOG("=== probe_slot20 done ===");
    return 0;
}
