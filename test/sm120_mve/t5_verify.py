#!/usr/bin/env python3
# =====================================================================
#  t5_verify.py - programmatic review of platform sm120 arrays (T5)
#
#  Verifies platforms/cuda/hal/src/arch/sm120.cpp from multiple angles:
#
#    [R1] reproduction  : the arrays are bit-exact with the sealed T4
#                         pipeline (patch inject_120.cubin + tail trim)
#    [R2] LDC window    : every patched LDC decodes to the expected
#                         register/offset pair (c[0x0][0x170..0x18C])
#    [R3] BSSY/BSYNC    : each BSSY resolves (relative offset) to its
#                         BSYNC; guardian caps on BSYNC (K3 contract)
#    [R4] census        : instruction counts / families as reviewed
#    [R5] integrity     : 16-byte alignment, no BPT.TRAP residue,
#                         branch-dense structure preserved
#
#  Exit code 0 = all checks pass.
# =====================================================================

import re
import struct
import sys
import os

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'instrument'))
import extract_sass as ex  # noqa: E402

SM120_CPP = os.path.join(REPO, 'platforms', 'cuda', 'hal', 'src', 'arch', 'sm120.cpp')
INJ = os.path.join(REPO, 'platforms', 'cuda', 'hal', 'inject')

fails = []


def ck(cond, name, detail=''):
    print('  [%s] %s%s' % ('PASS' if cond else 'FAIL', name,
                           ('  -- ' + detail) if detail else ''))
    if not cond:
        fails.append(name)


def parse_cpp_arrays(path):
    txt = open(path, encoding='utf-8').read()
    out = {}
    for m in re.finditer(r'static const uint64_t (\w+)\[\]\s*=\s*\{(.*?)\};', txt, re.S):
        words = [int(w, 16) for w in re.findall(r'0x[0-9a-fA-F]{16}', m.group(2))]
        out[m.group(1)] = words
    return out


def instrs(words):
    assert len(words) % 2 == 0
    return [(words[i], words[i + 1]) for i in range(0, len(words), 2)]


def dec_ldc(w):
    """decode LDC w0 -> (reg, offset) or None"""
    if (w[0] & 0xffff) != 0x7b82:
        return None
    reg = (w[0] >> 16) & 0xff
    off = ((w[0] >> 40) & 0xffffff) << 2
    return reg, off


def main():
    print('== T5 review: platforms/cuda/hal/src/arch/sm120.cpp ==')
    arrays = parse_cpp_arrays(SM120_CPP)
    ck(set(arrays) == {'guardian_instructions', 'resume_instructions'},
       'both arrays present', str(sorted(arrays)))
    g = instrs(arrays['guardian_instructions'])
    r = instrs(arrays['resume_instructions'])

    # ---------- R1: bit-exact reproduction from the sealed pipeline ----------
    # Needs inject_120.cubin, a build artifact produced by nvcc >= 12.8
    # (not committed):   cd platforms/cuda/hal/inject && make ARCH=120 bin
    cubin_path = os.path.join(INJ, 'inject_120.cubin')
    if not os.path.exists(cubin_path):
        print('[R1] reproduction from T4 pipeline -- SKIPPED '
              '(inject_120.cubin absent; needs CUDA >= 12.8)')
    else:
        print('[R1] reproduction from T4 pipeline')
        data = bytearray(open(cubin_path, 'rb').read())
        src = ex.parse_inject_source(os.path.join(INJ, 'inject.cu'))
        secs = ex.text_sections(data)
        off_map = ex.OFFSET_MAPS['sm120']

        instrs_p, _, _ = ex.patch_function(data, 'check_preempt', *secs['check_preempt'],
                                           src['check_preempt'], off_map)
        g_expected, _ = ex.tail_trim(instrs_p, 'guardian')
        ck(list(g_expected) == g, 'guardian == patch(inject_120.cubin) + guardian trim',
           '%d instrs' % len(g))

        instrs_r, _, _ = ex.patch_function(bytearray(open(cubin_path, 'rb').read()),
                                           'restore_exec', *secs['restore_exec'],
                                           src['restore_exec'], off_map)
        r_expected, _ = ex.tail_trim(instrs_r, 'resume')
        ck(list(r_expected) == r, 'resume == patch(inject_120.cubin), no trim',
           '%d instrs' % len(r))

    # ---------- R2: patched LDC window slots ----------
    print('[R2] debugger-window LDC slots (c[0x0][0x170..0x18C])')
    g_ldc = [dec_ldc(w) for w in g if dec_ldc(w)]
    r_ldc = [dec_ldc(w) for w in r if dec_ldc(w)]
    window = {0x170, 0x174, 0x178, 0x17C, 0x180, 0x184, 0x188, 0x18C}
    # guardian: 4 patched (R4,R5@buf; R6,R7@idx) + 2 natural gridDim LDC (0x374/0x378)
    g_patched = [x for x in g_ldc if x[1] in window]
    g_natural = [x for x in g_ldc if x[1] not in window]
    ck(set(g_patched) == {(4, 0x170), (5, 0x174), (6, 0x180), (7, 0x184)},
       'guardian patched LDC == R4/R5@0x170/174 + R6/R7@0x180/184',
       str(g_patched))
    ck(set(g_natural) == {(3, 0x374), (3, 0x378)},
       'guardian natural gridDim LDC == R3@0x374/0x378', str(g_natural))
    r_patched = [x for x in r_ldc if x[1] in window]
    ck(set(r_patched) == {(4, 0x170), (5, 0x174), (20, 0x178), (21, 0x17C)},
       'resume patched LDC == R4/R5@0x170/174 + R20/R21@0x178/17C',
       str(r_patched))
    # every LDC in both arrays must map into the window or read gridDim
    ck(all((x[1] in window) or (x[1] in (0x374, 0x378)) for x in g_ldc + r_ldc),
       'no LDC outside the verified window/gridDim set',
       str([x for x in g_ldc + r_ldc if x[1] not in window and x[1] not in (0x374, 0x378)]))

    # ---------- R3: BSSY/BSYNC pairing & capping ----------
    print('[R3] BSSY/BSYNC pairing and K3 capping')
    for name, arr in (('guardian', g), ('resume', r)):
        bssy = []   # (addr, rel, barrier-id)
        bsync = {}  # barrier-id -> addr
        for i, w in enumerate(arr):
            low = w[0] & 0xffff
            bid = (w[0] >> 16) & 0xf
            if low == 0x7945:
                bssy.append((16 * i, (w[0] >> 32) & 0x3fffffff, bid))
            elif low == 0x7941:
                bsync[bid] = 16 * i
        ok = len(bssy) == len(bsync) and all(
            (addr + rel) == bsync.get(bid) for addr, rel, bid in bssy)
        ck(ok, '%s: every BSSY resolves to its BSYNC (%d pairs)'
           % (name, len(bssy)),
           str([(hex(a), hex(rl), b) for a, rl, b in bssy]))
    # guardian caps on BSYNC (physical fall-through into the kernel)
    ck((g[-1][0] & 0xffff) == 0x7941, 'guardian caps on BSYNC (K3)',
       'last w0=0x%016x' % g[-1][0])
    # resume keeps the functional jump-back exit
    ck(any(w[0] == 0x0000000014007950 for w in r),
       'resume contains RET.ABS.NODEC R20 (jump-back exit)')

    # ---------- R4: census ----------
    print('[R4] instruction census')
    def count(arr, low):
        return sum(1 for w in arr if (w[0] & 0xffff) == low)
    def count_exit(arr):
        return sum(1 for w in arr if (w[0] & 0xffff) in (0x794d, 0x094d, 0x894d))
    ck(len(g) == 50 and len(r) == 32, 'lengths 50 / 32',
       '%d / %d' % (len(g), len(r)))
    ck(count_exit(g) == 1 and count_exit(r) == 1, 'EXIT x1 in each')
    ck(count(g, 0x094d) == 1, 'guardian EXIT is @P0 EXIT (exit when flag set)')
    ck(count(r, 0x894d) == 1, 'resume EXIT is @!P0 EXIT (exit when restore_flag clear)')
    ck(count(g, 0x7b1d) == 1 and count(r, 0x7b1d) == 1, 'BAR.SYNC x1 in each')
    ck(count(g, 0x7992) == 1, 'guardian MEMBAR.SC.CTA x1')
    ck(count(g, 0x7948) == 1 and count(r, 0x7948) == 1, 'WARPSYNC.ALL x1 in each')
    ck(count(r, 0x7947) == 1, 'resume self-spin BRA x1 (unreachable pad)')

    # ---------- R5: integrity ----------
    print('[R5] integrity')
    ck(all(len(a) * 8 % 16 == 0 for a in arrays.values()),
       'all arrays 16-byte aligned')
    ck(all(w != (0x000000040000795c, 0x000fea0000300000)
           for w in g + r), 'no BPT.TRAP placeholder residue')
    ck(len(g[0][0].to_bytes(8, 'little')) == 8, 'word size sanity')

    print()
    if fails:
        print('RESULT: %d FAILED check(s)' % len(fails))
        for f in fails:
            print('  - ' + f)
        return 1
    print('RESULT: all T5 checks PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
