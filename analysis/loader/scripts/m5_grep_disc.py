"""Read-only byte search over all files in the .img archives and the ISO root (no extraction).
Usage: python m5_grep_disc.py <pattern> [<pattern> ...]   (ASCII, case-insensitive)
Prints archive, path, hit count and a short ASCII context of the first hit."""
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS  # noqa: E402
import futimg  # noqa: E402

ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
pats = [p.lower().encode() for p in sys.argv[1:]]
v = XDVDFS(ISO)
for ip, istart, isize, isdir in sorted(v.walk()):
    if isdir or not ip.lower().endswith(".img"):
        continue
    hs = struct.unpack("<I", v.read(istart, 4))[0]
    for path, fsz, foff in futimg.entries(v.read(istart, hs)):
        if fsz is None or path.lower().endswith((".wav", ".xmv")):
            continue
        data = v.read(istart, fsz, foff).lower()
        for p in pats:
            n = data.count(p)
            if n:
                i = data.find(p)
                ctx = data[max(0, i - 40):i + 60]
                ctx = "".join(chr(c) if 32 <= c < 127 else "." for c in ctx)
                print("%-12s %-50s %-14s %3d  %s" % (ip.rsplit("/", 1)[-1], path, p.decode(), n, ctx))
