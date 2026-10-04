"""Review 2: XBE header / cert / TLS directory / sections / .data start values (key fact 12 + misc)."""
from verify_startup_r2_common import *
import struct
B = X.b
def h(o): return struct.unpack_from("<I", B, o)[0]
print("base 0x%X headersize 0x%X imagesize 0x%X imageheadersize 0x%X" % (h(0x104), h(0x108), h(0x10C), h(0x110)))
print("cert addr 0x%X  entry(enc) 0x%X  tls 0x%X" % (h(0x118), h(0x128), h(0x12C)))
print("stack commit [0x130]=0x%X heap reserve [0x134]=0x%X heap commit [0x138]=0x%X pe base [0x13C]=0x%X pe size [0x140]=0x%X" % tuple(h(x) for x in (0x130, 0x134, 0x138, 0x13C, 0x140)))
print("init flags [0x124]=0x%X" % h(0x124))
c = h(0x118) - h(0x104)
print("cert size 0x%X titleid 0x%X allowedmedia 0x%X region 0x%X ratings 0x%X" % (h(c), h(c + 8), h(c + 0x9C), h(c + 0xA0), h(c + 0xA4)))
print("cert title:", B[c + 0xC:c + 0xC + 80].decode("utf-16le").rstrip("\0"))
t = 0x2A5D2C
print("TLS dir @0x%X: StartRaw 0x%X EndRaw 0x%X Index 0x%X Callbacks 0x%X ZeroFill 0x%X Characteristics 0x%X" % ((t,) + tuple(X.u32(t + 4 * i) for i in range(6))))
print("TLS dir address in header [0x12C] = 0x%X" % h(0x12C))
sraw, eraw, zf = X.u32(t), X.u32(t + 4), X.u32(t + 0x10)
tds = ((zf - sraw + eraw + 0xF) & ~0xF) + 4
print("computed TlsDataSize 0x%X index %d" % (tds, int(tds / -4)))
print("index var section:", X.section(X.u32(t + 8)), "init value", X.read(X.u32(t + 8), 4) )
print()
for s in X.sections:
    print("%-10s va 0x%06X vsz 0x%06X raw 0x%06X rsz 0x%06X end 0x%06X" % (s[0], s[1], s[2], s[3], s[4], s[1] + s[2]))
# section flags
nsect, sect = struct.unpack_from("<II", B, 0x11C)
for i in range(nsect):
    o = sect - 0x10000 + i * 0x38
    fl = h(o)
    print("%-10s flags 0x%X (W=%d preload=%d exec=%d)" % (X.sections[i][0], fl, fl & 1, (fl >> 1) & 1, (fl >> 2) & 1))
print()
for a, nm in ((0x2D6748, "dt"), (0x2D6744, "fps"), (0x2D674C, "fixed"), (0x2D6740, "clearflags"), (0x2D7CEC, "pb size"), (0x2D7CF0, "pb kick"), (0x2D6750, "align"), (0x29BFF4, "6.0?")):
    v = X.u32(a)
    print("%s [0x%X] = 0x%08X float=%r" % (nm, a, v, struct.unpack("<f", struct.pack("<I", v))[0]))
