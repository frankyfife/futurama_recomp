"""Reference data for the host decoder test on REAL game files (M5 codec agent).

Extracts a few WAVs of every format class from the disc archives into run/m5-codec/refs/ (git-ignored, never into
the repo) and writes next to each <name>.wav the ffmpeg/PyAV decoding <name>.pcm (interleaved int16 little endian,
ffmpeg adpcm_ima_xbox for Xbox ADPCM, pcm_u8/pcm_s16le otherwise). The doctest case
"codec: real game files decode bit-exact like ffmpeg" in tests/audio_codec_tests.cpp compares the host decoder with
them when the environment variable FUT_AUDIO_CODEC_REFS points at that directory (otherwise it is skipped).

Usage: python analysis/loader/scripts/m5_codec_refs.py [--iso <image>] [--out <dir>] [--per-class N]
"""
import argparse
import io
import os
import struct
import sys

import av
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
from xdvdfs import XDVDFS  # noqa: E402
import futimg  # noqa: E402

DEFAULT_ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))


def wav_format(data):
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    o = 12
    fmt = None
    while o + 8 <= len(data):
        cid, csz = data[o:o + 4], struct.unpack_from("<I", data, o + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, o + 8)
        if cid == b"data":
            return fmt, csz
        o += 8 + csz + (csz & 1)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=DEFAULT_ISO)
    ap.add_argument("--out", default=os.path.join(REPO, "run", "m5-codec", "refs"))
    ap.add_argument("--per-class", type=int, default=2)
    ap.add_argument("--max-bytes", type=int, default=4 * 1024 * 1024, help="skip larger files")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    v = XDVDFS(a.iso)
    taken = {}
    written = 0
    for ip, s, sz, d in v.walk():
        if d or not ip.lower().endswith(".img"):
            continue
        hs = struct.unpack("<I", v.read(s, 4))[0]
        for path, fsz, foff in futimg.entries(v.read(s, hs)):
            if not path.lower().endswith(".wav") or fsz == 0 or fsz > a.max_bytes:
                continue
            data = v.read(s, fsz, foff)
            info = wav_format(data)
            if info is None or info[0] is None:
                continue
            (tag, ch, rate, _avg, _align, bits), _size = info
            cls = (tag, ch, rate, bits)
            if taken.get(cls, 0) >= a.per_class:
                continue
            try:
                c = av.open(io.BytesIO(data), format="wav")
                st = c.streams.audio[0]
                frames = [f.to_ndarray() for f in c.decode(st)]
                c.close()
            except Exception as e:  # noqa: BLE001
                print("skip %s: %s" % (path, e))
                continue
            arr = np.concatenate(frames, axis=1)
            if arr.dtype == np.uint8:
                arr = (arr.astype(np.int16) - 128) * 256
            if arr.shape[0] == ch:  # planar (channels, n)
                inter = arr.T.reshape(-1)
            else:  # packed (1, n * ch)
                inter = arr.reshape(-1)
            name = "%02X_%dch_%d_%db_%s" % (tag, ch, rate, bits, os.path.basename(path).replace(" ", "_"))
            with open(os.path.join(a.out, name), "wb") as f:
                f.write(data)
            with open(os.path.join(a.out, name[:-4] + ".pcm"), "wb") as f:
                f.write(inter.astype("<i2").tobytes())
            taken[cls] = taken.get(cls, 0) + 1
            written += 1
            print("%-60s %s codec=%s samples=%d" % (path, cls, st.codec_context.name, inter.size))
    print("%d files in %s" % (written, a.out))


if __name__ == "__main__":
    main()
