"""verify_d3d_r2_ranges.py - recompute table 4.2 of d3d.md (NiXBox sub-ranges: functions with
D3D call or inline lazy-state access, call sites, inline sites) - review pass 2.
Call sites from a fresh sweep; caller identity from d3d_functions.tsv ext_caller_list (which
uses the vtable-backed sub_ split), inline sites from the operand sweep."""
import csv, collections
from verify_d3d_r2_common import *
R = [(0x146020, 0x14cd50), (0x155000, 0x156d00), (0x156d00, 0x158700), (0x158700, 0x15a100), (0x15a100, 0x15c400),
     (0x15c400, 0x15ce00), (0x15ce00, 0x15d900), (0x15d900, 0x160000), (0x160000, 0x161800), (0x161800, 0x162400),
     (0x162400, 0x166871)]
callers = set()
for r in csv.DictReader(open(ROOT + r"\analysis\loader\d3d_functions.tsv"), delimiter="\t"):
    for c in r["ext_caller_list"].split(","):
        if c:
            callers.add(int(c.split(":")[0], 16))
sites = []
inl = []
for ins in sweep(".text"):
    if ins.mnemonic in ("call", "jmp") and ins.operands and ins.operands[0].type == X86_OP_IMM and (ins.operands[0].imm & 0xffffffff) in D3DFN:
        sites.append(ins.address)
    for op in ins.operands:
        v = None
        if op.type == X86_OP_MEM: v = op.mem.disp & 0xffffffff
        elif op.type == X86_OP_IMM: v = op.imm & 0xffffffff
        if v is not None and D3D_CODE_END <= v < D3D_END:
            inl.append((ins.address, owner(ins.address)[0]))
inlf = {f for a, f in inl}
tot = [0, 0, 0]
for lo, hi in R:
    fs = {c for c in callers if lo <= c < hi} | {f for f in inlf if lo <= f < hi}
    n_s = sum(1 for s in sites if lo <= s < hi)
    n_i = sum(1 for a, f in inl if lo <= a < hi)
    tot = [tot[0] + len(fs), tot[1] + n_s, tot[2] + n_i]
    print("%06x-%06x  Fkt %3d  Calls %3d  Inline %3d" % (lo, hi, len(fs), n_s, n_i))
print("sum", tot)
