"""M3d per-frame code audit (analysis/loader/m3d_frameclock.md, section 1).

Which guest code runs once per CApp::Frame / Game::Frame outside the fixed-step loop, and what does it change
per call? At 120 fps with a 60 Hz tick every second frame runs 0 steps, so everything below runs twice per tick.

Method (read-only on the XBE and the Ghidra export, capstone index of timing_common incl. gap code):
  1. Call sites of CApp::Frame 0xD2630 and Game::Frame 0xA2C20 classified as "pre-loop", "loop" (per fixed
     step) and "render branch" from the instruction addresses (loop = 0xA2D90..0xA2F22, render = 0xA2F55..).
  2. From every per-frame root: breadth-first walk over DIRECT calls/jumps inside game code (< 0xF6000; calls
     into XAPI/NetImmerse/renderer/CRT are recorded as leaves, not followed), depth <= MAX_DEPTH.
  3. Every reached instruction is checked for: reads of dt [0x2D6748] and g_frameDelta [0x2E7B88], calls of
     rand 0x1668CC, of the script scheduler FUN_000BA060, of NiAVObject::Update FUN_00101840, of the game timer
     FUN_000D2B90, of DisplayFrame/ClearBuffer (vtable +0x40/+0x44 on the renderer), absolute global writes,
     inc/dec of object fields (counters; refcount [reg+4] excluded).
  4. Indirect calls (call [reg+off]) are counted per function; the relevant ones are resolved by hand in the
     report.

Output: m3d_perframe.out (next to this script).
"""
import os
import re
import sys
from collections import defaultdict, deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from timing_common import R, index, func_of, FNAME, owner_name, fmt, decomp  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "m3d_perframe.out")
GAME_END = 0xF6000      # XAPI starts at about 0xF6000 (QPC 0xF641B, XapiInitProcess 0xF92C0)
# memory.cpp pool (new/delete FUN_000A4900/FUN_000A3880, coalescing FUN_000A3B10/FUN_000A3CE0, startup.md 5):
# reached from almost every root through new/delete; recorded as a leaf, not followed (noise)
ALLOCATOR = (0xA3000, 0xA4A00)
MAX_DEPTH = 5

DT = 0x2D6748
FRAME_DELTA = 0x2E7B88
RAND = 0x1668CC
SCRIPT_SCHED = 0xBA060
NI_UPDATE = 0x101840
GAME_TIMER = 0xD2B90

ROOTS = [
    # (group, name, address)
    ("CApp::Frame", "vf8 0xA24A0 Movie (vor Game::Frame)", 0xA24A0),
    ("CApp::Frame", "FUN_000E52D0 Pause-Overlay (nach Game::Frame, wenn Pause+0xA)", 0xE52D0),
    ("CApp::Frame", "FUN_000F2CF0 Debug-Overlay", 0xF2CF0),
    ("Game::Frame vor der Schleife", "FUN_000D2370 Async-Ladewarteschlange", 0xD2370),
    ("Game::Frame vor der Schleife", "FUN_000F2050 Streaming-Singleton", 0xF2050),
    ("Game::Frame vor der Schleife", "FUN_000F2680 Streaming-Pumpe", 0xF2680),
    ("Game::Frame Render-Zweig", "FUN_000A46D0 Speicherpflege", 0xA46D0),
    ("Game::Frame Render-Zweig", "FUN_0002C9C0 Profiler-Reset", 0x2C9C0),
    ("Game::Frame Render-Zweig", "FUN_000D7630 Objekt [0x2EBD0C]", 0xD7630),
    ("Kontext Render (vf3)", "Frontend::Render 0x21B60", 0x21B60),
    ("Kontext Render (vf3)", "Intro::Render 0x2B300", 0x2B300),
    ("Kontext Render (vf3)", "Attract::Render 0xB0390 (leer)", 0xB0390),
    ("Kontext Render (vf3)", "Ingame::Render 0x2A320", 0x2A320),
    ("Kontext Render (vf3)", "Death::Render 0x2BF10", 0x2BF10),
    ("Kontext Render (vf3)", "Intermission::Render 0xD62E0", 0xD62E0),
    # negative control: per fixed step
    ("pro Tick (Vergleich)", "CApp vf10 0xD25D0", 0xD25D0),
]

# ---- index helpers ----------------------------------------------------------------------------------------
IDX = index()
BY_OWNER = defaultdict(list)
BY_ADDR = {}
for e in IDX:
    BY_OWNER[e[5]].append(e)
    BY_ADDR[e[0]] = e
GAP_OWNERS = sorted(-o for o in BY_OWNER if o < 0)


def owner_of(va):
    """Owner key of the index (function start, or -gapstart) for code address va."""
    f = func_of(va)
    if f:
        return f[0]
    e = BY_ADDR.get(va)
    if e:
        return e[5]
    return None


def target(e):
    m = re.fullmatch(r"0x([0-9a-f]+)", e[3])
    return int(m.group(1), 16) if m else None


ABS_MEM = re.compile(r"(?:byte|word|dword|qword) ptr \[0x([0-9a-f]+)\]")
FIELD = re.compile(r"(?:byte|word|dword) ptr \[(e[a-z]{2})(?: \+ 0x([0-9a-f]+))?\]")


def classify(e):
    """Flags of one instruction (list of (tag, text))."""
    flags = []
    mn, ops = e[2], e[3]
    t = target(e) if mn in ("call", "jmp") else None
    if "0x2d6748" in ops:
        flags.append(("DT", "liest/schreibt dt [0x2D6748]"))
    if "0x2e7b88" in ops:
        flags.append(("FRAMEDELTA", "g_frameDelta [0x2E7B88]"))
    if t == RAND:
        flags.append(("RAND", "rand()"))
    if t == SCRIPT_SCHED:
        flags.append(("SCRIPT", "Skript-Scheduler FUN_000BA060"))
    if t == NI_UPDATE:
        flags.append(("NIUPDATE", "NiAVObject::Update FUN_00101840"))
    if t == GAME_TIMER:
        flags.append(("TIMER", "Spieltimer FUN_000D2B90 (QPC)"))
    if mn == "call" and re.fullmatch(r"dword ptr \[e[a-z]{2} \+ 0x4[04]\]", ops):
        flags.append(("VCALL40", "call [vt+0x40/0x44] (Renderer DisplayFrame/ClearBuffer?)"))
    if mn in ("mov", "inc", "dec", "add", "sub", "or", "and", "fstp", "fst", "fistp", "xor"):
        first = ops.split(",")[0]
        m = ABS_MEM.search(first)
        if m:
            a = int(m.group(1), 16)
            if 0x2B0000 <= a < 0x320000:
                flags.append(("GWRITE", "schreibt Global 0x%X" % a))
    if mn in ("inc", "dec"):
        m = FIELD.fullmatch(ops)
        if m and m.group(2) not in (None, "4"):
            flags.append(("FIELDCNT", "%s Feld +0x%s" % (mn, m.group(2))))
    if mn in ("add", "sub") and re.match(r"dword ptr \[e[a-z]{2} \+ 0x[0-9a-f]+\], (?:1|0xffffffff)$", ops):
        flags.append(("FIELDCNT", "%s Feld (Zähler)" % mn))
    return flags


def walk(root):  # noqa: C901
    """BFS over direct calls in game code. Returns {owner: depth}, leaves {target: count}, indirect {owner: n}."""
    seen = {}
    leaves = defaultdict(int)
    indirect = defaultdict(int)
    o = owner_of(root)
    if o is None:
        return seen, leaves, indirect
    q = deque([(o, 0)])
    seen[o] = 0
    while q:
        cur, d = q.popleft()
        for e in BY_OWNER.get(cur, []):
            if e[2] not in ("call", "jmp"):
                continue
            t = target(e)
            if t is None:
                if e[2] == "call":
                    indirect[cur] += 1
                continue
            if e[2] == "jmp" and owner_of(t) == cur:
                continue
            if t >= GAME_END or (ALLOCATOR[0] <= t < ALLOCATOR[1] and root != 0xA46D0):
                leaves[t] += 1
                continue
            to = owner_of(t)
            if to is None or to in seen:
                continue
            if d + 1 > MAX_DEPTH:
                leaves[t] += 1
                continue
            seen[to] = d + 1
            q.append((to, d + 1))
    return seen, leaves, indirect


def main():
    out = open(OUT, "w", encoding="utf-8")

    def p(*a):
        print(*a, file=out)

    p("# m3d_perframe.py - per-frame code audit (M3d)")
    p()
    p("## 1. Call sites in CApp::Frame 0xD2630 and Game::Frame 0xA2C20")
    p()
    for start, size in ((0xD2630, 0x5C), (0xA2C20, 0x46B)):
        for e in BY_OWNER[start]:
            if e[2] != "call":
                continue
            a = e[0]
            if start == 0xD2630:
                where = "CApp::Frame"
            elif a < 0xA2D84:
                where = "vor der Schleife"
            elif a < 0xA2F55:
                where = "Schleife (pro Tick)" if a < 0xA2DEB or a >= 0xA2EFD else "Schleife, Kontextwechsel"
            else:
                where = "Render-Zweig"
            t = target(e)
            name = FNAME.get(t, "") if t else ""
            p("  %08X  %-30s %-28s %s" % (a, e[3], where, name))
    p()
    p("## 2. Reachable game code from the per-frame roots (direct calls, depth <= %d)" % MAX_DEPTH)
    totals = {}
    for group, name, root in ROOTS:
        seen, leaves, indirect = walk(root)
        p()
        p("### %s - %s (%d Funktionen/Segmente, %d indirekte Aufrufe)" % (group, name, len(seen),
                                                                          sum(indirect.values())))
        tags = defaultdict(list)
        for o, d in sorted(seen.items(), key=lambda kv: (kv[1], abs(kv[0]))):
            for e in BY_OWNER.get(o, []):
                for tag, txt in classify(e):
                    tags[tag].append((d, o, e, txt))
        totals[name] = {k: len(v) for k, v in tags.items()}
        for tag in ("DT", "FRAMEDELTA", "RAND", "SCRIPT", "NIUPDATE", "TIMER", "VCALL40", "GWRITE", "FIELDCNT"):
            items = tags.get(tag, [])
            if not items:
                continue
            p("  [%s] %d Stellen" % (tag, len(items)))
            for d, o, e, txt in items[:60]:
                p("    d%d %-14s %s | %s" % (d, owner_name(o), fmt(e)[:96], txt))
            if len(items) > 60:
                p("    ... %d weitere" % (len(items) - 60))
        lv = sorted(leaves.items(), key=lambda kv: -kv[1])
        p("  Blätter außerhalb des Spielcodes (Top 25): " + ", ".join(
            "%s x%d" % (FNAME.get(t, "%06X" % t), n) for t, n in lv[:25]))
        p("  erreichte Funktionen: " + ", ".join(owner_name(o) for o in sorted(seen, key=abs)))
    p()
    p("## 3. Summary (flags per root)")
    for name, t in totals.items():
        p("  %-60s %s" % (name, ", ".join("%s %d" % kv for kv in sorted(t.items()))))
    out.close()
    print("written", OUT)


if __name__ == "__main__":
    main()
