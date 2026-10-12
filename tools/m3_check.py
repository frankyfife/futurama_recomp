"""M3 exit-criteria check (docs/loader_design.md section 6, M3 "Erstes Bild").

Runs futurama.exe N times with the D3D11 renderer and a test configuration (pattern of m2_check.py: the
script adds --selftest and, unless the INI sets them, --frames=3000 and --trace=vfs) and checks

  M3-1 the scenes appear completely: for every scene frame (default legal 3.0 s, tech_notice 8.0 s = the
       "Splash" NI_Splash_PAL.nif, loading 13.6 s, frontend 20.0 s, times the frame rate: frames 180/480/816/
       1200 at 60 Hz, 150/400/680/1000 at 50 Hz; --scene NAME=FRAME or NAME=SECONDSs) the INI lists the frame
       in [render] screenshot_frames, the log has "screenshot: <path> (640x480)" and the PNG exists and is
       not trivial (luminance spread, number of colors, no single color covering the picture), and the
       frame's "draw summary frame N:" line has draw calls and none that was not drawn - or, for a frame
       that draws nothing (the legal screen and the tech notice draw 0, 6 or 12 times per frame), the frame
       whose picture the persistent back buffer shows ("shows the picture of frame M (K draw calls, D
       drawn)", two presents ago like the Xbox flip; M3 review finding 8) has K > 0 and D = K. With the xemu
       reference captures (run\\xemu_ref, git-ignored; --xemu-ref) every scene is also compared with the
       original: mean absolute difference per channel on the 640x480 downscale (legal/tech notice/loading:
       whole picture without the xemu menu bar; frontend: only pixels that are static in all xemu frontend
       captures of every reference directory - run\\xemu_ref and, if present, run\\xemu_ref_steam (0.62 s
       interval, all phases of the 1 s steam cycle; M3 review finding 7) - and outside the known periodic
       regions (XEMU_FRONTEND_PERIODIC), because the menu background is animated)
  M3-2 no D3D debug-layer error: exit line "D3D11 debug layer: <state> - N errors, M warnings, K corruption"
       with N = K = 0 and no "D3D11 debug: error|corruption" line; state "enabled", or "not installed" (passes
       with a note: the SDK layers are missing, nothing was validated); "off" fails (Release builds need
       [render] debug_layer = on)
  M3-3 --frames=N without crash, presented frame rate >= the tick rate (M3b: 60 Hz by default, the user
       requirement "at least 60 fps"; 50 Hz with [game] tick_rate = 50): exit 0, "FRAMES: limit of N frames
       reached", the renderer sustains the tick rate - "present: frame work ...: 99th percentile P ms" with
       P <= --max-work-ms (default one tick period: 16.7 ms at 60 Hz, 20 ms at 50 Hz; guest + renderer work per
       frame without Present and pacing) - the paced presentation runs at the tick rate: "present: N frames in
       T s (average A Hz, ...)" with A >= --min-hz (default tick rate - 0.5: 59.5 / 49.5) - and the frames start
       on their period: "present: frame start intervals (planned period): ... -> X % on period" (within +-5 %
       of the planned period) with X >= --min-on-period (default 95). A frame that misses its slot by more
       than one period (a loading hitch) costs that time for good, so the average alone measures "no hitch at
       all", not the renderer (M3 review finding 3). Reported, not judged: the pacing mode and plan, the
       display refresh, plan changes, schedule resets, the swap-chain and on-screen statistics (DXGI frame
       statistics: refreshes per frame, vblank-coupled frames on plan, measured refresh, latency) and the
       window environment (foreground share: scripted runs often lose the foreground; the pacer then runs
       "free" on its timer at the same rate, so only the foreground frames exercise the vblank lock).
  M3-4 renderer totals: the d3d11 backend was active (Create line "(d3d11: ..."), no draw failed /
       unsupported / error / skipped ("d3d11 draws <Kind>:" lines), no texture creation failure
  M3b-5/6 game timing (m2_check.check_timing): T1 the patch table of [game] tick_rate, the read-back after
       Game::Init (60 Hz: fps 60 = 0x42700000, dt 1/60 = 0x3C888889), CApp+0xE8 = 0 and the movie tag "PAL"
       (PAL archives and the language are M2-1/M2-2), only *-PAL.xmv movies; T2 steady fixed steps per second
       = tick rate +- 0.2 % and >= 99 % of the frames with exactly one fixed step (lockstep frame clock)
  M3b-7 environment and display coupling (review M3b finding 7: a run that lost the foreground runs "free" on
       the timer and passed M3-3 just as well): "present: environment: foreground N frames (P %)" with
       P >= --min-foreground (default 95; below = environment invalid, repeat the run with the window in front);
       with [render] pacing = auto|vsync, vsync on and a display refresh that is k x the target (k 1..4 within
       0.2 %, e.g. 120 Hz at 60) also >= --min-vblank-frames % (default 95) of the frames in the vblank plan
       ("frames per plan") and >= --min-on-plan % (default 99) of the vblank-coupled frames exactly k refreshes
       on screen ("vblank-coupled N: M on plan (X %)")

plus every M2 criterion (m2_check.check_run: archives, language, BUILD.TXT, intro + orderly end, fibers and
APCs, SEH self-test, VFS trace) and the general rule: no error/warn/UNIMPLEMENTED/TRAP line.

Usage (paths with spaces belong in the INI, not on the command line):
    python tools/m3_check.py --exe build/m3/bin/futurama.exe --config run/m3/futurama.ini
                             [--runs 1] [--fresh] [--timeout SECONDS] [--scene NAME=FRAME ...]
                             [--xemu-ref run/xemu_ref | --no-xemu-ref] [--xemu-frontend DIR ...]
                             [--min-hz HZ] [--max-work-ms MS] [--min-on-period PERCENT]
                             [--min-foreground PERCENT] [--min-vblank-frames PERCENT] [--min-on-plan PERCENT]

Logs and screenshots of every run: <userdata>/m3_check/run_NN.log, run_NN_vfs_trace.log,
run_NN_frame_NNNNNN.png. Exit code 0 when all criteria pass. Needs Pillow for the image checks.
"""
import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m2_check  # noqa: E402  (same directory: config parsing and the M2 criteria)

try:
    from PIL import Image, ImageChops, ImageStat
except ImportError:  # the image criteria fail with a clear message
    Image = None

DEFAULT_FRAMES = 3000
# Scene times in seconds (M3b): frame = round(seconds * frame rate). At 50 Hz these are the M3 frames 150/400/680/
# 1000; at 60 Hz 180/480/816/1200. The screens count QPC seconds (legal 6.0 s from ~0.2 s, tech notice up to
# 5.0 s from ~6.3 s, loading screen ~11.4..15.1 s, frontend from ~15.2-16.1 s with a tick-counted fade of 50
# ticks = 0.83 s at 60 Hz; analysis/loader/timing60.md 4.2, 7), so the same seconds show the same scenes.
DEFAULT_SCENE_SECONDS = {"legal": 3.0, "tech_notice": 8.0, "loading": 13.6, "frontend": 20.0}
MIN_ON_PERIOD_PERCENT = 95.0
# M3b-7 (review M3b finding 7)
MIN_FOREGROUND_PERCENT = 95.0
MIN_VBLANK_FRAMES_PERCENT = 95.0
MIN_ON_PLAN_PERCENT = 99.0
INTEGER_TOLERANCE = 0.002  # platform/pacer_policy.h kIntegerTolerance
MAX_SYNC_INTERVAL = 4
# xemu reference (run\xemu_ref\scenes.tsv): scene -> reference image, rows of the 640x480 image to ignore
# (the xemu menu bar covers the top ~24 pixels of the early captures), mean-difference limit per channel.
XEMU_REFS = {
    "legal": ("ref_legal.png", 24, 1.0),
    "tech_notice": ("ref_tech_notice.png", 0, 1.0),
    "loading": ("ref_splash.png", 0, 2.0),  # the can in the corner is animated
}
# Frontend: compared on the pixels that are static in every xemu capture of the frontend context (capture
# times from the "frontend" row of <dir>/scenes.tsv, else XEMU_FRONTEND_TIMES) of every reference directory.
XEMU_FRONTEND_TIMES = (78.0, 132.0)
XEMU_FRONTEND_LIMIT = 2.0  # mean difference limit on the static pixels
# Regions of the 640x480 frontend that animate with a period the 3 s captures of run\xemu_ref alias to a
# constant (left, top, right, bottom): the steam cloud at the right pipe repeats every 1.0 s (NIF time: 50 frames
# at 50 Hz, 60 at 60 Hz; M3 review finding 7, confirmed with the 0.62 s captures of run\xemu_ref_steam).
XEMU_FRONTEND_PERIODIC = [(505, 95, 610, 170)]
MIN_LUMA_STDDEV = 8.0
MIN_COLORS = 64
MAX_TOP_COLOR_SHARE = 0.98

CRITERIA = [
    "M3-1 scenes appear completely (screenshots, draws, xemu comparison)",
    "M3-2 no D3D debug-layer error",
    "M3-3 frame limit reached, renderer sustains the tick rate, paced presentation at the tick rate",
    "M3-4 d3d11 renderer active, no draw/texture failure",
]


def image_stats(path: Path) -> tuple[bool, str]:
    """Non-trivial picture: luminance spread, number of colors, no dominant single color."""
    im = Image.open(path).convert("RGB")
    luma = ImageStat.Stat(im.convert("L"))
    colors = im.getcolors(maxcolors=1 << 20) or []
    total = im.width * im.height
    top = max((n for n, _ in colors), default=total) / total
    ok = luma.stddev[0] >= MIN_LUMA_STDDEV and len(colors) >= MIN_COLORS and top <= MAX_TOP_COLOR_SHARE
    return ok, (f"{im.width}x{im.height}, luminance mean {luma.mean[0]:.1f} stddev {luma.stddev[0]:.1f}, "
                f"{len(colors)} colors, top color {top * 100:.1f} %")


def mean_diff(a, b, mask=None) -> float:
    """Mean absolute difference per channel (0..255), optionally only where mask (L image) is 255."""
    d = ImageChops.difference(a, b)
    stat = ImageStat.Stat(d, mask) if mask is not None else ImageStat.Stat(d)
    return sum(stat.mean) / 3.0


def frontend_times(ref_dir: Path) -> tuple[float, float]:
    """Capture times of the frontend context from <dir>/scenes.tsv (row "frontend"), else the defaults."""
    scenes = ref_dir / "scenes.tsv"
    if scenes.exists():
        for line in scenes.read_text(encoding="utf-8").splitlines()[1:]:
            cols = line.split("\t")
            if len(cols) >= 3 and cols[0] == "frontend":
                return float(cols[1]), float(cols[2])
    return XEMU_FRONTEND_TIMES


def compare_xemu(scene: str, shot: Path, ref_dir: Path, frontend_cache: dict,
                 frontend_dirs: list[Path]) -> tuple[bool | None, str]:
    """(ok or None if no reference, detail)."""
    ours = Image.open(shot).convert("RGB")
    if scene in XEMU_REFS:
        name, skip, limit = XEMU_REFS[scene]
        ref_path = ref_dir / name
        if not ref_path.exists():
            return None, f"no xemu reference {name}"
        ref = Image.open(ref_path).convert("RGB").resize(ours.size, Image.BOX)
        box = (0, skip, ours.width, ours.height)
        diff = mean_diff(ours.crop(box), ref.crop(box))
        return diff <= limit, f"xemu {name}: mean difference {diff:.2f} (limit {limit})"
    if scene.startswith("frontend"):
        limit = XEMU_FRONTEND_LIMIT
        if "refs" not in frontend_cache:
            refs, per_dir = [], []
            for d in [ref_dir] + [f for f in frontend_dirs if f.resolve() != ref_dir.resolve()]:
                lo, hi = frontend_times(d)
                n = 0
                for f in sorted(d.glob("t*.png")):
                    try:
                        t = float(f.stem[1:-1])
                    except ValueError:
                        continue
                    if lo <= t <= hi:
                        refs.append(Image.open(f).convert("RGB").resize(ours.size, Image.BOX))
                        n += 1
                per_dir.append(f"{d.name} {n}")
            mask = None
            if len(refs) >= 2:
                # Static pixels: no reference frame differs from the first by more than 2 in any channel, and
                # not inside a known periodic region.
                moving = Image.new("L", ours.size, 0)
                for r in refs[1:]:
                    d = ImageChops.difference(refs[0], r)
                    r_, g_, b_ = d.split()
                    moving = ImageChops.lighter(moving, ImageChops.lighter(ImageChops.lighter(r_, g_), b_))
                for box in XEMU_FRONTEND_PERIODIC:
                    moving.paste(255, box)
                mask = moving.point(lambda v: 255 if v <= 2 else 0)
            frontend_cache["refs"] = refs
            frontend_cache["mask"] = mask
            frontend_cache["dirs"] = ", ".join(per_dir)
        refs, mask = frontend_cache["refs"], frontend_cache["mask"]
        if mask is None:
            return None, "no xemu frontend captures"
        static = ImageStat.Stat(mask).mean[0] / 255.0
        diff = mean_diff(ours, refs[0], mask)
        return diff <= limit, (f"xemu frontend ({len(refs)} captures: {frontend_cache['dirs']}): mean difference "
                               f"{diff:.2f} on the {static * 100:.0f} % static pixels (limit {limit})")
    return None, "no xemu reference for this scene"


def check_m3(log: str, exit_code: int, frames: int, scenes: dict, shot_frames: list[int], userdata: Path,
             out_dir: Path, run: int, ref_dir: Path | None, min_hz: float, max_work_ms: float = 20.0,
             frontend_dirs: list[Path] | None = None,
             min_on_period: float = MIN_ON_PERIOD_PERCENT) -> tuple[list, dict]:
    lines = log.splitlines()
    info: dict = {}
    results = []

    def first(rx: str):
        for ln in lines:
            m = re.search(rx, ln)
            if m:
                return m
        return None

    # M3-1 scenes
    problems, details = [], []
    frontend_cache: dict = {}
    for scene, frame in sorted(scenes.items(), key=lambda kv: kv[1]):
        if frame not in shot_frames:
            problems.append(f"{scene}: frame {frame} is not in [render] screenshot_frames")
            continue
        if frame > frames:
            problems.append(f"{scene}: frame {frame} is beyond the frame limit {frames}")
            continue
        shot_line = first(rf"screenshot: (.*frame_{frame:06d}\.png) \((\d+)x(\d+)\)")
        summary = first(rf"draw summary frame {frame}: (\d+) draw calls, (\d+) drawn, (\d+) not drawn, (\d+) clears"
                        rf"(?:.*?; shows the picture of frame (\d+) \((\d+) draw calls, (\d+) drawn\))?")
        if shot_line is None:
            problems.append(f"{scene}: no screenshot line for frame {frame}")
            continue
        shot = Path(shot_line.group(1))
        if not shot.exists():
            problems.append(f"{scene}: {shot} missing")
            continue
        copy = out_dir / f"run_{run:02d}_frame_{frame:06d}.png"
        shutil.copyfile(shot, copy)
        text = f"{scene} (frame {frame}): "
        if summary is None:
            problems.append(f"{scene}: no draw summary for frame {frame}")
        else:
            calls, drawn, not_drawn = int(summary.group(1)), int(summary.group(2)), int(summary.group(3))
            text += f"{calls} draws, {drawn} drawn, {not_drawn} not drawn; "
            if calls == 0 and summary.group(5):
                # Nothing drawn in this frame: the back buffer still holds the picture of frame M.
                shown, k, d = int(summary.group(5)), int(summary.group(6)), int(summary.group(7))
                text += f"shows the picture of frame {shown} ({k} draws, {d} drawn); "
                if k == 0 or d != k:
                    problems.append(f"{scene}: frame {frame} shows frame {shown} with {k} draw calls, {d} drawn")
            elif calls == 0 or not_drawn != 0:
                problems.append(f"{scene}: frame {frame} has {calls} draw calls, {not_drawn} not drawn")
        if Image is None:
            problems.append("Pillow is not installed - image checks impossible")
            details.append(text)
            continue
        ok, stats = image_stats(shot)
        text += stats
        if not ok:
            problems.append(f"{scene}: trivial picture ({stats})")
        if ref_dir is not None:
            ref_ok, ref_text = compare_xemu(scene, shot, ref_dir, frontend_cache, frontend_dirs or [])
            text += "; " + ref_text
            if ref_ok is False:
                problems.append(f"{scene}: {ref_text}")
        details.append(text)
    info["scenes"] = details
    results.append((CRITERIA[0], not problems,
                    "; ".join(problems) if problems else f"{len(details)} scenes ok (details below)"))

    # M3-2 debug layer
    layer = first(r"D3D11 debug layer: (.+?) - (\d+) errors, (\d+) warnings, (\d+) corruption")
    error_lines = [ln for ln in lines if re.search(r"D3D11 debug: (error|corruption) ", ln)]
    if layer is None:
        results.append((CRITERIA[1], False, "no 'D3D11 debug layer:' exit line"))
    else:
        state, errors, warnings, corruption = layer.group(1), int(layer.group(2)), int(layer.group(3)), \
            int(layer.group(4))
        # "not installed": the SDK layers are missing (reported, nothing validated); "off"/"unavailable" mean the
        # run did not ask for validation - set [render] debug_layer = on (Release builds) or auto (Debug builds).
        note = "" if state == "enabled" else f" - NOTE: debug layer {state}, nothing was validated"
        if state not in ("enabled", "not installed"):
            note += " (set [render] debug_layer = on)"
        results.append((CRITERIA[1], errors == 0 and corruption == 0 and not error_lines
                        and state in ("enabled", "not installed"),
                        f"debug layer {state}: {errors} errors, {warnings} warnings, {corruption} corruption, "
                        f"{len(error_lines)} error lines{note}"))
        info["debug_layer"] = (state, errors, warnings, corruption)

    # M3-3 frame rate
    present = first(r"present: (\d+) frames in ([\d.]+) s \(average ([\d.]+) Hz, slowest 1 s window ([\d.]+) Hz, "
                    r"longest frame ([\d.]+) ms\)")
    pacing = first(r"present: pacing (\d+) Hz, (\d+) schedule resets.*?, (\d+) frames longer")
    work = first(r"present: frame work (\d+) frames: average ([\d.]+) ms, 99th percentile ([\d.]+) ms, longest "
                 r"([\d.]+) ms \((\d+) frames longer than one pacing period\) -> sustainable (\d+) Hz")
    limit = first(rf"FRAMES: limit of {frames} frames reached")
    # M3b pacing lines (platform/frame_pacer.h): reported; the on-period share is judged.
    mode = first(r"present: mode (\w+) \(tick (\d+) Hz, target ([\d.]+) Hz\), plan (.+?); frames per plan: (.+?); "
                 r"(\d+) plan changes")
    display = first(r"present: display (.+?), tearing ([\w ]+), frame-latency waitable (\w+), VRR (.+?)(?:; (\d+) "
                    r"monitor changes, (\d+) display events)?$")
    intervals = first(r"present: frame start intervals \(planned period\): (.+) -> ([\d.]+) % on period")
    swap = first(r"present: swap chain: (.+)$")
    screen = first(r"present: on screen \(DXGI frame statistics\): (.+)$")
    env = first(r"present: environment: foreground (\d+) frames \(([\d.]+) %\), (.+)$")
    if present is None or work is None or intervals is None:
        results.append((CRITERIA[2], False, "no 'present:' statistics / frame work / frame start interval line"))
    else:
        n, secs, avg, slow, longest = int(present.group(1)), float(present.group(2)), float(present.group(3)), \
            float(present.group(4)), float(present.group(5))
        w_avg, w_p99, w_longest, w_over, w_hz = float(work.group(2)), float(work.group(3)), float(work.group(4)), \
            int(work.group(5)), int(work.group(6))
        on_period = float(intervals.group(2))
        ok = (exit_code == 0 and limit is not None and n >= frames and avg >= min_hz and w_p99 <= max_work_ms
              and on_period >= min_on_period)
        results.append((CRITERIA[2], ok,
                        f"exit {exit_code}, limit {'reached' if limit else 'NOT reached'}; frame work average "
                        f"{w_avg:.2f} ms, 99th percentile {w_p99:.1f} ms (maximum {max_work_ms:.1f}), longest "
                        f"{w_longest:.1f} ms, {w_over} over one period -> sustainable {w_hz} Hz; {n} frames presented "
                        f"in {secs:.2f} s: average {avg:.2f} Hz (minimum {min_hz}), slowest 1 s window {slow:.2f} Hz, "
                        f"longest frame {longest:.1f} ms"
                        + (f", {pacing.group(2)} schedule resets, {pacing.group(3)} slow frames" if pacing else "")
                        + f"; frame starts {on_period:.2f} % on period (minimum {min_on_period})"))
        info["fps"] = avg
        info["work_p99"] = w_p99
        info["on_period"] = on_period
    pacing_info = []
    if mode:
        pacing_info.append(f"mode {mode.group(1)} (tick {mode.group(2)} Hz, target {mode.group(3)} Hz), final plan "
                           f"{mode.group(4)}; frames per plan: {mode.group(5)}; {mode.group(6)} plan changes")
    if display:
        pacing_info.append(f"display {display.group(1)}, tearing {display.group(2)}, waitable {display.group(3)}, "
                           f"VRR {display.group(4)}")
    if intervals:
        pacing_info.append(f"frame start intervals {intervals.group(1)}")
    if swap:
        pacing_info.append(f"swap chain: {swap.group(1)}")
    if screen:
        pacing_info.append(f"on screen: {screen.group(1)}")
    if env:
        pacing_info.append(f"environment: foreground {env.group(1)} frames ({env.group(2)} %), {env.group(3)}")
        info["foreground"] = float(env.group(2))
    info["pacing"] = pacing_info
    if mode:
        info["plan_frames"] = mode.group(5)
        info["target_hz"] = float(mode.group(3))
    if display:
        hz = re.search(r"= ([\d.]+) Hz", display.group(1))
        info["refresh_hz"] = float(hz.group(1)) if hz else None
    if screen:
        vb = re.search(r"vblank-coupled (\d+): (\d+) on plan \(([\d.]+) %\)", screen.group(1))
        hz = re.search(r"measured refresh ([\d.]+) Hz", screen.group(1))
        info["vblank_on_plan"] = (int(vb.group(1)), int(vb.group(2)), float(vb.group(3))) if vb else None
        info["measured_refresh"] = float(hz.group(1)) if hz else None

    # M3-4 renderer totals
    create = first(r"-> NiPCRenderer 0x[0-9A-F]+ \(d3d11: ([^;]+);")
    draw_lines = [re.search(r"d3d11 draws (\w+): (\d+) calls - drawn (\d+), skipped (\d+), failed (\d+), empty (\d+), "
                            r"unsupported (\d+), error (\d+), not in scene (\d+), texture target (\d+)", ln)
                  for ln in lines]
    draw_lines = [m for m in draw_lines if m]
    tex = first(r"textures (\d+) created, (\d+) destroyed, (\d+) failed")
    bad = []
    totals = []
    for m in draw_lines:
        kind = m.group(1)
        calls, drawn, skipped, failed, empty, unsup, err = (int(m.group(i)) for i in range(2, 9))
        totals.append(f"{kind} {drawn}/{calls}")
        if skipped or failed or unsup or err:
            bad.append(f"{kind}: skipped {skipped}, failed {failed}, unsupported {unsup}, error {err}")
    if create is None:
        bad.append("the d3d11 renderer was not created (null renderer?)")
    if not draw_lines:
        bad.append("no 'd3d11 draws' statistics")
    if tex is None or int(tex.group(3)) != 0:
        bad.append(f"texture failures {tex.group(3) if tex else '?'}")
    results.append((CRITERIA[3], not bad,
                    (f"{create.group(1) if create else '?'}; drawn/calls: {', '.join(totals)}; textures "
                     f"{tex.group(1) if tex else '?'} created") + (f"; {'; '.join(bad)}" if bad else "")))
    return results, info


def plan_frames(text: str) -> dict:
    """'vblank 2970, free 30' -> {'vblank': 2970, 'free': 30}."""
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"([\w-]+) (\d+)", text or "")}


def check_coupling(info: dict, pacing_mode: str, vsync: bool, min_foreground: float, min_vblank_frames: float,
                   min_on_plan: float) -> tuple[str, bool, str]:
    """M3b-7: the window was in front, and an integer refresh was really vblank-coupled (review M3b finding 7)."""
    name = "M3b-7 window in the foreground, frames vblank-coupled on an integer refresh"
    fg = info.get("foreground")
    if fg is None:
        return name, False, "no 'present: environment' line"
    problems = []
    detail = [f"foreground {fg:.1f} % (minimum {min_foreground:.0f})"]
    if fg < min_foreground:
        problems.append(f"window in the foreground for only {fg:.1f} % of the frames - environment invalid, repeat "
                        f"the run with the window in front")
    refresh, target = info.get("refresh_hz"), info.get("target_hz")
    k = round(refresh / target) if refresh and target else 0
    integer = k >= 1 and abs(refresh / k - target) <= INTEGER_TOLERANCE * target
    if pacing_mode in ("auto", "vsync") and vsync and integer and k <= MAX_SYNC_INTERVAL:
        frames = plan_frames(info.get("plan_frames", ""))
        total = sum(frames.values())
        share = 100.0 * frames.get("vblank", 0) / total if total else 0.0
        detail.append(f"refresh {refresh:.3f} Hz = {k} x {target:.0f} Hz: {frames.get('vblank', 0)} of {total} frames "
                      f"in the vblank plan ({share:.1f} %, minimum {min_vblank_frames:.0f})")
        if share < min_vblank_frames:
            problems.append(f"only {share:.1f} % of the frames in the vblank plan")
        on_plan = info.get("vblank_on_plan")
        if not on_plan or on_plan[0] == 0:
            problems.append("no vblank-coupled on-screen statistics")
        else:
            detail.append(f"{on_plan[1]} of {on_plan[0]} vblank-coupled frames exactly {k} refreshes ({on_plan[2]:.2f} %, "
                          f"minimum {min_on_plan:.0f})")
            if on_plan[2] < min_on_plan:
                problems.append(f"only {on_plan[2]:.2f} % of the vblank-coupled frames on plan")
    else:
        detail.append(f"no vblank plan expected (pacing {pacing_mode}, vsync {int(vsync)}, refresh "
                      f"{refresh or 0:.3f} Hz, target {target or 0:.0f} Hz)")
    return name, not problems, "; ".join(problems + detail) if problems else "; ".join(detail)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--config", required=True, type=Path)
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--timeout", type=int, help="seconds per run (default: frames/rate + 90)")
    ap.add_argument("--scene", action="append", default=[], metavar="NAME=FRAME|NAME=SECONDSs",
                    help="scene frame or time (repeatable; replaces the defaults legal=3.0s tech_notice=8.0s "
                         "loading=13.6s frontend=20.0s, i.e. 180/480/816/1200 at 60 Hz)")
    ap.add_argument("--xemu-ref", type=Path, help="xemu reference captures (default run/xemu_ref if present)")
    ap.add_argument("--no-xemu-ref", action="store_true")
    ap.add_argument("--xemu-frontend", type=Path, action="append", default=[],
                    help="further xemu capture directories for the frontend's static-pixel mask (default "
                         "run/xemu_ref_steam if present)")
    ap.add_argument("--min-hz", type=float,
                    help="minimum average presented rate of the paced run (default tick rate - 0.5, see M3-3)")
    ap.add_argument("--max-work-ms", type=float,
                    help="maximum 99th percentile of the frame work in ms (default one tick period)")
    ap.add_argument("--min-on-period", type=float, default=MIN_ON_PERIOD_PERCENT,
                    help=f"minimum share of frame starts within +-5 %% of the planned period (default "
                         f"{MIN_ON_PERIOD_PERCENT:.0f})")
    ap.add_argument("--min-foreground", type=float, default=MIN_FOREGROUND_PERCENT,
                    help=f"M3b-7: minimum share of frames with the window in the foreground (default "
                         f"{MIN_FOREGROUND_PERCENT:.0f})")
    ap.add_argument("--min-vblank-frames", type=float, default=MIN_VBLANK_FRAMES_PERCENT,
                    help=f"M3b-7: minimum share of frames in the vblank plan on an integer refresh (default "
                         f"{MIN_VBLANK_FRAMES_PERCENT:.0f})")
    ap.add_argument("--min-on-plan", type=float, default=MIN_ON_PLAN_PERCENT,
                    help=f"M3b-7: minimum share of vblank-coupled frames exactly k refreshes on screen (default "
                         f"{MIN_ON_PLAN_PERCENT:.0f})")
    args = ap.parse_args()

    config = args.config.resolve()
    cp = m2_check.read_config(config)
    ud = m2_check.ini_value(cp, "paths", "userdata")
    if not ud:
        sys.exit("m3_check: [paths] userdata must be set in the test config (never %LOCALAPPDATA% in tests)")
    userdata = Path(ud) if Path(ud).is_absolute() else config.parent / ud
    if m2_check.ini_value(cp, "debug", "stop_at"):
        sys.exit("m3_check: the test config must not set [debug] stop_at")
    backend = m2_check.ini_value(cp, "render", "backend", "d3d11").lower()
    if backend != "d3d11":
        sys.exit(f"m3_check: [render] backend = {backend} - the M3 check needs d3d11")
    language = m2_check.expected_language(cp)
    # M3b: the game ticks at [game] tick_rate (60 by default) and [render] frame_rate follows it.
    ticks = m2_check.tick_rate(cp)
    clock = m2_check.frame_clock(cp)
    rate = m2_check.frame_rate(cp)
    pacing_mode = (m2_check.ini_value(cp, "render", "pacing", "auto") or "auto").strip().strip('"').lower()
    vsync = (m2_check.ini_value(cp, "render", "vsync", "1") or "1").strip().lower() in ("1", "true", "yes", "on")
    if rate != ticks:
        print(f"m3_check: warning: [render] frame_rate = {rate} differs from [game] tick_rate = {ticks} - the exe logs "
              f"a warning (general rule) and frame numbers are no longer ticks; remove frame_rate")
    if pacing_mode == "off" or rate == 0:
        print("m3_check: warning: [render] pacing = off - frames are not 1/rate s apart, scene frames and M3-3 do "
              "not apply")
    elif pacing_mode == "auto" and ticks == 50:
        print("m3_check: note: tick rate 50 with [render] pacing = auto - on a display whose refresh is not a "
              "multiple of 50 Hz without VRR the pacer presents at the display rate (frame numbers are no longer "
              "1/50 s); use [render] pacing = timer for frame-numbered scenes")
    min_hz = args.min_hz if args.min_hz is not None else max(rate, 1) - 0.5
    max_work_ms = args.max_work_ms if args.max_work_ms is not None else 1000.0 / max(rate, 1)
    scenes = {name: round(sec * max(rate, 1)) for name, sec in DEFAULT_SCENE_SECONDS.items()}
    if args.scene:
        scenes = {}
        for s in args.scene:
            name, _, value = s.partition("=")
            if name and value.isdigit():
                scenes[name] = int(value)
            elif name and value.endswith("s") and re.fullmatch(r"\d+(\.\d+)?", value[:-1]):
                scenes[name] = round(float(value[:-1]) * max(rate, 1))
            else:
                sys.exit(f"m3_check: bad --scene {s!r} (NAME=FRAME or NAME=SECONDSs)")
    shot_frames = sorted({int(v, 0) for v in m2_check.ini_value(cp, "render", "screenshot_frames").replace(" ", "")
                          .split(",") if v})
    missing = [f"{k}={v}" for k, v in scenes.items() if v not in shot_frames]
    if missing:
        print(f"m3_check: warning: scene frames {', '.join(missing)} are not in [render] screenshot_frames "
              f"{shot_frames} (frames at {rate} Hz) - M3-1 will fail")
    repo = Path(__file__).resolve().parent.parent
    ref_dir = None
    if not args.no_xemu_ref:
        ref_dir = args.xemu_ref or (repo / "run" / "xemu_ref")
        if not ref_dir.is_dir():
            print(f"m3_check: no xemu reference captures at {ref_dir} - scene comparison skipped")
            ref_dir = None
    frontend_dirs = list(args.xemu_frontend)
    if not frontend_dirs and (repo / "run" / "xemu_ref_steam").is_dir():
        frontend_dirs = [repo / "run" / "xemu_ref_steam"]

    # M6: movies end at once like before M6 ([debug] movies = skip), so the scene frames stay valid.
    extra_args = ["--selftest", "--set=debug.movies=skip"]
    frames = int(m2_check.ini_value(cp, "debug", "frames", "0") or "0", 0)
    if frames == 0:
        frames = DEFAULT_FRAMES
        extra_args.append(f"--frames={frames}")
    trace = [t.strip().lower() for t in m2_check.ini_value(cp, "debug", "trace").replace(";", ",").split(",")
             if t.strip()]
    if "vfs" not in trace and "all" not in trace:
        extra_args.append("--trace=" + ",".join(trace + ["vfs"]))
    timeout = args.timeout or (frames // max(rate, 1) + 90)

    out_dir = userdata / "m3_check"
    shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True)
    print(f"m3_check: {args.runs} run(s), {frames} frames at {rate} Hz (tick rate {ticks} Hz, frame clock {clock}, "
          f"pacing {pacing_mode}), language {language}, scenes "
          f"{', '.join(f'{k}={v}' for k, v in sorted(scenes.items(), key=lambda kv: kv[1]))}, "
          f"M3-3 minimum {min_hz:.2f} Hz / work p99 {max_work_ms:.1f} ms / {args.min_on_period:.0f} % on period, "
          f"M3b-7 foreground {args.min_foreground:.0f} % / vblank frames {args.min_vblank_frames:.0f} % / on plan "
          f"{args.min_on_plan:.0f} %, "
          f"xemu reference {ref_dir or 'none'}, extra args {' '.join(extra_args)}, timeout {timeout} s")

    all_ok = True
    for i in range(1, args.runs + 1):
        if args.fresh or i == 1:
            for sub in ("hdd", "cache", "logs", "screenshots"):
                shutil.rmtree(userdata / sub, ignore_errors=True)
        log_path = userdata / "logs" / "futurama.log"
        trace_path = userdata / "logs" / "vfs_trace.log"
        log_path.unlink(missing_ok=True)
        trace_path.unlink(missing_ok=True)
        start = time.monotonic()
        try:
            proc = subprocess.run([str(args.exe.resolve()), f"--config={config}", *extra_args], cwd=userdata,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=timeout)
            code, stderr = proc.returncode, proc.stderr.decode("utf-8", errors="replace").strip()
        except subprocess.TimeoutExpired:
            code, stderr = -1, f"killed after {timeout} s (hang)"
        elapsed = time.monotonic() - start
        log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        vfs = trace_path.read_text(encoding="utf-8", errors="replace") if trace_path.exists() else ""
        (out_dir / f"run_{i:02d}.log").write_text(log, encoding="utf-8")
        (out_dir / f"run_{i:02d}_vfs_trace.log").write_text(vfs, encoding="utf-8")
        m2_results, _ = m2_check.check_run(log, vfs, code, elapsed, timeout, frames, language, userdata, rate)
        m3_results, info = check_m3(log, code, frames, scenes, shot_frames, userdata, out_dir, i, ref_dir,
                                    min_hz, max_work_ms, frontend_dirs, args.min_on_period)
        # One frame per tick unless the run presents at another rate (display-rate plan, pacing off).
        at_tick_rate = rate == ticks and pacing_mode != "off" and "display-rate" not in info.get("plan_frames", "")
        timing_results, _ = m2_check.check_timing(log, vfs, ticks, clock, at_tick_rate)
        coupling = check_coupling(info, pacing_mode, vsync, args.min_foreground, args.min_vblank_frames,
                                  args.min_on_plan)
        results = (m3_results + [(f"M3b-{5 + k} " + name.split(" ", 1)[1], ok, detail)
                                 for k, (name, ok, detail) in enumerate(timing_results)]
                   + [coupling]
                   + [("M2-" + name, ok, detail) for name, ok, detail in m2_results])
        run_ok = all(ok for _, ok, _ in results)
        all_ok &= run_ok
        print(f"run {i:2d}: exit {code}, {elapsed:.0f} s, {'OK' if run_ok else 'FAILED'}")
        if not run_ok:
            own = [ln for ln in stderr.splitlines() if ln.strip() and not m2_check.LOG_LINE.match(ln)]
            first_error = next((ln for ln in log.splitlines() if re.search(r" (ERROR|WARN) ", ln)), None)
            if own:
                print("    stderr: " + own[-1][:200])
            elif first_error:
                print("    first error/warn line of the log: " + first_error[:200])
        for name, ok, detail in results:
            # Every run: the frame rate and timing criteria; the rest on the first run and on failure.
            if i == 1 or not ok or name.startswith(("M3-3", "M3b-")):
                print(f"    [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        for line in info.get("scenes", []):
            print(f"        {line}")
        for line in info.get("pacing", []):
            print(f"        pacing: {line}")
    print(f"logs and screenshots: {out_dir}")
    print("M3 check: " + ("ALL PASS" if all_ok else "FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
