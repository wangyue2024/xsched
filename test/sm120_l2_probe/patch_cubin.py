#!/usr/bin/env python3
# =====================================================================
#  patch_cubin.py - T1 probe static binary patch (OS independent)
#
#  Rewrites the 127 `brkpt` (BPT.TRAP) placeholders inside the
#  kernel_read_params text section of probe_kernels.cubin into real
#  SASS (LDC / LDC.64 / ST.E.64) instructions, 16 bytes -> 16 bytes,
#  in place.
#
#  Encodings derived from XSched inject evidence (cross-checked against
#  platforms/cuda/hal/inject/inject_sm86.asm and the runtime arrays in
#  platforms/cuda/hal/src/arch/sm86.cpp) plus the sm_120 ptxas reference
#  dump (test/sm120_l2_probe/probe_kernels.sass.txt):
#
#    LDC     Rx, c[0x0][off]  word0 = 0xff007b82 | (reg << 16)
#                                   | ((off >> 2) << 40)
#                             word1 = 0x000fc00000000800
#    LDC.64  Rx, c[0x0][off]  word0 = same structure (reg must be even,
#                                   pair is Rx:Rx+1)
#                             word1 = 0x000fc00000000a00  (bit 9 set)
#                             (ptxas reference: LDC.64 R2, c[0x0][0x380] ->
#                              w1 = 0x000e220000000a00: same low bits,
#                              high bits are scheduling control)
#    ST.E.64 [Rb+imm], Rp     word0 = 0x7385 | (base << 24) | (imm << 32)
#                             word1 = 0x000fe20000100b00 | Rp
#
#  Every generated word is decoded back and checked against the
#  requested operands (round-trip) before anything is written.
#  The script aborts without writing output if the placeholder count or
#  their location does not match the expected protocol exactly.
#
#  Usage:
#    python patch_cubin.py <in.cubin> <out.cubin>
#        standard 127-slot protocol (P1 sample points, see PROTOCOL)
#
#    python patch_cubin.py <in.cubin> <out.cubin> --scan BASE STEP [COUNT]
#        scan protocol: sample COUNT (<=62) u64 constant-bank cells
#        starting at BASE with point stride STEP bytes and store them
#        into out u64[0..] (the sm120 debugger-window search workhorse).
#        STEP=0x8, COUNT=62 -> 496 bytes of contiguous cbank per batch.
# =====================================================================

import hashlib
import struct
import sys

BPT_TRAP   = struct.pack("<QQ", 0x000000040000795c, 0x000fea0000300000)
TEXT_SECTION = ".text.kernel_read_params"
PARAM_OFF  = 0x380   # sm120 first-kernel-parameter offset (SASS evidence)

# Default P1 sample points (8-byte samples).
#  * 0x170..0x188: sm120 debugger-parameters window - VERIFIED by the
#    0x0-batch cbank sweep (all four sentinel values landed exactly
#    here; the sm86 ABI used 0x1880)
#  * 0x168: pre-window negative control (must not contain sentinels)
DEFAULT_POINTS = [0x170, 0x178, 0x180, 0x188, 0x168]

MAX_SLOTS  = 127
MAX_POINTS = (MAX_SLOTS - 3) // 2    # 2 param slots + >=1 NOP pad -> 62

LDC_W1    = 0x000fc00000000800
LDC64_W1  = 0x000fc00000000a00       # bit 9 = 1 marks the 64-bit form
STE64_W1  = 0x000fe20000100b00
STE32_W1  = 0x000fe20000100900


def points_protocol(points):
    """2 param slots + len(points) x (LDC.64 + ST.E.64), NOP-padded."""
    assert 1 <= len(points) <= MAX_POINTS, \
        "point count must be within 1..%d" % MAX_POINTS
    p = [("LDC", 2, PARAM_OFF), ("LDC", 3, PARAM_OFF + 0x4)]
    for i, off in enumerate(points):
        p.append(("LDC64", 4, off))      # reads 8 bytes c[0x0][off..off+8)
        p.append(("STE64", 2, 8 * i, 4)) # -> out u64[i]
    while len(p) < MAX_SLOTS:
        p.append(("NOP",))
    return p


def scan_protocol(base, step, count):
    """Scan protocol: count sample points spaced `step` bytes apart."""
    return points_protocol([base + i * step for i in range(count)])


# --------------------------- encoders --------------------------------

def enc_ldc(reg, off):
    w0 = 0xff007b82 | (reg << 16) | ((off >> 2) << 40)
    return w0, LDC_W1


def enc_ldc64(reg, off):
    w0 = 0xff007b82 | (reg << 16) | ((off >> 2) << 40)
    return w0, LDC64_W1


def enc_ste64(base, imm, r_pair):
    w0 = 0x7385 | (base << 24) | (imm << 32)
    return w0, STE64_W1 | r_pair


def enc_ste32(base, imm, reg):
    w0 = 0x7385 | (base << 24) | (imm << 32)
    return w0, STE32_W1 | reg


def encode(slot):
    if slot[0] == "LDC":
        return enc_ldc(slot[1], slot[2])
    if slot[0] == "LDC64":
        return enc_ldc64(slot[1], slot[2])
    if slot[0] == "STE64":
        return enc_ste64(slot[1], slot[2], slot[3])
    if slot[0] == "STE32":
        return enc_ste32(slot[1], slot[2], slot[3])
    if slot[0] == "NOP":
        return 0x0000000000007918, 0x000fc00000000000
    raise ValueError("unknown slot kind: %r" % (slot,))


def check_roundtrip(slot, w0, w1):
    """Decode the generated words and verify they match the request."""
    if slot[0] in ("LDC", "LDC64"):
        _, reg, off = slot
        assert off % 4 == 0, "LDC offset must be 4-byte aligned"
        assert (w0 & 0xffff) == 0x7b82, "bad LDC opcode"
        assert (w0 >> 16) & 0xff == reg, "bad LDC reg"
        assert (w0 >> 40) & 0xffffff == (off >> 2), "bad LDC offset"
        if slot[0] == "LDC64":
            assert reg % 2 == 0, "LDC.64 register must be even"
            assert w1 == LDC64_W1, "bad LDC.64 control word"
        else:
            assert w1 == LDC_W1, "bad LDC control word"
    elif slot[0] in ("STE64", "STE32"):
        _, base, imm, r = slot
        assert (w0 & 0xffff) == 0x7385, "bad ST.E opcode"
        assert (w0 >> 24) & 0xff == base, "bad ST.E base"
        assert (w0 >> 32) & 0xffffffff == imm, "bad ST.E imm"
        expect = (STE64_W1 if slot[0] == "STE64" else STE32_W1) | r
        assert w1 == expect, "bad ST.E control word"
    elif slot[0] == "NOP":
        assert (w0, w1) == (0x0000000000007918, 0x000fc00000000000), \
            "bad NOP encoding"
    else:
        raise ValueError(slot)


def describe(slot):
    if slot[0] == "LDC":
        return "LDC R%d, c[0x0][0x%x]" % (slot[1], slot[2])
    if slot[0] == "LDC64":
        return "LDC.64 R%d, c[0x0][0x%x]" % (slot[1], slot[2])
    if slot[0] == "NOP":
        return "NOP"
    w = "64" if slot[0] == "STE64" else ""
    return "ST.E.%s [R%d+0x%x], R%d" % (w, slot[1], slot[2], slot[3])


# ------------------------- ELF64 utilities ---------------------------

def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def find_section(data, name):
    """Return (sh_offset, sh_size) of a named section, or None."""
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise ValueError("not an ELF64 file")
    e_shoff, e_shentsize = u64(data, 0x28), u16(data, 0x3a)
    e_shnum, e_shstrndx = u16(data, 0x3c), u16(data, 0x3e)
    sh = lambda i: e_shoff + i * e_shentsize
    shstr = u64(data, sh(e_shstrndx) + 24)
    for i in range(e_shnum):
        noff = u32(data, sh(i))
        end = data.index(b"\x00", shstr + noff)
        sname = data[shstr + noff:end].decode("utf-8", "replace")
        if sname == name:
            return u64(data, sh(i) + 24), u64(data, sh(i) + 32)
    return None


def find_all(data, pattern, start=0, end=None):
    if end is None:
        end = len(data)
    out, i = [], data.find(pattern, start, end)
    while i != -1:
        out.append(i)
        i = data.find(pattern, i + 1, end)
    return out


# ------------------------------ main ---------------------------------

def main():
    args = sys.argv[1:]
    scan = None
    if "--scan" in args:
        i = args.index("--scan")
        try:
            base = int(args[i + 1], 0)
            step = int(args[i + 2], 0)
            count = int(args[i + 3], 0) if len(args) > i + 3 and \
                not args[i + 3].startswith("-") else MAX_POINTS
        except (IndexError, ValueError):
            print("usage: --scan BASE STEP [COUNT]  (0x-prefixed hex accepted)")
            return 2
        scan = (base, step, count)
        args = args[:i] + args[i + (4 if len(args) > i + 3 and
                                    not args[i + 3].startswith("-") else 3):]

    if len(args) != 2:
        print("usage: python patch_cubin.py <in.cubin> <out.cubin>"
              " [--scan BASE STEP [COUNT]]")
        return 2

    protocol = points_protocol(DEFAULT_POINTS) if scan is None \
        else scan_protocol(*scan)
    if len(protocol) > MAX_SLOTS:
        print("ABORT: protocol needs %d slots, max %d" %
              (len(protocol), MAX_SLOTS))
        return 1

    src, dst = args[0], args[1]
    data = bytearray(open(src, "rb").read())
    print("input   : %s" % src)
    print("sha256  : %s" % hashlib.sha256(data).hexdigest())
    if scan is not None:
        print("mode    : scan base=0x%x step=0x%x count=%d (span 0x%x bytes)"
              % (scan[0], scan[1], scan[2], scan[1] * scan[2]))

    sec = find_section(data, TEXT_SECTION)
    if sec is None:
        print("WARNING : section %s not found, using whole file" % TEXT_SECTION)
        sec_off, sec_end = 0, len(data)
    else:
        sec_off, sec_size = sec
        sec_end = sec_off + sec_size
        print("section : %s @ file 0x%x size 0x%x" %
              (TEXT_SECTION, sec_off, sec_size))

    hits = find_all(data, BPT_TRAP, 0, len(data))
    print("scan    : %d BPT.TRAP placeholder(s) found (expect %d)" %
          (len(hits), len(protocol)))
    if len(hits) != len(protocol):
        print("ABORT   : placeholder count mismatch, nothing written")
        return 1
    outside = [h for h in hits if not (sec_off <= h < sec_end)]
    if outside:
        print("ABORT   : placeholder(s) outside %s: %s" %
              (TEXT_SECTION, [hex(x) for x in outside]))
        return 1

    print("")
    print("slot  file-off   before                            after"
          "                             instruction")
    nops = []
    for idx, (slot, pos) in enumerate(zip(protocol, hits)):
        w0, w1 = encode(slot)
        check_roundtrip(slot, w0, w1)
        old = struct.unpack_from("<QQ", data, pos)
        data[pos:pos + 16] = struct.pack("<QQ", w0, w1)
        if slot[0] == "NOP":
            nops.append(idx)
        else:
            print("%3d   0x%06x  %016x %016x  %016x %016x  %s" %
                  (idx, pos, old[0], old[1], w0, w1, describe(slot)))
    if nops:
        print("      slots %d..%d: NOP pad (%d slots)" %
              (nops[0], nops[-1], len(nops)))

    # ---- post-write verification ------------------------------------
    remain = find_all(data, BPT_TRAP, 0, len(data))
    print("")
    print("verify  : %d BPT.TRAP remaining in patched image (expect 0)" %
          len(remain))
    if remain:
        return 1
    for idx, (slot, pos) in enumerate(zip(protocol, hits)):
        w0, w1 = struct.unpack_from("<QQ", data, pos)
        check_roundtrip(slot, w0, w1)

    with open(dst, "wb") as f:
        f.write(data)
    print("output  : %s" % dst)
    print("sha256  : %s" % hashlib.sha256(data).hexdigest())
    return 0


if __name__ == "__main__":
    sys.exit(main())
