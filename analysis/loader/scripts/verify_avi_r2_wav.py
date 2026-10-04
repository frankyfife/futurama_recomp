"""Round-2: decode a few Xbox-ADPCM WAVs in memory with PyAV (read-only from
the ISO .img archives) and compare decoded sample counts with
(dataSize/blockAlign)*64. Also prints the fmt chunk (cbSize, samplesPerBlock)."""
import io, struct, sys
import av
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS  # noqa
import futimg  # noqa
from verify_avi_r2_common import Tee
T = Tee("verify_avi_r2_wav.out")
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
WANT = {"sounds/button.wav", "sounds/theme22.wav", "sounds/footsteps/gravel1.wav",
        "sounds/reanimator.wav", "sounds/music/01 - front-end.wav"}
v = XDVDFS(ISO)
for p, start, size, isdir in v.walk():
    if not p.lower().endswith(".img"):
        continue
    hs = struct.unpack("<I", v.read(start, 4))[0]
    for path, fsz, foff in futimg.entries(v.read(start, hs)):
        if fsz is None or path.lower() not in WANT:
            continue
        data = v.read(start, fsz if fsz < 4_000_000 else 4_000_000, foff)
        o, fmt, dsz = 12, None, None
        while o + 8 <= len(data):
            cid, csz = data[o:o + 4], struct.unpack_from("<I", data, o + 4)[0]
            if cid == b"fmt ":
                fmt = struct.unpack_from("<HHIIHH", data, o + 8)
                extra = data[o + 8 + 16:o + 8 + csz]
            if cid == b"data":
                dsz = csz
                break
            o += 8 + csz + (csz & 1)
        tag, ch, rate, avg, ba, bits = fmt
        exp = dsz // ba * 64 if tag == 0x69 else dsz // ba
        if fsz >= 4_000_000:
            T("%-40s %s fmt=%s extra=%s data=%d (PCM, not decoded)" % (path, p, fmt, extra.hex(), dsz))
            continue
        c = av.open(io.BytesIO(data), format="wav")
        s = c.streams.audio[0]
        n = sum(fr.samples for fr in c.decode(s))
        T("%-40s %s fmt=%s extra=%s data=%d codec=%s decoded=%d expected=%d" % (
            path, p, fmt, extra.hex(), dsz, s.codec_context.name, n, exp))
