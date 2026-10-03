/*
 * probe_selftest.cpp - toolchain + scaffolding smoke test (NO GPU needed).
 *
 * Validates that the build/run pipeline and probe_common.h work end-to-end
 * on this machine before any real export-table probe is written:
 *   [S1] logging + CUresult error macros
 *   [S2] SEH guard (SAFE_CALL) survives a deliberate access violation
 *   [S3] empirical device/host VA classification
 *   [S4] ETID (CUuuid) formatting + hexdump helpers
 *   [S5] cuxtra static library linked (symbol resolves, not called here)
 *
 * Build: powershell -ExecutionPolicy Bypass -File build.ps1 -Target probe_selftest
 * Run  : powershell -ExecutionPolicy Bypass -File run.ps1   -Probe probe_selftest
 *
 * Exit code = number of failed self-tests (0 = all pass).
 */
#include "probe_common.h"

/* A function that faults on purpose; used to prove the SEH guard works. */
static CUresult boom(void) {
    *(volatile int *)0 = 42;   /* deliberate access violation */
    return CUDA_SUCCESS;
}
static CUresult good(void) { return CUDA_SUCCESS; }

/* SEH must live in a leaf frame with no C++ objects needing unwinding. */
static int run_seh_test(void) {
    CUresult rc = CUDA_SUCCESS;
    int fail = 0;

    probe_last_seh_code = 0;
    SAFE_CALL_TO(rc, good);
    LOG("[S2] good() -> rc=%d seh=0x%08lx", rc, (unsigned long)probe_last_seh_code);
    if (rc != CUDA_SUCCESS || probe_last_seh_code != 0) {
        LOG("[S2] FAIL: good() should return cleanly");
        ++fail;
    }

    probe_last_seh_code = 0;
    SAFE_CALL_TO(rc, boom);
    LOG("[S2] boom() -> rc=%d seh=0x%08lx (expect SEH_RAISED + 0xc0000005)",
        rc, (unsigned long)probe_last_seh_code);
    if (rc != PROBE_SEH_RAISED) {
        LOG("[S2] FAIL: boom() should have been caught by SAFE_CALL");
        ++fail;
    }
    if (probe_last_seh_code != 0xC0000005u) {
        LOG("[S2] WARN: unexpected exception code 0x%08lx",
            (unsigned long)probe_last_seh_code);
    }
    return fail;
}

int main(void) {
    int fail = 0;
    LOG("=== probe_selftest: scaffolding smoke test (no GPU) ===");

    /* [S1] logging + error macros ------------------------------------ */
    CUresult r = CUDA_ERROR_NOT_SUPPORTED;
    LOG("[S1] probe_cu_err_str(%d) = %s", r, probe_cu_err_str(r));
    CHECK_CU_VOID((CUresult)CUDA_SUCCESS);   /* must not log an error */
    if (probe_cu_err_str(CUDA_SUCCESS) == nullptr) { ++fail; }

    /* [S2] SEH guard -------------------------------------------------- */
    fail += run_seh_test();

    /* [S3] device/host VA classification ------------------------------ */
    CUdeviceptr dev = 0x00000007F0000000ull;   /* pretend cuMemAlloc result */
    CUdeviceptr host = 0x0000000000120000ull;  /* pretend pinned host       */
    probe_range_observe_device(dev, 1 << 20);
    probe_range_observe_host(host, 1 << 16);
    struct { CUdeviceptr a; ProbeAddrClass want; const char *label; } cases[] = {
        { dev,             PROBE_ADDR_DEVICE,   "device base"   },
        { dev + 4096,      PROBE_ADDR_DEVICE,   "device +4k"    },
        { host,            PROBE_ADDR_HOST,     "host base"     },
        { 0,               PROBE_ADDR_NULL,     "null"          },
        { 0xFFFFFFFFFFFFull, PROBE_ADDR_RESERVED, "non-canonical"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        ProbeAddrClass got = probe_addr_classify(cases[i].a);
        int ok = (got == cases[i].want);
        LOG("[S3] %-14s ptr=0x%016llx -> %-22s %s",
            cases[i].label, (unsigned long long)cases[i].a,
            probe_addr_class_str(got), ok ? "PASS" : "FAIL");
        if (!ok) { ++fail; }
    }
    if (!probe_addr_is_device(dev)) { LOG("[S3] FAIL is_device"); ++fail; }

    /* [S4] ETID formatting + hexdump --------------------------------- */
    CUuuid etid;   /* the CU_ETID_ToolsTrapHandler GUID recovered statically */
    const unsigned char bytes[16] = {
        0xcc,0x52,0x9e,0x94,0x5c,0x4e,0x9d,0x46,
        0x83,0x7c,0x96,0x25,0x86,0x83,0x34,0xe4 };
    memcpy(etid.bytes, bytes, 16);
    char guid[37];
    probe_uuid_to_str(&etid, guid);
    LOG("[S4] CU_ETID_ToolsTrapHandler = %s", guid);
    if (strcmp(guid, "cc529e94-5c4e-9d46-837c-9625868334e4") != 0) {
        LOG("[S4] FAIL: unexpected GUID text"); ++fail;
    }
    LOGRAW("[S4] hexdump of the 16-byte ETID:\n");
    probe_hexdump(bytes, 16, 16);

    /* [S5] cuxtra linkage (address of an API symbol; not invoked) ----- */
    void *sym = (void *)&cuXtraGetTrapHandlerInfo;
    LOG("[S5] cuxtra linked: &cuXtraGetTrapHandlerInfo = %p", sym);
    if (sym == nullptr) { ++fail; }

    LOG("=== probe_selftest: %s (failures=%d) ===",
        fail == 0 ? "ALL PASS" : "FAILED", fail);
    return fail;
}