/*
 * probe_kernels.cu - device-side kernels for the cuxtra sm120 probe (T1).
 *
 * Build:
 *   nvcc -cubin probe_kernels.cu -o probe_kernels.cubin -arch=sm_120
 *
 * Contents:
 *   kernel_marker_a   simple kernel, writes 0xA... sentinel to out[0]
 *   kernel_marker_b   simple kernel, writes 0xB... sentinel to out[0]
 *   kernel_read_params
 *                     reads constant-bank cells and stores the raw values
 *                     into `out` (u64 view).  The body consists of 127
 *                     `brkpt` placeholders that are rewritten 1:1
 *                     (16 bytes -> 16 bytes) by patch_cubin.py into real
 *                     SASS (LDC / LDC.64 / ST.E) instructions.
 *
 *                     Slot protocol (must match patch_cubin.py exactly):
 *                         slot   0: LDC    R2, c[0x0][0x380]   out ptr lo
 *                         slot   1: LDC    R3, c[0x0][0x384]   out ptr hi
 *                         slot 2..125: 62 x ( LDC.64 R4, c[0x0][off_i];
 *                                             ST.E.64 [R2+8*i], R4 )
 *                         slot 126: NOP pad
 *
 *                     Default (P1) sample points (8 bytes each):
 *                         out u64[0] @ 0x170  preempt buffer addr
 *                                             (sm120 window - VERIFIED
 *                                              by the full cbank sweep)
 *                         out u64[1] @ 0x178  guardian entry
 *                         out u64[2] @ 0x180  kernel idx
 *                         out u64[3] @ 0x188  killable (low 32 bits)
 *                         out u64[4] @ 0x168  pre-window negative control
 *
 *                     Scan mode (patch_cubin.py --scan BASE STEP COUNT,
 *                     STEP = point stride in bytes) re-points the same 62
 *                     slots at BASE + i*STEP, i in [0, COUNT), so one
 *                     kernel launch dumps 62 consecutive u64 samples
 *                     (496 bytes) of the constant bank - the workhorse for
 *                     the exhaustive sm120 debugger-window search.
 *
 *   ref_use_param     reference kernel used to learn the constant-bank
 *                     offset of kernel parameters and the ST.E.64
 *                     encoding emitted by ptxas for sm_120.  PARAM_OFF:
 *                     the sm120 ABI loads the first kernel parameter from
 *                     c[0x0][0x380] (verified from the ref_use_param SASS;
 *                     the sm86 ABI used 0x210).
 */

#include <cstdint>

extern "C" __global__ void kernel_marker_a(uint64_t *out)
{
    out[0] = 0xAAAAAAAAAAAAAAAAull;
}

extern "C" __global__ void kernel_marker_b(uint64_t *out)
{
    out[0] = 0xBBBBBBBBBBBBBBBBull;
}

/* Compact spelling of the 127-slot placeholder block. */
#define PB1 "brkpt;\n\t"
#define PB2 PB1 PB1
#define PB4 PB2 PB2
#define PB8 PB4 PB4
#define PB16 PB8 PB8
#define PB32 PB16 PB16
#define PB64 PB32 PB32

extern "C" __global__ void kernel_read_params(uint64_t *out)
{
    /*
     * All real semantics are injected by patch_cubin.py (fixed registers
     * R2..R5).  The `"l"(out)` references pin the `out` kernel parameter
     * in the constant bank across the placeholder block so the patched
     * stores have a valid base pointer.
     */
    asm volatile("brkpt;" ::"l"(out) : "memory"); // slot 0
    asm volatile(
        PB64 PB32 PB16 PB8 PB4 PB2 // slots 1..126 (126 placeholders)
        ::"l"(out)
        : "memory");
    /* total: 1 + 126 = 127 placeholders (2 param slots + 62 sample-point
     * slot pairs + 1 NOP pad after patching) */
}

extern "C" __global__ void ref_use_param(uint64_t *out)
{
    /* Natural codegen: reveals the constant-bank offset of `out` and the
     * ST.E.64 form ptxas uses on sm_120. */
    out[1] = 0x1234567890ABCDEFull;
}
