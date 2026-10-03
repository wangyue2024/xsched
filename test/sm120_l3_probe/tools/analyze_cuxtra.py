#!/usr/bin/env python3
# =====================================================================
#  analyze_cuxtra.py - static reverse-engineering of the cuxtra archive.
#
#  Goal (sm120 / Blackwell L3 trap study): understand how cuxtra reaches
#  the CUDA driver's undocumented trap-handler export table so we can add
#  the missing sm120 case.  Concretely this tool:
#
#    [1] nm     : list every symbol mentioning trap / ETID / ExportTable /
#                 Timeslice / GetInfo (raw + demangled), grouped by member.
#    [2] strings: sweep the archive for GUID patterns, assert messages and
#                 architecture codenames (Pascal/Volta/.../Blackwell).
#    [3] objdump -d : disassemble the trap-related members and capture the
#                 full body of trap functions plus a window around every
#                 call that reaches cuGetExportTable.
#    [4] ETID   : locate the CU_ETID_* constant symbol(s), then read their
#                 16 raw bytes out of the owning member's .rdata so we know
#                 the exact GUID cuxtra passes to cuGetExportTable.
#
#  Driven through subprocess (nm / strings / objdump from MinGW binutils).
#  No third-party Python deps.  Output: evidence/cuxtra_analysis.txt
#  Usage : python tools/analyze_cuxtra.py [path-to-libcuxtra.a]
# =====================================================================

import os
import re
import sys
import shutil
import subprocess

DEFAULT_LIB = (r"d:\1file\Desktop\code\xsched\3rdparty\cuxtra\lib"
               r"\libcuxtra_windows_amd64.a")

SYM_KEYWORDS = ["trap", "etid", "exporttable", "timeslice",
                "getinfo", "preempt", "tsg", "getentrypoint"]
STR_KEYWORDS = ["trap", "etid", "exporttable", "timeslice", "pascal",
                "volta", "turing", "ampere", "hopper", "ada", "blackwell",
                "assert", "cuGetExportTable", "sm_1", "sm_9", "sm_8"]
GUID_RE = re.compile(
    r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")

# nm --print-file-name on an archive prints:
#   <archive>.a:<member>.obj:<16-hex-addr> <type> <name>
# The archive path may itself contain ':' (drive letter), so anchor on the
# address+type+name tail and treat everything before it as the file prefix.
NM_LINE_RE = re.compile(r"([0-9a-fA-F]{8,16})\s+([a-zA-Z!?])\s+(.+?)\s*$")
# objdump -s / -d member header:  "<member>.obj:     file format pe-x86-64"
OBJ_MEMBER_RE = re.compile(r"^([\w\.\-]+\.obj):\s+file format")
# objdump -s content line:  " 03d0 01020304 05060708 ....  |ascii........|"
OBJ_CONTENT_RE = re.compile(r"^\s([0-9a-fA-F]+)\s((?:[0-9a-fA-F]{2,8}\s)+)")

def tool(name):
    p = shutil.which(name)
    if not p:
        raise SystemExit("required binutils tool not found on PATH: %s" % name)
    return p


def run(cmd):
    """Run cmd, return stdout text (stderr merged). Never raises."""
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
        return r.stdout.decode("utf-8", "replace")
    except Exception as e:  # noqa: BLE001
        return "<error running %s: %s>" % (cmd[0], e)


def parse_nm_line(ln):
    """Return (member, addr_int, type, name) or None for a nm line."""
    m = NM_LINE_RE.search(ln)
    if not m:
        return None
    addr, typ, name = m.group(1), m.group(2), m.group(3)
    prefix = ln[:m.start(1)].rstrip(":")
    member = prefix.split(":")[-1] if prefix else "<member?>"
    try:
        addr_i = int(addr, 16)
    except ValueError:
        return None
    return member, addr_i, typ, name


class Report:
    def __init__(self, path):
        self.lines = []
        self.path = path

    def w(self, s=""):
        self.lines.append(s)

    def hr(self, title=""):
        self.w("=" * 74)
        if title:
            self.w(title)
            self.w("=" * 74)

    def flush(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        text = "\n".join(self.lines) + "\n"
        with open(self.path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        sys.stdout.write(text)
        print("[analyze_cuxtra] wrote %s" % self.path)

def nm_symbols(rpt, lib, nm):
    raw = run([nm, "--print-file-name", lib])
    dem = run([nm, "-C", "--print-file-name", lib])

    def keep(line):
        low = line.lower()
        return any(k in low for k in SYM_KEYWORDS)

    raw_hits = [ln for ln in raw.splitlines() if keep(ln)]
    dem_hits = [ln for ln in dem.splitlines() if keep(ln)]

    rpt.hr("[1] nm - symbols matching %s" % SYM_KEYWORDS)
    rpt.w("library : %s" % lib)
    rpt.w("archive size : %d bytes" % (os.path.getsize(lib)
                                        if os.path.isfile(lib) else -1))
    rpt.w("")
    rpt.w("-- demangled (%d hit(s)) --" % len(dem_hits))
    for ln in dem_hits:
        parsed = parse_nm_line(ln)
        if parsed:
            member, addr, typ, name = parsed
            rpt.w("   [%s] %08x %s %s" % (member, addr, typ, name))
        else:
            rpt.w("   " + ln)
    rpt.w("")
    rpt.w("-- raw / mangled, grouped by member (%d hit(s)) --" % len(raw_hits))
    by_member = {}
    for ln in raw_hits:
        parsed = parse_nm_line(ln)
        if parsed:
            member, addr, typ, name = parsed
            by_member.setdefault(member, []).append("%08x %s %s"
                                                    % (addr, typ, name))
        else:
            by_member.setdefault("<unparsed>", []).append(ln.strip())
    for member in sorted(by_member):
        rpt.w("  [%s]" % member)
        for s in sorted(set(by_member[member])):
            rpt.w("      %s" % s)
    rpt.w("")

def strings_sweep(rpt, lib, strings):
    out = run([strings, "-a", "-n", "5", lib])
    lines = out.splitlines()
    rpt.hr("[2] strings - keyword / GUID / assert sweep")
    rpt.w("total printable strings (n>=5): %d" % len(lines))
    rpt.w("")

    kw_hits = [ln for ln in lines
               if any(k.lower() in ln.lower() for k in STR_KEYWORDS)]
    rpt.w("-- strings matching keywords (%d) --" % len(kw_hits))
    for ln in kw_hits[:400]:
        rpt.w("   " + ln)
    if len(kw_hits) > 400:
        rpt.w("   ... (%d more)" % (len(kw_hits) - 400))
    rpt.w("")

    guid_hits = sorted({m.group(0) for ln in lines
                        for m in [GUID_RE.search(ln)] if m})
    rpt.w("-- GUID-like strings (candidate ETIDs) (%d) --" % len(guid_hits))
    for g in guid_hits:
        rpt.w("   " + g)
    if not guid_hits:
        rpt.w("   (none as ASCII; ETIDs are raw 16-byte blobs, see section [4])")
    rpt.w("")

    assert_hits = [ln for ln in lines if "assert" in ln.lower()
                   or ".cpp" in ln or ".cu" in ln or ".cc" in ln]
    rpt.w("-- source-path / assert strings (%d) --" % len(assert_hits))
    for ln in assert_hits[:200]:
        rpt.w("   " + ln)
    if len(assert_hits) > 200:
        rpt.w("   ... (%d more)" % (len(assert_hits) - 200))
    rpt.w("")

FUNC_LABEL = re.compile(r"^([0-9a-fA-F]+)\s+<(.+)>:$")
CALL_RE = re.compile(r"\bcall\b", re.IGNORECASE)
TRAP_KW = ["trap", "etid", "exporttable", "timeslice",
           "getinfo", "preempt", "triggertrap"]


def disasm_scan(rpt, lib, objdump):
    text = run([objdump, "-d", "--no-show-raw-insn", lib])
    lines = text.splitlines()
    rpt.hr("[3] objdump -d - trap function bodies + cuGetExportTable calls")
    rpt.w("disassembly lines: %d" % len(lines))
    rpt.w("")

    funcs = []
    cur_member = None
    cur_name = None
    cur_body = []
    for ln in lines:
        mm = OBJ_MEMBER_RE.match(ln)
        if mm:
            cur_member = mm.group(1)
        fm = FUNC_LABEL.match(ln)
        if fm:
            if cur_name is not None:
                funcs.append((cur_member, cur_name, cur_body))
            cur_name = fm.group(2)
            cur_body = [ln]
        elif cur_name is not None:
            cur_body.append(ln)
    if cur_name is not None:
        funcs.append((cur_member, cur_name, cur_body))

    trap_funcs = [(m, n, b) for (m, n, b) in funcs
                  if any(k in n.lower() for k in TRAP_KW)]
    rpt.w("-- trap-related function bodies (%d) --" % len(trap_funcs))
    for (m, n, b) in trap_funcs:
        rpt.w("  [%s] <%s>  (%d insns)" % (m, n, len(b) - 1))
        for bl in b[:140]:
            rpt.w("    " + bl.rstrip())
        if len(b) > 140:
            rpt.w("    ... (%d more lines)" % (len(b) - 140))
        rpt.w("")

    rpt.w("-- call sites referencing ExportTable / trap (context windows) --")
    found = 0
    for i, ln in enumerate(lines):
        if CALL_RE.search(ln) and re.search(
                r"ExportTable|Trap|export_table", ln, re.IGNORECASE):
            found += 1
            lo = max(0, i - 12)
            hi = min(len(lines), i + 3)
            rpt.w("  --- window @line %d ---" % i)
            for j in range(lo, hi):
                mark = ">>" if j == i else "  "
                rpt.w("  %s %s" % (mark, lines[j].rstrip()))
            cands = re.findall(r"0x[0-9a-fA-F]{3,}", "\n".join(lines[lo:hi]))
            rpt.w("      candidate addr/immediates: %s"
                  % (", ".join(sorted(set(cands))[:12])))
            rpt.w("")
    if found == 0:
        rpt.w("  (no direct 'call ...ExportTable' text; the call is likely an")
        rpt.w("   indirect call through the lazy func_ptr - inspect bodies in")
        rpt.w("   section [3] above and the ETID load sites in section [4].)")
    rpt.w("")

def extract_etid(rpt, lib, nm, objdump):
    rpt.hr("[4] ETID constant extraction (CU_ETID_* -> 16 raw bytes)")
    tsym = run([nm, "--print-file-name", lib])
    etid_syms = []   # (member, addr, typ, name)
    for ln in tsym.splitlines():
        if "CU_ETID_" not in ln:
            continue
        parsed = parse_nm_line(ln)
        if parsed and "CU_ETID_" in parsed[3]:
            etid_syms.append(parsed)
    if not etid_syms:
        rpt.w("  (no CU_ETID_* symbols found)")
        rpt.w("")
        return

    # Build (member, addr) -> byte from each member's .rdata contents.
    contents = run([objdump, "-s", "-j", ".rdata", lib])
    addr_bytes = {}
    cur_member = None
    for ln in contents.splitlines():
        mm = OBJ_MEMBER_RE.match(ln)
        if mm:
            cur_member = mm.group(1)
            continue
        cm = OBJ_CONTENT_RE.match(ln)
        if cm and cur_member:
            base = int(cm.group(1), 16)
            try:
                raw = bytes.fromhex(cm.group(2).replace(" ", ""))
            except ValueError:
                continue
            for k, byte in enumerate(raw):
                addr_bytes[(cur_member, base + k)] = byte

    seen = set()
    for member, addr, typ, name in etid_syms:
        key = (member, name)
        if key in seen:
            continue
        seen.add(key)
        blob = bytes(addr_bytes.get((member, addr + k), 0) for k in range(16))
        have = all((member, addr + k) in addr_bytes for k in range(16))
        guid = ("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x%02x%02x%02x%02x" % tuple(blob))
        rpt.w("  symbol : %s" % name)
        rpt.w("  member : %-18s addr=0x%-6x type=%s  bytes-resolved=%s"
              % (member, addr, typ, have))
        rpt.w("  raw16  : %s" % blob.hex())
        rpt.w("  as-GUID: %s" % guid)
        if not have:
            rpt.w("  note   : addr not covered by .rdata dump (symbol may live")
            rpt.w("           in another section); confirm with objdump -s -j")
            rpt.w("           <section> on the extracted member object.")
        rpt.w("")

def main():
    lib = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_LIB
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.normpath(os.path.join(here, "..", "evidence",
                                        "cuxtra_analysis.txt"))
    rpt = Report(out)

    if not os.path.isfile(lib):
        rpt.hr("cuxtra static analysis")
        rpt.w("ERROR: library not found: %s" % lib)
        rpt.flush()
        return 2

    nm = tool("nm")
    strings = tool("strings")
    objdump = tool("objdump")

    rpt.hr("cuxtra static analysis (sm120 / Blackwell L3 trap study)")
    rpt.w("nm      : %s" % nm)
    rpt.w("strings : %s" % strings)
    rpt.w("objdump : %s" % objdump)
    rpt.w("")

    nm_symbols(rpt, lib, nm)
    strings_sweep(rpt, lib, strings)
    disasm_scan(rpt, lib, objdump)
    extract_etid(rpt, lib, nm, objdump)

    rpt.hr("[5] Interpretation / next steps")
    rpt.w("  * EtblTrapHandler::GetInfo<Pascal|...> shows cuxtra implements")
    rpt.w("    per-arch trap-info getters; sm120/Blackwell is the missing case")
    rpt.w("    (add EtblTrapHandler::GetInfoBlackwell + a driver offset entry).")
    rpt.w("  * CU_ETID_ToolsTrapHandler (section [4]) is the GUID cuxtra hands")
    rpt.w("    to cuGetExportTable to obtain the tools trap-handler vtable.")
    rpt.w("  * Verify at run time with a probe that calls cuGetExportTable with")
    rpt.w("    this ETID on the sm120 driver and inspects the returned table.")
    rpt.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())