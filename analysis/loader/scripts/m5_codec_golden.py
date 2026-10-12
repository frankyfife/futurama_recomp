"""Golden values for tests/audio_codec_tests.cpp and tests/audio_spatial_tests.cpp (M5 codec/3D agent).

Synthetic data only (no game files):
  1. Random Xbox ADPCM blocks from an LCG (state = state * 1664525 + 1013904223, byte = state >> 24; header step
     index % 89, reserved byte 0) decoded by ffmpeg/PyAV adpcm_ima_xbox (mono/stereo; ffmpeg's decoder supports
     only 1-2 channels) and by the reference decoder m5_adpcm.py; FNV-1a 64 of the interleaved int16 samples.
  2. DSOUND distance attenuation in mB with the truncation of FUN_0024974A (cvttss2si 0x24E2B4) and
     XAudioCalculatePitch values (round(4096 * log2((float)hz / 48000))).
Usage: python analysis/loader/scripts/m5_codec_golden.py
"""
import io
import math
import os
import struct
import sys

import av
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m5_adpcm  # noqa: E402


def lcg_bytes(seed, n):
    x = seed & 0xFFFFFFFF
    out = bytearray()
    for _ in range(n):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append(x >> 24)
    return out


def make_blocks(seed, ch, blocks, limit_index=True):
    raw = lcg_bytes(seed, 36 * ch * blocks)
    for b in range(blocks):
        for c in range(ch):
            o = b * 36 * ch + 4 * c
            if limit_index:
                raw[o + 2] = raw[o + 2] % 89
            raw[o + 3] = 0
    return bytes(raw)


def wav(data, ch, rate=22050):
    fmt = struct.pack("<HHIIHHHH", 0x69, ch, rate, 36 * ch * rate // 64, 36 * ch, 4, 2, 64)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def fnv(samples):
    h = 0xcbf29ce484222325
    for b in np.asarray(samples, dtype="<i2").tobytes():
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def ffmpeg_decode(data, ch):
    c = av.open(io.BytesIO(wav(data, ch)), format="wav")
    st = c.streams.audio[0]
    frames = [f.to_ndarray() for f in c.decode(st)]
    arr = np.concatenate(frames, axis=1)
    return (arr.T.reshape(-1) if arr.shape[0] == ch else arr.reshape(-1)), st.codec_context.name


def f32(v):
    return float(np.float32(v))


def trunc_mb(g):
    if g <= 0:
        return -10000
    if g >= 1:
        return 0
    return int(f32(0.3010299956639812 * math.log2(g) * 2000.0))


CURVE = [1.0, 0.9, 0.8, 0.5, 0.25, 0.0]


def dist_mb(d, mn, mx):
    d, mn, mx = f32(d), f32(mn), f32(mx)
    if d <= mn:
        return 0
    if d >= mx:
        return -10000
    span = f32((mx - mn) / 6)
    off = d - mn
    idx = min(int(off / span), 5)
    if idx == 0:
        lo, hi, t = 1.0, CURVE[0], off
    else:
        lo, hi, t = CURVE[idx - 1], CURVE[idx], off - idx * span
    return trunc_mb(f32((hi - lo) * (1.0 / span) * t + lo))


def main():
    for seed, ch, blocks in ((1, 1, 40), (2, 2, 40)):
        data = make_blocks(seed, ch, blocks)
        ff, codec = ffmpeg_decode(data, ch)
        ref = np.array(m5_adpcm.decode(data, ch, 64)).T.reshape(-1)
        print("adpcm seed %d ch %d blocks %d: %s n=%d hash=0x%016X, m5_adpcm equal=%s, first=%s" % (
            seed, ch, blocks, codec, ff.size, fnv(ff), bool(ff.size == ref.size and (ff == ref).all()),
            [int(v) for v in ff[:6]]))
    for seed, ch, blocks, limit in ((3, 6, 10, True), (4, 1, 20, False)):
        ref = np.array(m5_adpcm.decode(make_blocks(seed, ch, blocks, limit), ch, 64)).T.reshape(-1)
        print("adpcm seed %d ch %d blocks %d (m5_adpcm only%s): hash=0x%016X" % (
            seed, ch, blocks, "" if limit else ", step index > 88 clamped", fnv(ref)))
    for mn, mx in ((5, 80), (30, 40)):
        ds = [0, mn, mn + (mx - mn) / 6, mn + 2 * (mx - mn) / 6, mn + 3 * (mx - mn) / 6, mn + 4 * (mx - mn) / 6,
              mn + 5 * (mx - mn) / 6, mx - 0.01, mx, mx + 1, mn + 0.37 * (mx - mn)]
        print("distance mB min %g max %g: %s" % (mn, mx, ", ".join("%.4f:%d" % (d, dist_mb(d, mn, mx)) for d in ds)))
    pitches = []
    for hz in (48000, 24000, 96000, 22050, 44100, 11025, 1000, 191999):
        p = 0 if hz == 48000 else int(np.rint(4096 * math.log2(f32(np.float32(hz) * np.float32(2.0833333e-05)))))
        pitches.append("%d:%d" % (hz, p))
    print("XAudioCalculatePitch: " + ", ".join(pitches))


if __name__ == "__main__":
    main()
