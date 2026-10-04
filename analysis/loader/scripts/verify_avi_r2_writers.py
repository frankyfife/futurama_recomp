"""Round-2: every linear-sweep instruction in .text that references the
global vector 0x2f90bc..0x2f90c7 (read, write or address-taken) and the
rolloff curve 0x2ead3c..0x2ead53."""
import re
from verify_avi_r2_common import X, SECT, MD, func_of, Tee
T = Tee("verify_avi_r2_writers.out")
RANGES = [(0x2f90bc, 0x2f90c8, "vec 0x2f90bc"), (0x2ead3c, 0x2ead54, "rolloff curve"), (0x2eae50, 0x2eae60, "static zero vec 0x2eae50")]
va, vsz, raw, rsz = SECT[".text"]
code = X.read(va, rsz)
off = 0
while off < len(code):
    got = False
    for ins in MD.disasm(code[off:off + 0x4000], va + off):
        got = True
        off = ins.address - va + ins.size
        for m in re.finditer(r"0x([0-9a-f]{6,8})", ins.op_str):
            v = int(m.group(1), 16)
            for lo, hi, nm in RANGES:
                if lo <= v < hi:
                    ops = ins.op_str.split(", ")
                    kind = "WRITE" if (ins.mnemonic in ("mov", "fstp", "fst", "fistp", "add", "sub", "or", "and", "inc", "dec") and "[" in ops[0] and m.group(0) in ops[0]) else ("ADDR" if "[" not in ins.op_str.split(m.group(0))[0][-2:] and "ptr" not in ins.op_str else "READ")
                    fa, fn = func_of(ins.address)
                    T("%-24s %08x %-14s %-5s %s %s" % (nm, ins.address, fn or "nofunc", kind, ins.mnemonic, ins.op_str))
        if off >= len(code):
            break
    if not got:
        off += 1
