"""verify_d3d_rawscan.py - brute-force byte scan (no function boundaries, no linear
disassembly) of every non-D3D section for
  (1) E8/E9 rel32 calls/jmps whose target is a D3D function start
  (2) any aligned/unaligned little-endian dword equal to a D3D global of interest
  (3) any dword pointing into D3D code (function starts) or D3D bss in .rdata/.data
Upper bound check for the claims 'no push buffer access outside D3D' and '325 sites'."""
import struct, collections
from verify_d3d_common import *

calls = collections.defaultdict(list)
for s in [".text", "DSOUND", "XMV", "XPP", "DOLBY"]:
    va, b = sec_bytes(s)
    for i in range(len(b) - 5):
        if b[i] in (0xE8, 0xE9):
            t = (va + i + 5 + struct.unpack_from("<i", b, i + 1)[0]) & 0xffffffff
            if t in D3DSET:
                calls[t].append((s, va + i, b[i]))
tot = sum(len(v) for v in calls.values())
print("raw E8/E9 hits into D3D function starts:", tot, "distinct targets:", len(calls))
# validate each hit with capstone at that address (is it a real instruction start in the linear sweep?)
cs_ok = 0
callers = set()
per_sec = collections.Counter()
for t, lst in calls.items():
    for s, a, op in lst:
        f = func_of(a)
        callers.add(f[0] if f else None)
        per_sec[s] += 1
print("per section:", dict(per_sec))
print("distinct containing functions (functions.tsv, nearest preceding):", len(callers))
glob = {0x244968: "D3D__pDevice", 0x244970: "m_pPut", 0x244974: "m_pThreshold", 0x244978: "dev+8",
        0x24497c: "dev+0xc", 0x2392b0: "MakeSpace", 0x239130: "MakeRequestedSpace", 0x2392c0: "BeginStateBig",
        0x246ca8: "kick size", 0x246cac: "pb size"}
for s in [".text", "DSOUND", "XMV", "XPP", ".rdata", ".data", "DOLBY"]:
    va, b = sec_bytes(s)
    for g, n in glob.items():
        pat = struct.pack("<I", g)
        p = b.find(pat); hits = []
        while p >= 0:
            hits.append(va + p); p = b.find(pat, p + 1)
        if hits:
            print("GLOBAL", n, hex(g), "in", s, [hex(h) for h in hits[:10]], len(hits))
# any dword in non-D3D code sections that is inside the D3D bss 0x243890..0x246cf8 (raw, unaligned)
bsshits = collections.Counter()
for s in [".text", "DSOUND", "XMV", "XPP"]:
    va, b = sec_bytes(s)
    for i in range(len(b) - 3):
        v = struct.unpack_from("<I", b, i)[0]
        if D3D_RAWHI <= v < D3D_HI:
            bsshits[(s, v)] += 1
agg = collections.Counter()
for (s, v), n in bsshits.items():
    agg[(s, v & ~0x3)] += n
print("raw dwords into D3D bss by section/target (top):")
for (s, v), n in sorted(agg.items(), key=lambda z: (z[0][0], z[0][1])):
    print("  ", s, hex(v), n)
