"""Code that Ghidra did not put into any function ("gaps") inside .text, and what
it contains: direct D3D calls, accesses to the D3D BSS, and indirect calls with
renderer-range vtable offsets (0xa0..0x128). Gap starts referenced from .rdata
(vtable slots) are listed, because those are real methods.
A gap is decoded linearly from its first non-padding byte (0x90/0xcc skipped).
Usage: python verify_renderer_gaps.py [lo hi]   (default: whole .text)"""
import sys, struct, collections, capstone
from capstone import x86
from verify_renderer_common import *
md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md2.detail = True
lo, hi = (int(sys.argv[1], 16), int(sys.argv[2], 16)) if len(sys.argv) > 2 else (0x11000, 0x11000 + 0x2232c0)
D3D_LO, BSS_LO, D3D_HI = 0x2342c0, 0x243890, 0x2342c0 + 0x12a38
iv = sorted((a, a + max(F[a][1], 1)) for a in F if F[a][2] == ".text" and F[a][1] > 0)
gaps, cur = [], lo
for a, b in iv:
    if b <= cur: continue
    if a > cur and a > lo: gaps.append((max(cur, lo), min(a, hi)))
    cur = max(cur, b)
if cur < hi: gaps.append((cur, hi))
gaps = [(a, b) for a, b in gaps if b > a and a < hi]
rref = collections.defaultdict(list)
for va in range(0x293560, 0x293560 + 0x2a96c - 4, 4):
    v = X.u32(va)
    if lo <= v < hi: rref[v].append(va)
def region(a):
    if 0x146020 <= a < 0x14cd50 or 0x1552c0 <= a < 0x166871: return "REN"
    if 0xfd000 <= a < 0x146020 or 0x14cd50 <= a < 0x1552c0: return "NI"
    if 0x11000 <= a < 0xfd000: return "GAME"
    return "other"
tot = collections.Counter(); d3dcalls = collections.defaultdict(set); bss = collections.Counter(); vc = []
codegaps = 0
for a, b in gaps:
    data = X.read(a, b - a)
    k = 0
    while k < len(data) and data[k] in (0x90, 0xcc, 0x00): k += 1
    if k >= len(data): continue
    start = a + k
    ins = list(md2.disasm(data[k:], start))
    if not ins: continue
    codegaps += 1
    reg = region(start)
    refd = [hex(r) for v in range(start, start + 1) for r in rref.get(v, [])]
    tot[reg] += b - start
    for i in ins:
        if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x"):
            t = int(i.op_str, 16)
            if D3D_LO <= t < D3D_HI: d3dcalls[F.get(t, (hex(t),))[0]].add((reg, i.address))
        for op in i.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and BSS_LO <= (op.mem.disp & 0xffffffff) < D3D_HI:
                bss[(reg, start)] += 1
            if i.mnemonic in ("call", "jmp") and op.type == x86.X86_OP_MEM and op.mem.base != 0 and 0xa0 <= op.mem.disp <= 0x128 and op.mem.disp % 4 == 0:
                vc.append((reg, i.address, op.mem.disp // 4))
    if b - start >= 16 or refd:
        print(f"gap {start:#x}-{b:#x} ({b - start} B) {reg} rdata-refs={refd[:3]}")
print("\ncode gaps:", codegaps, "bytes per region:", dict(tot))
print("direct D3D calls from gaps:")
for n, s in sorted(d3dcalls.items()): print(f"  {n}: " + " ".join(f"{r}:{a:#x}" for r, a in sorted(s, key=lambda x: x[1])))
print("D3D-BSS accesses in gaps:", dict(bss))
print("indirect calls with offsets 0xa0..0x128 in gaps:", [(r, hex(a), s) for r, a, s in vc])
