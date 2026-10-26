#!/usr/bin/env python3
"""Extract the CUDA debugger/CILP SASS plugin objects embedded in libcuda.

libcuda.so ships the *live* trap-handler stack as relocatable EM_CUDA ELF
objects inside its own image: .text.traphandler, .text.do_pause,
.text.do_preempt_save/restore, .text.cilp_traphandler_save/restore, ...
Those are what the driver/debug-agent downloads into device memory, and they
are NOT the same object as the tools export-table (slot 20) handler image.

Usage:
  extract_plugins.py --libcuda /usr/lib/x86_64-linux-gnu/libcuda.so.1 \
                     --arch 120 --outdir evidence
"""
import argparse
import os
import struct
import sys

EM_CUDA = 190
SHT_SYMTAB = 2
SHT_RELA = 4

ARCH_NAMES = {
    0x4b: "sm_75", 0x50: "sm_80", 0x56: "sm_86", 0x57: "sm_87",
    0x58: "sm_88", 0x59: "sm_89", 0x5a: "sm_90", 0x64: "sm_100",
    0x67: "sm_103", 0x6e: "sm_110", 0x78: "sm_120", 0x79: "sm_121",
}


class Elf:
    def __init__(self, buf, base):
        self.buf = buf
        self.base = base
        (self.e_type, self.e_machine) = struct.unpack_from("<HH", buf, base + 16)
        self.e_flags = struct.unpack_from("<I", buf, base + 0x30)[0]
        self.e_shoff = struct.unpack_from("<Q", buf, base + 0x28)[0]
        self.e_shentsize = struct.unpack_from("<H", buf, base + 0x3A)[0]
        self.e_shnum = struct.unpack_from("<H", buf, base + 0x3C)[0]
        self.e_shstrndx = struct.unpack_from("<H", buf, base + 0x3E)[0]
        self.sections = {}
        self.order = []
        if self.e_shnum == 0 or self.e_shnum > 512:
            return
        shstr = self._shdr(self.e_shstrndx)
        strtab = self._slice(shstr["offset"], shstr["size"])
        for i in range(self.e_shnum):
            h = self._shdr(i)
            end = strtab.find(b"\0", h["name_off"])
            h["name"] = strtab[h["name_off"]:end].decode("utf8", "replace")
            self.sections[h["name"]] = h
            self.order.append(h)

    def _shdr(self, i):
        b = self.base + self.e_shoff + i * self.e_shentsize
        f = struct.unpack_from("<IIQQQQIIQQ", self.buf, b)
        return {
            "idx": i, "name_off": f[0], "type": f[1], "flags": f[2],
            "addr": f[3], "offset": f[4], "size": f[5], "link": f[6],
            "info": f[7], "align": f[8], "entsize": f[9],
        }

    def _slice(self, off, size):
        return self.buf[self.base + off: self.base + off + size]

    @property
    def arch_byte(self):
        return (self.e_flags >> 8) & 0xFF

    @property
    def arch(self):
        return ARCH_NAMES.get(self.arch_byte, f"arch_{self.arch_byte:#x}")

    def symbols(self):
        st = self.sections.get(".symtab")
        if not st:
            return []
        strtab_sec = self.order[st["link"]] if st["link"] < len(self.order) else None
        if strtab_sec is None:
            return []
        strtab = self._slice(strtab_sec["offset"], strtab_sec["size"])
        data = self._slice(st["offset"], st["size"])
        out = []
        for off in range(0, len(data) - 23, 24):
            nm, info, other, shndx, val, size = struct.unpack_from("<IBBHQQ", data, off)
            end = strtab.find(b"\0", nm)
            name = strtab[nm:end].decode("utf8", "replace") if nm < len(strtab) else ""
            sec = None
            for s in self.order:
                if s["idx"] == shndx:
                    sec = s["name"]
                    break
            out.append({"name": name, "value": val, "size": size,
                        "bind": info >> 4, "type": info & 0xF,
                        "shndx": shndx, "section": sec})
        return out

    def relocs(self, secname):
        rs = self.sections.get(".rela" + secname)
        if not rs:
            return []
        syms = {i: s for i, s in enumerate(self.symbols())}
        data = self._slice(rs["offset"], rs["size"])
        out = []
        for off in range(0, len(data) - 23, 24):
            r_off, r_info, r_addend = struct.unpack_from("<QQq", data, off)
            sym = syms.get(r_info >> 32, {})
            out.append({"offset": r_off, "type": r_info & 0xFFFFFFFF,
                        "sym": sym.get("name", "?"), "addend": r_addend})
        return out


def scan(path):
    buf = open(path, "rb").read()
    objs = []
    i = buf.find(b"\x7fELF")
    while i != -1:
        try:
            e = Elf(buf, i)
            if e.e_type == 1 and e.e_machine == EM_CUDA and ".text.traphandler" in e.sections:
                objs.append(e)
        except Exception:
            pass
        i = buf.find(b"\x7fELF", i + 1)
    return buf, objs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--libcuda", default="/usr/lib/x86_64-linux-gnu/libcuda.so.1")
    ap.add_argument("--arch", type=int, default=120, help="e.g. 120 for sm_120")
    ap.add_argument("--outdir", default="evidence")
    ap.add_argument("--list-all", action="store_true", help="list every arch variant found")
    args = ap.parse_args()

    path = os.path.realpath(args.libcuda)
    buf, objs = scan(path)
    os.makedirs(args.outdir, exist_ok=True)

    print(f"# libcuda: {path} ({len(buf)} bytes)")
    print(f"# plugin objects with .text.traphandler: {len(objs)}")

    if args.list_all:
        seen = {}
        for e in objs:
            seen.setdefault((e.arch, e.e_flags), []).append(e)
        print(f"\n{'arch':8} {'e_flags':>12} {'count':>5}  traphandler/cilp_save sizes")
        for (arch, flags), lst in sorted(seen.items(), key=lambda kv: kv[0][1]):
            sizes = sorted({(x.sections[".text.traphandler"]["size"],
                             x.sections.get(".text.cilp_traphandler_save",
                                            x.sections.get(".text.cilp_trap_handler_save", {"size": 0}))["size"])
                            for x in lst})
            print(f"{arch:8} {flags:#12x} {len(lst):5d}  {sizes}")

    want = args.arch
    sel = [e for e in objs if e.arch_byte == want]
    if not sel:
        print(f"no sm_{want} plugin objects found", file=sys.stderr)
        return 1
    print(f"\n# sm_{want} objects: {len(sel)}")

    for n, e in enumerate(sel):
        tag = f"{want}_{n}"
        raw = buf[e.base: e.base + e.e_shoff + e.e_shnum * e.e_shentsize]
        # the object image ends after the last section (plus shstrtab); compute conservatively
        end = max(s["offset"] + s["size"] for s in e.order)
        end = max(end, e.e_shoff + e.e_shnum * e.e_shentsize)
        raw = buf[e.base: e.base + end]
        opath = os.path.join(args.outdir, f"plugin_{tag}.o")
        open(opath, "wb").write(raw)
        print(f"\n== [{n}] file_offset={e.base:#x} e_flags={e.e_flags:#x} arch={e.arch} "
              f"size={len(raw)} -> {opath}")
        for s in e.order:
            if s["type"] == 8 or not s["name"]:
                continue
            print(f"   {s['name']:52s} type={s['type']:>3} off={s['offset']:#08x} size={s['size']}")
        syms = e.symbols()
        with open(os.path.join(args.outdir, f"plugin_{tag}_symbols.txt"), "w") as f:
            f.write(f"# plugin_{tag}  libcuda offset {e.base:#x}  arch {e.arch}\n")
            for s in syms:
                f.write(f"{s['value']:#010x} {s['size']:>6} bind={s['bind']} type={s['type']} "
                        f"shndx={s['shndx']:>3} {s['section'] or '-':40s} {s['name']}\n")
        print(f"   symbols: {len(syms)} -> plugin_{tag}_symbols.txt")
        with open(os.path.join(args.outdir, f"plugin_{tag}_relocs.txt"), "w") as f:
            for s in e.order:
                if not s["name"].startswith(".text."):
                    continue
                rl = e.relocs(s["name"])
                if not rl:
                    continue
                f.write(f"\n## {s['name']} ({len(rl)} relocs)\n")
                for r in rl:
                    f.write(f"  {r['offset']:#08x} type={r['type']:<3} sym={r['sym']} "
                            f"addend={r['addend']:#x}\n")
        print(f"   relocs -> plugin_{tag}_relocs.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
