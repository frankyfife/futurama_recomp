"""Level statistics of the game's audio material (decoded in memory with PyAV, bit-exact with
m5_adpcm.py): RMS and peak in dBFS, share of full-scale samples (clipping in the source), DC offset,
silence at start/end (relevant for loop seams), per category. Read-only on the ISO; per-file rows go to
run/analyse_m5/levels.tsv, the summary to scripts/out/m5_levels.out.

Categories: music, ambient, localized/<lang> (german only by default), menu, footsteps, loose level SFX
(sample), soundcache NIF SFX (sample), ingame dialogue NIF (german).
"""
import collections
import io
import math
import random
import struct
import sys

import av
import numpy as np

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS  # noqa: E402
import futimg  # noqa: E402
from m5_common import Tee, out_path, run_path  # noqa: E402

ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
T = Tee(out_path("m5_levels.out"))
random.seed(5)
v = XDVDFS(ISO)


def decode_wav(blob):
    c = av.open(io.BytesIO(blob), format="wav")
    st = c.streams.audio[0]
    frames = [f.to_ndarray() for f in c.decode(st)]
    c.close()
    if not frames:
        return None, 0
    a = np.concatenate(frames, axis=1 if frames[0].ndim == 2 else 0)
    if a.ndim == 1:
        a = a[None, :]
    if a.dtype == np.uint8:
        a = (a.astype(np.float64) - 128) * 256
    return a.astype(np.float64), st.codec_context.sample_rate


def stats(a, rate):
    x = a / 32768.0
    ms = float((x ** 2).mean())
    rms = 10 * math.log10(ms) if ms > 0 else -200
    peak = float(np.abs(x).max())
    pk = 20 * math.log10(peak) if peak > 0 else -200
    clip = float((np.abs(a) >= 32767).mean())
    dc = float(x.mean())
    env = np.abs(x).max(axis=0)
    thr = 10 ** (-60 / 20)
    nz = np.nonzero(env > thr)[0]
    lead = (nz[0] / rate * 1000) if len(nz) else 0
    tail = ((len(env) - 1 - nz[-1]) / rate * 1000) if len(nz) else 0
    # loop seam: jump between the last and first sample (per channel, in LSB)
    seam = float(np.abs(a[:, -1] - a[:, 0]).max())
    return rms, pk, clip, dc, lead, tail, seam


rows = []
per_cat = collections.defaultdict(list)
loose = []
nifs = []
for ip, s, sz, d in sorted(v.walk()):
    if d or not ip.lower().endswith(".img"):
        continue
    hs = struct.unpack("<I", v.read(s, 4))[0]
    for path, fsz, foff in futimg.entries(v.read(s, hs)):
        if fsz is None:
            continue
        lp = path.lower()
        if lp.endswith(".wav"):
            loose.append((s, path, fsz, foff))
        elif lp.endswith(".nif") and (lp.startswith("soundcache/") or "ingame_dlg_german" in lp):
            nifs.append((s, path, fsz, foff))


def cat_of(lp):
    if lp.startswith("sounds/music/"):
        return "music"
    if "/ambient/" in lp:
        return "ambient"
    if lp.startswith("sounds/localized/"):
        return "speech/" + lp.split("/")[2]
    if lp.startswith("sounds/menu/"):
        return "menu"
    if lp.startswith("sounds/footsteps/"):
        return "footsteps"
    return "sfx(loose)"


sel = []
sfx_loose = [x for x in loose if cat_of(x[1].lower()) == "sfx(loose)"]
for x in loose:
    c = cat_of(x[1].lower())
    if c in ("music", "ambient", "menu", "footsteps", "speech/german"):
        sel.append((c, x))
for x in random.sample(sfx_loose, min(250, len(sfx_loose))):
    sel.append(("sfx(loose)", x))
for c, (s, path, fsz, foff) in sel:
    blob = v.read(s, fsz, foff)
    a, rate = decode_wav(blob)
    if a is None:
        continue
    st = stats(a, rate)
    rows.append((c, path) + st)
    per_cat[c].append(st)
# NIF-embedded: soundcache (all levels, sampled) and German ingame dialogue
for s, path, fsz, foff in nifs:
    data = v.read(s, fsz, foff)
    c = "speech/german(nif)" if "ingame_dlg" in path.lower() else "sfx(soundcache)"
    i = data.find(b"RIFF")
    k = 0
    while i >= 0:
        rsize = struct.unpack_from("<I", data, i + 4)[0]
        if data[i + 8:i + 12] == b"WAVE" and (c != "sfx(soundcache)" or random.random() < 0.15):
            a, rate = decode_wav(data[i:i + 8 + rsize])
            if a is not None and a.shape[1] > 16:
                st = stats(a, rate)
                rows.append((c, "%s#%d" % (path, k)) + st)
                per_cat[c].append(st)
        k += 1
        i = data.find(b"RIFF", i + 4)

with open(run_path("levels.tsv"), "w", encoding="utf-8") as f:
    f.write("category\tfile\trms_dbfs\tpeak_dbfs\tclip_share\tdc\tlead_ms\ttail_ms\tseam_lsb\n")
    for r in rows:
        f.write("%s\t%s\t%.2f\t%.2f\t%.6f\t%.5f\t%.1f\t%.1f\t%.0f\n" % r)
T("files analysed: %d (rows in run/analyse_m5/levels.tsv)" % len(rows))
T("%-22s %5s %8s %8s %8s %8s %9s %8s %8s" % ("category", "n", "rms_med", "rms_p10", "rms_p90", "peak_med",
                                            "peak_max", "clip>0", "seam_med"))
for c in sorted(per_cat):
    a = np.array(per_cat[c])
    T("%-22s %5d %8.1f %8.1f %8.1f %8.1f %9.1f %8d %8.0f" % (
        c, len(a), np.median(a[:, 0]), np.percentile(a[:, 0], 10), np.percentile(a[:, 0], 90),
        np.median(a[:, 1]), a[:, 1].max(), int((a[:, 2] > 0).sum()), np.median(a[:, 6])))
T("\nmusic and ambience in detail (rms, peak, clip share, lead/tail silence ms, loop seam jump in LSB):")
for r in rows:
    if r[0] in ("music", "ambient"):
        T("  %-50s rms %6.1f peak %6.1f clip %.5f lead %6.1f tail %6.1f seam %6.0f" % (
            r[1], r[2], r[3], r[4], r[6], r[7], r[8]))
