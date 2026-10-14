#!/usr/bin/env python3
"""Parse ioctl_log3 capture: full ordered sequence with decoded allocs/controls/params."""
import re, struct, sys

LINE = re.compile(r"\[ioctl\] fd=(\d+) req=(0x[0-9a-f]+) dir=(\d+) size=(\d+) r=(-?\d+) pre=([0-9a-f]*) post=([0-9a-f]*)(?: cmd=(0x[0-9a-f]+) psize=(\d+))?(?: params=([0-9a-f]+))?(?: alloc_class=(0x[0-9a-f]+) palloc=(0x[0-9a-f]+))?(?: palloc=([0-9a-f]+))?")

def u32(b,o): return struct.unpack_from('<I', b, o)[0]
def u64(b,o): return struct.unpack_from('<Q', b, o)[0]

def fmt_83de(cmd, p):
    if p is None: return ""
    if cmd == 0x83de0317:
        return f"SuspendContext(flag={u32(p,0)})"
    if cmd == 0x83de0318:
        return "ResumeContext()"
    if cmd in (0x83de0315, 0x83de0316):
        # 24B: {u32 hMem, u32 size, u64 off?, u64 host}
        return f"{'ReadMemory' if cmd==0x83de0315 else 'WriteMemory'}(h=0x{u32(p,0):x} size=0x{u32(p,4):x} @0x{u64(p,8):x} host=0x{u64(p,16):x})"
    if cmd == 0x83de030c:
        return f"ReadAllSmErrorStates(first8={u32(p,0):x},{u32(p,4):x})"
    if cmd in (0x83de031f, 0x83de0307, 0x83de032a):
        name = {0x83de031f:'SetModeErrbarDebug', 0x83de0307:'SetModeMMUDebug', 0x83de032a:'SetModeMMUGccDebug'}[cmd]
        return f"{name}({u32(p,0)})"
    if cmd == 0x83de0309:
        return f"SetExceptionMask(0x{u32(p,0):x})"
    return ""

def main(path, show_all=False):
    idx = 0
    for line in open(path, errors='replace'):
        m = LINE.search(line)
        if not m: continue
        fd=int(m.group(1)); req=int(m.group(2),16); r=int(m.group(5))
        pre=bytes.fromhex(m.group(6) or ""); post=bytes.fromhex(m.group(7) or "")
        cmd=int(m.group(8),16) if m.group(8) else None
        psize=int(m.group(9)) if m.group(9) else 0
        params=bytes.fromhex(m.group(10) or "") if m.group(10) else None
        idx += 1
        if req == 0xc030462b:  # alloc
            if len(pre) < 48: continue
            hC=u32(pre,0); hP=u32(pre,4); hO=u32(pre,8); cl=u32(pre,12); pal=u64(pre,16)
            pl = bytes.fromhex(m.group(13) or "") if m.group(13) else b""
            extra = ""
            if pl and len(pl) >= 12:
                extra = f" | palloc=[{u32(pl,0):#x}, {u32(pl,4):#x}, {u32(pl,8):#x}, {u32(pl,12):#x}]"
            print(f"[{idx:03d}] ALLOC cl={cl:#x} hParent={hP:#010x} wcl={hC:#010x} hObj(pre)={hO:#010x}{extra}")
        elif req == 0xc020462a and len(pre) >= 32:
            hC=u32(pre,0); hO=u32(pre,4); c=u32(pre,8); st=u32(pre,28)
            desc = fmt_83de(c, params) if (c>>16)==0x83de else ""
            if not show_all and not desc and r==0:
                # keep non-83de visible only when uncommon
                pass
            pp = params[:32].hex() if params else ""
            suffix = desc if desc else f" raw={pp}"
            print(f"[{idx:03d}] CTRL  hC={hC:#010x} hObj={hO:#010x} cmd={c:#010x} psz={psize} st(pre)={st:#x} r={r} {suffix}")
        elif req in (0xc01c4634,):
            if len(pre)>=20:
                print(f"[{idx:03d}] DUP   {u32(pre,0):#010x} p={u32(pre,4):#010x} -> {u32(pre,8):#010x} src=({u32(pre,12):#010x},{u32(pre,16):#010x})")
        elif req in (0xc0104629,):
            print(f"[{idx:03d}] FREE  {u32(pre,0):#010x} obj={u32(pre,4):#010x}")
        elif show_all:
            print(f"[{idx:03d}] req={req:#x} fd={fd} r={r} pre={pre[:24].hex()}")

if __name__ == "__main__":
    main(sys.argv[1], len(sys.argv) > 2 and sys.argv[2] == "all")
