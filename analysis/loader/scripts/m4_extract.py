"""m4_extract.py - copy single files out of the retail EU .img archives (read-only from the ISO) into
run\\m4_input_analysis\\ (git-ignored) and list their printable strings, for the M4 analysis of the
level scripts (scripts/<level>.ful/.dbg). Game data never goes into the repository.
usage: python m4_extract.py <img> <path-in-archive> [<path> ...]
       e.g. python m4_extract.py FutPAL01.img scripts/level1-1.ful scripts/level1-1.dbg"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m4_ldb import XDVDFS, ISO, OUT, archive_files  # noqa: E402


def main():
    img = sys.argv[1]
    v = XDVDFS(ISO)
    sec, files = archive_files(v, img)
    os.makedirs(OUT, exist_ok=True)
    for path in sys.argv[2:]:
        size, off = files[path.lower()]
        data = v.read(sec, size, off)
        name = path.replace("/", "_").replace("\\", "_")
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(data)
        strs = [m.group().decode("latin-1") for m in re.finditer(rb"[\x20-\x7e]{4,}", data)]
        with open(os.path.join(OUT, name + ".strings.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(strs) + "\n")
        print(os.path.join(OUT, name), size, "bytes,", len(strs), "strings")


if __name__ == "__main__":
    main()
