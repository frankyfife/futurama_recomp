"""Dump vtable of a NetImmerse class (from retail_eu_ni_classes.json) with function names."""
import sys, json, csv
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
C = json.load(open(r"D:\FuturamaDecomp\analysis\retail_eu_ni_classes.json"))["classes"]
F = {int(r["address"], 16): (r["name"], int(r["size"])) for r in csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"), delimiter="\t")}
def vt(name):
    c = C[name]
    return [X.u32(c["vtable"] + 4 * i) for i in range(c["vtable_len"])]
if __name__ == "__main__":
    for n in sys.argv[1:]:
        p = C[n].get("parent")
        pv = vt(p) if p in C and "vtable" in C[p] else []
        print("==", n, hex(C[n]["vtable"]), "parent", p)
        for i, a in enumerate(vt(n)):
            ov = "" if i < len(pv) and pv[i] == a else ("NEW" if i >= len(pv) else "ovr")
            print(f"  {i:2d} {a:08x} {ov:3s} {F.get(a, ('?',0))[0]} ({F.get(a, ('?',0))[1]})")
