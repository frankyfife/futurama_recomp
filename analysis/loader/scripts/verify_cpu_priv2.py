"""verify_cpu_priv2.py - key facts 10 and 14 (privileged / special instructions, lock, xchg).
Two independent passes (no use of cpu_rd/cpu_scan):
  A) linear sweep of every Ghidra function body (functions.tsv)
  B) linear sweep of every byte range of the code sections NOT covered by a Ghidra body ("gaps"),
     each gap swept from its start; hits are listed with the gap range so they can be triaged.
"""
import sys, collections, bisect
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *
from capstone.x86 import X86_REG_CS, X86_REG_DS, X86_REG_ES, X86_REG_SS, X86_REG_FS, X86_REG_GS

from verify_cpu_classify import special, PRIV, SREG

# ---- A
A = collections.Counter()
Asites = collections.defaultdict(list)
cover = []
for (s, sz, nm) in FUNCS:
    if X.section(s) not in CODE or sz <= 0:
        continue
    cover.append((s, s + sz))
    for i in mdn.disasm(X.read(s, sz), s):
        k = special(i)
        if k:
            A[(X.section(i.address), k)] += 1
            Asites[k].append((i.address, i.bytes.hex(), i.mnemonic + " " + i.op_str, nm))
print("A) Ghidra bodies: special instructions by (section, kind)")
for k, v in sorted(A.items()):
    print("   %-7s %-22s %d" % (k[0], k[1], v))
print("\nA) sites (except int3/xchg-free kinds that are counted only):")
for k in sorted(Asites):
    if k in ("int 3",):
        continue
    for s in Asites[k][:40]:
        print("   %-18s %08x %-16s %-40s %s" % (k, s[0], s[1], s[2], s[3]))

# ---- B
cover.sort()
merged = []
for a, b in cover:
    if merged and a <= merged[-1][1]:
        merged[-1][1] = max(merged[-1][1], b)
    else:
        merged.append([a, b])
gaps = []
for (n, lo, hi, raw) in sec_ranges():
    cur = lo
    for a, b in merged:
        if b <= lo or a >= hi:
            continue
        if a > cur:
            gaps.append((n, cur, a))
        cur = max(cur, b)
    if cur < hi:
        gaps.append((n, cur, hi))
print("\nB) gaps:", len(gaps), "bytes:", sum(b - a for _, a, b in gaps))
B = collections.Counter()
Bsites = []
for (n, a, b) in gaps:
    data = X.read(a, b - a)
    p = 0
    # resync-on-failure linear sweep
    while p < len(data):
        got = False
        for i in mdn.disasm(data[p:], a + p):
            got = True
            k = special(i)
            if k and k != "int 3":
                Bsites.append((n, a, b, i.address, i.bytes.hex(), i.mnemonic + " " + i.op_str, k))
                B[(n, k)] += 1
            p = i.address + i.size - a
        if not got or p < len(data):
            p += 1
print("B) gap hits by (section, kind):")
for k, v in sorted(B.items()):
    print("   %-7s %-22s %d" % (k[0], k[1], v))
# group gap hits by gap; report gaps that contain privileged hits, with gap size and a 'ret' count
bygap = collections.defaultdict(list)
for s in Bsites:
    bygap[(s[0], s[1], s[2])].append(s)
print("\nB) gaps with hits (section, start, end, size, #hits, kinds):")
for g, L in sorted(bygap.items(), key=lambda kv: kv[0][1]):
    kinds = collections.Counter(x[6] for x in L)
    print("   %-7s %08x-%08x %6d  %3d  %s" % (g[0], g[1], g[2], g[2] - g[1], len(L), dict(kinds.most_common(6))))

