"""XBE header / certificate / section claims of files.md (1.1, 4.3, 5.4)."""
from verify_files_r2_common import *

b = X.b
base = X.base
u = lambda o: struct.unpack_from("<I", b, o)[0]
print("base", hex(base))
print("InitFlags @0x124:", hex(u(0x124)))
cert_va = u(0x118)
co = cert_va - base
print("cert VA", hex(cert_va))
print("  TitleID      +0x08:", hex(u(co + 8)))
print("  TimeDate     +0x04:", hex(u(co + 4)))
print("  TitleName    +0x0C:", b[co + 0xC:co + 0xC + 80].decode("utf-16le").split("\0")[0])
print("  AltTitleIDs  +0x5C:", [hex(u(co + 0x5C + 4 * i)) for i in range(16) if u(co + 0x5C + 4 * i)])
print("  AllowedMedia +0x9C:", hex(u(co + 0x9C)))
print("  GameRegion   +0xA0:", hex(u(co + 0xA0)))
print("  GameRatings  +0xA4:", hex(u(co + 0xA4)))
print("  DiskNumber   +0xA8:", hex(u(co + 0xA8)))
print("  Version      +0xAC:", hex(u(co + 0xAC)))
print("  LANKey       +0xB0:", b[co + 0xB0:co + 0xC0].hex())
print("  SignatureKey +0xC0:", b[co + 0xC0:co + 0xD0].hex())
print("  cert size    +0x00:", hex(u(co)))
nsect, sect = struct.unpack_from("<II", b, 0x11C)
for i in range(nsect):
    o = sect - base + i * 0x38
    fl, va, vsz, raw, rsz, na = struct.unpack_from("<6I", b, o)
    name = b[na - base:b.index(b"\0", na - base)].decode()
    print("  section %-10s flags=0x%02x va=0x%06x vsz=0x%05x raw=0x%06x rsz=0x%05x" % (name, fl, va, vsz, raw, rsz))
print("XBOX kernel version / lib versions not needed here")
# $$XTIMAGE content magic
for n, va, vsz, raw, rsz in X.sections:
    if n == "$$XTIMAGE":
        print("$$XTIMAGE first bytes:", b[raw:raw + 8])
