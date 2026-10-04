"""verify_d3d_rstables.py - complex render state function table 0x2420b0[0x88..0xa5]
(=0x2422d0..), who calls each setter from outside D3D (raw E8/E9), and whether the
NiXBox dispatcher 0x1478c0 reaches all of them."""
import struct, collections
from verify_d3d_common import *
ent = [(i, X.u32(0x2420b0 + 4 * i)) for i in range(0x88, 0xa6)]
print("next dword after table (0xa6):", hex(X.u32(0x2420b0 + 4 * 0xa6)))
callers = collections.defaultdict(list)
for s in [".text", "XMV", "DSOUND", "XPP"]:
    va, b = sec_bytes(s)
    for i in range(len(b) - 5):
        if b[i] in (0xE8, 0xE9):
            t = (va + i + 5 + struct.unpack_from("<i", b, i + 1)[0]) & 0xffffffff
            if t in D3DSET:
                callers[t].append(func_of(va + i)[0])
missing = []
for i, f in ent:
    nm = BYADDR[f][3] if f in BYADDR else "?"
    cs = callers.get(f, [])
    if 0x1478c0 not in cs: missing.append(nm)
    print("  0x%02x %08x %-50s ext=%d by1478c0=%s others=%s" % (i, f, nm, len(cs), 0x1478c0 in cs,
          sorted({"%08x" % c for c in cs if c != 0x1478c0})))
print("complex setters NOT called by 0x1478c0:", missing)
# all SetRenderState_* functions in D3D and whether they are complex-table entries
tab = {f for i, f in ent}
for a, sz, blk, nm in D3DF:
    if "SetRenderState_" in nm and a not in tab:
        print("  non-table setter:", hex(a), nm, "ext callers:", len(callers.get(a, [])), sorted({"%08x" % c for c in callers.get(a, [])}))
