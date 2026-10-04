"""cpu_datascan.py - dwords in .rdata/.data (and data regions inside code sections)
whose value lies in Xbox MMIO / physical / kernel windows.  Floats and HRESULTs make
the 0x80000000-0x8FFFFFFF and 0xB0-0xDF ranges useless, so only MMIO windows,
0x80010000-0x8007FFFF (kernel image) and exact 0xF0000000-style bases are reported.
usage: python cpu_datascan.py"""
import sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
R = [(0xFD000000, 0xFDFFFFFF, "NV2A"), (0xFE800000, 0xFE8FFFFF, "APU"), (0xFEC00000, 0xFEC00FFF, "AC97"),
     (0xFED00000, 0xFED0FFFF, "USB"), (0xFEF00000, 0xFEF003FF, "NIC"), (0xFF000000, 0xFF0FFFFF, "Flash"),
     (0x80010000, 0x8007FFFF, "kernel image?")]
for (n, va, vsz, raw, rsz) in X.sections:
    if n not in (".rdata", ".data", "D3D", "DSOUND", "XMV", "XPP", ".text"): continue
    b = X.b[raw:raw+rsz]
    hits = []
    for o in range(0, len(b)-3, 4):
        v = struct.unpack_from("<I", b, o)[0]
        for lo, hi, nm in R:
            if lo <= v <= hi:
                hits.append((va+o, v, nm))
    print(n, len(hits))
    for h in hits[:40]:
        print("   %08x: %08x %s" % h)
