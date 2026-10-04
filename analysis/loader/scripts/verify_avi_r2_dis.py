"""Round-2 disassembly helper: python verify_avi_r2_dis.py <start> <end> [...]

Prints instructions with call/jmp targets annotated by Ghidra/XbSymbolDatabase
names, imm32 floats and C strings. Optional env OUTNAME writes a copy to
scripts/out/<OUTNAME>.
"""
import os
import re
import struct
import sys
from verify_avi_r2_common import X, FNAME, func_of, dis_range, Tee, f32


def annot(ins):
    s = []
    for m in re.finditer(r"0x([0-9a-f]{5,8})", ins.op_str):
        v = int(m.group(1), 16)
        if v in FNAME:
            s.append(FNAME[v])
        elif X.section(v) in (".rdata", ".data"):
            try:
                c = X.cstr(v)
                if c and len(c) >= 3 and all(32 <= ord(ch) < 127 for ch in c[:20]):
                    s.append(repr(c[:60]))
                else:
                    s.append("[%08x]=%08x f=%g" % (v, X.u32(v), f32(v)))
            except Exception:
                pass
        if 0x3a000000 <= v <= 0x4f000000 and ins.mnemonic in ("push", "mov"):
            s.append("f=%g" % struct.unpack("<f", struct.pack("<I", v))[0])
    return ("   ; " + " | ".join(s)) if s else ""


name = os.environ.get("OUTNAME")
P = Tee(name) if name else print
args = sys.argv[1:]
for i in range(0, len(args), 2):
    a, b = int(args[i], 16), int(args[i + 1], 16)
    fa, fn = func_of(a)
    P("---- %08x..%08x (in %s) ----" % (a, b, fn or "nofunc"))
    for ins in dis_range(a, b):
        P("%08x  %-22s %-6s %s%s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, annot(ins)))
