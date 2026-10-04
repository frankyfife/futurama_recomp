"""Direct (non-virtual) call edges between the renderer module REN and the rest
(callgraph.tsv): GAME/NI -> REN and REN -> GAME. Shows which renderer internals a
NiRenderer-level replacement would also have to cover or keep."""
import collections
from verify_renderer_common import *
REN = [(0x146020, 0x14cd50), (0x1552c0, 0x166871)]
def cls(a):
    blk = F.get(a, ("", 0, ""))[2]
    if blk != ".text": return blk or "?"
    if any(lo <= a < hi for lo, hi in REN): return "REN"
    if 0xfd000 <= a < 0x166871: return "NI"
    if a >= 0x166871: return "CRT"
    return "GAME"
into = collections.defaultdict(set); outof = collections.defaultdict(set)
for a, (nm, sz, blk) in F.items():
    if cls(a) != "REN": continue
    for c in callers(a):
        if cls(c) in ("GAME", "NI"): into[a].add(c)
    for t in callees(a):
        if cls(t) == "GAME": outof[t].add(a)
nm = lambda a: f"{a:#x} {F.get(a, ('?',))[0]}"
print("== calls INTO REN from GAME/NI:")
for a in sorted(into): print(" ", nm(a), "<-", ", ".join(f"{cls(c)}:{c:#x}" for c in sorted(into[a])))
print("== calls FROM REN into GAME range:")
for t in sorted(outof): print(" ", nm(t), "<-", ", ".join(f"{c:#x}" for c in sorted(outof[t])))
