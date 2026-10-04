"""verify_d3d_kimports.py - kernel imports used by D3D-section code (indirect call/jmp/mov
through the thunk table 0x293560, decoded with capstone) and whether any non-D3D code
uses the same thunk slot. Ordinal names via kernel_common.py (if present)."""
import collections, sys, os
from capstone import CS_OP_MEM
from verify_d3d_common import *
try:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import kernel_common as kc
    NAMES = getattr(kc, "KNAMES", None) or getattr(kc, "ORDINALS", None) or {}
except Exception:
    NAMES = {}
if not NAMES:
    import re
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "kernel_common.py")).read()
    NAMES = {int(a): b for a, b in re.findall(r'(\d+): "(\w+)"', src)}
T0 = 0x293560
use = collections.defaultdict(lambda: collections.defaultdict(set))
for s in [".text", "D3D", "DSOUND", "XMV", "XPP"]:
    va, vsz, raw, rsz = SEC[s]
    starts = sorted(set([f[0] for f in FUNCS if va <= f[0] < va + rsz] + [va]))
    for i, a in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else va + rsz
        for ins in MD.disasm(X.read(a, end - a), a):
            for op in ins.operands:
                if op.type == CS_OP_MEM and not op.mem.base and not op.mem.index:
                    v = op.mem.disp & 0xffffffff
                    if T0 <= v < T0 + 4 * 112 and (v - T0) % 4 == 0:
                        use[v][s].add(func_of(ins.address)[0])
d3donly, shared = [], []
for slot in sorted(use):
    o = X.u32(slot) & 0x7fffffff
    secs = use[slot]
    if "D3D" in secs:
        nm = NAMES.get(o, "?")
        others = {k: len(v) for k, v in secs.items() if k != "D3D"}
        (shared if others else d3donly).append((slot, o, nm, len(secs["D3D"]), others))
print("kernel imports used ONLY by D3D:")
for slot, o, nm, n, ot in d3donly: print("   %08x ord %3d %-32s D3D funcs %d" % (slot, o, nm, n))
print("kernel imports used by D3D and others:")
for slot, o, nm, n, ot in shared: print("   %08x ord %3d %-32s D3D funcs %d others %s" % (slot, o, nm, n, ot))
