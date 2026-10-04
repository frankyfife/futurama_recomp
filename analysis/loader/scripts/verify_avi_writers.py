"""verify_avi_writers.py - find instructions in .text that WRITE to given absolute addresses
(e.g. 0x2f90bc listener velocity source, 0x2ead3c rolloff curve, 0x2e7b70 movie suffix,
0x2d659c vibration option). Raw scan for disp32 == addr (+0..+0x20), capstone-validated, and reports
whether the memory operand is the destination.
Usage: python verify_avi_writers.py <addr_hex> [<span_hex>] ..."""
import struct
from verify_avi_common import *

def insn_at_containing(a):
    """find an instruction whose bytes contain position a (validated decode from owner start)."""
    o = owner(a)
    starts = []
    if o is not None:
        for ins in md.disasm(x.read(o, a - o + 16), o):
            if ins.address <= a < ins.address + ins.size: return ins
            if ins.address > a: break
    for back in range(1, 12):
        st = a - back
        ins = next(md.disasm(x.read(st, 16), st), None)
        if ins and ins.address + ins.size > a + 3 and ins.address + ins.size <= a + 8:
            return ins
    return None

args = sys.argv[1:]
i = 0
while i < len(args):
    base = int(args[i], 16); span = 4
    if i + 1 < len(args) and len(args[i + 1]) <= 3:
        span = int(args[i + 1], 16); i += 1
    i += 1
    print("== refs to %08x..%08x" % (base, base + span))
    _, va, vsz, raw, rsz = SEC[".text"]
    d = x.b[raw:raw + rsz]
    seen = set()
    for j in range(len(d) - 4):
        v = struct.unpack_from("<I", d, j)[0]
        if base <= v < base + span:
            ins = insn_at_containing(va + j)
            if ins is None or ins.address in seen: continue
            seen.add(ins.address)
            ops = ins.op_str
            dst = ops.split(",")[0]
            w = ("WRITE" if ("ptr [0x%x]" % v) in dst and ins.mnemonic in ("mov", "fstp", "fst", "or", "and", "add", "sub", "inc", "dec", "movsd", "movss", "fistp")
                 else ("store?" if ("[0x%x]" % v) in dst else "read/imm"))
            o = owner(ins.address)
            print("  %08x %-6s %-40s %-8s in %s" % (ins.address, ins.mnemonic, ops, w, ("%08x %s" % (o, name(o))) if o else "NO-GHIDRA-FUNCTION"))
