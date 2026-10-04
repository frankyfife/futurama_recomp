"""Recount accesses of .text code to the D3D section's BSS (0x243890-0x246cf8),
classifying each memory operand as read / write / read-modify-write with capstone's
operand access flags (renderer_d3dglobals.py counted every memory operand that is the
first operand as a write, which also counts cmp/test/push)."""
import collections, capstone
from capstone import x86
from verify_renderer_common import *
md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md2.detail = True
REN = [(0x146020, 0x14cd50), (0x1552c0, 0x166871)]
def cls(a):
    if any(lo <= a < hi for lo, hi in REN): return "REN"
    if 0xfd000 <= a < 0x166871: return "NI"
    if a >= 0x166871: return "CRT"
    return "GAME"
LO, HI = 0x243890, 0x246cf8
acc = collections.Counter(); funcs = collections.defaultdict(set); glob = collections.defaultdict(collections.Counter)
mn = collections.Counter()
for fa, (nm, sz, blk) in F.items():
    if blk != ".text" or sz <= 0: continue
    for ins in md2.disasm(X.read(fa, sz), fa):
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and LO <= (op.mem.disp & 0xffffffff) < HI:
                k = cls(fa); v = op.mem.disp & 0xffffffff
                w = op.access & capstone.CS_AC_WRITE; r = op.access & capstone.CS_AC_READ
                kind = "RW" if (w and r) else "W" if w else "R"
                acc[(k, kind)] += 1; funcs[k].add(fa); glob[k][v] += 1; mn[(k, kind, ins.mnemonic)] += 1
for k in sorted(funcs):
    print(k, "functions", len(funcs[k]), "W", acc[(k, "W")], "RW", acc[(k, "RW")], "R", acc[(k, "R")], "distinct globals", len(glob[k]))
    print("   by mnemonic:", sorted(((kind, m, c) for (kk, kind, m), c in mn.items() if kk == k), key=lambda x: -x[2])[:14])
    print("   top:", " ".join(f"{g:#x}x{c}" for g, c in glob[k].most_common(12)))
    print("   range:", hex(min(glob[k])), hex(max(glob[k])))
    print("   funcs:", " ".join(f"{f:#x}" for f in sorted(funcs[k])))
# accesses to the initialised part of the D3D section (0x2342c0-0x243890) and to
# other library sections, excluding direct call/jmp targets
print("--- memory operands / immediates into D3D raw part, from .text")
c2 = collections.Counter()
for fa, (nm, sz, blk) in F.items():
    if blk != ".text" or sz <= 0: continue
    for ins in md2.disasm(X.read(fa, sz), fa):
        if ins.mnemonic in ("call", "jmp"): continue
        for op in ins.operands:
            v = None
            if op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0: v = op.mem.disp & 0xffffffff
            elif op.type == x86.X86_OP_IMM: v = op.imm & 0xffffffff
            if v is not None and 0x2342c0 <= v < 0x243890:
                c2[(cls(fa), fa, ins.address, ins.mnemonic + " " + ins.op_str)] += 1
for k in sorted(c2): print("  ", k[0], hex(k[1]), hex(k[2]), k[3])
