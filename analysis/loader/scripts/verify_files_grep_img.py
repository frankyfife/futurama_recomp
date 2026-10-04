"""Search byte patterns inside every file of the .img archives (read-only, from the ISO) and report which archive
entries contain them. usage: python verify_files_grep_img.py <archive.img|all> <pattern> [<pattern> ...]"""
import sys, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
IDX = r"D:\FuturamaDecomp\analysis\loader\scripts\out\verify_files_iso_index.tsv"
v = XDVDFS(ISO)
files = {p.lstrip("/").lower(): (s, n) for p, s, n, d in v.walk() if not d}
want = sys.argv[1].lower()
pats = [p.encode("latin-1") for p in sys.argv[2:]]
ents = collections.defaultdict(list)
for line in open(IDX, encoding="utf-8"):
    a = line.rstrip("\n").split("\t")
    if a[0] == "archive": continue
    if want != "all" and a[0].lower() != want: continue
    ents[a[0]].append((a[1], int(a[2]), int(a[3], 16)))
for arc, lst in ents.items():
    sec, _ = files[arc.lower()]
    for path, size, off in lst:
        data = v.read(sec, size, off)
        for p in pats:
            i = data.find(p)
            if i >= 0:
                ctx = data[max(0, i - 40):i + 80]
                print(f"{arc}:{path} @{i:#x} {p!r}: {ctx!r}")
