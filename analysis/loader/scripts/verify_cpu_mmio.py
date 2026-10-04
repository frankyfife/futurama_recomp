"""verify_cpu_mmio.py - key facts 12/13 (MMIO / phys window / I/O port references).
Own sweep of all Ghidra bodies: every memory operand displacement and every immediate is classified
by range. Counts per section and per function; lists the non-DSOUND hits and DSOUND immediates."""
import sys, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *
from capstone import CS_OP_MEM, CS_OP_IMM
from capstone.x86 import X86_REG_INVALID

R = [("NV2A", 0xFD000000, 0xFE000000), ("APU", 0xFE800000, 0xFE900000), ("AC97", 0xFEC00000, 0xFEC01000),
     ("USB", 0xFED00000, 0xFED10000), ("NIC", 0xFEF00000, 0xFEF01000), ("FLASH", 0xFF000000, 0xFFF00000),
     ("FEother", 0xFE000000, 0xFF000000), ("phys80", 0x80000000, 0x84000000), ("F0alias", 0xF0000000, 0xF4000000)]


def cls(v):
    for n, a, b in R:
        if a <= v < b:
            if n == "FEother" and any(a2 <= v < b2 for n2, a2, b2 in R[1:5]):
                continue
            return n
    return None


mem = collections.Counter()
memf = collections.defaultdict(set)
imm = collections.Counter()
rows = []
for (s, sz, nm) in FUNCS:
    sec = X.section(s)
    if sec not in CODE or sz <= 0:
        continue
    for i in md.disasm(X.read(s, sz), s):
        for op in i.operands:
            if op.type == CS_OP_MEM:
                v = op.mem.disp & 0xFFFFFFFF
                c = cls(v)
                if c:
                    based = op.mem.base != X86_REG_INVALID or op.mem.index != X86_REG_INVALID
                    mem[(sec, c, based)] += 1
                    memf[(sec, c)].add(nm)
                    rows.append(("mem", sec, c, based, i.address, i.mnemonic + " " + i.op_str, nm))
            elif op.type == CS_OP_IMM:
                v = op.imm & 0xFFFFFFFF
                c = cls(v)
                if c and c not in ("phys80", "F0alias"):
                    imm[(sec, c)] += 1
                    rows.append(("imm", sec, c, None, i.address, i.mnemonic + " " + i.op_str, nm))
            # port I/O with dx
        if i.mnemonic in ("in", "out"):
            rows.append(("io", sec, "port", None, i.address, i.mnemonic + " " + i.op_str, nm))

print("memory operands with MMIO/phys disp, by (section, class, based):")
for k, v in sorted(mem.items()):
    print("   %-7s %-8s based=%-5s %4d   functions(sec,class)=%d" % (k[0], k[1], k[2], v, len(memf[(k[0], k[1])])))
print("immediates in MMIO ranges (not phys80/F0), by (section, class):")
for k, v in sorted(imm.items()):
    print("   %-7s %-8s %4d" % (k[0], k[1], v))
apuf = set(r[6] for r in rows if r[1] == "DSOUND" and r[2] == "APU")
print("DSOUND functions with any APU operand (mem or imm):", len(apuf))
print("DSOUND functions with APU memory operand:", len(memf[("DSOUND", "APU")]))
print("AC97 functions:", len(memf[("DSOUND", "AC97")]), sorted(memf[("DSOUND", "AC97")]))
print("\nDSOUND immediates:")
for r in rows:
    if r[0] == "imm" and r[1] == "DSOUND":
        print("   %08x %-44s %s" % (r[4], r[5], r[6]))
print("\nall non-DSOUND MMIO/phys memory operands and MMIO immediates:")
for r in rows:
    if r[1] != "DSOUND" and r[0] in ("mem", "imm", "io"):
        print("   %-3s %-6s %-7s %08x %-44s %s" % (r[0], r[1], r[2], r[4], r[5], r[6]))

