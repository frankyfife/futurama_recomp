"""M5 check: game audio verified without ears (ROADMAP "M5 - Audio", analysis/loader/m5_audio.md 9, src/INTERFACES.md
4.30).

Runs futurama.exe with [audio] capture_wav (+ capture_stems, event_log) and an input script, then analyses the
captured mix, the category stems and the event list against the original sound files of the disc (decoded
independently in memory by tools/audio_compare.py: PyAV/ffmpeg adpcm_ima_xbox / PCM). Two runs by default:

  frontend  tools/input_scripts/m5_frontend.txt: main menu ~130 s, menu navigation, options in/out; the front-end
            music must stream continuously and loop once (R1, R2 of m5_audio.md 9.2)
  level     tools/input_scripts/m4_newgame.txt (the M4 script): "Neues Spiel" -> Level 1-1, walk, turn the camera,
            jump, attack, unplug/replug; sfx, voices, ambience, 3D panning (R4, R5); plus all M4 criteria

Criteria per run (A = audio):
  A1 audio engine up: "audio: XAudio2 2.9, ..." (or, with --device-optional, the software mixer when Windows has no
     active output device - e.g. the TV is off; the run then says so), "audio capture: started", capture summary
  A2 DSOUND objects plausible ("audio dsound calls:", "audio summary:"): buffers created and played, streams with
     packets, GetStatus polled several ticks per buffer (voices live longer than one tick), at most --max-live voices
     alive at the exit, peak below 256
  A3 no stuck voices (event list): every non-looping buffer that played a loose sound file of known length got an
     "end"/"stop"/"release" event within its length / pitch + 1.5 s (or the run ended before)
  A4 capture non-silent, category levels plausible ("audio levels <stem>:" lines): mix active rms -45..-6 dBFS with
     sound in >= 50 % of the 100 ms blocks after the first sound; frontend: music stem active rms -40..-8 dBFS;
     level: effects stem with sound
  A5 original sounds matched (pitch ratio 1.00 +- --ratio-tolerance, NCC >= --min-ncc): frontend: the front-end
     music followed chunk by chunk (mean NCC >= 0.9, speed 1 +- 0.001) and >= 1 menu sound; level: >= --min-matches
     sounds of the event list's "play" events found near their event time
  A6 no clipping: |x| >= 1 in at most --max-clip-share of the mix samples (host statistics and the WAV itself)
  A7 music stream continuity: no jump of the music alignment (dropout/underrun) other than the game's own loop gap
     (expected gap from the file header, +-2 ms, m5_audio.md 4.3), "audio summary: ... glitches 0"
  A8 no guest hang or crash: exit code 0 through the script's quit, no FATAL/ERROR line, no watchdog
  A9 (level) 3D panning: matched 3D sounds (event detail "3d") or the effects stem show left/right balances that
     differ by >= 3 dB while the camera turns, with sound louder on each side (events: one >= +3 dB and one <= -3 dB;
     stem: >= 5 % of the loud 100 ms windows on each side)
  A10 channel placement on outputs with more than 2 channels (5.1): the 2D stems (music, speech, movie) carry at most
     1 % of their energy outside front left/right (the game's 2D mixbins are 0 and 1, m5_audio.md 6.4) - e.g. no
     stereo channel in the LFE; stereo output: not applicable
  A11 (frontend, first start: the run began with an empty hdd folder, so no Options.dat and every game category is
     1.0, m5_audio.md 6.7) level model of 3.6: the front-end music (median over its 2 s chunks, each resampled with
     the measured speed and aligned to a fraction of a sample, fitted below 2 kHz so the resampler of the mixer does
     not count; the full-band level is printed too) plays at -6.0 +- 0.1 dB and the matched menu sounds (full band)
     at -6.0 +- 0.3 dB on front left and right (600 mB DSOUND headroom of 2D voices, centre pan)
  A12 limiter consistent with the mix: the capture is the mix BEFORE the limiter and the master volume; when its peak
     times [audio] master_volume exceeds -0.1 dBFS the engine summary must report limited frames and a deepest
     reduction of (peak + master volume dB + 0.1) +- 0.15 dB (instant attack), otherwise none ([audio] limiter = 0:
     not applicable). Runs with a low master volume (the check INIs use 2 %) only show "not needed"; a level run with
     --set audio.master_volume=100 (INI) exercises it.
  M4 the M4 criteria on the level run (M4-1..M4-6, M4-8; M4-7 rumble and M4-9 broken scripts as extra runs unless
     --no-rumble / --no-broken) - tools/m4_check.py

Usage (paths with blanks belong in the INI):
    python tools/m5_check.py --exe build/m5/bin/futurama.exe --config run/m5/check/futurama.ini
                             [--runs frontend,level] [--device-optional] [--fresh] [--keep-wav]

The INI must set [paths] userdata and [paths] iso and must not set [debug] stop_at. Captures, logs and results go to
<userdata>/m5_check/ (git-ignored run/ folder): <run>.wav, <run>.<stem>.wav, <run>.events.tsv, <run>.log,
<run>_result.json. Exit code 0 when all criteria pass.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_compare as ac  # noqa: E402
import m2_check  # noqa: E402
import m4_check  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "tools" / "input_scripts"
FRONTEND_MUSIC = ["sounds/music/01 - front-end.wav", "sounds/music/01 - front-end-shorted.wav"]
LEVEL_MUSIC = ["sounds/music/01 - planet express.wav"]
MENU_SOUNDS = ["sounds/menu/scroll.wav", "sounds/menu/cursor.wav", "sounds/menu/select.wav",
               "sounds/menu/go_to.wav", "sounds/menu/go_back.wav", "sounds/menu/scrolllimit.wav",
               "sounds/menu/toggleok.wav", "sounds/button.wav"]
STEMS = ("music", "speech", "effects", "movie", "reverb")

CRITERIA = {
    "A1": "A1 audio engine and capture up",
    "A2": "A2 DSOUND objects plausible (created/played/polled/released)",
    "A3": "A3 no stuck voices (every finite buffer ends)",
    "A4": "A4 capture non-silent, category levels plausible",
    "A5": "A5 original sounds matched (pitch, correlation)",
    "A6": "A6 no clipping",
    "A7": "A7 music stream continuity (no dropouts besides the loop gap, no glitches)",
    "A8": "A8 no guest hang or crash",
    "A9": "A9 3D panning while the camera turns",
    "A10": "A10 2D sounds (music, speech, movie) only on the front left/right speakers",
    "A11": "A11 DSOUND level model: 2D sounds at -6 dB on the first start (m5_audio.md 3.6)",
    "A12": "A12 limiter consistent with the mix peak and the master volume",
}

AUDIO_ENGINE = re.compile(r"\baudio: (XAudio2 2\.9, .*|software mixer .*)$")
CAPTURE_STARTED = re.compile(r"audio capture: started (.*) \((.*), source (\S+)\)")
CAPTURE_SUMMARY = re.compile(r"audio capture: (.*) - (\d+) Hz, (\d+) ch, (\S+), source (\S+), ([0-9.]+) s \((\d+) frames\), "
                             r"(\d+) dropped, (\d+) mismatched, (\d+) truncated, files: (.*), (\d+) events")
LEVELS = re.compile(r"audio levels (\w+): (\d+) ch, ([0-9.]+) s, rms (-?[0-9.]+) dBFS \(ch ([^)]*)\), active rms "
                    r"(-?[0-9.]+) dBFS over ([0-9.]+) % of 100 ms blocks, peak (-?[0-9.]+) dBFS at ([0-9.]+) s, clipped "
                    r"(\d+) \(([0-9.]+) %\), gaps >= \d+ ms: (\d+) \(longest ([0-9.]+) ms at ([0-9.]+) s; ([^)]*)\), "
                    r"first sound at (-?[0-9.]+) s")
DSOUND_CALLS = re.compile(r"audio dsound calls: (.*)$")
LIMITER_STATS = re.compile(r"limiter active on (\d+) frames \(max (-?[0-9.]+) dB\)")
ENGINE_SUMMARY = re.compile(r"audio summary: (.*); voices (\d+) created, (\d+) live, peak (\d+); ([0-9.]+) s mixed; "
                            r"glitches (\d+); device losses (\d+)")


def parse_calls(text: str) -> dict:
    out = {}
    for part in text.split(","):
        m = re.match(r"\s*(\S+) (\d+)\s*$", part)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


def parse_log(log: str) -> dict:
    info = {"engine": None, "started": None, "summary": None, "levels": {}, "calls": {}, "engine_summary": None,
            "errors": [], "warnings": []}
    for ln in log.splitlines():
        if (m := AUDIO_ENGINE.search(ln)) and info["engine"] is None:
            info["engine"] = m.group(1)
        if m := CAPTURE_STARTED.search(ln):
            info["started"] = m
        if m := CAPTURE_SUMMARY.search(ln):
            info["summary"] = m
        if m := LEVELS.search(ln):
            info["levels"][m.group(1)] = {
                "seconds": float(m.group(3)), "rms": float(m.group(4)), "active_rms": float(m.group(6)),
                "active_pct": float(m.group(7)), "peak": float(m.group(8)), "clipped": int(m.group(10)),
                "clipped_pct": float(m.group(11)), "gaps": int(m.group(12)), "longest_gap_ms": float(m.group(13)),
                "gap_list": m.group(15), "first_sound": float(m.group(16)),
                "channel_rms": [float(v) for v in m.group(5).split(" / ") if v.strip()]}
        if m := DSOUND_CALLS.search(ln):
            info["calls"] = parse_calls(m.group(1))
        if m := ENGINE_SUMMARY.search(ln):
            info["engine_summary"] = m
        if re.search(r" (ERROR|FATAL) |TRAP |UNIMPLEMENTED", ln):
            info["errors"].append(ln)
        if " WARN " in ln and (" SND " in ln or "audio" in ln):
            info["warnings"].append(ln)
    return info


def load_captures(base: Path) -> dict:
    caps = {}
    if base.exists():
        caps["mix"] = ac.load_wav(base)
    for stem in STEMS:
        p = base.with_name(f"{base.stem}.{stem}.wav")
        if p.exists():
            caps[stem] = ac.load_wav(p)
    return caps


def balance_spread(x: np.ndarray, rate: int, window_s: float = 0.1,
                   min_db: float = -50.0) -> tuple[float, int, float, float]:
    """Range (dB, 5th..95th percentile) of the left/right level difference over windows with sound (stereo or more
    channels), the number of those windows and the shares of them louder on the left / on the right by >= 3 dB."""
    if x.shape[0] < 2:
        return 0.0, 0, 0.0, 0.0
    w = max(1, int(window_s * rate))
    n = x.shape[1] // w
    if n == 0:
        return 0.0, 0, 0.0, 0.0
    left, right = ac.side_channels(x.shape[0])
    e = (x[:, : n * w].astype(np.float64) ** 2).reshape(x.shape[0], n, w).mean(axis=2)
    el, er = e[left].sum(axis=0), e[right].sum(axis=0)
    loud = 10 * np.log10(el + er + 1e-30) > min_db
    if loud.sum() < 3:
        return 0.0, int(loud.sum()), 0.0, 0.0
    bal = np.clip(10 * np.log10((el[loud] + 1e-30) / (er[loud] + 1e-30)), -60.0, 60.0)
    lo, hi = np.percentile(bal, [5, 95])
    return float(hi - lo), int(loud.sum()), float(np.mean(bal >= 3.0)), float(np.mean(bal <= -3.0))


# ---------------------------------------------------------------------------------------------------------------


def check_a1(info: dict, device_optional: bool) -> tuple[bool, str]:
    eng = info["engine"]
    summ = info["summary"]
    headless = eng is not None and eng.startswith("software mixer")
    ok = (eng is not None and info["started"] is not None and summ is not None
          and (not headless or device_optional))
    src = summ.group(5) if summ else "-"
    detail = (f"engine: {eng or 'no audio engine line'}; capture "
              + (f"{summ.group(2)} Hz {summ.group(3)} ch {summ.group(4)}, source {src}, {float(summ.group(6)):.1f} s, "
                 f"{summ.group(8)} dropped, files {summ.group(11)}, {summ.group(12)} events" if summ else
                 "summary missing (no 'audio capture:' exit line)"))
    if headless:
        detail += ("; NO AUDIO DEVICE (software mix, accepted with --device-optional)" if device_optional
                   else "; no audio device - pass --device-optional when Windows has no active output (TV off)")
    return ok, detail


def check_a2(info: dict, run: str, max_live: int) -> tuple[bool, str]:
    c = info["calls"]
    created = c.get("DirectSoundCreateBuffer", 0)
    plays = c.get("IDirectSoundBuffer_Play", 0)
    status = c.get("IDirectSoundBuffer_GetStatus", 0)
    released = c.get("IDirectSoundBuffer_Release", 0)
    streams = c.get("DirectSoundCreateStream", 0)
    es = info["engine_summary"]
    live = int(es.group(3)) if es else None
    peak = int(es.group(4)) if es else None
    min_created = 2 if run == "frontend" else 10
    ratio = status / created if created else 0.0
    ok = (created >= min_created and plays >= created // 2 and streams >= 1 and ratio >= 3.0
          and released <= created and es is not None and live is not None and live <= max_live
          and peak is not None and peak < 256)
    return ok, (f"buffers created {created} (>= {min_created}), played {plays}, released {released}, GetStatus "
                f"{status} ({ratio:.1f} per buffer, >= 3), streams {streams}; engine voices "
                + (f"{es.group(2)} created, {live} live at exit (<= {max_live}), peak {peak}" if es else
                   "summary missing"))


def check_a3(events: list[dict], refs: ac.ReferenceSource, end_s: float) -> tuple[bool, str, list]:
    """Non-looping buffers with a loose sound file must end within their length / pitch (+ 1.5 s)."""
    plays = {}
    freq = {}
    stuck = []
    checked = 0
    lengths: dict[str, float | None] = {}
    for ev in events:
        obj = ev.get("object")
        kind = ev.get("kind")
        if kind == "frequency":
            m = re.search(r"hz=(\d+)", ev.get("detail", ""))
            if m:
                freq[obj] = int(m.group(1))
        elif kind == "play" and "buffer" in ev.get("detail", "") and "loop" not in ev.get("detail", "").split():
            plays[obj] = ev
        elif kind in ("end", "stop", "release", "flush") and obj in plays:
            plays.pop(obj)
    for obj, ev in plays.items():
        name = ev.get("name") or ""
        if not name:
            continue
        if name not in lengths:
            try:
                key = refs.find(name)
                lengths[name] = ac.wav_header(refs.raw(key)[:8192])["seconds"] if key else None
            except Exception:  # noqa: BLE001
                lengths[name] = None
        length = lengths[name]
        if length is None:
            continue
        checked += 1
        m = re.search(r"hz=(\d+)", ev.get("detail", ""))
        rate = int(m.group(1)) if m else 0
        ratio = (freq.get(obj, rate) / rate) if rate and freq.get(obj, 0) else 1.0
        due = ev["wall_s"] + length / max(ratio, 0.05) + 1.5
        if due < end_s:
            stuck.append((name, ev["wall_s"], length))
    return not stuck, f"{len(plays)} finite buffers without an end event at the exit, {checked} with a known length, " \
        f"{len(stuck)} overdue" + (f" (first: {stuck[0][0]} played at {stuck[0][1]:.1f} s, {stuck[0][2]:.2f} s long)"
                                   if stuck else ""), stuck


def check_a4(info: dict, run: str) -> tuple[bool, str]:
    lv = info["levels"]
    mix = lv.get("mix")
    if mix is None:
        return False, "no 'audio levels mix' line"
    span = mix["seconds"] - max(0.0, mix["first_sound"])
    active = mix["active_pct"] * mix["seconds"] / max(span, 1e-6) if mix["first_sound"] >= 0 else 0.0
    ok = mix["first_sound"] >= 0 and -45.0 <= mix["active_rms"] <= -6.0 and active >= 50.0
    parts = [f"mix active rms {mix['active_rms']:.1f} dBFS, sound in {min(active, 100.0):.0f} % after the first sound "
             f"at {mix['first_sound']:.1f} s"]
    if run == "frontend":
        music = lv.get("music")
        okm = music is not None and -40.0 <= music["active_rms"] <= -8.0
        ok &= okm
        parts.append(f"music stem active rms {music['active_rms']:.1f} dBFS" if music else "no music stem")
    else:
        fx = lv.get("effects")
        okf = fx is not None and fx["first_sound"] >= 0 and fx["active_rms"] > -60.0
        ok &= okf
        parts.append(f"effects stem active rms {fx['active_rms']:.1f} dBFS" if fx else "no effects stem")
    for stem in STEMS:
        if stem in lv and stem not in ("music", "effects"):
            parts.append(f"{stem} {lv[stem]['active_rms']:.1f} dBFS ({lv[stem]['active_pct']:.0f} %)")
    return ok, "; ".join(parts)


def check_a10(info: dict) -> tuple[bool, str]:
    lv = info["levels"]
    mix = lv.get("mix")
    if mix is None:
        return False, "no 'audio levels mix' line"
    if len(mix["channel_rms"]) <= 2:
        return True, f"{len(mix['channel_rms'])}-channel output: not applicable"
    parts = []
    ok = True
    for stem in ("music", "speech", "movie"):
        st = lv.get(stem)
        if st is None or st["first_sound"] < 0:
            continue
        energy = [10 ** (db / 10.0) if db > -199 else 0.0 for db in st["channel_rms"]]
        total = sum(energy)
        other = sum(energy[2:])
        share = other / total if total > 0 else 0.0
        ok &= share <= 0.01
        parts.append(f"{stem} {share * 100:.1f} % outside FL/FR (ch " + " / ".join(f"{d:.1f}" for d in st["channel_rms"])
                     + " dBFS)")
    return ok, "; ".join(parts) if parts else "no 2D stem with sound"


def check_a6(info: dict, caps: dict, max_share: float) -> tuple[bool, str]:
    mix = info["levels"].get("mix")
    host = mix["clipped_pct"] / 100.0 if mix else None
    file_share = None
    if "mix" in caps:
        x, _ = caps["mix"]
        file_share = float((np.abs(x) >= 1.0).mean()) if x.size else 0.0
    ok = host is not None and host <= max_share and (file_share is None or file_share <= max_share)
    return ok, (f"host {mix['clipped']} samples ({host * 100:.6f} %)" if mix else "host statistics missing") + \
        (f", WAV {file_share * 100:.6f} %" if file_share is not None else "") + \
        (f", peak {mix['peak']:.2f} dBFS" if mix else "") + f" (limit {max_share * 100:.4f} %)"


def music_timeline(caps: dict, refs: ac.ReferenceSource, names: list[str]) -> tuple[ac.TimelineResult | None, str,
                                                                                      float]:
    cap, rate = caps.get("music") or caps["mix"]
    best = None
    for name in names:
        try:
            ref, rr = refs.decode(name)
        except Exception:  # noqa: BLE001
            continue
        tl = ac.timeline(cap, rate, ref, rr, name, chunk_s=2.0, min_ncc=0.8)
        if best is None or tl.matched > best[0].matched:
            gap = ac.stream_loop_gap_ms(ac.wav_header(refs.raw(name)[:8192]))
            best = (tl, name, gap)
    if best is None:
        return None, "", 0.0
    return best


def pause_intervals(events: list[dict]) -> list[tuple[float, float]]:
    """(start, end) capture seconds of every stream pause -> resume and buffer stop -> play of the same object."""
    out = []
    open_at: dict[str, float] = {}
    for e in events:
        obj, kind = e.get("object"), e.get("kind")
        if kind in ("pause", "stop"):
            open_at.setdefault(obj, e["seconds"])
        elif kind in ("resume", "play") and obj in open_at:
            out.append((open_at.pop(obj), e["seconds"]))
        elif kind in ("release", "create"):
            open_at.pop(obj, None)
    return out


def explained(jump: tuple[float, float], intervals: list[tuple[float, float]], chunk_s: float = 2.0) -> bool:
    """A timeline jump at capture time t of j ms is the game's own pause if a pause started within the chunk before t
    and lasted j ms (+- 60 ms: tick quantisation and the mixer block)."""
    t, j = jump
    return any(t - chunk_s - 0.1 <= a <= t + 0.1 and abs(j - (b - a) * 1000.0) <= 60.0 for a, b in intervals)


def check_music(caps: dict, refs: ac.ReferenceSource, names: list[str], info: dict, min_seconds: float,
                events: list[dict]) -> tuple[bool, bool, str, str]:
    """-> (A5 part ok, A7 ok, A5 text, A7 text)."""
    tl, name, gap = music_timeline(caps, refs, names)
    if tl is None or tl.matched == 0:
        return False, False, "music not found in the capture", "music not found", None
    covered = tl.matched * 2.0
    ok5 = tl.mean_ncc >= 0.9 and abs(tl.speed - 1.0) <= 0.001 and covered >= min_seconds
    t5 = (f"{Path(name).name}: from {tl.start_s:.2f} s, {tl.matched}/{tl.chunks} chunks of 2 s (>= {min_seconds:.0f} s), "
          f"mean NCC {tl.mean_ncc:.4f}, speed {tl.speed:.6f} over {tl.speed_span_s:.1f} s, gain "
          + " / ".join(f"{g:.2f}" for g in tl.gain_db) + " dB")
    loops_ok = all(abs(g - gap) <= 2.0 for _, g in tl.loops)
    es = info["engine_summary"]
    glitches = int(es.group(6)) if es else None
    intervals = pause_intervals(events)
    own = [j for j in tl.jumps if explained(j, intervals)]
    other = [j for j in tl.jumps if not explained(j, intervals)]
    # chunks that straddle a game pause, or lie at the end of the capture (a 2 s chunk no longer fits), are no loss
    cap_end = max(x.shape[1] / r for x, r in caps.values())
    lost = [t for t in tl.lost if t < cap_end - 4.0 and not any(a - 2.5 <= t <= b + 2.5 for a, b in intervals)]
    ok7 = not other and not lost and loops_ok and glitches == 0
    t7 = (f"loop points {[(round(t, 2), round(g, 1)) for t, g in tl.loops]} (expected gap {gap:.1f} +- 2 ms), "
          f"dropouts {[(round(t, 2), round(j, 1)) for t, j in other]}, game pauses "
          f"{[(round(t, 2), round(j, 1)) for t, j in own]}, lost chunks {len(lost)} (+{len(tl.lost) - len(lost)} at "
          f"pauses or the capture end), glitches "
          f"{glitches if glitches is not None else 'unknown'}")
    return ok5, ok7, t5, t7, tl


def check_a12(info: dict, log: str, master_volume_pct: float, limiter_on: bool) -> tuple[bool, str]:
    if not limiter_on:
        return True, "[audio] limiter = 0: not applicable"
    mix = info["levels"].get("mix")
    stats = LIMITER_STATS.findall(log)
    if mix is None or not stats:
        return False, "no 'audio levels mix' line or no limiter statistics in the 'audio summary' line"
    frames, reduction = int(stats[-1][0]), float(stats[-1][1])
    volume_db = 20.0 * math.log10(max(master_volume_pct, 1e-3) / 100.0)
    out_peak = mix["peak"] + volume_db
    text = (f"mix peak {mix['peak']:.2f} dBFS before the limiter, master volume {master_volume_pct:g} % "
            f"({volume_db:+.1f} dB) -> {out_peak:.2f} dBFS; limiter active on {frames} frames, max {reduction:.2f} dB")
    if out_peak > -0.1:
        expected = out_peak + 0.1
        ok = frames > 0 and abs(reduction - expected) <= 0.15
        return ok, text + f" (expected {expected:.2f} dB: the output stays at -0.1 dBFS)"
    return frames == 0, text + " (not needed: no reduction expected)"


def check_a11(tl, menu_rows: list[dict], first_start: bool, music_tol: float = 0.1,
              menu_tol: float = 0.3) -> tuple[bool, str]:
    """Music: the timeline's level below 2 kHz (every 2 s chunk resampled with the measured speed and aligned to a
    fraction of a sample, audio_compare.aligned_gains; the band limit keeps the mixer's resampler out of it) within
    +-music_tol of -6.0 dB, the full-band level for information; menu/speech sounds (short, full band, fitted at their
    own offset) within +-menu_tol."""
    if not first_start:
        return True, "not a first start (hdd folder kept): the game's own volume sliders apply - not checked"
    parts = []
    lf = tl.gain_lf_db if tl is not None else []
    ok = len(lf) >= 2 and all(abs(g + 6.0) <= music_tol for g in lf[:2])
    parts.append("music below 2 kHz " + (" / ".join(f"{g:.2f}" for g in lf[:2]) if lf else "-")
                 + f" dB (+- {music_tol:g}), full band "
                 + (" / ".join(f"{g:.2f}" for g in tl.gain_db[:2]) if tl is not None and tl.gain_db else "-") + " dB")
    menu = [r for r in menu_rows if "error" not in r and "gain_db" in r and r.get("ncc", 0) >= 0.9]
    bad = [r for r in menu if any(abs(g + 6.0) > menu_tol for g in r["gain_db"][:2])]
    ok &= bool(menu) and not bad
    span = ""
    if menu:
        gains = [g for r in menu for g in r["gain_db"][:2]]
        span = f" ({min(gains):.2f} .. {max(gains):.2f} dB)"
    parts.append(f"{len(menu)} menu/speech sounds{span}, "
                 + (f"{len(bad)} outside (first: {Path(bad[0]['name']).name} "
                    + " / ".join(f"{g:.2f}" for g in bad[0]["gain_db"][:2]) + " dB)" if bad
                    else f"all within -6 +- {menu_tol:g} dB"))
    return ok, "; ".join(parts) + " (expected -6.0 dB)"


def match_events(events, caps, refs, min_ncc, tol, max_refs=40):
    rows = ac.scan_events(events, caps, refs, max_refs=max_refs, min_ncc=min_ncc)
    good = [r for r in rows if "error" not in r and r["ncc"] >= min_ncc
            and abs(r["pitch_ratio"] - r["expected_ratio"]) <= tol]
    return rows, good


def check_menu_sounds(caps, refs, min_ncc, tol, events):
    rows, good = match_events(events, caps, refs, min_ncc, tol) if events else ([], [])
    if good:
        return True, f"{len(good)} of {len(rows)} event sounds matched", rows
    # without an event list: look for the menu sounds in the effects stem (or the mix)
    cap, rate = caps.get("effects") or caps["mix"]
    found = []
    for name in MENU_SOUNDS:
        try:
            ref, rr = refs.decode(name)
        except Exception:  # noqa: BLE001
            continue
        for m in ac.match(cap, rate, ref, rr, name, min_ncc=min_ncc, top=3):
            if abs(m.pitch_ratio - 1.0) <= tol:
                found.append(m)
    return bool(found), f"{len(found)} menu sound occurrences without event list (" + \
        ", ".join(sorted({Path(m.reference).name for m in found})) + ")", \
        [{"name": m.reference, "offset_s": m.offset_s, "ncc": m.ncc, "pitch_ratio": m.pitch_ratio} for m in found]


def check_a9(rows: list[dict], events: list[dict], caps: dict) -> tuple[bool, str]:
    names3d = {e["name"] for e in events if e.get("kind") in ("create", "play") and " 3d" in f" {e.get('detail', '')}"}
    bal = [r["balance_db"] for r in rows if "error" not in r and r["name"] in names3d]
    spread_events = (max(bal) - min(bal)) if len(bal) >= 2 else 0.0
    both_events = len(bal) >= 2 and max(bal) >= 3.0 and min(bal) <= -3.0
    cap, rate = caps.get("effects") or caps.get("mix", (np.zeros((2, 0)), 48000))
    spread_stem, windows, share_left, share_right = balance_spread(cap, rate)
    both_stem = share_left >= 0.05 and share_right >= 0.05
    # Both sides must occur: a panning stuck on one side (e.g. every 3D voice on front left) shows a large spread
    # against the windows where the other side is silent, but never sounds louder on that other side.
    ok = (spread_events >= 3.0 and both_events) or (spread_stem >= 3.0 and both_stem)
    return ok, (f"{len(names3d)} 3D sound names in the event list, {len(bal)} matched, balance spread "
                f"{spread_events:.1f} dB ({'both sides' if both_events else 'one side only'}); effects stem: "
                f"left/right spread {spread_stem:.1f} dB over {windows} loud 100 ms windows (>= 3 dB), "
                f"{100 * share_left:.0f} % louder left / {100 * share_right:.0f} % louder right (each >= 5 %)")


# ---------------------------------------------------------------------------------------------------------------


def run_game(exe: Path, config: Path, userdata: Path, extra: list[str], timeout: int, clean: bool, out_dir: Path,
             tag: str):
    return m4_check.run_game(exe, config, userdata, extra, timeout, clean, out_dir, tag)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--config", required=True, type=Path)
    ap.add_argument("--runs", default="frontend,level", help="comma list of frontend, level")
    ap.add_argument("--frontend-script", type=Path, default=SCRIPTS / "m5_frontend.txt")
    ap.add_argument("--level-script", type=Path, default=SCRIPTS / "m4_newgame.txt")
    ap.add_argument("--device-optional", action="store_true",
                    help="accept the software mixer when Windows has no active audio output (TV off)")
    ap.add_argument("--fresh", action="store_true", help="empty hdd/cache/logs before every run (always before the first)")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--min-ncc", type=float, default=0.6)
    ap.add_argument("--ratio-tolerance", type=float, default=0.01)
    ap.add_argument("--min-matches", type=int, default=5)
    ap.add_argument("--max-live", type=int, default=64)
    ap.add_argument("--max-clip-share", type=float, default=1e-5)
    ap.add_argument("--no-rumble", action="store_true")
    ap.add_argument("--no-broken", action="store_true")
    ap.add_argument("--keep-wav", action="store_true", help="keep the capture WAVs (default: deleted after a pass)")
    ap.add_argument("--analyse-only", action="store_true",
                    help="re-analyse the files of the last run (needs --keep-wav then), no game start, no M4 criteria")
    args = ap.parse_args()

    config = args.config.resolve()
    cp = m2_check.read_config(config)
    ud = m2_check.ini_value(cp, "paths", "userdata")
    if not ud:
        sys.exit("m5_check: [paths] userdata must be set in the test config")
    userdata = Path(ud) if Path(ud).is_absolute() else config.parent / ud
    if m2_check.ini_value(cp, "debug", "stop_at"):
        sys.exit("m5_check: the test config must not set [debug] stop_at")
    iso = ac.iso_from_config(config)
    if not iso or not Path(iso).exists():
        sys.exit("m5_check: [paths] iso must name the disc image (references are decoded from it)")
    refs = ac.ReferenceSource(iso)
    out_dir = userdata / "m5_check"
    if not args.analyse_only:
        shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    for p in (args.frontend_script, args.level_script):
        if " " in str(p.resolve()):
            sys.exit(f"m5_check: {p} contains blanks - put the script in a path without blanks")

    runs = [r.strip() for r in args.runs.split(",") if r.strip()]
    all_ok = True
    first = True
    for run in runs:
        script = (args.frontend_script if run == "frontend" else args.level_script).resolve()
        wav = out_dir / f"{run}.wav"
        extra = [f"--set=debug.input_script={script}", f"--set=debug.timeout={args.timeout}",
                 f"--set=audio.capture_wav={wav}", "--set=audio.capture_stems=1", "--set=audio.event_log=1"]
        if not int(m2_check.ini_value(cp, "debug", "frames", "0") or "0", 0):
            extra.append("--frames=72000")
        if run == "level":
            trace = [t.strip().lower() for t in m2_check.ini_value(cp, "debug", "trace").replace(";", ",").split(",")
                     if t.strip()]
            if "vfs" not in trace and "all" not in trace:
                extra.append("--trace=" + ",".join(trace + ["vfs"]))
        if args.analyse_only:
            log = (out_dir / f"{run}.log").read_text(encoding="utf-8", errors="replace")
            trace_path = out_dir / f"{run}_vfs_trace.log"
            trace_text = trace_path.read_text(encoding="utf-8", errors="replace") if trace_path.exists() else ""
            code, stderr, elapsed = 0, "", 0.0
            m = m4_check.PROCESS_EXIT.findall(log)
            if m:
                code = int(m[-1][1])
        else:
            print(f"m5_check: run '{run}': {script.name}, capture {wav}")
            code, stderr, elapsed, log, trace_text = run_game(args.exe, config, userdata, extra, args.timeout,
                                                              args.fresh or first, out_dir, run)
        first_start = (args.fresh or first) and not args.analyse_only or (args.analyse_only and run == runs[0])
        first = False
        info = parse_log(log)
        caps = load_captures(wav)
        ev_path = Path(f"{wav.with_suffix('')}.events.tsv")
        events = ac.read_events(ev_path) if ev_path.exists() else []
        end_s = max((e["wall_s"] for e in events), default=0.0)
        results = []
        ok1, d1 = check_a1(info, args.device_optional)
        results.append(("A1", ok1, d1))
        results.append(("A2", *check_a2(info, run, args.max_live)))
        ok3, d3, _ = check_a3(events, refs, end_s) if events else (False, "no event list", [])
        results.append(("A3", ok3, d3))
        results.append(("A4", *check_a4(info, run)))
        detail = {"run": run, "exit": code, "seconds": elapsed, "events": len(events)}
        if not caps:
            results.append(("A5", False, "no capture file"))
            results.append(("A6", False, "no capture file"))
            results.append(("A7", False, "no capture file"))
        else:
            if run == "frontend":
                ok5a, ok7, t5a, t7, tl = check_music(caps, refs, FRONTEND_MUSIC, info, 100.0, events)
                ok5b, t5b, menu_rows = check_menu_sounds(caps, refs, args.min_ncc, args.ratio_tolerance, events)
                results.append(("A5", ok5a and ok5b, f"music {t5a}; menu: {t5b}"))
                detail["menu"] = menu_rows
                results.append(("A11", *check_a11(tl, menu_rows, first_start)))
            else:
                rows, good = match_events(events, caps, refs, args.min_ncc, args.ratio_tolerance)
                ok5a, ok7, t5a, t7, _ = check_music(caps, refs, LEVEL_MUSIC, info, 10.0, events)
                ok5 = len(good) >= args.min_matches
                results.append(("A5", ok5, f"{len(good)} of {len(rows)} event sounds matched (>= {args.min_matches}, "
                                f"pitch +- {args.ratio_tolerance}, NCC >= {args.min_ncc}); "
                                f"level music: {t5a if ok5a else 'not followed (' + t5a + ')'}"))
                detail["event_matches"] = rows
                results.append(("A9", *check_a9(rows, events, caps)))
            results.append(("A6", *check_a6(info, caps, args.max_clip_share)))
            results.append(("A7", ok7, t7))
        results.append(("A10", *check_a10(info)))
        mv_text = m2_check.ini_value(cp, "audio", "master_volume", "100") or "100"
        lim_text = (m2_check.ini_value(cp, "audio", "limiter", "1") or "1").strip().lower()
        try:
            master_volume = float(mv_text)
        except ValueError:
            master_volume = 100.0
        results.append(("A12", *check_a12(info, log, master_volume, lim_text not in ("0", "false", "off", "no"))))
        bad = info["errors"]
        hang = code in (-1, 5)
        quit_ok = any("input script quit" in m[0] for m in m4_check.PROCESS_EXIT.findall(log))
        results.append(("A8", code == 0 and not bad and not hang and quit_ok,
                        f"exit code {code}, {elapsed:.0f} s, script quit {'yes' if quit_ok else 'no'}, "
                        f"{len(bad)} error lines" + (f" (first: {bad[0][:160]})" if bad else "")))
        results.sort(key=lambda r: int(r[0][1:]))
        if run == "level" and args.analyse_only:
            print("    [SKIP] M4 criteria: --analyse-only (the screenshots of the run may be gone)")
        elif run == "level":
            m4_results, m4_info = m4_check.check_run(log, trace_text, code, elapsed, args.timeout + 60, 600, 30,
                                                     script.read_text(encoding="utf-8"), out_dir, 1)
            ok8, d8 = m4_check.check_kbm(log)
            m4_results.append((m4_check.CRITERIA[7], ok8, d8))
            results += [(name.split(" ", 1)[0], ok, d) for name, ok, d in m4_results]
        run_ok = all(ok for _, ok, _ in results)
        all_ok &= run_ok
        print(f"run {run}: exit {code}, {elapsed:.0f} s, {'OK' if run_ok else 'FAILED'}")
        if not run_ok and stderr:
            own = [ln for ln in stderr.splitlines() if ln.strip() and not m2_check.LOG_LINE.match(ln)]
            if own:
                print("    stderr: " + own[-1][:200])
        for cid, ok, d in results:
            name = CRITERIA.get(cid) or next((c for c in m4_check.CRITERIA if c.startswith(cid + " ")), cid)
            print(f"    [{'PASS' if ok else 'FAIL'}] {name}: {d}")
        if info["engine_summary"]:
            print(f"    [INFO] {info['engine_summary'].group(0)}")
        for stem, lv in info["levels"].items():
            print(f"    [INFO] levels {stem}: active rms {lv['active_rms']:.1f} dBFS, peak {lv['peak']:.1f} dBFS, "
                  f"sound {lv['active_pct']:.0f} %, gaps {lv['gaps']} ({lv['gap_list']})")
        for w in info["warnings"][:10]:
            print(f"    [INFO] {w.split('] ', 1)[-1][:200]}")
        if "event_matches" in detail:
            for r in detail["event_matches"][:40]:
                if "error" in r:
                    print(f"      event {r['event_s']:8.2f} s {r['name']} [{r['stem']}]: {r['error']}")
                else:
                    print(f"      event {r['event_s']:8.2f} s {r['name']} [{r['stem']}]: delay {r['delay_ms']:6.0f} ms, "
                          f"NCC {r['ncc']:.3f}, pitch {r['pitch_ratio']:.4f} (exp. {r['expected_ratio']:.4f}), gain "
                          + " / ".join(f"{g:.1f}" for g in r["gain_db"]) + f" dB, balance {r['balance_db']:+.1f} dB")
        detail["criteria"] = [{"id": c, "ok": ok, "detail": d} for c, ok, d in results]
        (out_dir / f"{run}_result.json").write_text(json.dumps(detail, indent=1, default=float), encoding="utf-8")
        if run_ok and not args.keep_wav and not args.analyse_only:
            for p in out_dir.glob(f"{run}*.wav"):
                p.unlink()

    if "level" in runs and not args.analyse_only:
        common = [f"--set=debug.timeout={args.timeout}"]
        if not int(m2_check.ini_value(cp, "debug", "frames", "0") or "0", 0):
            common.append("--frames=72000")
        if not args.no_rumble:
            rs = m4_check.DEFAULT_RUMBLE_SCRIPT.resolve()
            code, _, elapsed, log, _ = run_game(args.exe, config, userdata, common + [f"--set=debug.input_script={rs}"],
                                                args.timeout, True, out_dir, "rumble")
            ok7, d7 = m4_check.check_rumble(log, code)
            all_ok &= ok7
            print(f"    [{'PASS' if ok7 else 'FAIL'}] {m4_check.CRITERIA[6]}: {d7}")
        if not args.no_broken:
            ok9, d9 = m4_check.check_broken_scripts(args.exe, config, userdata, common, out_dir)
            all_ok &= ok9
            print(f"    [{'PASS' if ok9 else 'FAIL'}] {m4_check.CRITERIA[8]}: {d9}")
    print(f"results: {out_dir}")
    print("M5 check: " + ("ALL PASS" if all_ok else "FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
