"""Re-check of renderer.md 5.7 / key fact 14 and 1.3 (+0x5c): every instruction in
the whole binary (outside D3D) whose memory operand is [reg + 0x504] or
[reg + 0x5c] and that WRITES it (capstone access flags), restricted to the
renderer module for 0x5c; plus the conditions in RenderShape (vf48 0x14c4f0)."""
import capstone
from capstone import x86
from verify_renderer_common import *
md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md2.detail = True
REN = lambda a: 0x146020 <= a < 0x14cd50 or 0x1552c0 <= a < 0x166871
for disp, only_ren in ((0x504, False), (0x5c, True), (0x500, True)):
    print(f"== writes/reads of [reg+{disp:#x}]" + (" (REN only)" if only_ren else " (all code)"))
    for fa, (nm, sz, blk) in sorted(F.items()):
        if sz <= 0 or blk != ".text" or (only_ren and not REN(fa)): continue
        for i in md2.disasm(X.read(fa, sz), fa):
            for op in i.operands:
                if op.type == x86.X86_OP_MEM and op.mem.base != 0 and op.mem.disp == disp and op.mem.base != x86.X86_REG_ESP:
                    acc = ("W" if op.access & capstone.CS_AC_WRITE else "") + ("R" if op.access & capstone.CS_AC_READ else "")
                    if disp != 0x504 and "W" not in acc and disp != 0x5c: continue
                    if disp == 0x504 or REN(fa):
                        print(f"  {acc:2s} {i.address:#x} in {nm}: {i.mnemonic} {i.op_str}")
