"""M3 exit-criteria check (docs/loader_design.md section 6, M3 "Erstes Bild").

Runs futurama.exe N times with the D3D11 renderer and a test configuration (pattern of m2_check.py: the
script adds --selftest and, unless the INI sets them, --frames=3000 and --trace=vfs) and checks

  M3-1 the scenes appear completely: for every scene frame (default legal 150, tech_notice 400 = the
       "Splash" NI_Splash_PAL.nif, loading 680, frontend 1000; --scene NAME=FRAME) the INI lists the frame
       in [render] screenshot_frames, the log has "screenshot: <path> (640x480)" and the PNG exists and is
       not trivial (luminance spread, number of colors, no single color covering the picture), and the
       frame's "draw summary frame N:" line has draw calls and none that was not drawn. With the xemu
       reference captures (run\\xemu_ref, git-ignored; --xemu-ref) every scene is also compared with the
       original: mean absolute difference per channel on the 640x480 downscale (legal/tech notice/loading:
       whole picture without the xemu menu bar; frontend: only pixels that are static in all xemu frontend
       captures, because the menu background is animated)
  M3-2 no D3D debug-layer error: exit line "D3D11 debug layer: <state> - N errors, M warnings, K corruption"
       with N = K = 0 and no "D3D11 debug: error|corruption" line; state "enabled", or "not installed" (passes
       with a note: the SDK layers are missing, nothing was validated); "off" fails (Release builds need
       [render] debug_layer = on)
  M3-3 --frames=N without crash, presented frame rate >= 50: exit 0, "FRAMES: limit of N frames reached",
       "present: N frames in T s (average A Hz, ...)" with A >= --min-hz (default 50)
  M3-4 renderer totals: the d3d11 backend was active (Create line "(d3d11: ..."), no draw failed /
       unsupported / error / skipped ("d3d11 draws <Kind>:" lines), no texture creation failure

plus every M2 criterion (m2_check.check_run: archives, language, BUILD.TXT, intro + orderly end, fibers and
APCs, SEH self-test, VFS trace) and the general rule: no error/warn/UNIMPLEMENTED/TRAP line.

Usage (paths with spaces belong in the INI, not on the command line):
    python tools/m3_check.py --exe build/m3/bin/futurama.exe --config run/m3/futurama.ini
                             [--runs 1] [--fresh] [--timeout SECONDS] [--scene NAME=FRAME ...]
                             [--xemu-ref run/xemu_ref | --no-xemu-ref] [--min-hz 50]

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
DEFAULT_SCENES = {"legal": 150, "tech_notice": 400, "loading": 680, "frontend": 1000}
# xemu reference (run\xemu_ref\scenes.tsv): scene -> reference image, rows of the 640x480 image to ignore
# (the xemu menu bar covers the top ~24 pixels of the early captures), mean-difference limit per channel.
XEMU_REFS = {
    "legal": ("ref_legal.png", 24, 1.0),
    "tech_notice": ("ref_tech_notice.png", 0, 1.0),
    "loading": ("ref_splash.png", 0, 2.0),  # the can in the corner is animated
}
# Frontend: compared on the pixels that are static in every xemu capture of the frontend context.
XEMU_FRONTEND = (78.0, 132.0, 2.0)  # capture times (s) of the frontend, limit for the static pixels
MIN_LUMA_STDDEV = 8.0
MIN_COLORS = 64
MAX_TOP_COLOR_SHARE = 0.98

CRITERIA = [
    "M3-1 scenes appear completely (screenshots, draws, xemu comparison)",
    "M3-2 no D3D debug-layer error",
    "M3-3 frame limit reached, presented frame rate >= 50 Hz",
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


def compare_xemu(scene: str, shot: Path, ref_dir: Path, frontend_cache: dict) -> tuple[bool | None, str]:
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
        lo, hi, limit = XEMU_FRONTEND
        if "refs" not in frontend_cache:
            refs = []
            for f in sorted(ref_dir.glob("t*.png")):
                try:
                    t = float(f.stem[1:-1])
                except ValueError:
                    continue
                if lo <= t <= hi:
                    refs.append(Image.open(f).convert("RGB").resize(ours.size, Image.BOX))
            mask = None
            if len(refs) >= 2:
                # Static pixels: no reference frame differs from the first by more than 2 in any channel.
                moving = Image.new("L", ours.size, 0)
                for r in refs[1:]:
                    d = ImageChops.difference(refs[0], r)
                    r_, g_, b_ = d.split()
                    moving = ImageChops.lighter(moving, ImageChops.lighter(ImageChops.lighter(r_, g_), b_))
                mask = moving.point(lambda v: 255 if v <= 2 else 0)
            frontend_cache["refs"] = refs
            frontend_cache["mask"] = mask
        refs, mask = frontend_cache["refs"], frontend_cache["mask"]
        if mask is None:
            return None, "no xemu frontend captures"
        static = ImageStat.Stat(mask).mean[0] / 255.0
        diff = mean_diff(ours, refs[0], mask)
        return diff <= limit, (f"xemu frontend ({len(refs)} captures): mean difference {diff:.2f} on the "
                               f"{static * 100:.0f} % static pixels (limit {limit})")
    return None, "no xemu reference for this scene"


def check_m3(log: str, exit_code: int, frames: int, scenes: dict, shot_frames: list[int], userdata: Path,
             out_dir: Path, run: int, ref_dir: Path | None, min_hz: float) -> tuple[list, dict]:
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
        summary = first(rf"draw summary frame {frame}: (\d+) draw calls, (\d+) drawn, (\d+) not drawn, (\d+) clears")
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
            if calls == 0 or not_drawn != 0:
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
            ref_ok, ref_text = compare_xemu(scene, shot, ref_dir, frontend_cache)
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
    limit = first(rf"FRAMES: limit of {frames} frames reached")
    if present is None:
        results.append((CRITERIA[2], False, "no 'present:' statistics line"))
    else:
        n, secs, avg, slow, longest = int(present.group(1)), float(present.group(2)), float(present.group(3)), \
            float(present.group(4)), float(present.group(5))
        ok = exit_code == 0 and limit is not None and n >= frames and avg >= min_hz
        results.append((CRITERIA[2], ok,
                        f"exit {exit_code}, limit {'reached' if limit else 'NOT reached'}, {n} frames presented in "
                        f"{secs:.2f} s: average {avg:.2f} Hz (minimum {min_hz}), slowest 1 s window {slow:.2f} Hz, "
                        f"longest frame {longest:.1f} ms"
                        + (f", {pacing.group(2)} schedule resets, {pacing.group(3)} slow frames" if pacing else "")))
        info["fps"] = avg

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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--config", required=True, type=Path)
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--timeout", type=int, help="seconds per run (default: frames/50 + 90)")
    ap.add_argument("--scene", action="append", default=[], metavar="NAME=FRAME",
                    help="scene frame (repeatable; replaces the defaults legal=150 tech_notice=400 loading=680 "
                         "frontend=1000)")
    ap.add_argument("--xemu-ref", type=Path, help="xemu reference captures (default run/xemu_ref if present)")
    ap.add_argument("--no-xemu-ref", action="store_true")
    ap.add_argument("--min-hz", type=float, default=50.0)
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
    scenes = dict(DEFAULT_SCENES)
    if args.scene:
        scenes = {}
        for s in args.scene:
            name, _, frame = s.partition("=")
            if not name or not frame.isdigit():
                sys.exit(f"m3_check: bad --scene {s!r} (NAME=FRAME)")
            scenes[name] = int(frame)
    shot_frames = sorted({int(v, 0) for v in m2_check.ini_value(cp, "render", "screenshot_frames").replace(" ", "")
                          .split(",") if v})
    repo = Path(__file__).resolve().parent.parent
    ref_dir = None
    if not args.no_xemu_ref:
        ref_dir = args.xemu_ref or (repo / "run" / "xemu_ref")
        if not ref_dir.is_dir():
            print(f"m3_check: no xemu reference captures at {ref_dir} - scene comparison skipped")
            ref_dir = None

    extra_args = ["--selftest"]
    frames = int(m2_check.ini_value(cp, "debug", "frames", "0") or "0", 0)
    if frames == 0:
        frames = DEFAULT_FRAMES
        extra_args.append(f"--frames={frames}")
    trace = [t.strip().lower() for t in m2_check.ini_value(cp, "debug", "trace").replace(";", ",").split(",")
             if t.strip()]
    if "vfs" not in trace and "all" not in trace:
        extra_args.append("--trace=" + ",".join(trace + ["vfs"]))
    rate = int(m2_check.ini_value(cp, "render", "frame_rate", "50") or "50", 0)
    if rate != 50:
        print(f"m3_check: warning: [render] frame_rate = {rate} - criterion M3-3 assumes the 50 Hz PAL pacing")
    timeout = args.timeout or (frames // max(rate, 1) + 90)

    out_dir = userdata / "m3_check"
    shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True)
    print(f"m3_check: {args.runs} run(s), {frames} frames at {rate} Hz, language {language}, scenes "
          f"{', '.join(f'{k}={v}' for k, v in sorted(scenes.items(), key=lambda kv: kv[1]))}, "
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
        m2_results, _ = m2_check.check_run(log, vfs, code, elapsed, timeout, frames, language, userdata)
        m3_results, info = check_m3(log, code, frames, scenes, shot_frames, userdata, out_dir, i, ref_dir,
                                    args.min_hz)
        results = m3_results + [("M2-" + name, ok, detail) for name, ok, detail in m2_results]
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
            if i == 1 or not ok:
                print(f"    [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        for line in info.get("scenes", []):
            print(f"        {line}")
    print(f"logs and screenshots: {out_dir}")
    print("M3 check: " + ("ALL PASS" if all_ok else "FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
