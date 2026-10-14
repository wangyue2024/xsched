#!/usr/bin/env python3
"""Find all references (relocations) to given symbols in nv-kernel.o_binary."""
import struct, sys, bisect
sys.path.insert(0, '/tmp/l3work')
from elf import ELF

e = ELF('/usr/src/nvidia-595.99.02/nvidia/nv-kernel.o_binary')

def all_relocs():
    """Yield (section_name, r_offset, symname, addend) for .rela.text/.rela.data/.rela.rodata."""
    for s in e.sec_list:
        if s['type'] != 4: continue
        target = e.sec_list[s['info']]
        n = s['size'] // 24
        for i in range(n):
            o = s['off'] + i*24
            r_off, r_info, r_add = struct.unpack_from('<QQq', e.data, o)
            r_sym = r_info >> 32
            r_type = r_info & 0xffffffff
            symname = e.syms[r_sym][2] if r_sym < len(e.syms) else None
            yield (target['sname'], r_off, symname, r_add, r_type)

def containing_func(vaddr):
    """Name of the function containing vaddr in .text (by symbol value)."""
    if vaddr >= 0x3c5835: return None
    i = bisect.bisect_right(e.sorted_addrs, vaddr) - 1
    if i < 0: return None
    v = e.sorted_addrs[i]
    nm = e.syms_by_addr[v]
    # only report if it's a code symbol reasonably close
    if vaddr - v < 0x8000:
        return f"{nm}+{vaddr-v:#x}"
    return None

if __name__ == "__main__":
    targets = sys.argv[1:]
    hits = {t: [] for t in targets}
    for secname, roff, symname, add, rtype in all_relocs():
        if symname is None: continue
        for t in targets:
            if symname == t or symname.startswith(t):
                hits[t].append((secname, roff, symname, add, rtype))
    for t in targets:
        print(f"=== {t}: {len(hits[t])} refs ===")
        for secname, roff, symname, add, rtype in hits[t][:40]:
            sec = next(s for s in e.sec_list if s['sname']==secname)
            vaddr = sec['addr'] + roff
            where = containing_func(vaddr) if secname=='.text' else secname
            print(f"  {secname}@{vaddr:#x} (func {where}) -> {symname}+{add:#x} type={rtype}")
