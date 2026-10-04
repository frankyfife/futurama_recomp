"""verify_cpu_callsites.py - count direct call/jmp rel32 sites to given targets (byte scan over
code sections, every E8/E9 whose rel32 lands on the target), plus whether the site is on a
Ghidra-body instruction boundary. usage: python verify_cpu_callsites.py 13f250 f6434 ..."""
import sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *

targets = [int(a, 16) for a in sys.argv[1:]]
gstarts = set()
for (s, sz, nm) in FUNCS:
    if X.section(s) in CODE and sz > 0:
        for i in mdn.disasm(X.read(s, sz), s):
            gstarts.add(i.address)
for t in targets:
    hits = []
    for (n, lo, hi, raw) in sec_ranges():
        d = X.b[raw:raw + hi - lo]
        for p in range(len(d) - 5):
            if d[p] in (0xE8, 0xE9):
                if (lo + p + 5 + struct.unpack_from("<i", d, p + 1)[0]) & 0xFFFFFFFF == t:
                    hits.append((lo + p, d[p]))
    print("target %08x: %d raw E8/E9 hits" % (t, len(hits)))
    for a, op in hits:
        o = owner(a)
        print("   %08x %s %-40s ghidraBoundary=%s" % (a, "call" if op == 0xE8 else "jmp ", (o[2] if o and o[3] else "?(after %s)" % (o[2] if o else "-")), a in gstarts))
