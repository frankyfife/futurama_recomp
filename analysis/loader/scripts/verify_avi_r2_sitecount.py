"""Round-2: direct E8 call-site counts (linear-sweep validated) for game
functions named in avi.md."""
import struct
from verify_avi_r2_common import X, SECT, func_of, linear_starts, Tee
T = Tee("verify_avi_r2_sitecount.out")
TG = [0x9b6b0, 0x9ba40, 0xc97d0, 0x13f250, 0x9c140, 0x9be10, 0x9c2a0, 0x9c220,
      0xee220, 0xee080, 0xede90, 0xa24a0, 0xcbeb0, 0xccd00, 0xc96e0, 0xed8b0, 0xd26a0, 0xa26a0,
      0xcb5b0, 0x13f590, 0xd8720]
va, vsz, raw, rsz = SECT[".text"]
data = X.read(va, rsz)
st = linear_starts(va, va + rsz)
hits = {t: [] for t in TG}
for o in range(len(data) - 5):
    if data[o] != 0xE8:
        continue
    s = va + o
    t = (s + 5 + struct.unpack_from("<i", data, o + 1)[0]) & 0xFFFFFFFF
    if t in hits and s in st:
        hits[t].append(s)
for t in TG:
    T("%08x n=%-3d %s" % (t, len(hits[t]), " ".join("%x" % s for s in hits[t]) if len(hits[t]) <= 20 else ""))
