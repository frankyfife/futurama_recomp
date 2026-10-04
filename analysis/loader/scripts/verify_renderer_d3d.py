"""Independent recount of direct calls into the D3D section (renderer.md section 6).
Two sources: (a) callgraph.tsv, (b) raw E8 rel32 call / E9 jmp scan over .text
(decoding every function with capstone) so that callgraph gaps would show up.
Classes: REN = [0x146020,0x14cd50) u [0x1552c0,0x166871); NI = rest of 0xfd000..0x166871;
GAME = < 0xfd000; CRT = >= 0x166871 in .text; XLIB = other library sections."""
import collections, sys
from verify_renderer_common import *
REN = [(0x146020, 0x14cd50), (0x1552c0, 0x166871)]
def cls(a):
    blk = F.get(a, ("", 0, ""))[2] if a in F else X.section(a)
    if blk == "D3D": return "D3D"
    if blk != ".text": return "XLIB"
    if any(lo <= a < hi for lo, hi in REN): return "REN"
    if 0xfd000 <= a < 0x166871: return "NI"
    if a >= 0x166871: return "CRT"
    return "GAME"
D3DLO, D3DHI = 0x2342c0, 0x2342c0 + 0x12a38
d3d = {a for a, v in F.items() if v[2] == "D3D"}
# (a) callgraph
cg = collections.defaultdict(set)
for a in d3d:
    for c in callers(a):
        k = cls(c)
        if k != "D3D": cg[k].add(a)
# (b) raw scan of direct calls/jmps from non-D3D code
raw = collections.defaultdict(set); rawsites = collections.defaultdict(list)
for fa, (nm, sz, blk) in F.items():
    if blk == "D3D" or sz <= 0: continue
    for ins in dis(fa, sz):
        if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
            t = int(ins.op_str, 16)
            if D3DLO <= t < D3DHI:
                k = cls(fa); raw[k].add(t); rawsites[(k, t)].append(fa)
nm = lambda a: F.get(a, (f"?{a:#x}",))[0].replace("D3D8::", "")
for k in sorted(set(cg) | set(raw)):
    a, b = cg.get(k, set()), raw.get(k, set())
    print(f"{k}: callgraph {len(a)} ({sum(1 for x in a if F[x][0].startswith('D3D8::'))} named)  raw {len(b)} ({sum(1 for x in b if F.get(x,('',))[0].startswith('D3D8::'))} named)  only-raw {[nm(x) for x in b - a]} only-cg {[nm(x) for x in a - b]}")
    if k in ("GAME", "NI", "CRT", "XLIB"):
        for t in sorted(b):
            print(f"     {nm(t):45s} <- " + ", ".join(sorted({f'{f:#x}' for f in rawsites[(k, t)]})))
ren = raw["REN"]; other = set().union(*[v for kk, v in raw.items() if kk != "REN"])
print("REN direct only (raw):", len(ren - other), "named:", sum(1 for x in ren - other if F.get(x, ('',))[0].startswith("D3D8::")))
# data references to D3D functions from non-D3D code (function pointers)
print("named D3D8 functions total:", sum(1 for a in d3d if F[a][0].startswith("D3D8::")), "all D3D funcs:", len(d3d))

# transitive reachability (inside D3D) from external callers
ext = collections.defaultdict(set)   # d3d func -> set of external classes reaching it
from collections import deque
for k, s in raw.items():
    for t in s:
        pass
# BFS from every external direct callee down the D3D-internal callee graph
for k in raw:
    seen = set(); dq = deque(raw[k])
    while dq:
        a = dq.popleft()
        if a in seen: continue
        seen.add(a); ext[a].add(k)
        for c in callees(a):
            if c in d3d and c not in seen: dq.append(c)
named = [a for a in d3d if F[a][0].startswith("D3D8::")]
onlyren = [a for a in named if ext.get(a) == {"REN"}]
other = [a for a in named if ext.get(a) and ext[a] != {"REN"}]
none = [a for a in named if not ext.get(a)]
print(f"transitive named: only REN {len(onlyren)}, reached from non-REN {len(other)}, unreached {len(none)}")
print("  unreached:", ", ".join(nm(a) for a in sorted(none)))
alls = list(d3d)
print(f"transitive all: only REN {sum(1 for a in alls if ext.get(a)=={'REN'})}, other {sum(1 for a in alls if ext.get(a) and ext[a]!={'REN'})}, none {sum(1 for a in alls if not ext.get(a))}")
# imm32 references to D3D functions from outside D3D (function pointers / vtables)
import struct
fp = collections.Counter()
for n, va, vsz, rawo, rsz in X.sections:
    if n in ("D3D",): continue
    blob = X.b[rawo:rawo + rsz]
    for i in range(0, len(blob) - 3):
        v = struct.unpack_from("<I", blob, i)[0]
        if v in d3d: fp[(n, v)] += 1
print("imm/data references to D3D function entry points from other sections:", [(n, nm(v), c) for (n, v), c in sorted(fp.items())][:40])
