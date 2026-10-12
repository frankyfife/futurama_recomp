"""Gain model of the original game + DSOUND, as derived in m5_audio.md (reference for host unit tests).

Game side (SoundVoice::vf11 0xCC5E0):
  v   = categoryVolume[cat] * voiceVolume                      (both 0..1)
  mB  = 0 if v >= 1; -10000 if v <= 0; else (int)((1 - v) * -1000)     (0xCC86F..0xCC901, _ftol)
  2D mixbins (0xCC92F..0xCC9D3): bin0 = (int)(mB * min(1, 1 + pan)), bin1 = (int)(mB * min(1, 1 - pan))
DSOUND side:
  voice volume register = lVolume - headroom, headroom 600 mB for non-3D voices, 0 for 3D (0x248A63)
  per mixbin attenuation (FUN_0024C3CA) = -(lVolume - headroom + mixbinVolume[bin]) - 3D terms,
  clamped to 0..0xFFF in 1/64 dB (i.e. at most 63.98 dB)
  3D distance term (FUN_00249893): 0 inside MinDistance; with a rolloff curve of N points the gain is
  linear in amplitude between the implicit 1.0 at MinDistance and curve[i] at Min + (i+1)*(Max-Min)/N;
  beyond Max: -10000 when DSBCAPS_MUTE3DATMAXDISTANCE (0x20000, set by the game), else clamped to Max.
  gain -> mB via 2000*log10 (FUN_0024974A), <= 0 -> -10000.
Output: scripts/out/m5_gain_model.out
"""
import math

import numpy as np

from m5_common import Tee, out_path

T = Tee(out_path("m5_gain_model.out"))
CURVE = [1.0, 0.9, 0.8, 0.5, 0.25, 0.0]  # g_rolloffCurve 0x2EAD3C, written in SoundMgr::ctor 0xCCDCD..0xCCDFF


def game_mb(v):
    """v is a float32 in the game (fstp dword at 0xCC862); the x87 rest is wider than float32 and _ftol
    truncates toward zero, so 0.9 gives -100 (Python doubles without the float32 step would give -99)."""
    v = float(np.float32(v))
    if v >= 1.0:
        return 0
    if v <= 0.0:
        return -10000
    return int((1.0 - v) * -1000.0)


def mb_to_gain(mb):
    return 0.0 if mb <= -10000 else 10 ** (mb / 2000.0)


def hw_att_db(total_mb):
    """DSOUND per-bin attenuation register: (att << 6) / 100, clamped to 0xFFF (1/64 dB)."""
    att = -total_mb
    if att < 0:
        att = 0
    reg = min((att << 6) // 100, 0xFFF)
    return reg / 64.0


def gains_2d(cat_vol, voice_vol, pan, channels):
    mb = game_mb(np.float32(cat_vol) * np.float32(voice_vol))
    a = min(1.0, float(np.float32(1.0 + pan)))
    b = min(1.0, 1.0 - float(np.float32(pan)))
    b0 = int(mb * a)
    b1 = int(mb * b)
    head = 600
    left = -hw_att_db(mb - head + b0)
    right = -hw_att_db(mb - head + b1)
    return mb, b0, b1, left, right


def distance_mb(d, dmin, dmax, curve=CURVE, mute_at_max=True):
    if d <= dmin:
        return 0
    if mute_at_max and d >= dmax:
        return -10000
    d = min(d, dmax)
    n = len(curve)
    step = (dmax - dmin) / n
    x = (d - dmin) / step
    idx = min(int(x), n - 1)
    if idx == 0:
        lo, hi, frac = 1.0, curve[0], (d - dmin) / step
    else:
        lo, hi, frac = curve[idx - 1], curve[idx], (d - dmin - idx * step) / step
    g = lo + (hi - lo) * frac
    if g <= 0:
        return -10000
    if g >= 1:
        return 0
    return int(round(2000 * math.log10(g)))


def x3daudio_curve(dmin, dmax, curve=CURVE):
    """Equivalent X3DAUDIO_DISTANCE_CURVE points (CurveDistanceScaler = dmax), amplitude linear."""
    n = len(curve)
    pts = [(0.0, 1.0), (dmin / dmax, 1.0)]
    for i, c in enumerate(curve):
        pts.append(((dmin + (i + 1) * (dmax - dmin) / n) / dmax, c))
    return pts


T("== 2D voices (music, menu, speech, 2D SFX): slider/category value * voice volume -> mB -> per output dB")
T("   v      mB   bin0  bin1   L dB(pan 0)  R dB(pan 0)   L/R dB at pan -1 (full left)")
for v in (1.0, 0.95, 0.9, 0.75, 0.5, 0.25, 0.1, 0.05, 0.01, 0.0):
    mb, b0, b1, l, r = gains_2d(v, 1.0, 0.0, 1)
    _, _, _, ll, rr = gains_2d(v, 1.0, -1.0, 1)
    T("  %5.2f %6d %6d %5d %10.2f %12.2f %10.2f/%6.2f" % (v, mb, b0, b1, l, r, ll, rr))
T("\n== 3D voices: distance attenuation (mB) with the game's curve; defaults min 5 max 80 (0xCA1E5), "
  "observed in Level 1-1: min 30 max 40")
for dmin, dmax in ((5.0, 80.0), (30.0, 40.0)):
    T("  min %g max %g:" % (dmin, dmax))
    for d in [0, dmin, dmin + 0.5 * (dmax - dmin) / 6, dmin + (dmax - dmin) / 6, dmin + 2 * (dmax - dmin) / 6,
              dmin + 3 * (dmax - dmin) / 6, dmin + 4 * (dmax - dmin) / 6, dmin + 5 * (dmax - dmin) / 6,
              dmax - 0.01, dmax, dmax + 1]:
        mb = distance_mb(d, dmin, dmax)
        T("     d=%7.3f  %6d mB  gain %.4f" % (d, mb, mb_to_gain(mb)))
    T("     X3DAudio curve (scaler %g): %s" % (dmax, ", ".join("(%.4f, %.2f)" % p for p in x3daudio_curve(dmin, dmax))))
T("\n== 3D per-bin levels at voice volume v (no distance): direct bins 6..9 = mB (+ front/back split), "
  "reverb send bin 10 = mB - 600 (DSI3DL2BUFFER.lRoom), LFE bin 3 = mB; headroom 0")
for v in (1.0, 0.75, 0.5):
    mb = game_mb(v)
    T("  v=%.2f  mB=%d  direct %.2f dB  reverb send %.2f dB  LFE %.2f dB" % (
        v, mb, -hw_att_db(mb), -hw_att_db(mb - 600), -hw_att_db(mb)))
