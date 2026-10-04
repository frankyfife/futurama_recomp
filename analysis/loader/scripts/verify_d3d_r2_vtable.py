"""verify_d3d_r2_vtable.py - NiXBoxRenderer vtable 0x2aa7c0 (review pass 2).
Dumps slots until a non-code value, counts distinct targets / NiXBoxRenderer::* names,
checks the vfunc -> address mapping used in d3d.md 4.1 and the NiXBox range sizes."""
import json, collections
from verify_d3d_r2_common import *

cls = json.load(open(ROOT + r"\analysis\retail_eu_ni_classes.json"))["classes"]
r = cls["NiXBoxRenderer"]
print("json: size=%#x vtable=%#x len=%d parent=%s" % (r["size"], r["vtable"], r["vtable_len"], r["parent"]))
VT = r["vtable"]
slots = []
i = 0
while True:
    v = u32(VT + 4 * i)
    if not (0x11000 <= v < D3D_LO):
        print("slot %d = %#x (not .text code) -> end" % (i, v))
        break
    slots.append(v)
    i += 1
print("slots:", len(slots))
names = {f[0]: f[3] for f in F}
for i, v in enumerate(slots):
    print("  vf%-2d %08x %s" % (i, v, names.get(v, "<not a Ghidra function start>")))
rs = slots[11:]
print("slots 11..%d: %d, distinct targets %d, distinct NiXBoxRenderer::* %d" % (
    len(slots) - 1, len(rs), len(set(rs)), len({v for v in rs if names.get(v, "").startswith("NiXBoxRenderer::")})))
print("all slots: distinct NiXBoxRenderer::*", len({v for v in slots if names.get(v, "").startswith("NiXBoxRenderer::")}))
claims = {11: 0x146350, 15: 0x146400, 16: 0x146460, 17: 0x146490, 24: 0x1466b0, 25: 0x146800, 42: 0x14b700,
          47: 0x1469e0, 52: 0x148ae0, 53: 0x149530, 54: 0x14b5f0, 27: None}
for k, a in claims.items():
    print("  claim vf%d=%s -> table %08x %s" % (k, hex(a) if a else "?", slots[k], "OK" if a is None or slots[k] == a else "MISMATCH"))
# NiXBox ranges
for lo, hi in ((0x146020, 0x14cd50), (0x155000, 0x166871)):
    fl = [f for f in F if lo <= f[0] < hi]
    print("range %06x-%06x: %d functions, %d bytes; first %s last %s, next after: %s" % (
        lo, hi, len(fl), sum(f[1] for f in fl), fl[0][3], fl[-1][3], [f[3] + "@%x" % f[0] for f in F if f[0] >= hi][:3]))
    print("   preceding:", [(hex(f[0]), f[1], f[3]) for f in F if f[0] < lo][-2:])
# named classes inside the NiOther gap 0x14cd50..0x155000
gapn = collections.Counter(f[3].split("::")[0] for f in F if 0x14cd50 <= f[0] < 0x155000 and "::" in f[3])
print("named classes in 0x14cd50-0x155000:", dict(gapn))
gapn = collections.Counter(f[3].split("::")[0] for f in F if 0x146020 <= f[0] < 0x14cd50 and "::" in f[3])
print("named classes in 0x146020-0x14cd50:", dict(gapn))
gapn = collections.Counter(f[3].split("::")[0] for f in F if 0x155000 <= f[0] < 0x166871 and "::" in f[3])
print("named classes in 0x155000-0x166871:", dict(gapn))
