"""Find raw 32-bit immediate/data references to given VAs in the retail_eu XBE (all sections).
usage: python files_xref.py <hexva> [<hexva> ...]"""
import struct, sys
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
for a in sys.argv[1:]:
    v = int(a, 16)
    needle = struct.pack("<I", v)
    hits = []
    for n, va, vsz, raw, rsz in X.sections:
        blob = X.b[raw:raw + rsz]
        i = blob.find(needle)
        while i >= 0:
            hits.append((n, va + i))
            i = blob.find(needle, i + 1)
    print(f"{v:08x}: " + ", ".join(f"{n}:{h:x}" for n, h in hits))
