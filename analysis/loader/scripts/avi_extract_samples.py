r"""avi_extract_samples.py - copy (read-only access to the ISO) two small XMV samples into
D:\FuturamaDecomp\analysis\loader\samples\ for decoder tests."""
import sys, os
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS, SECTOR
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
OUT = r"D:\FuturamaDecomp\analysis\loader\samples"
WANT = {"/video/uds-PAL.xmv", "/video/2-1-03-PAL.xmv"}
v = XDVDFS(ISO)
os.makedirs(OUT, exist_ok=True)
for path, start, size, d in v.walk():
    if path in WANT:
        v.f.seek(v.base + start * SECTOR)
        data = v.f.read(size)
        dst = os.path.join(OUT, os.path.basename(path))
        open(dst, "wb").write(data)
        print("wrote", dst, len(data))
