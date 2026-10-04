"""XBE header fields relevant for startup + IMAGE_TLS_DIRECTORY at 0x2a5d2c."""
import sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
h = lambda off: struct.unpack_from("<I", X.b, off)[0]
print("base %x sizeofheaders %x sizeofimage %x imghdr %x" % (h(0x104), h(0x108), h(0x10c), h(0x110)))
print("cert %x initflags %x entry(enc) %x tls %x" % (h(0x118), h(0x124), h(0x128), h(0x12c)))
print("PE stack commit %x heap reserve %x heap commit %x PE base %x PE size %x" % (h(0x130), h(0x134), h(0x138), h(0x13c), h(0x140)))
print("kthunk(enc) %x" % h(0x158))
cert = h(0x118) - 0x10000
print("cert size %x titleid %x allowedmedia %x region %x ratings %x" % (h(cert), h(cert + 8), h(cert + 0x9c), h(cert + 0xa0), h(cert + 0xa4)))
tls = h(0x12c)
s, e, idx, cb, zf, ch = struct.unpack("<6I", X.read(tls, 24))
print("TLS dir @%x: StartRaw %x EndRaw %x (len %x) AddrOfIndex %x AddrOfCallBacks %x SizeOfZeroFill %x Characteristics %x" % (tls, s, e, e - s, idx, cb, zf, ch))
print("  raw template:", X.read(s, e - s).hex() if e > s else "(none, StartRaw==EndRaw)")
print("  index var section:", X.section(idx), " file value:", hex(X.u32(idx)) if X.read(idx, 4) else "(bss)")
if cb:
    print("  callbacks[0] =", hex(X.u32(cb)))

# entry (0xf84dd) computes TlsDataSize = ((EndRaw-StartRaw+ZeroFill+0xf)&~0xf)+4 and _tls_index = -(TlsDataSize/4)
tsz = ((e - s + zf + 0xf) & ~0xf) + 4
print("derived TlsDataSize = %#x, _tls_index = %d (stored by entry at %#x)" % (tsz, -(tsz // 4), idx))
