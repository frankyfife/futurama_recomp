"""Correlate data-import thunk slots (no call edges) with the *_exref names Ghidra used in the decompiled C.
For each slot: functions whose code contains the slot address as a 32-bit operand, and the exref names in them."""
import sys, struct, re, bisect, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools"); sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from xbe import Xbe
import startup_getfunc as G
import startup_kimports as K
X = K.X
funcs = []
for l in open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8"):
    p = l.rstrip("\n").split("\t")
    if p[0] == "address": continue
    funcs.append((int(p[0], 16), int(p[1]), p[3]))
funcs.sort(); starts = [f[0] for f in funcs]
def func_of(v):
    i = bisect.bisect_right(starts, v) - 1
    if i >= 0 and funcs[i][0] <= v < funcs[i][0] + funcs[i][1]: return funcs[i]
for s, o in enumerate(K.slots):
    if s in K.names: continue
    addr = 0x293560 + 4 * s
    pat = struct.pack("<I", addr)
    hits = collections.Counter()
    for name, va, vsz, raw, rsz in X.sections:
        if name not in (".text", "D3D", "DSOUND", "XMV", "XPP"): continue
        d = X.b[raw:raw + rsz]; p = d.find(pat)
        while p >= 0:
            f = func_of(va + p)
            hits[(f[0], f[2]) if f else (va + p, "<no func>")] += 1
            p = d.find(pat, p + 1)
    cand = collections.Counter()
    for (fa, fn), c in hits.items():
        src = G.get(fa) or ""
        for n in set(re.findall(r"\b(\w+)_exref\b", src)) | set(re.findall(r"xboxkrnl\.exe::(\w+)", fn)):
            cand[n] += 1
    print("slot %3d ord %3d  refs in %s  -> exref candidates %s" % (s, o, ["%x %s" % k for k in list(hits)[:4]], dict(cand)))
