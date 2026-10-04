"""avi_img_audio.py - read-only scan of the UDS .img archives inside the PAL ISO:
lists Video/*.txt (subtitles), dsstdfx.bin, SoundLookup and statistics over all .wav files
(format tag, channels, rate, bits, size) by reading each RIFF header in place.
Never writes into the ISO. Output: summary on stdout."""
import sys, struct, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS, SECTOR
from futimg import entries
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
v = XDVDFS(ISO)
imgs = [(p, s, z) for p, s, z, d in v.walk() if p.lower().endswith(".img")]
def rd(start, off, n):
    v.f.seek(v.base + start * SECTOR + off); return v.f.read(n)
fmtstat = collections.Counter(); sizes = collections.defaultdict(int); n_wav = 0
special = []
examples = {}
for path, start, size in sorted(imgs):
    hs = struct.unpack("<I", rd(start, 0, 4))[0]
    hdr = rd(start, 0, hs)
    for name, fsz, foff in entries(hdr):
        if fsz is None: continue
        ln = name.lower()
        if ln.startswith("video/") or ln.endswith("dsstdfx.bin") or "soundlookup" in ln or ln.endswith(".xmv") or ln.endswith(".wma"):
            special.append((path, name, fsz))
        if ln.endswith(".wav"):
            n_wav += 1
            h = rd(start, foff, 64)
            if h[:4] != b"RIFF" or h[8:12] != b"WAVE":
                fmtstat[("not-RIFF",)] += 1; continue
            # walk chunks inside the first 64 bytes for 'fmt '
            o = 12; key = None
            while o + 8 <= len(h):
                cid, cl = h[o:o+4], struct.unpack_from("<I", h, o + 4)[0]
                if cid == b"fmt ":
                    tag, ch, rate, avg, ba, bits = struct.unpack_from("<HHIIHH", h, o + 8)
                    key = ({1: "PCM", 0x69: "XboxADPCM"}.get(tag, hex(tag)), ch, rate, bits); break
                o += 8 + cl
            fmtstat[key] += 1; sizes[key] += fsz
            top = ln.split("/")[1] if ln.count("/") > 1 else ln
            examples.setdefault(key, name)
print("img archives:", [(p, z) for p, s, z in imgs])
print("special entries:")
for s in special: print("  ", s)
print("wav files:", n_wav)
for k, c in fmtstat.most_common():
    print("  %-40s count=%5d bytes=%10d e.g. %s" % (k, c, sizes[k], examples.get(k)))
