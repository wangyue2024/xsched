/*
 * probe_common.h - shared scaffolding for the sm120 (Blackwell) L3
 *                  Trap-Handler / Export-Table reverse-engineering probes.
 *
 * Design goals
 * ------------
 *   1. Self-contained: the CUDA driver API surface we touch is declared
 *      by hand (typedefs + function-pointer types). We deliberately do
 *      NOT #include <cuda.h> so the probe never silently depends on a
 *      particular CUDA Toolkit header revision; the driver under test is
 *      resolved at run time from nvcuda.dll (see CUXTRA_CUDA_LIB).
 *   2. cuxtra interop: the closed-source static library already typedefs
 *      the object handles (CUcontext/CUfunction/CUstream/CUdeviceptr...).
 *      Those identical typedefs are re-stated here BEFORE including
 *      <cuxtra/cuxtra.h>; in C++ a repeated identical typedef is legal,
 *      so there is no ODR/redefinition clash.
 *   3. Fault tolerance: reverse engineering an undocumented export table
 *      means calling into code paths that may raise a Win32 SEH exception
 *      (access violation, illegal instruction). SAFE_CALL() wraps such a
 *      call in a VEH (vectored exception handler) guard, so one bad ETID
 *      cannot kill the whole probe run; the fault is logged and execution
 *      continues.
 *
 * Toolchain: MinGW-w64 g++ (SEH flavour), matching the xsched host build
 *            used by test/sm120_l2_probe/build.ps1.
 */
#ifndef PROBE_COMMON_H_
#define PROBE_COMMON_H_

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

/* ================================================================== */
/*  Minimal CUDA driver API types (hand declared, no <cuda.h>)         */
/* ================================================================== */

typedef int                CUresult;    /* enum CUresult_enum, 0 == success  */
typedef int                CUdevice;    /* device ordinal                    */
typedef struct CUctx_st   *CUcontext;   /* matches cuxtra.h                  */
typedef struct CUmod_st   *CUmodule;    /* not present in cuxtra.h           */
typedef struct CUfunc_st  *CUfunction;  /* matches cuxtra.h                  */
typedef struct CUkern_st  *CUkernel;    /* matches cuxtra.h                  */
typedef struct CUstream_st *CUstream;   /* matches cuxtra.h                  */
typedef unsigned long long CUdeviceptr; /* matches cuxtra.h                  */

/* 16-byte table identifier passed to cuGetExportTable (the "ETID"). */
typedef struct CUuuid_st {
    char bytes[16];
} CUuuid;

/* CUresult values we actually branch on. */
#define CUDA_SUCCESS                    0
#define CUDA_ERROR_INVALID_VALUE        1
#define CUDA_ERROR_OUT_OF_MEMORY        2
#define CUDA_ERROR_NOT_INITIALIZED      3
#define CUDA_ERROR_DEINITIALIZED        4
#define CUDA_ERROR_INVALID_CONTEXT      201
#define CUDA_ERROR_INVALID_HANDLE       400
#define CUDA_ERROR_NOT_FOUND            500
#define CUDA_ERROR_NOT_SUPPORTED        801

/* Device attributes used to confirm we are on sm120 (CC 12.0). */
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR 75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR 76
#define CU_DEVICE_ATTRIBUTE_NAME                     0

/* Function-pointer types for the driver entry points we load dynamically
 * from nvcuda.dll via GetProcAddress (avoids linking cudart and keeps the
 * driver-under-test unambiguous). */
typedef CUresult (*pfn_cuInit)(unsigned int Flags);
typedef CUresult (*pfn_cuDriverGetVersion)(int *driverVersion);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice *device, int ordinal);
typedef CUresult (*pfn_cuDeviceGetCount)(int *count);
typedef CUresult (*pfn_cuDeviceGetAttribute)(int *pi, int attrib, CUdevice dev);
typedef CUresult (*pfn_cuDeviceGetName)(char *name, int len, CUdevice dev);
typedef CUresult (*pfn_cuCtxCreate_v2)(CUcontext *pctx, unsigned int flags, CUdevice dev);
typedef CUresult (*pfn_cuCtxGetCurrent)(CUcontext *pctx);
typedef CUresult (*pfn_cuCtxDestroy_v2)(CUcontext ctx);
typedef CUresult (*pfn_cuMemAlloc_v2)(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult (*pfn_cuMemFree_v2)(CUdeviceptr dptr);
/* The undocumented hook every L3 path funnels through. */
typedef CUresult (*pfn_cuGetExportTable)(const void **ppExportTable, const CUuuid *pExportTableId);

/* ================================================================== */
/*  cuxtra API declarations                                            */
/* ================================================================== */
#include <cuxtra/cuxtra.h>

/* ================================================================== */
/*  Logging                                                            */
/* ================================================================== */

/* Monotonic seconds since first probe_elapsed_sec() call. */
static inline double probe_elapsed_sec(void) {
    static LARGE_INTEGER freq = {};
    static LARGE_INTEGER t0   = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
}

/* LOG(fmt, ...) - timestamped, immediately-flushed trace so a hard crash
 * still leaves the tail on disk (run.ps1 tees the console into evidence/). */
#define LOG(fmt, ...)                                                          \
    do {                                                                       \
        std::fprintf(stdout, "[%8.3fs] " fmt "\n",                             \
                     probe_elapsed_sec(), ##__VA_ARGS__);                      \
        std::fflush(stdout);                                                   \
    } while (0)

/* LOGRAW - no timestamp decoration, for tabular dumps. */
#define LOGRAW(fmt, ...)                                                       \
    do {                                                                       \
        std::fprintf(stdout, fmt, ##__VA_ARGS__);                              \
        std::fflush(stdout);                                                   \
    } while (0)

/* ================================================================== */
/*  Error checking                                                     */
/* ================================================================== */

/* Human-readable name for the CUresult codes we care about. */
static inline const char *probe_cu_err_str(CUresult r) {
    switch (r) {
        case CUDA_SUCCESS:               return "CUDA_SUCCESS";
        case CUDA_ERROR_INVALID_VALUE:   return "CUDA_ERROR_INVALID_VALUE";
        case CUDA_ERROR_OUT_OF_MEMORY:   return "CUDA_ERROR_OUT_OF_MEMORY";
        case CUDA_ERROR_NOT_INITIALIZED: return "CUDA_ERROR_NOT_INITIALIZED";
        case CUDA_ERROR_DEINITIALIZED:   return "CUDA_ERROR_DEINITIALIZED";
        case CUDA_ERROR_INVALID_CONTEXT: return "CUDA_ERROR_INVALID_CONTEXT";
        case CUDA_ERROR_INVALID_HANDLE:  return "CUDA_ERROR_INVALID_HANDLE";
        case CUDA_ERROR_NOT_FOUND:       return "CUDA_ERROR_NOT_FOUND";
        case CUDA_ERROR_NOT_SUPPORTED:   return "CUDA_ERROR_NOT_SUPPORTED";
        default:                         return "CUDA_ERROR_<unknown>";
    }
}

/* CHECK_CU(expr) - evaluate a CUresult expression; on non-success log the
 * code and return it from the enclosing CUresult-returning function. */
#define CHECK_CU(expr)                                                         \
    do {                                                                       \
        CUresult _r = (expr);                                                  \
        if (_r != CUDA_SUCCESS) {                                              \
            LOG("[CU-ERR] %s -> %d (%s)", #expr, _r,                           \
                probe_cu_err_str(_r));                                         \
            return _r;                                                         \
        }                                                                      \
    } while (0)

/* CHECK_CU_VOID(expr) - same, but logs and continues (void callers). */
#define CHECK_CU_VOID(expr)                                                    \
    do {                                                                       \
        CUresult _r = (expr);                                                  \
        if (_r != CUDA_SUCCESS) {                                              \
            LOG("[CU-ERR] %s -> %d (%s)", #expr, _r,                           \
                probe_cu_err_str(_r));                                         \
        }                                                                      \
    } while (0)

/* ================================================================== */
/*  SEH protection (VEH based - this MinGW g++ lacks __try/__except)   */
/* ================================================================== */

/* This toolchain (x86_64-win32-seh, MinGW-Builds g++ 14.2) rejects the
 * MSVC SEH keywords __try/__except in C++ AND in C ("not declared in this
 * scope"), even with -fms-extensions. We therefore deliver the same
 * guarantee - survive a fault raised inside the driver or an export-table
 * thunk - with a Vectored Exception Handler that longjmp()s back to a
 * leaf guard frame.
 *
 * Why a template guard (probe_seh_guard) and not a bare setjmp macro:
 *   the setjmp() lives in probe_seh_guard's OWN frame, never in the
 *   caller's. So (a) the caller's locals are not "clobbered" by longjmp
 *   (no -Wclobbered), and (b) after a fault probe_seh_guard simply
 *   returns PROBE_SEH_RAISED and the caller keeps running normally.
 *
 * Flow: the VEH is registered once by probe_seh_install(). While a guard
 * is in flight probe_seh_armed != 0; any exception on this thread has its
 * Win32 code stashed in probe_last_seh_code and longjmp()s back. When not
 * armed the handler declines (EXCEPTION_CONTINUE_SEARCH) so debuggers and
 * other handlers keep working.
 *
 * CAVEAT (identical to real SEH): the guarded body runs on the same stack,
 * so a longjmp skips C++ destructors of objects created INSIDE the body.
 * Keep the body a thin call; do the bookkeeping in the caller. Verified
 * end-to-end on this toolchain (see src/probe_selftest). */

#define PROBE_SEH_RAISED ((CUresult)0x7FFFFFFF)

#include <csetjmp>

static jmp_buf       probe_seh_jb;
static volatile LONG probe_seh_armed     = 0;
static DWORD         probe_last_seh_code = 0;                 /* last code */
static CUresult      probe_safe_result   = PROBE_SEH_RAISED;   /* last rc   */

static LONG CALLBACK probe_seh_veh(EXCEPTION_POINTERS *ep) {
    if (!probe_seh_armed) return EXCEPTION_CONTINUE_SEARCH;
    probe_last_seh_code = (ep && ep->ExceptionRecord)
                              ? ep->ExceptionRecord->ExceptionCode
                              : 0xFFFFFFFFu;
    probe_seh_armed = 0;
    longjmp(probe_seh_jb, 1);
    return EXCEPTION_CONTINUE_SEARCH;   /* unreachable */
}

static inline void probe_seh_install(void) {
    static volatile LONG installed = 0;
    if (InterlockedCompareExchange(&installed, 1, 0) == 0)
        AddVectoredExceptionHandler(1, probe_seh_veh);
}

/* probe_seh_guard(body) - run body() (a callable returning CUresult) under
 * SEH protection; yields its CUresult, or PROBE_SEH_RAISED if it faulted.
 * Also mirrors the outcome into probe_safe_result / probe_last_seh_code. */
template <class ProbeBody>
static inline CUresult probe_seh_guard(ProbeBody body) {
    probe_seh_install();
    probe_last_seh_code = 0;
    probe_safe_result   = PROBE_SEH_RAISED;
    if (setjmp(probe_seh_jb) == 0) {
        probe_seh_armed   = 1;
        probe_safe_result = body();
        probe_seh_armed   = 0;
    } else {
        probe_seh_armed   = 0;
        probe_safe_result = PROBE_SEH_RAISED;
        LOG("[SEH] guarded call raised exception code=0x%08lx",
            (unsigned long)probe_last_seh_code);
    }
    return probe_safe_result;
}

/* SAFE_CALL(func, ...) - expression form. Invokes func(__VA_ARGS__) under
 * SEH protection and evaluates to its CUresult (PROBE_SEH_RAISED on fault).
 *   CUresult rc = SAFE_CALL(cuGetExportTable, &tbl, &etid);
 * The raw Win32 code (0 when clean) stays in probe_last_seh_code. */
#define SAFE_CALL(func, ...)                                                   \
    probe_seh_guard([&]() -> CUresult { return (func)(__VA_ARGS__); })

/* SAFE_CALL_TO(_rc, func, ...) - statement form storing into CUresult _rc. */
#define SAFE_CALL_TO(_rc, func, ...)                                           \
    do { (_rc) = SAFE_CALL(func, __VA_ARGS__); } while (0)

/* ================================================================== */
/*  GPU device-memory address-range classification                     */
/* ================================================================== */

/* Under WDDM + unified virtual addressing, host and device pointers share
 * one 64-bit VA space, so "is this a device pointer?" cannot be answered
 * from the value alone. The robust method is empirical: the probe records
 * every range it got from cuMemAlloc (device) / cuMemHostAlloc (pinned),
 * then classifies a queried address by membership, with a non-canonical
 * hole check as fallback. */

#define PROBE_MAX_RANGE 64

typedef enum {
    PROBE_ADDR_NULL = 0,   /* 0                                     */
    PROBE_ADDR_HOST,       /* observed pinned-host allocation       */
    PROBE_ADDR_DEVICE,     /* observed device allocation            */
    PROBE_ADDR_RESERVED,   /* non-canonical hole / obviously bogus  */
    PROBE_ADDR_UNKNOWN     /* not observed, heuristic inconclusive  */
} ProbeAddrClass;

typedef struct {
    CUdeviceptr base;
    CUdeviceptr end;      /* base + size, exclusive */
    int         kind;     /* 1 = device, 2 = host   */
} ProbeRange;

static ProbeRange probe_ranges[PROBE_MAX_RANGE];
static int        probe_range_count = 0;

static inline void probe_range_observe(CUdeviceptr base, size_t size, int kind) {
    if (probe_range_count >= PROBE_MAX_RANGE || size == 0) return;
    probe_ranges[probe_range_count].base = base;
    probe_ranges[probe_range_count].end  = base + (CUdeviceptr)size;
    probe_ranges[probe_range_count].kind = kind;
    ++probe_range_count;
}
static inline void probe_range_observe_device(CUdeviceptr base, size_t size) {
    probe_range_observe(base, size, 1);
}
static inline void probe_range_observe_host(CUdeviceptr base, size_t size) {
    probe_range_observe(base, size, 2);
}

static inline ProbeAddrClass probe_addr_classify(CUdeviceptr addr) {
    if (addr == 0) return PROBE_ADDR_NULL;
    for (int i = 0; i < probe_range_count; ++i) {
        if (addr >= probe_ranges[i].base && addr < probe_ranges[i].end) {
            return probe_ranges[i].kind == 1 ? PROBE_ADDR_DEVICE
                                             : PROBE_ADDR_HOST;
        }
    }
    {   /* x86-64 non-canonical hole: bits 48..63 must sign-extend bit 47. */
        uint64_t hi  = (uint64_t)addr >> 48;
        uint64_t b47 = ((uint64_t)addr >> 47) & 1u;
        uint64_t exp = b47 ? 0xFFFFu : 0x0000u;
        if (hi != exp) return PROBE_ADDR_RESERVED;
    }
    return PROBE_ADDR_UNKNOWN;
}

static inline int probe_addr_is_device(CUdeviceptr addr) {
    return probe_addr_classify(addr) == PROBE_ADDR_DEVICE;
}

static inline const char *probe_addr_class_str(ProbeAddrClass c) {
    switch (c) {
        case PROBE_ADDR_NULL:     return "NULL";
        case PROBE_ADDR_HOST:     return "HOST(pinned)";
        case PROBE_ADDR_DEVICE:   return "DEVICE";
        case PROBE_ADDR_RESERVED: return "RESERVED/non-canonical";
        default:                  return "UNKNOWN";
    }
}

/* ================================================================== */
/*  Small utilities                                                    */
/* ================================================================== */

/* Format a CUuuid as canonical 8-4-4-4-12 GUID text (buf >= 37 bytes). */
static inline void probe_uuid_to_str(const CUuuid *u, char *buf /*[37]*/) {
    const unsigned char *b = (const unsigned char *)u->bytes;
    std::snprintf(buf, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/* Dump len bytes at p as hex + ASCII, width bytes per line. */
static inline void probe_hexdump(const void *p, size_t len, size_t width) {
    const unsigned char *b = (const unsigned char *)p;
    if (width == 0) width = 16;
    for (size_t off = 0; off < len; off += width) {
        LOGRAW("  %08zx  ", off);
        for (size_t i = 0; i < width; ++i) {
            if (off + i < len) LOGRAW("%02x ", b[off + i]);
            else               LOGRAW("   ");
        }
        LOGRAW(" |");
        for (size_t i = 0; i < width && off + i < len; ++i) {
            unsigned char ch = b[off + i];
            LOGRAW("%c", (ch >= 0x20 && ch < 0x7f) ? ch : '.');
        }
        LOGRAW("|\n");
    }
    std::fflush(stdout);
}

#endif /* PROBE_COMMON_H_ */
