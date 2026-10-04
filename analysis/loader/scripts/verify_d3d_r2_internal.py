"""verify_d3d_r2_internal.py - D3D-internal facts (review pass 2):
 * internal call sites of MakeSpace 0x2392b0, MakeRequestedSpace 0x239130, BeginStateBig
   0x2392c0, BlockUntilVerticalBlank 0x2344c0 (sites, Ghidra functions, gap blocks)
 * push-buffer writer count with an independent, broader criterion:
     A = absolute operand m_pPut/m_pThreshold (0x244970/0x244974) or call MakeSpace/
         MakeRequestedSpace/BeginStateBig  (criterion of d3d_pushbuffer_writers.py)
     B = A or (loads [0x244968] and later writes 'mov [reg], imm' with an NV2A-like
         method header (imm & 0xe0000003 == 0, count 1..2047, method < 0x2000))
 * function count / named / size of the D3D section"""
import collections
from verify_d3d_r2_common import *

TG = {0x2392b0: "MakeSpace", 0x239130: "MakeRequestedSpace", 0x2392c0: "BeginStateBig", 0x2344c0: "BlockUntilVerticalBlank"}
cnt = collections.defaultdict(list)
A, B, touch = set(), set(), set()
dev_loaded = set()
for ins in sweep("D3D"):
    o = owner(ins.address)
    if ins.mnemonic == "call" or ins.mnemonic.startswith("j"):
        if ins.operands and ins.operands[0].type == X86_OP_IMM:
            t = ins.operands[0].imm & 0xffffffff
            if t in TG and o[0] != t:
                cnt[t].append((ins.address, o))
            if t in (0x2392b0, 0x239130, 0x2392c0) and o[0] != t:
                A.add(o)
    for op in ins.operands:
        if op.type == X86_OP_MEM and not op.mem.base and not op.mem.index:
            d = op.mem.disp & 0xffffffff
            if d in (0x244970, 0x244974):
                A.add(o)
            if d in (0x244968, 0x244970, 0x244974):
                touch.add(o)
            if d == 0x244968:
                dev_loaded.add(o)
        if op.type == X86_OP_IMM and (op.imm & 0xffffffff) in (0x244968, 0x244970):
            touch.add(o)
    if ins.mnemonic == "mov" and len(ins.operands) == 2 and ins.operands[0].type == X86_OP_MEM and ins.operands[1].type == X86_OP_IMM:
        v = ins.operands[1].imm & 0xffffffff
        c, m = (v >> 18) & 0x7ff, v & 0x1fff
        if v & 0xe0000003 == 0 and 1 <= c <= 2047 and 0x100 <= m < 0x2000 and ins.operands[0].mem.base and o in dev_loaded:
            B.add(o)
for t, l in cnt.items():
    gf = {o for a, o in l}
    ghidra = {o for o in gf if not o[1].startswith("gap_")}
    print("%-24s sites=%d functions(incl. gaps)=%d ghidra-only=%d gaps=%s" % (TG[t], len(l), len(gf), len(ghidra),
          [(hex(a), o[1]) for a, o in l if o[1].startswith("gap_")]))
print("PB writers criterion A (abs m_pPut/m_pThreshold or MakeSpace-family call):", len(A),
      " ghidra-only:", len({o for o in A if not o[1].startswith('gap_')}))
print("criterion B (A or dev-ptr + method header store):", len(A | B), " B\\A:", sorted(o[1] for o in (B - A)))
print("functions touching pDevice/m_pPut/m_pThreshold (abs operand or imm):", len(touch))
d3 = [f for f in F if f[2] == "D3D"]
print("D3D functions:", len(d3), "named D3D8::", sum(1 for f in d3 if f[3].startswith("D3D8::")), "bytes:", sum(f[1] for f in d3))
print("D3D section va %#x vsz %#x raw size %#x -> bss %#x..%#x" % (D3D["va"], D3D["vsz"], D3D["rsz"], D3D_CODE_END, D3D_END))

# attribution by nearest preceding Ghidra function start (gap sites belong to truncated
# Ghidra functions: D3DDevice_Clear ends after 'call 0x23a5d0' at 0x23b63f although the
# body continues (jump 0x23b460 -> 0x23b70a); FUN_0023fdb0 stops at the unrecovered jump
# table 'jmp [ecx*4+0x240328]' at 0x23fe67)
import bisect
ms = cnt[0x2392b0]
near = {F[bisect.bisect_right(FS, a) - 1][0] for a, o in ms}
print("MakeSpace callers by nearest preceding Ghidra start:", len(near))
for a in (0x23b460,):
    i = next(cs.disasm(XB.read(a, 8), a)); print("  evidence %08x %s %s" % (a, i.mnemonic, i.op_str))
