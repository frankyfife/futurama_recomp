"""verify_avi_iso.py - independent, read-only re-check of the ISO contents relevant to avi.md:
 * XDVDFS directory walk implemented here (tree walk AND linear scan of every directory sector as
   cross-check), count of *.xmv per directory, presence of -NTSC / -PAL variants, *.txt files
 * header fields of every .xmv (magic/version/size/duration/audio descriptors)
 * dsstdfx.bin / SoundLookup.nif presence at disc level
Never writes to the image."""
import struct, collections, sys
from verify_avi_common import ISO
S = 2048
f = open(ISO, "rb")
base = None
for b in (0, 0x18300000):
    f.seek(b + 32 * S)
    if f.read(20) == b"MICROSOFT*XBOX*MEDIA": base = b; break
assert base is not None
root_sec, root_size = struct.unpack("<II", f.read(8))
def rd(sec, n, off=0):
    f.seek(base + sec * S + off); return f.read(n)

def tree(sec, size, prefix, out):
    data = rd(sec, size)
    st = [0]; seen = set()
    while st:
        o = st.pop()
        if o in seen or o + 14 > len(data): continue
        seen.add(o)
        l, r, start, fsz, attr, nl = struct.unpack_from("<HHIIBB", data, o)
        if l == 0xFFFF and r == 0xFFFF: continue
        nm = data[o + 14:o + 14 + nl].decode("latin-1")
        p = prefix + "/" + nm
        out.append((p, start, fsz, bool(attr & 0x10)))
        if attr & 0x10 and fsz: tree(start, fsz, p, out)
        if l: st.append(l * 4)
        if r: st.append(r * 4)

def linear(sec, size, prefix, out):
    data = rd(sec, size)
    for s0 in range(0, len(data), S):
        o = s0
        while o + 14 <= min(s0 + S, len(data)):
            l, r, start, fsz, attr, nl = struct.unpack_from("<HHIIBB", data, o)
            if l == 0xFFFF and r == 0xFFFF and data[o:o+4] == b"\xff\xff\xff\xff": break
            if nl == 0: break
            nm = data[o + 14:o + 14 + nl].decode("latin-1")
            p = prefix + "/" + nm
            out.append((p, start, fsz, bool(attr & 0x10)))
            if attr & 0x10 and fsz: linear(start, fsz, p, out)
            o = (o + 14 + nl + 3) & ~3

T = []; tree(root_sec, root_size, "", T)
L = []; linear(root_sec, root_size, "", L)
print("entries tree=%d linear=%d identical=%s" % (len(T), len(L), sorted(T) == sorted(L)))
xmv = sorted(p for p, s, z, d in T if p.lower().endswith(".xmv"))
print("xmv files: %d" % len(xmv))
print("  per dir:", dict(collections.Counter(p.rsplit("/", 1)[0] for p in xmv)))
print("  -PAL: %d  -NTSC: %d  other: %s" % (sum("-pal." in p.lower() for p in xmv), sum("-ntsc" in p.lower() for p in xmv),
      [p for p in xmv if "-pal." not in p.lower() and "-ntsc" not in p.lower()]))
print("txt files on disc level:", [p for p, s, z, d in T if p.lower().endswith(".txt")])
print("img archives:", [(p, z) for p, s, z, d in T if p.lower().endswith(".img")])
print("dsstdfx at disc level:", [p for p, s, z, d in T if "dsstdfx" in p.lower()])
print("\n== xmv headers")
agg = collections.Counter(); durs = []
for p, s, z, d in sorted(T):
    if not p.lower().endswith(".xmv"): continue
    h = rd(s, 0x100)
    nxt, first, mx, magic, ver, w, hh, dur, nas, ls = struct.unpack_from("<III4sIIIIHH", h, 0)
    aud = [struct.unpack_from("<HHIHH", h, 0x24 + 12 * i) for i in range(nas)]
    durs.append(dur)
    agg[(magic, ver, w, hh, nas, tuple(sorted(set(aud))))] += 1
    print("%-34s %9d nxt=%x first=%x max=%x %s v%d %dx%d dur=%dms nas=%d %s" % (p, z, nxt, first, mx, magic, ver, w, hh, dur, nas,
          sorted(set(aud))))
print("\n== aggregate (magic, ver, w, h, nas, audio set) -> count")
for k, v in agg.items(): print("  ", v, k)
print("duration min/max ms:", min(durs), max(durs))
