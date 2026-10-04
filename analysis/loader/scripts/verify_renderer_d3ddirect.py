"""Independent raw re-count of direct D3D-section calls (renderer.md section 6,
key facts 10/11): linear sweep of every function outside the D3D section, every
`call/jmp rel32` whose target lies in D3D [0x2342c0, 0x246cf8) is attributed
to REN / NI / GAME / XLIB (other XDK sections) by the caller address.
Also re-counts transitive reachability downward (BFS over callgraph.tsv
restricted to the D3D section) from the REN and non-REN direct targets.
With argument 'gaps' the code that Ghidra did not assign to any function
(.text gaps, decoded linearly from the first non-padding byte) is included,
attributed to a pseudo-function at the gap start.
Usage: python verify_renderer_d3ddirect.py [gaps]"""
import collections, sys
from verify_renderer_common import *
D3D_LO, D3D_HI = 0x2342c0, 0x2342c0 + 0x12a38
def region(a):
    if 0x146020 <= a < 0x14cd50 or 0x1552c0 <= a < 0x166871: return "REN"
    if 0xfd000 <= a < 0x146020 or 0x14cd50 <= a < 0x1552c0: return "NI"
    if 0x11000 <= a < 0xfd000: return "GAME"
    if 0x166871 <= a < 0x2342c0: return "CRT/other"
    return "XLIB"
direct = collections.defaultdict(lambda: collections.defaultdict(set))
for fa, (nm, sz, blk) in F.items():
    if D3D_LO <= fa < D3D_HI or sz <= 0: continue
    for i in dis(fa, sz):
        if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x"):
            t = int(i.op_str, 16)
            if D3D_LO <= t < D3D_HI and not (D3D_LO <= fa < D3D_HI):
                direct[region(fa)][t].add(fa)
if len(sys.argv) > 1 and sys.argv[1] == "gaps":
    iv = sorted((a, a + F[a][1]) for a in F if F[a][2] == ".text" and F[a][1] > 0)
    cur = 0x11000
    gaps = []
    for a, b in iv:
        if a > cur: gaps.append((cur, a))
        cur = max(cur, b)
    for a, b in gaps:
        data = X.read(a, b - a); k = 0
        while k < len(data) and data[k] in (0x90, 0xcc, 0x00): k += 1
        if k >= len(data): continue
        for i in md.disasm(data[k:], a + k):
            if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x"):
                t = int(i.op_str, 16)
                if D3D_LO <= t < D3D_HI: direct[region(i.address)][t].add(a + k)
named = {a for a in F if D3D_LO <= a < D3D_HI and F[a][0].startswith("D3D8::")}
allD = {a for a in F if D3D_LO <= a < D3D_HI}
print("D3D section functions", len(allD), "named D3D8::", len(named))
for r in ("REN", "NI", "GAME", "XLIB", "CRT/other"):
    ts = direct.get(r, {})
    print(f"\n{r}: {len(ts)} direct targets, {len(set(ts) & named)} named")
    if r != "REN":
        for t in sorted(ts): print(f"   {t:#x} {F.get(t, ('?',))[0]} <- {' '.join(hex(c) for c in sorted(ts[t]))}")
ren = set(direct["REN"]); oth = set().union(*(set(direct[r]) for r in direct if r != "REN"))
print("\nnamed direct REN-only:", len((ren - oth) & named), " named direct shared with others:", sorted(F[t][0] for t in (ren & oth & named)))
def bfs(starts):
    seen, q = set(starts), list(starts)
    while q:
        a = q.pop()
        for c in callees(a):
            if D3D_LO <= c < D3D_HI and c not in seen: seen.add(c); q.append(c)
    return seen
R, O = bfs(ren), bfs(oth)
print(f"transitive: all D3D funcs reachable only from REN {len(R - O)}, from others too {len(O)}, unreachable {len(allD - R - O)}")
print(f"transitive named: only REN {len((R - O) & named)}, also/only others {len(O & named)}, unreachable {len(named - R - O)}:",
      sorted(F[a][0] for a in named - R - O))
