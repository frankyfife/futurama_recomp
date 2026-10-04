"""verify_d3d_r2_kimports.py - which kernel thunks does the D3D section use, and which of
them are used ONLY by D3D (review pass 2). Every instruction operand (mem disp or imm)
in the thunk table 0x293560..0x29371c counts as a use; names from kernel_common.py,
argument pushes are shown for the D3D call sites as a plausibility check."""
import sys, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
import kernel_common as K
from verify_d3d_r2_common import *

T0 = 0x293560
n = 0
while u32(T0 + 4 * n):
    n += 1
T1 = T0 + 4 * n
use = collections.defaultdict(lambda: collections.Counter())
d3dsites = collections.defaultdict(list)
for s in CODE:
    for ins in sweep(s):
        for op in ins.operands:
            v = None
            if op.type == X86_OP_MEM and not op.mem.base and not op.mem.index:
                v = op.mem.disp & 0xffffffff
            elif op.type == X86_OP_IMM:
                v = op.imm & 0xffffffff
            if v is not None and T0 <= v < T1 and (v - T0) % 4 == 0:
                use[v][s] += 1
                if s == "D3D":
                    d3dsites[v].append(ins.address)
print("thunk entries:", n, "table end", hex(T1))
for v in sorted(use):
    if "D3D" not in use[v]:
        continue
    o = u32(v) & 0x7fffffff
    only = set(use[v]) == {"D3D"}
    print("%08x ord %3d %-34s %s %s sites=%s" % (v, o, K.XBOXKRNL_EXPORTS.get(o), "D3D-ONLY" if only else "shared",
          dict(use[v]), [hex(a) for a in d3dsites[v][:6]]))
