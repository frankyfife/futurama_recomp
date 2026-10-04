"""cpu_smc.py - search for self-modifying code: instructions that write to an absolute
address that is (a) inside .text (mapped read-only/execute on Xbox) or (b) an
instruction decoded by cpu_rd in any code section; plus stores of code addresses.
usage: python cpu_smc.py"""
import sys, re, bisect
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from cpu_rd import RD
rd = RD().run()
fst = [f[0] for f in rd.funcs]
covered = set()
for a, (sz, *_r) in rd.insn.items():
    covered.update(range(a, a + sz))
WRITERS = {"mov", "add", "sub", "or", "and", "xor", "inc", "dec", "not", "neg", "shl", "shr", "sar",
           "rol", "ror", "adc", "sbb", "xchg", "stosb", "stosd", "movsb", "movsd", "cmpxchg", "bts", "btr", "btc"}
hits = 0
for va in sorted(rd.insn):
    sz, mn, ops, bb = rd.insn[va]
    if mn not in WRITERS:
        continue
    m = re.match(r"(?:byte|word|dword|qword) ptr (?:\w+:)?\[0x([0-9a-f]+)\]", ops)
    if not m:
        continue
    t = int(m.group(1), 16)
    if rd.x.section(t) == ".text" or t in covered:
        k = bisect.bisect_right(fst, va) - 1
        print("%08x %-40s target %08x (%s) in %s" % (va, mn + " " + ops, t, rd.x.section(t), rd.funcs[k][2]))
        hits += 1
print("absolute writes into code:", hits)
