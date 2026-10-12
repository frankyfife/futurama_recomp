"""Compare an audio capture of the game with the original sound files - verification without ears (M5, ROADMAP
"M5 - Audio", analysis/loader/m5_audio.md 9.2, src/INTERFACES.md 4.30).

The capture is the WAV the host writes with [audio] capture_wav (float32 or pcm16, any channel count; stems
<name>.music.wav ... with capture_stems, the event list <name>.events.tsv with event_log). The references are decoded
independently of the host: loose WAVs are read from the game's .img archives inside the ISO (read-only, in memory,
never written into the repository) and decoded with PyAV/ffmpeg ('adpcm_ima_xbox' for Xbox ADPCM, PCM as is) or with
the own reference decoder of this file (--decoder own; bit-exact with ffmpeg, m5_audio.md 4.5).

For each reference it reports every occurrence in the capture:
  time offset (s), normalized correlation (NCC of the best fit), pitch/speed ratio (1.0 = played at the original
  rate), gain per capture channel (dB, least squares), residual SNR (dB), stereo balance (left - right, dB).
Long references (music, ambience) can also be followed through the capture chunk by chunk (--timeline): every jump of
the alignment is a dropout/underrun or a loop gap (the game's stream pump pads the last packet with zeros, so every
loop point has a known silence, m5_audio.md 4.3).
Capture overview: duration, rate, channels, RMS/peak per channel, clipped samples (|x| >= 1), digital-silence gaps.

Usage (paths with blanks in quotes; the ISO from --iso or the [paths] iso of --config):
    python tools/audio_compare.py --capture run/m5/frontend.wav --config run/m5/futurama.ini
        --ref "sounds/music/01 - front-end.wav" [--ref ...] [--ref-file some.wav] [--timeline]
        [--events run/m5/frontend.events.tsv [--stems]] [--window 10:60] [--json out.json]
    python tools/audio_compare.py --selftest          (synthetic signals only, no game data)

Library use (tools/m5_check.py): load_wav, ReferenceSource, match, timeline, overview, scan_events.
All decoded game audio stays in memory; --cache-dir (only below run/) keeps decoded references as .npy.
"""
from __future__ import annotations

import argparse
import configparser
import io
import json
import math
import struct
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path

import numpy as np

try:
    from scipy.signal import butter, resample_poly, sosfiltfilt
except ImportError:  # pragma: no cover - scipy is installed on the dev machine
    resample_poly = None
    butter = sosfiltfilt = None

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

DEFAULT_MIN_NCC = 0.6
SILENCE = 1e-6           # -120 dBFS, same as the host's level meter (capture_wav.h)
GAP_MIN_MS = 5.0
LEVEL_BAND_HZ = 2000.0  # timeline level "gain_lf_db": below this frequency

# ---------------------------------------------------------------------------------------------------------------
# WAV reading (what the host writes: PCM16, float32, WAVE_FORMAT_EXTENSIBLE; also the game's PCM8/PCM16/ADPCM)


@dataclass
class WavInfo:
    tag: int
    channels: int
    rate: int
    bits: int
    block_align: int
    data: bytes
    sub_tag: int = 0
    samples_per_block: int = 0


def parse_wav_bytes(raw: bytes) -> WavInfo:
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    off = 12
    fmt = None
    data = None
    while off + 8 <= len(raw):
        cid, size = raw[off:off + 4], struct.unpack_from("<I", raw, off + 4)[0]
        body = raw[off + 8: off + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate, _avg, ba, bits = struct.unpack_from("<HHIIHH", body, 0)
            sub, spb = 0, 0
            if tag == 0xFFFE and len(body) >= 26:
                sub = struct.unpack_from("<H", body, 24)[0]
            if tag == 0x69 and len(body) >= 20:
                spb = struct.unpack_from("<H", body, 18)[0]
            fmt = (tag, ch, rate, ba, bits, sub, spb)
        elif cid == b"data":
            data = body
            break
        off += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise ValueError("fmt or data chunk missing")
    tag, ch, rate, ba, bits, sub, spb = fmt
    return WavInfo(tag, ch, rate, bits, ba, data, sub, spb)


def wav_header(raw: bytes) -> dict:
    """Format, data size/offset and duration of a WAV without decoding (game files: PCM or Xbox ADPCM)."""
    off = 12
    fmt = None
    while off + 8 <= len(raw):
        cid, size = raw[off:off + 4], struct.unpack_from("<I", raw, off + 4)[0]
        if cid == b"fmt ":
            tag, ch, rate, _avg, ba, bits = struct.unpack_from("<HHIIHH", raw, off + 8)
            fmt = (tag, ch, rate, ba, bits)
        elif cid == b"data" and fmt is not None:
            tag, ch, rate, ba, bits = fmt
            frames = (size // ba) * 64 if tag == 0x69 else size // max(1, ba)
            return {"tag": tag, "channels": ch, "rate": rate, "block_align": ba, "bits": bits, "data_size": size,
                    "data_offset": off + 8, "frames": frames, "seconds": frames / rate if rate else 0.0}
        off += 8 + size + (size & 1)
    raise ValueError("fmt or data chunk missing")


def stream_loop_gap_ms(header: dict) -> float:
    """Silence the game's stream pump leaves at the loop point of a streamed WAV, measured against the full file
    (m5_audio.md 3.5, 4.3): packets of P = rate * blockAlign * 3 / 10 bytes (rounded down to blockAlign), the pump
    plays data bytes [0, dataSize - dataOffset) and zero-pads the last packet."""
    ba, rate = header["block_align"], header["rate"]
    packet = (rate * ba * 3 // 10) // ba * ba
    played = header["data_size"] - header["data_offset"]
    pad = (packet - played % packet) % packet
    bytes_per_second = rate * ba / 64.0 if header["tag"] == 0x69 else float(rate * ba)  # ADPCM: 64 frames per block
    return (pad - header["data_offset"]) / bytes_per_second * 1000.0


def wav_to_float(info: WavInfo) -> np.ndarray:
    """-> float32 array [channels, frames] in -1..1."""
    tag = info.sub_tag if info.tag == 0xFFFE else info.tag
    ch = info.channels
    if tag == 3 and info.bits == 32:
        a = np.frombuffer(info.data[: len(info.data) // (4 * ch) * 4 * ch], "<f4").astype(np.float32)
    elif tag == 1 and info.bits == 16:
        a = np.frombuffer(info.data[: len(info.data) // (2 * ch) * 2 * ch], "<i2").astype(np.float32) / 32768.0
    elif tag == 1 and info.bits == 8:
        a = (np.frombuffer(info.data[: len(info.data) // ch * ch], np.uint8).astype(np.float32) - 128.0) / 128.0
    elif tag == 1 and info.bits == 24:
        b = np.frombuffer(info.data[: len(info.data) // (3 * ch) * 3 * ch], np.uint8).reshape(-1, 3)
        v = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        a = v.astype(np.float32) / float(1 << 23)
    elif tag == 0x69:
        pcm = decode_xbox_adpcm(info.data, ch)
        return pcm.astype(np.float32) / 32768.0
    else:
        raise ValueError(f"unsupported WAV format tag 0x{tag:X}, {info.bits} bits")
    return a.reshape(-1, ch).T.copy()


def load_wav(path) -> tuple[np.ndarray, int]:
    """Capture or reference file on disk -> (float32 [channels, frames], rate)."""
    info = parse_wav_bytes(Path(path).read_bytes())
    return wav_to_float(info), info.rate


# ---------------------------------------------------------------------------------------------------------------
# Xbox ADPCM reference decoder (independent of the host's codec; variant "64 samples per block", step formula of
# ffmpeg adpcm_ima_xbox, m5_audio.md 4.5)

_STEP = np.array([7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
                  80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
                  544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
                  2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
                  11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767], np.int64)
_INDEX = np.array([-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8], np.int64)


def decode_xbox_adpcm(data: bytes, channels: int) -> np.ndarray:
    """Xbox ADPCM -> int16 [channels, frames]; vectorized over blocks and channels (the nibble loop runs 64 times)."""
    ba = 36 * channels
    nblocks = len(data) // ba
    if nblocks == 0:
        return np.zeros((channels, 0), np.int16)
    blk = np.frombuffer(data[: nblocks * ba], np.uint8).reshape(nblocks, ba)
    out = np.zeros((channels, nblocks, 64), np.int64)
    for c in range(channels):
        hdr = blk[:, 4 * c: 4 * c + 4]
        pred = (hdr[:, 0].astype(np.int64) | (hdr[:, 1].astype(np.int64) << 8))
        pred = np.where(pred >= 32768, pred - 65536, pred)
        idx = np.minimum(hdr[:, 2].astype(np.int64), 88)
        # nibbles of channel c in decode order: 8 rounds x 4 bytes, low nibble first
        cols = []
        for r in range(8):
            base = 4 * channels + r * 4 * channels + 4 * c
            for k in range(4):
                byte = blk[:, base + k].astype(np.int64)
                cols.append(byte & 0xF)
                cols.append(byte >> 4)
        nib = np.stack(cols, axis=1)  # [blocks, 64]
        out[c, :, 0] = pred
        for i in range(63):  # the 64th nibble is decoded but not output
            n = nib[:, i]
            step = _STEP[idx]
            diff = ((2 * (n & 7) + 1) * step) >> 3
            pred = np.clip(np.where(n & 8, pred - diff, pred + diff), -32768, 32767)
            idx = np.clip(idx + _INDEX[n], 0, 88)
            out[c, :, i + 1] = pred
    return out.reshape(channels, nblocks * 64).astype(np.int16)


def encode_xbox_adpcm(samples: np.ndarray) -> bytes:
    """Greedy IMA encoder into Xbox ADPCM blocks (synthetic self-test signals only). samples int [channels, n], n % 64 == 0."""
    ch, n = samples.shape
    out = bytearray()
    idx = [0] * ch
    for b in range(n // 64):
        hdr = bytearray()
        preds = []
        for c in range(ch):
            p = int(samples[c, b * 64])
            preds.append(p)
            hdr += struct.pack("<hBB", p, idx[c], 0)
        nibs = [[] for _ in range(ch)]
        for c in range(ch):
            p, i = preds[c], idx[c]
            for k in range(64):
                target = int(samples[c, b * 64 + 1 + k]) if k < 63 else int(samples[c, b * 64 + 63])
                step = int(_STEP[i])
                d = target - p
                code = 8 if d < 0 else 0
                d = abs(d)
                q = min(7, (d * 4) // step) if step else 0
                code |= q
                diff = ((2 * (code & 7) + 1) * step) >> 3
                p = max(-32768, min(32767, p - diff if code & 8 else p + diff))
                i = max(0, min(88, i + int(_INDEX[code])))
                nibs[c].append(code)
            idx[c] = i
        body = bytearray()
        for r in range(8):
            for c in range(ch):
                for k in range(4):
                    j = r * 8 + 2 * k
                    body.append(nibs[c][j] | (nibs[c][j + 1] << 4))
        out += hdr + body
    return bytes(out)


def build_wav(tag: int, channels: int, rate: int, bits: int, data: bytes) -> bytes:
    """WAV file bytes (PCM or Xbox ADPCM with the 20-byte fmt the game's files have)."""
    if tag == 0x69:
        ba = 36 * channels
        fmt = struct.pack("<HHIIHHHH", 0x69, channels, rate, rate * ba // 64, ba, 4, 2, 64)
    else:
        ba = channels * bits // 8
        fmt = struct.pack("<HHIIHH", tag, channels, rate, rate * ba, ba, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def decode_with_pyav(raw: bytes) -> tuple[np.ndarray, int]:
    import av  # PyAV (ffmpeg)
    with av.open(io.BytesIO(raw), format="wav") as c:
        st = c.streams.audio[0]
        frames = [f.to_ndarray() for f in c.decode(st)]
        rate = st.codec_context.sample_rate
        channels = st.codec_context.channels if hasattr(st.codec_context, "channels") else st.channels
    if not frames:
        return np.zeros((channels, 0), np.float32), rate
    if frames[0].ndim == 2 and frames[0].shape[0] == channels and channels > 1:
        a = np.concatenate(frames, axis=1)
    else:
        a = np.concatenate([f.reshape(-1) for f in frames])
        a = a.reshape(-1, channels).T if channels > 1 else a[None, :]
    if a.dtype == np.uint8:
        return (a.astype(np.float32) - 128.0) / 128.0, rate
    if a.dtype == np.int16:
        return a.astype(np.float32) / 32768.0, rate
    if a.dtype == np.int32:
        return a.astype(np.float32) / 2147483648.0, rate
    return a.astype(np.float32), rate


def decode_reference_bytes(raw: bytes, decoder: str = "pyav") -> tuple[np.ndarray, int]:
    if decoder == "pyav":
        return decode_with_pyav(raw)
    info = parse_wav_bytes(raw)
    return wav_to_float(info), info.rate


# ---------------------------------------------------------------------------------------------------------------
# Reference sources: the ISO's .img archives (read-only) or files on disk


def iso_from_config(config: Path) -> str | None:
    cp = configparser.ConfigParser(strict=False, inline_comment_prefixes=(";", "#"), interpolation=None)
    cp.read(config, encoding="utf-8")
    if cp.has_option("paths", "iso"):
        return cp.get("paths", "iso").strip().strip('"')
    return None


def normalize_name(name: str) -> str:
    n = name.strip().replace("\\", "/").lower()
    for prefix in ("d:/", "/"):
        if n.startswith(prefix):
            n = n[len(prefix):]
    if not n.endswith(".wav"):
        n += ".wav"
    return n


class ReferenceSource:
    """Index of every loose file in the game's .img archives; decodes WAVs in memory."""

    def __init__(self, iso: str | None, decoder: str = "pyav", cache_dir: Path | None = None):
        self.iso = iso
        self.decoder = decoder
        self.cache_dir = cache_dir
        self._index = None
        self._vol = None
        self._decoded: dict[str, tuple[np.ndarray, int]] = {}

    def _build_index(self):
        if self._index is not None:
            return
        if not self.iso:
            raise FileNotFoundError("no ISO given (--iso or --config with [paths] iso)")
        from xdvdfs import XDVDFS
        import futimg
        self._vol = XDVDFS(self.iso)
        self._index = {}
        for ip, s, _sz, is_dir in self._vol.walk():
            if is_dir or not ip.lower().endswith(".img"):
                continue
            hs = struct.unpack("<I", self._vol.read(s, 4))[0]
            for path, fsz, foff in futimg.entries(self._vol.read(s, hs)):
                if fsz:
                    self._index.setdefault(path.lower(), (s, fsz, foff, ip))

    def find(self, name: str) -> str | None:
        """Archive path for a game sound name (case-insensitive; a suffix match is accepted)."""
        self._build_index()
        n = normalize_name(name)
        if n in self._index:
            return n
        hits = [p for p in self._index if p.endswith("/" + n) or n.endswith("/" + p)]
        return hits[0] if len(hits) == 1 else (min(hits, key=len) if hits else None)

    def raw(self, path: str) -> bytes:
        self._build_index()
        key = self.find(path)
        if key is None:
            raise FileNotFoundError(path)
        s, fsz, foff, _ = self._index[key]
        return self._vol.read(s, fsz, foff)

    def decode(self, path: str) -> tuple[np.ndarray, int]:
        p = Path(path)
        if p.suffix.lower() == ".wav" and p.exists():
            return load_wav(p) if self.decoder == "own" else decode_reference_bytes(p.read_bytes(), self.decoder)
        key = self.find(path)
        if key is None:
            raise FileNotFoundError(path)
        if key in self._decoded:
            return self._decoded[key]
        cached = None
        if self.cache_dir is not None:
            safe = key.replace("/", "__").replace(" ", "_")
            cached = self.cache_dir / f"{safe}.{self.decoder}.npz"
            if cached.exists():
                z = np.load(cached)
                result = (z["a"], int(z["rate"]))
                self._decoded[key] = result
                return result
        result = decode_reference_bytes(self.raw(key), self.decoder)
        self._decoded[key] = result
        if cached is not None:
            cached.parent.mkdir(parents=True, exist_ok=True)
            np.savez(cached, a=result[0], rate=result[1])
        return result


# ---------------------------------------------------------------------------------------------------------------
# signal helpers


def resample(x: np.ndarray, src: int, dst: int) -> np.ndarray:
    if src == dst:
        return x.astype(np.float32, copy=False)
    g = math.gcd(int(src), int(dst))
    up, down = dst // g, src // g
    if resample_poly is not None:
        return resample_poly(x, up, down, axis=-1).astype(np.float32)
    n = int(round(x.shape[-1] * dst / src))
    t = np.arange(n) * (src / dst)
    return np.stack([np.interp(t, np.arange(x.shape[-1]), ch) for ch in np.atleast_2d(x)]).astype(np.float32)


def stretch(x: np.ndarray, ratio: float) -> np.ndarray:
    """Plays x at `ratio` times its speed (linear interpolation; ratio > 1 = shorter, higher)."""
    if abs(ratio - 1.0) < 1e-9:
        return x
    x2 = np.atleast_2d(x)
    n = int(math.floor((x2.shape[-1] - 1) / ratio)) + 1
    t = np.arange(n) * ratio
    return np.stack([np.interp(t, np.arange(x2.shape[-1]), ch) for ch in x2]).astype(np.float32)


_SINC_TAPS = 32
_SINC_PHASES = 2048
_sinc_table = None


def _sinc_kernel() -> np.ndarray:
    """[phase, tap] windowed-sinc kernel (Kaiser beta 8.6, 32 taps, 2048 phases, rows normalized to unit DC)."""
    global _sinc_table
    if _sinc_table is None:
        half = _SINC_TAPS // 2
        frac = np.arange(_SINC_PHASES)[:, None] / _SINC_PHASES
        k = np.arange(-half + 1, half + 1)[None, :]
        d = k - frac
        w = np.i0(8.6 * np.sqrt(np.clip(1.0 - (d / half) ** 2, 0.0, None))) / np.i0(8.6)
        h = np.sinc(d) * w
        _sinc_table = (h / h.sum(axis=1, keepdims=True)).astype(np.float64)
    return _sinc_table


def stretch_sinc(x: np.ndarray, ratio: float, start: float = 0.0, n: int | None = None) -> np.ndarray:
    """Like stretch(), with a 32-tap windowed-sinc interpolator (flat to about 0.45 x the rate) and a fractional
    start position: out[j] = x(start + j * ratio). For level measurements, where linear interpolation would dull the
    treble and an uncorrected speed drifts the alignment inside a long segment."""
    x2 = np.atleast_2d(x).astype(np.float64)
    if n is None:
        n = int(math.floor((x2.shape[-1] - 1 - start) / ratio)) + 1
    half = _SINC_TAPS // 2
    t = start + np.arange(max(0, n)) * ratio
    i0 = np.floor(t).astype(np.int64)
    phase = np.minimum(((t - i0) * _SINC_PHASES).astype(np.int64), _SINC_PHASES - 1)
    table = _sinc_kernel()
    pad = np.pad(x2, ((0, 0), (half, half + 2)))
    out = np.zeros((x2.shape[0], len(t)))
    for j, k in enumerate(range(-half + 1, half + 1)):
        idx = np.clip(i0 + k + half, 0, pad.shape[-1] - 1)
        out += pad[:, idx] * table[phase, j]
    return out.astype(np.float32)


def lowpass(x: np.ndarray, rate: int, cutoff_hz: float) -> np.ndarray:
    """Zero-phase 8th-order Butterworth low-pass (both signals of a fit get the same one)."""
    if not cutoff_hz or sosfiltfilt is None or cutoff_hz >= 0.45 * rate:
        return x
    return sosfiltfilt(butter(8, cutoff_hz, fs=rate, output="sos"), np.atleast_2d(x), axis=-1).astype(np.float32)


def aligned_gains(capture: np.ndarray, capture_m: np.ndarray, ref: np.ndarray, offset: int, ref_pos: int,
                  length: int, speed: float, band_hz: float = 0.0, rate: int = 48000) -> tuple[list, float]:
    """Least-squares gains of ref[ref_pos ...] played at `speed` (ref samples per capture sample) inside the
    capture near `offset`: the reference segment is resampled with the speed (no drift inside the segment) and
    aligned to a fraction of a sample (parabola through the correlation around the best integer lag), so the gain
    is not lowered by treble that lost its phase. -> (gains per capture channel in dB, ncc)."""
    margin = 8
    lo = max(0, ref_pos - margin)
    seg = ref[:, lo: ref_pos + int(math.ceil(length * speed)) + 2 * margin]
    st = stretch_sinc(seg, speed, start=float(ref_pos - lo), n=length)
    if st.shape[-1] < length or offset < 0 or offset + length > capture.shape[-1]:
        return [-200.0] * capture.shape[0], 0.0
    off, _ = _local_offset(capture_m, mono(st), offset, 4)
    sm = mono(st).astype(np.float64)
    corr = []
    for lag in (off - 1, off, off + 1):
        c = capture_m[max(0, lag): max(0, lag) + length].astype(np.float64)
        corr.append(float((c[: len(sm)] * sm[: len(c)]).sum()) if lag >= 0 else 0.0)
    den = corr[0] - 2.0 * corr[1] + corr[2]
    delta = 0.5 * (corr[0] - corr[2]) / den if den < 0.0 else 0.0
    delta = max(-0.5, min(0.5, delta))
    if abs(delta) > 1e-3:
        # capture[off + delta + j] <-> ref(ref_pos + j * speed): capture[off + j] <-> ref(ref_pos + (j - delta) * speed)
        st = stretch_sinc(seg, speed, start=float(ref_pos - lo) - delta * speed, n=length)
    seg_c = capture[:, off: off + length]
    if band_hz:
        # below band_hz only: independent of the treble response of the mixer's resampler (XAudio2's SRC dulls and
        # shifts the top octaves of 22 kHz material, measured -0.13 dB full band vs -0.01 dB below 2 kHz)
        seg_c, st = lowpass(seg_c, rate, band_hz), lowpass(st, rate, band_hz)
    gains, _snr, ncc, _bal = _fit(seg_c, st, 0)
    return gains, ncc


def mono(x: np.ndarray) -> np.ndarray:
    return x.mean(axis=0) if x.ndim == 2 else x


def db(x: float) -> float:
    return 20.0 * math.log10(x) if x > 1e-10 else -200.0


def ncc_curve(c: np.ndarray, r: np.ndarray, relative_floor: float = 0.0) -> np.ndarray:
    """Normalized cross-correlation of r (shorter) at every offset of c (1-D), via FFT. Windows of c below -90 dBFS
    (and, with relative_floor, below that share of the loudest window's energy) get 0: a nearly silent window would
    otherwise "correlate" with anything through rounding noise or a short edge of sound."""
    nc, nr = len(c), len(r)
    if nr == 0 or nc < nr:
        return np.zeros(0)
    n = 1 << int(math.ceil(math.log2(nc + nr)))
    xc = np.fft.irfft(np.fft.rfft(c, n) * np.conj(np.fft.rfft(r, n)), n)[: nc - nr + 1]
    cs = np.concatenate([[0.0], np.cumsum(c.astype(np.float64) ** 2)])
    win = cs[nr:] - cs[:-nr]
    e = np.sqrt(np.maximum(win, 0.0)) * math.sqrt(float((r.astype(np.float64) ** 2).sum())) + 1e-12
    out = xc / e
    floor = max(1e-9 * nr, relative_floor * float(win.max()) if win.size else 0.0)
    out[win < floor] = 0.0
    return out


def peaks(curve: np.ndarray, min_value: float, min_distance: int, top: int) -> list[tuple[int, float]]:
    out: list[tuple[int, float]] = []
    if curve.size == 0:
        return out
    order = np.argsort(-curve)
    for i in order[: max(top * 50, 200)]:
        v = float(curve[i])
        if v < min_value or len(out) >= top:
            break
        if all(abs(int(i) - p) >= min_distance for p, _ in out):
            out.append((int(i), v))
    return sorted(out)


# ---------------------------------------------------------------------------------------------------------------
# matching


@dataclass
class Match:
    reference: str
    offset_s: float
    ncc: float
    pitch_ratio: float
    gain_db: list
    snr_db: float
    balance_db: float
    length_s: float
    window: str = ""

    def ok(self, min_ncc: float, ratio_tolerance: float) -> bool:
        return self.ncc >= min_ncc and abs(self.pitch_ratio - 1.0) <= ratio_tolerance


def _fit(capture: np.ndarray, ref: np.ndarray, offset: int) -> tuple[list, float, float, float]:
    """Least-squares gain per capture channel, residual SNR, correlation of the fit."""
    n = ref.shape[-1]
    seg = capture[:, offset: offset + n]
    n = seg.shape[-1]
    if n == 0:
        return [-200.0] * capture.shape[0], -200.0, 0.0, 0.0
    r = ref[:, :n]
    if r.shape[0] != seg.shape[0]:
        if r.shape[0] == 2 and seg.shape[0] > 2:
            # stereo reference on a multichannel output: left side <- L, right side <- R, the rest <- mono
            left, right = side_channels(seg.shape[0])
            rows = [mono(r)] * seg.shape[0]
            for i in left:
                rows[i] = r[0]
            for i in right:
                rows[i] = r[1]
            r = np.stack(rows)
        else:
            r = np.repeat(mono(r)[None, :], seg.shape[0], axis=0)
    rr = (r.astype(np.float64) ** 2).sum(axis=1) + 1e-20
    g = (seg.astype(np.float64) * r).sum(axis=1) / rr
    model = g[:, None] * r
    res = seg - model
    snr = 10 * math.log10(max(float((model ** 2).sum()), 1e-20) / max(float((res ** 2).sum()), 1e-20))
    a, b = mono(seg).astype(np.float64), mono(model).astype(np.float64)
    ncc = float((a * b).sum() / (math.sqrt(float((a * a).sum()) * float((b * b).sum())) + 1e-20))
    gains = [db(abs(float(v))) for v in g]
    return gains, snr, ncc, balance_db(gains)


def side_channels(channels: int) -> tuple[list[int], list[int]]:
    """Left and right speakers of an output layout: stereo 0 / 1; 4 ch 0,2 / 1,3; 5.1 and 7.1 (FL FR FC LFE BL BR
    [SL SR]) front + back (+ side)."""
    if channels >= 8:
        return [0, 4, 6], [1, 5, 7]
    if channels >= 6:
        return [0, 4], [1, 5]
    if channels == 4:
        return [0, 2], [1, 3]
    return [0], [1]


def balance_db(gains_db: list, limit: float = 60.0) -> float:
    """Left minus right level (dB) from per-channel gains; +-limit when one side is silent."""
    if len(gains_db) < 2:
        return 0.0
    left, right = side_channels(len(gains_db))
    el = sum(10 ** (gains_db[i] / 10.0) for i in left if gains_db[i] > -199.0)
    er = sum(10 ** (gains_db[i] / 10.0) for i in right if gains_db[i] > -199.0)
    if el <= 0.0 and er <= 0.0:
        return 0.0
    return float(max(-limit, min(limit, 10.0 * math.log10((el + 1e-30) / (er + 1e-30)))))


def _local_offset(capture_m: np.ndarray, ref_m: np.ndarray, center: int, radius: int) -> tuple[int, float]:
    lo = max(0, center - radius)
    hi = min(len(capture_m), center + radius + len(ref_m))
    curve = ncc_curve(capture_m[lo:hi], ref_m, relative_floor=0.25)
    if curve.size == 0:
        return center, 0.0
    i = int(np.argmax(curve))
    return lo + i, float(curve[i])


def estimate_ratio(capture: np.ndarray, ref: np.ndarray, offset: int, rate: int,
                   guess: float = 1.0) -> tuple[float, int, float]:
    """Speed ratio of ref inside capture near `offset`: long references by aligning their first and last quarter,
    short ones by maximizing the correlation over stretched copies. -> (ratio, refined offset, ncc)."""
    cm, rm = mono(capture), mono(ref)
    n = len(rm)
    if n >= int(1.5 * rate):
        q = max(int(0.25 * n), int(0.3 * rate))
        rad = int(0.02 * rate) + int(abs(guess - 1.0) * n) + 4
        a, na = _local_offset(cm, rm[:q], offset, rad)
        b, nb = _local_offset(cm, rm[n - q:], offset + int((n - q) / guess), rad)
        if min(na, nb) > 0.3 and b > a:
            return (n - q) / (b - a), a, min(na, nb)
    best = (guess, offset, -1.0)
    for span, step in ((0.06, 0.004), (0.006, 0.0004), (0.0006, 0.00004)):
        center = best[0]
        for r in np.arange(center - span, center + span + step / 2, step):
            s = mono(stretch(ref, float(r)))
            off, v = _local_offset(cm, s, best[1], int(0.01 * rate) + 2)
            if v > best[2]:
                best = (float(r), off, v)
    return best


def match(capture: np.ndarray, rate: int, ref: np.ndarray, ref_rate: int, name: str, window=None,
          min_ncc: float = DEFAULT_MIN_NCC, top: int = 5, guess_ratio: float = 1.0) -> list[Match]:
    """Occurrences of `ref` in `capture` (both float [ch, n]); window = (start_s, end_s) or None."""
    refc = resample(np.atleast_2d(ref), ref_rate, rate)
    if guess_ratio != 1.0:
        refc = stretch(refc, guess_ratio)
    lo = 0 if window is None else max(0, int(window[0] * rate))
    hi = capture.shape[-1] if window is None else min(capture.shape[-1], int(window[1] * rate) + refc.shape[-1])
    cap = capture[:, lo:hi]
    if refc.shape[-1] > cap.shape[-1]:
        refc = refc[:, : cap.shape[-1]]  # a long reference (music) that is longer than the capture: its start
    if refc.shape[-1] < max(16, rate // 100):
        return []
    # coarse search at about 8 kHz (references longer than the window are cut to it)
    dec = max(1, rate // 8000)
    cm = mono(cap)
    rm = mono(refc)
    if len(rm) > len(cm):
        rm = rm[: len(cm)]
        refc = refc[:, : len(cm)]
    if dec > 1 and resample_poly is not None:
        cmd = resample_poly(cm, 1, dec)
        rmd = resample_poly(rm, 1, dec)
    else:
        cmd, rmd, dec = cm, rm, 1
    curve = ncc_curve(cmd, rmd)
    hits = peaks(curve, min_ncc * 0.7, max(1, len(rmd) // 2), top)
    out = []
    for i, _v in hits:
        off, _ = _local_offset(cm, rm, i * dec, 4 * dec + 2)
        ratio, off2, _ = estimate_ratio(cap, refc, off, rate)
        aligned = stretch(refc, ratio) if abs(ratio - 1.0) > 1e-6 else refc
        off3, _ = _local_offset(cm, mono(aligned), off2, 3)
        gains, snr, ncc, bal = _fit(cap, aligned, off3)
        if abs(ratio - 1.0) > 1e-6:
            # a jump inside the window (pause/resume, dropout) fakes a speed: keep the unstretched fit if it is better
            g1, s1, n1, b1 = _fit(cap, refc, off)
            if n1 >= ncc:
                ratio, off3, gains, snr, ncc, bal = 1.0, off, g1, s1, n1, b1
        total_ratio = ratio * guess_ratio
        if ncc < min_ncc:
            continue
        out.append(Match(name, (lo + off3) / rate, ncc, total_ratio, gains, snr, bal, refc.shape[-1] / rate,
                         "" if window is None else f"{window[0]:.3f}:{window[1]:.3f}"))
    return out


# ---------------------------------------------------------------------------------------------------------------
# timeline of a long (looping) reference: dropouts, underruns, loop gaps


@dataclass
class TimelineResult:
    reference: str
    chunks: int = 0
    matched: int = 0
    mean_ncc: float = 0.0
    start_s: float = -1.0
    loops: list = field(default_factory=list)      # [(capture time of the loop point s, gap ms)]
    jumps: list = field(default_factory=list)      # [(capture time s, jump ms)] other than loop points
    lost: list = field(default_factory=list)       # capture times where the reference was not found (even later)
    speed: float = 0.0
    speed_span_s: float = 0.0  # length of the jump-free stretch the speed was measured on
    gain_db: list = field(default_factory=list)     # full band (what is heard)
    gain_lf_db: list = field(default_factory=list)  # below LEVEL_BAND_HZ (the level model, resampler-independent)


def timeline(capture: np.ndarray, rate: int, ref: np.ndarray, ref_rate: int, name: str, chunk_s: float = 2.0,
             start_window=None, min_ncc: float = 0.8, jump_ms: float = 1.0, resync_s: float = 10.0) -> TimelineResult:
    """Follows a looping reference through the capture in chunks: each chunk is located within +-50 ms of where the
    previous one predicts it; after the reference's end the start is searched in the next 1 s (the loop gap). A chunk
    that is not there is searched up to resync_s later (a pause that resumes at the same position, a dropout): the
    alignment continues there and the shift is reported as a jump; a chunk found nowhere is reported as lost."""
    res = TimelineResult(name)
    refc = resample(np.atleast_2d(ref), ref_rate, rate)
    rm = mono(refc)
    cm = mono(capture)
    n_ref = len(rm)
    chunk = int(chunk_s * rate)
    if n_ref < chunk or len(cm) < chunk:
        return res
    # the earliest strong occurrence of the reference's start (a loop restart later in the capture looks the same)
    first = match(capture, rate, refc[:, :chunk], rate, name, window=start_window, min_ncc=min_ncc, top=8)
    if not first:
        return res
    pos = int(round(min(m.offset_s for m in first) * rate))
    res.start_s = pos / rate
    ref_pos = 0
    nccs = []
    anchors = []  # (capture sample, ref sample) of matched chunk starts, within one pass
    radius = int(0.05 * rate)
    while pos + chunk <= len(cm):
        if ref_pos + chunk > n_ref:
            # loop: the rest of the reference plays, then the gap, then the start again
            expected = pos + (n_ref - ref_pos)
            off, v = _local_offset(cm, rm[:chunk], expected, int(1.0 * rate))
            if expected + chunk > len(cm) or v < min_ncc:
                break
            res.loops.append((expected / rate, (off - expected) / rate * 1000.0))
            pos, ref_pos = off, 0
            continue
        off, v = _local_offset(cm, rm[ref_pos: ref_pos + chunk], pos, radius)
        res.chunks += 1
        quiet = float(np.sqrt((rm[ref_pos: ref_pos + chunk] ** 2).mean())) < 1e-4
        if v < min_ncc and not quiet and resync_s > 0:
            far = min(len(cm), pos + int(resync_s * rate) + chunk)
            curve = ncc_curve(cm[pos:far], rm[ref_pos: ref_pos + chunk], relative_floor=0.1)
            if curve.size:
                k = int(np.argmax(curve))
                if curve[k] >= min_ncc:
                    off, v = pos + k, float(curve[k])
        if v >= min_ncc or quiet:
            if not quiet:
                res.matched += 1
                nccs.append(v)
                anchors.append((off, ref_pos))
            if abs(off - pos) / rate * 1000.0 > jump_ms and not quiet:
                res.jumps.append((pos / rate, (off - pos) / rate * 1000.0))
            pos = (off if not quiet else pos) + chunk
        else:
            res.lost.append(pos / rate)
            pos += chunk
        ref_pos += chunk
    res.mean_ncc = float(np.mean(nccs)) if nccs else 0.0
    if len(anchors) >= 2:
        # speed over the longest stretch without a jump (a pause, dropout or loop restarts the stretch): capture and
        # reference distances between consecutive anchors agree there within 1 ms
        runs = [[anchors[0]]]
        for c, r in anchors[1:]:
            pc, pr = runs[-1][-1]
            if r > pr and abs((c - pc) - (r - pr)) <= 0.001 * rate:
                runs[-1].append((c, r))
            else:
                runs.append([(c, r)])
        best_run = max(runs, key=lambda run: run[-1][1] - run[0][1])
        (c0, r0), (c1, r1) = best_run[0], best_run[-1]
        res.speed = (r1 - r0) / (c1 - c0) if c1 > c0 else 1.0
        res.speed_span_s = (c1 - c0) / rate
    # Level: every matched chunk resampled with the measured speed and aligned to a fraction of a sample (the
    # Xbox pitch quantization plays 22050 Hz music at 0.99996, about 4 samples of drift per 2 s chunk: fitting the
    # chunk unstretched lowered the gain by 0.2-0.5 dB, review finding M5-5); median over the chunks.
    speed = res.speed if res.speed > 0.0 and res.speed_span_s >= 2.0 * chunk_s else 1.0
    chunk_gains, chunk_lf = [], []
    for off, rp in anchors:
        g, v = aligned_gains(capture, cm, refc, off, rp, chunk, speed)
        if v >= min_ncc:
            chunk_gains.append(g)
            chunk_lf.append(aligned_gains(capture, cm, refc, off, rp, chunk, speed, LEVEL_BAND_HZ, rate)[0])
    if chunk_gains:
        res.gain_db = [float(np.median([g[i] for g in chunk_gains])) for i in range(len(chunk_gains[0]))]
        res.gain_lf_db = [float(np.median([g[i] for g in chunk_lf])) for i in range(len(chunk_lf[0]))]
    return res


# ---------------------------------------------------------------------------------------------------------------
# capture overview


def gaps(x: np.ndarray, rate: int, min_ms: float = GAP_MIN_MS) -> list[tuple[float, float]]:
    """Runs of digital silence (every channel < -120 dBFS) of at least min_ms between sound (like the host meter)."""
    quiet = np.all(np.abs(np.atleast_2d(x)) < SILENCE, axis=0)
    if quiet.size == 0:
        return []
    d = np.diff(np.concatenate([[0], quiet.astype(np.int8), [0]]))
    starts, ends = np.where(d == 1)[0], np.where(d == -1)[0]
    out = []
    for s, e in zip(starts, ends):
        if s == 0 or e >= len(quiet):
            continue  # leading/trailing silence
        ms = (e - s) / rate * 1000.0
        if ms >= min_ms:
            out.append((float(s / rate), float(ms)))
    return out


def overview(x: np.ndarray, rate: int) -> dict:
    x = np.atleast_2d(x)
    n = x.shape[-1]
    rms = [db(float(np.sqrt((ch.astype(np.float64) ** 2).mean()))) if n else -200.0 for ch in x]
    peak = [db(float(np.abs(ch).max())) if n else -200.0 for ch in x]
    clipped = int((np.abs(x) >= 1.0).sum())
    g = gaps(x, rate)
    block = max(1, rate // 10)
    nb = n // block
    act = 0
    if nb:
        e = (x[:, : nb * block].astype(np.float64) ** 2).reshape(x.shape[0], nb, block).mean(axis=(0, 2))
        act = int((10 * np.log10(e + 1e-30) > -70).sum())
    return {"seconds": n / rate if rate else 0, "rate": rate, "channels": x.shape[0], "rms_db": rms,
            "peak_db": peak, "clipped": clipped, "clipped_share": clipped / max(1, x.size),
            "active_share": act / nb if nb else 0.0, "gaps": g}


# ---------------------------------------------------------------------------------------------------------------
# event list driven matching


def read_events(path: Path) -> list[dict]:
    rows = []
    lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    if not lines:
        return rows
    head = lines[0].split("\t")
    for ln in lines[1:]:
        parts = ln.split("\t")
        if len(parts) < len(head):
            parts += [""] * (len(head) - len(parts))
        row = dict(zip(head, parts))
        for k in ("sample", "tick"):
            try:
                row[k] = int(row.get(k, 0))
            except ValueError:
                row[k] = 0
        for k in ("seconds", "wall_s"):
            try:
                row[k] = float(row.get(k, 0))
            except ValueError:
                row[k] = 0.0
        try:
            row["category"] = int(row.get("category", -1))
        except ValueError:
            row["category"] = -1
        det = {}
        for kv in row.get("detail", "").split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                det[k] = v
        row["fields"] = det
        rows.append(row)
    return rows


STEM_OF_CATEGORY = {0: "effects", 1: "speech", 2: "music", 3: "movie"}
END_KINDS = ("end", "stop", "release", "flush", "pause")


def object_after(events: list[dict], index: int, horizon_s: float = 0.25) -> tuple[int, float | None]:
    """For the event at `index` (a play): the object's SetFrequency value (first "frequency" event of the same object
    within horizon_s after the play, else the last one since its "create"; 0 = unknown) and how long it played (until
    the object's next end/stop/release/flush/pause; None = until the end of the list)."""
    ev = events[index]
    obj = ev.get("object")
    hz = 0
    for j in range(index - 1, -1, -1):  # backwards to the object's create
        e = events[j]
        if e.get("object") != obj:
            continue
        if e.get("kind") == "frequency" and hz == 0:
            hz = _int(e["fields"].get("hz"))
        if e.get("kind") in ("create", "release"):
            break
    played = None
    for e in events[index + 1:]:
        if e.get("object") != obj:
            continue
        if e.get("kind") == "frequency" and e["seconds"] - ev["seconds"] <= horizon_s:
            hz = _int(e["fields"].get("hz")) or hz
        if e.get("kind") in END_KINDS or e.get("kind") in ("create", "play"):
            played = e["seconds"] - ev["seconds"]
            break
    return hz, played


def _int(text) -> int:
    try:
        return int(text)
    except (TypeError, ValueError):
        return 0


def scan_events(events: list[dict], captures: dict, refs: ReferenceSource, kinds=("play",), max_refs: int = 40,
                max_seconds: float = 20.0, min_ncc: float = DEFAULT_MIN_NCC, log=print) -> list[dict]:
    """Matches the sound of each 'play' event near its time: in the stem of its category if present (captures =
    {stem name: (array, rate)}), else in the mix. One result per (name, occurrence); at most max_refs names."""
    out = []
    seen: dict[str, int] = {}
    for idx, ev in enumerate(events):
        if ev.get("kind") not in kinds or not ev.get("name"):
            continue
        name = ev["name"]
        if seen.get(name, 0) >= 3 or (name not in seen and len(seen) >= max_refs):
            continue
        stem = STEM_OF_CATEGORY.get(ev["category"], "mix")
        cap, rate = captures.get(stem) or captures["mix"]
        try:
            ref, rr = refs.decode(name)
        except Exception as e:  # noqa: BLE001 - not a loose file (NIF sound cache), unreadable, ...
            out.append({"name": name, "event_s": ev["seconds"], "stem": stem, "error": str(e)[:120]})
            seen[name] = seen.get(name, 0) + 1
            continue
        hz, played = object_after(events, idx)
        guess = hz / rr if hz > 0 else 1.0  # the game's SetFrequency (rate * pitch, m5_audio.md 3.4)
        keep = max_seconds if played is None else max(0.05, min(max_seconds, played * guess))
        ref = ref[:, : int(keep * rr)]
        t = ev["seconds"]
        ms = match(cap, rate, ref, rr, name, window=(max(0.0, t - 0.25), t + 0.75), min_ncc=min_ncc * 0.5, top=1,
                   guess_ratio=guess)
        seen[name] = seen.get(name, 0) + 1
        row = {"name": name, "event_s": t, "stem": stem, "expected_ratio": guess,
               "played_s": played if played is not None else -1.0}
        if ms:
            m = ms[0]
            row.update({"offset_s": m.offset_s, "delay_ms": (m.offset_s - t) * 1000.0, "ncc": m.ncc,
                        "pitch_ratio": m.pitch_ratio, "gain_db": m.gain_db, "snr_db": m.snr_db,
                        "balance_db": m.balance_db, "length_s": m.length_s})
        else:
            row["error"] = "not found near the event"
        out.append(row)
    return out


# ---------------------------------------------------------------------------------------------------------------
# self-test (synthetic signals only)


def selftest() -> bool:
    rng = np.random.default_rng(5)
    ok = True

    def check(cond, text):
        nonlocal ok
        ok &= bool(cond)
        print(f"  [{'PASS' if cond else 'FAIL'}] {text}")

    print("selftest: Xbox ADPCM reference decoder vs PyAV/ffmpeg on synthetic blocks")
    t = np.arange(64 * 200) / 22050.0
    sig = np.stack([0.5 * np.sin(2 * np.pi * 440 * t) + 0.1 * rng.standard_normal(len(t)),
                    0.3 * np.sign(np.sin(2 * np.pi * 97 * t))])
    pcm = np.clip(np.round(sig * 32767), -32768, 32767).astype(np.int64)
    for ch in (1, 2):
        data = encode_xbox_adpcm(pcm[:ch])
        wav = build_wav(0x69, ch, 22050, 4, data)
        own, rate = decode_reference_bytes(wav, "own")
        try:
            ff, rate2 = decode_reference_bytes(wav, "pyav")
            same = ff.shape == own.shape and np.array_equal(np.round(ff * 32768), np.round(own * 32768))
            check(same and rate == rate2 == 22050, f"{ch} ch: own decoder == ffmpeg adpcm_ima_xbox ({own.shape[1]} frames)")
        except Exception as e:  # noqa: BLE001
            check(False, f"PyAV decode failed: {e}")
        err = np.abs(own - pcm[:ch, : own.shape[1]] / 32768.0).mean()
        check(err < 0.05, f"{ch} ch: ADPCM round trip mean error {err:.4f}")

    print("selftest: WAV parsing (float32, pcm16, extensible 6 ch)")
    x = (0.25 * rng.standard_normal((6, 1000))).astype(np.float32)
    fmt = struct.pack("<HHIIHHHHI", 0xFFFE, 6, 48000, 48000 * 24, 24, 32, 22, 32, 0x3F) + \
        struct.pack("<H", 3) + bytes([0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71])
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", x.size * 4) + \
        x.T.astype("<f4").tobytes()
    got = wav_to_float(parse_wav_bytes(b"RIFF" + struct.pack("<I", len(body)) + body))
    check(np.array_equal(got, x), "extensible float 6 ch")
    got16 = wav_to_float(parse_wav_bytes(build_wav(1, 2, 22050, 16, (np.array([[1000, -2000], [32767, -32768]],
                                                                                 "<i2")).tobytes())))
    check(np.allclose(got16, np.array([[1000, 32767], [-2000, -32768]]) / 32768.0), "pcm16 stereo")

    print("selftest: locating, pitch, gain, balance, gaps, clipping, timeline")
    rate_src, rate = 22050, 48000
    tt = np.arange(int(rate_src * 1.2)) / rate_src
    ref = (0.5 * np.sin(2 * np.pi * (300 + 400 * tt) * tt) * np.exp(-1.5 * tt) +
           0.08 * rng.standard_normal(len(tt)))[None, :].astype(np.float32)
    up = resample(ref, rate_src, rate)
    cap = np.zeros((2, rate * 8), np.float32)
    cap += (2e-3 * rng.standard_normal(cap.shape)).astype(np.float32)
    off = int(1.25 * rate)
    gl, gr = 10 ** (-8.5 / 20), 10 ** (-11.0 / 20)  # 2D voice at slider 0.75, pan -1 (m5_gain_model.out)
    cap[0, off: off + up.shape[1]] += gl * up[0]
    cap[1, off: off + up.shape[1]] += gr * up[0]
    fast = stretch(up, 1.03)  # played 3 % fast
    off2 = int(4.5 * rate)
    cap[:, off2: off2 + fast.shape[1]] += 0.3 * fast[0]
    ms = match(cap, rate, ref, rate_src, "chirp", min_ncc=0.5)
    m0 = next((m for m in ms if abs(m.offset_s - 1.25) < 0.01), None)
    check(m0 is not None and abs(m0.offset_s - 1.25) < 0.0005, f"located at {m0.offset_s if m0 else None} s (1.25)")
    if m0:
        check(abs(m0.pitch_ratio - 1.0) < 0.002, f"pitch ratio {m0.pitch_ratio:.5f} (1.0)")
        check(abs(m0.gain_db[0] + 8.5) < 0.3 and abs(m0.gain_db[1] + 11.0) < 0.3,
              f"gain {m0.gain_db[0]:.2f} / {m0.gain_db[1]:.2f} dB (-8.5 / -11.0)")
        check(abs(m0.balance_db - 2.5) < 0.3, f"balance {m0.balance_db:.2f} dB (2.5)")
        check(m0.ncc > 0.95, f"ncc {m0.ncc:.3f}")
    m1 = match(cap, rate, ref, rate_src, "chirp", window=(4.0, 6.5), min_ncc=0.3, top=1, guess_ratio=1.0)
    check(bool(m1) and abs(m1[0].pitch_ratio - 1.03) < 0.003,
          f"fast copy: pitch ratio {m1[0].pitch_ratio if m1 else None} (1.03)")
    hot = cap.copy()
    hot[0, 100:110] = 1.2
    ov = overview(hot, rate)
    check(ov["clipped"] == 10, f"clipped samples {ov['clipped']} (10)")

    # a looping "music" stream with a 103.4 ms zero-padded gap and one 20 ms dropout
    music_src = (0.3 * rng.standard_normal((2, rate_src * 7))).astype(np.float32)
    music = resample(music_src, rate_src, rate)
    gap = np.zeros((2, int(0.1034 * rate)), np.float32)
    stream = np.concatenate([np.zeros((2, rate // 2), np.float32), music, gap, music[:, : rate * 4]], axis=1)
    drop_at = rate // 2 + int(3.0 * rate)
    stream = np.concatenate([stream[:, :drop_at], np.zeros((2, int(0.02 * rate)), np.float32), stream[:, drop_at:]],
                            axis=1)
    g = gaps(stream, rate)
    check(len(g) == 2 and abs(g[0][1] - 20.0) < 0.1 and abs(g[1][1] - 103.4) < 0.1,
          f"gaps {[(round(a, 3), round(b, 1)) for a, b in g]} (20 ms at 3.5 s, 103.4 ms at 7.52 s)")
    tl = timeline(stream, rate, music_src, rate_src, "music", chunk_s=1.0)
    check(abs(tl.start_s - 0.5) < 0.001, f"timeline start {tl.start_s:.4f} s (0.5)")
    check(len(tl.loops) == 1 and abs(tl.loops[0][1] - 103.4) < 1.0,
          f"loop gaps {[(round(a, 3), round(b, 1)) for a, b in tl.loops]} (one of 103.4 ms)")
    check(len(tl.jumps) == 1 and abs(tl.jumps[0][1] - 20.0) < 1.0,
          f"jumps {[(round(a, 3), round(b, 1)) for a, b in tl.jumps]} (one dropout of 20 ms)")
    check(not tl.lost and tl.mean_ncc > 0.99, f"lost {tl.lost}, mean ncc {tl.mean_ncc:.4f}")

    # music at the Xbox pitch quantization (0.9999593) and -6.0 / -6.0 dB: the timeline level is exact
    # (music-like spectrum: white noise would decorrelate over the 4-sample drift of a chunk before it is aligned)
    soft_src = np.stack([np.convolve(ch, np.hanning(9) / np.hanning(9).sum(), mode="same")
                         for ch in music_src]).astype(np.float32)
    slow = stretch_sinc(resample(soft_src, rate_src, rate), 0.9999593)
    lvl = np.concatenate([np.zeros((2, rate // 4), np.float32), 10 ** (-6.0 / 20) * slow], axis=1)
    tl3 = timeline(lvl, rate, soft_src, rate_src, "music", chunk_s=2.0)
    check(len(tl3.gain_db) == 2 and all(abs(g + 6.0) <= 0.05 for g in tl3.gain_db + tl3.gain_lf_db)
          and abs(tl3.speed - 0.9999593) < 2e-6,
          f"slow music level {[round(g, 3) for g in tl3.gain_db]} dB (-6.0 +- 0.05), speed {tl3.speed:.7f} (0.9999593)")

    # a pause of 1.5 s that resumes at the same position (the pause menu) is followed as one jump
    paused = np.concatenate([music[:, : rate * 3], np.zeros((2, int(1.5 * rate)), np.float32), music[:, rate * 3:]],
                            axis=1)
    tl2 = timeline(paused, rate, music_src, rate_src, "music", chunk_s=1.0)
    check(len(tl2.jumps) == 1 and abs(tl2.jumps[0][1] - 1500.0) < 1.0 and not tl2.lost,
          f"pause: jumps {[(round(a, 3), round(b, 1)) for a, b in tl2.jumps]} (one of 1500 ms), lost {tl2.lost}")

    # event list helpers: pitch from the object's own frequency event, played time until its stop
    rows = [
        {"kind": "create", "object": "0x1", "seconds": 1.0, "fields": {"hz": "0"}},
        {"kind": "frequency", "object": "0x1", "seconds": 1.0, "fields": {"hz": "22050"}},
        {"kind": "release", "object": "0x1", "seconds": 1.5, "fields": {}},
        {"kind": "create", "object": "0x1", "seconds": 2.0, "fields": {"hz": "0"}},
        {"kind": "play", "object": "0x1", "seconds": 2.0, "fields": {"hz": "0"}},
        {"kind": "frequency", "object": "0x1", "seconds": 2.0, "fields": {"hz": "44100"}},
        {"kind": "play", "object": "0x2", "seconds": 2.1, "fields": {}},
        {"kind": "stop", "object": "0x1", "seconds": 2.4, "fields": {}},
    ]
    hz, played = object_after(rows, 4)
    check(hz == 44100 and played is not None and abs(played - 0.4) < 1e-9,
          f"event list: frequency {hz} (44100, not the earlier object's 22050), played {played} s (0.4)")
    print("selftest:", "PASS" if ok else "FAIL")
    return ok


# ---------------------------------------------------------------------------------------------------------------


def fmt_match(m: Match) -> str:
    return (f"{m.reference}: at {m.offset_s:9.3f} s  ncc {m.ncc:.3f}  pitch {m.pitch_ratio:.4f}  gain "
            + " / ".join(f"{g:6.2f}" for g in m.gain_db) + f" dB  balance {m.balance_db:+.2f} dB  SNR {m.snr_db:5.1f} dB"
            + f"  ({m.length_s:.2f} s)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--capture", type=Path)
    ap.add_argument("--ref", action="append", default=[], help="sound in the game's archives (e.g. sounds/button.wav)")
    ap.add_argument("--ref-file", action="append", default=[], type=Path, help="reference WAV on disk")
    ap.add_argument("--iso")
    ap.add_argument("--config", type=Path, help="INI whose [paths] iso is used")
    ap.add_argument("--decoder", choices=("pyav", "own"), default="pyav")
    ap.add_argument("--cache-dir", type=Path, help="keep decoded references (must lie below run/)")
    ap.add_argument("--window", help="START:END seconds of the capture to search")
    ap.add_argument("--min-ncc", type=float, default=DEFAULT_MIN_NCC)
    ap.add_argument("--top", type=int, default=5)
    ap.add_argument("--timeline", action="store_true", help="follow each reference chunk by chunk (loops, dropouts)")
    ap.add_argument("--events", type=Path, help="event list (.events.tsv): match the sound of every play event")
    ap.add_argument("--stems", action="store_true", help="with --events: search in the category stems next to --capture")
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    if a.selftest:
        return 0 if selftest() else 1
    if not a.capture:
        ap.error("--capture is required")
    if a.cache_dir and "run" not in [p.lower() for p in a.cache_dir.resolve().parts]:
        ap.error("--cache-dir must lie below run/ (decoded game audio never goes into the repository)")
    iso = a.iso or (iso_from_config(a.config) if a.config else None)
    refs = ReferenceSource(iso, a.decoder, a.cache_dir)
    cap, rate = load_wav(a.capture)
    result = {"capture": str(a.capture), "overview": overview(cap, rate), "matches": [], "timelines": [],
              "events": []}
    ov = result["overview"]
    print(f"capture {a.capture}: {ov['seconds']:.3f} s, {rate} Hz, {ov['channels']} ch, rms "
          + " / ".join(f"{v:.2f}" for v in ov["rms_db"]) + " dBFS, peak " + " / ".join(f"{v:.2f}" for v in ov["peak_db"])
          + f" dBFS, clipped {ov['clipped']} ({ov['clipped_share'] * 100:.6f} %), active {ov['active_share'] * 100:.1f} %")
    for s, ms in ov["gaps"][:20]:
        print(f"  gap at {s:9.3f} s: {ms:7.1f} ms")
    window = None
    if a.window:
        s0, s1 = a.window.split(":")
        window = (float(s0), float(s1))
    names = [(r, None) for r in a.ref] + [(str(p), p) for p in a.ref_file]
    for name, path in names:
        ref, rr = (load_wav(path) if a.decoder == "own" else decode_reference_bytes(path.read_bytes(), a.decoder)) \
            if path else refs.decode(name)
        print(f"reference {name}: {ref.shape[1] / rr:.3f} s, {rr} Hz, {ref.shape[0]} ch")
        for m in match(cap, rate, ref, rr, name, window=window, min_ncc=a.min_ncc, top=a.top):
            print("  " + fmt_match(m))
            result["matches"].append(asdict(m))
        if a.timeline:
            tl = timeline(cap, rate, ref, rr, name, start_window=window)
            print(f"  timeline: start {tl.start_s:.3f} s, {tl.matched}/{tl.chunks} chunks, mean ncc {tl.mean_ncc:.4f}, "
                  f"speed {tl.speed:.6f}, gain " + " / ".join(f"{g:.2f}" for g in tl.gain_db) + " dB")
            for t, ms in tl.loops:
                print(f"    loop point at {t:9.3f} s: gap {ms:7.1f} ms")
            for t, ms in tl.jumps:
                print(f"    JUMP at {t:9.3f} s: {ms:+8.1f} ms (dropout/underrun)")
            for t in tl.lost[:10]:
                print(f"    lost at {t:9.3f} s")
            result["timelines"].append(asdict(tl))
    if a.events:
        captures = {"mix": (cap, rate)}
        if a.stems:
            stem_base = a.capture.with_suffix("")
            for stem in ("music", "speech", "effects", "movie", "reverb"):
                p = Path(f"{stem_base}.{stem}.wav")
                if p.exists():
                    captures[stem] = load_wav(p)
        rows = scan_events(read_events(a.events), captures, refs, min_ncc=a.min_ncc)
        for r in rows:
            if "error" in r:
                print(f"  event {r['event_s']:9.3f} s {r['name']} [{r['stem']}]: {r['error']}")
            else:
                print(f"  event {r['event_s']:9.3f} s {r['name']} [{r['stem']}]: delay {r['delay_ms']:7.1f} ms ncc "
                      f"{r['ncc']:.3f} pitch {r['pitch_ratio']:.4f} (expected {r['expected_ratio']:.4f}) gain "
                      + " / ".join(f"{g:.2f}" for g in r["gain_db"]) + f" dB balance {r['balance_db']:+.2f} dB")
        result["events"] = rows
    if a.json:
        a.json.write_text(json.dumps(result, indent=1, default=float), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
