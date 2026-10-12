"""M4 check: scripted input from the main menu into level 1-1 (ROADMAP M4, analysis/loader/m4_input.md 7 and 10).

Runs futurama.exe with an input script ([debug] input_script, default tools/input_scripts/m4_newgame.txt; format in
tools/input_scripts/README.md) and the VFS trace, N times, and checks per run

  M4-1 the script ran to its end: "input script: loaded ...", no "input script: FAILED", the script's own
       "quit 0" and the process exit "input script quit (exit code 0)" before the timeout (not the frame limit)
  M4-2 contexts in order Intro -> Frontend -> Ingame (probe:Game::ContextSwitch lines), the main menu became active
       (probe:Game::MainMenuActivate) before the script pressed A in the Frontend, and the switch to Ingame
       followed the press within --max-press-ticks ticks (the game reacted to the virtual pad)
  M4-3 level 1-1 loaded: the VFS trace opened scripts\\Level1-1.Ful and files under Levels\\Level1-1\\ (archive hits or
       loose hits, no error), "game: level loaded" (probe:Game::LevelReadyTest) after the switch to Ingame
  M4-4 played in the level without a crash: the script reached "controllable" and at least --level-frames frames
       (default 600) passed between that moment and the orderly process end; no error/TRAP/FATAL/UNIMPLEMENTED
       line (warnings are listed as information)
  M4-5 screenshots in the level: every "screenshot <name>" of the script after the level loaded has its PNG
       ("screenshot: <path> (WxH)" after the request), the picture is not trivial (luminance spread, colors, no
       single dominant color; Pillow); the PNGs are copied to <userdata>/m4_check/run_NN_<name>.png - look at them
  M4-6 controller criteria that need no hardware: the script source was sampled once per tick (summary ticks
       within 2 % of the fixed steps since the script started), the game took the script's port as its active pad
       (two "expect active_pad - ok"), and the simulated unplug opened the pause from the "pad removed" call site
       0x2ACD6 (return 0x0002ACDB, ERRCTRLINGAME) - skipped with a note if the script has no unplug
  M4-7 rumble (a second run with --rumble-script, default tools/input_scripts/m4_rumble.txt: main menu -> Optionen
       -> Vibration off and on): the game's test rumble reaches the script's port ("input script: rumble port 1"
       lines) with both motors >= 60000 and fades to 0 within 30..120 frames over >= 3 changes; the fade needs a new
       XInputSetState per change, which the game sends only while dwStatus != ERROR_IO_PENDING - so it also shows the
       synchronous dwStatus completion. Skipped with --no-rumble
  M4-8 keyboard/mouse and XInput backends without hardware (input layer log lines): the keyboard/mouse source is
       attached to the game window, the XInput backend reports its state ("XInput xinput1_4.dll (N of 4 controllers
       connected)" or "not available"), port 1 was connected before the script's first press (no controller needed),
       and the input summary counts XInputGetState calls with 0 failures

  M4-9 a broken input script ends the start with exit code 1 and an error line (M4 review finding 1: before the fix
       the process died with 0xC0000409 inside the exit reporters): two short runs, a script with an unknown command
       in line 2 and a script path that does not exist (skipped with --no-broken)

Information only (never a failure): rendering gaps seen in the level - "d3d11: first <kind> draw -> <outcome>" lines
(outcome other than Drawn, with frame >= the level start), the run totals "d3d11 draws <kind>:" with non-drawn
outcomes, renderer slots that ran as generic no-ops (slot set parsed from src/host/gfx/nipc_renderer.cpp, names from
src/host/gen/renderer_slots.gen.inc), texture resolver/conversion gaps, D3D traps; rumble the game sent to the
script's port; input layer lines (XInput backend, keyboard/mouse, bindings); warnings; with --runs > 1 whether every
script line ran at the same tick in all runs (lockstep determinism).

Not checked here (no hardware): real XInput controllers, rumble feel, real keyboard/mouse input - the input layer's
unit tests (tests/input_*_tests.cpp) cover the mapping. Nothing here depends on the display: screenshots work with
the display off (M3d F1), so no --display-optional is needed.

Usage (paths with spaces belong in the INI, not on the command line):
    python tools/m4_check.py --exe build/m4-script/bin/futurama.exe --config run/m4-scripts/futurama.ini
                             [--script tools/input_scripts/m4_newgame.txt] [--runs 1] [--fresh]
                             [--timeout SECONDS] [--level-frames N] [--max-press-ticks N]

The INI must set [paths] userdata and must not set [debug] stop_at. The script adds --trace=vfs (unless set),
--set=debug.input_script=<script> (unless the INI sets it and --script is not given), a frame limit as a safety net
(--frames, default 72000 = 20 min at 60 Hz, unless the INI sets one) and [debug] timeout (watchdog) = --timeout.
Logs: <userdata>/m4_check/run_NN.log, run_NN_vfs_trace.log, run_NN_<screenshot>.png. Exit code 0 when all pass.
"""
import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m2_check  # noqa: E402  (config parsing, VFS trace parser)

try:
    from PIL import Image, ImageStat
except ImportError:
    Image = None

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SCRIPT = ROOT / "tools" / "input_scripts" / "m4_newgame.txt"
DEFAULT_RUMBLE_SCRIPT = ROOT / "tools" / "input_scripts" / "m4_rumble.txt"
DEFAULT_TIMEOUT = 900
DEFAULT_FRAME_LIMIT = 72000
DEFAULT_LEVEL_FRAMES = 600
DEFAULT_MAX_PRESS_TICKS = 30
TICK_SAMPLE_TOLERANCE = 0.02
MIN_LUMA_STDDEV = 8.0
MIN_COLORS = 64
MAX_TOP_COLOR_SHARE = 0.98
UNPLUG_PAUSE_RETURN = "0x0002ACDB"  # FUN_000A8220 called at 0x2ACD6: the active pad was removed

CRITERIA = [
    "M4-1 input script ran to its end (quit 0)",
    "M4-2 contexts Intro -> Frontend -> Ingame, main menu active, game reacted to the press",
    "M4-3 level 1-1 files opened and level loaded",
    "M4-4 frames played in the level without crash",
    "M4-5 screenshots in the level",
    "M4-6 controller criteria without hardware (tick sampling, active pad, simulated unplug)",
    "M4-7 rumble reaches the input layer (options menu vibration test, separate run)",
    "M4-8 keyboard/mouse and XInput backends without hardware (attached, port 1 connected, no failed reads)",
    "M4-9 a broken input script (syntax error, missing file) ends the start with exit code 1",
]

LOG_TIME = re.compile(r"^\[\s*([0-9.]+)\]")
SCRIPT_LINE = re.compile(r"input script: line (\d+): (.*) \(tick (\d+), frame (\d+)\)\s*$")
CONTEXT_LINE = re.compile(r"game: context (\S+) -> (\S+) \(CApp::Frame #(\d+)\)")
SHOT_REQUEST = re.compile(r'input script: screenshot "([^"]+)" requested \(frame (\d+)\)')
SHOT_WRITTEN = re.compile(r"screenshot: (.+\.png) \((\d+)x(\d+)\)")
LEVEL_LOADED = re.compile(r"game: level loaded \(level object 0x[0-9A-F]+, frame (\d+)\)")
MAIN_MENU = re.compile(r"game: main menu active \(menu 0x[0-9A-F]+, entry (-?\d+)\)")
PAUSE_OPENED = re.compile(r"game: pause opened \(pause object 0x[0-9A-F]+, called from (0x[0-9A-F]+)\)")
SUMMARY = re.compile(r"input script summary: (\w+) at line (\d+), (\d+) ticks sampled, (\d+) presses, (\d+) pad "
                     r"changes, (\d+) screenshots, rumble (\d+) changes \(max left (\d+), right (\d+)\)")
GAME_SUMMARY = re.compile(r"game summary: (\d+) CApp::Frame calls")
GAME_TICKS = re.compile(r"game ticks: (\d+) fixed steps")
FIRST_DRAW = re.compile(r"d3d11: first (\w+) draw -> (\w+)(?:: (.*?))? \(object 0x[0-9A-F]+, frame (\d+)\)")
DRAW_TOTALS = re.compile(r"d3d11 draws (\w+): (\d+) calls - (.*)$")
SLOT_CALLS = re.compile(r"NiPCRenderer slot calls \(slot:count\):(.*)$")
PROCESS_EXIT = re.compile(r"process exit: (.*) \(exit code (-?\d+)")


def image_stats(path: Path) -> tuple[bool, str]:
    if Image is None:
        return False, "Pillow missing (pip install pillow)"
    im = Image.open(path).convert("RGB")
    luma = ImageStat.Stat(im.convert("L"))
    colors = im.getcolors(maxcolors=1 << 20) or []
    total = im.width * im.height
    top = max((n for n, _ in colors), default=total) / total
    ok = luma.stddev[0] >= MIN_LUMA_STDDEV and len(colors) >= MIN_COLORS and top <= MAX_TOP_COLOR_SHARE
    return ok, (f"{im.width}x{im.height}, luminance mean {luma.mean[0]:.1f} stddev {luma.stddev[0]:.1f}, "
                f"{len(colors)} colors, top color {top * 100:.1f} %")


def renderer_slot_info() -> tuple[set[int], dict[int, str], set[int]]:
    """(slots with an own handler or the original code, slot names, generic slots with a documented fixed result)."""
    implemented: set[int] = set(range(2, 11))  # kFirstOriginalSlot..kLastOriginalSlot
    names: dict[int, str] = {}
    documented = {1, 12, 23, 39, 57, 67, 68, 69, 70}  # NoopResult / comment in nipc_renderer.cpp
    src = ROOT / "src" / "host" / "gfx" / "nipc_renderer.cpp"
    gen = ROOT / "src" / "host" / "gen" / "renderer_slots.gen.inc"
    if src.exists():
        text = src.read_text(encoding="utf-8", errors="replace")
        implemented |= {int(m) for m in re.findall(r"Slot == (\d+)\)\s*\{\s*return Checked", text)}
    if gen.exists():
        for m in re.finditer(r'FUT_RENDERER_SLOT\((\d+), [^,]+, [^,]+, \d+, \d+, "([^"]*)"\)',
                             gen.read_text(encoding="utf-8", errors="replace")):
            names[int(m.group(1))] = m.group(2)
    return implemented, names, documented


def script_lines(log: str) -> list[dict]:
    out = []
    for ln in log.splitlines():
        m = SCRIPT_LINE.search(ln)
        if m:
            out.append({"line": int(m.group(1)), "text": m.group(2), "tick": int(m.group(3)),
                        "frame": int(m.group(4)), "raw": ln})
    return out


def first(lines: list[dict], pred) -> dict | None:
    return next((x for x in lines if pred(x)), None)


def check_run(log: str, trace: str, code: int, elapsed: float, timeout: int, level_frames: int,
              max_press_ticks: int, script_text: str, out_dir: Path, run: int) -> tuple[list, dict]:
    results = []
    info: dict = {"warnings": [], "gaps": [], "input": []}
    lines = log.splitlines()
    sl = script_lines(log)
    info["script_ticks"] = [(x["line"], x["text"], x["tick"]) for x in sl]

    # ---- M4-1
    loaded = next((ln for ln in lines if "input script: loaded " in ln), None)
    failed = next((ln for ln in lines if "input script: FAILED" in ln), None)
    quit_line = first(sl, lambda x: x["text"].startswith("quit "))
    exit_m = None
    for ln in lines:
        m = PROCESS_EXIT.search(ln)
        if m:
            exit_m = m
    ok1 = (loaded is not None and failed is None and quit_line is not None and quit_line["text"] == "quit 0"
           and code == 0 and exit_m is not None and "input script quit" in exit_m.group(1) and elapsed < timeout)
    detail = (f"exit code {code}, {elapsed:.0f} s, "
              + (f"process exit '{exit_m.group(1)}'" if exit_m else "no process exit line")
              + (f"; {failed.split('input script: ', 1)[1][:200]}" if failed else "")
              + ("" if loaded else "; script not loaded"))
    results.append((CRITERIA[0], ok1, detail))

    # ---- M4-2
    contexts = [(m.group(1), m.group(2), int(m.group(3))) for m in map(CONTEXT_LINE.search, lines) if m]
    seq = [c[1] for c in contexts]
    order_ok = False
    if "Frontend" in seq and "Ingame" in seq:
        order_ok = seq.index("Frontend") < seq.index("Ingame") and contexts[0][0] == "Intro"
    menu_line = next((i for i, ln in enumerate(lines) if MAIN_MENU.search(ln)), None)
    ready = first(sl, lambda x: x["text"] == 'condition "main_menu" reached')
    ingame_wait = first(sl, lambda x: x["text"] in ('condition "context Ingame" reached',
                                                    'loop condition "context Ingame" reached'))
    # The confirm that started the game: the last A/START press before the script saw the Ingame context. The
    # switch shows in the game at once (log line), the script sees it at its next test (after its wait).
    presses = [x for x in sl if (x["text"].startswith("press A") or x["text"].startswith("press START"))
               and (ingame_wait is None or x["tick"] <= ingame_wait["tick"])]
    press = presses[-1] if presses else None
    press_idx = next((i for i, ln in enumerate(lines) if press and ln == press["raw"]), None)
    switch_idx = next((i for i, ln in enumerate(lines) if "game: context Frontend -> Ingame" in ln), None)
    react = None
    if press and switch_idx is not None and press_idx is not None and switch_idx > press_idx:
        # ticks from the press to the switch: frames between them at one tick per frame (lockstep at the tick
        # rate); the CApp::Frame number of the switch line minus the press frame
        m = CONTEXT_LINE.search(lines[switch_idx])
        react = int(m.group(3)) - press["frame"] if m else None
    ready_idx = next((i for i, ln in enumerate(lines) if ready and ln == ready["raw"]), None)
    menu_before = ready_idx is not None and press_idx is not None and menu_line is not None and ready_idx < press_idx
    ok2 = order_ok and menu_before and react is not None and 0 <= react <= max_press_ticks
    results.append((CRITERIA[1], ok2,
                    f"contexts {' -> '.join([contexts[0][0]] + seq) if contexts else 'none'}; main menu "
                    f"{'activated' if menu_line is not None else 'never activated'}, "
                    f"{'ready before the confirm' if menu_before else 'not ready before the confirm'}"
                    f"{f' (press at tick {press['tick']}, {len(presses)} presses)' if press else ''}; Frontend -> "
                    f"Ingame {react if react is not None else '-'} frames after the press (<= {max_press_ticks})"))
    info["contexts"] = contexts

    # ---- M4-3
    entries, _ = m2_check.parse_trace(trace)
    level_entries = [e for e in entries if "level1-1" in e["path"].lower().replace("/", "\\")]
    ful = [e for e in level_entries if e["path"].lower().replace("/", "\\").endswith("scripts\\level1-1.ful")]
    lvl_dir = [e for e in level_entries if "levels\\level1-1\\" in e["path"].lower().replace("/", "\\")]
    bad = [e for e in level_entries if m2_check.classify_entry(e) in ("error", "unknown")]
    # Optional files the level asks for and the disc does not have (the game goes on without them): information,
    # to be documented in vfs/vfs_known_missing.cpp (io module).
    info["undocumented_misses"] = sorted({e["path"] for e in entries if m2_check.classify_entry(e) == "undocumented"})
    good = lambda es: [e for e in es if m2_check.classify_entry(e) in ("hit", "archive-hit")]  # noqa: E731
    level_loaded = [int(m.group(1)) for m in map(LEVEL_LOADED.search, lines) if m]
    ingame_frame = next((c[2] for c in contexts if c[1] == "Ingame"), None)
    ok3 = (bool(good(ful)) and bool(good(lvl_dir)) and not bad and bool(level_loaded) and ingame_frame is not None
           and level_loaded[0] >= ingame_frame)
    results.append((CRITERIA[2], ok3,
                    f"{len(level_entries)} Level1-1 trace lines ({len(good(ful))} Level1-1.Ful hits, "
                    f"{len(good(lvl_dir))} under Levels\\Level1-1\\, {len(bad)} errors); level loaded at frame "
                    f"{level_loaded[0] if level_loaded else '-'} (Ingame from frame {ingame_frame})"))
    info["level_files"] = sorted({e["path"] for e in good(level_entries)})
    info["level_start_frame"] = level_loaded[0] if level_loaded else ingame_frame

    # ---- M4-4
    controllable = first(sl, lambda x: x["text"] in ('loop condition "controllable" reached',
                                                     'condition "controllable" reached'))
    total_frames = None
    for ln in lines:
        m = GAME_SUMMARY.search(ln)
        if m:
            total_frames = int(m.group(1))
    played = (total_frames - controllable["frame"]) if (controllable and total_frames is not None) else None
    bad_lines = [ln for ln in lines if re.search(r" (ERROR|FATAL) |TRAP |UNIMPLEMENTED", ln)]
    info["warnings"] = [ln for ln in lines if " WARN " in ln]
    ok4 = controllable is not None and played is not None and played >= level_frames and not bad_lines and code == 0
    results.append((CRITERIA[3], ok4,
                    (f"controllable at frame {controllable['frame']} (tick {controllable['tick']}), "
                     if controllable else "never controllable, ")
                    + f"{played if played is not None else '-'} frames played after that (>= {level_frames}), "
                    + f"{total_frames} frames in total, {len(bad_lines)} error lines"
                    + (f" (first: {bad_lines[0][:160]})" if bad_lines else "")))
    info["controllable_frame"] = controllable["frame"] if controllable else None

    # ---- M4-5
    level_start = info["level_start_frame"] or 0
    shots = []
    for i, ln in enumerate(lines):
        m = SHOT_REQUEST.search(ln)
        if not m or m.group(1) == "fail":
            continue
        name, frame = m.group(1), int(m.group(2))
        written = None
        for ln2 in lines[i + 1:]:
            w = SHOT_WRITTEN.search(ln2)
            if w:
                written = w
                break
        shots.append((name, frame, written))
    shot_ok = True
    shot_details = []
    level_shots = [s for s in shots if s[1] >= level_start]
    for name, frame, written in level_shots:
        if not written:
            shot_ok = False
            shot_details.append(f"{name}: no PNG written")
            continue
        path = Path(written.group(1))
        if not path.exists():
            shot_ok = False
            shot_details.append(f"{name}: {path} missing")
            continue
        dest = out_dir / f"run_{run:02d}_{name}.png"
        shutil.copyfile(path, dest)
        good_pic, stats = image_stats(dest)
        shot_ok &= good_pic
        shot_details.append(f"{name} (frame {frame}) {'ok' if good_pic else 'TRIVIAL'}: {stats} -> {dest}")
    ok5 = shot_ok and bool(level_shots)
    results.append((CRITERIA[4], ok5, f"{len(level_shots)} level screenshots"
                    + ("" if level_shots else " (the script took none after the level loaded)")))
    info["screenshots"] = shot_details

    # ---- M4-6
    summary = None
    for ln in lines:
        m = SUMMARY.search(ln)
        if m:
            summary = m
    steps = None
    for ln in lines:
        m = GAME_TICKS.search(ln)
        if m:
            steps = int(m.group(1))
    started = first(sl, lambda x: True)
    sampled = int(summary.group(3)) if summary else 0
    # The script's tick counter starts with the first sample; the fixed steps count from the start of the game.
    first_tick_frame = None
    m_start = next((re.search(r"input script: started \(\d+ instructions, tick (\d+), frame (\d+)\)", ln)
                    for ln in lines if "input script: started" in ln), None)
    if m_start:
        first_tick_frame = int(m_start.group(2))
    expected = (steps - first_tick_frame) if (steps is not None and first_tick_frame is not None) else None
    sample_ok = expected is not None and expected > 0 and abs(sampled - expected) <= TICK_SAMPLE_TOLERANCE * expected
    active_ok = sum(1 for x in sl if x["text"].startswith("expect active_pad") and x["text"].endswith(" - ok"))
    has_unplug = re.search(r"^\s*unplug\b", script_text, re.MULTILINE) is not None
    unplug_ok = True
    unplug_detail = "script has no unplug (skipped)"
    if has_unplug:
        unplug = first(sl, lambda x: x["text"] == "unplug")
        unplug_idx = next((i for i, ln in enumerate(lines) if unplug and ln == unplug["raw"]), None)
        pauses = [(i, m.group(1)) for i, m in ((i, PAUSE_OPENED.search(ln)) for i, ln in enumerate(lines)) if m]
        after = [p for p in pauses if unplug_idx is not None and p[0] > unplug_idx]
        unplug_ok = bool(after) and after[0][1] == UNPLUG_PAUSE_RETURN
        unplug_detail = (f"pause after the unplug from {after[0][1]}" if after else "no pause after the unplug") + \
            f" (expected {UNPLUG_PAUSE_RETURN}: active pad removed -> ERRCTRLINGAME)"
    ok6 = summary is not None and sample_ok and active_ok >= 2 and unplug_ok
    results.append((CRITERIA[5], ok6,
                    f"{sampled} ticks sampled vs {expected} fixed steps since the script started"
                    f"{'' if started else ' (no script line)'}; active_pad ok x {active_ok}; {unplug_detail}"))
    if summary:
        info["input"].append(f"rumble: {summary.group(7)} changes on the script's port (max left {summary.group(8)}, "
                             f"right {summary.group(9)}); {summary.group(4)} presses, {summary.group(6)} screenshots")
    info["input"] += [ln.split("] ", 1)[-1] for ln in lines
                      if " INP " in ln and "input script" not in ln
                      and re.search(r"xinput|keyboard|mouse|raw input|binding|router|port", ln, re.IGNORECASE)][:25]
    info["input"] += [ln.split("] ", 1)[-1] for ln in lines if "XDK XPP" in ln]

    # ---- information: rendering gaps
    implemented, names, documented = renderer_slot_info()
    for ln in lines:
        m = FIRST_DRAW.search(ln)
        if m and m.group(2) != "Drawn":
            where = "level" if int(m.group(4)) >= level_start else "before the level"
            info["gaps"].append(f"first {m.group(1)} draw -> {m.group(2)}"
                                f"{': ' + m.group(3) if m.group(3) else ''} (frame {m.group(4)}, {where})")
        m = DRAW_TOTALS.search(ln)
        if m:
            parts = [p.strip() for p in m.group(3).split(",")]
            nonzero = [p for p in parts if not p.startswith("drawn") and not re.search(r"\b0$", p)]
            if nonzero:
                info["gaps"].append(f"run totals {m.group(1)}: {m.group(2)} calls, " + ", ".join(nonzero))
        m = SLOT_CALLS.search(ln)
        if m:
            for item in m.group(1).split():
                slot, n = (int(v) for v in item.split(":"))
                if slot not in implemented:
                    kind = "generic no-op with a documented result" if slot in documented else "generic no-op"
                    info["gaps"].append(f"renderer slot {slot} {names.get(slot, '?')}: {n} calls ({kind})")
        if "NiPCRenderer textures:" in ln:
            for pat in (r"(\d+) movie \(black\)", r"(\d+) rendered \(black, P2\)", r"(\d+) unsupported",
                        r"(\d+) no pixel data", r"(\d+) without data"):
                mm = re.search(pat, ln)
                if mm and int(mm.group(1)) > 0:
                    info["gaps"].append("textures: " + mm.group(0))
        if "default 8x8 texture for" in ln:
            mm = re.search(r"default 8x8 texture for (\d+) without image", ln)
            if mm and int(mm.group(1)) > 0:
                info["gaps"].append(f"textures: default 8x8 texture for {mm.group(1)} without image")
        if re.search(r"D3D trap \S+ @0x[0-9A-F]+ hit", ln):
            info["gaps"].append(ln.split("INFO  ", 1)[-1])
    return results, info


def run_game(exe: Path, config: Path, userdata: Path, extra: list[str], timeout: int, clean: bool,
             out_dir: Path, tag: str) -> tuple[int, str, float, str, str]:
    """One game run; returns (exit code, stderr, seconds, log, vfs trace) and keeps copies in out_dir."""
    if clean:
        for sub in ("hdd", "cache", "logs", "screenshots"):
            shutil.rmtree(userdata / sub, ignore_errors=True)
    log_path = userdata / "logs" / "futurama.log"
    trace_path = userdata / "logs" / "vfs_trace.log"
    log_path.unlink(missing_ok=True)
    trace_path.unlink(missing_ok=True)
    start = time.monotonic()
    try:
        proc = subprocess.run([str(exe.resolve()), f"--config={config}", *extra], cwd=userdata,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=timeout + 60)
        code, stderr = proc.returncode, proc.stderr.decode("utf-8", errors="replace").strip()
    except subprocess.TimeoutExpired:
        code, stderr = -1, f"killed after {timeout + 60} s (hang)"
    elapsed = time.monotonic() - start
    log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
    trace_text = trace_path.read_text(encoding="utf-8", errors="replace") if trace_path.exists() else ""
    (out_dir / f"{tag}.log").write_text(log, encoding="utf-8")
    if trace_text:
        (out_dir / f"{tag}_vfs_trace.log").write_text(trace_text, encoding="utf-8")
    return code, stderr, elapsed, log, trace_text


RUMBLE_LINE = re.compile(r"input script: rumble port 1 left (\d+) right (\d+) \(frame (\d+)\)")
ROUTER_RUMBLE = re.compile(r"rumble (\d+) requests \((\d+) non-zero, max (\d+)/(\d+)\)")
XSTATE = re.compile(r"XInputGetState (\d+) \((\d+) failed\), XInputSetState (\d+)")


def check_rumble(log: str, code: int) -> tuple[bool, str]:
    """M4-7: the options menu's vibration test reaches the input layer and fades out (repeated XInputSetState:
    the game sends only while dwStatus != ERROR_IO_PENDING, so the fade proves the synchronous completion)."""
    lines = log.splitlines()
    sl = script_lines(log)
    failed = next((ln for ln in lines if "input script: FAILED" in ln), None)
    quit_ok = any(x["text"] == "quit 0" for x in sl) and code == 0
    changes = [(int(m.group(1)), int(m.group(2)), int(m.group(3))) for m in map(RUMBLE_LINE.search, lines) if m]
    on = next((c for c in changes if c[0] > 0 or c[1] > 0), None)
    off = next((c for c in changes if on and c[2] >= on[2] and c[0] == 0 and c[1] == 0), None)
    peak = max(((c[0], c[1]) for c in changes), default=(0, 0))
    fade = (off[2] - on[2]) if (on and off) else None
    steps = sum(1 for c in changes if on and off and on[2] <= c[2] <= off[2])
    router = next((m for m in map(ROUTER_RUMBLE.search, lines) if m), None)
    xs = next((m for m in map(XSTATE.search, lines) if m), None)
    ok = (quit_ok and failed is None and on is not None and peak[0] >= 60000 and peak[1] >= 60000
          and fade is not None and 30 <= fade <= 120 and steps >= 3)
    fade_text = f"fade to 0 in {fade} frames over {steps} changes" if fade is not None else "no fade to 0"
    detail = (f"exit {code}" + (f"; {failed.split('input script: ', 1)[1][:160]}" if failed else "")
              + f"; {len(changes)} rumble changes on port 1, peak {peak[0]}/{peak[1]}, {fade_text}"
              + (f"; router {router.group(0)}" if router else "")
              + (f"; XInputSetState {xs.group(3)}" if xs else ""))
    return ok, detail


def check_kbm(log: str) -> tuple[bool, str]:
    """M4-8: keyboard/mouse and XInput backends without hardware: the keyboard/mouse source is attached to the game
    window, the XInput backend reports its state, port 1 was connected before the script's first input (no
    controller needed), and XInputGetState never failed."""
    lines = log.splitlines()
    kbm = next((ln for ln in lines if re.search(r"keyboard/mouse (attached to|on) the game window", ln)), None)
    xinput = next((ln for ln in lines if re.search(r"XInput (xinput\S+\.dll|not available)", ln)), None)
    connected = next((i for i, ln in enumerate(lines) if re.search(r"input: port 1 connected", ln)), None)
    first_press = next((i for i, ln in enumerate(lines) if re.search(r"input script: line \d+: press ", ln)), None)
    xs = None
    for m in map(XSTATE.search, lines):
        if m:
            xs = m
    early = connected is not None and first_press is not None and connected < first_press
    ok = (kbm is not None and xinput is not None and early and xs is not None and int(xs.group(1)) > 0
          and int(xs.group(2)) == 0)
    xi = re.search(r"XInput (xinput\S+\.dll \(\d+ of 4 controllers connected\)|not available)", xinput or "")
    detail = (f"keyboard/mouse {'attached' if kbm else 'NOT attached'}; XInput {xi.group(1) if xi else 'state missing'}; "
              f"port 1 {'connected before the first press' if early else 'not connected before the first press'}; "
              + (f"XInputGetState {xs.group(1)} calls, {xs.group(2)} failed" if xs else "no input summary"))
    return ok, detail


def check_broken_scripts(exe: Path, config: Path, userdata: Path, common: list[str], out_dir: Path
                         ) -> tuple[bool, str]:
    """M4-9: a syntax error and a missing script file end the start with exit code 1 (not a crash)."""
    bad = out_dir / "broken_syntax.txt"
    bad.write_text("# M4-9: line 2 is not a command\nbogus_command 3\n", encoding="utf-8")
    cases = [
        ("syntax", bad, 'line 2: unknown command "bogus_command"', "process exit: input script syntax error (exit code 1"),
        ("missing", out_dir / "does_not_exist.txt", "input script: cannot open",
         "process exit: input script not found (exit code 1"),
    ]
    parts = []
    ok = True
    for tag, path, error_line, exit_line in cases:
        code, _, elapsed, log, _ = run_game(exe, config, userdata, common + [f"--set=debug.input_script={path}"], 60,
                                            False, out_dir, f"broken_{tag}")
        good = code == 1 and error_line in log and exit_line in log
        ok &= good
        parts.append(f"{tag}: exit {code if code >= 0 else 'hang'}"
                     + (f" (0x{code & 0xFFFFFFFF:08X})" if code > 255 or code < -1 else "")
                     + (", error line and orderly exit" if good else
                        f", error line {'yes' if error_line in log else 'no'}, exit line {'yes' if exit_line in log else 'no'}")
                     + f", {elapsed:.1f} s")
    return ok, "; ".join(parts)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--config", required=True, type=Path)
    ap.add_argument("--script", type=Path, help="input script (default: the INI's [debug] input_script or "
                    "tools/input_scripts/m4_newgame.txt)")
    ap.add_argument("--rumble-script", type=Path, default=DEFAULT_RUMBLE_SCRIPT,
                    help="script of the M4-7 rumble run (default tools/input_scripts/m4_rumble.txt)")
    ap.add_argument("--no-rumble", action="store_true", help="skip the M4-7 rumble run")
    ap.add_argument("--no-broken", action="store_true", help="skip the M4-9 runs with broken scripts")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--fresh", action="store_true", help="empty hdd/cache/logs before every run (always before run 1)")
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT, help="seconds per run (watchdog)")
    ap.add_argument("--level-frames", type=int, default=DEFAULT_LEVEL_FRAMES)
    ap.add_argument("--max-press-ticks", type=int, default=DEFAULT_MAX_PRESS_TICKS)
    args = ap.parse_args()

    config = args.config.resolve()
    cp = m2_check.read_config(config)
    ud = m2_check.ini_value(cp, "paths", "userdata")
    if not ud:
        sys.exit("m4_check: [paths] userdata must be set in the test config (never %LOCALAPPDATA% in tests)")
    userdata = Path(ud) if Path(ud).is_absolute() else config.parent / ud
    if m2_check.ini_value(cp, "debug", "stop_at"):
        sys.exit("m4_check: the test config must not set [debug] stop_at")
    ini_script = m2_check.ini_value(cp, "debug", "input_script")
    script = args.script or (Path(ini_script) if ini_script else DEFAULT_SCRIPT)
    if not script.is_absolute():
        script = (config.parent / script) if (ini_script and not args.script) else script.resolve()
    if not script.exists():
        sys.exit(f"m4_check: input script {script} not found")
    for path in (script, args.rumble_script):
        if " " in str(path.resolve()):
            sys.exit(f"m4_check: {path} contains blanks - put the script in a path without blanks")
    script_text = script.read_text(encoding="utf-8", errors="replace")

    common = []
    if not int(m2_check.ini_value(cp, "debug", "frames", "0") or "0", 0):
        common.append(f"--frames={DEFAULT_FRAME_LIMIT}")
    common.append(f"--set=debug.timeout={args.timeout}")
    extra = list(common)
    if args.script or not ini_script:
        extra.append(f"--set=debug.input_script={script}")
    trace = [t.strip().lower() for t in m2_check.ini_value(cp, "debug", "trace").replace(";", ",").split(",")
             if t.strip()]
    if "vfs" not in trace and "all" not in trace:
        extra.append("--trace=" + ",".join(trace + ["vfs"]))

    out_dir = userdata / "m4_check"
    shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True)
    print(f"m4_check: {args.runs} run(s), script {script}, extra args {' '.join(extra)}, timeout {args.timeout} s")

    all_ok = True
    tick_sequences = []
    for i in range(1, args.runs + 1):
        code, stderr, elapsed, log, trace_text = run_game(args.exe, config, userdata, extra, args.timeout,
                                                          args.fresh or i == 1, out_dir, f"run_{i:02d}")
        results, info = check_run(log, trace_text, code, elapsed, args.timeout + 60, args.level_frames,
                                  args.max_press_ticks, script_text, out_dir, i)
        ok8, detail8 = check_kbm(log)
        results.append((CRITERIA[7], ok8, detail8))
        run_ok = all(ok for _, ok, _ in results)
        all_ok &= run_ok
        tick_sequences.append(info["script_ticks"])
        print(f"run {i:2d}: exit {code}, {elapsed:.0f} s, {'OK' if run_ok else 'FAILED'}")
        if not run_ok:
            own = [ln for ln in stderr.splitlines() if ln.strip() and not m2_check.LOG_LINE.match(ln)]
            if own:
                print("    stderr: " + own[-1][:200])
        for name, ok, detail in results:
            print(f"    [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        for d in info["screenshots"]:
            print(f"      screenshot {d}")
        for path in info.get("undocumented_misses", []):
            print(f"    [INFO] missing on the disc, not yet documented (vfs_known_missing.cpp): {path}")
        if info["level_files"]:
            shown = ", ".join(info["level_files"][:6])
            more = " ..." if len(info["level_files"]) > 6 else ""
            print(f"      level files ({len(info['level_files'])}): {shown}{more}")
        print(f"    [INFO] rendering gaps ({len(info['gaps'])}):")
        for g in info["gaps"]:
            print(f"      {g}")
        print(f"    [INFO] input layer ({len(info['input'])}):")
        for g in info["input"]:
            print(f"      {g[:220]}")
        warns = info["warnings"]
        distinct = []
        for w in warns:
            body = re.sub(r"^\[\s*[0-9.]+\]\s+\S+\s+", "", w)
            body = re.sub(r"0x[0-9A-F]{8}", "0x...", body)
            if body not in distinct:
                distinct.append(body)
        print(f"    [INFO] warnings: {len(warns)} lines, {len(distinct)} distinct")
        for w in distinct[:15]:
            print(f"      {w[:220]}")

    if len(tick_sequences) > 1:
        same = all(s == tick_sequences[0] for s in tick_sequences[1:])
        print(f"    [INFO] script lines at identical ticks in all runs (lockstep determinism): {'yes' if same else 'no'}")
        if not same:
            diffs = [abs(a[2] - b[2]) for seq in tick_sequences[1:] for a, b in zip(tick_sequences[0], seq)
                     if a[:2] == b[:2]]
            first_diff = next(((a, b) for a, b in zip(tick_sequences[0], tick_sequences[1]) if a != b), None)
            print(f"      largest tick difference of a script line: {max(diffs) if diffs else '-'} ticks"
                  + (f"; first: run 1 {first_diff[0]} vs run 2 {first_diff[1]}" if first_diff else "")
                  + " (loading and the intro screens follow I/O and QPC time, not ticks)")

    if not args.no_rumble:
        rumble_script = args.rumble_script.resolve()
        rextra = list(common) + [f"--set=debug.input_script={rumble_script}"]
        code, stderr, elapsed, log, _ = run_game(args.exe, config, userdata, rextra, args.timeout, True, out_dir,
                                                 "rumble")
        ok7, detail7 = check_rumble(log, code)
        all_ok &= ok7
        print(f"rumble run ({rumble_script.name}): exit {code}, {elapsed:.0f} s, {'OK' if ok7 else 'FAILED'}")
        print(f"    [{'PASS' if ok7 else 'FAIL'}] {CRITERIA[6]}: {detail7}")
    else:
        print(f"    [SKIP] {CRITERIA[6]}: --no-rumble")

    if not args.no_broken:
        ok9, detail9 = check_broken_scripts(args.exe, config, userdata, common, out_dir)
        all_ok &= ok9
        print(f"    [{'PASS' if ok9 else 'FAIL'}] {CRITERIA[8]}: {detail9}")
    else:
        print(f"    [SKIP] {CRITERIA[8]}: --no-broken")

    print(f"logs and screenshots: {out_dir}")
    print("M4 check: " + ("ALL PASS" if all_ok else "FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
