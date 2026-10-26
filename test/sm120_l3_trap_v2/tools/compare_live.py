#!/usr/bin/env python3
"""Compare a live (installed) device image against the libcuda plugin section.

The installed image is byte-identical to the relocatable plugin section except at
the slots the driver patched with absolute addresses.  For each live dump this
tool finds the best matching plugin section, then prints every differing
instruction with the decoded 64-bit immediate (MOV lo/hi pairs) and the
relocation symbol that occupies that offset.

Usage:
  compare_live.py --live evidence/live --sections evidence/sections \
                  --relocs evidence [--arch 120]
"""
import argparse
import glob
import os
import re
import struct
import sys


def load(path):
    return open(path, "rb").read()


def opcode(b, i):
    return struct.unpack_from("<I", b, i * 16)[0]


def imm32(b, i):
    return struct.unpack_from("<I", b, i * 16 + 4)[0]


def load_relocs(reloc_dir, arch, variant):
    p = os.path.join(reloc_dir, f"plugin_{arch}_{variant}_relocs.txt")
    out, sec = {}, None
    if not os.path.exists(p):
        return out
    for line in open(p):
        if line.startswith("##"):
            sec = line.split()[1]
            out.setdefault(sec, {})
        elif sec and line.strip():
            f = line.split()
            out[sec][int(f[0], 16)] = f[2].split("=")[1]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--live", default="evidence/live")
    ap.add_argument("--sections", default="evidence/sections")
    ap.add_argument("--relocs", default="evidence")
    ap.add_argument("--arch", default="120")
    ap.add_argument("--max-report", type=int, default=64)
    args = ap.parse_args()

    secs = {}
    for p in sorted(glob.glob(os.path.join(args.sections, f"plugin_{args.arch}_*.bin"))):
        b = os.path.basename(p)[:-4]
        m = re.match(rf"plugin_{args.arch}_(\d+)\.(.+)", b)
        if m:
            secs[(m.group(1), m.group(2))] = load(p)
    relocs = {v: load_relocs(args.relocs, args.arch, v) for v in {k[0] for k in secs}}

    for lp in sorted(glob.glob(os.path.join(args.live, "obj_*.bin"))):
        live = load(lp)
        best = None
        for (variant, name), sb in secs.items():
            n = min(len(live), len(sb)) // 16
            if n < 8:
                continue
            d = sum(1 for i in range(n) if opcode(live, i) != opcode(sb, i))
            if best is None or d < best[0]:
                best = (d, n, variant, name, sb)
        if not best:
            continue
        d, n, variant, name, sb = best
        print(f"\n=== {os.path.basename(lp)}  ({len(live)} bytes)")
        print(f"    best match: plugin_{args.arch}_{variant}.text.{name} "
              f"({len(sb)} bytes)  opcode diffs={d}/{n}")
        if d > n * 0.2:
            print("    (weak match - skipping reloc dump)")
            continue
        rel = relocs[variant].get(".text." + name, {})
        shown = 0
        for i in range(n):
            if live[i * 16: i * 16 + 16] == sb[i * 16: i * 16 + 16]:
                continue
            off = i * 16
            sym = rel.get(off, "")
            li, pi = imm32(live, i), imm32(sb, i)
            pair = ""
            if off + 16 < len(live) and (off + 16) in rel or rel.get(off + 16):
                pair = f" -> next imm {imm32(live, i + 1):#x}"
            print(f"    +{off:#07x} {sym:38s} live_imm={li:#010x} plugin_imm={pi:#010x}"
                  f"  live={live[off:off+16].hex()} plugin={sb[off:off+16].hex()}")
            shown += 1
            if shown >= args.max_report:
                print("    ...")
                break
        # decode MOV lo/hi pairs at the relocation slots
        print("    -- decoded 64-bit addresses --")
        seen = set()
        for off in sorted(rel):
            if off + 32 > len(live):
                continue
            lo, hi = imm32(live, off // 16), imm32(live, off // 16 + 1)
            if hi > 0xFFFF:
                continue
            addr = lo | (hi << 32)
            key = (rel[off], addr)
            if key in seen:
                continue
            seen.add(key)
            print(f"    {rel[off]:38s} @+{off:#07x} = {addr:#016x}")


if __name__ == "__main__":
    sys.exit(main())
