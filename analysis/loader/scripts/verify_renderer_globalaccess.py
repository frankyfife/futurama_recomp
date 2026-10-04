"""All .text/library instructions whose absolute memory operand or immediate equals one
of the given addresses, with R/W classification (capstone access flags).
renderer_xref.py guesses instruction starts by back-scanning, which mis-decodes
some sites; this decodes each function linearly from its entry."""
import sys, capstone
from capstone import x86
from verify_renderer_common import *
md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md2.detail = True
T = {int(x, 16) for x in sys.argv[1:]}
for fa, (nm, sz, blk) in sorted(F.items()):
    if sz <= 0 or blk in (".rdata", ".data"): continue
    code = X.read(fa, sz)
    if not code: continue
    for ins in md2.disasm(code, fa):
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xffffffff) in T:
                acc = ("W" if op.access & capstone.CS_AC_WRITE else "") + ("R" if op.access & capstone.CS_AC_READ else "")
                print(f"{fa:08x} {nm:28s} {ins.address:08x} {acc:2s} {ins.mnemonic} {ins.op_str}")
            elif op.type == x86.X86_OP_IMM and (op.imm & 0xffffffff) in T:
                print(f"{fa:08x} {nm:28s} {ins.address:08x} IM {ins.mnemonic} {ins.op_str}")
