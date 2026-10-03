#!/usr/bin/env python3
# =====================================================================
#  analyze_nvcuda.py - static reconnaissance of the CUDA driver DLL.
#
#  Purpose (sm120 / Blackwell L3 study):
#    * Enumerate the PE export table of nvcuda.dll with a PURE-Python PE
#      parser (no pefile, no third-party deps) and flag every name that
#      smells like the trap / export-table / debug machinery we need for
#      Level-3 preemption (cuGetExportTable, TrapHandler, Debug*, ETID...).
#    * Sweep the raw image for architecture codenames and API fragments
#      ("TrapHandler", "GetInfo", "Pascal", "Blackwell", "Hopper",
#      "Volta", ...) to confirm which GPU generations the driver's trap
#      handler table knows about, and to harvest nearby strings that hint
#      at ETID GUIDs / assert messages.
#
#  Output: evidence/nvcuda_analysis.txt  (and stdout mirror).
#  Usage : python tools/analyze_nvcuda.py [path-to-nvcuda.dll]
#          default DLL = C:\Windows\System32\nvcuda.dll
# =====================================================================

import os
import re
import sys
import struct

DEFAULT_DLL = r"C:\Windows\System32\nvcuda.dll"

# Export-name substrings (case-insensitive) that mark an interesting symbol.
EXPORT_KEYWORDS = [
    "exporttable", "trap", "traphandler", "debug", "etid",
    "timeslice", "preempt", "sm", "inject", "getinfo",
    "exception", "breakpoint", "patch", "cubin", "elf",
]

# Raw-byte strings to hunt anywhere in the image.
STRING_PATTERNS = [
    "TrapHandler", "GetTrapHandler", "TriggerTrap", "GetInfo",
    "cuGetExportTable", "ExportTable", "ETID",
    "Pascal", "Volta", "Turing", "Ampere", "Hopper", "Ada", "Blackwell",
    "sm_120", "sm_100", "sm_90", "sm_86", "sm_80", "sm_70",
    "timeslice", "preempt", "assert", "BPT", "TRAP", "breakpoint",
]

# A GUID-ish regex to surface candidate ETID literals near string hits.
GUID_RE = re.compile(
    rb"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    rb"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"
)


# ----------------------------------------------------------------------
#  Minimal PE loader
# ----------------------------------------------------------------------
class PEImage:
    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not a PE image (missing MZ)")
        self.e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if data[self.e_lfanew:self.e_lfanew + 4] != b"PE\x00\x00":
            raise ValueError("not a PE image (missing PE signature)")
        coff = self.e_lfanew + 4
        (self.machine, self.num_sections, _t, _psym, _nsym,
         self.opt_size, _chars) = struct.unpack_from("<HHIIIHH", data, coff)
        self.opt = coff + 20
        self.magic = struct.unpack_from("<H", data, self.opt)[0]
        self.pe32plus = (self.magic == 0x20B)
        # DataDirectory starts at opt+96 (PE32) or opt+112 (PE32+).
        dd = self.opt + (112 if self.pe32plus else 96)
        self.num_dirs = struct.unpack_from("<I", data, dd - 4)[0]
        self.export_rva, self.export_size = struct.unpack_from("<II", data, dd)
        # Section headers follow the optional header.
        self.sections = []
        sh = self.opt + self.opt_size
        for i in range(self.num_sections):
            off = sh + i * 40
            name = data[off:off + 8].rstrip(b"\x00").decode("latin1")
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
            self.sections.append((name, vaddr, vsize, raddr, rsize))

    def rva_to_off(self, rva):
        for _name, vaddr, vsize, raddr, rsize in self.sections:
            end = vaddr + max(vsize, rsize)
            if vaddr <= rva < end:
                return raddr + (rva - vaddr)
        return None

    def read_rva(self, rva, size):
        off = self.rva_to_off(rva)
        if off is None:
            return None
        return self.data[off:off + size]

    def cstr(self, off, maxlen=512):
        end = self.data.find(b"\x00", off, off + maxlen)
        if end == -1:
            end = off + maxlen
        return self.data[off:end].decode("latin1", "replace")

    def exports(self):
        """Return list of (ordinal, name, func_rva)."""
        if self.export_rva == 0:
            return []
        base = self.rva_to_off(self.export_rva)
        if base is None:
            return []
        (_flags, _ts, _maj, _min, _name_rva, _base,
         n_funcs, n_names, rva_funcs, rva_names,
         rva_ords) = struct.unpack_from("<IIHHIIIIIII", self.data, base)
        off_funcs = self.rva_to_off(rva_funcs)
        off_names = self.rva_to_off(rva_names)
        off_ords = self.rva_to_off(rva_ords)
        out = []
        for i in range(n_names):
            name_rva = struct.unpack_from("<I", self.data, off_names + i * 4)[0]
            ord_idx = struct.unpack_from("<H", self.data, off_ords + i * 2)[0]
            func_rva = 0
            if off_funcs is not None:
                func_rva = struct.unpack_from("<I", self.data,
                                              off_funcs + ord_idx * 4)[0]
            name_off = self.rva_to_off(name_rva)
            name = self.cstr(name_off) if name_off is not None else "<bad>"
            out.append((ord_idx + _base, name, func_rva))
        out.sort(key=lambda t: t[1].lower())
        return out


# ----------------------------------------------------------------------
#  Output helpers
# ----------------------------------------------------------------------
class Report:
    def __init__(self, path):
        self.lines = []
        self.path = path

    def w(self, s=""):
        self.lines.append(s)
        print(s)

    def hr(self, title=""):
        self.w("=" * 72)
        if title:
            self.w(title)
            self.w("=" * 72)

    def flush(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(self.lines) + "\n")
        print("\n[analyze_nvcuda] wrote %s" % self.path)


def main():
    dll = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DLL
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "evidence", "nvcuda_analysis.txt")
    rpt = Report(os.path.normpath(out))

    rpt.hr("nvcuda.dll static analysis (sm120 / Blackwell L3 trap study)")
    rpt.w("file : %s" % dll)
    if not os.path.isfile(dll):
        rpt.w("ERROR: file not found")
        rpt.flush()
        return 2
    data = open(dll, "rb").read()
    rpt.w("size : %d bytes (0x%x)" % (len(data), len(data)))
    rpt.w("")

    # ---- PE overview + export table ---------------------------------
    try:
        pe = PEImage(data)
    except Exception as e:  # noqa: BLE001
        rpt.w("ERROR: PE parse failed: %s" % e)
        rpt.flush()
        return 1

    arch = {0x8664: "x86-64", 0xAA64: "ARM64", 0x14C: "x86"}.get(
        pe.machine, hex(pe.machine))
    rpt.hr("[1] PE overview")
    rpt.w("machine        : %s" % arch)
    rpt.w("format         : %s" % ("PE32+ (64-bit)" if pe.pe32plus else "PE32"))
    rpt.w("sections       : %d" % pe.num_sections)
    for name, vaddr, vsize, raddr, rsize in pe.sections:
        rpt.w("   %-8s VA=0x%08x vsize=0x%07x raw=0x%07x rsize=0x%07x"
              % (name, vaddr, vsize, raddr, rsize))
    rpt.w("export dir RVA : 0x%08x (size 0x%x)" % (pe.export_rva, pe.export_size))
    rpt.w("")

    exps = pe.exports()
    rpt.hr("[2] Export table (%d named exports)" % len(exps))
    interesting = []
    for ordinal, name, func_rva in exps:
        low = name.lower()
        if any(k in low for k in EXPORT_KEYWORDS):
            interesting.append((ordinal, name, func_rva))
    rpt.w("-- exports matching keywords %s" % (EXPORT_KEYWORDS,))
    rpt.w("   (%d hit(s))" % len(interesting))
    for ordinal, name, func_rva in interesting:
        rpt.w("   ord=%-5d RVA=0x%08x  %s" % (ordinal, func_rva, name))
    rpt.w("")
    rpt.w("-- first 40 exports (alphabetical, for context) --")
    for ordinal, name, func_rva in exps[:40]:
        rpt.w("   ord=%-5d RVA=0x%08x  %s" % (ordinal, func_rva, name))
    if len(exps) > 40:
        rpt.w("   ... (%d more)" % (len(exps) - 40))
    rpt.w("")

    # ---- raw string sweep -------------------------------------------
    rpt.hr("[3] Raw string sweep (whole image, case-sensitive)")
    low_data = data.lower()
    for pat in STRING_PATTERNS:
        needle = pat.encode("latin1")
        # case-sensitive primary hit list, plus case-insensitive count
        hits = []
        i = data.find(needle)
        while i != -1 and len(hits) < 64:
            hits.append(i)
            i = data.find(needle, i + 1)
        ci_count = low_data.count(needle.lower())
        rpt.w("%-16s exact-hits=%-4d ci-hits=%-4d" % (pat, len(hits), ci_count))
        for h in hits[:12]:
            ctx = _context(data, h, len(needle))
            rpt.w("      @0x%08x  %s" % (h, ctx))
        if len(hits) > 12:
            rpt.w("      ... (%d more exact hits)" % (len(hits) - 12))
    rpt.w("")

    # ---- GUID literals (candidate ETIDs) ----------------------------
    rpt.hr("[4] GUID-like literals in image (candidate ETIDs)")
    guids = {}
    for m in GUID_RE.finditer(data):
        s = m.group(0).decode("latin1")
        guids.setdefault(s, []).append(m.start())
    if not guids:
        rpt.w("   (no ASCII GUID strings found)")
    for s, offs in sorted(guids.items()):
        rpt.w("   %s  x%d  first@0x%08x" % (s, len(offs), offs[0]))
    rpt.w("")

    rpt.hr("[5] Notes / next steps")
    rpt.w("   * cuGetExportTable(&table, &etid) is the entry point every L3")
    rpt.w("     trap path funnels through; the ETID GUID is undocumented.")
    rpt.w("   * Cross-reference the GUIDs above against cuxtra's")
    rpt.w("     cuGetExportTable call sites (see analyze_cuxtra.py).")
    rpt.w("   * Arch codenames present here tell us which generations the")
    rpt.w("     driver's trap-handler table supports; confirm sm120/Blackwell.")
    rpt.flush()
    return 0


def _context(data, off, nlen, span=24):
    """Return a printable ASCII window around a hit."""
    lo = max(0, off - span)
    hi = min(len(data), off + nlen + span)
    raw = data[lo:hi]
    txt = "".join(chr(b) if 32 <= b < 127 else "." for b in raw)
    return txt


if __name__ == "__main__":
    sys.exit(main())