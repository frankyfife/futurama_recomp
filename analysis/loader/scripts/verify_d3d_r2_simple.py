"""verify_d3d_r2_simple.py - SetRenderState_Simple (0x2357d0) external sites (review pass 2).

For each external call, decode the containing function forward from its start and take
the last instruction before the call that writes ecx (any form). Constant methods are
mapped back to the render-state index via the D3D table 0x293828[0..0x5b].
Also: shows the table, disassembles 0x2357d0 and checks the fastcall contract."""
import collections
from verify_d3d_r2_common import *

TAB = 0x293828
inv = collections.defaultdict(list)
for i in range(0x5c):
    inv[u32(TAB + 4 * i)].append(i)
print("table 0x293828 first/last:", hex(u32(TAB)), hex(u32(TAB + 4 * 0x5b)), " entry 0x5c ->", hex(u32(TAB + 4 * 0x5c)))
print("SetRenderState_Simple:")
pdis(0x2357d0, 0x30)

sites = []
for s in [".text"]:
    for ins in sweep(s):
        if ins.mnemonic in ("call", "jmp") and ins.operands and ins.operands[0].type == X86_OP_IMM and (ins.operands[0].imm & 0xffffffff) == 0x2357d0:
            sites.append(ins.address)
print("external sites:", len(sites), "functions:", len({owner(a)[0] for a in sites}))
const, var = [], []
for a in sites:
    fs, fn = owner(a)
    last = None
    lastedx = None
    for ins in cs.disasm(XB.read(fs, a - fs + 5), fs):
        if ins.address >= a:
            break
        regs_w = ins.regs_access()[1]
        names = [ins.reg_name(r) for r in regs_w]
        if "ecx" in names or "cx" in names or "cl" in names:
            last = ins
        if "edx" in names:
            lastedx = ins
    txt = "%s %s @%x" % (last.mnemonic, last.op_str, last.address) if last else "?"
    v = None
    if last is not None and last.mnemonic == "mov" and last.operands[1].type == X86_OP_IMM:
        v = last.operands[1].imm & 0xffffffff
        const.append((a, v))
    else:
        var.append(a)
    print("  %08x in %-14s ecx<- %-38s state=%s  edx<- %s" % (a, fn, txt, [hex(i) for i in inv.get(v, [])] if v else "-",
          "%s %s" % (lastedx.mnemonic, lastedx.op_str) if lastedx else "?"))
print("constant sites:", len(const), "variable:", len(var), [hex(x) for x in var])
vals = sorted({v for a, v in const})
print("distinct constants:", len(vals), [hex(v) for v in vals])
print("states covered:", sorted({hex(i) for a, v in const for i in inv.get(v, [])}))
