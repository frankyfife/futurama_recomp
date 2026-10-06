"""M3d frame clock: evaluation of the per-frame traces of the experiment build (analysis/loader/m3d_frameclock.md 2).

The experiment build (a patched COPY of src/ under run/analyse_m3d_clock/src_exp, see patch_exp.py there; the
repository sources stay unchanged) writes one CSV line per CApp::Frame (env FUT_M3D_TRACE):
  entry_qpc   QPC when CApp::Frame was entered (= right after the pacer's wait of the previous frame)
  sample_qpc  QPC in the Game::Frame accumulator sample (return 0xA2C6F), after ClearBuffer, movie and pumps
  real / v    the timer value the guest read / the value the frame clock returned
  last_prev   CApp+0xC8 before this frame, last_after = CApp+0xC8 after the fixed-step loop (render branch)
  leftover    v - last_after (the guest's remainder), alpha = leftover / ticks(dt)
  steps       fixed steps in this frame (probe:Game::FixedStep)
  frame_delta g_frameDelta [0x2E7B88] after the frame (ms)
  maint41/201 the call counters of FUN_000A46D0 [0x2E8E48]/[0x2E8E44]; postinter = [0x2EA99C]
  ctx, draws, color_clear: context name, draw calls and colour clear of the frame (GpuRenderer::DisplayFrame)

Usage: python m3d_clock_trace.py <run dir> [<run dir> ...]   (prints to stdout; m3d_clock_trace.out keeps a copy)
"""
import csv
import os
import statistics
import sys
from collections import Counter, defaultdict

QPF = 10_000_000.0


def load(path):
    rows = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            for k in ("frame", "entry_qpc", "sample_qpc", "real", "v", "last_prev", "last_after", "leftover", "step",
                      "steps", "maint41", "maint201", "postinter", "draws", "color_clear", "align_shift"):
                r[k] = int(r[k])
            r["alpha"] = float(r["alpha"])
            r["frame_delta_ms"] = float(r["frame_delta_ms"])
            rows.append(r)
    return rows


def pct(values, p):
    v = sorted(values)
    if not v:
        return float("nan")
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


def report(run, out):
    def p(*a):
        print(*a)
        print(*a, file=out)

    rows = load(os.path.join(run, "frames.csv"))
    p("=" * 100)
    p("run %s: %d frames" % (run, len(rows)))
    steps = Counter(min(r["steps"], 3) for r in rows)
    p("steps per frame: 0 x %d, 1 x %d, 2 x %d, 3+ x %d" % (steps[0], steps[1], steps[2], steps[3]))
    # step pattern: run lengths of the 1/0 alternation
    pattern = "".join(str(min(r["steps"], 9)) for r in rows)
    breaks = sum(1 for a, b in zip(pattern, pattern[1:]) if a == b)
    p("consecutive frames with the same step count (pattern breaks of 1/0 or 1/1): %d" % breaks)
    # alpha
    alphas = [r["alpha"] for r in rows if r["alpha"] >= 0]
    p("alpha: min %.6f max %.6f" % (min(alphas), max(alphas)))
    buckets = Counter(round(a, 2) for a in alphas)
    p("alpha rounded to 0.01 (top 8): " + ", ".join("%.2f x %d" % kv for kv in buckets.most_common(8)))
    by_steps = defaultdict(list)
    for r in rows:
        if r["alpha"] >= 0:
            by_steps[min(r["steps"], 3)].append(r["alpha"])
    for k in sorted(by_steps):
        v = by_steps[k]
        p("  frames with %d step(s): alpha median %.6f, p1 %.6f, p99 %.6f, n %d" % (k, statistics.median(v),
                                                                                  pct(v, 1), pct(v, 99), len(v)))
    leftovers = Counter(r["leftover"] for r in rows)
    p("leftover (ticks) most common: " + ", ".join("%d x %d" % kv for kv in leftovers.most_common(8)))
    # remainder modulo half a step (the alignment of the alternation)
    if rows and rows[0]["step"] > 0:
        q = rows[0]["step"] // 2
        mod = Counter((r["leftover"] % q) for r in rows if r["leftover"] >= 0)
        p("leftover mod q (q = %d): " % q + ", ".join("%d x %d" % kv for kv in mod.most_common(6)))
    # frame delta
    fds = Counter(round(r["frame_delta_ms"], 3) for r in rows)
    p("g_frameDelta (ms) most common: " + ", ".join("%.3f x %d" % kv for kv in fds.most_common(6)))
    # clock phase real - v and sample delay
    phase = [(r["real"] - r["v"]) / QPF * 1000.0 for r in rows if r["v"] != 0]
    delay = [(r["sample_qpc"] - r["entry_qpc"]) / QPF * 1000.0 for r in rows if r["sample_qpc"] and r["entry_qpc"]]
    p("phase real - V (ms): min %.3f p1 %.3f median %.3f p99 %.3f max %.3f" % (min(phase), pct(phase, 1),
                                                                             statistics.median(phase), pct(phase, 99),
                                                                             max(phase)))
    p("sample delay = sample_qpc - CApp::Frame entry (ms): median %.3f p90 %.3f p99 %.3f p99.9 %.3f max %.3f" % (
        statistics.median(delay), pct(delay, 90), pct(delay, 99), pct(delay, 99.9), max(delay)))
    by_ctx = defaultdict(list)
    for r in rows:
        by_ctx[r["ctx"]].append((r["sample_qpc"] - r["entry_qpc"]) / QPF * 1000.0)
    for c, v in by_ctx.items():
        p("  sample delay in %-10s n %5d median %.3f p99 %.3f max %.3f ms" % (c, len(v), statistics.median(v),
                                                                             pct(v, 99), max(v)))
    # entry intervals (frame start jitter)
    ent = [(b["entry_qpc"] - a["entry_qpc"]) / QPF * 1000.0 for a, b in zip(rows, rows[1:])]
    p("CApp::Frame entry intervals (ms): median %.3f p1 %.3f p99 %.3f max %.3f" % (
        statistics.median(ent), pct(ent, 1), pct(ent, 99), max(ent)))
    # frame-bound maintenance: counter increments per frame
    inc41 = Counter(((b["maint41"] - a["maint41"]) % 41) for a, b in zip(rows, rows[1:]))
    p("FUN_000A46D0 counter [0x2E8E48] increments per frame (mod 41): " + ", ".join("%d x %d" % kv for kv in
                                                                                     inc41.most_common(4)))
    wraps41 = sum(1 for a, b in zip(rows, rows[1:]) if b["maint41"] < a["maint41"])
    wraps201 = sum(1 for a, b in zip(rows, rows[1:]) if b["maint201"] < a["maint201"])
    secs = (rows[-1]["entry_qpc"] - rows[0]["entry_qpc"]) / QPF
    p("coalescing runs: FUN_000A3CE0 %d (%.2f/s), FUN_000A3B10 %d (%.2f/s) over %.1f s" % (
        wraps41, wraps41 / secs, wraps201, wraps201 / secs, secs))
    # draws vs steps per context
    tab = defaultdict(Counter)
    for r in rows:
        tab[r["ctx"]][(min(r["steps"], 2), "draw" if r["draws"] else ("clear" if r["color_clear"] else "EMPTY"))] += 1
    p("frames per context by (steps, picture): EMPTY = no draw and no colour clear (shows the picture of two "
      "presents ago)")
    for c, cnt in tab.items():
        p("  %-10s " % c + ", ".join("%s/%s %d" % (k[0], k[1], n) for k, n in sorted(cnt.items())))
    # catch-up frames
    big = [r for r in rows if r["steps"] >= 2]
    p("frames with >= 2 steps: %d" % len(big))
    for r in big[:12]:
        p("  frame %d ctx %s steps %d leftover %d alpha %.6f phase %.3f ms delay %.3f ms align %d" % (
            r["frame"], r["ctx"], r["steps"], r["leftover"], r["alpha"], (r["real"] - r["v"]) / QPF * 1000.0,
            (r["sample_qpc"] - r["entry_qpc"]) / QPF * 1000.0, r["align_shift"]))
    al = [r for r in rows if r["align_shift"] != 0]
    p("frames with an alignment shift: %d" % len(al))
    for r in al[:10]:
        p("  frame %d ctx %s shift %d ticks, steps %d, leftover %d alpha %.6f" % (r["frame"], r["ctx"], r["align_shift"],
                                                                                  r["steps"], r["leftover"], r["alpha"]))
    # display-time continuity: V - step * ... -> displayed sim time = last_after + leftover - step = v - step
    # with the per-step history: shown time S = (ticks so far) * step - step + leftover
    total = 0
    shown = []
    for r in rows:
        total += r["steps"]
        if r["leftover"] >= 0 and r["step"] > 0:
            shown.append((total - 1) * r["step"] + r["leftover"])
    d = [b - a for a, b in zip(shown, shown[1:])]
    neg = sum(1 for x in d if x < 0)
    p("shown simulation time (ticks of the guest timer) per frame: median step %d, min %d, max %d, backwards %d" % (
        statistics.median(d), min(d), max(d), neg))
    dc = Counter(d)
    p("  increments most common: " + ", ".join("%d x %d" % kv for kv in dc.most_common(6)))


def main():
    runs = sys.argv[1:]
    out = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "m3d_clock_trace.out"), "w", encoding="utf-8")
    for run in runs:
        report(run, out)
    out.close()


if __name__ == "__main__":
    main()
