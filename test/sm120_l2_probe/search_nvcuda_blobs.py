#!/usr/bin/env python3
# =====================================================================
#  search_nvcuda_blobs.py - locate embedded ELF/cubin images and SASS
#  signatures inside a CUDA driver binary (nvcuda.dll, sm120 study).
#
#  Rationale: the driver ships the trap handler (and other SASS blobs)
#  embedded in its data sections.  The sm86 trap handler dump in the
#  XSched repository (platforms/cuda/hal/inject/trap_handler_sm86.asm)
#  starts with the 16-byte BPT.DRAIN sequence, which is a stable
#  anchor for hunting the equivalent sm120 blob in 13.x drivers.
#
#  Usage: python search_nvcuda_blobs.py [path-to-nvcuda.dll]
# =====================================================================

import sys

DLL = sys.argv[1] if len(sys.argv) > 1 else r"C:\Windows\System32\nvcuda.dll"

PATS = {
    # 7f 45 4c 46 02 01 01 -> ELF64, little endian
    "ELF64_LE": b"\x7fELF\x02\x01\x01",
    # 16-byte BPT.DRAIN: word0=0x000000000000795c word1=0x000fc00000500000
    "BPT.DRAIN": b"\x5c\x79\x00\x00\x00\x00\x00\x00\x00\x00\x50\x00\x00\xc0\x0f\x00",
    # 16-byte BPT.TRAP placeholder: word0=0x000000040000795c word1=0x000fea0000300000
    "BPT.TRAP": b"\x5c\x79\x00\x00\x04\x00\x00\x00\x00\x00\x30\x00\x00\xea\x0f\x00",
    # EXIT: word0=0x000000000000794d word1=0x000fea0003800000
    "EXIT": b"\x4d\x79\x00\x00\x00\x00\x00\x00\x00\x00\x80\x03\x00\xea\x0f\x00",
}


def main():
    data = open(DLL, "rb").read()
    print("file : %s" % DLL)
    print("size : %d bytes (0x%x)" % (len(data), len(data)))
    print("")
    for name, pat in PATS.items():
        hits = []
        i = data.find(pat)
        while i != -1:
            hits.append(i)
            i = data.find(pat, i + 1)
        print("%-10s hits: %d" % (name, len(hits)))
        for h in hits[:80]:
            print("    @ file 0x%08x" % h)
        if len(hits) > 80:
            print("    ... (%d more)" % (len(hits) - 80))
        print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
