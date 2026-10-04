"""verify_d3d_inline_tables.py - non-D3D code reading D3D constant tables in .rdata
(0x293608 deferred dirty-flag table, 0x293828 simple-RS encode table, 0x293998 primitive->
vertex count table used by the inline DrawIndexedPrimitive/DrawPrimitive macros)."""
import collections
from capstone import CS_OP_MEM, CS_OP_IMM
from verify_d3d_common import *
print("0x293998 table (mul, add) per D3DPRIMITIVETYPE 0..10:",
      [(X.u32(0x293998 + 8 * i), X.u32(0x29399c + 8 * i)) for i in range(11)])
RANGES = [(0x293608, 0x293608 + 4 * 0x88, "DirtyFlagTable 0x293608"), (0x293828, 0x293828 + 4 * 0x5c, "SimpleEncode 0x293828"),
          (0x293998, 0x293998 + 8 * 11, "PrimToVertexCount 0x293998")]
hits = collections.defaultdict(list)
for s in [".text", "D3D", "XMV", "DSOUND", "XPP"]:
    va, vsz, raw, rsz = SEC[s]
    starts = sorted(set([f[0] for f in FUNCS if va <= f[0] < va + rsz] + [va]))
    for i, a in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else va + rsz
        for ins in MD.disasm(X.read(a, end - a), a):
            for op in ins.operands:
                v = op.mem.disp & 0xffffffff if op.type == CS_OP_MEM else (op.imm & 0xffffffff if op.type == CS_OP_IMM else None)
                if v is None: continue
                for lo, hi, nm in RANGES:
                    if lo <= v < hi or (nm.startswith("Dirty") and v == lo):
                        hits[(nm, s)].append((func_of(ins.address), ins.address, ins.mnemonic + " " + ins.op_str))
for (nm, s), l in sorted(hits.items()):
    fs = sorted({f[0] for f, a, t in l})
    print("%-28s %-6s %3d insns in %2d funcs: %s" % (nm, s, len(l), len(fs), " ".join("%08x" % f for f in fs) if s != "D3D" else ""))
    if s != "D3D":
        for f, a, t in l: print("     %08x %-26s %s" % (a, f[3], t))
