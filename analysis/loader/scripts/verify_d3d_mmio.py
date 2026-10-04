"""verify_d3d_mmio.py - decoded operands (imm/disp) in 0xFD000000-0xFEFFFFFF (NV2A regs
0xFD000000.., also 0xFE000000 APU/other) in every code section, via linear sweep from
functions.tsv starts; plus D3D-internal count for comparison."""
import collections
from capstone import CS_OP_IMM, CS_OP_MEM
from verify_d3d_common import *
hits = collections.defaultdict(list)
for s in [".text", "D3D", "DSOUND", "XMV", "XPP"]:
    va, vsz, raw, rsz = SEC[s]
    starts = sorted(set([f[0] for f in FUNCS if va <= f[0] < va + rsz] + [va]))
    for i, a in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else va + rsz
        for ins in MD.disasm(X.read(a, end - a), a):
            for op in ins.operands:
                v = op.imm & 0xffffffff if op.type == CS_OP_IMM else (op.mem.disp & 0xffffffff if op.type == CS_OP_MEM else None)
                if v is not None and 0xFD000000 <= v < 0xFF000000:
                    hits[s].append((ins.address, v, ins.mnemonic + " " + ins.op_str))
for s, l in hits.items():
    print(s, len(l))
    for a, v, t in l[:12]:
        print("   %08x %08x %s" % (a, v, t))
