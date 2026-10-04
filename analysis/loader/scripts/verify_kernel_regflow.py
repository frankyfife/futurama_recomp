"""Reviewer check: for every 'mov reg,[slot]' follow the register through the CFG (BFS over
conditional/unconditional jumps, stop on redefinition of the register, on ret, and - for the
volatile registers eax/ecx/edx - on any call) and collect all 'call reg' / 'jmp reg'.
Independent of verify_kernel_sites.py's linear follower and of the original stack solver.
"""
import json
import os
from collections import Counter

from capstone.x86 import X86_OP_IMM, X86_OP_REG

from verify_kernel_common import XBE, Funcs, fmt, insn

HERE = os.path.dirname(os.path.abspath(__file__))
v = json.load(open(os.path.join(HERE, "verify_kernel_sites.json")))
F = Funcs()
SUB = {"eax": ("eax", "ax", "al", "ah"), "ebx": ("ebx", "bx", "bl", "bh"), "ecx": ("ecx", "cx", "cl", "ch"),
       "edx": ("edx", "dx", "dl", "dh"), "esi": ("esi", "si"), "edi": ("edi", "di"), "ebp": ("ebp", "bp")}
VOLATILE = ("eax", "ecx", "edx")

found = []
for r in v["refs"]:
    if r["kind"] != "mov_reg":
        continue
    va = int(r["va"], 16)
    i0 = insn(va)
    reg = i0.reg_name(i0.operands[0].reg)
    todo = [va + i0.size]
    seen = set()
    uses = []
    while todo:
        a = todo.pop()
        steps = 0
        while a not in seen and steps < 400:
            seen.add(a)
            steps += 1
            i = insn(a)
            if i is None:
                break
            m = i.mnemonic
            if m in ("call", "jmp") and i.op_str == reg:
                uses.append((i.address, m))
                if m == "jmp":
                    break
                if reg in VOLATILE:
                    break
                a += i.size
                continue
            if m == "call" and reg in VOLATILE:
                break
            if m.startswith("ret") or m == "int3":
                break
            if m == "jmp":
                if i.operands[0].type == X86_OP_IMM:
                    a = i.operands[0].imm
                    continue
                break
            if m.startswith("j") or m in ("loop", "jecxz"):
                if i.operands[0].type == X86_OP_IMM:
                    todo.append(i.operands[0].imm)
                a += i.size
                continue
            # redefinition
            if i.operands and i.operands[0].type == X86_OP_REG and i.reg_name(i.operands[0].reg) in SUB.get(reg, (reg,)) \
                    and m not in ("cmp", "test", "push"):
                break
            if m == "pop" and i.op_str in SUB.get(reg, (reg,)):
                break
            if m == "xchg" and reg in i.op_str:
                break
            a += i.size
    for u in uses:
        found.append((r["slot"], u[0], u[1], va))

print("call/jmp reg sites found by CFG register flow:", len(found), Counter(x[2] for x in found))
mine = set(x[1] for x in found)
lin = set(int(a, 16) for l in v["sites"].values() for k, a in l if k in ("call_reg", "jmp_reg"))
print("linear follower found:", len(lin))
print("only in CFG flow:", sorted("%06x(slot %d)" % (x[1], x[0]) for x in found if x[1] not in lin))
print("only in linear:", sorted("%06x" % a for a in lin - mine))
