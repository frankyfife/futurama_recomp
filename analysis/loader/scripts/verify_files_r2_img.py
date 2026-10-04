"""Independent re-implementation of the .img lookup semantics of FUN_0013FC40 (relative directory
field) and comparison with tools/futimg.py (absolute) and with the *_files.txt / gamelist files.
Reads the ISO read-only via tools/xdvdfs.py. Answers key facts 6, 10 and table 3.4."""
import sys, struct, collections, os
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS
import futimg
from verify_files_r2_common import ISO

v = XDVDFS(ISO)
tree = {p.lower(): (s, n, d) for p, s, n, d in v.walk()}
print("ISO base", hex(v.base), "root sector", v.root_sector, "entries", len(tree))
root_files = [p for p in tree if p.count("/") == 1 and not tree[p][2]]
print("root files:", sorted(root_files))
print("video files:", sum(1 for p in tree if p.startswith("/video/") and not tree[p][2]),
      "pal xmv:", sum(1 for p in tree if p.startswith("/video/") and p.endswith("-pal.xmv")))
print("SaveGameIcons:", [(p, tree[p][1]) for p in tree if p.startswith("/savegameicons/")])
print("dirs:", [p for p in tree if tree[p][2]])


def game_entries(hdr):
    """walk header the way FUN_0013FC40 does: dir field rel -> children [&rel+4, &rel+rel-4), sibling &rel+rel"""
    hs = struct.unpack_from("<I", hdr, 0)[0]
    out = []
    pad = []

    def walk(start, end, prefix):
        off = start
        while off < end:
            nul = hdr.index(b"\0", off)
            raw = bytearray(hdr[off:nul])
            if not raw:  # trailing pad bytes (header_size not multiple of 4)
                pad.append(off); break
            fld = off + ((nul - off + 4) & ~3)  # (strlen+4)&~3 as in the code
            if raw[0] & 0x80:
                raw[0] &= 0x7F
                rel = struct.unpack_from("<I", hdr, fld)[0]
                walk(fld + 4, fld + rel - 4, prefix + raw.decode("latin-1") + "/")
                off = fld + rel
            else:
                size, foff = struct.unpack_from("<II", hdr, fld)
                out.append((prefix + raw.decode("latin-1"), size, foff, off))
                off = fld + 8
    walk(4, hs, "")
    if pad:
        print("   empty name (padding) at", [hex(x) for x in pad], "hs=0x%X" % hs)
    return hs, out


IMGS = ["SysPAL.img", "FutPAL00.img", "FutPAL01.img", "FutPAL02.img", "FutPAL03.img", "FutPAL04.img"]
tot_bad = tot = tot_old_bad = 0


def old_futimg_entries(data):
    """verbatim logic of tools/futimg.py as it was when files.md was written (seen in this review)"""
    hdr_size = struct.unpack_from("<I", data, 0)[0]
    off, stack = 4, []
    while off < hdr_size:
        while stack and off >= stack[-1][1]:
            stack.pop()
        end = data.index(bytes(1), off)
        raw = bytearray(data[off:end])
        off = (end + 4) & ~3
        if not raw:
            break
        is_dir = bool(raw[0] & 0x80)
        raw[0] &= 0x7F
        name = raw.decode("latin-1")
        path = "/".join([s[0] for s in stack] + [name])
        if is_dir:
            dend = struct.unpack_from("<I", data, off)[0]
            off += 4
            stack.append((name, dend))
        else:
            size, foff = struct.unpack_from("<II", data, off)
            off += 8
            yield path, size, foff

allfiles = collections.OrderedDict()
for img in IMGS:
    s, n, d = tree["/" + img.lower()]
    hdr_size = struct.unpack("<I", v.read(s, 4))[0]
    hdr = v.read(s, hdr_size)
    hs, ents = game_entries(hdr)
    names_upper = [e for e in ents if any(c.isupper() for c in e[0])]
    mis = [e for e in ents if e[2] % 0x800]
    over = [e for e in ents if e[2] + e[1] > n]
    top = collections.Counter(e[0].split("/")[0] for e in ents)
    print("\n%s size=%d hdr=0x%X files=%d upper=%d unaligned=%d past_end=%d" % (img, n, hs, len(ents), len(names_upper), len(mis), len(over)))
    print("   top:", dict(top.most_common(8)))
    # futimg (absolute) result
    fut = [(p, sz, o) for p, sz, o in futimg.entries(hdr) if sz is not None]
    gm = {(sz, o): p for p, sz, o, _ in ents}
    bad = sum(1 for p, sz, o in fut if gm.get((sz, o), "").lower() != p.lower())
    print("   futimg.py paths differing from game semantics: %d of %d" % (bad, len(fut)))
    tot_bad += bad; tot += len(fut)
    # emulate the OLD tools/futimg.py (state before 2026-10-04 20:22): dir field read as ABSOLUTE end
    old = list(old_futimg_entries(hdr))
    bad_old = sum(1 for p, sz, o in old if gm.get((sz, o), "").lower() != p.lower())
    print("   OLD futimg.py (absolute dir end) paths differing: %d of %d" % (bad_old, len(old)))
    tot_old_bad += bad_old
    # compare with orig/retail_eu/<img>_files.txt
    stem = img[:-4]
    ft = r"D:\FuturamaDecomp\orig\retail_eu\%s_files.txt" % stem
    if os.path.exists(ft):
        lines = [l.strip().split(None, 2) for l in open(ft, encoding="latin-1") if l.strip()]
        badf = sum(1 for l in lines if gm.get((int(l[0]), int(l[1], 16)), "").lower() != l[2].lower())
        print("   %s: %d lines, differing from game semantics: %d" % (os.path.basename(ft), len(lines), badf))
    gl = r"D:\FuturamaDecomp\analysis\loader\scripts\files_%s_gamelist.txt" % stem
    if os.path.exists(gl):
        gtxt = open(gl, encoding="latin-1").read()
        missing = [p for p, _, _, _ in ents if p not in gtxt]
        print("   gamelist %s: %d of our paths missing" % (os.path.basename(gl), len(missing)))
    for p, sz, o, ho in ents:
        allfiles.setdefault(p.lower(), []).append(img)
    if img == "SysPAL.img":
        print("   hexdump 0x4C0..0x520:")
        for k in range(0x4C0, 0x520, 16):
            print("     %04x  %s  %s" % (k, hdr[k:k + 16].hex(" "), "".join(chr(c) if 32 <= c < 127 else "." for c in hdr[k:k + 16])))
        for p, sz, o, ho in ents:
            if "kanjii" in p:
                print("    ", p, sz, hex(o), "hdr@", hex(ho))
        bt = [e for e in ents if e[0].lower() == "build.txt"][0]
        print("   build.txt:", v.read(s, bt[1], bt[2]))
print("\nTOTAL futimg differing: %d of %d" % (tot_bad, tot))
dups = {p: l for p, l in allfiles.items() if len(l) > 1}
print("paths in >1 archive:", len(dups))
print("   build.txt in:", allfiles.get("build.txt"))
print("   dup top dirs:", collections.Counter(p.split("/")[0] for p in dups))
