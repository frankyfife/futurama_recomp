"""Verify XBE header fields, certificate, TLS directory and section layout claimed in startup.md."""
from verify_startup_common import *
h = lambda off: struct.unpack_from("<I", X.b, off)[0]
print("base %#x SizeOfHeaders %#x SizeOfImage %#x" % (h(0x104), h(0x108), h(0x10c)))
print("cert %#x InitFlags %#x TLS %#x" % (h(0x118), h(0x124), h(0x12c)))
print("0x130 PE stack commit %#x | 0x134 heap reserve %#x | 0x138 heap commit %#x" % (h(0x130), h(0x134), h(0x138)))
c = h(0x118) - 0x10000
print("cert: size %#x TitleID %#x AllowedMedia %#x GameRegion %#x GameRatings %#x" % (h(c), h(c+8), h(c+0x9c), h(c+0xa0), h(c+0xa4)))
print("TitleName:", X.b[c+0xc:c+0xc+0x50].decode('utf-16le').rstrip('\0'))
t = h(0x12c)
s, e, idx, cb, zf, ch = struct.unpack("<6I", X.read(t, 24))
print("TLS: StartRaw %#x EndRaw %#x AddrIndex %#x Callbacks %#x ZeroFill %#x Char %#x; index section %s" % (s, e, idx, cb, zf, ch, X.section(idx)))
for n, va, vsz, raw, rsz in X.sections:
    print("  %-10s va %#08x vsz %#07x end %#08x raw %#07x" % (n, va, vsz, va + vsz, rsz))
last = max(va + vsz for n, va, vsz, raw, rsz in X.sections)
print("image end %#x, size from base %#x" % (last, last - 0x10000))
d = [x for x in X.sections if x[0] == ".data"][0]
print(".data bss %#x" % (d[2] - d[4]))
# data values
for a in (0x2d674c, 0x2d6748, 0x2d6744, 0x2d6740, 0x2d6750, 0x2d7cec, 0x2d7cf0, 0x2d85fc):
    print("  [%#x] = %#x" % (a, X.u32(a)))
