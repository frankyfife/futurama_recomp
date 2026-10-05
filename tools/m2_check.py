"""M2 exit-criteria check (docs/loader_design.md section 6, M2).

Runs futurama.exe N times past main with a test configuration (no stop_at; the script adds
--selftest and, unless the INI already sets them, --frames=3000 and --trace=vfs), keeps the log and
the VFS trace of every run and checks the seven M2 exit criteria plus the general milestone rule
("0 Fallen, 0 UNIMPLEMENTED, Log ohne error"; like M1 also no warn):

  1 SysPAL.img and FutPAL00..04.img mounted, FutPAL05.img not found, PAL mode (CApp+0xE8 = 0)
  2 the language from the INI ([settings] language) takes effect: XGetLanguage() and the game's
    language string (german -> 3 / "german")
  3 BUILD.TXT = "1.25" read (archive member of sysPAL.img)
  4 the intro context runs, the frame limit ends the run orderly (exit 0, no watchdog, no hang);
    additionally the intro context finishes (Intro -> next context) - no context waits forever
  5 fiber switches > 0, APC deliveries > 0 (XBE completions), no error completion / failed APC
    request, the disc error handler (DAT_0030E68C -> vf0 0xF2FD0) registered and never called
  6 self-test "SEH on fiber stack" (GuestSehOnFiberStack) passes
  7 vfs_trace.log lists every open; each one is a hit or a documented miss (no UNDOCUMENTED, no
    error), every asset lookup is an archive hit or followed by its loose-file open

The evidence lines come from the game observer probes (src/host/xapi/game_probes.cpp), the exit
reporters (core::AddExitReporter: fiber summary, I/O APC stats, game summary) and the VFS trace.

Usage (paths with spaces belong in the INI, not on the command line):
    python tools/m2_check.py --exe build/x86-debug/bin/futurama.exe --config run/m2/futurama.ini
                             [--runs 3] [--fresh] [--timeout SECONDS]

--fresh deletes <userdata>/hdd, <userdata>/cache and <userdata>/logs before every run (first boot each
time); without it only the first run starts from an empty user data directory. Logs of every run:
<userdata>/m2_check/run_NN.log and run_NN_vfs_trace.log. Exit code 0 when all criteria pass.
"""
import argparse
import configparser
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_FRAMES = 3000
# XC_LANGUAGE values (ExQueryNonVolatileSetting 7, design 2.1; kernel/kernel_settings: names or 1..9).
LANGUAGES = {"english": 1, "japanese": 2, "german": 3, "french": 4, "spanish": 5, "italian": 6,
             "korean": 7, "chinese": 8, "portuguese": 9}
# FUN_000D9990 (files.md 3.2): 3 german, 4 french, 5 spanish, 6 italian, everything else english.
GAME_LANGUAGE = {3: "german", 4: "french", 5: "spanish", 6: "italian"}
BUILD_TXT = "1.25"
ARCHIVES = ["sysPAL.img"] + [f"FutPAL{i:02d}.img" for i in range(5)]

CRITERIA = [
    "1 SysPAL.img + FutPAL00-04 mounted, FutPAL05 not found, PAL (CApp+0xE8 = 0)",
    "2 language from the config (XGetLanguage, game language string)",
    "3 BUILD.TXT = \"1.25\" read",
    "4 intro context runs, frame limit ends the run orderly (exit 0)",
    "5 fiber switches > 0, APC deliveries > 0, disc error handler never called",
    "6 self-test SEH on fiber stack (GuestSehOnFiberStack)",
    "7 vfs_trace.log: every open a hit or a documented miss",
    "log without error/warn/UNIMPLEMENTED/TRAP",
]


def read_config(config: Path) -> configparser.ConfigParser:
    # Same comment rule as core/config.cpp: ';' / '#' after whitespace starts an inline comment.
    cp = configparser.ConfigParser(inline_comment_prefixes=(";", "#"), interpolation=None)
    cp.read(config, encoding="utf-8-sig")
    return cp


def ini_value(cp: configparser.ConfigParser, section: str, key: str, default: str = "") -> str:
    value = cp.get(section, key, fallback=default).strip()
    if len(value) >= 2 and value[0] == value[-1] == '"':
        value = value[1:-1]
    return value


def expected_language(cp: configparser.ConfigParser) -> int:
    value = ini_value(cp, "settings", "language", "english").lower()
    if value.isdigit():
        return int(value)
    if value not in LANGUAGES:
        sys.exit(f"m2_check: unknown [settings] language {value!r}")
    return LANGUAGES[value]


def log_userdata(lines: list[str]) -> str | None:
    """userdata path from the log header line 'CORE INFO  userdata  <path>'."""
    for ln in lines[:40]:
        m = re.search(r" userdata  (.+)$", ln)
        if m:
            return m.group(1).strip()
    return None


TRACE_LINE = re.compile(r'^\[\s*[0-9.]+\]\s+\d+\s+(?P<api>.+?)\s+"(?P<path>[^"]*)"(?: -> (?P<target>.*?))? : '
                        r'(?P<result>.*)$')


def parse_trace(text: str) -> tuple[list[dict], list[str]]:
    """Data lines of vfs_trace.log as dicts; lines that do not parse are returned separately."""
    entries, bad = [], []
    for ln in text.splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        m = TRACE_LINE.match(ln)
        if not m:
            bad.append(ln)
            continue
        entries.append({"api": m.group("api").strip(), "path": m.group("path"),
                        "target": m.group("target") or "", "result": m.group("result").strip(), "line": ln})
    return entries, bad


def classify(result: str) -> str:
    if "UNDOCUMENTED" in result:
        return "undocumented"
    if result.startswith("missing") and "(documented:" in result:
        return "documented-miss"
    if result.startswith("hit archive"):
        return "archive-hit"
    if result.startswith("hit"):
        return "hit"
    if result.startswith("not archived"):
        return "not-archived"
    if result.startswith(("created", "exists")):
        return "symlink"
    if result.startswith("mounted"):
        return "mounted"
    if result.startswith("not mounted"):
        return "not-mounted"
    if result.startswith("error"):
        return "error"
    return "unknown"


def loose_path(name: str) -> str:
    # NiUDSStream ctor 0x13F250: "D:\" + name, '/' -> '\' (files.md 2.4).
    return "\\??\\D:\\" + name.replace("/", "\\")


def check_run(log: str, trace: str, exit_code: int, elapsed: float, timeout: int, frames: int,
              language: int, userdata: Path) -> tuple[list[tuple[str, bool, str]], dict]:
    lines = log.splitlines()
    info: dict = {}
    if not lines:
        return [(name, False, "log file missing or empty") for name in CRITERIA], info
    used = log_userdata(lines)
    if used is None or os.path.normcase(os.path.normpath(used)) != os.path.normcase(os.path.normpath(userdata)):
        why = f"exe used userdata {used!r}, expected {str(userdata)!r} (INI parsed differently?)"
        return [(name, False, why) for name in CRITERIA], info

    def first(rx: str, flags: int = 0) -> re.Match | None:
        for ln in lines:
            m = re.search(rx, ln, flags)
            if m:
                return m
        return None

    def all_matches(rx: str, flags: int = 0) -> list[re.Match]:
        return [m for m in (re.search(rx, ln, flags) for ln in lines) if m]

    entries, unparsed = parse_trace(trace)
    results = []

    # 1 archives + PAL
    mounted = {m.group(1).lower(): int(m.group(2)) for m in all_matches(r"archive: mounted (\S+) as #(\d+)")}
    order_ok = all(mounted.get(a.lower()) == i for i, a in enumerate(ARCHIVES))
    pal05_not_mounted = first(r"archive: FutPAL05\.img not mounted", re.I) is not None
    pal05_miss = any(e["api"] == "NtCreateFile" and e["path"].lower().endswith("\\futpal05.img")
                     and classify(e["result"]) == "documented-miss" for e in entries)
    pal = first(r"game: video standard CApp\+0xE8 = (\d+) \((\w+)")
    pal_ok = pal is not None and pal.group(1) == "0" and pal.group(2) == "PAL"
    extra = sorted(set(mounted) - {a.lower() for a in ARCHIVES})
    results.append((CRITERIA[0], order_ok and pal05_not_mounted and pal05_miss and pal_ok and not extra,
                    f"mounted {[a for a, _ in sorted(mounted.items(), key=lambda kv: kv[1])]}, "
                    f"order {'ok' if order_ok else 'WRONG'}, FutPAL05 not mounted {pal05_not_mounted} "
                    f"(trace: documented miss {pal05_miss}), CApp+0xE8 = {pal.group(1) if pal else '?'} "
                    f"({pal.group(2) if pal else '?'})" + (f", unexpected {extra}" if extra else "")))

    # 2 language
    xlang = sorted({int(m.group(1)) for m in all_matches(r"XGetLanguage\(\) = (\d+) \(")})
    game_lang = first(r'game: language "([^"]*)"')
    want_game = GAME_LANGUAGE.get(language, "english")
    lang_ok = xlang == [language] and game_lang is not None and game_lang.group(1) == want_game
    results.append((CRITERIA[1], lang_ok,
                    f"expected XGetLanguage {language} / \"{want_game}\": XGetLanguage {xlang or 'not logged'}, "
                    f"game language {game_lang.group(1) if game_lang else 'not logged'!r}"))

    # 3 BUILD.TXT
    build = first(r'game: BUILD\.TXT = "([^"]*)"')
    build_hit = next((e for e in entries if e["api"] == "archive lookup" and e["path"].upper() == "BUILD.TXT"
                      and classify(e["result"]) == "archive-hit"), None)
    results.append((CRITERIA[2], build is not None and build.group(1) == BUILD_TXT and build_hit is not None,
                    f"BUILD.TXT {build.group(1) if build else 'not logged'!r}, archive lookup "
                    f"{build_hit['target'] if build_hit else 'missing in vfs_trace.log'}"))

    # 4 intro context + orderly end
    contexts = first(r'game: contexts (.*); current "([^"]*)"')
    switches = [(m.group(1), m.group(2), int(m.group(3)))
                for m in all_matches(r"game: context (\S+) -> (\S+) \(CApp::Frame #(\d+)\)")]
    intro_first = contexts is not None and contexts.group(2) == "Intro"
    intro_done = any(a == "Intro" and b != "Intro" for a, b, _ in switches)
    limit = first(rf"FRAMES: limit of {frames} frames reached")
    pexit = first(r"process exit: frame limit reached \(exit code 0")
    summary = first(r"game summary: (\d+) CApp::Frame calls, contexts (.*?) \((\d+) switches\)")
    app_frames = int(summary.group(1)) if summary else 0
    no_hang = exit_code == 0 and elapsed < timeout
    ok4 = intro_first and intro_done and limit is not None and pexit is not None and no_hang and app_frames > 0
    history = summary.group(2) if summary else "?"
    info.update(frames=app_frames, contexts=history, elapsed=elapsed)
    intro_frames = next((n for a, _, n in switches if a == "Intro"), None)
    results.append((CRITERIA[3], ok4,
                    f"first context {contexts.group(2) if contexts else '?'!r}, contexts {history}"
                    f"{f' (Intro left after CApp::Frame #{intro_frames})' if intro_frames else ' (Intro never left)'}, "
                    f"{app_frames} CApp::Frame calls, frame limit {'reached' if limit else 'NOT reached'}, "
                    f"exit {exit_code} after {elapsed:.0f} s (timeout {timeout} s)"))

    # 5 fibers, APCs, disc error handler
    fib = first(r"fiber summary: (\d+) switches")
    first_switch = first(r"fiber: first SwitchToFiber") is not None
    apc = first(r"I/O APC stats \([^)]*\): queued (\d+), delivered (\d+) \(XBE completions (\d+)\).*"
                r"error completions (\d+), failed APC requests (\d+)")
    first_apc = first(r"I/O APC: first XBE completion delivered") is not None
    handler = first(r"game: disc error handler \[0x0030E68C\] = 0x002D707C .*= trap:Game::DiscErrorScreen")
    disc_calls = first(r"disc error screen calls (\d+)")
    disc_trap = first(r"TRAP DiscErrorScreen") is not None
    n_sw = int(fib.group(1)) if fib else 0
    n_del, n_xbe = (int(apc.group(2)), int(apc.group(3))) if apc else (0, 0)
    n_err, n_fail = (int(apc.group(4)), int(apc.group(5))) if apc else (-1, -1)
    ok5 = (n_sw > 0 and first_switch and n_del > 0 and n_xbe > 0 and first_apc and n_err == 0 and n_fail == 0
           and handler is not None and disc_calls is not None and disc_calls.group(1) == "0" and not disc_trap)
    info.update(switches=n_sw, apcs=n_del)
    results.append((CRITERIA[4], ok5,
                    f"fiber switches {n_sw}, APC deliveries {n_del} (XBE {n_xbe}), error completions {n_err}, "
                    f"failed APC requests {n_fail}, handler registered+trapped {handler is not None}, "
                    f"disc error screen calls {disc_calls.group(1) if disc_calls else '?'}"))

    # 6 self-test SEH on fiber stack
    seh = first(r"SELFTEST PASS GuestSehOnFiberStack")
    fails = [ln for ln in lines if "SELFTEST FAIL" in ln]
    passes = sum(1 for ln in lines if "SELFTEST PASS" in ln)
    results.append((CRITERIA[5], seh is not None and not fails,
                    f"GuestSehOnFiberStack {'PASS' if seh else 'missing'}; {passes} PASS, {len(fails)} FAIL"))

    # 7 VFS trace
    counts: dict[str, int] = {}
    for e in entries:
        counts[classify(e["result"])] = counts.get(classify(e["result"]), 0) + 1
    problems = [e["line"] for e in entries if classify(e["result"]) in ("undocumented", "error", "unknown")]
    problems += [f"unparsed: {ln}" for ln in unparsed]
    # every lookup that is not archived must be followed by the loose-file open of the same path
    for i, e in enumerate(entries):
        if e["api"] == "archive lookup" and classify(e["result"]) == "not-archived":
            want = loose_path(e["path"]).lower()
            if not any(f["api"] == "NtCreateFile" and f["path"].lower() == want for f in entries[i + 1:i + 8]):
                problems.append(f"no loose open after: {e['line']}")
    opens = sum(1 for e in entries if e["api"] in ("NtCreateFile", "NtOpenFile"))
    logged_opens = sum(1 for ln in lines if re.search(r" VFS  DEBUG (NtCreateFile|NtOpenFile) ", ln))
    if logged_opens and logged_opens != opens:
        problems.append(f"futurama.log has {logged_opens} opens, vfs_trace.log {opens}")
    ok7 = bool(entries) and not problems and counts.get("archive-hit", 0) > 0
    info.update(trace=counts)
    results.append((CRITERIA[6], ok7,
                    (f"{len(entries)} lines: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))
                     if entries else "vfs_trace.log missing or empty")
                    + (f"; first problem: {problems[0][:200]}" if problems else "")))

    # general rule
    bad = [ln for ln in lines if re.search(r" (ERROR|WARN) |UNIMPLEMENTED|TRAP ", ln)]
    results.append((CRITERIA[7], not bad, bad[0][:200] if bad else "clean"))
    return results, info


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--config", required=True, type=Path)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--timeout", type=int, help="seconds per run (default: frames/50 + 90)")
    args = ap.parse_args()

    config = args.config.resolve()
    cp = read_config(config)
    ud = ini_value(cp, "paths", "userdata")
    if not ud:
        sys.exit("m2_check: [paths] userdata must be set in the test config (never %LOCALAPPDATA% in tests)")
    userdata = Path(ud) if Path(ud).is_absolute() else config.parent / ud
    if ini_value(cp, "debug", "stop_at"):
        sys.exit("m2_check: the test config must not set [debug] stop_at (M2 runs past main)")
    language = expected_language(cp)

    extra_args = ["--selftest"]
    frames = int(ini_value(cp, "debug", "frames", "0") or "0", 0)
    if frames == 0:
        frames = DEFAULT_FRAMES
        extra_args.append(f"--frames={frames}")
    trace = [t.strip().lower() for t in ini_value(cp, "debug", "trace").replace(";", ",").split(",") if t.strip()]
    if "vfs" not in trace and "all" not in trace:
        extra_args.append("--trace=" + ",".join(trace + ["vfs"]))
    rate = int(ini_value(cp, "render", "frame_rate", "50") or "50", 0)
    if rate == 0:
        print("m2_check: warning: [render] frame_rate = 0 - time-based loading loops spin, the intro barely runs")
    timeout = args.timeout or (frames // max(rate, 1) + 90)
    ini_timeout = int(ini_value(cp, "debug", "timeout", "0") or "0", 0)
    if ini_timeout and ini_timeout < frames // max(rate, 1) + 10:
        print(f"m2_check: warning: [debug] timeout = {ini_timeout} s is shorter than {frames} frames at {rate} Hz")

    out_dir = userdata / "m2_check"
    shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True)
    print(f"m2_check: {args.runs} run(s), {frames} frames at {rate} Hz, language {language}, "
          f"extra args {' '.join(extra_args)}, timeout {timeout} s")

    all_ok = True
    infos = []
    trace_paths = []
    for i in range(1, args.runs + 1):
        if args.fresh or i == 1:
            for sub in ("hdd", "cache", "logs"):
                shutil.rmtree(userdata / sub, ignore_errors=True)
        log_path = userdata / "logs" / "futurama.log"
        trace_path = userdata / "logs" / "vfs_trace.log"
        log_path.unlink(missing_ok=True)  # never evaluate the files of an earlier run
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
        trace = trace_path.read_text(encoding="utf-8", errors="replace") if trace_path.exists() else ""
        (out_dir / f"run_{i:02d}.log").write_text(log, encoding="utf-8")
        (out_dir / f"run_{i:02d}_vfs_trace.log").write_text(trace, encoding="utf-8")
        results, info = check_run(log, trace, code, elapsed, timeout, frames, language, userdata)
        results.insert(4, ("4 exit code 0", code == 0, f"exit code {code}"))
        run_ok = all(ok for _, ok, _ in results)
        all_ok &= run_ok
        infos.append(info)
        trace_paths.append([e["api"] + " " + e["path"] for e in parse_trace(trace)[0]])
        print(f"run {i:2d}: exit {code}, {elapsed:.0f} s, {'OK' if run_ok else 'FAILED'}")
        if not run_ok and stderr:
            print("    stderr: " + stderr.splitlines()[-1][:200])
        for name, ok, detail in results:
            if i == 1 or not ok:
                print(f"    [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        if i > 1 and run_ok:
            print(f"    frames {info.get('frames')}, contexts {info.get('contexts')}, fiber switches "
                  f"{info.get('switches')}, APC deliveries {info.get('apcs')}")

    # Information only: are the opened paths the same in every run (single guest thread)?
    if len(trace_paths) > 1:
        same = all(p == trace_paths[0] for p in trace_paths[1:])
        print(f"    vfs_trace.log path sequences identical in all runs: {'yes' if same else 'no'}")
    print(f"logs: {out_dir}")
    print("M2 check: " + ("ALL PASS" if all_ok else "FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
