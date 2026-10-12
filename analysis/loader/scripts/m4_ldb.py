"""m4_ldb.py - dump the label -> text pairs of the game's string tables (language/*.ldb, format "0BDL")
directly from the retail EU ISO (read-only), for the menu and control-hint analysis of M4.

Format (derived here from frontend.ldb): "0BDL", u32 count, u32 label_off, u32 label_size, then 15
language records of 0x20 bytes from 0x10 {u32 off, u32 size, char name[24]} (English, French,
Italian, Spanish, German, Undefined0..9). The label block holds count records
"<label>\\0<group>\\0" padded with 0..2 NULs; each language block holds count NUL-terminated strings
in the same order as UTF-16LE (checked: the number of strings equals count).

The texts are game data: the output stays under run\\m4_input_analysis\\ (git-ignored), only short
quotes go into m4_input.md.
usage: python m4_ldb.py <ldb-name> [<ldb-name> ...]   (e.g. frontend hints_xbox ingame)"""
import os
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xdvdfs import XDVDFS  # noqa: E402
from files_imgheader import ISO, game_walk  # noqa: E402

OUT = r"D:\FuturamaDecomp\run\m4_input_analysis"


def archive_files(v, img):
    files = {p.lstrip("/"): (s, sz) for p, s, sz, d in v.walk() if not d}
    sec, _ = files[img]
    hs = struct.unpack("<I", v.read(sec, 4))[0]
    out = []
    game_walk(v.read(sec, hs), 4, hs, "", out)
    return sec, {p.lower(): (s, o) for p, s, o in out}


def parse(data):
    assert data[:4] == b"0BDL", data[:4]
    count, loff, lsize = struct.unpack_from("<III", data, 4)
    langs = []
    for i in range(15):
        off, size = struct.unpack_from("<II", data, 0x10 + i * 0x20)
        name = data[0x18 + i * 0x20:0x30 + i * 0x20].split(b"\0")[0].decode("latin-1")
        langs.append((name, off, size))
    # labels: "<label>\0<group>\0" followed by NUL padding
    labels = []
    p = loff
    end = loff + lsize
    while p < end and len(labels) < count:
        while p < end and data[p] == 0:
            p += 1
        if p >= end:
            break
        q = data.index(b"\0", p)
        lab = data[p:q].decode("latin-1")
        r = data.index(b"\0", q + 1)
        grp = data[q + 1:r].decode("latin-1")
        labels.append((lab, grp))
        p = r + 1
    texts = {}
    for name, off, size in langs:
        if size == 0 or name.startswith("Undefined"):
            continue
        blob = data[off:off + size]
        texts[name] = blob.decode("utf-16-le", "replace").split("\0")[:count]
    return labels, texts


def main():
    v = XDVDFS(ISO)
    os.makedirs(OUT, exist_ok=True)
    sec, files = archive_files(v, "SysPAL.img")
    for name in sys.argv[1:]:
        key = "language/%s.ldb" % name.lower()
        size, off = files[key]
        data = v.read(sec, size, off)
        labels, texts = parse(data)
        lines = ["# %s: %d labels, languages %s" % (key, len(labels), ", ".join(texts))]
        for i, (lab, grp) in enumerate(labels):
            en = texts.get("English", [""] * len(labels))
            de = texts.get("German", [""] * len(labels))
            lines.append("%-24s %-12s EN=%r | DE=%r" % (lab, grp, en[i] if i < len(en) else "?",
                                                       de[i] if i < len(de) else "?"))
        p = os.path.join(OUT, "%s.ldb.txt" % name.lower())
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        print(p, len(labels))


if __name__ == "__main__":
    main()
