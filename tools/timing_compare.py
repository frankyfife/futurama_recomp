"""M3b duration comparison: the runtime observables of analysis/loader/timing60.md section 8 in seconds.

Reads one or more run directories (<userdata> with logs/futurama.log, logs/vfs_trace.log and the screenshots
the log lists) and prints, per run, the durations of the intro screens, the frontend idle until attract, the
attract cycle, the frontend fade and the steam cloud period - in seconds and in frames - plus, optionally, the
bounds the xemu reference captures (run\\xemu_ref, the original in xemu at PAL 50 Hz) give for the same
screens. Expected: everything that counts seconds (QPC or dt) is equal at 50 and 60 Hz within one tick; what
counts frames or ticks is 20 % shorter at 60 Hz (timing60.md 4.2, 4.10).

Measurement sources (all time stamps are the log's "[seconds]" since process start, guest main thread):
  - scene logic: VFS trace "archive lookup" of legal.nif, Video/fox-PAL.txt (first logo movie), NI_Splash_PAL.nif
    (tech notice), Loadmenu.nif (loading screen), intro-PAL.xmv (intro movie, ends the loading screen),
    Attract-PAL.xmv; game observer lines "game: context A -> B (CApp::Frame #n)"
  - screen: "screenshot: <file> (640x480)" lines of a run with dense [render] screenshot_frames; each picture is
    classified against the xemu scene references (legal, tech notice, loading screen: mean difference < 3 on the
    640x480 box downscale, the top 24 rows of the legal reference ignored; frontend < 10), black (mean
    luminance < 1) or other; the xemu captures themselves give the reference timeline
  - frontend fade: mean luminance of the frames after the frontend entry; the fade ends at the first frame within
    3 % of the plateau (median of the contiguous screenshots from 1.05 s after the first frontend frame on)
  - steam cloud: mean luminance of the region (515,105)-(600,160) (M3 review finding 7) in the longest equally
    spaced screenshot series from 3 s after the frontend entry; it steps through five levels, the period is the lag
    of the autocorrelation maximum over the log time (M3d review finding 13; the level transitions of a 120 fps
    series sampled every second frame gave 1.103 s instead of 1.0 s)

Usage:
    python tools/timing_compare.py --run run/m3b/dur50 --run run/m3b/dur60 [--xemu run/xemu_ref]
Needs Pillow.
"""
import argparse
import re
import statistics
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

LOG_TIME = r"\[\s*([\d.]+)\]"
STEAM_BOX = (515, 105, 600, 160)
# scene -> (xemu reference, rows to ignore at the top, mean-difference limit). The frontend is animated (limit 10:
# its own xemu captures differ by 2.4-6.2 from the reference, every other scene by more than 30).
SCENE_REFS = {"legal": ("ref_legal.png", 24, 3.0), "tech_notice": ("ref_tech_notice.png", 0, 3.0),
              "loading": ("ref_splash.png", 0, 3.0), "frontend": ("ref_frontend.png", 0, 10.0),
              "loading_attract": ("ref_splash_attract.png", 0, 3.0)}
LOOKUPS = {
    "legal": r'archive lookup\s+"images\\[a-z]+\\legal\.nif"',
    "movies": r'archive lookup\s+"Video/fox-PAL\.txt"',
    "tech_notice": r'archive lookup\s+"images\\[a-z]+\\NI_Splash_PAL\.nif"',
    "loading": r'archive lookup\s+"images\\[a-z]+\\Loadmenu\.nif"',
    "intro_movie": r'archive lookup\s+"Video\\intro-PAL\.xmv"',
    "attract_movie": r'archive lookup\s+"Video\\Attract-PAL\.xmv"',
}


def load_refs(xemu: Path) -> dict:
    refs = {}
    for scene, (name, skip, limit) in SCENE_REFS.items():
        p = xemu / name
        if p.exists():
            refs[scene] = (Image.open(p).convert("RGB").resize((640, 480), Image.BOX), skip, limit)
    return refs


def classify(im: Image.Image, refs: dict) -> str:
    im = im.convert("RGB")
    if im.size != (640, 480):
        im = im.resize((640, 480), Image.BOX)
    if ImageStat.Stat(im.convert("L")).mean[0] < 1.0:
        return "black"
    best, best_ratio = "other", 1.0
    for scene, (ref, skip, limit) in refs.items():
        box = (0, skip, 640, 480)
        diff = sum(ImageStat.Stat(ImageChops.difference(im.crop(box), ref.crop(box))).mean) / 3.0
        if diff / limit < best_ratio:
            best, best_ratio = scene, diff / limit
    return best


class Run:
    def __init__(self, directory: Path, refs: dict):
        self.dir = directory
        self.log = (directory / "logs" / "futurama.log").read_text(encoding="utf-8", errors="replace")
        trace = directory / "logs" / "vfs_trace.log"
        self.trace = trace.read_text(encoding="utf-8", errors="replace") if trace.exists() else ""
        m = re.search(r"game timing: (\d+) Hz", self.log)
        self.tick = int(m.group(1)) if m else 0
        m = re.search(r"present: mode (\w+) \(tick \d+ Hz, target ([\d.]+) Hz\)", self.log)
        self.mode, self.rate = (m.group(1), float(m.group(2))) if m else ("?", float(self.tick))
        self.contexts = [(float(m.group(1)), m.group(2), m.group(3), int(m.group(4))) for m in re.finditer(
            LOG_TIME + r"[^\n]*game: context (\S+) -> (\S+) \(CApp::Frame #(\d+)\)", self.log)]
        self.lookups = {}
        for key, rx in LOOKUPS.items():
            self.lookups[key] = [float(m.group(1)) for m in re.finditer(LOG_TIME + r"[^\n]*" + rx, self.trace)]
        self.shots = {}
        for m in re.finditer(LOG_TIME + r"[^\n]*screenshot: (.*frame_(\d{6})\.png) \(640x480\)", self.log):
            self.shots[int(m.group(3))] = (float(m.group(1)), Path(m.group(2)))
        self.refs = refs
        self._classes = {}

    def cls(self, frame: int) -> str:
        if frame not in self._classes:
            self._classes[frame] = classify(Image.open(self.shots[frame][1]), self.refs)
        return self._classes[frame]

    def t(self, frame: int) -> float:
        return self.shots[frame][0]

    def switch(self, a: str, b: str, nth: int = 0):
        hits = [c for c in self.contexts if c[1] == a and c[2] == b]
        return hits[nth] if len(hits) > nth else None

    def screen_span(self, scene: str):
        """(first frame showing `scene`, first frame after it or None, whether the frame before the first one was
        captured too). The end counts only if the scene's last frame and the next frame are both captured."""
        frames = sorted(self.shots)
        first = next((f for f in frames if self.cls(f) == scene), None)
        if first is None:
            return None
        after = None
        for f in frames:
            if f > first and self.cls(f) != scene:
                prev = max(g for g in frames if g < f)
                if self.cls(prev) == scene and f == prev + 1:
                    after = f
                    break
        prev_first = max((g for g in frames if g < first), default=None)
        exact_start = prev_first is not None and prev_first == first - 1
        return first, after, exact_start

    def fade(self):
        entry = self.switch("Intro", "Frontend")
        if entry is None:
            return None
        frames = [f for f in sorted(self.shots) if self.t(f) >= entry[0] - 0.5]
        lum = {f: ImageStat.Stat(Image.open(self.shots[f][1]).convert("L")).mean[0] for f in frames}
        # First frontend frame: the first lit frame after the black context-switch frames.
        start = next((f for f in frames if lum[f] >= 0.5 and self.t(f) >= entry[0] - 0.05 and
                      lum.get(f - 1, 99.0) < 1.0), None)
        if start is None:
            return None
        window = []  # the consecutive screenshots after `start`
        while start + len(window) + 1 in lum:
            window.append(start + len(window) + 1)
        late = [lum[f] for f in window if self.t(f) - self.t(start) >= 1.05]
        if len(late) < 5:
            return None
        plateau = statistics.median(late)
        end = next((f for f in window if lum[f] >= 0.97 * plateau), None)
        if end is None:
            return None
        return start, end, plateau, lum[start + 1] / plateau if start + 1 in lum else None

    def steam(self):
        # The longest run of equally spaced screenshots at least 3 s after the frontend entry (after the fade).
        entry = self.switch("Intro", "Frontend")
        if entry is None:
            return None
        frames = [f for f in sorted(self.shots) if self.t(f) >= entry[0] + 3.0]
        best, series = [], []
        for f in frames:
            if len(series) >= 2 and f - series[-1] != series[1] - series[0]:
                best = max(best, series, key=len)
                series = [series[-1]]
            series.append(f)
        best = max(best, series, key=len)
        if len(best) < 20:
            return None
        # The cloud steps through five brightness levels (22.2, 24.8, 39.1, 35.8, 28.5 at 50 Hz, finding 7). The
        # period is the lag of the autocorrelation maximum of the luminance over the log time (M3d review finding 13:
        # level transitions sampled every second frame at 120 fps, with the interpolated in-between levels, gave
        # 1.103 s instead of 1.0 s - a measuring artefact). Resampled on a uniform time grid by linear interpolation;
        # the first lag whose correlation reaches 90 % of the best one between 0.3 s and half the series (refined by a
        # parabola through its neighbours) is the period, so a multiple of the period is not picked.
        samples = [(self.t(f), ImageStat.Stat(Image.open(self.shots[f][1]).convert("L").crop(STEAM_BOX)).mean[0])
                   for f in best]
        period = autocorrelation_period(samples, 0.3, 3.0)
        if period is None:
            return None
        seconds, correlation = period
        span = samples[-1][0] - samples[0][0]
        return int(span / seconds), seconds * self.rate, seconds, best[1] - best[0], correlation


def autocorrelation_period(samples: list, min_lag: float, max_lag: float):
    """(period in seconds, normalised correlation) of a series [(time, value)] or None."""
    if len(samples) < 20:
        return None
    t0, t1 = samples[0][0], samples[-1][0]
    span = t1 - t0
    max_lag = min(max_lag, span / 2.0)
    if span <= 0.0 or max_lag <= min_lag:
        return None
    intervals = sorted(b[0] - a[0] for a, b in zip(samples, samples[1:]) if b[0] > a[0])
    step = max(0.002, (intervals[len(intervals) // 2] if intervals else 0.01) / 2.0)
    grid = []
    j = 0
    t = t0
    while t <= t1:
        while j + 1 < len(samples) - 1 and samples[j + 1][0] <= t:
            j += 1
        (ta, va), (tb, vb) = samples[j], samples[j + 1]
        grid.append(va if tb <= ta else va + (vb - va) * (t - ta) / (tb - ta))
        t += step
    mean = statistics.fmean(grid)
    x = [v - mean for v in grid]
    energy = sum(v * v for v in x)
    if energy <= 0.0:
        return None
    lo, hi = int(min_lag / step), int(max_lag / step)
    corr = {}
    for lag in range(max(1, lo - 1), hi + 2):
        n = len(x) - lag
        if n <= 0:
            break
        corr[lag] = sum(x[i] * x[i + lag] for i in range(n)) / energy * len(x) / n
    peaks = [lag for lag in range(lo, hi + 1)
             if lag in corr and lag - 1 in corr and lag + 1 in corr and corr[lag] >= corr[lag - 1]
             and corr[lag] >= corr[lag + 1]]
    if not peaks:
        return None
    best = max(corr[p] for p in peaks)
    lag = next(p for p in peaks if corr[p] >= 0.9 * best)
    a, b, c = corr[lag - 1], corr[lag], corr[lag + 1]
    denom = a - 2.0 * b + c
    shift = 0.5 * (a - c) / denom if denom != 0.0 else 0.0
    return (lag + max(-0.5, min(0.5, shift))) * step, b


def fmt(v, unit="s", digits=3):
    return "-" if v is None else f"{v:.{digits}f} {unit}"


def measure(run: Run) -> list:
    """[(observable, seconds or None, frames or None, note)]."""
    rows = []
    lk = run.lookups
    rate = run.rate

    def span(a, b):
        return (b - a) if a is not None and b is not None else None

    def first(key, nth=0):
        v = lk.get(key, [])
        return v[nth] if len(v) > nth else None

    legal = span(first("legal"), first("movies"))
    rows.append(("8.4 legal screen (logic: legal.nif -> first logo movie)", legal, legal * rate if legal else None,
                 "QPC 6.0 s"))
    sp = run.screen_span("legal")
    if sp and sp[1]:
        d = run.t(sp[1]) - run.t(sp[0])
        rows.append(("8.4 legal screen (screen: first legal frame -> first frame after it)", d, sp[1] - sp[0],
                     f"frames {sp[0]}..{sp[1] - 1}" + ("" if sp[2] else " (start: first screenshot)")))
    movies = span(first("movies"), first("tech_notice"))
    rows.append(("    logo movies fox/sci/uds (XMV stub: 2 frames each)", movies, movies * rate if movies else None,
                 "frames (M6: movie length)"))
    tech = span(first("tech_notice"), first("loading"))
    rows.append(("8.5 tech notice NI_Splash_PAL.nif (logic)", tech, tech * rate if tech else None, "QPC max 5.0 s"))
    sp = run.screen_span("tech_notice")
    if sp and sp[1]:
        rows.append(("8.5 tech notice (screen)", run.t(sp[1]) - run.t(sp[0]), sp[1] - sp[0],
                     f"frames {sp[0]}..{sp[1] - 1}" + ("" if sp[2] else " (start: first screenshot)")))
    load = span(first("loading"), first("intro_movie"))
    rows.append(("    loading screen Loadmenu.nif (logic: -> intro movie)", load, load * rate if load else None,
                 "async queue once per frame"))
    sp = run.screen_span("loading")
    if sp and sp[1]:
        rows.append(("    loading screen (screen)", run.t(sp[1]) - run.t(sp[0]), sp[1] - sp[0],
                     f"frames {sp[0]}..{sp[1] - 1}" + ("" if sp[2] else " (start: first screenshot)")))
    entry = run.switch("Intro", "Frontend")
    intro_skip = span(first("intro_movie"), entry[0] if entry else None)
    rows.append(("    intro movie skip + context switch", intro_skip, intro_skip * rate if intro_skip else None,
                 "frames"))
    rows.append(("8.6 Intro -> Frontend (from process start)", entry[0] if entry else None, None,
                 f"CApp::Frame #{entry[3]}" if entry else ""))
    if entry and first("legal") is not None:
        rows.append(("8.6 Intro -> Frontend (from legal.nif)", entry[0] - first("legal"),
                     (entry[0] - first("legal")) * rate, "sum"))
    f = run.fade()
    if f:
        start, end, plateau, second = f
        rows.append(("8.7 frontend fade (first frontend frame -> within 3 % of the plateau)",
                     run.t(end) - run.t(start), end - start,
                     f"frames {start}..{end}, plateau luminance {plateau:.1f}, 2nd frame at {second * 100:.0f} %"))
    att = run.switch("Frontend", "Attract")
    if entry and att:
        rows.append(("8.8 frontend idle -> Attract (1st cycle)", att[0] - entry[0], att[3] - entry[3],
                     "dt 60.0 s (CApp::Frame calls)"))
    back = run.switch("Attract", "Frontend")
    if att and back:
        rows.append(("8.9 Attract -> Frontend (movie stub)", back[0] - att[0], None, "8 clear frames + 2"))
    att2 = run.switch("Frontend", "Attract", 1)
    if att and att2:
        rows.append(("8.10 attract cycle (Frontend -> Attract, consecutive)", att2[0] - att[0], att2[3] - att[3],
                     "60 s + reload"))
    if back and att2:
        rows.append(("8.10 frontend after attract (incl. reload) -> Attract", att2[0] - back[0], att2[3] - back[3],
                     "reload frame-bound + 60 s"))
    st = run.steam()
    if st:
        cycles, pf, pt, step, correlation = st
        rows.append(("8.11 steam cloud period (frontend)", pt, pf,
                     f"autocorrelation over {cycles} cycles (correlation {correlation:.2f}), sampled every {step} "
                     f"frame(s)"))
    movies_opened = sorted({m.group(1) for m in re.finditer(r'NtCreateFile\s+"\\\?\?\\D:\\Video\\([^"]+\.xmv)"',
                                                            run.trace)})
    rows.append(("8.12 movie names", None, None, ", ".join(movies_opened)))
    return rows


def xemu_timeline(xemu: Path, refs: dict) -> list:
    """[(scene, first capture, last capture, previous capture, next capture)] of the 3 s captures."""
    caps = []
    for p in sorted(xemu.glob("t*.png")):
        try:
            t = float(p.stem[1:-1])
        except ValueError:
            continue
        caps.append((t, classify(Image.open(p), refs)))
    out = []
    for scene in SCENE_REFS:
        idx = [i for i, (_, c) in enumerate(caps) if c == scene]
        if not idx:
            continue
        # first contiguous block
        block = [idx[0]]
        for i in idx[1:]:
            if i == block[-1] + 1:
                block.append(i)
        a, b = block[0], block[-1]
        out.append((scene, caps[a][0], caps[b][0], caps[a - 1][0] if a > 0 else None,
                    caps[b + 1][0] if b + 1 < len(caps) else None))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", type=Path, action="append", required=True, help="run directory (<userdata>)")
    ap.add_argument("--xemu", type=Path, default=Path(__file__).resolve().parent.parent / "run" / "xemu_ref")
    args = ap.parse_args()
    refs = load_refs(args.xemu)
    if not refs:
        print(f"timing_compare: no xemu references in {args.xemu} - screen classification off")
    runs = [Run(d.resolve(), refs) for d in args.run]
    tables = [measure(r) for r in runs]
    names = [row[0] for row in tables[0]]
    header = " | ".join(f"{r.dir.name} ({r.tick} Hz tick, {r.mode} {r.rate:.0f} Hz)" for r in runs)
    two = len(runs) == 2
    print(f"observable | {header}" + (f" | {runs[1].dir.name} - {runs[0].dir.name}" if two else ""))
    for name in names:
        cells, secs = [], []
        for table in tables:
            row = next((x for x in table if x[0] == name), None)
            if row is None:
                cells.append("-")
                secs.append(None)
                continue
            _, sec, frames, note = row
            secs.append(sec)
            text = fmt(sec) if sec is not None else ""
            if frames is not None:
                text += f" ({frames:.0f} frames)"
            if note:
                text += f" [{note}]"
            cells.append(text.strip())
        if two:
            a, b = secs
            cells.append(f"{(b - a) * 1000:+.0f} ms ({b / a:.3f}x)" if a and b else "")
        print(f"{name} | " + " | ".join(cells))
    if refs:
        print("xemu (PAL 50 Hz, captures every 3 s; real movies): scene first..last capture, bounds of the duration")
        for scene, a, b, prev, nxt in xemu_timeline(args.xemu, refs):
            lo = b - a
            hi = (nxt - prev) if prev is not None and nxt is not None else None
            print(f"  {scene}: captures {a:.1f}..{b:.1f} s (previous {prev}, next {nxt}) -> duration {lo:.1f}.."
                  f"{hi if hi is None else f'{hi:.1f}'} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
