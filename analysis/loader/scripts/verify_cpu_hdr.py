"""verify_cpu_hdr.py - all absolute memory operands / immediates in the XBE header range
0x10000..0x10968 (Ghidra-body sweep + non-Ghidra regions), to re-check cpu.md section 5."""
import sys, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *
from capstone import CS_OP_MEM, CS_OP_IMM, CS_AC_WRITE
from capstone.x86 import X86_REG_INVALID
res = collections.defaultdict(list)
def scan(a, b, nm):
    for i in md.disasm(X.read(a, b - a), a):
        for op in i.operands:
            if op.type == CS_OP_MEM and op.mem.base == X86_REG_INVALID and op.mem.index == X86_REG_INVALID:
                v = op.mem.disp & 0xFFFFFFFF
                if 0x10000 <= v < 0x10968:
                    res[v].append((i.address, nm, "W" if op.access & CS_AC_WRITE else "R"))
            elif op.type == CS_OP_IMM and 0x10000 <= (op.imm & 0xFFFFFFFF) < 0x10968 and i.mnemonic in ("push", "mov", "lea"):
                res[op.imm & 0xFFFFFFFF].append((i.address, nm, "imm"))
for (s, sz, nm) in FUNCS:
    if X.section(s) in CODE and sz > 0:
        scan(s, s + sz, nm)
for a, b in [(0xf8469, 0xf84dd), (0xf99da, 0xf9a72)]:
    scan(a, b, "extra@%x" % a)
for v in sorted(res):
    L = res[v]
    print("%08x  %3d  %s" % (v, len(L), ", ".join("%x:%s:%s" % (a, n, k) for a, n, k in L[:8])))
