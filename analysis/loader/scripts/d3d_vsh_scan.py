"""d3d_vsh_scan.py - search .rdata/.data for Xbox vertex shader program headers
(low word 0x2078 / 0x7778 / 0x6878, high word = instruction count 1..136).
Usage: python d3d_vsh_scan.py"""
import sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
n = 0
for name, va, vsz, raw, rsz in x.sections:
    if name not in (".rdata", ".data"):
        continue
    d = x.b[raw:raw + rsz]
    for off in range(0, rsz - 3, 4):
        v = struct.unpack_from("<I", d, off)[0]
        if (v & 0xffff) in (0x2078, 0x7778, 0x6878) and 0 < (v >> 16) <= 136:
            n += 1
            print(name, hex(va + off), hex(v), "next:", d[off + 4:off + 20].hex())
print("candidates:", n)
