#!/usr/bin/env python3
"""Build device-memory fingerprints for the live sm120 trap-handler plugin.

The plugin sections extracted from libcuda are relocatable; the driver fills in
the absolute addresses before installing them.  A fingerprint must therefore
only cover the relocation-free prefix of a section.  This tool emits, for each
requested section, the leading N *opcode words* (the low 8 bytes of every
16-byte Blackwell instruction) so the runtime scanner can match an installed
image regardless of how the control words / immediates were patched.

Usage:
  make_fingerprints.py --sections evidence/sections --relocs evidence \
                       --outdir evidence/fp --words 8
"""
import argparse
import glob
import os
import re
import sys


def reloc_offsets(reloc_file, section):
    """First relocation offset inside `section` (or None)."""
    cur = None
    for line in open(reloc_file):
        if line.startswith("##"):
            cur = line.split()[1]
        elif cur == section and line.strip():
            m = re.match(r"\s*(0x[0-9a-f]+)", line)
            if m:
                return int(m.group(1), 16)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sections", default="evidence/sections")
    ap.add_argument("--relocs", default="evidence")
    ap.add_argument("--outdir", default="evidence/fp")
    ap.add_argument("--words", type=int, default=8, help="opcode words in the fingerprint")
    ap.add_argument("--only", default="traphandler,do_read_trap_reason")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    want = args.only.split(",")
    manifest = []
    for path in sorted(glob.glob(os.path.join(args.sections, "plugin_*.bin"))):
        base = os.path.basename(path)[:-4]          # plugin_120_0.traphandler
        m = re.match(r"plugin_(\d+)_(\d+)\.(.+)", base)
        if not m:
            continue
        arch, variant, sec = m.group(1), m.group(2), m.group(3)
        if not any(w == sec or w in sec for w in want):
            continue
        data = open(path, "rb").read()
        first_rel = reloc_offsets(
            os.path.join(args.relocs, f"plugin_{arch}_{variant}_relocs.txt"), ".text." + sec)
        limit = len(data)
        if first_rel is not None:
            limit = min(limit, first_rel)
        nwords = min(args.words, limit // 16, len(data) // 16)
        if nwords < 4:
            print(f"skip {base}: only {nwords} relocation-free words", file=sys.stderr)
            continue
        fp = b"".join(data[i * 16: i * 16 + 8] for i in range(nwords))
        out = os.path.join(args.outdir, f"{base}.fp")
        open(out, "wb").write(fp)
        manifest.append((base, out, nwords, first_rel))
        print(f"{out}  words={nwords} bytes={len(fp)} first_reloc={first_rel}")

    with open(os.path.join(args.outdir, "MANIFEST"), "w") as f:
        for base, out, nwords, first_rel in manifest:
            f.write(f"{os.path.basename(out)}\t{base}\t{nwords}\t{first_rel}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
