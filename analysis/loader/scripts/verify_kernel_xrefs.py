"""Reviewer helper: find code/data references to given addresses
(E8/E9 rel32, 0F 8x rel32 targets and absolute 4-byte pointers anywhere in the image).
Usage: python verify_kernel_xrefs.py 2742c0 f99da ...
"""
import struct
import sys

from verify_kernel_common import CODE_SECTIONS, XBE, Funcs

F = Funcs()
for arg in sys.argv[1:]:
    t = int(arg, 16)
    print("==== %06x" % t)
    for name, va, vsz, raw, rsz in XBE.sections:
        b = XBE.b[raw:raw + rsz]
        p = struct.pack("<I", t)
        o = b.find(p)
        while o >= 0:
            print("   abs ptr at %06x (%s) %s" % (va + o, name, F.name_or(va + o)))
            o = b.find(p, o + 1)
        if name not in CODE_SECTIONS:
            continue
        for o in range(len(b) - 5):
            op = b[o]
            if op in (0xE8, 0xE9):
                tg = (va + o + 5 + struct.unpack_from("<i", b, o + 1)[0]) & 0xFFFFFFFF
                if tg == t:
                    print("   rel32 %s at %06x (%s) %s" % ("call" if op == 0xE8 else "jmp", va + o, name, F.name_or(va + o)))
            elif op == 0x0F and 0x80 <= b[o + 1] <= 0x8F and o + 6 <= len(b):
                tg = (va + o + 6 + struct.unpack_from("<i", b, o + 2)[0]) & 0xFFFFFFFF
                if tg == t:
                    print("   jcc rel32 at %06x (%s) %s" % (va + o, name, F.name_or(va + o)))
