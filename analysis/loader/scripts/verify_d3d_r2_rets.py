"""verify_d3d_r2_rets.py - stack cleanup ('ret N') of the externally called D3D functions,
recomputed by following the control flow (recursive descent inside the D3D section,
not bounded by Ghidra sizes) and compared with the 'ret' column of the d3d.md tables
(section 3) and of d3d_functions.tsv (review pass 2)."""
import re, csv
from verify_d3d_r2_common import *

def rets_of(start, limit=0x3000):
    seen, todo, rs, tails = set(), [start], set(), set()
    while todo:
        a = todo.pop()
        while a not in seen and D3D_LO <= a < D3D_CODE_END and a - start < limit:
            seen.add(a)
            ins = next(cs.disasm(XB.read(a, 16), a), None)
            if ins is None:
                break
            m = ins.mnemonic
            if m == "ret":
                rs.add(int(ins.op_str, 16) if ins.op_str else 0)
                break
            if m == "jmp":
                op = ins.operands[0]
                if op.type == X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if t in D3DFN and t != start:
                        tails.add(t); break
                    a = t; continue
                if op.type == X86_OP_MEM and op.mem.index and not op.mem.base:   # jump table
                    tb = op.mem.disp & 0xffffffff
                    k = 0
                    while k < 64:
                        v = u32(tb + 4 * k)
                        if not (start <= v < start + limit): break
                        todo.append(v); k += 1
                    break
                tails.add("indirect"); break
            if m.startswith("j"):
                todo.append(ins.operands[0].imm & 0xffffffff)
            a += ins.size
    return rs, tails

tsv = {int(r["address"], 16): r for r in csv.DictReader(open(ROOT + r"\analysis\loader\d3d_functions.tsv"), delimiter="\t")}
md = open(ROOT + r"\analysis\loader\d3d.md", encoding="utf-8").read()
rows = re.findall(r"^\| (0x[0-9a-f]+) \| `?([^|`]+?)`?(?: \(.*?\))? \| ([0-9a-fx–-]+) \| (\d+)/(\d+) \|", md, re.M)
bad = 0
for ad, nm, ret, s_, f_ in rows:
    a = int(ad, 16)
    rs, tails = rets_of(a)
    exp = None if ret in ("–", "-") else int(ret, 16)
    ok = (exp is None and not rs) or (rs == {exp})
    if not ok:
        bad += 1
    print("%s %-40s md=%-5s tsv=%-12s cfg=%s tails=%s %s" % (ad, nm[:40], ret, tsv[a]["ret"], sorted(rs),
          [hex(t) if isinstance(t, int) else t for t in tails], "" if ok else "<-- CHECK"))
print("rows:", len(rows), "differences:", bad)
