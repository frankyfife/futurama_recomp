"""XBE header/certificate fields quoted in files.md (InitFlags, cert address, TitleID, timestamp, AllowedMedia,
GameRatings, SignatureKey, AlternateTitleIDs) and the section list (no $$XSIMAGE/$$XTINFO)."""
from verify_files_common import *
import struct
b = X.b
initflags = struct.unpack_from("<I", b, 0x124)[0]
certaddr = struct.unpack_from("<I", b, 0x118)[0]
print("InitFlags", hex(initflags), "cert VA", hex(certaddr))
c = certaddr - X.base
size, ts, tid = struct.unpack_from("<III", b, c)
print("cert size", hex(size), "timestamp", hex(ts), "TitleID", hex(tid))
print("TitleName", b[c + 0xc:c + 0xc + 80].decode("utf-16le").split("\0")[0])
print("AltTitleIDs", [hex(x) for x in struct.unpack_from("<16I", b, c + 0x5c)])
print("AllowedMedia", hex(struct.unpack_from("<I", b, c + 0x9c)[0]), "GameRegion", hex(struct.unpack_from("<I", b, c + 0xa0)[0]),
      "GameRatings", hex(struct.unpack_from("<I", b, c + 0xa4)[0]), "DiskNumber", struct.unpack_from("<I", b, c + 0xa8)[0], "Version", hex(struct.unpack_from("<I", b, c + 0xac)[0]))
print("LANKey", b[c + 0xb0:c + 0xc0].hex(), "SignatureKey", b[c + 0xc0:c + 0xd0].hex())
for n, va, vsz, raw, rsz in X.sections: print(f"  section {n:10s} va {va:#x} vsz {vsz:#x} raw {raw:#x} rsz {rsz:#x}")
# section flags for $$XTIMAGE
nsect, sect = struct.unpack_from("<II", b, 0x11C)
for i in range(nsect):
    o = sect - X.base + i * 0x38
    fl, va, vsz = struct.unpack_from("<3I", b, o)
    na = struct.unpack_from("<I", b, o + 0x14)[0]
    nm = b[na - X.base:b.index(b"\0", na - X.base)].decode()
    print(f"  {nm:10s} flags {fl:#x}")
