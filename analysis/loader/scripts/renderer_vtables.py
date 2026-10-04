"""Dump NiRenderer / NiXBoxRenderer vtables from retail_eu default.xbe and
compare slot by slot.  Output: slot, NiRenderer impl, NiXBoxRenderer impl,
override flag, Ghidra name + size from functions.tsv."""
import sys, csv
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
funcs = {}
for r in csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"), delimiter="\t"):
    funcs[int(r["address"], 16)] = (r["name"], int(r["size"]))
NIR, NIX = 0x2a9ef0, 0x2aa7c0
for s in range(75):
    a = X.u32(NIX + 4 * s)
    p = X.u32(NIR + 4 * s) if s < 72 else None
    ov = "new" if p is None else ("same" if p == a else "OVR")
    n, sz = funcs.get(a, ("?", 0))
    pn = funcs.get(p, ("?", 0))[0] if p else "-"
    print(f"{s:2d}\t{a:08x}\t{(p or 0):08x}\t{ov}\t{n}\t{sz}\t{pn}")
