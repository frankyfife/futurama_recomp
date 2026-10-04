"""verify_d3d_callers.py - list every external D3D call site (raw E8/E9 scan, validated
against capstone at the site), attribute it to functions.tsv (inside-size or beyond end
of the preceding function), group callers by address range / name, check NiXBox ranges."""
import struct, collections, sys
from verify_d3d_common import *

sites = []
for s in [".text", "DSOUND", "XMV", "XPP"]:
    va, b = sec_bytes(s)
    for i in range(len(b) - 5):
        if b[i] in (0xE8, 0xE9):
            t = (va + i + 5 + struct.unpack_from("<i", b, i + 1)[0]) & 0xffffffff
            if t in D3DSET:
                sites.append((va + i, t, s, b[i]))
rows = []
beyond = []
for a, t, s, op in sites:
    f = func_of(a)
    inside = a < f[0] + f[1]
    rows.append((a, t, s, op, f, inside))
    if not inside: beyond.append((a, t, f))
print("sites", len(sites), "jmp(E9):", sum(1 for r in rows if r[3] == 0xE9))
print("sites beyond end of preceding functions.tsv function:", len(beyond))
gaps = collections.defaultdict(list)
for a, t, f in beyond:
    gaps[f[0]].append(a)
for fa, lst in sorted(gaps.items()):
    f = BYADDR[fa]
    print("   after %08x %s (size %d, end %08x): sites %s" % (fa, f[3], f[1], fa + f[1], " ".join("%08x" % x for x in lst)))
callers_inside = {r[4][0] for r in rows if r[5]}
print("distinct callers (inside-size):", len(callers_inside))
# group
def grp(f, s):
    a, sz, blk, nm = f
    if s == "XMV": return "XMV"
    if nm.startswith("NiXBoxRenderer::"): return "NiXBoxRenderer"
    if 0x146020 <= a < 0x14cd50 or 0x155000 <= a < 0x166871: return "NiXBoxLib"
    if nm.startswith("Ni") or "::" in nm: return "OtherNamed:" + nm.split("::")[0]
    if a < 0xf3000: return "Game"
    return "Other"
g = collections.defaultdict(set)
for r in rows:
    if r[5]: g[grp(r[4], r[2])].add(r[4][0])
for k, v in sorted(g.items()):
    print("  %-20s %d" % (k, len(v)))
print("Game callers:", " ".join("%08x" % x for x in sorted(g.get("Game", []))))
print("XMV callers:", " ".join("%08x" % x for x in sorted(g.get("XMV", []))))
print("Renderer:", " ".join("%08x:%s" % (x, BYADDR[x][3].split('::')[1]) for x in sorted(g.get("NiXBoxRenderer", []))))
# extent of callers
lib = sorted(g.get("NiXBoxLib", []))
print("NiXBoxLib caller extent: %08x..%08x" % (lib[0], lib[-1]))
