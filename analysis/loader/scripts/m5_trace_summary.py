"""Summary of a DSOUND/XMV call trace of the existing stub build ([debug] trace = snd,vid, [audio]
dummy_streams = 1, input script tools/input_scripts/m4_newgame.txt: intro, frontend, Level 1-1).
Input:  run/analyse_m5/trace_level/logs/futurama.log (produced by build/m4-final-rel)
Output: scripts/out/m5_trace_level.out (first call per site with arguments, call counts per entry, ticks).
Note: the stubs return NULL buffers, so GetStatus reports 0 and every buffer voice ends after one tick;
buffer-voice counts are therefore inflated (each looping emitter is recreated) - use the per-tick listener
calls and the stream lines, not the buffer counts, as rates."""
import re
import sys
from m5_common import Tee, out_path, RUN
import os

log = os.path.join(RUN, "trace_level", "logs", "futurama.log")
T = Tee(out_path("m5_trace_level.out"))
lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
T("source: run/analyse_m5/trace_level/logs/futurama.log (%d lines)" % len(lines))
T("\n== first call per call site (time, entry(args) -> result, return address)")
for l in lines:
    m = re.search(r"\[\s*([\d.]+)\].*(SND|VID)\s+DEBUG XDK (DSOUND|XMV)::(\w+)\((.*)\) ret=(0x[0-9A-F]+) (\S+)", l)
    if m:
        T("  %8s %-6s %-36s %s  @%s %s" % (m.group(1), m.group(3), m.group(4), m.group(5)[:150], m.group(6), m.group(7)))
T("\n== stream lines")
for l in lines:
    if "stub stream" in l or "XMV: movie" in l:
        T("  " + l[l.find("]") + 1:].strip()[:200])
T("\n== call counts")
for l in lines:
    m = re.search(r"XDK\s+(DSOUND|XMV)::(\w+)\s+(\d+) calls, (\d+) sites", l)
    if m:
        T("  %-6s %-40s %7s calls %3s sites" % m.groups())
for l in lines:
    if "XDK DSOUND stubs:" in l or "stub streams:" in l or "process exit" in l:
        T("  " + l[l.find("]") + 1:].strip())
