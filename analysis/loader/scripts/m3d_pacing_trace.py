"""M3d pacing: latency and vblank phase from [render] pacing_trace CSVs (analysis/loader/m3d_frameclock.md 3).

For every frame of a vblank-coupled plan the trace has: the frame start (end of the pacer's wait), the end of the
frame's work, the Present call, its present id and one DXGI frame-statistics sample (newest visible present id,
its vblank count and the QPC of that vblank). Joining a frame's present id with a later sample that names it
gives the vblank at which the frame became visible:
  start -> visible      the latency of everything that is sampled at the frame start (input in the tick loop)
  present -> visible    the part after the work
  start phase           position of the frame start in the refresh grid (fraction of a refresh after the vblank
                        the sample names; nominal refresh from the stat fit)
Usage: python m3d_pacing_trace.py <run dir> [...]
"""
import csv
import os
import statistics
import sys
from collections import Counter

QPF = 1e7


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))] if v else float("nan")


def analyse(run):
    rows = []
    with open(os.path.join(run, "pacing_trace.csv"), encoding="utf-8") as f:
        lines = [ln for ln in f if not ln.startswith("#")]
    for r in csv.DictReader(lines):
        for k in ("frame", "sync", "presented", "frame_start_qpc", "work_done_qpc", "present_call_qpc",
                  "present_return_qpc", "present_id", "stats_valid", "stat_present_count", "stat_sync_refresh",
                  "stat_sync_qpc", "foreground"):
            r[k] = int(r[k])
        r["wait_ms"] = float(r["wait_ms"])
        rows.append(r)
    visible = {}
    for r in rows:
        if r["stats_valid"] and r["stat_present_count"] not in visible:
            visible[r["stat_present_count"]] = (r["stat_sync_qpc"], r["stat_sync_refresh"])
    # refresh period from the samples (median of d qpc / d refresh over consecutive samples)
    samples = sorted(set(visible.values()), key=lambda x: x[1])
    per = [(b[0] - a[0]) / (b[1] - a[1]) for a, b in zip(samples, samples[1:]) if b[1] > a[1] and b[1] - a[1] <= 4]
    period = statistics.median(per) if per else QPF / 120
    out = []
    for path in sorted({r["path"] for r in rows}):
        sel = [r for r in rows if r["path"] == path and r["presented"] and r["foreground"]]
        lat_start, lat_present, work, phase = [], [], [], []
        for i, r in enumerate(sel):
            v = visible.get(r["present_id"])
            # the frame START of this present is the frame_start of the row (pacing trace: start of this frame)
            if v and r["frame_start_qpc"]:
                lat_start.append((v[0] - r["frame_start_qpc"]) / QPF * 1000)
                lat_present.append((v[0] - r["present_call_qpc"]) / QPF * 1000)
                ph = ((r["frame_start_qpc"] - v[0]) / period) % 1.0
                phase.append(ph)
            if r["frame_start_qpc"] and r["work_done_qpc"]:
                work.append((r["work_done_qpc"] - r["frame_start_qpc"]) / QPF * 1000)
        if not lat_start:
            continue
        out.append("  plan %-12s frames %5d (foreground, presented): start->visible median %.2f p1 %.2f p99 %.2f ms; "
                   "present->visible median %.2f p99 %.2f ms; work median %.2f p99 %.2f ms; wait median %.2f ms; "
                   "start phase in the refresh (0 = vblank) median %.3f p1 %.3f p99 %.3f" % (
                       path, len(sel), statistics.median(lat_start), pct(lat_start, 1), pct(lat_start, 99),
                       statistics.median(lat_present), pct(lat_present, 99), statistics.median(work), pct(work, 99),
                       statistics.median([r["wait_ms"] for r in sel]), statistics.median(phase), pct(phase, 1),
                       pct(phase, 99)))
        refreshes = Counter(round(x / (period / QPF * 1000)) for x in lat_start)
        out.append("    start->visible in refreshes: " + ", ".join("%d x %d" % kv for kv in sorted(refreshes.items())))
    print("%s: refresh period %.4f ms (%.3f Hz)" % (run, period / QPF * 1000, QPF / period))
    for line in out:
        print(line)


for run in sys.argv[1:]:
    analyse(run)
