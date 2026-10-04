"""Reviewer helper: dump disassembly around addresses.
Usage: python verify_kernel_dump.py <va> [before_bytes] [after_bytes] ...   (several va allowed: va:before:after)
Resynchronises the linear sweep so that <va> is an instruction boundary.
"""
import sys

from verify_kernel_common import XBE, Funcs, fmt, md

F = Funcs()


def dump(va, before=0x40, after=0x10):
    fr = F.containing(va)
    for d in range(0, 24):
        start = va - before + d
        if fr and start < fr[0] <= va:
            start = fr[0]
        ins = list(md.disasm(XBE.read(start, va - start + after + 16), start))
        if any(i.address == va for i in ins):
            break
    print("---- %06x in %s" % (va, F.name_or(va)))
    for i in ins:
        if i.address > va + after:
            break
        print(("=> " if i.address == va else "   ") + fmt(i))


for arg in sys.argv[1:]:
    p = arg.split(":")
    va = int(p[0], 16)
    b = int(p[1], 0) if len(p) > 1 else 0x40
    a = int(p[2], 0) if len(p) > 2 else 0x10
    dump(va, b, a)
