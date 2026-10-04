"""verify_d3d_r2_threads.py - who creates threads (review pass 2): raw E8/E9 rel32 and raw
dword references to XAPILIB::CreateThread 0xf9a72 in all sections, and every code operand
that references the PsCreateSystemThreadEx thunk slot (ordinal 255)."""
import sys
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
import kernel_common as K
from verify_d3d_r2_common import *
T = 0xf9a72
for s in CODE + [".rdata", ".data"]:
    d = SECS[s]; b = XB.b[d["raw"]:d["raw"] + d["rsz"]]
    for i in range(len(b) - 5):
        if s in CODE and b[i] in (0xE8, 0xE9) and (d["va"] + i + 5 + struct.unpack_from("<i", b, i + 1)[0]) & 0xffffffff == T:
            print("rel32 call/jmp to CreateThread at %08x (%s) in %s" % (d["va"] + i, s, owner(d["va"] + i)[1]))
        if struct.unpack_from("<I", b, i)[0] == T:
            print("raw dword ref to CreateThread at %08x (%s)" % (d["va"] + i, s))
slot = next(0x293560 + 4 * k for k in range(111) if u32(0x293560 + 4 * k) & 0x7fffffff == 255)
print("PsCreateSystemThreadEx slot", hex(slot), K.XBOXKRNL_EXPORTS.get(255))
for s in CODE:
    for ins in sweep(s):
        for op in ins.operands:
            if op.type == X86_OP_MEM and not op.mem.base and not op.mem.index and (op.mem.disp & 0xffffffff) == slot:
                print("  thunk use %08x %s %s in %s" % (ins.address, ins.mnemonic, ins.op_str, owner(ins.address)[1]))
