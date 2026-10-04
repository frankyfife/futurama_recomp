"""Round-2 disc check (read-only): XMV headers, WAV formats per directory,
subtitle .txt presence. Uses tools/xdvdfs.py and tools/futimg.py.
"""
import collections
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS, SECTOR  # noqa: E402
import futimg  # noqa: E402
from verify_avi_r2_common import Tee  # noqa: E402

T = Tee("verify_avi_r2_disc.out")
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
v = XDVDFS(ISO)
allent = list(v.walk())
T("ISO entries:", len(allent), "files:", sum(1 for e in allent if not e[3]),
  "dirs:", sum(1 for e in allent if e[3]))


def rd(start, off, n):
    return v.read(start, n, off)


# ---------------- XMV ----------------
xmv = sorted([e for e in allent if e[0].lower().endswith(".xmv")])
T("\n.xmv files:", len(xmv), "dirs:", sorted(set(p.rsplit("/", 1)[0] for p, *_ in xmv)))
combo = collections.Counter()
dur = []
hdrsz = []
for p, start, size, isdir in xmv:
    h = rd(start, 0, 0x100)
    nxt, first, mx = struct.unpack_from("<III", h, 0)
    magic = h[0xc:0x10]
    ver, w, hh, ms = struct.unpack_from("<IIII", h, 0x10)
    na = struct.unpack_from("<H", h, 0x20)[0]
    tr = []
    for i in range(na):
        tag, ch, rate, bits, fl = struct.unpack_from("<HHIHH", h, 0x24 + 12 * i)
        tr.append((tag, ch, rate, bits, fl))
    combo[(magic, ver, w, hh, na, tuple(sorted(set(tr))))] += 1
    dur.append((ms, p))
    hdrsz.append((nxt, first, mx))
    T("  %-40s %9d nxt=%06x first=%06x max=%06x %s v%d %dx%d %7.1fs tracks=%d %s" % (
        p, size, nxt, first, mx, magic, ver, w, hh, ms / 1000, na, sorted(set(tr))))
T("combos:")
for k, c in combo.items():
    T("  ", c, k)
T("duration min/max:", min(dur), max(dur))
T("next/first/max ranges:", min(x[0] for x in hdrsz), max(x[0] for x in hdrsz),
  min(x[1] for x in hdrsz), max(x[1] for x in hdrsz), min(x[2] for x in hdrsz), max(x[2] for x in hdrsz))
T("first == 0x1000-aligned?", all(x[1] % 0x1000 == 0 for x in hdrsz))

# ---------------- IMG archives / WAV ----------------
imgs = sorted([e for e in allent if e[0].lower().endswith(".img")])
fmt = collections.Counter()
fmtbytes = collections.Counter()
bydir = collections.defaultdict(collections.Counter)
txt = []
loc = collections.Counter()
for p, start, size, isdir in imgs:
    hs = struct.unpack("<I", rd(start, 0, 4))[0]
    hdr = rd(start, 0, hs)
    for path, fsz, foff in futimg.entries(hdr):
        if fsz is None:
            continue
        lp = path.lower()
        if lp.endswith(".txt"):
            txt.append((p, path, fsz))
        if not lp.endswith(".wav"):
            continue
        d = rd(start, foff, min(fsz, 4096))
        if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
            fmt[("nonriff",)] += 1
            continue
        o = 12
        f = None
        while o + 8 <= len(d):
            cid, csz = d[o:o + 4], struct.unpack_from("<I", d, o + 4)[0]
            if cid == b"fmt ":
                tag, ch, rate, avg, ba, bits = struct.unpack_from("<HHIIHH", d, o + 8)
                f = (hex(tag), ch, rate, bits)
                break
            o += 8 + csz + (csz & 1)
        fmt[f] += 1
        fmtbytes[f] += fsz
        top = "/".join(lp.split("/")[:2]) if lp.startswith("sounds/") else lp.split("/")[0] if "/" in lp else "<root>"
        bydir[top][f] += 1
        if "localized" in lp:
            loc[(lp.split("/")[2] if lp.count("/") >= 3 else "?", f)] += 1
T("\nWAV formats (tag,ch,rate,bits):", sum(fmt.values()))
for k, c in fmt.most_common():
    T("  %-28s %5d %10d bytes" % (k, c, fmtbytes[k]))
T("\nWAV by directory:")
for d_, c in sorted(bydir.items()):
    T("  %-30s %s" % (d_, dict(c)))
T("\nlocalized by language:")
for k, c in sorted(loc.items()):
    T("  ", k, c)
T("\n.txt in archives:", txt)
T(".txt on disc:", [e[0] for e in allent if e[0].lower().endswith(".txt")])

# ---------------- PCM file list with full archive paths ----------------
T("\nPCM (tag 1) WAVs with full archive paths:")
for p, start, size, isdir in imgs:
    hs = struct.unpack("<I", rd(start, 0, 4))[0]
    hdr = rd(start, 0, hs)
    for path, fsz, foff in futimg.entries(hdr):
        if fsz is None or not path.lower().endswith(".wav"):
            continue
        d = rd(start, foff, 64)
        o = 12
        while o + 8 <= len(d):
            cid, csz = d[o:o + 4], struct.unpack_from("<I", d, o + 4)[0]
            if cid == b"fmt ":
                tag, ch, rate = struct.unpack_from("<HHI", d, o + 8)
                if tag == 1:
                    T("  %-14s %-55s %9d ch=%d %d" % (p, path, fsz, ch, rate))
                break
            o += 8 + csz + (csz & 1)
    for path, fsz, foff in futimg.entries(hdr):
        if fsz is not None and ("dsstdfx" in path.lower() or "soundlookup" in path.lower()):
            T("  special: %s %s %d" % (p, path, fsz))
