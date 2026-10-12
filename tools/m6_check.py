"""M6 check: the host movie decoder against FFmpeg (PyAV), without a game run (docs/m6_report.md, src/INTERFACES.md 4.32).

Runs (or reads) the host test mode

    futurama.exe --decode-movies=<disc image> --result=<json> --movie-dump=<dir>

which demuxes and decodes every .xmv of the disc's \\video folder (own XMV demuxer, WMV2 through the Windows WMVideo
Decoder DMO, Xbox ADPCM through the M5 decoder) and lists per file the frame pts, a CRC-32 of every decoded luma plane
and of every audio track's PCM16; --movie-dump writes five raw I420 frames per file and the middle frame through the
display path (I420 -> YUY2 -> BGRA as the renderer shows it).

PyAV (FFmpeg's xmv demuxer, wmv2 and adpcm_ima_xbox decoders) decodes the same files straight from the disc image,
and the script compares:

  V1  the test mode decoded every file without an error (exit code 0, ok for each file, 47 files on the PAL disc)
  V2  frame count and every frame pts equal FFmpeg's (the A/V timing of the HLE is the pts)
  V3  (information) luma planes with a CRC-32 equal to FFmpeg's: the Windows decoder (Microsoft's own WMV8
      implementation) and FFmpeg's wmv2 decoder round differently in places (inverse transform / motion
      compensation), so most frames differ by +-1 in a few pixels and the difference propagates until the next
      keyframe; the first frames and keyframes are often identical
  V4  PSNR of the dumped frames vs FFmpeg: Y >= --min-psnr-y (default 55 dB), U/V >= --min-psnr-uv (default 33 dB)
  V5  display path (YUY2 + BT.601 conversion of the renderer) vs FFmpeg's own yuv420p -> rgb24 conversion: PSNR >=
      --min-psnr-rgb (default 30 dB; FFmpeg upsamples chroma differently)
  A1  every audio track: sample count and PCM16 CRC-32 equal FFmpeg's adpcm_ima_xbox output (bit-exact)
  T1  frame rates: the 25-fps files (Attract, Credits, SCI) have 40 ms gaps, the others 66/67 ms (+ single irregular
      gaps); reported per file

Game runs (optional, --run <dir> per run; a run with [audio] capture_wav + capture_stems and an input script such as
tools/input_scripts/m6_boot.txt / m6_level.txt / m6_attract.txt): every movie of the log ("XMV: movie #n ..."
lines of video/xmv_hle.cpp) is identified on the disc by its header (duration, track count, first/next packet size)
and its part of the capture's movie stem is compared with each audio track of the file (FFmpeg decode, resampled to
the capture rate; normalized cross-correlation of 2-s windows near the start and near the end):

  G1  the track heard is the one the HLE enabled ("track t" of the playing line; for 5-track files the language:
      english 0, german 1, french 2, spanish 3, italian 4) and it correlates clearly (NCC >= 0.6)
  G2  speed: the audio offset (capture position - movie time) near the end equals the one near the start within
      15 ms, i.e. the movie audio plays at the right speed (the picture follows the audio clock, "max N ms late")
  G3  movies that ended by themselves report a length within 100 ms of the file's audio / header duration (the
      25-fps files included); skipped movies are listed as such

Usage:
  python tools/m6_check.py [--iso <image>] [--exe build\\m6\\bin\\futurama.exe] [--out run\\m6] [--reuse]
                           [--filter <text>] [--run run\\m6\\boot ...] [--runs-only]
--reuse skips the test-mode run and reads <out>\\decode_all.json + <out>\\dump.
Writes <out>\\m6_check.txt (the report) and prints it. Exit 0 = all criteria met.
"""
import argparse
import io
import json
import os
import subprocess
import sys
import time
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xdvdfs import XDVDFS, SECTOR  # noqa: E402

import av  # noqa: E402

DEFAULT_ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAL25 = {"attract-pal.xmv", "credits-pal.xmv", "sci-pal.xmv"}


def disc_movies(iso_path):
    v = XDVDFS(iso_path)
    out = {}
    for path, start, size, is_dir in v.walk():
        if not is_dir and path.lower().startswith("/video/") and path.lower().endswith(".xmv"):
            out[os.path.basename(path).lower()] = (os.path.basename(path), start, size)
    return v, out


def read_file(v, start, size):
    v.f.seek(v.base + start * SECTOR)
    return v.f.read(size)


def psnr(a, b):
    a = a.astype(np.float64)
    b = b.astype(np.float64)
    mse = np.mean((a - b) ** 2)
    return float("inf") if mse == 0 else 10.0 * np.log10(255.0 * 255.0 / mse)


def ffmpeg_decode(data, want_frames):
    """Decodes with PyAV: pts, luma CRCs, I420 planes of `want_frames`, rgb24 of the same, audio per track."""
    c = av.open(io.BytesIO(data), format="xmv")
    vstream = c.streams.video[0]
    astreams = list(c.streams.audio)
    pts, crcs, planes, rgbs = [], [], {}, {}
    audio = {s.index: [0, 0] for s in astreams}  # crc, samples
    index = 0
    for packet in c.demux():
        for frame in packet.decode():
            if packet.stream.type == "video":
                a = frame.to_ndarray(format="yuv420p")
                h, w = frame.height, frame.width
                y = np.ascontiguousarray(a[:h])
                pts.append(frame.pts)
                crcs.append(zlib.crc32(y.tobytes()) & 0xFFFFFFFF)
                if index in want_frames:
                    rest = a[h:].reshape(-1)
                    u = rest[: (w // 2) * (h // 2)].reshape(h // 2, w // 2)
                    vv = rest[(w // 2) * (h // 2):].reshape(h // 2, w // 2)
                    planes[index] = (y, u, vv)
                    rgbs[index] = frame.to_ndarray(format="rgb24")
                index += 1
            else:
                arr = frame.to_ndarray()  # s16p: (channels, samples)
                inter = np.ascontiguousarray(arr.T).astype("<i2").tobytes()
                st = audio[packet.stream.index]
                st[0] = zlib.crc32(inter, st[0]) & 0xFFFFFFFF
                st[1] += arr.shape[1]
    tracks = [audio[s.index] for s in astreams]
    c.close()
    return pts, crcs, planes, rgbs, tracks


def disc_headers(v, movies):
    """(duration, tracks, first, next) -> name of each movie on the disc."""
    out = {}
    for key, (name, start, size) in movies.items():
        head = read_file(v, start, 0x40)
        nxt, first, _mx, _magic, _ver, _w, _h, dur = (int(x) for x in np.frombuffer(head[:32], "<u4"))
        tracks = int(np.frombuffer(head[0x20:0x22], "<u2")[0])
        k = (dur, tracks, first, nxt)
        out[k] = out[k] + "|" + name if k in out else name  # e.g. Intro-PAL and 1-1-00-PAL are the same movie
    return out


def ffmpeg_tracks(data):
    """All audio tracks of a movie as float32 mono downmixes + rate."""
    c = av.open(io.BytesIO(data), format="xmv")
    streams = list(c.streams.audio)
    parts = {s.index: [] for s in streams}
    rate = streams[0].codec_context.sample_rate if streams else 0
    for packet in c.demux(*streams):
        if packet.stream.type != "audio" or packet.stream.index not in parts:
            continue
        for frame in packet.decode():
            a = frame.to_ndarray().astype(np.float32) / 32768.0
            parts[packet.stream.index].append(a.mean(axis=0))
    result = [np.concatenate(parts[s.index]) if parts[s.index] else np.zeros(0, np.float32) for s in streams]
    c.close()  # the stream objects are invalid afterwards
    return result, rate


def best_lag(ref, seg):
    """Position and NCC of the best match of `ref` inside `seg`."""
    n = len(ref)
    if n == 0 or len(seg) < n:
        return 0, 0.0
    size = 1 << int(np.ceil(np.log2(len(seg) + n)))
    corr = np.fft.irfft(np.fft.rfft(seg, size) * np.conj(np.fft.rfft(ref, size)), size)[: len(seg) - n + 1]
    energy = np.sqrt(np.convolve(seg * seg, np.ones(n), "valid") * float(np.dot(ref, ref))) + 1e-6 * n
    ncc = corr / energy
    k = int(np.argmax(ncc))
    return k, float(ncc[k])


def analyse_run(run_dir, v, movies, say):
    """G1-G3 for one game run (log + capture.movie.wav). Returns the failed criteria."""
    import re
    from audio_compare import load_wav, resample
    log_path = os.path.join(run_dir, "logs", "futurama.log")
    lines = open(log_path, encoding="utf-8", errors="replace").read().splitlines()
    t_capture = None
    opened, playing, closed = {}, {}, {}
    for ln in lines:
        m = re.match(r"\[\s*([\d.]+)\]", ln)
        if not m:
            continue
        t = float(m.group(1))
        if "audio capture: started" in ln and t_capture is None:
            t_capture = t
        m2 = re.search(r"XMV: movie #(\d+): \d+x\d+, (\d+) ms, (\d+) audio tracks.*\(first 0x([0-9A-F]+), next "
                       r"0x([0-9A-F]+)\)", ln)
        if m2:
            opened[int(m2.group(1))] = (t, int(m2.group(2)), int(m2.group(3)), int(m2.group(4), 16),
                                        int(m2.group(5), 16))
        m3 = re.search(r"XMV: movie #(\d+) playing: .*\(track (\d+)\)", ln)
        if m3:
            playing[int(m3.group(1))] = (t, int(m3.group(2)))
        m4 = re.search(r"XMV: movie #(\d+) (ended|closed early \(skipped\)|closed before its first frame) at (\d+) "
                       r"ms: (\d+) frames shown of (\d+) demuxed, (\d+) dropped \(late\), max (\d+) ms late", ln)
        if m4:
            closed[int(m4.group(1))] = (t, m4.group(2), int(m4.group(3)), int(m4.group(4)), int(m4.group(5)),
                                        int(m4.group(6)), int(m4.group(7)))
    say(f"run {run_dir}: {len(opened)} movies opened, {len(closed)} closed")
    if not opened:
        say("  no movie found in the log")
        return ["G1"]
    wav = os.path.join(run_dir, "capture.movie.wav")
    if t_capture is None or not os.path.exists(wav):
        say("  no movie stem capture ([audio] capture_wav + capture_stems) - audio criteria skipped")
        return []
    stem, rate = load_wav(wav)
    mono = stem[:2].mean(axis=0) if stem.shape[0] >= 2 else stem[0]
    headers = disc_headers(v, movies)
    lang = {0: "english", 1: "german", 2: "french", 3: "spanish", 4: "italian"}
    fails = []
    for mid in sorted(opened):
        t_open, dur, ntracks, first, nxt = opened[mid]
        name = headers.get((dur, ntracks, first, nxt), "?")
        if mid not in playing:
            say(f"  #{mid} {name}: never played")
            continue
        t_play, track = playing[mid]
        end = closed.get(mid)
        how = end[1] if end else "still open at the end of the run"
        info = f"  #{mid} {name}: track {track}" + (f" ({lang[track]})" if ntracks == 5 else "") + f", {how}"
        if end:
            info += f" at {end[2]} ms, {end[3]}/{end[4]} frames shown, {end[5]} dropped, max {end[6]} ms late"
        say(info)
        key = name.split("|")[0].lower()
        if key not in movies:
            fails.append("G1")
            continue
        _, start, size = movies[key]
        tracks, trate = ffmpeg_tracks(read_file(v, start, size))
        tracks = [resample(t[None, :], trate, rate)[0] for t in tracks]
        played_ms = end[2] if end else 30000
        seg_from = int((t_play - t_capture) * rate)
        seg_to = seg_from + int((played_ms + 1500) * rate / 1000)
        seg = mono[max(seg_from, 0): min(seg_to, len(mono))]
        win = 2 * rate
        # Reference windows: 0.5 s into the movie and the latest 2-s window before the reached movie time that is not
        # silent (the logos end in silence, where any lag fits).
        ref_track = tracks[track] if track < len(tracks) else tracks[0]
        if played_ms < 2600:
            say("     played less than 2.6 s - too short to compare")
            continue
        points = [500]
        p_end = played_ms - 2500
        while p_end > 2500:
            a = int(p_end * rate / 1000)
            if a + win <= len(ref_track) and float(np.sqrt(np.mean(ref_track[a: a + win] ** 2))) > 0.01:
                points.append(p_end)
                break
            p_end -= 500
        # Language window: the first 2 s in which the tracks differ (5-track files start with the same music).
        lang_ms = None
        if len(tracks) > 1:
            for p_ms in range(500, played_ms - 2000, 500):
                a = int(p_ms * rate / 1000)
                def similar(x, y):
                    return float(np.dot(x, y)) / (float(np.sqrt(np.dot(x, x) * np.dot(y, y))) + 1e-9)
                if a + win <= min(len(t) for t in tracks) and min(
                        similar(t[a: a + win], tracks[0][a: a + win]) for t in tracks[1:]) < 0.8:
                    lang_ms = p_ms
                    break

        def match(tr, p_ms):
            a = int(p_ms * rate / 1000)
            lo = max(a - int(0.6 * rate), 0)
            k, c = best_lag(tr[a: a + win], seg[lo: a + win + int(0.6 * rate)])
            return c, (lo + k - a) * 1000.0 / rate

        offsets = [match(ref_track, p_ms)[1] for p_ms in points]
        heard_ncc = min(match(ref_track, p_ms)[0] for p_ms in points)
        nccs = [match(t, lang_ms)[0] for t in tracks] if lang_ms is not None else None
        best = int(np.argmax(nccs)) if nccs else track
        drift = offsets[-1] - offsets[0] if len(points) > 1 else 0.0
        text = f"     NCC of track {track} {heard_ncc:.2f}; audio offset {offsets[0]:+.1f} ms at 0.5 s"
        if len(points) > 1:
            text += f", {offsets[-1]:+.1f} ms at {points[-1] / 1000:.1f} s (drift {drift:+.1f} ms)"
        if nccs:
            text += (f"; language window at {lang_ms / 1000:.1f} s: NCC per track "
                     f"{', '.join(f'{c:.2f}' for c in nccs)} -> heard track {best}")
        elif len(tracks) > 1:
            text += "; the tracks do not differ in the part played (language not distinguishable)"
        say(text)
        if best != track or heard_ncc < 0.6:
            fails.append("G1")
        if abs(drift) > 15.0:
            fails.append("G2")
        if end and end[1] == "ended":
            audio_ms = len(tracks[track]) * 1000.0 / rate
            if end[2] + 100 < min(audio_ms, dur) or end[2] > max(audio_ms, dur) + 100:
                fails.append("G3")
    return fails


def report_runs(args, v, movies, say):
    fails = []
    for r in args.run:
        fails += analyse_run(r, v, movies, say)
    say(f"G1 heard track = enabled track (language): {'PASS' if 'G1' not in fails else 'FAIL'}")
    say(f"G2 audio speed (drift <= 15 ms): {'PASS' if 'G2' not in fails else 'FAIL'}")
    say(f"G3 movie lengths: {'PASS' if 'G3' not in fails else 'FAIL'}")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=DEFAULT_ISO)
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "m6", "bin", "futurama.exe"))
    ap.add_argument("--out", default=os.path.join(ROOT, "run", "m6"))
    ap.add_argument("--reuse", action="store_true")
    ap.add_argument("--filter", default="")
    ap.add_argument("--min-psnr-y", type=float, default=55.0)
    ap.add_argument("--min-psnr-uv", type=float, default=33.0)
    ap.add_argument("--min-psnr-rgb", type=float, default=30.0)
    ap.add_argument("--run", action="append", default=[], help="game run folder (log + capture) for G1-G3")
    ap.add_argument("--runs-only", action="store_true", help="only the game-run criteria (no test mode)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    result_path = os.path.join(args.out, "decode_all.json")
    dump_dir = os.path.join(args.out, "dump")
    lines = []

    def say(text=""):
        print(text)
        lines.append(text)

    exit_code = 0
    if args.runs_only:
        v, movies = disc_movies(args.iso)
        fails = report_runs(args, v, movies, say)
        say("ALL PASS" if not fails else "FAILED: " + ", ".join(sorted(set(fails))))
        with open(os.path.join(args.out, "m6_check_runs.txt"), "w", encoding="utf-8") as f:
            f.write(chr(10).join(lines) + chr(10))
        return 0 if not fails else 1
    if not args.reuse:
        cmd = [args.exe, f"--decode-movies={args.iso}", f"--result={result_path}", f"--movie-dump={dump_dir}"]
        if args.filter:
            cmd.append(f"--movie-filter={args.filter}")
        t0 = time.time()
        p = subprocess.run(cmd, capture_output=True, text=True)
        exit_code = p.returncode
        say(f"test mode: exit {p.returncode} in {time.time() - t0:.1f} s ({os.path.basename(args.exe)})")
    with open(result_path, encoding="utf-8") as f:
        result = json.load(f)

    v, movies = disc_movies(args.iso)
    files = result["files"]
    fails = []
    say(f"decoder: {result['decoder']}")
    say(f"files: {len(files)} decoded by the test mode, {result['files_ok']} ok, {result['files_failed']} failed; "
        f"{result['frames_decoded']} frames; disc has {len(movies)} .xmv")
    if exit_code != 0 or result["files_failed"] != 0 or (not args.filter and len(files) != len(movies)):
        fails.append("V1")

    total_frames = exact_frames = 0
    psnr_y, psnr_u, psnr_v, psnr_rgb = [], [], [], []
    audio_tracks = audio_exact = 0
    say()
    say(f"{'file':<26} {'frames':>6} {'pts':>4} {'luma=':>7} {'Y dB':>6} {'UV dB':>6} {'RGB dB':>6} {'audio':>7} "
        f"{'gaps ms':>12} {'ms/frm':>6}")
    for fr in files:
        name = fr["name"]
        key = name.split("|")[0].lower()
        if key not in movies:
            fails.append("V1")
            say(f"{name}: not on the disc")
            continue
        _, start, size = movies[key]
        data = read_file(v, start, size)
        dumped = {}
        ddir = os.path.join(dump_dir, name)
        if os.path.isdir(ddir):
            for fn in os.listdir(ddir):
                if fn.startswith("frame_") and fn.endswith(".i420"):
                    dumped[int(fn[6:11])] = os.path.join(ddir, fn)
        display = {}
        if os.path.isdir(ddir):
            for fn in os.listdir(ddir):
                if fn.startswith("display_") and fn.endswith(".bgra"):
                    display[int(fn[8:13])] = os.path.join(ddir, fn)
        pts, crcs, planes, rgbs, tracks = ffmpeg_decode(data, set(dumped) | set(display))
        w, h = fr["width"], fr["height"]
        pts_ok = pts == fr["pts"]
        n = min(len(crcs), len(fr["luma_crc32"]))
        exact = sum(1 for i in range(n) if crcs[i] == fr["luma_crc32"][i])
        total_frames += len(crcs)
        exact_frames += exact
        if not pts_ok or len(crcs) != fr["frames_decoded"]:
            fails.append("V2")
        fy, fuv = [], []
        for idx, path in dumped.items():
            raw = np.fromfile(path, dtype=np.uint8)
            y = raw[: w * h].reshape(h, w)
            u = raw[w * h: w * h + (w // 2) * (h // 2)].reshape(h // 2, w // 2)
            vv = raw[w * h + (w // 2) * (h // 2):].reshape(h // 2, w // 2)
            ry, ru, rv = planes[idx]
            fy.append(psnr(y, ry))
            fuv.append(min(psnr(u, ru), psnr(vv, rv)))
        frgb = []
        for idx, path in display.items():
            raw = np.fromfile(path, dtype=np.uint8).reshape(h, w, 4)
            rgb = raw[:, :, [2, 1, 0]]
            frgb.append(psnr(rgb, rgbs[idx]))
        psnr_y += fy
        psnr_u += fuv
        psnr_rgb += frgb
        a_ok = 0
        for i, t in enumerate(fr["tracks"]):
            audio_tracks += 1
            if i < len(tracks) and tracks[i][1] == t["frames"] and tracks[i][0] == t["pcm_crc32"]:
                a_ok += 1
                audio_exact += 1
        if a_ok != len(fr["tracks"]):
            fails.append("A1")
        gaps = f"{fr['min_gap_ms']}..{fr['max_gap_ms']}"
        if key in PAL25 and not (fr["min_gap_ms"] == 40):
            fails.append("T1")
        if key not in PAL25 and not (fr["min_gap_ms"] in (40, 66, 67)):
            fails.append("T1")

        def fmt(values):
            if not values:
                return "-"
            m = min(values)
            return "inf" if m == float("inf") else f"{m:.1f}"

        say(f"{name:<26} {fr['frames_decoded']:>6} {'ok' if pts_ok else 'DIFF':>4} {exact / max(n, 1):>7.4f} "
            f"{fmt(fy):>6} {fmt(fuv):>6} {fmt(frgb):>6} {a_ok}/{len(fr['tracks']):<5} {gaps:>12} "
            f"{fr['decode_ms_per_frame']:>6.2f}")

    share = exact_frames / max(total_frames, 1)
    say()
    say(f"V1 files decoded without error: {result['files_ok']}/{len(files)} (disc {len(movies)})"
        f" -> {'PASS' if 'V1' not in fails else 'FAIL'}")
    say(f"V2 frame counts and pts equal FFmpeg: {'PASS' if 'V2' not in fails else 'FAIL'}")
    say(f"V3 (information) luma bit-exact with FFmpeg: {exact_frames}/{total_frames} frames = {share:.2%}")

    def minimum(values):
        finite = [x for x in values if x != float("inf")]
        return (min(finite) if finite else float("inf")), sum(1 for x in values if x == float("inf")), len(values)

    my, iy, ny = minimum(psnr_y)
    mu, iu, nu = minimum(psnr_u)
    mr, ir, nr = minimum(psnr_rgb)
    ok4 = my >= args.min_psnr_y and mu >= args.min_psnr_uv

    def mean(values):
        finite = [x for x in values if x != float("inf")]
        return float(np.mean(finite)) if finite else float("inf")

    say(f"V4 PSNR of {ny} dumped frames: Y min {my:.2f} / mean {mean(psnr_y):.2f} dB ({iy} identical), U/V min "
        f"{mu:.2f} / mean {mean(psnr_u):.2f} dB ({iu} identical) -> {'PASS' if ok4 else 'FAIL'}")
    if not ok4:
        fails.append("V4")
    ok5 = mr >= args.min_psnr_rgb
    say(f"V5 display path (YUY2 + BT.601) vs FFmpeg rgb24: {nr} frames, PSNR min {mr:.2f} dB, mean "
        f"{np.mean([x for x in psnr_rgb if x != float('inf')] or [0]):.2f} dB -> {'PASS' if ok5 else 'FAIL'}")
    if not ok5:
        fails.append("V5")
    say(f"A1 audio tracks bit-exact (samples + PCM CRC): {audio_exact}/{audio_tracks} -> "
        f"{'PASS' if 'A1' not in fails else 'FAIL'}")
    say(f"T1 frame gaps: 25-fps files {sorted(PAL25)} at 40 ms, the others 66/67 ms -> "
        f"{'PASS' if 'T1' not in fails else 'FAIL'}")
    if args.run:
        say()
        fails += report_runs(args, v, movies, say)
    say()
    say("ALL PASS" if not fails else "FAILED: " + ", ".join(sorted(set(fails))))
    with open(os.path.join(args.out, "m6_check.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
