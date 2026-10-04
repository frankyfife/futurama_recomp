"""verify_d3d_r2_xbehdr.py - XBE library version table (header 0x160 count, 0x164 address),
XG* names in functions.tsv, Swizzler constructor callers and their stack cleanup,
47 .xmv files on the disc listing (review pass 2)."""
import struct, re
from verify_d3d_r2_common import *
b = XB.b
cnt, addr = struct.unpack_from("<II", b, 0x160)
print("library count", cnt, "table @%#x" % addr)
o = addr - XB.base
for i in range(cnt):
    name = b[o + 16 * i:o + 16 * i + 8].rstrip(b"\0").decode()
    maj, mnr, bld, flags = struct.unpack_from("<HHHH", b, o + 16 * i + 8)
    print("   %-8s %d.%d.%d flags=%#x" % (name, maj, mnr, bld, flags))
xg = [f[3] for f in F if re.search(r"(^|::)XG", f[3])]
print("functions with XG* names:", xg)
# swizzler ctor callers
callers = set()
for ins in sweep(".text"):
    if ins.mnemonic == "call" and ins.op_str == "0x155660":
        callers.add(owner(ins.address))
print("callers of 0x155660:", sorted((hex(a), n) for a, n in callers))
for a, n in sorted(callers):
    if a in FBY:
        sz = FBY[a][1]
        rets = sorted({i.op_str or "0" for i in cs.disasm(XB.read(a, sz), a) if i.mnemonic == "ret"})
        print("   %s rets %s" % (n, rets))
for a in (0x158790, 0x158990):
    sz = FBY[a][1]
    print("  %x %s size %d rets %s" % (a, FBY[a][3], sz, sorted({i.op_str or "0" for i in cs.disasm(XB.read(a, sz), a) if i.mnemonic == "ret"})))
lines = [l for l in open(ROOT + r"\orig\retail_eu\disc_files.txt", encoding="utf-8", errors="replace") if ".xmv" in l.lower()]
print(".xmv lines:", len(lines))
for l in lines:
    if "1-1-01" in l: print("   ", l.strip())
