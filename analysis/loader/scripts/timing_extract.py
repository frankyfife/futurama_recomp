"""Extract the level scripts (scripts/*.ful, *.dbg) of the EU retail disc for timing_script.py.

Reads the ISO read-only (path from the M3 INI convention, override with argv[1]) and the .img archives inside it
without extracting the whole archives. Output: D:\\FuturamaDecomp\\run\\timing60\\pal\\<archive>\\<path>
(run\\ is git-ignored; no disc data goes into the repository).
Usage: python timing_extract.py [iso]
"""
import os
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS, SECTOR  # noqa: E402
import futimg  # noqa: E402

ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
DST = r"D:\FuturamaDecomp\run\timing60\pal"
WANT = (".ful", ".dbg")


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else ISO
    v = XDVDFS(iso)
    n = 0
    for path, start, size, is_dir in sorted(v.walk()):
        if is_dir or not path.lower().endswith(".img"):
            continue
        base = v.base + start * SECTOR
        v.f.seek(base)
        hdr_size = struct.unpack("<I", v.f.read(4))[0]
        v.f.seek(base)
        hdr = v.f.read(hdr_size)
        arch = os.path.splitext(os.path.basename(path))[0]
        for p, sz, off in futimg.entries(hdr):
            if sz is None or not p.lower().endswith(WANT):
                continue
            v.f.seek(base + off)
            data = v.f.read(sz)
            dst = os.path.join(DST, arch, p.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as o:
                o.write(data)
            n += 1
            print("%-10s %8d %s" % (arch, sz, p))
    print(n, "files ->", DST)


if __name__ == "__main__":
    main()
