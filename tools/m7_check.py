"""M7 check: save games end to end (docs/loader_design.md M7, analysis/loader/m7_saves.md, docs/m7_report.md).

Five short scripted game runs ([debug] input_script, tools/input_scripts/m7_*.txt, [debug] movies = skip) on the
check's OWN userdata folder (default run/m7/check; never %LOCALAPPDATA%/FuturamaPC), each about 25-50 s:

  save      (m7_save.txt, fresh userdata)  options changed and saved; new game; level 1-1 ended with the game's own
            end-of-level screen (`game level_end Level1-2` = the level-script native FsChangeLevel) and saved there
  load      (m7_load.txt)  restart; options and extras load; options saved again unchanged; "Spiel laden" loads the
            save of run 1 (level 1-2); level 1-2 ended and saved in a new slot
  damaged   (m7_damaged.txt)  the check flips one payload byte of the oldest Futurama.dat; loading it reports the
            damage (signature mismatch, MEMCARD_052) and "Ja" deletes it through the menu (XDeleteSaveGame)
  slots     (m7_slots.txt)  the check fills U: with signed copies up to 25 save games; "Neues Spiel" shows the
            maximum dialog (MEMCARD_050); at the end of level 1-1 the save menu has no empty slot and the run overwrites
            entry 0 (MEMCARD_016 "Ja": XDeleteSaveGame + new save)
  nospace   (m7_nospace.txt, separate fresh userdata, [saves] free_space_limit = 65536)  "Neues Spiel" shows "not
            enough free blocks" (MEMCARD_047); option "Xbox Dashboard" -> XLaunchNewImageA -> the port reports it and
            returns to the game, which goes on into level 1-1

Criteria (exit code 0 when all pass):
  M7-1 every run ran its script to "quit 0" (exit code 0, no "input script: FAILED", no ERROR/FATAL line)
  M7-2 saving: Options.dat, Futurama.dat and Unlock.dat written by the game (probe lines "save: write ... -> ok"),
       the directories have the XAPI layout (SaveMeta.xbx with the name, directory = hash of the name,
       saveimage.xbx = 4096-byte XPR0 icon, TitleMeta.xbx/TitleImage.xbx), Futurama.dat holds "Level=Level1-2"
  M7-3 tools/verify_save.py accepts every container with the configured key (HMAC-SHA1 at 0xFEC) and rejects them
       with a wrong key
  M7-4 restart + load: Options.dat and Unlock.dat read with a valid signature at the first confirm (no MEMCARD_049/
       053), the unchanged options saved again equal run 1 byte for byte (bytes 0..0x20F of the payload; the last 4
       bytes are never written by the game, files.md 4.1), the loaded game data equal the saved ones, level 1-2
       loaded (scripts\\Level1-2.Ful in the VFS trace), the next save continues the play time (AgeSeconds) and the
       extras keep the best time of level 1-1 from run 1 (Unlock.dat entry 0)
  M7-5 a manipulated file is reported as damaged: verify_save exit code 1, the game logs the signature mismatch and
       shows MEMCARD_052
  M7-6 deleting through the menu removes the directory (damaged run: "Ja" -> XDeleteSaveGame -> directory gone;
       slots run: overwrite -> the old directory gone)
  M7-7 25 save games: MEMCARD_050 at "Neues Spiel", the overwrite keeps exactly 25 valid save games
  M7-8 not enough space: MEMCARD_047, the dashboard request returns to the game (log line, Ingame + level loaded,
       process exit code 0 instead of 4)
Information: the WriteFileEx completion latency of the save layer (design R16), the save summary lines.

Usage:
    python tools/m7_check.py --exe build/m7/bin/futurama.exe [--iso <image>] [--workdir run/m7/check]
                             [--runs save,load,damaged,slots,nospace] [--timeout 150] [--keep]
--runs selects runs (later runs need the userdata of the earlier ones; "save" starts from an empty folder unless
--keep is given). Logs, screenshots and INIs: <workdir>/<run>.log, <workdir>/shots_<run>/, <workdir>/<run>.ini.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_save as vs  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "tools" / "input_scripts"
DEFAULT_ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
RUN_ORDER = ["save", "load", "damaged", "slots", "nospace"]
SCRIPT_OF = {"save": "m7_save.txt", "load": "m7_load.txt", "damaged": "m7_damaged.txt", "slots": "m7_slots.txt",
             "nospace": "m7_nospace.txt"}
NOSPACE_LIMIT = 65536
MAX_SAVES = 25
VOLUME_DWORDS = 3  # Options.dat payload: the three volume floats come first (SFX, music, dialogue; m7_saves.md 3.3)
UNLOCK_TEST_ENTRY = 23  # Unlock.dat entry (level index) whose flag run "load" sets before it starts

CRITERIA = {
    "M7-1": "every run ran its script to quit 0",
    "M7-2": "saving writes Options.dat, Futurama.dat, Unlock.dat in the XAPI layout",
    "M7-3": "verify_save.py: HMAC-SHA1 signatures valid with the configured key, invalid with another key",
    "M7-4": "restart + load restores options, extras and the game state",
    "M7-5": "a manipulated file is reported as damaged",
    "M7-6": "deleting through the menu removes the directory",
    "M7-7": "up to 25 save games",
    "M7-8": "not enough space: message, the dashboard request returns to the game",
}

SAVE_LINE = re.compile(r"XAPI INFO  save: (.*)$")
SAVED_DATA = re.compile(r"save: saved game data: (.*)$")
LOADED_DATA = re.compile(r"save: loaded game data: (.*)$")
PROCESS_EXIT = re.compile(r"process exit: (.*) \(exit code (-?\d+)")
APC_LATENCY = re.compile(r"save write completion 0x[0-9A-F]+ delivered ([0-9.]+) ms")


def write_ini(path, iso, userdata, script, shots, timeout):
    path.write_text(
        "; generated by tools/m7_check.py\n"
        "[paths]\n"
        f"iso      = {iso}\n"
        f"userdata = {userdata}\n"
        "[debug]\n"
        "msgbox   = 0\n"
        f"timeout  = {timeout}\n"
        "movies   = skip\n"
        f"input_script = {script}\n"
        "[settings]\n"
        "language = german\n"
        "[render]\n"
        "backend    = d3d11\n"
        "resolution = 1280x960\n"
        "msaa       = off\n"
        f"screenshot_dir = {shots}\n"
        "[audio]\n"
        "master_volume = 0\n"
        "[log]\n"
        "level    = info\n", encoding="utf-8")


def run_game(exe, ini, userdata, extra, timeout):
    t0 = time.time()
    try:
        proc = subprocess.run([str(exe), f"--config={ini}", "--trace=vfs", *extra], cwd=str(userdata),
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=timeout + 60)
        code = proc.returncode
    except subprocess.TimeoutExpired:
        code = -999
    return code, time.time() - t0


def udata(userdata):
    return Path(userdata) / "hdd" / "partition1" / "UDATA" / vs.TITLE_ID


def game_saves(userdata):
    """{directory name: (save name, Futurama.dat path)} for the game saves (directories with Futurama.dat)."""
    out = {}
    root = udata(userdata)
    if not root.is_dir():
        return out
    for d in root.iterdir():
        f = d / "Futurama.dat"
        if d.is_dir() and f.is_file():
            out[d.name] = (vs.read_save_meta_name(d / "SaveMeta.xbx"), f)
    return out


def payload(path):
    data = Path(path).read_bytes()
    n = struct.unpack_from("<I", data, 0)[0]
    return data[4:4 + n]


def game_text(path):
    return payload(path).rstrip(b"\0").decode("latin-1")


def age_seconds(text):
    m = re.search(r"AgeSeconds=([0-9.]+)", text)
    h = re.search(r"AgeHours=(\d+)", text)
    return (float(m.group(1)) if m else -1.0) + (int(h.group(1)) * 3600 if h else 0)


def unlock_entries(path):
    p = payload(path)
    return [struct.unpack_from("<BI", p, 5 * i) for i in range(len(p) // 5)]


def fmt(values):
    return "[" + ", ".join("%.3f" % v for v in values) + "]"


def one_line(text):
    return text.replace(chr(13), "").replace(chr(10), " ").strip()


def unescape(s):
    return s.replace("\\n", "\n").replace("\\r", "\r")


def save_lines(log):
    return [m.group(1) for m in map(SAVE_LINE.search, log.splitlines()) if m]


def has(lines, *parts):
    return any(all(p in ln for p in parts) for ln in lines)


def index_of(lines, *parts):
    for i, ln in enumerate(lines):
        if all(p in ln for p in parts):
            return i
    return -1


def error_lines(log):
    bad = []
    for ln in log.splitlines():
        if (" ERROR " in ln or "FATAL" in ln or "TRAP" in ln) and "input script" not in ln:
            bad.append(ln.strip())
    return bad


class Report:
    def __init__(self):
        self.results = {}
        self.info = []

    def add(self, crit, ok, detail):
        prev = self.results.get(crit)
        if prev is None:
            self.results[crit] = [ok, [detail]]
        else:
            prev[0] = prev[0] and ok
            prev[1].append(detail)

    def note(self, text):
        self.info.append(text)


def check_script_end(rep, name, log, code, elapsed):
    failed = next((ln for ln in log.splitlines() if "input script: FAILED" in ln), None)
    exits = [m for m in map(PROCESS_EXIT.search, log.splitlines()) if m]
    errors = error_lines(log)
    ok = code == 0 and failed is None and exits and "input script quit" in exits[-1].group(1) and not errors
    detail = f"{name}: exit code {code}, {elapsed:.0f} s"
    if failed:
        detail += "; " + failed.split("input script: ", 1)[1][:160]
    if errors:
        detail += f"; {len(errors)} error line(s), first: {errors[0][:160]}"
    rep.add("M7-1", ok, detail)
    return ok


def check_layout(rep, userdata):
    root = udata(userdata)
    problems = []
    for f in ("TitleMeta.xbx", "TitleImage.xbx"):
        if not (root / f).is_file():
            problems.append(f"{f} missing")
    dirs = [d for d in root.iterdir() if d.is_dir()] if root.is_dir() else []
    for d in dirs:
        name = vs.read_save_meta_name(d / "SaveMeta.xbx")
        if name is None:
            problems.append(f"{d.name}: SaveMeta.xbx missing/unreadable")
            continue
        if vs.save_dir_name(name) != d.name.upper():
            problems.append(f'{d.name}: not the hash of "{name}" ({vs.save_dir_name(name)})')
        icon = d / "saveimage.xbx"
        if not icon.is_file() or icon.stat().st_size != 4096 or icon.read_bytes()[:4] != b"XPR0":
            problems.append(f"{d.name}: saveimage.xbx missing or not a 4096-byte XPR0 image")
    return dirs, problems


def check_verify(rep, userdata, key, label):
    files = vs.collect([udata(userdata)])
    results = [vs.check_container(f.read_bytes(), key)[0] for f in files]
    wrong = bytes(16)
    wrong_results = [vs.check_container(f.read_bytes(), wrong)[0] for f in files]
    ok = bool(files) and all(results) and not any(wrong_results)
    rep.add("M7-3", ok, f"{label}: {len(files)} containers, {sum(results)} valid with the configured key, "
                        f"{sum(wrong_results)} valid with a zero key")
    return ok


def main():
    ap = argparse.ArgumentParser(description="M7 save-game check (see the module docstring)")
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--iso", default=DEFAULT_ISO)
    ap.add_argument("--workdir", type=Path, default=ROOT / "run" / "m7" / "check")
    ap.add_argument("--runs", default=",".join(RUN_ORDER))
    ap.add_argument("--timeout", type=int, default=150, help="watchdog per run in seconds ([debug] timeout)")
    ap.add_argument("--keep", action="store_true", help="do not empty the userdata folders before the save run")
    args = ap.parse_args()

    exe = args.exe.resolve()
    work = args.workdir.resolve()
    local = os.environ.get("LOCALAPPDATA")
    if local and str(work).lower().startswith(str(Path(local) / "FuturamaPC").lower()):
        print("m7_check: refusing to use the player's save folder %LOCALAPPDATA%\\FuturamaPC", file=sys.stderr)
        return 2
    runs = [r.strip() for r in args.runs.split(",") if r.strip()]
    for r in runs:
        if r not in RUN_ORDER:
            print(f"m7_check: unknown run {r}", file=sys.stderr)
            return 2
    main_ud = work / "main"
    nospace_ud = work / "nospace"
    work.mkdir(parents=True, exist_ok=True)
    key = vs.fixed_key()  # the generated INIs use the default [saves] key_mode = fixed
    rep = Report()
    state_file = work / "state.json"  # facts of earlier runs (run save -> run load across invocations)
    state = json.loads(state_file.read_text(encoding="utf-8")) if state_file.exists() else {}

    def save_state():
        state_file.write_text(json.dumps(state, indent=1), encoding="utf-8")

    def do_run(name, userdata, extra=()):
        shots = work / f"shots_{name}"
        shutil.rmtree(shots, ignore_errors=True)
        shots.mkdir(parents=True, exist_ok=True)
        ini = work / f"{name}.ini"
        write_ini(ini, args.iso, userdata, SCRIPTS / SCRIPT_OF[name], shots, args.timeout)
        print(f"--- run {name} ({SCRIPT_OF[name]}) ...", flush=True)
        code, elapsed = run_game(exe, ini, userdata, list(extra), args.timeout)
        logs = Path(userdata) / "logs"
        log = (logs / "futurama.log").read_text(encoding="utf-8", errors="replace")
        trace_file = logs / "vfs_trace.log"
        trace = trace_file.read_text(encoding="utf-8", errors="replace") if trace_file.exists() else ""
        shutil.copyfile(logs / "futurama.log", work / f"{name}.log")
        if trace:
            shutil.copyfile(trace_file, work / f"{name}_vfs_trace.log")
        print(f"    exit code {code}, {elapsed:.0f} s", flush=True)
        check_script_end(rep, name, log, code, elapsed)
        lat = [float(m.group(1)) for m in map(APC_LATENCY.search, log.splitlines()) if m]
        if lat:
            rep.note(f"{name}: WriteFileEx completion of the save layer delivered after {min(lat):.1f}..{max(lat):.1f} ms "
                     f"({len(lat)} writes)")
        summ = next((ln.split("save summary: ", 1)[1] for ln in log.splitlines() if "save summary: " in ln), None)
        if summ:
            rep.note(f"{name}: {summ}")
        return code, log, trace

    # ---------------------------------------------------------------- run 1: save
    if "save" in runs:
        if not args.keep:
            shutil.rmtree(main_ud, ignore_errors=True)
        main_ud.mkdir(parents=True, exist_ok=True)
        code, log, trace = do_run("save", main_ud)
        sl = save_lines(log)
        wrote = {f: has(sl, f"write {f} for", "-> ok") for f in ("Options.dat", "Futurama.dat", "Unlock.dat")}
        saved = [unescape(m.group(1)) for m in map(SAVED_DATA.search, log.splitlines()) if m]
        saves = game_saves(main_ud)
        dirs, problems = check_layout(rep, main_ud)
        opts = udata(main_ud) / vs.save_dir_name("Futurama Options") / "Options.dat"
        unlock = udata(main_ud) / vs.save_dir_name("Futurama Extras") / "Unlock.dat"
        text = game_text(next(iter(saves.values()))[1]) if len(saves) == 1 else ""
        ok = (all(wrote.values()) and len(saves) == 1 and opts.is_file() and unlock.is_file() and not problems
              and text.startswith("Level=Level1-2") and saved and saved[-1] == text and
              has(sl, "dialog MEMCARD_007"))
        rep.add("M7-2", ok, f"save: written {wrote}; {len(saves)} game save(s) {[n for n, _ in saves.values()]}; "
                            f"Futurama.dat '{one_line(text)[:70]}...'; layout problems: {problems or 'none'}")
        check_verify(rep, main_ud, key, "after run save")
        state.clear()
        if opts.is_file():
            state["volumes1"] = list(struct.unpack_from("<%df" % VOLUME_DWORDS, payload(opts), 0))
        if unlock.is_file():
            state["unlock1"] = [list(e) for e in unlock_entries(unlock)]
        state["text1"] = text
        save_state()

    # ---------------------------------------------------------------- run 2: load
    if "load" in runs:
        before = dict(game_saves(main_ud))
        # Extras: set the unlock flag of the last level (entry 23, never reached here) in Unlock.dat and sign the
        # file with verify_save - the game must accept the externally signed file (signature ok, no MEMCARD_053) and
        # keep the flag (the merges at load 0xDC6E0 and at save 0xDD200 OR the flags).
        unlock = udata(main_ud) / vs.save_dir_name("Futurama Extras") / "Unlock.dat"
        if unlock.is_file():
            data = bytearray(unlock.read_bytes())
            data[4 + 5 * UNLOCK_TEST_ENTRY] = 1
            unlock.write_bytes(vs.sign(bytes(data), key))
        code, log, trace = do_run("load", main_ud)
        sl = save_lines(log)
        opt_read = index_of(sl, "Options.dat -> ok")
        opt_sig = index_of(sl, "signature of", "Options.dat ok")
        unl_read = index_of(sl, "Unlock.dat -> ok")
        unl_sig = index_of(sl, "signature of", "Unlock.dat ok")
        opt_write = index_of(sl, "write Options.dat")
        no_err_dialog = not any(has(sl, f"dialog {d}") for d in ("MEMCARD_049", "MEMCARD_053", "MEMCARD_052"))
        opts = udata(main_ud) / vs.save_dir_name("Futurama Options") / "Options.dat"
        # Run 2 lowers the SFX volume once more. Loaded options: exactly one volume differs from run 1 and is lower;
        # with the defaults (load failed) the same slider move would give the run-1 values again.
        v1 = state.get("volumes1", [])
        v2 = list(struct.unpack_from("<%df" % VOLUME_DWORDS, payload(opts), 0)) if opts.is_file() else []
        changed = [i for i in range(min(len(v1), len(v2))) if abs(v1[i] - v2[i]) > 1e-6]
        options_same = bool(v1) and len(changed) == 1 and v2[changed[0]] < v1[changed[0]]
        loaded = [unescape(m.group(1)) for m in map(LOADED_DATA.search, log.splitlines()) if m]
        text1 = state.get("text1", "")
        loaded_ok = bool(loaded) and bool(text1) and loaded[0].rstrip("\0") == text1
        level_file = re.search(r"scripts\\Level1-2\.Ful", trace, re.IGNORECASE) is not None
        level_loaded = "game: level loaded" in log
        after = game_saves(main_ud)
        new = [v for k, v in after.items() if k not in before]
        text2 = game_text(new[0][1]) if len(new) == 1 else ""
        age_ok = bool(text2) and text2.startswith("Level=Level1-3") and age_seconds(text2) > age_seconds(text1) > 0
        u2 = unlock_entries(unlock) if unlock.is_file() else []
        u1 = [tuple(e) for e in state.get("unlock1", [])]
        # flag kept, level 1-2 got its best time. The best time of level 1-1 from run 1 is lost by the game itself:
        # the merge FUN_000F3B90 takes min(file, memory) with a signed compare (0xF3C5E jge), so the -1 ("no time")
        # of the fresh in-memory table wins (m7_saves.md 3.4) - reported as information.
        extras_ok = (len(u2) == 24 and u2[UNLOCK_TEST_ENTRY][0] == 1 and u2[1][1] != 0xFFFFFFFF)
        if u1 and u2:
            rep.note(f"load: Unlock.dat best time of level 1-1: run 1 {u1[0][1]} s -> after run 2 "
                     f"{'none (-1)' if u2[0][1] == 0xFFFFFFFF else u2[0][1]} (original game: signed min with -1)")
        ok = (opt_read >= 0 and opt_sig >= 0 and unl_read >= 0 and unl_sig >= 0 and opt_write > opt_sig and
              no_err_dialog and options_same and loaded_ok and level_file and level_loaded and age_ok and extras_ok)
        rep.add("M7-4", ok,
                f"load: Options.dat read/signature ok {opt_read >= 0 and opt_sig >= 0} (before the re-save "
                f"{opt_write > opt_sig}), Unlock.dat read/signature ok {unl_read >= 0 and unl_sig >= 0}, no "
                f"MEMCARD_049/052/053 {no_err_dialog}; volumes run 1 {fmt(v1)} -> run 2 {fmt(v2)} (one lowered "
                f"further: {options_same}); loaded game data = "
                f"run 1 {loaded_ok}; scripts\\Level1-2.Ful opened {level_file}, level loaded {level_loaded}; new "
                f"save '{one_line(text2)[:40]}...' AgeSeconds {age_seconds(text2):.2f} > {age_seconds(text1):.2f}: {age_ok}; "
                f"extras: unlock flag {UNLOCK_TEST_ENTRY} set before the run kept "
                f"{u2[UNLOCK_TEST_ENTRY] if len(u2) == 24 else '-'}, level 1-2 best time {u2[1] if len(u2) > 1 else '-'}"
                f": {extras_ok}")
        check_verify(rep, main_ud, key, "after run load")

    # ---------------------------------------------------------------- run 3: damaged
    if "damaged" in runs:
        saves = game_saves(main_ud)
        if not saves:
            rep.add("M7-5", False, "damaged: no game save to manipulate (run save/load first)")
        else:
            # the oldest save = entry 0 of the load menu (creation order)
            oldest_dir, (oldest_name, oldest_file) = min(saves.items(), key=lambda kv: kv[1][1].parent.stat().st_ctime)
            data = bytearray(oldest_file.read_bytes())
            data[8] ^= 0x01  # one bit in the payload text
            oldest_file.write_bytes(bytes(data))
            vrc = vs.main(["--quiet", str(oldest_file)])
            code, log, trace = do_run("damaged", main_ud)
            sl = save_lines(log)
            mismatch = has(sl, "signature of", oldest_dir, "MISMATCH")
            dialog = has(sl, "dialog MEMCARD_052")
            deleted = has(sl, f'XDeleteSaveGame(U:\\, "{oldest_name}") -> 0')
            gone = not (udata(main_ud) / oldest_dir).exists()
            rep.add("M7-5", vrc == 1 and mismatch and dialog,
                    f"damaged: verify_save exit code {vrc} (1 = damaged) for {oldest_dir}\\Futurama.dat; game: "
                    f"signature MISMATCH logged {mismatch}, MEMCARD_052 shown {dialog}")
            rep.add("M7-6", deleted and gone, f'damaged: XDeleteSaveGame("{oldest_name}") -> 0 {deleted}, '
                                              f"directory {oldest_dir} removed {gone}")
            check_verify(rep, main_ud, key, "after run damaged")

    # ---------------------------------------------------------------- run 4: slots
    if "slots" in runs:
        saves = game_saves(main_ud)
        if not saves:
            rep.add("M7-7", False, "slots: no game save to copy (run save/load first)")
        else:
            src_dir = next(iter(saves.values()))[1].parent
            container = (src_dir / "Futurama.dat").read_bytes()
            icon = (src_dir / "saveimage.xbx").read_bytes()
            i = 0
            while len(game_saves(main_ud)) < MAX_SAVES:
                i += 1
                name = "M7 Test %02d" % i
                d = udata(main_ud) / vs.save_dir_name(name)
                if d.exists():
                    continue
                d.mkdir()
                (d / "SaveMeta.xbx").write_bytes(vs.save_meta(name))
                (d / "Futurama.dat").write_bytes(container)  # the signature does not cover the name
                (d / "saveimage.xbx").write_bytes(icon)
                time.sleep(0.02)  # distinct creation times (the menus list the saves oldest first)
            before = game_saves(main_ud)
            oldest_dir = min(before, key=lambda k: before[k][1].parent.stat().st_ctime)
            oldest_name = before[oldest_dir][0]
            code, log, trace = do_run("slots", main_ud)
            sl = save_lines(log)
            after = game_saves(main_ud)
            max_dialog = has(sl, "dialog MEMCARD_050")
            deleted = has(sl, f'XDeleteSaveGame(U:\\, "{oldest_name}") -> 0')
            written = has(sl, "write Futurama.dat for", "-> ok")
            gone = oldest_dir not in after
            files = vs.collect([udata(main_ud)])
            valid = all(vs.check_container(f.read_bytes(), key)[0] for f in files)
            rep.add("M7-7", max_dialog and written and len(before) == MAX_SAVES and len(after) == MAX_SAVES and valid,
                    f"slots: {len(before)} save games before, MEMCARD_050 at 'Neues Spiel' {max_dialog}, new save "
                    f"written {written}, {len(after)} save games after, all {len(files)} containers valid {valid}")
            rep.add("M7-6", deleted and gone, f'slots: overwrite of entry 0 deleted "{oldest_name}" {deleted}, '
                                              f"directory {oldest_dir} removed {gone}")

    # ---------------------------------------------------------------- run 5: nospace
    if "nospace" in runs:
        shutil.rmtree(nospace_ud, ignore_errors=True)
        nospace_ud.mkdir(parents=True, exist_ok=True)
        code, log, trace = do_run("nospace", nospace_ud, [f"--set=saves.free_space_limit={NOSPACE_LIMIT}"])
        sl = save_lines(log)
        dialog = has(sl, "dialog MEMCARD_047")
        dashboard = "asks for the Xbox dashboard" in log and "back to the game" in log
        ingame = "game: context Frontend -> Ingame" in log and "game: level loaded" in log
        rep.add("M7-8", dialog and dashboard and ingame and code == 0,
                f"nospace: MEMCARD_047 {dialog}, dashboard request answered and back in the game {dashboard}, "
                f"Ingame + level loaded {ingame}, exit code {code} (0; 4 = process ended by XLaunchNewImageA)")

    # ---------------------------------------------------------------- report
    print()
    all_ok = True
    for crit, text in CRITERIA.items():
        if crit not in rep.results:
            print(f"[SKIP] {crit} {text}")
            continue
        ok, details = rep.results[crit]
        all_ok = all_ok and ok
        print(f"[{'PASS' if ok else 'FAIL'}] {crit} {text}")
        for d in details:
            print(f"         {d}")
    if rep.info:
        print("\ninformation:")
        for line in rep.info:
            print("  " + line)
    print(f"\nlogs and screenshots: {work}")
    return 0 if all_ok and rep.results else 1


if __name__ == "__main__":
    sys.exit(main())
