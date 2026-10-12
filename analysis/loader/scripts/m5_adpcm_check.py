"""Cross-check of the reference Xbox ADPCM decoder (m5_adpcm.py, both variants) against ffmpeg/PyAV on
real game files read in memory from the ISO (nothing written except the summary).
Output: scripts/out/m5_adpcm_check.out"""
import io
import struct
import sys

import av
import numpy as np

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS  # noqa: E402
import futimg  # noqa: E402
import m5_adpcm  # noqa: E402
from m5_common import Tee, out_path  # noqa: E402

ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
WANT = ["sounds/button.wav", "sounds/footsteps/gravel1.wav", "sounds/menu/theme22.wav",
        "sounds/reanimator.wav", "sounds/localized/german/menu/voice/6zb1-01.wav"]
T = Tee(out_path("m5_adpcm_check.out"))
v = XDVDFS(ISO)
files = {}
for ip, s, sz, d in v.walk():
    if d or not ip.lower().endswith(".img"):
        continue
    hs = struct.unpack("<I", v.read(s, 4))[0]
    for path, fsz, foff in futimg.entries(v.read(s, hs)):
        if fsz and path.lower() in WANT:
            files[path.lower()] = v.read(s, fsz, foff)
for name in WANT:
    data = files.get(name)
    if data is None:
        T("missing", name)
        continue
    o = 12
    fmt = None
    while o + 8 <= len(data):
        cid, csz = data[o:o + 4], struct.unpack_from("<I", data, o + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, o + 8)
        if cid == b"data":
            payload = data[o + 8:o + 8 + csz]
            break
        o += 8 + csz
    tag, ch, rate, avg, align, bits = fmt
    c = av.open(io.BytesIO(data), format="wav")
    st = c.streams.audio[0]
    codec = st.codec_context.name
    frames = [f.to_ndarray() for f in c.decode(st)]
    ff = np.concatenate(frames, axis=1) if frames[0].ndim == 2 else np.concatenate(frames)
    ff = ff.astype(np.int32)
    res = []
    for exact in (False, True):
        m5_adpcm.EXACT = exact
        for var in (64, 65):
            ref = np.array(m5_adpcm.decode(payload, ch, var), dtype=np.int32)
            n = min(ref.shape[1], ff.shape[1])
            res.append("%s/v%d: n=%d maxdiff=%d ndiff=%d" % ("exact" if exact else "bitwise", var, ref.shape[1],
                       int(np.abs(ref[:, :n] - ff[:, :n]).max()), int((ref[:, :n] != ff[:, :n]).sum())))
    m5_adpcm.EXACT = True
    T("%-48s codec=%s ch=%d rate=%d blocks=%d ffmpeg=%d samples" % (
        name, codec, ch, rate, len(payload) // (36 * ch), ff.shape[1]))
    for r in res:
        T("      " + r)
# synthetic round trip (no game data): 1 kHz sine, 22050 Hz, 0.5 s
sr = 22050
t = np.arange(64 * 172)
sig = (np.sin(2 * np.pi * 1000 * t / sr) * 12000).astype(int).tolist()
enc = m5_adpcm.encode([sig], 1)
dec = np.array(m5_adpcm.decode(enc, 1, 64)[0])
err = dec - np.array(sig)
T("synthetic 1 kHz sine round trip: %d samples, rms error %.1f (signal rms %.1f), SNR %.1f dB" % (
    len(dec), np.sqrt((err ** 2).mean()), np.sqrt((np.array(sig) ** 2).mean()),
    20 * np.log10(np.sqrt((np.array(sig) ** 2).mean()) / max(1e-9, np.sqrt((err ** 2).mean())))))
