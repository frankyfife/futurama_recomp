"""Reviewer check: independent enumeration of every reference to a kernel thunk slot.

1. byte scan of ALL sections for the 4-byte value of every slot address (0x293560..0x293718)
2. in code sections: try every instruction start 1..7 bytes before the hit; keep decodes whose
   instruction covers the hit and has the slot as [disp32] (no base/index) or as imm32
3. stubs = 'jmp [slot]' at a Ghidra function entry; find call/jmp rel32 to them by byte scan
   (E8/E9 + rel32 whose target is a stub)
4. 'mov reg,[slot]' -> follow linearly up to 60 insns (stop at ret/jmp/redefinition of reg) and count
   'call reg' / 'jmp reg'
Writes verify_kernel_sites.json (per-slot site list) for the other verify_* scripts.
"""
import csv
import json
import os
import re
import struct
from collections import Counter, defaultdict

from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from verify_kernel_common import (CODE_SECTIONS, LOADER, XBE, Funcs, fmt, insn, md,
                                  mem_disp_only, module_of, thunk_table)

enc, TVA, ents = thunk_table()
N = len(ents)
LO, HI = TVA, TVA + 4 * N
F = Funcs()
tsv = {int(r["slot"]): r for r in csv.DictReader(open(os.path.join(LOADER, "kernel_imports.tsv"), encoding="utf-8"), delimiter="\t")}
NAME = {s: tsv[s]["name"] for s in tsv}


def slot(v):
    if LO <= v < HI and (v - LO) % 4 == 0:
        return (v - LO) // 4
    return None


# ---- 1. byte scan of all sections
hits = []  # (va, slot, section)
for name, va, vsz, raw, rsz in XBE.sections:
    data = XBE.b[raw:raw + rsz]
    for off in range(0, len(data) - 3):
        v = struct.unpack_from("<I", data, off)[0]
        s = slot(v)
        if s is not None:
            hits.append((va + off, s, name))
print("raw 4-byte hits by section:", Counter(h[2] for h in hits))
for h in hits:
    if h[2] not in CODE_SECTIONS:
        print("  non-code hit %06x -> slot %d %s (section %s)" % (h[0], h[1], NAME[h[1]], h[2]))

# ---- 2. decode
refs = []      # dict(va, ins, slot, kind)
rejected = []
for hv, s, sec in hits:
    if sec not in CODE_SECTIONS:
        continue
    cands = []
    for k in range(1, 8):
        i = insn(hv - k)
        if i is None or i.address + i.size < hv + 4:
            continue
        # disp/imm must be located exactly at hv
        dispoff = hv - i.address
        how = None
        for op in i.operands:
            if op.type == X86_OP_MEM and mem_disp_only(op) and (op.mem.disp & 0xFFFFFFFF) == LO + 4 * s:
                how = "mem"
            elif op.type == X86_OP_MEM and (op.mem.disp & 0xFFFFFFFF) == LO + 4 * s:
                how = how or "memidx"
            elif op.type == X86_OP_IMM and (op.imm & 0xFFFFFFFF) == LO + 4 * s:
                how = how or "imm"
        if how:
            cands.append((how, i))
    mem = [c for c in cands if c[0] == "mem"]
    if len(mem) == 1:
        refs.append(dict(va=mem[0][1].address, slot=s, ins=mem[0][1]))
    elif len(mem) > 1:
        # pick the one that starts at an instruction boundary of a linear sweep of the containing function
        fr = F.containing(hv)
        chosen = None
        if fr:
            bounds = set(i.address for i in md.disasm(XBE.read(fr[0], fr[1]), fr[0]))
            for c in mem:
                if c[1].address in bounds:
                    chosen = c[1]
        print("AMBIGUOUS %06x slot %d candidates %s -> %s" % (hv, s, [fmt(c[1]) for c in mem], fmt(chosen) if chosen else None))
        if chosen:
            refs.append(dict(va=chosen.address, slot=s, ins=chosen))
    else:
        rejected.append((hv, s, [(c[0], fmt(c[1])) for c in cands]))
print("decoded [disp32] refs:", len(refs))
for r in rejected:
    print("REJECTED %06x slot %d (%s): %s" % (r[0], r[1], NAME[r[1]], r[2][:3]))

# dedupe
seen = {}
for r in refs:
    seen[r["va"]] = r
refs = sorted(seen.values(), key=lambda r: r["va"])
kinds = Counter()
for r in refs:
    i = r["ins"]
    m = i.mnemonic
    if m == "call":
        r["kind"] = "call_mem"
    elif m == "jmp":
        r["kind"] = "jmp_mem"
    elif m == "mov" and i.operands[0].type == X86_OP_REG:
        r["kind"] = "mov_reg"
    elif m == "push":
        r["kind"] = "push_mem"
    elif m == "mov" and i.operands[0].type == X86_OP_MEM:
        r["kind"] = "WRITE_TO_SLOT"
    else:
        r["kind"] = "other:" + m
    kinds[r["kind"]] += 1
print("ref kinds:", dict(kinds))
for r in refs:
    if r["kind"] not in ("call_mem", "jmp_mem", "mov_reg", "push_mem"):
        print("  unusual:", fmt(r["ins"]), F.name_or(r["va"]))

# ---- 3. stubs
stubs = {}
for r in refs:
    if r["kind"] == "jmp_mem" and r["va"] in F.by:
        stubs[r["va"]] = r["slot"]
print("stubs at Ghidra function entries:", len(stubs), {("%06x" % a): NAME[s] for a, s in stubs.items()})
stubcalls = []
for name, va, vsz, raw, rsz in XBE.sections:
    if name not in CODE_SECTIONS:
        continue
    data = XBE.b[raw:raw + rsz]
    for off in range(len(data) - 4):
        op = data[off]
        if op in (0xE8, 0xE9):
            tgt = (va + off + 5 + struct.unpack_from("<i", data, off + 1)[0]) & 0xFFFFFFFF
            if tgt in stubs:
                stubcalls.append(dict(va=va + off, slot=stubs[tgt], kind=("call_stub" if op == 0xE8 else "jmp_stub"), stub=tgt))
# also 0F 8x jcc rel32 to stubs
print("rel32 call/jmp to stubs:", len(stubcalls), Counter(c["kind"] for c in stubcalls))
for c in stubcalls:
    i = insn(c["va"])
    fr = F.containing(c["va"])
    c["ok_func"] = bool(fr)
    if not fr:
        print("  stubcall outside Ghidra func:", "%06x" % c["va"], NAME[c["slot"]])

# ---- 4. mov reg,[slot] -> call reg
regcalls = []
for r in refs:
    if r["kind"] != "mov_reg":
        continue
    reg = r["ins"].reg_name(r["ins"].operands[0].reg)
    a = r["va"] + r["ins"].size
    uses = []
    for _ in range(80):
        i = insn(a)
        if i is None:
            break
        ops = i.op_str
        if i.mnemonic in ("call", "jmp") and ops == reg:
            uses.append((i.address, i.mnemonic))
            if i.mnemonic == "jmp":
                break
        elif i.mnemonic in ("call",) and reg == "eax" or (i.mnemonic in ("call",) and reg in ("ecx", "edx")):
            # volatile reg destroyed by a call
            break
        elif i.mnemonic.startswith("ret") or (i.mnemonic == "jmp"):
            break
        else:
            # redefinition of reg
            if i.operands and i.operands[0].type == X86_OP_REG and i.reg_name(i.operands[0].reg) == reg and \
                    i.mnemonic in ("mov", "lea", "pop", "xor", "movzx", "movsx") and not (i.mnemonic == "xor" and False):
                break
            if i.mnemonic == "pop" and ops == reg:
                break
        a += i.size
    r["reg_uses"] = uses
    other = []
    for u in uses:
        regcalls.append(dict(va=u[0], slot=r["slot"], kind="call_reg" if u[1] == "call" else "jmp_reg", loader=r["va"]))
print("call/jmp via register after mov reg,[slot]:", Counter(c["kind"] for c in regcalls), "total", len(regcalls))
noncall_mov = [r for r in refs if r["kind"] == "mov_reg" and not r["reg_uses"]]
print("mov reg,[slot] without call reg (data use / address passing):", len(noncall_mov))
for r in noncall_mov:
    print("   ", fmt(r["ins"]), NAME[r["slot"]], F.name_or(r["va"]))

# ---- per slot totals
sites = defaultdict(list)
for r in refs:
    if r["kind"] == "call_mem":
        sites[r["slot"]].append(("call_mem", r["va"]))
    elif r["kind"] == "jmp_mem" and r["va"] not in stubs:
        sites[r["slot"]].append(("tail_jmp", r["va"]))
for c in regcalls:
    sites[c["slot"]].append((c["kind"], c["va"]))
for c in stubcalls:
    sites[c["slot"]].append((c["kind"], c["va"]))
tot = Counter()
for s, l in sites.items():
    for k, a in l:
        tot[k] += 1
print("SITE TOTALS:", dict(tot), "sum", sum(tot.values()))
mods = Counter()
for s, l in sites.items():
    for k, a in l:
        mods[module_of(a)] += 1
print("sites by module (report boundaries):", dict(mods))
diff = 0
for s in range(N):
    mine = len(sites.get(s, []))
    theirs = int(tsv[s]["call_sites"])
    if mine != theirs:
        diff += 1
        print("SITE COUNT DIFF slot %3d %-30s mine %3d tsv %3d  %s" % (s, NAME[s], mine, theirs, sorted(sites.get(s, []), key=lambda x: x[1])))
print("slots with different site count:", diff)
json.dump({"stubs": {("%06x" % a): s for a, s in stubs.items()},
           "sites": {str(s): [(k, "%06x" % a) for k, a in sorted(l, key=lambda x: x[1])] for s, l in sites.items()},
           "refs": [dict(va="%06x" % r["va"], slot=r["slot"], kind=r["kind"], text=fmt(r["ins"])) for r in refs]},
          open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "verify_kernel_sites.json"), "w"), indent=1)
