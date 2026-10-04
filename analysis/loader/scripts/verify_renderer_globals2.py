"""Re-check of renderer.md claims about single globals: every instruction (all code,
all sections) whose memory operand or immediate equals one of the given VAs,
with access type. Default set: DAT_0030edd4 (texture cache mode), DAT_002d7900
(NiSourceTexture preload flag), DAT_002d6740 (main-loop clear mode),
DAT_002d7b18..20 (renderer list), DAT_002e7928/2c (screen offset).
Also prints the initial .data value (if inside raw data).
Usage: python verify_renderer_globals2.py [hexva ...]"""
import sys, capstone
from capstone import x86
from verify_renderer_common import *
md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md2.detail = True
T = [int(a, 16) for a in sys.argv[1:]] or [0x30edd4, 0x2d7900, 0x2d6740, 0x2e7928, 0x2e792c]
for t in T:
    raw = X.read(t, 4)
    print(f"== {t:#x} initial={int.from_bytes(raw, 'little'):#x}" if raw else f"== {t:#x} (bss)")
    for fa, (nm, sz, blk) in sorted(F.items()):
        if sz <= 0: continue
        code = X.read(fa, sz)
        if not code: continue
        for i in md2.disasm(code, fa):
            for op in i.operands:
                if op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xffffffff) == t:
                    acc = ("W" if op.access & capstone.CS_AC_WRITE else "") + ("R" if op.access & capstone.CS_AC_READ else "")
                    print(f"  {acc or 'lea':3s} {i.address:#x} {nm}: {i.mnemonic} {i.op_str}")
                elif op.type == x86.X86_OP_IMM and (op.imm & 0xffffffff) == t:
                    print(f"  IMM {i.address:#x} {nm}: {i.mnemonic} {i.op_str}")
