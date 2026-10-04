"""verify_d3d_r2_tables.py - constant D3D tables (review pass 2):
 0x293998 primitive -> (factor, offset) vertex count table (used inline by NiXBox draw paths)
 0x293608[0x5c..0x87] deferred render state dirty flags (effective 0x293778..0x293828)
 0x2420b0[0x88..0xa5] complex render state setter table (0x2422d0..0x242344) + external callers
 0x2422d0..: names of the 30 setters; who calls them from outside D3D"""
import collections
from verify_d3d_r2_common import *
print("0x293998:", [(u32(0x293998 + 8 * i), u32(0x29399c + 8 * i)) for i in range(11)])
print("deferred dirty flags 0x5c..0x87:", [hex(u32(0x293608 + 4 * i)) for i in range(0x5c, 0x88)])
print("start of deferred data:", hex(0x293608 + 4 * 0x5c), "end:", hex(0x293608 + 4 * 0x88))
ext = collections.defaultdict(set)
for s in [".text", "DSOUND", "XMV", "XPP"]:
    for ins in sweep(s):
        if ins.mnemonic in ("call", "jmp") and ins.operands and ins.operands[0].type == X86_OP_IMM:
            t = ins.operands[0].imm & 0xffffffff
            if t in D3DFN:
                ext[t].add(owner(ins.address)[0])
cx = []
for i in range(0x88, 0xa6):
    slot = 0x2420b0 + 4 * i
    v = u32(slot)
    nm = D3DFN[v][3] if v in D3DFN else "?"
    cx.append(v)
    print("  state %#x slot %08x -> %08x %-55s ext callers: %s" % (i, slot, v, nm, sorted(hex(c) for c in ext.get(v, []))))
print("complex setters:", len(cx), "distinct:", len(set(cx)), "all called by 0x1478c0:", all(0x1478c0 in ext.get(v, ()) for v in cx))
print("entry 0xa6 ->", hex(u32(0x2420b0 + 4 * 0xa6)), " entry 0x87 ->", hex(u32(0x2420b0 + 4 * 0x87)))
