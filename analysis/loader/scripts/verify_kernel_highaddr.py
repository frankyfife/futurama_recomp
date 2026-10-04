"""Reviewer check: does code (inside Ghidra functions) touch kernel-space addresses directly
(memory operands or immediates in 0x80000000..0x8FFFFFFF), apart from the thunk table?
Linear sweep of every Ghidra function in the code sections.
"""
from collections import Counter

from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from verify_kernel_common import CODE_SECTIONS, XBE, Funcs, fmt, md, module_of

F = Funcs()
hits = []
for a, sz, blk, nm in F.rows:
    if blk not in CODE_SECTIONS or sz <= 0:
        continue
    for i in md.disasm(XBE.read(a, sz), a):
        for op in i.operands:
            v = None
            if op.type == X86_OP_MEM:
                v = op.mem.disp & 0xFFFFFFFF
            elif op.type == X86_OP_IMM:
                v = op.imm & 0xFFFFFFFF
            if v is not None and 0x80000000 <= v < 0x90000000 and i.mnemonic not in ("and", "or", "test", "xor", "cmp") \
                    and not (op.type == X86_OP_IMM and (v & 0xFFFF0000) == 0x80000000):
                hits.append((module_of(a), nm, fmt(i)))
                break
print("instructions with 0x8xxxxxxx operands (excluding and/or/test/xor/cmp and 0x8000xxxx immediates):", len(hits))
print(Counter(h[0] for h in hits))
for h in hits:
    if h[0] in ("XAPI", "LIBC", "GAME"):
        print("  %-5s %-40s %s" % h)
