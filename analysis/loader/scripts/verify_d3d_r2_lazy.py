"""verify_d3d_r2_lazy.py - inline accesses of non-D3D code to D3D BSS (review pass 2).

Every operand (memory displacement or immediate) of every instruction in .text/DSOUND/
XMV/XPP that falls into the D3D BSS (raw end .. virtual end of section D3D) is
classified by target (IndexData/DirtyFlags/TextureState/RenderState/other) and by
capstone access flags (read / write / read-modify-write / address-only immediate).
Also lists the written RenderState / TextureState indices (constant displacements)."""
import collections
from verify_d3d_r2_common import *

def tgt(v):
    if v == 0x2444c4: return "IndexData"
    if v == 0x2444c8: return "DirtyFlags"
    if 0x2444d0 <= v < 0x2446d0: return "TextureState"
    if 0x2446d0 <= v < 0x2446d0 + 4 * 0xa6: return "RenderState"
    return "OTHER_%08x" % v

acc = collections.Counter()
perf = collections.defaultdict(collections.Counter)
rs_idx, ts_idx = collections.Counter(), collections.Counter()
other = []
for s in [".text", "DSOUND", "XMV", "XPP"]:
    for ins in sweep(s):
        for op in ins.operands:
            v = None
            if op.type == X86_OP_MEM:
                v = op.mem.disp & 0xffffffff
                kind = "mem"
            elif op.type == X86_OP_IMM:
                v = op.imm & 0xffffffff
                kind = "imm"
            if v is None or not (D3D_CODE_END <= v < D3D_END):
                continue
            t = tgt(v)
            if kind == "imm":
                a = "addr"
            else:
                r, w = bool(op.access & 1), bool(op.access & 2)
                a = "rmw" if (r and w) else ("w" if w else "r")
            acc[(t, a)] += 1
            o = owner(ins.address)
            perf[o][t + ":" + a] += 1
            if t == "RenderState" and kind == "mem" and a != "r":
                if op.mem.index:
                    rs_idx["var(" + ins.op_str + ")@%x" % ins.address] += 1
                else:
                    rs_idx[(v - 0x2446d0) // 4] += 1
            if t == "TextureState" and kind == "mem" and a != "r":
                ts_idx[((v - 0x2444d0) // 4) % 32] += 1
            if t.startswith("OTHER"):
                other.append((ins.address, ins.mnemonic, ins.op_str))
print("access counts (target, access):")
for k in sorted(acc):
    print("  ", k, acc[k])
tot = sum(acc.values())
print("total instructions/operands:", tot)
print("functions:", len(perf))
for (fa, fn), c in sorted(perf.items(), key=lambda z: z[0][0] or 0):
    print("   %08x %-26s %s grp=%s" % (fa, fn, dict(c), group(fa, fn)))
print("written RenderState idx:", sorted(((k if isinstance(k, str) else hex(k)), n) for k, n in rs_idx.items()))
print("written TextureState idx (mod 32):", sorted(ts_idx))
print("other D3D BSS refs:", other)
