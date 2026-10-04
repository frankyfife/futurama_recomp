"""Transitive reachability of D3D-section functions from outside callers, computed
upward WITHOUT memoisation (renderer_d3dusage.py memoises partial results inside
cycles). For each named D3D8 function: set of external classes whose code can reach it
through D3D-internal calls; prints a path for the functions whose class differs
from renderer_d3dusage.py's result."""
import collections
from verify_renderer_common import *
REN = [(0x146020, 0x14cd50), (0x1552c0, 0x166871)]
def cls(a):
    blk = F.get(a, ("", 0, ""))[2]
    if blk == "D3D": return "D3D"
    if blk != ".text": return "XLIB"
    if any(lo <= a < hi for lo, hi in REN): return "REN"
    if 0xfd000 <= a < 0x166871: return "NI"
    if a >= 0x166871: return "CRT"
    return "GAME"
d3d = {a for a, v in F.items() if v[2] == "D3D"}
def up(a):
    seen = {a}; st = [(a, [a])]; out = {}
    while st:
        x, path = st.pop()
        for c in callers(x):
            k = cls(c)
            if k != "D3D":
                out.setdefault(k, path + [c])
            elif c not in seen:
                seen.add(c); st.append((c, path + [c]))
    return out
nm = lambda a: F.get(a, (f"{a:#x}",))[0].replace("D3D8::", "")
named = sorted(a for a in d3d if F[a][0].startswith("D3D8::"))
res = {a: up(a) for a in d3d}
only = [a for a in named if set(res[a]) == {"REN"}]
oth = [a for a in named if res[a] and set(res[a]) != {"REN"}]
none = [a for a in named if not res[a]]
print(f"named {len(named)}: only REN {len(only)}, reached from non-REN {len(oth)}, unreached {len(none)}")
print("unreached:", [nm(a) for a in none])
print(f"all {len(d3d)}: only REN {sum(1 for a in d3d if set(res[a])=={'REN'})}, other {sum(1 for a in d3d if res[a] and set(res[a])!={'REN'})}, none {sum(1 for a in d3d if not res[a])}")
for a in named:
    if nm(a) in ("D3DDevice_Begin", "D3DDevice_SetVertexData2f", "D3DDevice_LoadVertexShader", "D3D_AllocContiguousMemory"):
        print(nm(a), {k: " -> ".join(nm(p) for p in v) for k, v in res[a].items()})
print("reached from non-REN (named):")
for a in oth: print("  ", nm(a), sorted(res[a]))
