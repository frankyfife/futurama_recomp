"""M3d interpolation analysis: iterate the game files inside the ISO's .img archives (read-only).

Same approach as the M3 texconv census (tools/xdvdfs.py + tools/futimg.py). Nothing is written."""
import os
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS, SECTOR  # noqa: E402
import futimg  # noqa: E402

ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"


def iter_files(exts=None, archives=None):
    """yield (archive, path, bytes) for archived files with an extension in `exts` (lowercase, no dot)."""
    v = XDVDFS(ISO)
    for path, start, size, is_dir in sorted(v.walk()):
        if is_dir or not path.lower().endswith(".img"):
            continue
        name = os.path.basename(path)
        if archives and name not in archives:
            continue
        base = v.base + start * SECTOR
        v.f.seek(base)
        hsz = struct.unpack("<I", v.f.read(4))[0]
        v.f.seek(base)
        hdr = v.f.read(hsz)
        for p, fsize, foff in futimg.entries(hdr):
            if fsize is None:
                continue
            ext = p.rsplit(".", 1)[-1].lower() if "." in p else ""
            if exts and ext not in exts:
                continue
            v.f.seek(base + foff)
            yield name, p, v.f.read(fsize)


if __name__ == "__main__":
    import collections
    c = collections.Counter()
    dirs = collections.Counter()
    for a, p, b in iter_files():
        ext = p.rsplit(".", 1)[-1].lower() if "." in p else ""
        c[(a, ext)] += 1
        dirs[(a, p.split("/")[0])] += 1
    for k, n in sorted(c.items()):
        print(k, n)
    for k, n in sorted(dirs.items()):
        print("dir", k, n)
