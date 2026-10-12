"""M3d check: frame rates up to 120 Hz with interpolation between the logic ticks (docs/ROADMAP.md "M3d";
src/INTERFACES.md 4.24/4.25; src/host/gfx/GFX_CONTRACT.md 13; analysis/loader/m3d_frameclock.md 4.5 and
m3d_interpolation.md 8).

Runs futurama.exe over a variant matrix derived from ONE base INI (like m3c_check: [paths] iso, [settings] and the
render keys of the base - classic 640x480 without AA for the xemu comparison - are kept; [paths] userdata is the root:
every variant runs in <userdata>/m3d_check/<variant>/ with its own generated futurama.ini).

Variants
  i120     [render] frame_rate = 120, pacing auto, low_latency on: the launcher's "120 (interpoliert)"
  d120     [render] frame_rate = display: "Monitor (max. 120)"
  f120     i120 in borderless fullscreen (the swap chain covers the monitor since M3d) with a desktop capture at 20 s
  t120     developer frame_rate = 0x78 (fixed 120 fps, M3b meaning) on the timer plan - independent of the display -
           with [render] interpolation_debug = trace (per-frame counters) and a screenshot of every frame of the
           frontend fly-in (13.0-14.9 s) and of a steady frontend stretch (21.6-22.0 s): the motion data
  t120off  t120 with [render] interpolation = off: reference for the picture metric (R1)
  t120x    t120 with trace_frames instead of screenshots: every draw and node of every tick of the same frames (D8;
           writing them slows these frames, so only the transforms are judged there)
  c60      frame_rate = 60: the full m3_check (xemu comparison, M3b timing, display coupling) at 60 fps
  ll60off  c60 with [render] low_latency = off: latency reference for the just-in-time start of c60
  c60off   c60 with [render] interpolation = off and [hooks] disable = probe:NiAVObject::Destroy (no M3d renderer
           hooks): its pictures must be bit-identical to the 60 fps runs with the interpolation on

Criteria
  every variant
    G1  exit 0, frame limit reached and every M2 criterion (m2_check.check_run with the variant's frame rate; the only
        WARN line tolerated is t120's developer frame_rate warning)
    G2  D3D11 debug layer enabled with 0 errors / corruption (m3_check M3-2; the variants set debug_layer = on)
    G3  d3d11 renderer active, every draw drawn, no texture failure (m3_check M3-4)
  display variants i120 / d120 - the expectation follows the display refresh R like pacing::ChooseFrameRate: R up to
  120 Hz one frame per refresh (60 Hz: 60 fps without interpolation), k x 120 Hz vblank k, otherwise "display" R/k
  with k = ceil(R / 120) and "120" the VRR timer or the cadence (120 on average, no vblank k):
    D1  the plan of the "present:" lines is vblank k at the expected rate for >= 95 % of the frames with the window in
        front (in the background the pacer runs "free" on its timer by design), >= 1000 vblank-coupled frames of which
        >= 99 % were exactly k refreshes on screen, the average rate >= 99.5 % of the expected one; without a vblank
        plan also >= 95 % of the frame starts on period (on a vblank plan the starts follow the waitable and the
        just-in-time start, whose release jitters by up to ~0.7 ms in a composed window - reported). ENVIRONMENT: a refresh of at least --min-refresh Hz (default 120) and the window in the
        foreground for >= 95 % or >= 1500 of the frames; otherwise the run is "environment invalid": FAIL, or SKIP
        with --display-optional (then only the display-independent criteria count). --env-retries repeats such a run.
  f120
    F1  the 'window:' view is the whole monitor and the desktop capture shows the 4:3 picture centred with black bars
  120 fps (i120 / d120 / f120 on 120 Hz, t120)
    D2  logic at the tick rate: steady 60.0 steps/s +- 0.2 % (M3b-6), "game clock M3d: 2 frames per tick", steps per
        frame 0 and 1 each 50 +- 2 %, two or more <= 1 % (every frame pair runs one fixed step)
    D3  alpha: "alpha 1/2 x a, 1 x b, other x c" with a and b 50 +- 2 % of the frames between ticks and c <= 0.5 %;
        clock events (snap-all: context switch, clock reset; step pairs: catch-up, 6 dt cap) <= 1 % of the frames
    D4  interpolation: "interpolation: active from frame N"; of the moving draws of the frames between two ticks
        >= 98 % interpolated (interpolated / (interpolated + translation, rotation, scale, frame-driven, destroyed and
        generation-gap snaps)); no identity / not-orthonormal / mirror snap, nothing "not recorded", no overflow, no
        failed skin self-test. Reported: snap reasons, cameras, skins, hierarchy, cost per frame
    D5  repeated picture (F1): "repeated pictures R" > 0 and the renderer presented all R again ("showing the previous
        picture again"); the screenshots of two consecutive frames (one with, one without a fixed step) of the legal
        screen (3.0 s) and the tech notice (8.0 s) are the same picture (mean difference <= 0.5) - no flicker
    D6  the M3 scenes at 120 fps: m3_check M3-1 (legal 3.0 s, tech notice 8.0 s, loading 12.3 s, frontend 20.0 s -
        the loading screen is frame-bound and ends earlier at 120 fps) with the xemu comparison of both frames of
        every pair, and M3-3 at 119.5 Hz without the frame-start share (display variants, see D1; t120 reports it)
  t120 / t120x (motion)
    D7  alpha sequence (logs/interp_trace.csv): in the frontend the displayed time G - 1 + alpha grows by exactly 1/2
        from frame to frame in >= 99 % of the frames (the loading frames before a context switch counted apart), never
        backwards or off the half-tick grid, no frame without a step with alpha < 1
    D8  transforms (logs/interp_draws.csv, interp_nodes.csv, "interp camera frame" lines): (a) draws of frames with
        alpha 1 and static draws show S[G]; (b) world-interpolated draws show lerp(S[G-1], S[G], alpha); (c) hierarchy
        draws show the independent Python reference of the parent-chain interpolation (relative 1e-4); (d) every
        interpolated translation lies between its neighbours (distance to the chord midpoint <= half the step: also
        the arc of a hierarchy draw); (e) two-tick midpoint test with the real middle state (interp(S[G-2], S[G], 1/2)
        against S[G-1] at the bound centre, projected at 640x480): p99 <= --max-midpoint-px (default 1.0)
    D9  pictures: screenshot triplets (frame showing S[G-1], the interpolated frame, frame showing S[G]) over the
        fly-in and the steady frontend; on the pixels that move between the outer frames the middle one lies between
        them: r = d(F0,F1) / (d(F0,F1) + d(F1,F2)) (0 = the old picture, 1 = not interpolated: S[G] already) with
        median 0.3..0.75 and >= 90 % of the triplets within 0.15..0.85; reported: the detour (d01 + d12) / d02.
        Content that stays at the tick rate by design (lightning arcs, the oscilloscope wave, the fade, objects seen
        for the first time) pulls r above 1/2: measured 0.65 in the frontend. Contact sheets: <variant>/sheets/*.png
    R1  (t120off: the same screenshots with [render] interpolation = off) r median >= 0.9 - the metric sees the
        interpolation
    R2  every frame of t120 without a step (S[G]) has the same picture in t120off within +-8 frames (the loading phase
        shifts the frontend by a few frames from run to run; at most 0.1 % of the pixels may differ: content that follows
        the real time, a few pixels in the steady frontend), the interpolated frames have none
  c60
    Z1  m3_check M3-1..M3-4 with the xemu comparison, M3b-5/6/7, and the interpolation inactive: "1 frame per tick
        (... M3b)", "0 recorded", no "interpolation: active" line, "repeated pictures 0", frame fixes not called
  c60off
    Z2  against the 60 fps runs with the interpolation on (c60, ll60off): legal screen and tech notice bit-identical at
        the same frame, the loading screen (animated can) and the frontend bit-identical to a frame within +-6 frames
        (a loading hitch makes the clock catch up at another frame from run to run, which shifts the animations)
  ll60off
    L1  just-in-time start (M3c review finding 6): c60 (low_latency on) frame start -> visible median <= 8 ms and at
        least 5 ms below ll60off, both >= 99 % of the vblank-coupled frames on plan, just-in-time late frames <= 1 %;
        ENVIRONMENT as D1 (both runs)

Usage (paths with spaces belong in the INI, not on the command line):
    python tools/m3d_check.py --exe build/m3d/bin/futurama.exe --config run/m3d/futurama.ini
                              [--variants i120,d120,f120,t120,t120off,t120x,c60,ll60off,c60off] [--fresh]
                              [--display-optional]
                              [--env-retries N] [--min-refresh 120] [--xemu-ref DIR | --no-xemu-ref]
    python tools/m3d_check.py --analyze <variant dir>   (D7-D9 again on an existing t120 run)
The display criteria need the game window in front: run the check through tools/foreground_hold.py
(python tools/foreground_hold.py -- python tools/m3d_check.py ...) and do not use the computer meanwhile.

Exit code 0 when every criterion of every selected variant passes. Needs Pillow and numpy. Output:
<userdata>/m3d_check/<variant>/ (INI, run.log, screenshots, sheets) and <userdata>/m3d_check/report.txt.
"""
import argparse
import csv
import math
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m2_check  # noqa: E402  (configuration parsing, the M2 criteria)
import m3_check  # noqa: E402  (scenes, debug layer, frame rate, renderer criteria, xemu comparison)

try:
    import numpy as np
    from PIL import Image, ImageDraw, ImageStat
except ImportError:  # checked in main()
    np = None
    Image = None

TICK_HZ = 60
CAP_HZ = 120.0                 # platform/pacer_policy.h kMaxFrameRate
INTEGER_TOLERANCE = 0.002      # pacer_policy.h kIntegerTolerance
MAX_SYNC_INTERVAL = 4
SCENES_60 = {"legal": 3.0, "tech_notice": 8.0, "loading": 13.6, "frontend": 20.0}   # m3_check
SCENES_120 = {"legal": 3.0, "tech_notice": 8.0, "loading": 12.3, "frontend": 20.0}  # loading is frame-bound
STATIC_SCENES = ("legal", "tech_notice")  # fully static: bit-identical between runs
DEV_RATE_WARN = re.compile(r"\[render\] frame_rate = \d+ differs from \[game\] tick_rate")
# t120: the frontend fly-in after Intro -> Frontend (CApp::Frame ~1564 at 120 fps on the timer) and a steady
# stretch of the frontend (seconds, x 120).
T120_FLYIN = (13.0, 14.9)
T120_STEADY = (21.6, 22.0)

VARIANTS = {
    "i120": {"render": {"frame_rate": "120", "pacing": "auto", "low_latency": "on", "interpolation": "on"},
             "frames": 3600, "kind": "display", "mode": "120"},
    "d120": {"render": {"frame_rate": "display", "pacing": "auto", "low_latency": "on", "interpolation": "on"},
             "frames": 2700, "kind": "display", "mode": "display"},
    # Borderless fullscreen (M3d: the swap chain covers the monitor, the present pass draws the 4:3 picture with
    # bars): a desktop capture at 20 s (F1).
    "f120": {"render": {"frame_rate": "120", "pacing": "auto", "low_latency": "on", "interpolation": "on",
                        "fullscreen": "1"},
             "frames": 2700, "kind": "display", "mode": "120", "desktop": 20.0},
    # Screenshots of every frame of the fly-in and the steady stretch, the per-frame counters (interp_trace.csv):
    # cheap enough for an undisturbed 1/0 step pattern (D2-D7, D9).
    "t120": {"render": {"frame_rate": "0x78", "pacing": "timer", "interpolation": "on",
                        "interpolation_debug": "trace"},
             "frames": 3200, "kind": "motion", "mode": "fixed", "shots": True},
    # Reference for D9: the same screenshots with [render] interpolation = off (every tick picture shown twice): the
    # ratio must be ~1 there, so the metric of t120 really sees the interpolation.
    "t120off": {"render": {"frame_rate": "0x78", "pacing": "timer", "interpolation": "off",
                           "interpolation_debug": "off"},
                "frames": 2700, "kind": "reference", "mode": "fixed", "shots": True},
    # trace_frames over the same frames: every draw and node of every tick (D8). Writing them takes ~10 ms per frame,
    # so the clock corrects in these frames; only the transforms are judged.
    "t120x": {"render": {"frame_rate": "0x78", "pacing": "timer", "interpolation": "on",
                         "interpolation_debug": "trace"},
              "frames": 3000, "kind": "transforms", "mode": "fixed", "debug_layer": "off", "traces": True},
    "c60": {"render": {"frame_rate": "60", "pacing": "auto", "low_latency": "on", "interpolation": "on"},
            "frames": 3000, "kind": "classic", "mode": "60"},
    "ll60off": {"render": {"frame_rate": "60", "pacing": "auto", "low_latency": "off", "interpolation": "on"},
                "frames": 2400, "kind": "latency", "mode": "60"},
    "c60off": {"render": {"frame_rate": "60", "pacing": "auto", "low_latency": "on", "interpolation": "off"},
               "hooks": {"disable": "probe:NiAVObject::Destroy"}, "frames": 1300, "kind": "identity", "mode": "60"},
}


# ---- helpers ------------------------------------------------------------------------------------------------------

def first(log: str, rx: str):
    return re.search(rx, log, re.M)


def expected_rate(mode: str, refresh: float) -> tuple[float, int]:
    """(frames per second, vblank k; 0 = no vblank plan) like pacing::ChooseFrameRate / SelectPlanM3d."""
    if mode == "60":
        return 60.0, 0  # M3b selection (m3_check judges it)
    if mode == "fixed":
        return 120.0, 0
    if not refresh or refresh <= 0:
        return CAP_HZ, 0
    if refresh <= CAP_HZ * (1 + INTEGER_TOLERANCE):
        return refresh, 1
    k = round(refresh / CAP_HZ)
    if 1 <= k <= MAX_SYNC_INTERVAL and abs(refresh / k - CAP_HZ) <= INTEGER_TOLERANCE * CAP_HZ:
        return refresh / k, k
    if mode == "display":
        k = math.ceil(refresh / (CAP_HZ * (1 + INTEGER_TOLERANCE)))
        if 2 <= k <= MAX_SYNC_INTERVAL:
            return refresh / k, k
    return CAP_HZ, 0  # VRR timer or cadence


def frames_per_tick(fps: float) -> int:
    n = round(fps / TICK_HZ)
    return n if n >= 1 and abs(fps - n * TICK_HZ) <= 1e-4 * fps else 0


def write_variant_ini(base: Path, name: str, spec: dict, root: Path, frames: int, shots: list[int],
                      traces: list[int]) -> Path:
    cp = m2_check.read_config(base)
    vdir = root / name
    vdir.mkdir(parents=True, exist_ok=True)
    for section in ("paths", "debug", "render"):
        if not cp.has_section(section):
            cp.add_section(section)
    cp.set("paths", "userdata", str(vdir))
    cp.set("debug", "frames", str(frames))
    cp.set("debug", "timeout", str(frames // 60 + 120))
    cp.remove_option("debug", "stop_at")
    for key in ("trace_frames", "screenshot_frames", "pacing_trace", "interpolation_debug"):
        cp.remove_option("render", key)
    for key, value in spec["render"].items():
        cp.set("render", key, value)
    cp.set("render", "backend", "d3d11")
    cp.set("render", "debug_layer", spec.get("debug_layer", "on"))
    cp.set("render", "pacing_trace", str(vdir / "pacing_trace.csv"))
    cp.set("render", "screenshot_frames", ", ".join(str(f) for f in shots))
    if traces:
        cp.set("render", "trace_frames", ", ".join(str(f) for f in traces))
    for key, value in spec.get("hooks", {}).items():
        if not cp.has_section("hooks"):
            cp.add_section("hooks")
        cp.set("hooks", key, value)
    ini = vdir / "futurama.ini"
    with ini.open("w", encoding="utf-8") as f:
        f.write(f"; generated by tools/m3d_check.py from {base} - variant {name}\n")
        cp.write(f)
    return ini


def grab_desktop(path: Path, delay: float) -> None:
    """Capture of the primary monitor in physical pixels after `delay` seconds (thread; F1)."""
    time.sleep(delay)
    try:
        import ctypes
        from PIL import ImageGrab
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # this process only: physical pixels
    except (ImportError, OSError, AttributeError):
        pass
    try:
        ImageGrab.grab().save(path)
    except Exception as e:  # noqa: BLE001 - reported by the criterion as a missing capture
        path.with_suffix(".txt").write_text(f"capture failed: {e}\n", encoding="utf-8")


def run_variant(exe: Path, ini: Path, userdata: Path, frames: int, fps: float, desktop: float = 0.0):
    for sub in ("hdd", "cache", "logs", "screenshots", "m3_check", "sheets"):
        shutil.rmtree(userdata / sub, ignore_errors=True)
    for name in ("pacing_trace.csv", "desktop.png", "desktop.txt"):
        (userdata / name).unlink(missing_ok=True)
    timeout = int(frames / max(fps, 30.0)) + 120
    start = time.monotonic()
    grabber = None
    if desktop > 0.0:
        import threading
        grabber = threading.Thread(target=grab_desktop, args=(userdata / "desktop.png", desktop), daemon=True)
        grabber.start()
    try:
        # M6: [debug] movies = skip keeps the pre-M6 timeline (Intro -> Frontend at ~15 s) the variants assume.
        proc = subprocess.run([str(exe), f"--config={ini}", "--selftest", "--trace=vfs", "--set=debug.movies=skip"],
                              cwd=userdata,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=timeout)
        code, stderr = proc.returncode, proc.stderr.decode("utf-8", errors="replace").strip()
    except subprocess.TimeoutExpired:
        code, stderr = -1, f"killed after {timeout} s (hang)"
    if grabber is not None:
        grabber.join(timeout=5)
    elapsed = time.monotonic() - start
    log_path = userdata / "logs" / "futurama.log"
    trace_path = userdata / "logs" / "vfs_trace.log"
    log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
    vfs = trace_path.read_text(encoding="utf-8", errors="replace") if trace_path.exists() else ""
    (userdata / "run.log").write_text(log, encoding="utf-8")
    return code, stderr, log, vfs, elapsed, timeout


def display_info(log: str) -> dict:
    """Refresh, foreground share, plan and on-screen statistics of the present lines."""
    info: dict = {}
    d = first(log, r"present: display (.+?) = ([\d.]+) Hz")
    info["refresh"] = float(d.group(2)) if d else 0.0
    info["display"] = d.group(1) if d else "?"
    env = first(log, r"present: environment: foreground (\d+) frames \(([\d.]+) %\)")
    info["foreground"] = float(env.group(2)) if env else 0.0
    info["foreground_frames"] = int(env.group(1)) if env else 0
    mode = first(log, r"present: mode (\w+) \(tick (\d+) Hz, target ([\d.]+) Hz\), plan (.+?); frames per plan: (.+?); "
                      r"(\d+) plan changes")
    info["plan"] = mode.group(4) if mode else "?"
    info["plan_frames"] = m3_check.plan_frames(mode.group(5)) if mode else {}
    present = first(log, r"present: (\d+) frames in ([\d.]+) s \(average ([\d.]+) Hz")
    info["average"] = float(present.group(3)) if present else 0.0
    screen = first(log, r"present: on screen \(DXGI frame statistics\): (.+)$")
    vb = re.search(r"vblank-coupled (\d+): (\d+) on plan \(([\d.]+) %\), (\d+) late, (\d+) early",
                   screen.group(1)) if screen else None
    info["on_plan"] = (int(vb.group(1)), float(vb.group(3)), int(vb.group(4)), int(vb.group(5))) if vb else None
    lat = re.search(r"present to visible median ([\d.]+) ms, 99th percentile ([\d.]+) ms", screen.group(1)) \
        if screen else None
    info["present_to_visible"] = (float(lat.group(1)), float(lat.group(2))) if lat else None
    intervals = first(log, r"present: frame start intervals \(planned period\): .+ -> ([\d.]+) % on period")
    info["on_period"] = float(intervals.group(1)) if intervals else 0.0
    rate = first(log, r"present: frame rate (\w+) \(plan ([\d.]+) Hz, (\d+) frames? per tick\); cadence frames "
                      r"1:(\d+) 2:(\d+) 3\+:(\d+); repeated pictures (\d+) \(of (\d+) frames without a fixed step\); "
                      r"just-in-time start (on|off): (\d+) of (\d+) vblank frames \(average delay ([\d.]+) ms, budget "
                      r"([\d.]+) ms, (\d+) late[;)]")
    if rate:
        info.update(rate_mode=rate.group(1), plan_hz=float(rate.group(2)), plan_n=int(rate.group(3)),
                    repeated=int(rate.group(7)), tickless=int(rate.group(8)), jit=rate.group(9),
                    jit_frames=int(rate.group(10)), jit_vblank=int(rate.group(11)), jit_delay=float(rate.group(12)),
                    jit_budget=float(rate.group(13)), jit_late=int(rate.group(14)))
    return info


def judge_display(info: dict, fps: float, k: int, env_ok: bool) -> tuple[bool, str]:
    """D1 / M3b-7: plan, rate and on-screen statistics. In the background the pacer runs "free" on its timer by design
    (M3b), so the share of the vblank plan is taken over the frames with the window in front."""
    plan_frames = info.get("plan_frames", {})
    total = min(sum(plan_frames.values()), max(1, info.get("foreground_frames", 0)))
    vshare = min(1.0, plan_frames.get("vblank", 0) / total) if total else 0.0
    on_plan = info.get("on_plan")
    detail = (f"display {info.get('display')} = {info.get('refresh', 0.0):.3f} Hz -> expected {fps:.3f} fps"
              + (f", vblank k={k}" if k else ", no vblank plan (VRR timer / cadence / 60 Hz M3b)")
              + f"; frames per plan {plan_frames}; vblank plan {vshare * 100:.1f} % of the {total} foreground frames; "
              f"average {info.get('average', 0):.2f} Hz; frame starts {info.get('on_period', 0):.2f} % on period; on "
              f"screen (vblank-coupled frames, % exactly k refreshes, late, early) {on_plan}; foreground "
              f"{info.get('foreground', 0):.1f} %; frame rate line {info.get('rate_mode')}, plan {info.get('plan_hz')} "
              f"Hz, {info.get('plan_n')} per tick; present -> visible {info.get('present_to_visible')} ms")
    ok = env_ok and info.get("average", 0) >= 0.995 * fps and abs(info.get("plan_hz", 0) - fps) <= 0.01 * fps
    if k:
        # Vblank plans: the frame starts follow the waitable (and the just-in-time start), whose release jitters by
        # up to ~0.7 ms in a composed window; what counts is the on-screen statistic (exactly k refreshes).
        ok = ok and vshare >= 0.95 and on_plan is not None and on_plan[0] >= 1000 and on_plan[1] >= 99.0
    else:
        ok = ok and info.get("on_period", 0) >= 95.0
    return ok, detail


def check_fullscreen(log: str, capture: Path, env_ok) -> tuple:
    """F1: in borderless fullscreen the view (swap chain) covers the monitor and the present pass draws the 4:3 picture
    centred with black bars (desktop capture of the primary monitor in physical pixels)."""
    name = "F1 borderless fullscreen: view = monitor, 4:3 picture with black bars (desktop capture)"
    view = first(log, r"window: frame [0-9A-Fa-fx]+, view [0-9A-Fa-fx]+ (\d+)x(\d+) \(.*borderless fullscreen")
    if view is None:
        return name, False, "no 'window: ... borderless fullscreen' line"
    vw, vh = int(view.group(1)), int(view.group(2))
    if not capture.exists():
        why = capture.with_suffix(".txt")
        return name, False, f"view {vw}x{vh}; no desktop capture" + (
            f" ({why.read_text(encoding='utf-8').strip()})" if why.exists() else "")
    im = np.asarray(Image.open(capture).convert("RGB"), dtype=np.float32)
    h, w = im.shape[:2]
    pic_w = round(h * 4 / 3)
    x0 = (w - pic_w) // 2
    bars = np.concatenate([im[:, : max(0, x0 - 2)], im[:, x0 + pic_w + 2:]], axis=1)
    picture = im[:, x0 + 2: x0 + pic_w - 2]
    bar_mean = float(bars.mean()) if bars.size else 0.0
    edge_l = float(im[:, x0 + 4: x0 + 12].mean())
    pic_std = float(picture.std())
    ok = (vw, vh) == (w, h) and bar_mean < 1.0 and pic_std > 8.0 and edge_l > bar_mean + 2.0
    if env_ok is False:
        ok = False
    return name, (None if env_ok is None else ok), (
        f"view {vw}x{vh}, capture {w}x{h} (physical pixels); picture x = {x0}..{x0 + pic_w - 1}: bars mean "
        f"{bar_mean:.2f} (black), picture stddev {pic_std:.1f}, picture edge mean {edge_l:.1f}"
        + ("" if env_ok is not False else "; ENVIRONMENT INVALID (the capture may show another window)"))


MIN_FOREGROUND_FRAMES = 1500  # enough display-coupled frames when the window lost the foreground part of the time


def environment_ok(info: dict, min_refresh: float, min_foreground: float) -> tuple[bool, str]:
    """The display runs need the refresh and the window in front: >= min_foreground % of the frames, or at least
    MIN_FOREGROUND_FRAMES foreground frames (the display-coupled criteria are then judged on those frames)."""
    why = []
    if info.get("refresh", 0.0) < min_refresh - 0.5:
        why.append(f"display refresh {info.get('refresh', 0.0):.3f} Hz ({info.get('display')}) below {min_refresh:.0f}")
    if info.get("foreground", 0.0) < min_foreground and info.get("foreground_frames", 0) < MIN_FOREGROUND_FRAMES:
        why.append(f"window in the foreground for {info.get('foreground_frames', 0)} frames = "
                   f"{info.get('foreground', 0.0):.1f} % (minimum {min_foreground:.0f} % or {MIN_FOREGROUND_FRAMES} "
                   f"frames; someone used the computer?)")
    return not why, "; ".join(why)


# ---- log criteria ----------------------------------------------------------------------------------------------

def check_general(name: str, log: str, vfs: str, code: int, elapsed: float, timeout: int, frames: int, language: int,
                  userdata: Path, fps: float) -> list:
    results = []
    m2, _ = m2_check.check_run(log, vfs, code, elapsed, timeout, frames, language, userdata, max(1, round(fps)))
    # The general rule again without the developer frame-rate warning of the t120 variants (frame_rate = 0x78).
    dev_rate = VARIANTS.get(name, {}).get("mode") == "fixed"
    bad = [ln for ln in log.splitlines() if re.search(r" (ERROR|WARN) |UNIMPLEMENTED|TRAP ", ln)
           and not (dev_rate and DEV_RATE_WARN.search(ln))]
    m2 = [(n, ok, d) for n, ok, d in m2 if not n.startswith("log without")]
    m2.append(("log without error/warn/UNIMPLEMENTED/TRAP" + (" (developer frame_rate warning tolerated)"
                                                              if dev_rate else ""),
               not bad, bad[0][:200] if bad else "clean"))
    bad_m2 = [f"M2-{n.split(' ', 1)[0]}: {d}" for n, ok, d in m2 if not ok]
    limit = first(log, rf"FRAMES: limit of {frames} frames reached")
    results.append(("G1 exit 0, frame limit, every M2 criterion", code == 0 and limit is not None and not bad_m2,
                    f"exit {code}, limit {'reached' if limit else 'NOT reached'}, {elapsed:.0f} s; "
                    + ("M2 all pass" if not bad_m2 else "; ".join(bad_m2)[:600])))
    return results


def check_renderer(log: str, frames: int, fps: float, scenes: dict, shots: list[int], userdata: Path,
                   out_dir: Path, ref_dir, frontend_dirs, min_hz: float, max_work_ms: float):
    m3, info = m3_check.check_m3(log, 0, frames, scenes, shots, userdata, out_dir, 1, ref_dir, min_hz, max_work_ms,
                                 frontend_dirs, m3_check.MIN_ON_PERIOD_PERCENT)
    by = {n.split(" ", 1)[0]: (n, ok, d) for n, ok, d in m3}
    return by, info


def parse_clock(log: str) -> dict | None:
    # Since the M3d review (finding 10/11) a catch-up and the 6 dt cap are interpolated over the step pair instead of
    # snapped, and the line reports the clock input (frame start, plan-change rephase).
    m = first(log, r"game clock M3d: (.+?); (\d+) regime changes, (\d+) phase B alignments, (\d+) samples after a "
                   r"just-in-time start; (\d+) rendered frames, (\d+) between ticks enabled: alpha 1/2 x (\d+), 1 x "
                   r"(\d+), other x (\d+); snap-all: context switch (\d+), clock reset (\d+); interpolated over the "
                   r"step pair: catch-up (\d+), 6 dt cap (\d+); clock input: frame start x (\d+), plan-change rephase "
                   r"x (\d+)")
    if not m:
        return None
    g = [int(m.group(i)) for i in range(2, 16)]
    return {"regime": m.group(1), "regime_changes": g[0], "alignments": g[1], "jit_samples": g[2], "rendered": g[3],
            "enabled": g[4], "half": g[5], "one": g[6], "other": g[7], "switch": g[8], "reset": g[9],
            "catchup": g[10], "cap": g[11], "frame_starts": g[12], "rephases": g[13]}


def parse_ticks(log: str) -> dict | None:
    m = first(log, r"game ticks: (\d+) fixed steps in ([\d.]+) s .*?steady ([\d.]+) steps/s\), steps per CApp::Frame "
                   r"over (\d+) frames: 0 x (\d+), 1 x (\d+), 2 x (\d+), 3\+ x (\d+) \(max (\d+)\)")
    if not m:
        return None
    return {"steps": int(m.group(1)), "steady": float(m.group(3)), "frames": int(m.group(4)),
            "hist": [int(m.group(i)) for i in range(5, 9)], "max": int(m.group(9))}


def parse_interp(log: str) -> dict | None:
    m = first(log, r"interpolation: (\d+) frames \((\d+) recorded, (\d+) with alpha < 1\); draws (\d+): interpolated "
                   r"(\d+), static (\d+), alpha one (\d+), first sight (\d+), snapped (\d+) \(translation (\d+), rotation "
                   r"(\d+), scale (\d+), identity (\d+), destroyed (\d+), generation gap (\d+), frame-driven (\d+), "
                   r"snap-all (\d+), not orthonormal (\d+), mirror (\d+)\), not recorded (\d+)")
    if not m:
        return None
    keys = ["frames", "recorded", "below_one", "draws", "interpolated", "static", "alpha_one", "first_sight",
            "snapped", "translation", "rotation", "scale", "identity", "destroyed", "gap", "frame_driven", "snap_all",
            "not_orthonormal", "mirror", "not_recorded"]
    d = {k: int(m.group(i + 1)) for i, k in enumerate(keys)}
    cams = first(log, r"interpolation: cameras (\d+) \(interpolated (\d+), static (\d+), alpha one (\d+), first sight "
                      r"(\d+), cuts (\d+).*?skinned draws (\d+) \(substitute skins (\d+), bones interpolated (\d+), bone "
                      r"snaps (\d+)\)")
    d["cameras_line"] = cams.group(0)[len("interpolation: "):] if cams else "?"
    gen = first(log, r"interpolation: (\d+) generations \((\d+) gaps.*?overflows: table (\d+), occurrences (\d+), "
                     r"arena (\d+); skin self-test (\d+) run, (\d+) failed.*?cost average ([\d.]+) us, p99 (\d+) us, max "
                     r"([\d.]+) us")
    if gen:
        d.update(overflows=int(gen.group(3)) + int(gen.group(4)) + int(gen.group(5)), skin_tests=int(gen.group(6)),
                 skin_failed=int(gen.group(7)), cost=(float(gen.group(8)), int(gen.group(9)), float(gen.group(10))))
    hier = first(log, r"interpolation: hierarchy: (.+)$")
    d["hierarchy_line"] = hier.group(1) if hier else "?"
    active = first(log, r"interpolation: active from frame (\d+)")
    d["active_from"] = int(active.group(1)) if active else None
    unchanged = first(log, r"with neither = the back buffer of two presents ago, (\d+) of them showing the previous "
                           r"picture again")
    d["renderer_repeats"] = int(unchanged.group(1)) if unchanged else None
    return d


def check_120(name: str, log: str) -> list:
    """D2-D4 and the repeat count of D5 (log part)."""
    results = []
    ticks, clock, interp = parse_ticks(log), parse_clock(log), parse_interp(log)
    # D2
    if ticks is None or clock is None:
        results.append(("D2 logic at 60 Hz, one fixed step per frame pair", False, "no 'game ticks' / 'game clock M3d'"))
    else:
        n = max(1, ticks["frames"])
        h = ticks["hist"]
        s0, s1, s2 = h[0] / n, h[1] / n, (h[2] + h[3]) / n
        ok = (abs(ticks["steady"] - TICK_HZ) <= TICK_HZ * 0.002 and clock["regime"].startswith("2 frames per tick")
              and abs(s0 - 0.5) <= 0.02 and abs(s1 - 0.5) <= 0.02 and s2 <= 0.01)
        results.append(("D2 logic at 60 Hz, one fixed step per frame pair", ok,
                        f"steady {ticks['steady']:.3f} steps/s (60 +- 0.12); clock '{clock['regime']}'; steps per "
                        f"frame over {n} frames: 0 x {h[0]} ({s0 * 100:.2f} %), 1 x {h[1]} ({s1 * 100:.2f} %), 2+ x "
                        f"{h[2] + h[3]} ({s2 * 100:.2f} %, max {ticks['max']})"))
    # D3
    if clock is None:
        results.append(("D3 alpha 1/2 and 1 alternate", False, "no 'game clock M3d' line"))
    else:
        n = max(1, clock["enabled"])
        snaps = clock["switch"] + clock["catchup"] + clock["cap"] + clock["reset"]
        ok = (abs(clock["half"] / n - 0.5) <= 0.02 and abs(clock["one"] / n - 0.5) <= 0.02
              and clock["other"] / n <= 0.005 and snaps / max(1, clock["rendered"]) <= 0.01)
        results.append(("D3 alpha 1/2 and 1 alternate", ok,
                        f"{clock['enabled']} frames between ticks: alpha 1/2 x {clock['half']} "
                        f"({clock['half'] / n * 100:.2f} %), 1 x {clock['one']} ({clock['one'] / n * 100:.2f} %), other "
                        f"x {clock['other']}; snap-all: context switch {clock['switch']}, clock reset "
                        f"{clock['reset']}; step pairs: catch-up {clock['catchup']}, 6 dt cap {clock['cap']} ({snaps} "
                        f"events = {snaps / max(1, clock['rendered']) * 100:.2f} % of {clock['rendered']} frames); "
                        f"{clock['alignments']} phase B alignments, {clock['regime_changes']} regime changes, "
                        f"{clock['jit_samples']} just-in-time samples, clock input frame start x "
                        f"{clock['frame_starts']}, rephase x {clock['rephases']}"))
    # D4
    if interp is None:
        results.append(("D4 interpolation of the moving draws", False, "no 'interpolation:' exit lines"))
    else:
        moving_snaps = (interp["translation"] + interp["rotation"] + interp["scale"] + interp["frame_driven"]
                        + interp["destroyed"] + interp["gap"])
        moving = interp["interpolated"] + moving_snaps
        share = interp["interpolated"] / moving if moving else 0.0
        bad = []
        if interp["active_from"] is None:
            bad.append("no 'interpolation: active from frame' line")
        if share < 0.98:
            bad.append(f"only {share * 100:.2f} % of the moving draws interpolated")
        for key in ("identity", "not_orthonormal", "mirror", "not_recorded"):
            if interp[key]:
                bad.append(f"{key.replace('_', ' ')} {interp[key]}")
        if interp.get("overflows"):
            bad.append(f"{interp['overflows']} overflows")
        if interp.get("skin_failed"):
            bad.append(f"skin self-test {interp['skin_failed']} failed")
        cost = interp.get("cost", (0.0, 0, 0.0))
        results.append(("D4 interpolation of the moving draws", not bad,
                        ("; ".join(bad) + "; " if bad else "")
                        + f"active from frame {interp['active_from']}; {interp['recorded']} recorded frames, "
                        f"{interp['below_one']} with alpha < 1; draws {interp['draws']}: interpolated "
                        f"{interp['interpolated']} = {share * 100:.2f} % of {moving} moving (snaps: translation "
                        f"{interp['translation']}, rotation {interp['rotation']}, scale {interp['scale']}, frame-driven "
                        f"{interp['frame_driven']}, destroyed {interp['destroyed']}, generation gap {interp['gap']}); "
                        f"static {interp['static']}, alpha one {interp['alpha_one']}, first sight "
                        f"{interp['first_sight']}, snap-all {interp['snap_all']}; {interp['cameras_line']}; hierarchy: "
                        f"{interp['hierarchy_line']}; skin self-test {interp.get('skin_tests', '?')} run; cost average "
                        f"{cost[0]:.0f} us, p99 {cost[1]} us, max {cost[2]:.0f} us per recorded frame"))
    return results


def check_repeats(log: str, info: dict, shots_dir: Path, pairs: dict) -> tuple:
    """D5: repeated pictures counted and presented, no flicker in the static intro scenes (screenshot pairs)."""
    interp = parse_interp(log) or {}
    repeated = info.get("repeated")
    renderer = interp.get("renderer_repeats")
    problems, detail = [], []
    if repeated is None:
        problems.append("no 'present: frame rate' line")
    else:
        detail.append(f"repeated pictures {repeated} of {info.get('tickless')} frames without a fixed step, renderer "
                      f"presented {renderer} again")
        if repeated == 0:
            problems.append("no repeated picture (intro frames between ticks?)")
        if renderer != repeated:
            problems.append(f"pacer {repeated} vs renderer {renderer} repeated pictures")
    for scene, (a, b) in pairs.items():
        if scene not in ("legal", "tech_notice"):
            continue
        pa, pb = shot_path(log, a), shot_path(log, b)
        if pa is None or pb is None or not pa.exists() or not pb.exists():
            problems.append(f"{scene}: screenshots of frames {a}/{b} missing")
            continue
        ia, ib = Image.open(pa).convert("RGB"), Image.open(pb).convert("RGB")
        diff = m3_check.mean_diff(ia, ib)
        luma = ImageStat.Stat(ib.convert("L")).mean[0]
        detail.append(f"{scene} frames {a}/{b}: mean difference {diff:.2f}, luminance {luma:.1f}")
        if diff > 0.5:
            problems.append(f"{scene}: frames {a}/{b} differ by {diff:.2f} (flicker)")
    return ("D5 repeated picture: no stale or black frame between ticks", not problems,
            "; ".join(problems + detail))


def shot_path(log: str, frame: int) -> Path | None:
    m = re.search(rf"screenshot: (.*frame_{frame:06d}\.png) \(\d+x\d+\)", log)
    return Path(m.group(1)) if m else None


def check_scene_pairs(log: str, pairs: dict, ref_dir, frontend_dirs) -> tuple:
    """D6 part: the second frame of each scene pair compared with xemu as well."""
    if ref_dir is None:
        return None
    cache: dict = {}
    problems, detail = [], []
    for scene, (_, b) in pairs.items():
        p = shot_path(log, b)
        if p is None or not p.exists():
            problems.append(f"{scene}: no screenshot of frame {b}")
            continue
        ok, text = m3_check.compare_xemu(scene, p, ref_dir, cache, frontend_dirs)
        detail.append(f"{scene} frame {b}: {text}")
        if ok is False:
            problems.append(f"{scene} frame {b}: {text}")
    return not problems, "; ".join(problems + detail)


# ---- pacing trace -----------------------------------------------------------------------------------------------

def read_pacing_trace(path: Path) -> tuple[list[dict], int]:
    if not path.exists():
        return [], 0
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    qpf = 10_000_000
    if lines and lines[0].startswith("#"):
        m = re.search(r"qpf=(\d+)", lines[0])
        qpf = int(m.group(1)) if m else qpf
    rows = list(csv.DictReader(ln for ln in lines if not ln.startswith("#")))
    return rows, qpf


def start_to_visible(rows: list[dict], qpf: int) -> list[float]:
    """Frame start -> the vblank at which its present became visible (ms), from the DXGI statistics in the trace."""
    start_of = {}
    for r in rows:
        if r.get("presented") == "1" and int(r.get("present_id") or 0):
            start_of[int(r["present_id"])] = int(r["frame_start_qpc"])
    visible = {}
    for r in rows:
        if r.get("stats_valid") == "1" and r.get("foreground") == "1":
            visible.setdefault(int(r["stat_present_count"]), int(r["stat_sync_qpc"]))
    out = []
    for pid, sync in visible.items():
        st = start_of.get(pid)
        if st and sync > st:
            ms = (sync - st) * 1000.0 / qpf
            if ms < 100.0:
                out.append(ms)
    return out


def pct(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    v = sorted(values)
    return v[min(len(v) - 1, int(q * len(v)))]


# ---- t120: alpha sequence --------------------------------------------------------------------------------------

def read_interp_trace(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with path.open(encoding="utf-8", errors="replace") as f:
        reader = csv.reader(f)
        header = next(reader, [])
        idx = {name: i for i, name in enumerate(header) if name not in ("",)}
        rows = []
        for cols in reader:
            try:
                rows.append({"frame": int(cols[idx["frame"]]), "tick": int(cols[idx["tick"]]),
                             "steps": int(cols[idx["steps"]]), "alpha": float(cols[idx["alpha"]]),
                             "flags": int(cols[idx["flags"]]), "draws": int(cols[idx["draws"]]),
                             "interpolated": int(cols[header.index("interpolated")])})
            except (ValueError, IndexError, KeyError):
                continue
    return rows


def check_alpha_sequence(rows: list[dict], active_from: int | None) -> tuple:
    """D7: displayed time G - 1 + alpha grows by 1/2 per frame in the frontend."""
    name = "D7 displayed time grows by half a tick per frame (interp_trace.csv)"
    if not rows:
        return name, False, "logs/interp_trace.csv missing or empty"
    start = active_from or 0
    sel = [r for r in rows if r["frame"] >= start and not (r["flags"] & 2)]
    by = {r["frame"]: r for r in sel}
    # A context switch loads the next context: the guest runs no step for a while (hold) and catches up in the
    # flagged snap-all frame; the 40 frames before every flagged frame are counted apart.
    switch_window = {f for r in sel if r["flags"] & 5 for f in range(r["frame"] - 40, r["frame"] + 1)}
    steps_ok = flagged = corrections = backwards = in_switch = 0
    bad_frames = []
    for r in sel:
        p = by.get(r["frame"] - 1)
        if p is None:
            continue
        if r["frame"] in switch_window:
            in_switch += 1
            continue
        t, tp = r["tick"] - 1 + r["alpha"], p["tick"] - 1 + p["alpha"]
        d = t - tp
        if abs(d - 0.5) <= 1e-3:
            steps_ok += 1
        elif r["flags"] & 5 or p["flags"] & 5:
            flagged += 1  # snap-all / clock reset: the frame shows S[G] (context switch, 6 dt cap)
        elif d > -1e-3 and abs(d * 2 - round(d * 2)) <= 2e-3:
            corrections += 1  # hold (+0) or catch-up (+1, +1.5, ...) of the clock in whole quanta
        else:
            backwards += 1  # the displayed time went back or off the half-tick grid: visible judder
            if len(bad_frames) < 8:
                bad_frames.append(f"{r['frame']}: tick {p['tick']}->{r['tick']}, alpha {p['alpha']}->{r['alpha']}, "
                                  f"steps {r['steps']}")
    total = steps_ok + flagged + corrections + backwards
    share = steps_ok / total if total else 0.0
    # Frames without a step and alpha < 1 outside a hold (the previous frame showed a later state): stepping back.
    wrong = [r for r in sel if r["steps"] == 0 and abs(r["alpha"] - 1.0) > 1e-3 and not r["flags"] & 5
             and r["frame"] not in switch_window and (p := by.get(r["frame"] - 1)) is not None
             and r["tick"] - 1 + r["alpha"] < p["tick"] - 1 + p["alpha"] - 1e-3]
    ok = total > 0 and share >= 0.99 and backwards == 0 and not wrong
    return name, ok, (f"{total} consecutive frames from frame {start} (outside the step loop, without {in_switch} "
                      f"frames of context-switch loading): +1/2 x {steps_ok} ({share * 100:.2f} %, minimum 99), at "
                      f"snap-all/reset frames x {flagged}, clock corrections in whole half ticks (hold, catch-up) x "
                      f"{corrections}, backwards or off the grid x {backwards}"
                      + (f" ({'; '.join(bad_frames)})" if bad_frames else "")
                      + f"; frames without a step showing an earlier state than the frame before: {len(wrong)}")


# ---- t120: transforms ------------------------------------------------------------------------------------------

CAMERA_LINE = re.compile(r"interp camera frame (\d+): (0x[0-9A-F]+) location (\S+) (\S+) (\S+), direction (\S+) (\S+) "
                         r"(\S+), up (\S+) (\S+) (\S+), right (\S+) (\S+) (\S+), frustum (\S+) (\S+) (\S+) (\S+) (\S+) "
                         r"(\S+)")


class Xf:
    """NiTransform with row vectors (v' = s v R + t), gfx/interp_math.h conventions."""

    def __init__(self, r, t, s):
        self.r, self.t, self.s = r, t, s

    def apply(self, v):
        return self.s * (v @ self.r) + self.t


def compose(a: Xf, b: Xf) -> Xf:  # a(b(v))
    return Xf(b.r @ a.r, a.s * (b.t @ a.r) + a.t, a.s * b.s)


def inverse(a: Xf) -> Xf:
    rt = a.r.T
    return Xf(rt, -(a.t @ rt) / a.s, 1.0 / a.s)


def quat(m) -> "np.ndarray":
    tr = m[0, 0] + m[1, 1] + m[2, 2]
    if tr > 0:
        s = math.sqrt(tr + 1) * 2
        q = [0.25 * s, (m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(max(1 + m[0, 0] - m[1, 1] - m[2, 2], 0)) * 2
        q = [(m[2, 1] - m[1, 2]) / s, 0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s]
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(max(1 + m[1, 1] - m[0, 0] - m[2, 2], 0)) * 2
        q = [(m[0, 2] - m[2, 0]) / s, (m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s]
    else:
        s = math.sqrt(max(1 + m[2, 2] - m[0, 0] - m[1, 1], 0)) * 2
        q = [(m[1, 0] - m[0, 1]) / s, (m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s]
    q = np.array(q)
    return q / np.linalg.norm(q)


def mat(q) -> "np.ndarray":
    w, x, y, z = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                     [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                     [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])


def slerp(a, b, t):
    if a @ b < 0:
        b = -b
    d = min(float(a @ b), 1.0)
    if d > 0.9995:
        r = a * (1 - t) + b * t
    else:
        th = math.acos(d)
        r = (math.sin((1 - t) * th) * a + math.sin(t * th) * b) / math.sin(th)
    return r / np.linalg.norm(r)


def blend(a: Xf, b: Xf, t: float) -> Xf:
    return Xf(mat(slerp(quat(a.r), quat(b.r), t)), a.t + (b.t - a.t) * t, a.s + (b.s - a.s) * t)


def angle_deg(a: Xf, b: Xf) -> float:
    return math.degrees(2 * math.acos(min(1.0, abs(float(quat(a.r) @ quat(b.r))))))


def project(cam, p):
    loc, d, u, r = np.array(cam[0:3]), np.array(cam[3:6]), np.array(cam[6:9]), np.array(cam[9:12])
    left, right, top, bottom = cam[12:16]
    q = p - loc
    x, y, z = q @ d, q @ u, q @ r
    if x <= 1e-3:
        return None
    return np.array([(z / x - left) / (right - left) * 640.0, (top - y / x) / (top - bottom) * 480.0])


def hier_interp(key, a: dict, b: dict, t: float, cache: dict):
    """interp(S_a, S_b, t) of node `key` along its parent chain (the topmost node with history in world space)."""
    if key in cache:
        return cache[key]
    na, nb = a.get(key), b.get(key)
    if na is None or nb is None or na[0] != nb[0]:
        cache[key] = None
        return None
    pk = (na[0], key[1])
    res = None
    if na[0] != 0 and pk in a and pk in b:
        pm = hier_interp(pk, a, b, t, cache)
        if pm is not None:
            la = compose(inverse(a[pk][1]), na[1])
            lb = compose(inverse(b[pk][1]), nb[1])
            res = compose(pm, blend(la, lb, t))
    if res is None:
        res = blend(na[1], nb[1], t)
    cache[key] = res
    return res


def check_transforms(logdir: Path, log: str, max_midpoint_px: float) -> list:
    draws_path, nodes_path = logdir / "interp_draws.csv", logdir / "interp_nodes.csv"
    name = "D8 transforms: S[G] at alpha 1, lerp / parent chain at 1/2, between the neighbours, midpoint test"
    if not draws_path.exists():
        return [(name, False, "logs/interp_draws.csv missing ([render] interpolation_debug = trace, trace_frames)")]
    ticks: dict = {}
    tick_frame: dict = {}
    if nodes_path.exists():
        with nodes_path.open(encoding="utf-8") as f:
            for row in csv.DictReader(f):
                g = int(row["tick"])
                r = np.array([[float(row[f"r{i}{j}"]) for j in range(3)] for i in range(3)])
                tr = np.array([float(row["tx"]), float(row["ty"]), float(row["tz"])])
                c = np.array([float(row["cx"]), float(row["cy"]), float(row["cz"])])
                ticks.setdefault(g, {})[(int(row["node"], 16), int(row["camera"], 16))] = (
                    int(row["parent"], 16), Xf(r, tr, float(row["scale"])), c, float(row["radius"]))
                tick_frame[g] = int(row["frame"])
    cams = {}
    for m in CAMERA_LINE.finditer(log):
        cams[(int(m.group(1)), int(m.group(2), 16))] = [float(x) for x in m.groups()[2:]]

    counts = {"alpha_one": 0, "static": 0, "world": 0, "hier": 0, "between": 0}
    worst = {"alpha_one": 0.0, "world": 0.0, "hier": 0.0, "between": 0.0}
    bad: list[str] = []
    leaves: dict = {}
    reasons: dict = {}
    with draws_path.open(encoding="utf-8") as f:
        for row in csv.DictReader(f):
            frame, alpha, reason = int(row["frame"]), float(row["alpha"]), row["reason"]
            reasons[reason] = reasons.get(reason, 0) + 1
            if row["occurrence"] == "0":
                leaves.setdefault(frame, set()).add((int(row["object"], 16), int(row["camera"], 16)))
            prev = np.array([float(row["prev_" + c]) for c in "xyz"])
            cur = np.array([float(row["cur_" + c]) for c in "xyz"])
            shown = np.array([float(row["shown_" + c]) for c in "xyz"])
            lerp = np.array([float(row["lerp_" + c]) for c in "xyz"])
            scale = max(1.0, float(np.abs(cur).max()), float(np.abs(prev).max()))
            if reason in ("alpha one", "static"):
                key = "alpha_one" if reason == "alpha one" else "static"
                counts[key] += 1
                d = float(np.abs(shown - cur).max()) / scale
                worst["alpha_one"] = max(worst["alpha_one"], d)
                if d > 2e-6 and len(bad) < 10:
                    bad.append(f"frame {frame} {row['object']} ({reason}) shows {shown} instead of S[G] {cur}")
                continue
            if reason != "interpolated":
                continue
            # (b) world interpolation: the chord.
            want = prev + (cur - prev) * alpha
            d_l = float(np.abs(lerp - want).max()) / scale
            if row["hierarchy"] == "0":
                counts["world"] += 1
                d = max(d_l, float(np.abs(shown - want).max()) / scale)
                worst["world"] = max(worst["world"], d)
                if d > 1e-5 and len(bad) < 10:
                    bad.append(f"frame {frame} {row['object']} (world) shows {shown}, lerp {want}")
            else:
                counts["hier"] += 1
                g = int(row["tick"])
                key = (int(row["object"], 16), int(row["camera"], 16))
                if g in ticks and g - 1 in ticks and abs(alpha - 0.5) < 1e-3:
                    ref = hier_interp(key, ticks[g - 1], ticks[g], 0.5, {})
                    if ref is not None:
                        d = float(np.linalg.norm(shown - ref.t)) / max(1.0, float(np.linalg.norm(ref.t)))
                        worst["hier"] = max(worst["hier"], d)
                        if d > 1e-4 and len(bad) < 10:
                            bad.append(f"frame {frame} {row['object']} (hierarchy) shows {shown}, Python {ref.t}")
            # (d) between the neighbours.
            stepv = float(np.linalg.norm(cur - prev))
            if abs(alpha - 0.5) < 1e-3:
                counts["between"] += 1
                excess = float(np.linalg.norm(shown - (prev + cur) * 0.5)) - 0.5 * stepv
                worst["between"] = max(worst["between"], excess / scale)
                if excess > 1e-5 * scale and len(bad) < 10:
                    bad.append(f"frame {frame} {row['object']}: {excess:.3g} beyond half the step {stepv:.3g}")
    results = []
    ok = (not bad and counts["alpha_one"] > 0 and counts["world"] + counts["hier"] > 0 and counts["between"] > 0)
    results.append((name.split(":")[0] + " (a-d): S[G] at alpha 1, lerp / parent chain at 1/2, between the "
                    "neighbours", ok,
                    f"alpha one {counts['alpha_one']} (max relative difference {worst['alpha_one']:.1e}), static "
                    f"{counts['static']}; interpolated world {counts['world']} (max {worst['world']:.1e} vs the lerp), "
                    f"hierarchy {counts['hier']} (max {worst['hier']:.1e} vs the Python parent-chain reference); "
                    f"between the neighbours {counts['between']} (max excess {worst['between']:.1e}); reasons "
                    + ", ".join(f"{k} {v}" for k, v in sorted(reasons.items()))
                    + (f"; PROBLEMS: {'; '.join(bad)}" if bad else "")))
    # (e) two-tick midpoint test.
    errs = {"world": [], "hierarchy": []}
    worst_rows = []
    for g in sorted(ticks):
        if g - 1 not in ticks or g - 2 not in ticks:
            continue
        sa, sm, sb = ticks[g - 2], ticks[g - 1], ticks[g]
        fm = tick_frame[g - 1]
        cache: dict = {}
        for key in leaves.get(fm, ()):
            if key not in sa or key not in sm or key not in sb:
                continue
            pa, ta, _, _ = sa[key]
            pm_, tm, cm, rm = sm[key]
            pb, tb, _, _ = sb[key]
            if not pa == pm_ == pb:
                continue
            s1, s2 = float(np.linalg.norm(tm.t - ta.t)), float(np.linalg.norm(tb.t - tm.t))
            a1, a2 = angle_deg(ta, tm), angle_deg(tm, tb)
            if s1 == 0 and s2 == 0 and a1 == 0 and a2 == 0:
                continue
            r = max(rm, 1e-3)
            if ((s1 > 2 * r and s1 > 4 * s2) or (s2 > 2 * r and s2 > 4 * s1) or (a1 > 30 and a1 > 4 * a2 + 1)
                    or (a2 > 30 and a2 > 4 * a1 + 1)):
                continue  # a jump (loop wrap): snapped, not interpolated
            cam = cams.get((fm, key[1]))
            if cam is None:
                continue
            local_c = inverse(tm).apply(cm)
            truth = project(cam, cm)
            pw = project(cam, blend(ta, tb, 0.5).apply(local_c))
            hm = hier_interp(key, sa, sb, 0.5, cache)
            ph = project(cam, hm.apply(local_c)) if hm is not None else pw
            if truth is None or pw is None or ph is None:
                continue
            ew, eh = float(np.linalg.norm(pw - truth)), float(np.linalg.norm(ph - truth))
            errs["world"].append(ew)
            errs["hierarchy"].append(eh)
            worst_rows.append((eh, fm, hex(key[0])))
    hier = errs["hierarchy"]
    ok_e = len(hier) >= 100 and pct(hier, 0.99) <= max_midpoint_px
    worst_rows.sort(reverse=True)
    results.append(("D8 (e) two-tick midpoint test against the real middle state", ok_e,
                    f"{len(hier)} moving draws (jumps excluded), error of interp(S[G-2], S[G], 1/2) against S[G-1] at "
                    f"the bound centre, px at 640x480: hierarchy (shipped) p50 {pct(hier, .5):.3f}, p90 "
                    f"{pct(hier, .9):.3f}, p99 {pct(hier, .99):.3f} (maximum {max_midpoint_px}), max "
                    f"{max(hier) if hier else 0:.2f}; world-only p99 {pct(errs['world'], .99):.3f}, max "
                    f"{max(errs['world']) if errs['world'] else 0:.2f}; largest: "
                    + ", ".join(f"{e:.2f} px frame {fr} {o}" for e, fr, o in worst_rows[:3])))
    return results


# ---- t120: screenshot triplets ----------------------------------------------------------------------------------

def trace_from_pacing(rows: list[dict]) -> list[dict]:
    """Per-frame steps from the pacing trace (column frame_ticks; same frame numbers as the screenshots) for runs
    without interp_trace.csv ([render] interpolation = off): alpha as the clock would give it (1/2 with a step)."""
    out, tick = [], 0
    for r in rows:
        steps = int(r.get("frame_ticks") or -1)
        if steps < 0:
            continue
        tick += steps
        out.append({"frame": int(r["frame"]), "tick": tick, "steps": steps, "alpha": 0.5 if steps == 1 else 1.0,
                    "flags": 0 if steps <= 1 else 1, "draws": 0, "interpolated": 0})
    return out


def triplets_of(trace: list[dict]) -> list[tuple[int, int, int]]:
    by = {r["frame"]: r for r in trace}
    out = []
    for f, r in by.items():
        p, n = by.get(f - 1), by.get(f + 1)
        if p is None or n is None:
            continue
        if (r["flags"] == 0 and abs(r["alpha"] - 0.5) < 1e-3 and r["steps"] == 1
                and p["flags"] == 0 and p["steps"] == 0 and abs(p["alpha"] - 1.0) < 1e-3 and p["tick"] == r["tick"] - 1
                and n["flags"] == 0 and n["steps"] == 0 and abs(n["alpha"] - 1.0) < 1e-3 and n["tick"] == r["tick"]):
            out.append((f - 1, f, f + 1))
    return sorted(out)


def check_triplets(log: str, trace: list[dict], sheets: Path, reference: bool = False) -> tuple:
    name = ("R1 reference without interpolation: the tick frame already shows S[G] (ratio ~1)" if reference else
            "D9 pictures: the interpolated frame lies between its neighbours (screenshot triplets)")
    trips = triplets_of(trace)
    stats = []
    for f0, f1, f2 in trips:
        paths = [shot_path(log, f) for f in (f0, f1, f2)]
        if any(p is None or not p.exists() for p in paths):
            continue
        a, b, c = (np.asarray(Image.open(p).convert("RGB"), dtype=np.float32) for p in paths)
        moving = np.abs(c - a).max(axis=2) > 12.0
        n = int(moving.sum())
        if n < 300:
            continue
        d01 = float(np.abs(b - a)[moving].mean())
        d12 = float(np.abs(c - b)[moving].mean())
        d02 = float(np.abs(c - a)[moving].mean())
        r = d01 / (d01 + d12) if d01 + d12 > 0 else 0.5
        stats.append((f0, f1, f2, n, d01, d12, d02, r, (d01 + d12) / d02 if d02 > 0 else 1.0))
    if not stats:
        return name, False, f"no usable triplet ({len(trips)} triplets by the trace; screenshots of consecutive frames?)"
    ratios = [s[7] for s in stats]
    detours = [s[8] for s in stats]
    inside = sum(1 for x in ratios if 0.15 <= x <= 0.85) / len(ratios)
    med = statistics.median(ratios)
    ok = med >= 0.9 if reference else (0.3 <= med <= 0.75 and inside >= 0.9)
    sheets.mkdir(parents=True, exist_ok=True)
    with (sheets / "triplets.tsv").open("w", encoding="utf-8") as f:
        f.write("f0\tf1\tf2\tmoving_px\td01\td12\td02\tratio\tdetour\n")
        for s in stats:
            f.write("\t".join(str(round(x, 4)) if isinstance(x, float) else str(x) for x in s) + "\n")
    make_sheets(log, stats, sheets)
    worst = sorted(stats, key=lambda s: abs(s[7] - 0.5), reverse=True)[:3]
    return name, ok, (f"{len(stats)} triplets with motion (of {len(trips)}): ratio d01/(d01+d12) median {med:.3f} "
                      + ("(minimum 0.9: no interpolation)" if reference else "(0.3..0.75)")
                      + f", {inside * 100:.1f} % within 0.15..0.85" + ("" if reference else " (minimum 90)")
                      + f"; detour (d01+d12)/d02 median "
                      f"{statistics.median(detours):.3f}, p90 {pct(detours, .9):.3f}; moving pixels median "
                      f"{statistics.median(s[3] for s in stats):.0f}; farthest from 1/2: "
                      + ", ".join(f"{s[1]} r {s[7]:.2f} ({s[3]} px)" for s in worst)
                      + f"; sheets {sheets}")


def make_sheets(log: str, stats: list, sheets: Path) -> None:
    """Contact sheets: the triplets with the most motion (crop of the moving area, x2) with the difference of the middle
    frame to the average of its neighbours, and a strip of consecutive frames of the fly-in."""
    def crop_box(mask):
        ys, xs = np.nonzero(mask)
        x0, x1, y0, y1 = int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max())
        cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
        w, h = max(120, min(320, x1 - x0 + 24)), max(90, min(240, y1 - y0 + 24))
        x0, y0 = max(0, min(640 - w, cx - w // 2)), max(0, min(480 - h, cy - h // 2))
        return x0, y0, x0 + w, y0 + h

    top = sorted(stats, key=lambda s: s[3], reverse=True)[:6]
    rows = []
    for s in top:
        ims = [Image.open(shot_path(log, f)).convert("RGB") for f in s[:3]]
        a, b, c = (np.asarray(im, dtype=np.float32) for im in ims)
        box = crop_box(np.abs(c - a).max(axis=2) > 12.0)
        diff = np.clip(np.abs(b - (a + c) * 0.5) * 3.0, 0, 255).astype(np.uint8)
        tiles = [im.crop(box) for im in ims] + [Image.fromarray(diff).crop(box)]
        w, h = (box[2] - box[0]) * 2, (box[3] - box[1]) * 2
        row = Image.new("RGB", (4 * w + 30, h + 16), (40, 40, 40))
        draw = ImageDraw.Draw(row)
        for i, t in enumerate(tiles):
            row.paste(t.resize((w, h), Image.NEAREST), (i * (w + 10), 16))
        draw.text((2, 2), f"frames {s[0]} (S[G-1]) | {s[1]} (alpha 1/2) | {s[2]} (S[G]) | |mid - avg| x3; "
                          f"ratio {s[7]:.2f}", fill=(255, 255, 0))
        rows.append(row)
    if rows:
        width = max(r.width for r in rows)
        sheet = Image.new("RGB", (width, sum(r.height + 6 for r in rows)), (20, 20, 20))
        y = 0
        for r in rows:
            sheet.paste(r, (0, y))
            y += r.height + 6
        sheet.save(sheets / "triplets_top.png")
    # Strip of 12 consecutive frames around the triplet with the most motion (half size, full frame).
    if top:
        mid = top[0][1]
        frames = [f for f in range(mid - 6, mid + 6) if shot_path(log, f) is not None]
        tiles = [Image.open(shot_path(log, f)).convert("RGB").resize((320, 240)) for f in frames]
        strip = Image.new("RGB", (4 * 324, ((len(tiles) + 3) // 4) * 258), (20, 20, 20))
        draw = ImageDraw.Draw(strip)
        for i, (f, t) in enumerate(zip(frames, tiles)):
            x, y = (i % 4) * 324, (i // 4) * 258
            strip.paste(t, (x, y + 16))
            draw.text((x + 2, y + 2), f"frame {f}", fill=(255, 255, 0))
        strip.save(sheets / "sequence.png")


# ---- main -------------------------------------------------------------------------------------------------------

def t120_frames() -> list[int]:
    fly = list(range(round(T120_FLYIN[0] * 120), round(T120_FLYIN[1] * 120) + 1))
    steady = list(range(round(T120_STEADY[0] * 120), round(T120_STEADY[1] * 120) + 1))
    return fly + steady


def analyze_motion(vdir: Path, log: str, max_midpoint_px: float) -> list:
    """D7 + D9 (t120: screenshots) and/or D8 (t120x: draw and node traces), whatever the run has."""
    interp = parse_interp(log) or {}
    trace = read_interp_trace(vdir / "logs" / "interp_trace.csv")
    results = []
    if (vdir / "logs" / "interp_draws.csv").exists():
        results += check_transforms(vdir / "logs", log, max_midpoint_px)
    elif trace:
        results.append(check_alpha_sequence(trace, interp.get("active_from")))
        results.append(check_triplets(log, trace, vdir / "sheets"))
    else:  # interpolation off: the steps of the pacing trace classify the frames
        rows, _ = read_pacing_trace(vdir / "pacing_trace.csv")
        results.append(check_triplets(log, trace_from_pacing(rows), vdir / "sheets", reference=True))
    return results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path)
    ap.add_argument("--config", type=Path, help="base INI ([paths] iso + userdata = the root)")
    ap.add_argument("--variants", default=",".join(VARIANTS))
    ap.add_argument("--fresh", action="store_true", help="delete <userdata>/m3d_check first")
    ap.add_argument("--display-optional", action="store_true",
                    help="display-dependent criteria of an invalid environment are SKIP instead of FAIL")
    ap.add_argument("--env-retries", type=int, default=0, help="repeat a display run whose environment was invalid")
    ap.add_argument("--min-refresh", type=float, default=120.0)
    ap.add_argument("--min-foreground", type=float, default=95.0)
    ap.add_argument("--max-midpoint-px", type=float, default=1.0)
    ap.add_argument("--xemu-ref", type=Path)
    ap.add_argument("--no-xemu-ref", action="store_true")
    ap.add_argument("--analyze", type=Path, help="only D7-D9 on an existing t120 variant directory")
    args = ap.parse_args()
    if Image is None or np is None:
        sys.exit("m3d_check: Pillow and numpy are required")
    if args.analyze:
        vdir = args.analyze.resolve()
        log = (vdir / "logs" / "futurama.log").read_text(encoding="utf-8", errors="replace")
        ok_all = True
        for name, ok, detail in analyze_motion(vdir, log, args.max_midpoint_px):
            ok_all &= ok
            print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        return 0 if ok_all else 1
    if args.exe is None or args.config is None:
        sys.exit("m3d_check: --exe and --config are required")

    base = args.config.resolve()
    cp = m2_check.read_config(base)
    ud = m2_check.ini_value(cp, "paths", "userdata")
    if not ud:
        sys.exit("m3d_check: [paths] userdata must be set in the base config")
    root = (Path(ud) if Path(ud).is_absolute() else base.parent / ud) / "m3d_check"
    if args.fresh:
        shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True, exist_ok=True)
    selected = [v.strip() for v in args.variants.split(",") if v.strip()]
    for v in selected:
        if v not in VARIANTS:
            sys.exit(f"m3d_check: unknown variant {v!r} (known: {', '.join(VARIANTS)})")
    language = m2_check.expected_language(cp)
    repo = Path(__file__).resolve().parent.parent
    ref_dir = None
    if not args.no_xemu_ref:
        ref_dir = args.xemu_ref or (repo / "run" / "xemu_ref")
        if not ref_dir.is_dir():
            print(f"m3d_check: no xemu reference captures at {ref_dir} - xemu comparisons skipped")
            ref_dir = None
    frontend_dirs = [repo / "run" / "xemu_ref_steam"] if (repo / "run" / "xemu_ref_steam").is_dir() else []
    print(f"m3d_check: variants {', '.join(selected)}; environment: refresh >= {args.min_refresh:.0f} Hz, foreground "
          f">= {args.min_foreground:.0f} % ({'SKIP' if args.display_optional else 'FAIL'} otherwise, "
          f"{args.env_retries} retries); xemu {ref_dir or 'none'}; output {root}")

    report: list[str] = []
    all_ok = True
    runs: dict = {}

    def emit(line: str) -> None:
        print(line)
        report.append(line)

    for name in selected:
        spec = VARIANTS[name]
        kind = spec["kind"]
        fps_nominal = 120.0 if spec["mode"] in ("120", "display", "fixed") else 60.0
        scene_seconds = SCENES_120 if fps_nominal > 60 else SCENES_60
        scenes = {k: round(s * fps_nominal) for k, s in scene_seconds.items()}
        pairs = {k: (f, f + 1) for k, f in scenes.items()} if fps_nominal > 60 else {}
        shots = sorted(set(scenes.values()) | {b for _, b in pairs.values()})
        traces = []
        if kind in ("classic", "latency", "identity"):
            # Z2: the animated scenes are matched within a few frames (run-to-run shifts of the loading phase).
            shots = sorted(set(shots) | set(range(810, 823)) | set(range(1192, 1209)))
        if spec.get("shots"):
            shots = sorted(set(shots) | set(t120_frames()))
        if spec.get("traces"):
            traces = t120_frames()
        frames = spec["frames"]
        ini = write_variant_ini(base, name, spec, root, frames, shots, traces)
        vdir = root / name
        attempt = 0
        while True:
            code, stderr, log, vfs, elapsed, timeout = run_variant(args.exe.resolve(), ini, vdir, frames, fps_nominal,
                                                                   spec.get("desktop", 0.0))
            info = display_info(log)
            env_ok, env_why = environment_ok(info, args.min_refresh, args.min_foreground)
            needs_env = kind in ("display", "classic", "latency")
            if not needs_env or env_ok or attempt >= args.env_retries:
                break
            attempt += 1
            print(f"{name}: environment invalid ({env_why}) - retry {attempt} of {args.env_retries}")
            time.sleep(5)
        runs[name] = {"log": log, "info": info, "env_ok": env_ok, "env_why": env_why, "dir": vdir}
        refresh = info.get("refresh", 0.0)
        fps, k = expected_rate(spec["mode"], refresh)
        n_tick = frames_per_tick(fps)
        results = check_general(name, log, vfs, code, elapsed, timeout, frames, language, vdir, fps)
        out_dir = vdir / "m3_check"
        out_dir.mkdir(parents=True, exist_ok=True)
        by, m3info = check_renderer(log, frames, fps, scenes, shots, vdir, out_dir,
                                    ref_dir if kind in ("classic", "display", "motion") else None, frontend_dirs,
                                    fps - 0.5 if kind not in ("motion", "transforms") else 0.0, 1000.0 / max(fps, 1.0))
        if spec.get("debug_layer", "on") == "off":
            # t120x: the validation layer would slow the traced frames further; i120 and t120 validate 120 fps.
            results.append(("G2 D3D11 debug layer (off in this variant; i120 and t120 validate 120 fps)",
                            "0 errors" in by["M3-2"][2], by["M3-2"][2]))
        else:
            results.append(("G2 " + by["M3-2"][0].split(" ", 1)[1], by["M3-2"][1], by["M3-2"][2]))
        results.append(("G3 " + by["M3-4"][0].split(" ", 1)[1], by["M3-4"][1], by["M3-4"][2]))
        skip_env = needs_env and not env_ok and args.display_optional
        env_note = f"ENVIRONMENT INVALID: {env_why}" if needs_env and not env_ok else ""

        if kind in ("display", "classic"):
            if kind == "classic":  # M3b at 60 fps: vblank k = R / 60 on an integer refresh (M3b-7)
                kk = round(refresh / 60.0) if refresh else 0
                k = kk if 1 <= kk <= MAX_SYNC_INTERVAL and abs(refresh / kk - 60.0) <= INTEGER_TOLERANCE * 60 else 0
            ok, detail = judge_display(info, fps, k, env_ok)
            label = ("D1 plan and presentation at the display's rate" if kind == "display" else
                     "M3b-7 display coupling at 60 fps (foreground frames)")
            results.append((label, None if skip_env else ok, (env_note + "; " if env_note else "") + detail))
        if spec.get("desktop"):
            results.append(check_fullscreen(log, vdir / "desktop.png", None if skip_env else env_ok))
        if kind in ("display", "motion") and n_tick == 2:
            for r in check_120(name, log):
                results.append(r)
            results.append(check_repeats(log, info, vdir / "screenshots", pairs))
            # D6 scenes at 120 fps (M3-1 incl. xemu for the first frame of each pair, the second frame below).
            pair_check = check_scene_pairs(log, pairs, ref_dir, frontend_dirs)
            ok6 = by["M3-1"][1] and (pair_check is None or pair_check[0])
            m33 = by["M3-3"]
            detail6 = by["M3-1"][2] + (f"; second frames: {pair_check[1]}" if pair_check else "")
            if kind == "display":
                # M3-3 without its frame-start regularity (judged by D1 on the screen for the vblank plans): exit 0,
                # frame limit, average >= 119.5 Hz, frame work p99 <= one period.
                work = first(log, r"present: frame work \d+ frames: average [\d.]+ ms, 99th percentile ([\d.]+) ms")
                limit = first(log, rf"FRAMES: limit of {frames} frames reached")
                m33_ok = (code == 0 and limit is not None and info.get("average", 0) >= fps - 0.5 and work is not None
                          and float(work.group(1)) <= 1000.0 / fps)
                ok6 = ok6 and (m33_ok or skip_env)
                detail6 += f"; M3-3 (frame starts reported, see D1): {m33[2]}"
            else:
                detail6 += f"; M3-3 (reported: screenshot frames on the timer plan): {m33[2]}"
            results.append(("D6 M3 scenes at 120 fps (xemu), frame rate", ok6, detail6))
            for line in m3info.get("scenes", []):
                results.append(("    scene", True, line))
        elif kind == "display":
            # 60 Hz display: 60 fps, one frame per tick, no interpolation.
            clock, interp = parse_clock(log), parse_interp(log) or {}
            ok = clock is not None and clock["regime"].startswith("1 frame per tick") and interp.get("recorded") == 0
            results.append(("D2-D5 one frame per tick on this display (no interpolation)", ok,
                            f"clock {clock['regime'] if clock else '?'}; interpolation recorded "
                            f"{interp.get('recorded')} frames"))
        if kind in ("motion", "transforms", "reference"):
            for r in analyze_motion(vdir, log, args.max_midpoint_px):
                results.append(r)
        if kind == "reference" and "t120" in runs:
            # Frames without a step show S[G] in both runs: each has a bit-identical frame in the run without
            # interpolation (the loading phase can shift the frontend by a few frames from run to run, so the
            # partner is searched within +-8 frames); the interpolated frames with a step have none.
            trace_on = read_interp_trace(runs["t120"]["dir"] / "logs" / "interp_trace.csv")
            pixels_on = {}
            for f, p in ((r["frame"], shot_path(runs["t120"]["log"], r["frame"])) for r in trace_on if not r["flags"]):
                if p is not None and p.exists():
                    pixels_on[f] = p
            cache_off: dict = {}

            def off_pixels(frame):
                if frame not in cache_off:
                    p = shot_path(log, frame)
                    cache_off[frame] = (np.asarray(Image.open(p).convert("RGB")) if p is not None and p.exists()
                                        else None)
                return cache_off[frame]

            steps_of = {r["frame"]: r["steps"] for r in trace_on}
            found = exact = missing = tick_matched = tick_new = 0
            offsets: dict = {}
            worst = 0

            def partner(a, f):
                """(offset, differing pixels) of the closest frame of t120off within +-8 frames."""
                best = None
                for o in range(-8, 9):
                    b = off_pixels(f + o)
                    if b is None:
                        continue
                    n = int(np.count_nonzero(np.any(a != b, axis=2)))
                    if best is None or n < best[1]:
                        best = (o, n)
                return best

            # A partner differs in at most 0.1 % of the pixels: content that follows the real time instead of the
            # ticks (a few pixels in the steady frontend) differs between any two runs.
            limit = 640 * 480 // 1000
            missing_frames = []
            for f, p in sorted(pixels_on.items()):
                a = np.asarray(Image.open(p).convert("RGB"))
                best = partner(a, f)
                hit = best is not None and best[1] <= limit
                if steps_of.get(f) == 0:
                    if not hit:
                        missing_frames.append(f)
                    else:
                        found += 1
                        exact += best[1] == 0
                        worst = max(worst, best[1])
                        offsets[best[0]] = offsets.get(best[0], 0) + 1
                else:
                    tick_matched, tick_new = tick_matched + hit, tick_new + (not hit)
            # M3d phase 5: the first frame of a screenshot window has no partner when the run-to-run shift (the
            # dominant offset) points before the window of t120off (e.g. -3 at the window starting at 2593); such a
            # frame is not judged.
            dominant = max(offsets, key=offsets.get) if offsets else 0
            edge = [f for f in missing_frames if off_pixels(f + dominant) is None]
            missing = len(missing_frames) - len(edge)
            results.append(("R2 frames without a step = S[G]: the same picture exists without interpolation",
                            found > 0 and missing == 0 and tick_new > 0,
                            f"frames without a step: {found} with a partner within +-8 frames ({exact} bit-identical, "
                            f"the others differ in at most {worst} pixels; frame offsets {offsets}), {missing} "
                            f"without" + (f" (missing: {[f for f in missing_frames if f not in edge][:8]})"
                                          if missing else "")
                            + f", {len(edge)} at a window edge (partner at offset {dominant} not captured: {edge[:8]}); "
                            f"frames with a step (interpolated): {tick_new} without a partner, {tick_matched} "
                            f"with one (no motion)"))
        if kind in ("classic", "identity", "latency"):
            clock, interp = parse_clock(log), parse_interp(log) or {}
            fixes = first(log, r"frame fixes \(frames between ticks\): death screen time added once x (\d+); occluder "
                               r"fades (\d+) calls, (\d+) skipped without a step; flicker (\d+) calls")
            inactive = (clock is not None and clock["regime"].startswith("1 frame per tick") and clock["enabled"] == 0
                        and interp.get("recorded", 0) == 0 and interp.get("active_from") is None
                        and info.get("repeated") == 0 and fixes is not None and fixes.group(1) == "0"
                        and fixes.group(3) == "0")
            g1 = first(log, r"game clock: 1 frame per tick \(.*M3b\)")
            results.append(("Z1 60 fps: M3b clock, interpolation inactive, no repeated picture", inactive
                            and g1 is not None,
                            f"'{g1.group(0) if g1 else 'no M3b clock line'}'; clock M3d "
                            f"{clock['regime'] if clock else '?'}, {clock['enabled'] if clock else '?'} frames between "
                            f"ticks; interpolation {interp.get('recorded')} recorded, active from "
                            f"{interp.get('active_from')}; repeated pictures {info.get('repeated')}; frame fixes "
                            f"{fixes.group(0)[len('frame fixes (frames between ticks): '):] if fixes else '?'}"))
        if kind == "classic":
            ticks_r, _ = m2_check.check_timing(log, vfs, TICK_HZ, "lockstep",
                                               "display-rate" not in m3info.get("plan_frames", ""))
            for i, (n_, ok_, d_) in enumerate(ticks_r):
                results.append((f"M3b-{5 + i} " + n_.split(" ", 1)[1], ok_, d_))
            for key in ("M3-1", "M3-3"):
                results.append((by[key][0], by[key][1] if key == "M3-1" else (by[key][1] or skip_env), by[key][2]))
            for line in m3info.get("scenes", []):
                results.append(("    scene", True, line))
        others = [v for v in ("c60", "ll60off") if v in runs]
        if kind == "identity" and others:
            # Against every 60 fps run with the interpolation on: the fully static scenes always bit-identical; all
            # four scenes bit-identical with at least one of them (the runs without a loading hitch at another frame).
            problems, detail = [], []
            for other in others:
                for scene in ("legal", "tech_notice", "loading", "frontend"):
                    f = round(SCENES_60[scene] * 60)
                    if f > frames:
                        continue
                    pb = shot_path(log, f)
                    if pb is None or not pb.exists():
                        problems.append(f"{scene}: screenshot of frame {f} missing")
                        continue
                    ib = np.asarray(Image.open(pb).convert("RGB"))
                    # The static scenes at the same frame; the animated ones (can, frontend) within +-6 frames.
                    window = [0] if scene in STATIC_SCENES else sorted(range(-6, 7), key=abs)
                    hit = None
                    for o in window:
                        pa = shot_path(runs[other]["log"], f + o)
                        if pa is not None and pa.exists() and np.array_equal(np.asarray(Image.open(pa).convert("RGB")),
                                                                              ib):
                            hit = o
                            break
                    detail.append(f"{other} {scene} frame {f}: " + (f"bit-identical (frame offset {hit:+d})"
                                                                    if hit is not None else "NO bit-identical frame"))
                    if hit is None:
                        problems.append(f"{other} {scene} differs")
            results.append(("Z2 interpolation off + probe removed = interpolation on at 60 fps (bit-identical)",
                            not problems, "; ".join(problems + detail)))
        if kind == "latency" and "c60" in runs:
            on, off = runs["c60"], runs[name]
            rows_on, qpf_on = read_pacing_trace(on["dir"] / "pacing_trace.csv")
            rows_off, qpf_off = read_pacing_trace(off["dir"] / "pacing_trace.csv")
            lat_on, lat_off = start_to_visible(rows_on, qpf_on), start_to_visible(rows_off, qpf_off)
            i_on, i_off = on["info"], off["info"]
            env_both = on["env_ok"] and off["env_ok"]
            med_on, med_off = pct(lat_on, 0.5), pct(lat_off, 0.5)
            plan_on = i_on.get("on_plan") or (0, 0.0, 0, 0)
            plan_off = i_off.get("on_plan") or (0, 0.0, 0, 0)
            late = i_on.get("jit_late", 0) / max(1, i_on.get("jit_frames", 0))
            ok = (env_both and lat_on and lat_off and med_on <= 8.0 and med_on <= med_off - 5.0 and plan_on[1] >= 99.0
                  and plan_off[1] >= 99.0 and late <= 0.01)
            why = "" if env_both else f"ENVIRONMENT INVALID: c60 {on['env_why'] or 'ok'}; ll60off {off['env_why'] or 'ok'}; "
            results.append(("L1 just-in-time start: frame start -> visible", None if (not env_both and
                                                                                       args.display_optional) else ok,
                            why + f"low_latency on: median {med_on:.2f} ms, p99 {pct(lat_on, .99):.2f} ms over "
                            f"{len(lat_on)} frames, on plan {plan_on[1]:.2f} %, just in time {i_on.get('jit_frames')} "
                            f"of {i_on.get('jit_vblank')} vblank frames (delay {i_on.get('jit_delay')} ms, budget "
                            f"{i_on.get('jit_budget')} ms, {i_on.get('jit_late')} late); off: median {med_off:.2f} ms, "
                            f"p99 {pct(lat_off, .99):.2f} ms over {len(lat_off)} frames, on plan {plan_off[1]:.2f} %; "
                            f"present -> visible on {i_on.get('present_to_visible')} / off "
                            f"{i_off.get('present_to_visible')}"))

        judged = [(n_, ok_, d_) for n_, ok_, d_ in results if not n_.startswith("    ")]
        run_ok = all(ok_ is not False for _, ok_, _ in judged)
        all_ok &= run_ok
        emit(f"{name}: exit {code}, {elapsed:.0f} s, {'OK' if run_ok else 'FAILED'}"
             + (f" (environment invalid: {env_why})" if needs_env and not env_ok else ""))
        if not run_ok and stderr:
            own = [ln for ln in stderr.splitlines() if ln.strip() and not m2_check.LOG_LINE.match(ln)]
            if own:
                emit("    stderr: " + own[-1][:200])
        for n_, ok_, d_ in results:
            if n_.startswith("    "):
                emit(f"        {d_}")
            else:
                emit(f"    [{'SKIP' if ok_ is None else 'PASS' if ok_ else 'FAIL'}] {n_}: {d_}")
    (root / "report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print(f"logs, INIs, screenshots and sheets: {root}")
    print("M3d check: " + ("ALL PASS" if all_ok else "FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
