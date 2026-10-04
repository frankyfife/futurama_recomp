"""Find vtable-like pointer arrays in .rdata whose entries point into the
NiXBoxRenderer module (0x146000-0x166000) and show which code stores them
(mov [reg], imm32 -> constructor/destructor)."""
import sys, struct, csv
sys.path.insert(0, r"D:\FuturamaDecomp\tools"); sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from xbe import Xbe
from renderer_xref import find_refs
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
F = {int(r["address"], 16): (r["name"], int(r["size"])) for r in csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"), delimiter="\t")}
FA = sorted(F)
import bisect
def func_of(a):
    i = bisect.bisect_right(FA, a) - 1
    if i >= 0 and FA[i] <= a < FA[i] + max(F[FA[i]][1], 1): return FA[i]
    return None
LO, HI = 0x146000, 0x166000
rd = [s for s in X.sections if s[0] == ".rdata"][0]
_, va, vsz, raw, rsz = rd
def is_code(p): return 0x11000 <= p < 0x2342c0
v = va
res = []
while v < va + rsz:
    p = X.u32(v)
    if is_code(p) and p in F:
        # run
        run = []
        w = v
        while w < va + rsz and is_code(X.u32(w)) and X.u32(w) in F:
            run.append(X.u32(w)); w += 4
        inmod = sum(1 for q in run if LO <= q < HI)
        if inmod >= max(1, len(run) // 2) and len(run) >= 2 and 0x2a9000 <= v:
            refs = [r for r in find_refs(v) if r[1] is not None]
            res.append((v, run, refs))
        v = w
    else:
        v += 4
for v, run, refs in res:
    stores = sorted({(func_of(r[1].address) or 0, r[1].mnemonic) for r in refs})
    if not stores: continue
    print(f"vtable {v:08x} len {len(run)} first {run[0]:08x}  refs: " + ", ".join(f"{(s[0] or 0):08x}({s[1]})" for s in stores))
