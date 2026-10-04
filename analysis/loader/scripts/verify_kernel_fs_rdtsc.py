"""Reviewer check: fs:-segment accesses, rdtsc and the 733 MHz constant.
Method (independent of kernel_analyze): linear sweep of every Ghidra function body in the code
sections; count instructions with an fs:[disp32] memory operand (no base/index) by displacement.
Additionally a byte scan for 0x64-prefixed instructions in code that is NOT covered by any Ghidra
function (reported separately, these need a manual look).
"""
import struct
from collections import Counter, defaultdict

from capstone.x86 import X86_OP_MEM, X86_REG_FS, X86_REG_INVALID

from verify_kernel_common import CODE_SECTIONS, XBE, Funcs, fmt, insn, md

F = Funcs()
fs = defaultdict(list)
rdtsc = []
covered = set()
for a, sz, blk, nm in F.rows:
    if blk not in CODE_SECTIONS or sz <= 0:
        continue
    for i in md.disasm(XBE.read(a, sz), a):
        covered.update(range(i.address, i.address + i.size))
        if i.mnemonic == "rdtsc":
            rdtsc.append((i.address, blk, nm))
        if i.bytes[0] != 0x64 and 0x64 not in i.bytes[:4]:
            continue
        for op in i.operands:
            if op.type == X86_OP_MEM and op.mem.segment == X86_REG_FS and op.mem.base == X86_REG_INVALID \
                    and op.mem.index == X86_REG_INVALID:
                fs[op.mem.disp].append((i.address, blk, nm, i.mnemonic + " " + i.op_str))
for d in sorted(fs):
    l = fs[d]
    print("fs:[0x%x] %5d  by section %s" % (d, len(l), dict(Counter(x[1] for x in l))))
    if d != 0:
        for x in l:
            print("      %06x %-7s %-40s %s" % x)
print("total non-zero fs displacements:", sum(len(fs[d]) for d in fs if d not in (0,)),
      " excluding fs:[4]:", sum(len(fs[d]) for d in fs if d not in (0, 4)))

# byte scan outside Ghidra functions
print("== fs-prefixed instructions outside Ghidra functions (byte scan, manual review needed)")
for name, va, vsz, raw, rsz in XBE.sections:
    if name not in CODE_SECTIONS:
        continue
    b = XBE.b[raw:raw + rsz]
    o = b.find(b"\x64")
    while o >= 0:
        a = va + o
        if a not in covered:
            i = insn(a)
            if i is not None:
                for op in i.operands:
                    if op.type == X86_OP_MEM and op.mem.segment == X86_REG_FS and op.mem.base == X86_REG_INVALID \
                            and op.mem.index == X86_REG_INVALID and op.mem.disp in (0, 4, 0x18, 0x20, 0x24, 0x28, 0x58):
                        print("   %06x %-7s %s" % (a, name, fmt(i)))
        o = b.find(b"\x64", o + 1)

print("== rdtsc in Ghidra functions:")
for r in rdtsc:
    print("   %06x %-7s %s" % r)
print("== rdtsc byte pattern 0f 31 outside Ghidra functions:")
for name, va, vsz, raw, rsz in XBE.sections:
    if name not in CODE_SECTIONS:
        continue
    b = XBE.b[raw:raw + rsz]
    o = b.find(b"\x0f\x31")
    while o >= 0:
        if va + o not in covered:
            print("   %06x %s (unverified decode)" % (va + o, name))
        o = b.find(b"\x0f\x31", o + 1)
pat = struct.pack("<I", 0x2BB5C755)
o = XBE.b.find(pat)
while o >= 0:
    v = XBE.off2va(o)
    print("const 0x2BB5C755 at file off %x va %s %s" % (o, hex(v) if v else None, XBE.section(v) if v else None))
    o = XBE.b.find(pat, o + 1)
for a in (0xF641B, 0xF642C):
    print("---- %06x %s" % (a, F.name_or(a)))
    for i in md.disasm(XBE.read(a, 0x18), a):
        print("   " + fmt(i))
