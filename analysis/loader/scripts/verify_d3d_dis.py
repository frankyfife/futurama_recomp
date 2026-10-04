"""verify_d3d_dis.py - print disassembly of given VAs: python verify_d3d_dis.py addr[:len] ..."""
import sys
from verify_d3d_common import *
for arg in sys.argv[1:]:
    a, _, n = arg.partition(":")
    a = int(a, 16); n = int(n, 16) if n else 0x40
    f = func_of(a)
    print("==== %08x (%s)" % (a, f[3] if f else "?"))
    for ins in dis(a, n):
        print("%08x %-20s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
