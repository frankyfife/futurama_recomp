"""verify_d3d_rdata.py - .rdata/.data addresses that D3D code references (operand imm/disp),
and which non-D3D functions reference the same addresses (shared D3D constant tables /
globals in .rdata/.data that d3d_scan_refs.py does not cover)."""
import collections
from capstone import CS_OP_IMM, CS_OP_MEM
from verify_d3d_common import *
RD = (SEC[".rdata"][0], SEC[".rdata"][0] + SEC[".rdata"][1])
DA = (SEC[".data"][0], SEC[".data"][0] + SEC[".data"][1])
THUNK = (0x293560, 0x293560 + 4 * 112)
def refs(secname):
    va, vsz, raw, rsz = SEC[secname]
    starts = sorted(set([f[0] for f in FUNCS if va <= f[0] < va + rsz] + [va]))
    out = collections.defaultdict(set)
    for i, a in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else va + rsz
        for ins in MD.disasm(X.read(a, end - a), a):
            for op in ins.operands:
                v = op.imm & 0xffffffff if op.type == CS_OP_IMM else (op.mem.disp & 0xffffffff if op.type == CS_OP_MEM else None)
                if v is None: continue
                if (RD[0] <= v < RD[1] or DA[0] <= v < DA[1]) and not (THUNK[0] <= v < THUNK[1]):
                    out[v].add((func_of(ins.address)[0], ins.address, ins.mnemonic + " " + ins.op_str))
    return out
d3d = refs("D3D")
print("D3D-referenced .rdata/.data addresses:", len(d3d))
rd = sorted(v for v in d3d if RD[0] <= v < RD[1]); da = sorted(v for v in d3d if DA[0] <= v < DA[1])
print("  .rdata range %08x..%08x (%d)" % (rd[0], rd[-1], len(rd)))
print("  .data  range %08x..%08x (%d): %s" % (da[0], da[-1], len(da), " ".join("%08x" % v for v in da)))
other = {}
for s in [".text", "XMV", "DSOUND", "XPP"]:
    for v, st in refs(s).items():
        other.setdefault(v, set()).update(st)
# exact address sharing, plus 'inside D3D rdata block' (between min and max D3D rdata ref)
shared = sorted(v for v in other if v in d3d)
print("addresses referenced by both D3D and non-D3D code:", len(shared))
for v in shared:
    fs = sorted({(f, BYADDR.get(f, (0, 0, 0, '?'))[3]) for f, a, t in other[v]})
    ex = next(iter(other[v]))
    print("   %08x  %d refs from %s   e.g. %08x %s" % (v, len(other[v]), ", ".join("%08x" % f for f, n in fs), ex[1], ex[2]))
blk = sorted(v for v in other if rd[0] <= v <= rd[-1] and v not in d3d)
print("non-D3D refs inside D3D's .rdata span but not D3D-referenced:", len(blk))
for v in blk[:30]:
    ex = next(iter(other[v])); print("   %08x %s %08x %s" % (v, BYADDR.get(ex[0], (0,0,0,'?'))[3], ex[1], ex[2]))
