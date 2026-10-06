"""M3c check: render resolution and anti-aliasing (docs/ROADMAP.md "M3c"; src/host/gfx/GFX_CONTRACT.md 12).

Runs futurama.exe over a configuration matrix derived from ONE base INI (its [paths] iso, [settings] and the other
sections are kept; [paths] userdata is the root: every variant runs in <userdata>/m3c_check/<variant>/ with its own
generated futurama.ini) and checks:

  every variant (C1-C6)
    C1 exit code 0 and "FRAMES: limit of N frames reached"
    C2 the paced presentation: "present: N frames in T s (average A Hz ...)" with A >= --min-hz (default 59.5), the
       frame work "99th percentile P ms" with P <= --max-work-ms (default 80 % of the tick period = 13.3 ms at 60 Hz,
       i.e. well below the period) and >= --min-on-period % of the frame starts on their period (m3_check M3-3)
    C3 D3D11 debug layer enabled with 0 errors / 0 corruption and no "D3D11 debug: error" line (m3_check M3-2; the
       variants set [render] debug_layer = on, so Release builds are validated too)
    C4 the render configuration of the "render: output WxH (resolution ...), internal WxH (...), MSAA Nx (requested
       Mx; supported: ...)" line: output = the largest 4:3 rectangle inside the view of the "window:" line (resolution
       auto) or inside [render] resolution; internal = output x render_scale; MSAA = the requested level or the
       highest supported one below it; the expected downsample (none / box / catmull-rom); the resolve (gamma, or
       linear light with [render] linear_aa = on) and no step down of the quality ("reduced from", review M3c
       finding 2)
    C5 screenshots: "screenshot: <path> (WxH)" for every scene frame with WxH = the output size ([render]
       screenshot_size = output) or the internal size (internal), the PNG exists and has that size; the scenes are
       complete (m3_check M3-1: draw summary, non-trivial picture)
    C6 the d3d11 renderer drew every call (m3_check M3-4) and every M2 criterion holds (m2_check: archives, language,
       intro, fibers/APCs, SEH self-test, VFS trace, no error/warn/UNIMPLEMENTED/TRAP line)
  classic (resolution 640x480, render_scale 1, msaa off, anisotropy off - the M3 renderer bit for bit): the full
    m3_check including the xemu comparison of every scene (legal/tech notice 1.0, loading 2.0, frontend 2.0 on the
    static pixels) and the game timing criteria M3b-5/6
  hq (resolution auto in borderless fullscreen: on the 3840x2160 monitor of the dev machine 2880x2160, MSAA 4x,
    alpha-to-coverage, anisotropy 16, render_scale 1): X1 every scene, box-downscaled to 640x480, still matches the
    xemu captures within the m3_check limits
  hq_noaa (hq without MSAA) and ref (hq output with render_scale 2 + MSAA 8x = 32 samples per output pixel, the
    supersampled reference): X2 edge aliasing of the frontend frame - on the pixels that are edges in the reference
    (3x3 luminance contrast >= 48) and static in every xemu frontend capture, the mean luminance difference to the
    reference with MSAA 4x must be at most --max-edge-ratio (default 0.7) of the difference without AA; reported
    as well: the share of those edge pixels with a hard step (difference > 32 = an uncovered or fully covered
    pixel where the reference has a partial one, the stair steps of an aliased edge)
  ss (supersampling at a smaller output: resolution 1440x1080 in the window, render_scale 2 -> internal 2880x2160,
    exact 2x2 box downsample, MSAA 4x, screenshot_size = internal): C4/C5 at the internal size
  max (the launcher's "Maximum" preset since the M3c review: hq with render_scale 2 + MSAA 8x and [render]
    linear_aa = on): C1-C6 with the linear-light resolve and box downsample
  frac (a fractional render_scale 1.5 with MSAA 4x and linear_aa = on): C1-C6 with the Catmull-Rom downsample on
    linear light (review M3c findings 1 and 3)

Reported for every run, not judged: the "d3d11 gpu frame time" line (timestamp span from the first clear/draw of a
frame to the end of its present pass: GPU work plus the gaps while the CPU still submits), the "render totals" line (alpha-to-coverage draws, anisotropic and anti-aliased-nearest stages) and the
environment line of the pacer ("present: environment: foreground ..., display off ..."); the environment is not
judged (with the display off or the window in the background the pacer runs on its timer and the
presentation criteria C2 still apply). Runs one variant after another; borderless fullscreen covers the primary
monitor for the hq/hq_noaa/ref runs.

Usage (paths with spaces belong in the INI, not on the command line):
    python tools/m3c_check.py --exe build/m3c/bin/futurama.exe --config run/m3c/futurama.ini
                              [--variants classic,hq,hq_noaa,ref,ss,max,frac] [--fresh] [--aa-frames 1300]
                              [--min-hz 59.5] [--max-work-ms MS] [--min-on-period 95] [--max-edge-ratio 0.7]
                              [--xemu-ref DIR | --no-xemu-ref] [--xemu-frontend DIR ...]

Exit code 0 when every criterion of every selected variant passes. Needs Pillow. Logs, generated INIs, screenshot
copies and the edge-metric images: <userdata>/m3c_check/<variant>/ and <userdata>/m3c_check/report.txt.
"""
import argparse
import math
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m2_check  # noqa: E402  (configuration parsing, the M2 criteria)
import m3_check  # noqa: E402  (scene, debug-layer, frame-rate and renderer criteria; xemu comparison)

try:
    from PIL import Image, ImageChops, ImageFilter, ImageStat
except ImportError:
    Image = None

GUEST_W, GUEST_H = 640, 480
MAX_DIMENSION = 16384
SCENE_SECONDS = m3_check.DEFAULT_SCENE_SECONDS  # legal 3.0 s, tech notice 8.0 s, loading 13.6 s, frontend 20.0 s
EDGE_CONTRAST = 48       # 3x3 luminance max - min in the reference that makes a pixel an edge pixel
HARD_STEP = 32           # luminance difference to the reference that counts as a hard (aliased) step
STATIC_ERODE = 9         # erosion of the upscaled xemu static mask (pixels at 2880x2160: 2 guest pixels)
MAX_EDGE_RATIO = 0.7

# The configuration matrix. "render" overrides [render] keys of the base INI; "frames" None = --aa-frames.
VARIANTS = {
    "classic": {
        "render": {"resolution": "640x480", "render_scale": "1.0", "msaa": "off", "alpha_to_coverage": "on",
                   "anisotropy": "off", "fullscreen": "0", "screenshot_size": "output", "linear_aa": "off"},
        "frames": 3000, "extra_shots": [2990], "keep_trace": True,
    },
    "hq": {
        "render": {"resolution": "auto", "render_scale": "1.0", "msaa": "4", "alpha_to_coverage": "on",
                   "anisotropy": "16", "fullscreen": "1", "screenshot_size": "output", "linear_aa": "off"},
        "frames": None,
    },
    "hq_noaa": {
        "render": {"resolution": "auto", "render_scale": "1.0", "msaa": "off", "alpha_to_coverage": "on",
                   "anisotropy": "16", "fullscreen": "1", "screenshot_size": "output", "linear_aa": "off"},
        "frames": None,
    },
    "ref": {
        "render": {"resolution": "auto", "render_scale": "2.0", "msaa": "8", "alpha_to_coverage": "on",
                   "anisotropy": "16", "fullscreen": "1", "screenshot_size": "output", "linear_aa": "off"},
        "frames": None,
    },
    "ss": {
        "render": {"resolution": "1440x1080", "render_scale": "2.0", "msaa": "4", "alpha_to_coverage": "on",
                   "anisotropy": "16", "fullscreen": "0", "screenshot_size": "internal", "linear_aa": "off"},
        "frames": None,
    },
    "max": {
        "render": {"resolution": "auto", "render_scale": "2.0", "msaa": "8", "alpha_to_coverage": "on",
                   "anisotropy": "16", "fullscreen": "1", "screenshot_size": "output", "linear_aa": "on"},
        "frames": None,
    },
    "frac": {
        "render": {"resolution": "auto", "render_scale": "1.5", "msaa": "4", "alpha_to_coverage": "on",
                   "anisotropy": "16", "fullscreen": "1", "screenshot_size": "output", "linear_aa": "on"},
        "frames": None,
    },
}


def fit_aspect(w: int, h: int, aw: int = GUEST_W, ah: int = GUEST_H) -> tuple[int, int]:
    """platform::FitAspectRect: the largest aw:ah rectangle inside w x h (same integer rounding)."""
    if w == 0 or h == 0:
        return 0, 0
    if w * ah <= h * aw:
        return w, min(h, (w * ah + aw // 2) // aw)
    return min(w, (h * aw + ah // 2) // ah), h


def expected_sizes(resolution: str, view: tuple[int, int] | None, scale: float) -> tuple[tuple[int, int], tuple[int, int]]:
    """gfx::ComputeRenderSizes: (output, internal)."""
    if resolution.lower() == "auto":
        tw, th = view if view and view[0] and view[1] else (GUEST_W, GUEST_H)
    else:
        m = re.fullmatch(r"\s*(\d+)\s*[xX]\s*(\d+)\s*", resolution)
        tw, th = (int(m.group(1)), int(m.group(2))) if m else (GUEST_W, GUEST_H)
    ow, oh = fit_aspect(min(tw, MAX_DIMENSION), min(th, MAX_DIMENSION))
    k = min(max(scale, 1.0), 2.0)
    k = max(1.0, min(k, MAX_DIMENSION / ow, MAX_DIMENSION / oh))
    iw = min(max(int(math.floor(ow * k + 0.5)), ow), MAX_DIMENSION)
    ih = min(max(int(math.floor(oh * k + 0.5)), oh), MAX_DIMENSION)
    return (ow, oh), (iw, ih)


def write_variant_ini(base: Path, variant: str, spec: dict, root: Path, frames: int, shots: list[int],
                      debug_layer: str) -> Path:
    """<root>/<variant>/futurama.ini: the base INI with userdata, [debug] frames and the [render] overrides."""
    cp = m2_check.read_config(base)
    vdir = root / variant
    vdir.mkdir(parents=True, exist_ok=True)
    for section in ("paths", "debug", "render"):
        if not cp.has_section(section):
            cp.add_section(section)
    cp.set("paths", "userdata", str(vdir))
    cp.set("debug", "frames", str(frames))
    cp.remove_option("debug", "stop_at")
    for key, value in spec["render"].items():
        cp.set("render", key, value)
    cp.set("render", "backend", "d3d11")
    cp.set("render", "debug_layer", debug_layer)
    cp.set("render", "screenshot_frames", ", ".join(str(f) for f in shots))
    if not spec.get("keep_trace"):
        cp.remove_option("render", "trace_frames")
    ini = vdir / "futurama.ini"
    with ini.open("w", encoding="utf-8") as f:
        f.write(f"; generated by tools/m3c_check.py from {base} - variant {variant}\n")
        cp.write(f)
    return ini


def parse_render_line(log: str) -> dict | None:
    m = re.search(r"render: output (\d+)x(\d+) \(resolution ([^)]*)\), internal (\d+)x(\d+) \(render scale ([\d.]+)"
                  r".*?MSAA (\d+)x \(requested (\d+)x; supported: ([^)]*)\), alpha to coverage (\w+), anisotropy (\d+)x, "
                  r"downsample ([\w-]+), screenshots (\w+)", log)
    if not m:
        return None
    supported = [int(v) for v in re.findall(r"\d+", m.group(9))]
    tail = log[m.end():].split("\n", 1)[0]
    resolve = re.search(r"resolve (gamma|linear light)", tail)
    reduced = re.search(r"reduced from ([^\n]+)", tail)
    return {"output": (int(m.group(1)), int(m.group(2))), "resolution": m.group(3),
            "internal": (int(m.group(4)), int(m.group(5))), "scale": float(m.group(6)), "msaa": int(m.group(7)),
            "requested": int(m.group(8)), "supported": supported, "a2c": m.group(10) == "on",
            "anisotropy": int(m.group(11)), "downsample": m.group(12), "screenshots": m.group(13),
            "resolve": resolve.group(1) if resolve else "gamma", "reduced": reduced.group(1) if reduced else None}


def view_size(log: str) -> tuple[int, int] | None:
    m = re.search(r"window: frame \S+, view \S+ (\d+)x(\d+)", log)
    return (int(m.group(1)), int(m.group(2))) if m else None


def check_render_config(log: str, spec: dict) -> tuple[bool, str, dict]:
    """C4: the "render:" line against the variant."""
    r = parse_render_line(log)
    if r is None:
        return False, "no 'render: output ...' line (d3d11 renderer not created?)", {}
    render = spec["render"]
    view = view_size(log)
    scale = float(render["render_scale"])
    out, internal = expected_sizes(render["resolution"], view, scale)
    msaa = 1 if render["msaa"] == "off" else int(render["msaa"])
    allowed = [n for n in r["supported"] if n <= msaa]
    expected_msaa = max(allowed) if allowed and msaa > 1 else 1
    if out == internal:
        expected_down = "none"
    elif internal[0] % out[0] == 0 and internal[1] % out[1] == 0:
        expected_down = "box"
    else:
        expected_down = "catmull-rom"  # review M3c finding 1 (Mitchell-Netravali before)
    problems = []
    expected_resolve = "linear light" if render.get("linear_aa", "off") in ("on", "1", "true", "yes") else "gamma"
    if r["resolve"] != expected_resolve:
        problems.append(f"resolve {r['resolve']} != expected {expected_resolve}")
    if r["reduced"]:
        problems.append(f"quality reduced from {r['reduced']} (back buffers not available)")
    if r["output"] != out:
        problems.append(f"output {r['output']} != expected {out}")
    if r["internal"] != internal:
        problems.append(f"internal {r['internal']} != expected {internal}")
    if r["msaa"] != expected_msaa or r["requested"] != msaa:
        problems.append(f"MSAA {r['msaa']}x (requested {r['requested']}x) != expected {expected_msaa}x "
                        f"(requested {msaa}x; supported {r['supported']})")
    if r["downsample"] != expected_down:
        problems.append(f"downsample {r['downsample']} != expected {expected_down}")
    a2c = render["alpha_to_coverage"] in ("on", "1", "true", "yes") and expected_msaa > 1
    if r["a2c"] != a2c:
        problems.append(f"alpha to coverage {r['a2c']} != expected {a2c}")
    aniso = 1 if render["anisotropy"] == "off" else int(render["anisotropy"])
    if r["anisotropy"] != aniso:
        problems.append(f"anisotropy {r['anisotropy']}x != expected {aniso}x")
    detail = (f"view {view[0]}x{view[1]}" if view else "no view line") + \
        f", resolution {render['resolution']}: output {r['output'][0]}x{r['output'][1]}, internal " \
        f"{r['internal'][0]}x{r['internal'][1]} (render scale {r['scale']:.2f}), MSAA {r['msaa']}x (supported " \
        f"{', '.join(map(str, r['supported'])) or 'none'}), alpha to coverage {'on' if r['a2c'] else 'off'}, " \
        f"anisotropy {r['anisotropy']}x, downsample {r['downsample']}, resolve {r['resolve']}"
    shot = internal if render["screenshot_size"] == "internal" else out
    return not problems, ("; ".join(problems) + "; " if problems else "") + detail, {"shot": shot, "render": r}


def check_screenshot_sizes(log: str, shots: list[int], size: tuple[int, int]) -> tuple[bool, str]:
    """C5 (size part): every screenshot line and PNG has the expected size."""
    problems = []
    for frame in shots:
        m = re.search(rf"screenshot: (.*frame_{frame:06d}\.png) \((\d+)x(\d+)\)", log)
        if not m:
            problems.append(f"frame {frame}: no screenshot line")
            continue
        logged = (int(m.group(2)), int(m.group(3)))
        path = Path(m.group(1))
        if logged != size:
            problems.append(f"frame {frame}: logged {logged[0]}x{logged[1]}, expected {size[0]}x{size[1]}")
        if not path.exists():
            problems.append(f"frame {frame}: {path} missing")
        elif Image is not None:
            with Image.open(path) as im:
                if im.size != size:
                    problems.append(f"frame {frame}: PNG is {im.size[0]}x{im.size[1]}")
    return not problems, ("; ".join(problems) if problems
                          else f"{len(shots)} screenshots at {size[0]}x{size[1]}")


def shot_path(log: str, frame: int) -> Path | None:
    m = re.search(rf"screenshot: (.*frame_{frame:06d}\.png) \(", log)
    return Path(m.group(1)) if m else None


def downscaled(path: Path, out: Path) -> Path:
    """Box downscale of a screenshot to 640x480 (the size the xemu references are compared at)."""
    with Image.open(path) as im:
        im.convert("RGB").resize((GUEST_W, GUEST_H), Image.BOX).save(out)
    return out


def edge_metric(hq: Path, noaa: Path, ref: Path, static640, out_dir: Path) -> tuple[bool, str, dict]:
    """X2: mean luminance difference to the supersampled reference on its static edge pixels, MSAA vs no AA."""
    lr = Image.open(ref).convert("L")
    lh = Image.open(hq).convert("L")
    ln = Image.open(noaa).convert("L")
    if not (lr.size == lh.size == ln.size):
        return False, f"different sizes: hq {lh.size}, hq_noaa {ln.size}, ref {lr.size}", {}
    contrast = ImageChops.subtract(lr.filter(ImageFilter.MaxFilter(3)), lr.filter(ImageFilter.MinFilter(3)))
    edges = contrast.point(lambda v: 255 if v >= EDGE_CONTRAST else 0)
    if static640 is not None:
        static = static640.resize(lr.size, Image.NEAREST).filter(ImageFilter.MinFilter(STATIC_ERODE))
        mask = ImageChops.multiply(edges, static)
    else:
        mask = edges
    share = ImageStat.Stat(mask).mean[0] / 255.0
    pixels = int(round(share * lr.size[0] * lr.size[1]))
    if pixels < 1000:
        return False, f"only {pixels} static edge pixels - metric not meaningful", {}
    mask.save(out_dir / "edge_mask.png")

    def stats(img):
        diff = ImageChops.difference(img, lr)
        mean = ImageStat.Stat(diff, mask).mean[0]
        hard = ImageStat.Stat(diff.point(lambda v: 255 if v > HARD_STEP else 0), mask).mean[0] / 255.0
        return mean, hard, diff

    e_msaa, h_msaa, d_msaa = stats(lh)
    e_noaa, h_noaa, d_noaa = stats(ln)
    d_msaa.point(lambda v: min(255, v * 4)).save(out_dir / "edge_diff_msaa_x4.png")
    d_noaa.point(lambda v: min(255, v * 4)).save(out_dir / "edge_diff_noaa_x4.png")
    ratio = e_msaa / e_noaa if e_noaa > 0 else 1.0
    ok = ratio <= MAX_EDGE_RATIO_ARG[0]
    text = (f"{pixels} static edge pixels ({share * 100:.2f} % of {lr.size[0]}x{lr.size[1]}): mean |luminance - "
            f"reference| no AA {e_noaa:.2f}, MSAA {e_msaa:.2f} -> ratio {ratio:.2f} (maximum {MAX_EDGE_RATIO_ARG[0]}); "
            f"hard steps (> {HARD_STEP}) no AA {h_noaa * 100:.1f} %, MSAA {h_msaa * 100:.1f} %")
    return ok, text, {"ratio": ratio, "noaa": e_noaa, "msaa": e_msaa, "hard_noaa": h_noaa, "hard_msaa": h_msaa}


MAX_EDGE_RATIO_ARG = [MAX_EDGE_RATIO]  # set from the command line


def run_variant(exe: Path, ini: Path, userdata: Path, frames: int, rate: int) -> tuple[int, str, str, str, float, int]:
    for sub in ("hdd", "cache", "logs", "screenshots", "m3_check"):
        shutil.rmtree(userdata / sub, ignore_errors=True)
    extra = ["--selftest", "--trace=vfs"]
    timeout = frames // max(rate, 1) + 90
    start = time.monotonic()
    try:
        proc = subprocess.run([str(exe), f"--config={ini}", *extra], cwd=userdata, stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE, timeout=timeout)
        code, stderr = proc.returncode, proc.stderr.decode("utf-8", errors="replace").strip()
    except subprocess.TimeoutExpired:
        code, stderr = -1, f"killed after {timeout} s (hang)"
    elapsed = time.monotonic() - start
    log_path = userdata / "logs" / "futurama.log"
    trace_path = userdata / "logs" / "vfs_trace.log"
    log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
    vfs = trace_path.read_text(encoding="utf-8", errors="replace") if trace_path.exists() else ""
    return code, stderr, log, vfs, elapsed, timeout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--config", required=True, type=Path, help="base INI ([paths] iso + userdata = the root)")
    ap.add_argument("--variants", default=",".join(VARIANTS), help="comma-separated subset of " + ", ".join(VARIANTS))
    ap.add_argument("--fresh", action="store_true", help="delete <userdata>/m3c_check first")
    ap.add_argument("--aa-frames", type=int, default=1300, help="frames of the non-classic variants (default 1300)")
    ap.add_argument("--min-hz", type=float, help="minimum average presented rate (default tick rate - 0.5)")
    ap.add_argument("--max-work-ms", type=float, help="maximum frame-work 99th percentile (default 80 %% of a tick)")
    ap.add_argument("--min-on-period", type=float, default=m3_check.MIN_ON_PERIOD_PERCENT)
    ap.add_argument("--max-edge-ratio", type=float, default=MAX_EDGE_RATIO)
    ap.add_argument("--debug-layer", default="on", choices=("on", "auto", "off"))
    ap.add_argument("--xemu-ref", type=Path)
    ap.add_argument("--no-xemu-ref", action="store_true")
    ap.add_argument("--xemu-frontend", type=Path, action="append", default=[])
    args = ap.parse_args()
    if Image is None:
        sys.exit("m3c_check: Pillow is required")
    MAX_EDGE_RATIO_ARG[0] = args.max_edge_ratio

    base = args.config.resolve()
    cp = m2_check.read_config(base)
    ud = m2_check.ini_value(cp, "paths", "userdata")
    if not ud:
        sys.exit("m3c_check: [paths] userdata must be set in the base config (never %LOCALAPPDATA% in tests)")
    root = (Path(ud) if Path(ud).is_absolute() else base.parent / ud) / "m3c_check"
    if args.fresh:
        shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True, exist_ok=True)
    selected = [v.strip() for v in args.variants.split(",") if v.strip()]
    for v in selected:
        if v not in VARIANTS:
            sys.exit(f"m3c_check: unknown variant {v!r} (known: {', '.join(VARIANTS)})")
    language = m2_check.expected_language(cp)
    ticks = m2_check.tick_rate(cp)
    clock = m2_check.frame_clock(cp)
    rate = ticks  # the variants never set [render] frame_rate
    min_hz = args.min_hz if args.min_hz is not None else rate - 0.5
    max_work_ms = args.max_work_ms if args.max_work_ms is not None else 0.8 * 1000.0 / rate
    scenes = {name: round(sec * rate) for name, sec in SCENE_SECONDS.items()}
    repo = Path(__file__).resolve().parent.parent
    ref_dir = None
    if not args.no_xemu_ref:
        ref_dir = args.xemu_ref or (repo / "run" / "xemu_ref")
        if not ref_dir.is_dir():
            print(f"m3c_check: no xemu reference captures at {ref_dir} - xemu comparisons skipped")
            ref_dir = None
    frontend_dirs = list(args.xemu_frontend)
    if not frontend_dirs and (repo / "run" / "xemu_ref_steam").is_dir():
        frontend_dirs = [repo / "run" / "xemu_ref_steam"]
    print(f"m3c_check: variants {', '.join(selected)}; scenes "
          f"{', '.join(f'{k}={v}' for k, v in sorted(scenes.items(), key=lambda kv: kv[1]))} at {rate} Hz; C2 minimum "
          f"{min_hz:.2f} Hz, work p99 <= {max_work_ms:.1f} ms, {args.min_on_period:.0f} % on period; edge ratio <= "
          f"{args.max_edge_ratio}; debug layer {args.debug_layer}; xemu {ref_dir or 'none'}; output {root}")

    report: list[str] = []
    all_ok = True
    shots_by_variant: dict[str, dict[int, Path]] = {}
    frontend_cache: dict = {}
    for variant in selected:
        spec = VARIANTS[variant]
        frames = spec["frames"] or args.aa_frames
        shots = sorted({f for f in scenes.values() if f <= frames} | {f for f in spec.get("extra_shots", []) if f <= frames})
        ini = write_variant_ini(base, variant, spec, root, frames, shots, args.debug_layer)
        userdata = root / variant
        code, stderr, log, vfs, elapsed, timeout = run_variant(args.exe.resolve(), ini, userdata, frames, rate)
        (userdata / "run.log").write_text(log, encoding="utf-8")
        out_dir = userdata / "m3_check"
        out_dir.mkdir(parents=True, exist_ok=True)
        variant_scenes = {k: v for k, v in scenes.items() if v <= frames}
        use_xemu = ref_dir if variant == "classic" else None
        m3_results, info = m3_check.check_m3(log, code, frames, variant_scenes, shots, userdata, out_dir, 1, use_xemu,
                                             min_hz, max_work_ms, frontend_dirs, args.min_on_period)
        m2_results, _ = m2_check.check_run(log, vfs, code, elapsed, timeout, frames, language, userdata, rate)
        results = []
        by_name = {name.split(" ", 1)[0]: (name, ok, detail) for name, ok, detail in m3_results}
        limit = re.search(rf"FRAMES: limit of {frames} frames reached", log)
        results.append(("C1 exit 0, frame limit reached", code == 0 and limit is not None,
                         f"exit {code}, limit {'reached' if limit else 'NOT reached'}, {elapsed:.0f} s"))
        results.append(("C2 " + by_name["M3-3"][0].split(" ", 1)[1], by_name["M3-3"][1], by_name["M3-3"][2]))
        results.append(("C3 " + by_name["M3-2"][0].split(" ", 1)[1], by_name["M3-2"][1], by_name["M3-2"][2]))
        ok4, text4, cfg_info = check_render_config(log, spec)
        results.append(("C4 render configuration (output, internal, MSAA, downsample)", ok4, text4))
        if cfg_info:
            ok5, text5 = check_screenshot_sizes(log, shots, cfg_info["shot"])
        else:
            ok5, text5 = False, "no render configuration line"
        ok5 = ok5 and by_name["M3-1"][1]
        results.append(("C5 screenshots at the expected size, scenes complete", ok5,
                        text5 + ("" if by_name["M3-1"][1] else "; " + by_name["M3-1"][2])))
        bad_m2 = [f"{n}: {d}" for n, ok, d in m2_results if not ok]
        results.append(("C6 every draw drawn (M3-4), every M2 criterion", by_name["M3-4"][1] and not bad_m2,
                        by_name["M3-4"][2] + ("; M2: " + "; ".join(bad_m2)[:400] if bad_m2 else "; M2 all pass")))
        if variant == "classic":
            timing, _ = m2_check.check_timing(log, vfs, ticks, clock, "display-rate" not in info.get("plan_frames", ""))
            for k, (name, ok, detail) in enumerate(timing):
                results.append((f"M3b-{5 + k} " + name.split(" ", 1)[1], ok, detail))
            results.append(("M3-1 scenes vs xemu (classic = M3)", by_name["M3-1"][1], by_name["M3-1"][2]))
        shots_by_variant[variant] = {f: p for f in shots if (p := shot_path(log, f)) is not None and p.exists()}
        # X1: the high-resolution scenes, box-downscaled, still match the xemu captures.
        if variant == "hq" and ref_dir is not None:
            problems, details = [], []
            for scene, frame in sorted(variant_scenes.items(), key=lambda kv: kv[1]):
                p = shots_by_variant[variant].get(frame)
                if p is None:
                    problems.append(f"{scene}: no screenshot")
                    continue
                small = downscaled(p, out_dir / f"down_{frame:06d}.png")
                ok, text = m3_check.compare_xemu(scene, small, ref_dir, frontend_cache, frontend_dirs)
                details.append(f"{scene}: {text}")
                if ok is False:
                    problems.append(f"{scene}: {text}")
            missing = [p for p in problems if p.endswith("no screenshot")]
            results.append(("X1 high-resolution scenes downscaled to 640x480 match xemu", not problems,
                            "; ".join(missing + details)))
        env = re.search(r"present: environment: (.+)$", log, re.M)
        run_ok = all(ok for _, ok, _ in results)
        all_ok &= run_ok
        header = f"{variant}: exit {code}, {elapsed:.0f} s, {'OK' if run_ok else 'FAILED'}"
        print(header)
        report.append(header)
        if not run_ok and stderr:
            own = [ln for ln in stderr.splitlines() if ln.strip() and not m2_check.LOG_LINE.match(ln)]
            if own:
                print("    stderr: " + own[-1][:200])
        for name, ok, detail in results:
            line = f"    [{'PASS' if ok else 'FAIL'}] {name}: {detail}"
            print(line)
            report.append(line)
        for line in info.get("scenes", []):
            print(f"        {line}")
            report.append(f"        {line}")
        if env:
            line = f"        environment (reported): {env.group(1)}"
            print(line)
            report.append(line)
        gpu = re.search(r"d3d11 gpu frame time: (.+)$", log, re.M)
        totals = re.search(r"render totals: (.+)$", log, re.M)
        for label, m in (("gpu frame span (reported)", gpu), ("render totals", totals)):
            if m:
                line = f"        {label}: {m.group(1)}"
                print(line)
                report.append(line)

    # X2: MSAA reduces the edge aliasing of the frontend against the supersampled reference.
    frontend = scenes["frontend"]
    if all(v in selected for v in ("hq", "hq_noaa", "ref")):
        paths = [shots_by_variant.get(v, {}).get(frontend) for v in ("hq", "hq_noaa", "ref")]
        if None in paths:
            ok, text = False, "frontend screenshot missing in hq / hq_noaa / ref"
        else:
            static = None
            if ref_dir is not None:
                if "mask" not in frontend_cache:
                    # Builds the static mask of the xemu frontend captures (m3_check.compare_xemu).
                    m3_check.compare_xemu("frontend", downscaled(paths[2], root / "ref_frontend_640.png"), ref_dir,
                                          frontend_cache, frontend_dirs)
                static = frontend_cache.get("mask")
            ok, text, _ = edge_metric(paths[0], paths[1], paths[2], static, root)
        all_ok &= ok
        line = f"X2 edge aliasing (frontend frame {frontend}, MSAA 4x vs no AA against render_scale 2 + MSAA 8x): " \
               f"[{'PASS' if ok else 'FAIL'}] {text}"
        print(line)
        report.append(line)
    (root / "report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print(f"logs, INIs and screenshots: {root}")
    print("M3c check: " + ("ALL PASS" if all_ok else "FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
