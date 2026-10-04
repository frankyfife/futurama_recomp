"""verify_cpu_smc.py - independent re-check of key facts 15/16 (no self-modifying code, section
API, DOLBY = 24-bit DSP words).
 * every instruction (Ghidra-body sweep) whose memory operand is an absolute address inside a code
   section and which writes (mov/stos-like dest, or rmw ops) -> target class: inside a Ghidra body
   instruction stream or not
 * rep movs/stos with immediate code-section destination pointers (mov edi, imm in code)
 * DOLBY: byte at VA%4==3 statistics
 * callers of the section API
"""
import sys, collections, struct
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *
from capstone import CS_OP_MEM, CS_OP_IMM, CS_AC_WRITE
from capstone.x86 import X86_REG_INVALID

code_lo_hi = [(lo, hi, n) for (n, lo, hi, raw) in sec_ranges()]


def code_sec(v):
    for lo, hi, n in code_lo_hi:
        if lo <= v < hi:
            return n


istart = {}
for (s, sz, nm) in FUNCS:
    if X.section(s) in CODE and sz > 0:
        for i in mdn.disasm(X.read(s, sz), s):
            istart[i.address] = i.size
covered = set()
for a, sz in istart.items():
    for k in range(sz):
        covered.add(a + k)

writes = []
edi_imm = []
for (s, sz, nm) in FUNCS:
    if X.section(s) not in CODE or sz <= 0:
        continue
    for i in md.disasm(X.read(s, sz), s):
        if i.mnemonic == "mov" and i.op_str.startswith("edi, 0x"):
            v = int(i.op_str.split(",")[1], 16)
            if code_sec(v) == ".text":
                edi_imm.append((i.address, v, nm))
        for op in i.operands:
            if op.type == CS_OP_MEM and op.mem.base == X86_REG_INVALID and op.mem.index == X86_REG_INVALID:
                v = op.mem.disp & 0xFFFFFFFF
                if code_sec(v) and (op.access & CS_AC_WRITE):
                    writes.append((i.address, v, i.mnemonic, i.op_str, nm, code_sec(v), v in covered))
print("absolute writes into code-section addresses:", len(writes))
c = collections.Counter((w[5], w[6]) for w in writes)
print("  by (target section, target lies inside a Ghidra-body instruction):", dict(c))
for w in writes:
    if w[6] or w[5] == ".text":
        print("   %08x -> %08x %s %s  [%s] sec=%s onCode=%s" % w)
print("mov edi, imm pointing into .text (rep movs/stos dest candidates):", len(edi_imm), edi_imm[:10])

# DOLBY
(n, lo, hi, raw) = sec_ranges(("DOLBY",))[0]
d = X.b[raw:raw + hi - lo]
nd = len(d) // 4
z3 = sum(1 for k in range(nd) if d[4 * k + 3] == 0)
print("DOLBY raw bytes 0x%x, dwords %d, dwords with byte3==0: %d, VA%%4 of section start: %d" % (len(d), nd, z3, lo % 4))
