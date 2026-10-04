"""Task 4: which D3D-section functions (named D3D8::* or unnamed FUN_ in section D3D)
are called from where. Callers outside the D3D section are classified as
  REN  = NiXBoxRenderer module (address ranges below, verified by names/strings)
  NI   = other NetImmerse core
  GAME = game code (< 0xfd000)
  XLIB = other XDK library sections (XMV, DSOUND, XPP) / CRT
Transitive: a D3D function counts as reachable from a class if any direct
non-D3D caller of it, or of a D3D function that (transitively, inside D3D)
calls it, is in that class."""
import csv, collections, sys
F = {}
for r in csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"), delimiter="\t"):
    F[int(r["address"], 16)] = (r["name"], int(r["size"]), r["block"])
callers = collections.defaultdict(set); callees = collections.defaultdict(set)
for r in csv.reader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\callgraph.tsv"), delimiter="\t"):
    if r[0] == "caller" or not all(c in "0123456789abcdef" for c in r[0] + r[2]): continue
    a, b = int(r[0], 16), int(r[2], 16)
    callers[b].add(a); callees[a].add(b)
# NiXBoxRenderer module ranges (see renderer.md section 2): 0x146020-0x14cd50 (vtable impls, ctor, dtor,
# Create, helpers, D3D error strings), 0x1552c0-0x166871 (fences/texture cache, VB/IB managers, render state,
# texture pipeline, light manager, texture data classes, palette/blt data, texture manager, memory pool,
# shader/material helpers) - boundaries derived from callgraph + debug strings
REN = [(0x146020, 0x14cd50), (0x1552c0, 0x166871)]
def cls(a):
    blk = F.get(a, ("", 0, ""))[2]
    if blk == "D3D": return "D3D"
    if blk != ".text": return "XLIB"
    if any(lo <= a < hi for lo, hi in REN): return "REN"
    if 0xfd000 <= a < 0x166000: return "NI"
    if a >= 0x166000: return "CRT"
    return "GAME"
d3d = [a for a, v in F.items() if v[2] == "D3D"]
# direct external callers
direct = {a: collections.Counter(cls(c) for c in callers[a] if cls(c) != "D3D") for a in d3d}
# transitive closure inside D3D: who (externally) reaches a D3D function
reach = {}
def ext_reach(a, seen=None):
    if a in reach: return reach[a]
    seen = seen or set(); seen.add(a)
    s = set()
    for c in callers[a]:
        k = cls(c)
        if k != "D3D": s.add((k, c))
        elif c not in seen: s |= ext_reach(c, seen)
    reach[a] = s
    return s
rows = []
for a in sorted(d3d):
    s = ext_reach(a)
    kinds = sorted({k for k, _ in s})
    rows.append((a, F[a][0], kinds, sorted({c for k, c in s if k != "REN"})))
if __name__ == "__main__":
    import json
    mode = sys.argv[1] if len(sys.argv) > 1 else "summary"
    named = [r for r in rows if r[1].startswith("D3D8::")]
    def summarize(rs, label):
        only_ren = [r for r in rs if r[2] == ["REN"]]
        none = [r for r in rs if not r[2]]
        other = [r for r in rs if r[2] and r[2] != ["REN"]]
        print(f"{label}: total {len(rs)}  only-REN {len(only_ren)}  REN+other/other-only {len(other)}  no external caller {len(none)}")
        return only_ren, other, none
    o, b, n = summarize(named, "named D3D8::*")
    summarize(rows, "all D3D-section functions")
    if mode == "detail":
        print("\n# reached from outside NiXBoxRenderer:")
        for a, nm, k, cs in b:
            print(f"{a:08x}\t{nm}\t{','.join(k)}\t" + ",".join(f"{c:08x}" for c in cs[:12]))
        print("\n# only REN:")
        print(", ".join(r[1].replace("D3D8::", "") for r in o))
        print("\n# no external caller:")
        print(", ".join(r[1].replace("D3D8::", "") for r in n))

def direct_summary():
    """Direct (first-level) calls from outside the D3D section into D3D-section functions."""
    out = collections.defaultdict(set)
    for a in d3d:
        for c in callers[a]:
            k = cls(c)
            if k != "D3D":
                out[k].add(a)
    return out
if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "direct":
    ds = direct_summary()
    for k, s in sorted(ds.items()):
        named = sorted(F[a][0].replace("D3D8::", "") for a in s if F[a][0].startswith("D3D8::"))
        print(f"{k}: {len(s)} D3D-section functions called directly ({len(named)} named)")
        if k != "REN": print("   ", ", ".join(named), "| unnamed:", ", ".join(f"{a:08x}" for a in s if not F[a][0].startswith("D3D8::")))
    only_ren = ds["REN"] - set().union(*[v for k, v in ds.items() if k != "REN"])
    print("called directly only by REN:", len(only_ren), "named:", sum(1 for a in only_ren if F[a][0].startswith("D3D8::")))
