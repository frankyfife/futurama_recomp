"""verify_avi_assets.py - independent read-only re-check of the audio assets (avi.md 2.4.6) and of the
archive-level claims (dsstdfx.bin, SoundLookup.nif, Video/*.txt, '01 - front-end.wav').
Own parser for the UDS .img header (format re-derived: u32 hdr_size; entries = NUL-terminated name
padded to 4, bit7 of first char = directory followed by u32 end-offset, file followed by u32 size,
u32 offset) and a full RIFF chunk walk (fmt + data chunk sizes) per WAV.
Never writes to the ISO."""
import struct, collections
from verify_avi_common import ISO
S = 2048
f = open(ISO, "rb")
base = None
for b in (0, 0x18300000):
    f.seek(b + 32 * S)
    if f.read(20) == b"MICROSOFT*XBOX*MEDIA": base = b; break
root_sec, root_size = struct.unpack("<II", f.read(8))
def rd(sec, n, off=0):
    f.seek(base + sec * S + off); return f.read(n)
def walk(sec, size, prefix=""):
    data = rd(sec, size); st = [0]; seen = set()
    while st:
        o = st.pop()
        if o in seen or o + 14 > len(data): continue
        seen.add(o)
        l, r, start, fsz, attr, nl = struct.unpack_from("<HHIIBB", data, o)
        if l == 0xFFFF and r == 0xFFFF: continue
        nm = data[o + 14:o + 14 + nl].decode("latin-1")
        yield prefix + "/" + nm, start, fsz, bool(attr & 0x10)
        if attr & 0x10 and fsz: yield from walk(start, fsz, prefix + "/" + nm)
        if l: st.append(l * 4)
        if r: st.append(r * 4)

def img_entries(hdr):
    hs = struct.unpack_from("<I", hdr, 0)[0]
    o = 4; dirs = []
    while o < hs:
        while dirs and o >= dirs[-1][1]: dirs.pop()
        e = hdr.index(b"\0", o)
        nm = bytearray(hdr[o:e]); o = (e + 4) & ~3
        if not nm: break
        isdir = nm[0] & 0x80; nm[0] &= 0x7f; nm = nm.decode("latin-1")
        if isdir:
            dirs.append((nm, struct.unpack_from("<I", hdr, o)[0])); o += 4
        else:
            sz, off = struct.unpack_from("<II", hdr, o); o += 8
            yield "/".join([d[0] for d in dirs] + [nm]), sz, off

imgs = sorted((p, s, z) for p, s, z, d in walk(root_sec, root_size) if p.lower().endswith(".img"))
stat = collections.Counter(); sizes = collections.Counter(); ex = {}
music = []; special = []; bad = []; nwav = 0; adpcm_check = collections.Counter()
for p, s, z in imgs:
    hs = struct.unpack("<I", rd(s, 4))[0]
    hdr = rd(s, hs)
    for name, sz, off in img_entries(hdr):
        ln = name.lower()
        if ln.endswith(".txt") or "dsstdfx" in ln or "soundlookup" in ln or ln.startswith("video") or "front-end" in ln:
            special.append((p, name, sz))
        if not ln.endswith(".wav"): continue
        nwav += 1
        h = rd(s, 256, off)
        if h[:4] != b"RIFF" or h[8:12] != b"WAVE": bad.append(name); continue
        o = 12; fmt = None; dsz = None
        while o + 8 <= len(h):
            cid = h[o:o + 4]; cl = struct.unpack_from("<I", h, o + 4)[0]
            if cid == b"fmt ": fmt = struct.unpack_from("<HHIIHH", h, o + 8); fcl = cl
            if cid == b"data": dsz = cl; break
            o += 8 + cl + (cl & 1)
        if fmt is None: bad.append(name); continue
        tag, ch, rate, avg, ba, bits = fmt
        k = ({1: "PCM", 0x69: "XboxADPCM"}.get(tag, hex(tag)), ch, rate, bits)
        stat[k] += 1; sizes[k] += sz; ex.setdefault(k, name)
        if tag == 0x69:
            adpcm_check[(ba == 36 * ch, fcl, avg == rate * 36 * ch // 64)] += 1
        if k == ("PCM", 2, 22050, 16): music.append((p, name, sz))
print("img archives:", [(p, z) for p, s, z in imgs])
print("wav entries:", nwav, "non-RIFF/unparsed:", len(bad))
for k, c in stat.most_common():
    print("  %-34s count=%5d bytes=%11d (%.1f MB) e.g. %s" % (k, c, sizes[k], sizes[k] / 1e6, ex[k]))
print("ADPCM fmt chunks (blockAlign==36*ch, fmt chunk size, file nAvgBytesPerSec==rate*36*ch/64):", dict(adpcm_check))
print("PCM16 stereo 22050 files (%d):" % len(music))
for m in music: print("   ", m)
print("special entries:")
for sp in special: print("   ", sp)
