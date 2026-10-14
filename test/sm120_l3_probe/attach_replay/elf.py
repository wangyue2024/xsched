#!/usr/bin/env python3
"""Minimal ELF64 parser for nv-kernel.o_binary analysis (no deps)."""
import struct, sys

class ELF:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        assert d[:4] == b'\x7fELF' and d[4] == 2, "not ELF64"
        self.e_shoff, = struct.unpack_from('<Q', d, 0x28)
        self.e_shentsize, self.e_shnum, self.e_shstrndx = struct.unpack_from('<HHH', d, 0x3A)
        self.sections = {}
        self.sec_list = []
        for i in range(self.e_shnum):
            o = self.e_shoff + i*self.e_shentsize
            name, stype = struct.unpack_from('<II', d, o)
            flags, addr, off, size = struct.unpack_from('<QQQQ', d, o+8)
            link, info, align, entsize = struct.unpack_from('<IIQQ', d, o+0x28)
            self.sec_list.append(dict(name=name, type=stype, flags=flags, addr=addr,
                                      off=off, size=size, link=link, info=info, entsize=entsize))
        # section name string table
        shstr = self.sec_list[self.e_shstrndx]
        for s in self.sec_list:
            s['sname'] = self._str(shstr['off'], s['name'])
        # symtab
        self.syms = []  # (value, size, name, shndx)
        for s in self.sec_list:
            if s['type'] == 2:  # SYMTAB
                strtab = self.sec_list[s['link']]
                n = s['size'] // 24
                for i in range(n):
                    o = s['off'] + i*24
                    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', d, o)
                    nm = self._str(strtab['off'], st_name)
                    self.syms.append((st_value, st_size, nm, st_shndx))
                break
        self.syms_by_addr = {}
        for v, sz, nm, sh in self.syms:
            if nm and v not in self.syms_by_addr:
                self.syms_by_addr[v] = nm
        self.sorted_addrs = sorted(self.syms_by_addr)

    def _str(self, base, off):
        end = self.data.index(b'\x00', base+off)
        return self.data[base+off:end].decode('utf-8', 'replace')

    def section_of(self, addr):
        for s in self.sec_list:
            if s['addr'] <= addr < s['addr']+s['size'] and s['type'] != 8:
                return s
        return None

    def vaddr_to_off(self, addr):
        s = self.section_of(addr)
        if not s: return None
        return s['off'] + (addr - s['addr'])

    def read(self, addr, n):
        o = self.vaddr_to_off(addr)
        if o is None: return None
        return self.data[o:o+n]

    def u32(self, addr):
        b = self.read(addr, 4); return struct.unpack('<I', b)[0] if b and len(b)==4 else None
    def u64(self, addr):
        b = self.read(addr, 8); return struct.unpack('<Q', b)[0] if b and len(b)==8 else None

    def nearest_sym(self, addr, max_back=0x4000):
        import bisect
        i = bisect.bisect_right(self.sorted_addrs, addr) - 1
        if i < 0: return None
        v = self.sorted_addrs[i]
        if addr - v > max_back: return None
        return self.syms_by_addr[v], addr - v

    # relocations
    def relocs(self, secname):
        """Return {r_offset: (symname, addend)} for a section (via its .rela.*)."""
        target = None
        for s in self.sec_list:
            if s['sname'] == secname: target = s
        if target is None: return {}
        out = {}
        for s in self.sec_list:
            if s['type'] == 4 and s['info'] == self.sec_list.index(target):  # RELA, sh_info = target index
                n = s['size'] // 24
                for i in range(n):
                    o = s['off'] + i*24
                    r_off, r_info, r_add = struct.unpack_from('<QQq', self.data, o)
                    r_sym = r_info >> 32
                    symname = self.syms[r_sym][2] if r_sym < len(self.syms) else f"sym{r_sym}"
                    out[r_off] = (symname, r_add)
                break
        return out

    def dump_reloc_window(self, secname, addr, nbytes):
        """Dump [addr, addr+nbytes) as u32/u64 with relocations resolved."""
        rel = self.relocs(secname)
        o = self.vaddr_to_off(addr)
        if o is None: return
        raw = self.data[o:o+nbytes]
        for i in range(0, nbytes, 8):
            q = struct.unpack_from('<Q', raw, i)[0]
            line = f"  +{i:#04x}: {raw[i:i+8].hex(' ')}  u32={struct.unpack_from('<I',raw,i)[0]:#010x} u64={q:#018x}"
            # relocation applies to the section-relative offset (r_offset measured from section start)
            sec = self.section_of(addr+i)
            roff = (addr + i) - sec['addr'] if sec else None
            if roff is not None and roff in rel:
                sym, add = rel[roff]
                line += f"   -> {sym}+{add:#x}"
            else:
                near = self.nearest_sym(q, 0x100000)
                if near: line += f"   ~ {near[0]}+{near[1]:#x}"
            print(line)

if __name__ == "__main__":
    e = ELF(sys.argv[1])
    for s in e.sec_list:
        print(f"{s['sname']:16s} type={s['type']} addr={s['addr']:#x} off={s['off']:#x} size={s['size']:#x}")
