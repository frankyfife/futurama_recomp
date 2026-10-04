"""verify_d3d_simple.py - every external call of SetRenderState_Simple 0x2357d0:
find the last instruction writing ecx (within 40 bytes, linear back-decode by trying
start offsets) and map constant NV2A headers back through table 0x293828 (0..0x5b)."""
import struct, collections
from verify_d3d_common import *
T = {X.u32(0x293828 + 4 * i): i for i in range(0x5c)}
print("table 0x293828 first entries:", [hex(X.u32(0x293828 + 4 * i)) for i in range(4)], "dup?", len(T) != 0x5c)
va, b = sec_bytes(".text")
sites = []
for i in range(len(b) - 5):
    if b[i] in (0xE8, 0xE9) and (va + i + 5 + struct.unpack_from("<i", b, i + 1)[0]) & 0xffffffff == 0x2357d0:
        sites.append(va + i)
print("sites:", len(sites))
fn = collections.Counter()
consts = collections.Counter()
for s in sites:
    f = func_of(s); fn[f[0]] += 1
    # decode from function start to the site, track last write to ecx
    insns = dis(f[0], s - f[0] + 5)
    last = None
    for ins in insns:
        if ins.address >= s: break
        if ins.op_str.startswith("ecx,") or ins.op_str == "ecx" and ins.mnemonic in ("pop",):
            last = ins
    txt = "%s %s" % (last.mnemonic, last.op_str) if last else "?"
    val = None
    if last is not None and last.mnemonic == "mov" and last.op_str.startswith("ecx, 0x"):
        val = int(last.op_str.split(",")[1], 16)
    st = T.get(val)
    consts[val] += 1
    print("  %08x in %08x  ecx <- %-40s state=%s" % (s, f[0], txt, hex(st) if st is not None else "-"))
print("functions:", len(fn), {"%08x" % k: v for k, v in fn.items()})
print("distinct constants:", sorted(hex(k) for k in consts if k is not None), "variable:", consts[None])
