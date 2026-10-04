"""Read single files out of the retail EU .img archives directly from the ISO (read-only) and hexdump their start.
usage: python files_peek.py <img-name> <path-in-archive> [nbytes]"""
import struct, sys
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from xdvdfs import XDVDFS
from files_imgheader import ISO, game_walk
img, path = sys.argv[1], sys.argv[2].lower()
n = int(sys.argv[3]) if len(sys.argv) > 3 else 256
v = XDVDFS(ISO)
files = {p.lstrip("/"): (s, sz) for p, s, sz, d in v.walk() if not d}
sec, size = files[img]
hs = struct.unpack("<I", v.read(sec, 4))[0]
out = []; game_walk(v.read(sec, hs), 4, hs, "", out)
for p, s, o in out:
    if p.lower() == path:
        d = v.read(sec, min(s, n), o)
        print(f"{img}:{p} size={s} off={o:#x}")
        for i in range(0, len(d), 16):
            c = d[i:i+16]
            print(f"{i:04x} {c.hex(' '):48s} {''.join(chr(x) if 32 <= x < 127 else '.' for x in c)}")
        break
else:
    print("not found")
