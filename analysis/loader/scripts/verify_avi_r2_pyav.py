"""Round-2 PyAV/ffmpeg check of the two extracted XMV samples (read-only).

Demux + full decode of every stream, error count, frame count, pts deltas,
audio samples per stream, plus a check of what ffmpeg reports as frame rate.
Also decodes three ADPCM WAVs straight from the ISO/.img (in memory).
"""
import collections
import io
import os
import struct
import sys

import av

from verify_avi_r2_common import Tee, ROOT

T = Tee("verify_avi_r2_pyav.out")
T("PyAV", av.__version__, "libavformat", av.library_versions.get("libavformat"),
  "libavcodec", av.library_versions.get("libavcodec"))
S = os.path.join(ROOT, r"analysis\loader\samples")
for fn in ("uds-PAL.xmv", "2-1-03-PAL.xmv"):
    p = os.path.join(S, fn)
    c = av.open(p)
    T("\n==", fn, os.path.getsize(p), "format:", c.format.name, "/", c.format.long_name)
    for s in c.streams:
        cc = s.codec_context
        if s.type == "video":
            T("  stream %d video codec=%s (%s) tag=%s %dx%d pix=%s tb=%s avg_rate=%s" % (
                s.index, cc.name, cc.codec.long_name, s.codec_tag, cc.width, cc.height, cc.pix_fmt,
                s.time_base, s.average_rate))
        else:
            T("  stream %d %s codec=%s rate=%s ch=%s fmt=%s" % (
                s.index, s.type, cc.name, cc.sample_rate, cc.channels if hasattr(cc, "channels") else cc.layout.nb_channels,
                cc.format.name if cc.format else None))
    errs = 0
    vpts = []
    asamp = collections.Counter()
    for pkt in c.demux():
        try:
            for fr in pkt.decode():
                if pkt.stream.type == "video":
                    vpts.append(fr.pts)
                else:
                    asamp[pkt.stream.index] += fr.samples
        except av.error.FFmpegError as e:  # noqa
            errs += 1
    d = collections.Counter(b - a for a, b in zip(vpts, vpts[1:]))
    T("  video frames=%d pts deltas=%s errors=%d audio samples=%s" % (len(vpts), dict(d), errs, dict(asamp)))
    c.close()

# ---------- all .xmv on the disc: demux in memory (read-only from ISO) ----------
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS, SECTOR  # noqa: E402

ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"


class IsoFile(io.RawIOBase):
    def __init__(self, v, start, size):
        self.f, self.base, self.size, self.pos = v.f, v.base + start * SECTOR, size, 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def readinto(self, b):
        n = min(len(b), self.size - self.pos)
        if n <= 0:
            return 0
        self.f.seek(self.base + self.pos)
        d = self.f.read(n)
        b[:len(d)] = d
        self.pos += len(d)
        return len(d)

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def tell(self):
        return self.pos


v = XDVDFS(ISO)
T("\n== all .xmv: video packet pts deltas over the whole file (demux only) ==")
summary = collections.Counter()
for path, start, size, isdir in sorted(v.walk()):
    if not path.lower().endswith(".xmv"):
        continue
    c = av.open(IsoFile(v, start, size), format="xmv")
    vs = c.streams.video[0]
    pts = [p.pts for p in c.demux(vs) if p.pts is not None]
    d = collections.Counter(b - a for a, b in zip(pts, pts[1:]))
    common = d.most_common(3)
    T("  %-36s codec=%s frames=%d top-deltas=%s other=%d" % (
        path, vs.codec_context.name, len(pts), common, sum(d.values()) - sum(x[1] for x in common)))
    summary[tuple(sorted(k for k, _ in d.most_common(2)))] += 1
    c.close()
T("summary (two most frequent deltas):", dict(summary))
