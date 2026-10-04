"""Print (part of) a file from inside an .img archive, read straight from the ISO (read-only).
usage: python verify_files_peek.py <archive.img> <path-in-archive> [maxbytes] [--grep word]"""
import struct, sys
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
IDX = r"D:\FuturamaDecomp\analysis\loader\scripts\out\verify_files_iso_index.tsv"
v = XDVDFS(ISO)
files = {p.lstrip("/").lower(): (s, n) for p, s, n, d in v.walk() if not d}
arc, path = sys.argv[1], sys.argv[2].lower().replace("\\", "/")
mx = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 600
for line in open(IDX, encoding="utf-8"):
    a = line.rstrip("\n").split("\t")
    if a[0].lower() == arc.lower() and a[1] == path:
        size, off = int(a[2]), int(a[3], 16)
        sec, _ = files[arc.lower()]
        data = v.read(sec, size, off)
        print(f"{arc}:{path} size {size} offset {off:#x}")
        print(data[:mx])
        break
else:
    print("not found")
