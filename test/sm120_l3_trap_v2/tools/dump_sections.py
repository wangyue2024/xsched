#!/usr/bin/env python3
"""Dump raw section bytes from an extracted CUDA plugin object.

nvdisasm cannot walk the ET_REL plugin directly (unresolved local branches),
but it can decode a raw instruction stream with `-b SM120a`.  This tool splits
the plugin object into per-section .bin files so each can be decoded that way.

Usage: dump_sections.py plugin_120_0.o --outdir sections
"""
import argparse
import os
import struct
import sys


def sections(buf):
    e_shoff = struct.unpack_from("<Q", buf, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", buf, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", buf, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", buf, 0x3E)[0]

    def shdr(i):
        b = e_shoff + i * e_shentsize
        f = struct.unpack_from("<IIQQQQIIQQ", buf, b)
        return {"idx": i, "name_off": f[0], "type": f[1], "offset": f[4],
                "size": f[5], "entsize": f[9]}

    shstr = shdr(e_shstrndx)
    strtab = buf[shstr["offset"]: shstr["offset"] + shstr["size"]]
    out = []
    for i in range(e_shnum):
        h = shdr(i)
        end = strtab.find(b"\0", h["name_off"])
        h["name"] = strtab[h["name_off"]:end].decode("utf8", "replace")
        out.append(h)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("obj")
    ap.add_argument("--outdir", default="sections")
    ap.add_argument("--only", default=None, help="comma separated section substrings")
    args = ap.parse_args()

    buf = open(args.obj, "rb").read()
    os.makedirs(args.outdir, exist_ok=True)
    tag = os.path.basename(args.obj).replace(".o", "")
    want = args.only.split(",") if args.only else None
    n = 0
    for s in sections(buf):
        if s["type"] != 1 or not s["name"].startswith(".text."):
            continue
        if want and not any(w in s["name"] for w in want):
            continue
        data = buf[s["offset"]: s["offset"] + s["size"]]
        out = os.path.join(args.outdir, f"{tag}{s['name'][5:]}.bin")
        open(out, "wb").write(data)
        print(f"{out}  size={len(data)}  instrs={len(data)//16}")
        n += 1
    return 0 if n else 1


if __name__ == "__main__":
    sys.exit(main())
