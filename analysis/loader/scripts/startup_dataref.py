"""Raw search for a 32-bit absolute address (operand/displacement) in code sections, reporting the containing
Ghidra function (or none). usage: startup_dataref.py 0xADDR ..."""
import sys, struct, bisect
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
funcs = []
for l in open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8"):
    p = l.rstrip("\n").split("\t")
    if p[0] == "address": continue
    funcs.append((int(p[0], 16), int(p[1]), p[3]))
funcs.sort(); starts = [f[0] for f in funcs]
def func_of(v):
    i = bisect.bisect_right(starts, v) - 1
    if i >= 0 and funcs[i][0] <= v < funcs[i][0] + funcs[i][1]: return "%x %s" % (funcs[i][0], funcs[i][2])
    return "<no func>"
def refs(a, secs=(".text", "D3D", "DSOUND", "XMV", "XPP")):
    pat = struct.pack("<I", a); out = []
    for name, va, vsz, raw, rsz in X.sections:
        if name not in secs: continue
        d = X.b[raw:raw + rsz]; p = d.find(pat)
        while p >= 0:
            out.append((va + p, func_of(va + p))); p = d.find(pat, p + 1)
    return out
if __name__ == "__main__":
    for a in sys.argv[1:]:
        r = refs(int(a, 16))
        print(a, len(r)); [print("   %08x %s" % x) for x in r[:30]]
