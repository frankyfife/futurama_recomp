"""cpu_gaps.py - list byte ranges of code sections not reached by cpu_rd.RD and
characterise them (padding / strings / code-like / data)."""
import sys, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from cpu_rd import RD
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

def gaps(rd):
    covered = {}
    for va, (sz, *_r) in rd.insn.items():
        covered[va] = sz
    jt = set()
    for t, tg in rd.jumptables.items():
        for k in range(len(tg)*4): jt.add(t+k)
    out = []
    for lo, hi, raw, n in rd.ranges:
        cov = bytearray(hi-lo)
        for va, sz in covered.items():
            if lo <= va < hi:
                cov[va-lo:va-lo+sz] = b"\1"*sz
        for a in jt:
            if lo <= a < hi: cov[a-lo] = 2
        i = 0
        while i < hi-lo:
            if cov[i]:
                i += 1; continue
            j = i
            while j < hi-lo and not cov[j]: j += 1
            out.append((lo+i, lo+j, n))
            i = j
    return out

def classify(rd, a, b):
    d = rd.x.read(a, b-a)
    if all(c in (0x90, 0xCC, 0x00) for c in d): return "pad"
    pr = sum(1 for c in d if 0x20 <= c < 0x7f or c in (0, 9, 10, 13))
    if pr / len(d) > 0.9 and len(d) >= 8: return "ascii"
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    n = bad = 0
    p = 0
    rets = 0
    while p < len(d):
        ins = list(md.disasm(d[p:p+16], a+p, 1))
        if not ins:
            bad += 1; p += 1; continue
        n += 1
        if ins[0].mnemonic in ("ret",): rets += 1
        p += ins[0].size
    return "codelike" if bad == 0 and rets > 0 else "data?(bad=%d,ins=%d,ret=%d)" % (bad, n, rets)

if __name__ == "__main__":
    rd = RD().run()
    g = gaps(rd)
    agg = collections.Counter()
    for a, b, n in g:
        c = classify(rd, a, b)
        agg[(n, c.split("(")[0])] += b - a
        if b - a >= int(sys.argv[1], 16) if len(sys.argv) > 1 else b - a >= 0x400:
            print("%08x-%08x %-6s %6x %s" % (a, b, n, b - a, c))
    for k, v in sorted(agg.items()): print(k, hex(v))
