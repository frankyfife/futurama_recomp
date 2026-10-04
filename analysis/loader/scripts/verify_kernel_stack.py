"""Reviewer check: independent re-implementation of a stack-consistency purge check.

For every Ghidra function that contains a kernel call site, walk the CFG (direct jumps/jcc only)
with a stack-depth model:
  push/pop/pushad/popad/pushfd/popfd, sub/add esp,imm, push ebp;mov ebp,esp (frame), mov esp,ebp,
  leave, lea esp,[ebp-N], __SEH_prolog (push L; push scope; call -> depth D+24+L, ebp=D-4) and
  __SEH_epilog (depth -> ebp-4), direct calls to internal functions with a unique 'ret N' value,
  import calls with the purge under test; any other call (indirect, unknown purge) -> depth unknown.
Inconsistencies: ret/tail-jmp with depth != 0, join with different depths, negative depth.
For each import: count inconsistencies over its functions with the TSV purge and with every other
purge 0..0x30 (step 4). Output: per import TSV purge, inconsistencies at TSV value, minimum of the
other values (None = no constraint), and whether the TSV value is the unique minimum.
"""
import csv
import json
import os
import re
from collections import defaultdict

from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_INVALID

from verify_kernel_common import LOADER, XBE, Funcs, insn, md, thunk_table

HERE = os.path.dirname(os.path.abspath(__file__))
F = Funcs()
enc, TVA, ents = thunk_table()
N = len(ents)
TSV = {int(r["slot"]): r for r in csv.DictReader(open(os.path.join(LOADER, "kernel_imports.tsv"), encoding="utf-8"), delimiter="\t")}
v = json.load(open(os.path.join(HERE, "verify_kernel_sites.json")))
STUBS = {int(a, 16): s for a, s in v["stubs"].items()}
SEH_PROLOG, SEH_EPILOG = 0x167DC8, 0x167E03

sites = defaultdict(list)
for k, l in v["sites"].items():
    for kind, a in l:
        sites[int(k)].append(int(a, 16))
for s, a in [(1, 0x292d4c), (3, 0xf792a), (3, 0xf9bc0), (7, 0x291a65), (26, 0xf7b3f), (41, 0xf9278),
             (41, 0xf950c), (93, 0x240735), (93, 0x240797), (104, 0x24ae48)]:
    sites[s].append(a)

_purge_cache = {}


def internal_purge(fa):
    if fa in _purge_cache:
        return _purge_cache[fa]
    r = F.by.get(fa)
    res = None
    if r and r[1] > 0:
        vals = set()
        for i in md.disasm(XBE.read(fa, r[1]), fa):
            if i.mnemonic == "ret":
                vals.add(int(i.op_str, 0) if i.op_str else 0)
        if len(vals) == 1:
            res = vals.pop()
    _purge_cache[fa] = res
    return res


def slot_of_mem(op):
    if op.type == X86_OP_MEM and op.mem.base == X86_REG_INVALID and op.mem.index == X86_REG_INVALID:
        a = op.mem.disp & 0xFFFFFFFF
        if TVA <= a < TVA + 4 * N and (a - TVA) % 4 == 0:
            return (a - TVA) // 4
    return None


def walk(fa, purge, regslot):
    """Return number of inconsistencies in function fa given purge dict slot->bytes."""
    fr = F.by[fa]
    lo, hi = fa, fa + fr[1]
    bad = 0
    seen = {}
    work = [(fa, 0, None)]
    while work:
        a, d, ebp = work.pop()
        while True:
            if a in seen:
                od = seen[a]
                if od is not None and d is not None and od != d:
                    bad += 1
                break
            seen[a] = d
            i = insn(a)
            if i is None:
                break
            m, ops, nxt = i.mnemonic, i.operands, a + i.size
            if d is not None and d < 0:
                bad += 1
                d = None
            if m == "int3" or m == "hlt":
                break
            if m == "ret":
                if d is not None and d != 0:
                    bad += 1
                break
            if m == "push":
                d = None if d is None else d + 4
                if ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) == "esp":
                    pass
            elif m == "pop":
                if ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) == "esp":
                    d = None
                else:
                    d = None if d is None else d - 4
            elif m in ("pushal", "pushad"):
                d = None if d is None else d + 32
            elif m in ("popal", "popad"):
                d = None if d is None else d - 32
            elif m in ("pushfd", "pushf"):
                d = None if d is None else d + 4
            elif m in ("popfd", "popf"):
                d = None if d is None else d - 4
            elif m == "leave":
                d = None if ebp is None else ebp - 4
            elif ops and ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) == "esp" and m not in ("cmp", "test"):
                if m in ("sub", "add") and ops[1].type == X86_OP_IMM:
                    if d is not None:
                        d = d + ops[1].imm if m == "sub" else d - ops[1].imm
                elif m == "mov" and ops[1].type == X86_OP_REG and i.reg_name(ops[1].reg) == "ebp":
                    d = ebp
                elif m == "lea" and ops[1].type == X86_OP_MEM and i.reg_name(ops[1].mem.base) == "ebp" and ops[1].mem.index == X86_REG_INVALID:
                    d = None if ebp is None else ebp - ops[1].mem.disp
                else:
                    d = None
            elif m == "mov" and ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) == "ebp":
                if ops[1].type == X86_OP_REG and i.reg_name(ops[1].reg) == "esp":
                    ebp = d
                else:
                    ebp = None
            elif m == "lea" and ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) == "ebp":
                if ops[1].type == X86_OP_MEM and i.reg_name(ops[1].mem.base) == "esp" and ops[1].mem.index == X86_REG_INVALID and d is not None:
                    ebp = d - ops[1].mem.disp
                else:
                    ebp = None
            elif m == "call":
                op = ops[0]
                p = "unk"
                if op.type == X86_OP_IMM:
                    t = op.imm & 0xFFFFFFFF
                    if t == SEH_PROLOG:
                        # find the 'push L' two pushes back
                        L = None
                        back = [x for x in md.disasm(XBE.read(a - 12, 12), a - 12)]
                        pushes = [x for x in back if x.mnemonic == "push" and x.address + x.size <= a]
                        if len(pushes) >= 2 and pushes[-2].operands[0].type == X86_OP_IMM:
                            L = pushes[-2].operands[0].imm
                        if d is None or L is None:
                            d, ebp = None, None
                        else:
                            ebp = d - 4
                            d = d + 24 + L
                        a = nxt
                        continue
                    if t == SEH_EPILOG:
                        d = None if ebp is None else ebp - 4
                        a = nxt
                        continue
                    if t in STUBS:
                        p = purge.get(STUBS[t], "unk")
                    else:
                        ip = internal_purge(t)
                        p = "unk" if ip is None else ip
                elif op.type == X86_OP_MEM:
                    s = slot_of_mem(op)
                    if s is not None:
                        p = purge.get(s, "unk")
                elif op.type == X86_OP_REG and a in regslot:
                    p = purge.get(regslot[a], "unk")
                if p == "unk" or d is None:
                    d = None
                else:
                    d = d - p
            elif m == "jmp":
                op = ops[0]
                if op.type == X86_OP_IMM:
                    t = op.imm & 0xFFFFFFFF
                    if lo <= t < hi:
                        a = t
                        continue
                    # tail jump out of the function
                    if d is not None and d != 0:
                        bad += 1
                    break
                # jmp [slot] / jmp reg / switch
                if (op.type == X86_OP_MEM and slot_of_mem(op) is not None) or (op.type == X86_OP_REG and a in regslot):
                    if d is not None and d != 0:
                        bad += 1
                break
            elif m.startswith("j") or m in ("loop", "loope", "loopne", "jecxz"):
                if ops[0].type == X86_OP_IMM:
                    t = ops[0].imm & 0xFFFFFFFF
                    if lo <= t < hi:
                        work.append((t, d, ebp))
            a = nxt
    return bad


def main():
    regslot = {}
    for s, l in sites.items():
        for a in l:
            i = insn(a)
            if i.operands[0].type == X86_OP_REG:
                regslot[a] = s
    base = {s: int(TSV[s]["purge_bytes"]) for s in TSV if TSV[s]["kind"] == "FUNC" and TSV[s]["purge_bytes"] != ""}
    funcs_of = defaultdict(set)
    nofunc = defaultdict(list)
    for s, l in sites.items():
        for a in l:
            fr = F.containing(a)
            if fr:
                funcs_of[s].add(fr[0])
            else:
                nofunc[s].append(a)
    out = []
    for s in sorted(base):
        fs = funcs_of.get(s, set())
        if not fs:
            out.append((s, base[s], None, None, "no Ghidra function", nofunc.get(s)))
            continue
        res = {}
        for p in range(0, 0x34, 4):
            pg = dict(base)
            pg[s] = p
            res[p] = sum(walk(fa, pg, regslot) for fa in fs)
        at = res[base[s]]
        others = [res[p] for p in res if p != base[s]]
        mo = min(others)
        verdict = "UNIQUE" if at < mo else ("UNCONSTRAINED" if at == mo and len(set(res.values())) == 1 else ("TIE" if at == mo else "WORSE"))
        out.append((s, base[s], at, mo, verdict, nofunc.get(s)))
    for s, b, at, mo, verdict, nf in out:
        print("%3d %-32s purge %2s  incons@tsv %-4s min(other) %-4s %-14s %s" % (
            s, TSV[s]["name"], b, at, mo, verdict, ("nofunc sites: " + ",".join("%06x" % x for x in nf)) if nf else ""))
    from collections import Counter
    print("verdicts:", Counter(o[4] for o in out))


if __name__ == "__main__":
    main()
