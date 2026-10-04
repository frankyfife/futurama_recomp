"""verify_d3d_devptr.py - heuristic: in NiXBox lib + movie code, find loads 'mov R, [X+0x60]'
whose destination register R is dereferenced ([R+disp]) within the next 6 instructions
before being overwritten -> possible inline access to g_Device fields via the device pointer
stored in NiXBoxRenderer+0x60."""
from capstone import CS_OP_MEM, CS_OP_REG
from verify_d3d_common import *
rng = [(0x146020, 0x14cd50), (0x155000, 0x166871), (0xede00, 0xee900)]
va, vsz, raw, rsz = SEC[".text"]
starts = sorted(set(STARTS))
hits = []
for i, a in enumerate(starts):
    if not any(lo <= a < hi for lo, hi in rng): continue
    end = starts[i + 1] if i + 1 < len(starts) else va + rsz
    ins = list(MD.disasm(X.read(a, end - a), a))
    for k, x in enumerate(ins):
        if x.mnemonic == "mov" and len(x.operands) == 2 and x.operands[0].type == CS_OP_REG and \
           x.operands[1].type == CS_OP_MEM and x.operands[1].mem.disp == 0x60 and x.operands[1].mem.base:
            r = x.operands[0].reg
            for y in ins[k + 1:k + 7]:
                used = any(op.type == CS_OP_MEM and op.mem.base == r for op in y.operands)
                if used:
                    hits.append((x.address, func_of(x.address)[3], y.address, y.mnemonic + " " + y.op_str)); break
                if y.operands and y.operands[0].type == CS_OP_REG and y.operands[0].reg == r and y.mnemonic in ("mov", "lea", "pop", "xor"): break
print(len(hits))
for h in hits: print("  %08x %-26s -> %08x %s" % h)
