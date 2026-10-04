"""List every instruction in .text that writes a memory operand [reg+DISP] for the
given displacements (default 0x504 0x500 0x60), with the containing function.
Used to check 'renderer+0x504 is only written with 0' and who touches +0x60."""
import sys, capstone
from capstone import x86
from verify_renderer_common import *
md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md2.detail = True
disps = [int(x, 16) for x in sys.argv[1:]] or [0x504]
for fa, (nm, sz, blk) in sorted(F.items()):
    if blk != ".text" or sz <= 0: continue
    for ins in md2.disasm(X.read(fa, sz), fa):
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base != 0 and (op.mem.disp & 0xffffffff) in disps and (op.access & capstone.CS_AC_WRITE):
                print(f"{fa:08x} {nm:30s} {ins.address:08x} {ins.mnemonic} {ins.op_str}")
