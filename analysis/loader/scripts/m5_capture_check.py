"""Analysis of an audio capture (WAV written by the host's [audio] capture_wav) against the original game
sounds - verification without ears (m5_audio.md section 9). Ready for M5; until the host writes captures
it runs on synthetic signals only (--selftest).

Functions (all numpy):
  load_wav(path)                  -> (float32 [channels, n], rate)
  reference(path_in_archive)      -> decoded original from the ISO, in memory (PyAV, bit-exact with m5_adpcm)
  locate(capture, ref, rate)      -> best offsets of ref in capture (normalized cross-correlation, FFT)
  fit(capture, ref, offset)       -> gain per channel, speed ratio (two-window alignment), residual SNR
  gaps(x, rate, min_ms)           -> silent stretches (digital zero / below -90 dBFS) longer than min_ms
  clipping(x)                     -> share of |x| >= 1.0
Usage:
  python m5_capture_check.py --selftest
  python m5_capture_check.py --capture run/analyse_m5/<run>/capture.wav --ref "sounds/music/01 - front-end.wav"
Outputs go to stdout (and, for real captures, to run/analyse_m5/ only - never into the repository).
"""
import argparse
import io
import struct
import sys
import wave

import numpy as np

ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"


def load_wav(path):
    with wave.open(path, "rb") as w:
        ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        a = np.frombuffer(raw, "<i2").astype(np.float32) / 32768.0
    elif sw == 4:
        a = np.frombuffer(raw, "<f4").astype(np.float32)  # host writes float32 (WAVE_FORMAT_IEEE_FLOAT)
    else:
        raise ValueError("unsupported sample width %d" % sw)
    return a.reshape(-1, ch).T.copy(), rate


def reference(path_in_archive):
    """Decode one WAV of the .img archives in memory (read-only on the ISO)."""
    import av
    sys.path.insert(0, r"D:\FuturamaDecomp\tools")
    from xdvdfs import XDVDFS
    import futimg
    v = XDVDFS(ISO)
    want = path_in_archive.lower().replace("\\", "/")
    for ip, s, sz, d in v.walk():
        if d or not ip.lower().endswith(".img"):
            continue
        hs = struct.unpack("<I", v.read(s, 4))[0]
        for path, fsz, foff in futimg.entries(v.read(s, hs)):
            if fsz and path.lower() == want:
                c = av.open(io.BytesIO(v.read(s, fsz, foff)), format="wav")
                st = c.streams.audio[0]
                fr = [f.to_ndarray() for f in c.decode(st)]
                a = np.concatenate(fr, axis=1 if fr[0].ndim == 2 else 0)
                a = a[None, :] if a.ndim == 1 else a
                if a.dtype == np.uint8:
                    return (a.astype(np.float32) - 128) / 128.0, st.codec_context.sample_rate
                return a.astype(np.float32) / 32768.0, st.codec_context.sample_rate
    raise FileNotFoundError(path_in_archive)


def resample(x, src, dst):
    """Linear-phase resampling via FFT (good enough for alignment and level checks)."""
    if src == dst:
        return x
    n = int(round(x.shape[-1] * dst / src))
    X = np.fft.rfft(x, axis=-1)
    m = n // 2 + 1
    Y = np.zeros(x.shape[:-1] + (m,), dtype=complex)
    k = min(m, X.shape[-1])
    Y[..., :k] = X[..., :k]
    return np.fft.irfft(Y, n=n, axis=-1) * (n / x.shape[-1])


def mono(x):
    return x.mean(axis=0) if x.ndim == 2 else x


def locate(capture, ref, top=5, min_corr=0.5):
    """Offsets (in capture samples) where ref (same rate) occurs; normalized cross-correlation via FFT."""
    c, r = mono(capture), mono(ref)
    n = 1 << int(np.ceil(np.log2(len(c) + len(r))))
    xc = np.fft.irfft(np.fft.rfft(c, n) * np.conj(np.fft.rfft(r, n)), n)[:len(c) - len(r) + 1]
    e = np.sqrt(np.convolve(c * c, np.ones(len(r)), "valid")) * np.sqrt((r * r).sum()) + 1e-12
    ncc = xc / e
    hits = []
    order = np.argsort(-ncc)
    for i in order:
        if ncc[i] < min_corr or len(hits) >= top:
            break
        if all(abs(i - h[0]) > len(r) // 2 for h in hits):
            hits.append((int(i), float(ncc[i])))
    return hits


def fit(capture, ref, offset):
    """Least-squares gain per capture channel (ref mono or per channel), speed from two windows, SNR."""
    n = ref.shape[-1]
    seg = capture[:, offset:offset + n]
    r = ref if ref.shape[0] == seg.shape[0] else np.repeat(mono(ref)[None, :], seg.shape[0], axis=0)
    gains = (seg * r).sum(axis=1) / ((r * r).sum(axis=1) + 1e-12)
    res = seg - gains[:, None] * r
    snr = 10 * np.log10(((gains[:, None] * r) ** 2).sum() / max((res ** 2).sum(), 1e-20))
    # speed: locate the first and last quarter separately
    q = n // 4
    a = locate(capture[:, max(0, offset - 2000):offset + q + 2000], ref[:, :q], top=1, min_corr=0.0)
    b = locate(capture[:, offset + n - q - 2000:offset + n + 2000], ref[:, n - q:], top=1, min_corr=0.0)
    speed = None
    if a and b:
        pa = max(0, offset - 2000) + a[0][0]
        pb = offset + n - q - 2000 + b[0][0]
        speed = (n - q) / max(1, (pb - pa))
    return gains, speed, snr


def gaps(x, rate, min_ms=5.0, floor_db=-90.0):
    env = np.abs(mono(x))
    quiet = env < 10 ** (floor_db / 20)
    out = []
    i = 0
    n = len(quiet)
    while i < n:
        if quiet[i]:
            j = i
            while j < n and quiet[j]:
                j += 1
            if (j - i) / rate * 1000 >= min_ms:
                out.append((i / rate, (j - i) / rate * 1000))
            i = j
        else:
            i += 1
    return out


def clipping(x):
    return float((np.abs(x) >= 1.0).mean())


def selftest():
    rng = np.random.default_rng(7)
    rate_src, rate_out = 22050, 48000
    t = np.arange(int(rate_src * 2.0)) / rate_src
    ref = (0.4 * np.sin(2 * np.pi * 330 * t) * np.exp(-t) + 0.05 * rng.standard_normal(len(t)))[None, :]
    ref = ref.astype(np.float32)
    up = resample(ref, rate_src, rate_out)
    cap = np.zeros((2, rate_out * 6), dtype=np.float32)
    off = int(1.25 * rate_out)
    g = (10 ** (-11 / 20), 10 ** (-13.5 / 20))  # 2D voice at slider 0.75, pan -1 (m5_gain_model.out)
    cap[0, off:off + up.shape[1]] += g[0] * up[0]
    cap[1, off:off + up.shape[1]] += g[1] * up[0]
    cap[:, int(4.0 * rate_out):int(4.0 * rate_out) + int(0.1034 * rate_out)] = 0.0  # loop gap 103.4 ms
    cap[:, :int(4.0 * rate_out)] += 1e-4 * rng.standard_normal((2, int(4.0 * rate_out)))
    cap[:, int(4.1034 * rate_out):] += 1e-4 * rng.standard_normal((2, cap.shape[1] - int(4.1034 * rate_out)))
    hits = locate(cap, up)
    gains, speed, snr = fit(cap, up, hits[0][0])
    gp = gaps(cap, rate_out, 20)
    print("selftest: located at %.4f s (expected 1.2500), ncc %.3f" % (hits[0][0] / rate_out, hits[0][1]))
    print("selftest: gains %.2f / %.2f dB (expected -11.00 / -13.50), speed %.5f, SNR %.1f dB" % (
        20 * np.log10(gains[0]), 20 * np.log10(gains[1]), speed, snr))
    print("selftest: gaps >= 20 ms: %s (expected one of ~103 ms at 4.0 s)" % [(round(a, 3), round(b, 1)) for a, b in gp])
    print("selftest: clipping share %.6f" % clipping(cap))
    ok = abs(hits[0][0] / rate_out - 1.25) < 0.001 and abs(20 * np.log10(gains[0]) + 11) < 0.2 and \
        abs(speed - 1.0) < 0.002 and any(abs(b - 103.4) < 2 for a, b in gp)
    print("selftest:", "PASS" if ok else "FAIL")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--capture")
    ap.add_argument("--ref", action="append", default=[])
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)
    cap, rate = load_wav(a.capture)
    print("capture: %s, %d ch, %d Hz, %.1f s, clipping %.6f" % (a.capture, cap.shape[0], rate,
                                                                 cap.shape[1] / rate, clipping(cap)))
    for g in gaps(cap, rate, 20):
        print("  gap at %.3f s: %.1f ms" % g)
    for r in a.ref:
        ref, rr = reference(r)
        ref = resample(ref, rr, rate)
        for off, ncc in locate(cap, ref):
            gains, speed, snr = fit(cap, ref, off)
            print("  %s at %.3f s ncc %.3f gain %s dB speed %s SNR %.1f dB" % (
                r, off / rate, ncc, " / ".join("%.2f" % (20 * np.log10(max(x, 1e-9))) for x in gains),
                "%.5f" % speed if speed else "?", snr))


if __name__ == "__main__":
    main()
