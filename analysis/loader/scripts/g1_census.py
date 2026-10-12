"""G1 outlines (analysis/loader/g1_outlines.md 2): census of outline markers (QDOutline nodes, NiOutline objects,
NiData_Outline* / NiData_NoLines extra data, outline.tga, NiStencilProperty) in the NIF/UCF files of the disc
archives. Reads the ISO read-only, writes nothing."""
import sys, os, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS
import futimg
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
v = XDVDFS(ISO)
imgs = {p.lstrip("/"): (s, n) for p, s, n, d in v.walk() if not d}
pats = [b"QDOutline", b"NiOutline", b"NiData_Outline", b"NiData_NoLines", b"outline.tga", b"NiStencilProperty"]
for img in ["FutPAL00.img", "FutPAL01.img", "FutPAL02.img", "FutPAL04.img", "SysPAL.img"]:
    s, n = imgs[img]
    class F:
        pos = 0
        def seek(self, p): self.pos = p
        def read(self, k): d = v.read(s, k, self.pos); self.pos += len(d); return d
    hdr = futimg.open_header(F())
    tot = collections.Counter(); files = collections.defaultdict(list)
    for path, size, off in futimg.entries(hdr):
        if size is None or not path.lower().endswith((".nif", ".ucf", ".kfm", ".nfo")): continue
        d = v.read(s, size, off)
        for p in pats:
            c = d.count(p)
            if c:
                tot[p] += c; files[p].append(path)
    print(img, dict(tot))
    for p in (b"QDOutline", b"NiOutline", b"NiData_Outline", b"outline.tga"):
        if files[p]: print("   ", p.decode(), len(files[p]), files[p][:12])
