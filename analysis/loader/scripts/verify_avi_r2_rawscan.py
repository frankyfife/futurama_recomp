"""Round-2 raw call-site scan for DSOUND/XMV/XPP entry points.

Independent method: linear-sweep instruction starts (capstone) over each
executable section; every E8/E9 rel32 whose target lies in DSOUND/XMV/XPP and
which is a linear-sweep instruction start is counted. Compares with Ghidra
callgraph.tsv. Also scans for function pointers (imm32 / data dwords) to the
entry points.
"""
import collections
import os
import struct
from verify_avi_r2_common import X, SECT, FNAME, FSTART, func_of, linear_starts, Tee, EXP, u32

T = Tee("verify_avi_r2_rawscan.out")
CODE = [".text", "D3D", "DSOUND", "XMV", "XPP"]
LIBS = ["DSOUND", "XMV", "XPP"]
rng = {n: (SECT[n][0], SECT[n][0] + SECT[n][1]) for n in SECT}
rawlen = {n: SECT[n][3] for n in SECT}


def secof(va):
    for n, (a, b) in rng.items():
        if a <= va < b:
            return n
    return None


starts = {}
for n in CODE:
    a, b = rng[n][0], rng[n][0] + rawlen[n]
    starts[n] = linear_starts(a, b)
    T("linear sweep %-6s %08x..%08x  %d insns" % (n, a, b, len(starts[n])))

fset = set(FSTART)
sites = collections.defaultdict(list)   # target -> [(site, srcsec, op)]
odd = []
for n in CODE:
    a = rng[n][0]
    data = X.read(a, rawlen[n])
    for off in range(len(data) - 5):
        op = data[off]
        if op not in (0xE8, 0xE9):
            continue
        site = a + off
        tgt = (site + 5 + struct.unpack_from("<i", data, off + 1)[0]) & 0xFFFFFFFF
        ts = secof(tgt)
        if ts not in LIBS or ts == n:
            continue
        if site not in starts[n]:
            if tgt in fset:
                odd.append((site, n, tgt, "not-linear-start"))
            continue
        if tgt not in fset:
            odd.append((site, n, tgt, "target-not-func-start"))
            continue
        sites[tgt].append((site, n, "call" if op == 0xE8 else "jmp"))

T("\n== targets reached from outside their own section ==")
tot = collections.Counter()
for tgt in sorted(sites):
    lst = sites[tgt]
    c = collections.Counter(s for _, s, _ in lst)
    for s, k in c.items():
        tot[(secof(tgt), s)] += k
    desc = []
    for site, s, op in sorted(lst):
        fa, fn = func_of(site)
        desc.append("%08x(%s%s%s)" % (site, s, ":" + fn if fn else ":nofunc", "" if op == "call" else ",JMP"))
    T("%08x %-45s %-6s n=%-3d %s" % (tgt, FNAME.get(tgt, "?"), secof(tgt), len(lst), " ".join(desc)))
T("\n== totals (target section <- source section) ==")
for k, v in sorted(tot.items()):
    T("  %s <- %s : %d" % (k[0], k[1], v))
T("distinct targets per lib:", {l: len([t for t in sites if secof(t) == l]) for l in LIBS})

T("\n== candidates rejected (E8/E9 into lib, but not validated) ==")
for o in odd:
    T("  %08x %s -> %08x %s %s" % (o[0], o[1], o[2], FNAME.get(o[2], "?"), o[3]))

# Ghidra callgraph comparison
cg = collections.defaultdict(set)
with open(os.path.join(EXP, "callgraph.tsv"), encoding="utf-8") as f:
    next(f)
    for line in f:
        c, cn, e, en = line.rstrip("\n").split("\t")
        try:
            c, e = int(c, 16), int(e, 16)
        except ValueError:
            continue
        if secof(e) in LIBS and secof(c) != secof(e):
            cg[secof(e)].add((c, e))
T("\n== Ghidra callgraph edges into libs from other sections ==")
for l in LIBS:
    src = collections.Counter(secof(c) for c, e in cg[l])
    T("  %s: %d edges %s" % (l, len(cg[l]), dict(src)))

# function pointers to entry points: dwords anywhere in the image equal to a target
T("\n== dwords equal to an entry address (all sections, any alignment) ==")
ent = set(sites)
img = X.b
for n, (a, b) in rng.items():
    raw = SECT[n][2]
    data = img[raw:raw + rawlen[n]]
    for off in range(len(data) - 3):
        v = struct.unpack_from("<I", data, off)[0]
        if v in ent:
            va = a + off
            ok = ""
            if n in starts:
                # instruction containing it?
                for back in range(1, 8):
                    if va - back in starts[n]:
                        ok = "insn@%08x" % (va - back)
                        break
            T("  %08x (%s) = %08x %s %s" % (va, n, v, FNAME.get(v, "?"), ok))
