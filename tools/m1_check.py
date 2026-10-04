"""M1 exit-criteria check (docs/loader_design.md section 6, M1).

Runs futurama.exe N times with a test configuration (--stop-at=main --selftest are expected in the
INI), keeps every log, checks the six M1 exit criteria and compares the logs after masking
timestamps and addresses (criterion 6).

Usage (paths with spaces belong in the INI, not on the command line):
    python tools/m1_check.py --exe build/x86-debug/bin/futurama.exe --config run/integ/futurama.ini
                             [--runs 10] [--fresh] [--xbe orig/retail_eu/default.xbe]

--fresh deletes <userdata>/hdd, <userdata>/cache and <userdata>/logs before every run (first boot
each time); without it only the first run starts from an empty user data directory.
Exit code 0 when all criteria pass.
"""
import argparse
import configparser
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

TITLE_ID = "46490002"
XTIMAGE_VA, XTIMAGE_SIZE = 0x3178C0, 0x2800


def read_userdata(config: Path) -> Path:
    cp = configparser.ConfigParser(inline_comment_prefixes=(";", "#"), interpolation=None)
    cp.read(config, encoding="utf-8-sig")
    value = cp.get("paths", "userdata", fallback="").strip()
    if not value:
        sys.exit("m1_check: [paths] userdata must be set in the test config (never %LOCALAPPDATA% in tests)")
    p = Path(value)
    return p if p.is_absolute() else (config.parent / p)


def xbe_section_bytes(xbe: Path, va: int, size: int) -> bytes:
    b = xbe.read_bytes()
    base = struct.unpack_from("<I", b, 0x104)[0]
    count, headers = struct.unpack_from("<II", b, 0x11C)
    for i in range(count):
        _, sva, vsize, raw, rsize, _ = struct.unpack_from("<6I", b, headers - base + i * 0x38)
        if sva <= va < sva + rsize:
            off = raw + va - sva
            return b[off:off + size]
    raise ValueError(f"VA 0x{va:X} not in a section")


TIMESTAMP = re.compile(r"^\[\s*[0-9.]+\]\s*")
# Addresses are printed as 0x%08X or %p (8 hex digits); host thread ids in decimal. Shorter hex
# values (sizes, status codes, 0x4000) stay part of the comparison.
MASKS = [
    (re.compile(r"0x[0-9A-Fa-f]{8}\b"), "0x########"),
    (re.compile(r"\b[0-9A-Fa-f]{8}\b"), "########"),
    (re.compile(r"(host thread|thread id) \d+"), r"\1 #"),
]


def strip_timestamp(line: str) -> str:
    return TIMESTAMP.sub("", line).rstrip()


def normalize(line: str) -> str:
    line = strip_timestamp(line)
    for rx, rep in MASKS:
        line = rx.sub(rep, line)
    return line


def check_run(log: str, partition1: Path, xtimage: bytes | None) -> list[tuple[str, bool, str]]:
    lines = log.splitlines()

    def has(rx: str) -> bool:
        return any(re.search(rx, ln) for ln in lines)

    def first(rx: str) -> str:
        return next((ln for ln in lines if re.search(rx, ln)), "")

    results = []
    # 1 self-tests
    passes = [ln for ln in lines if "SELFTEST PASS" in ln]
    fails = [ln for ln in lines if "SELFTEST FAIL" in ln]
    seh = has(r"SELFTEST PASS GuestSehIsBadReadPtr") and has(r"SELFTEST PASS GuestIsBadReadPtrNull")
    results.append(("1 self-tests (IsBadReadPtr via guest SEH)", not fails and seh and len(passes) >= 18,
                    f"{len(passes)} PASS, {len(fails)} FAIL, SEH test {'ok' if seh else 'missing'}"))
    # 2 XapiInitProcess without XapiBootToDash / HalReturnToFirmware
    xip = has(r"boot: XapiInitProcess .*start") and has(r"boot: XapiInitProcess done")
    dash = has(r"XapiBootToDash\(") or has(r"HalReturnToFirmware\(") or has(r"XLaunchNewImageA\(")
    results.append(("2 XapiInitProcess, no XapiBootToDash/HalReturnToFirmware", xip and not dash,
                    f"XapiInitProcess start+done {'yes' if xip else 'no'}, dashboard/firmware {'yes' if dash else 'no'}"))
    # 3 partition1 contents
    tdata = partition1 / "TDATA" / TITLE_ID
    udata = partition1 / "UDATA" / TITLE_ID
    meta, image = udata / "TitleMeta.xbx", udata / "TitleImage.xbx"
    meta_ok = meta.is_file() and meta.read_bytes()[:2] == b"\xff\xfe" and \
        meta.read_bytes().decode("utf-16").startswith("TitleName=")
    image_ok = image.is_file() and image.stat().st_size == XTIMAGE_SIZE and \
        (xtimage is None or image.read_bytes() == xtimage)
    results.append(("3 TDATA/UDATA title dirs, TitleMeta.xbx (UTF-16), TitleImage.xbx == $$XTIMAGE",
                    tdata.is_dir() and meta_ok and image_ok,
                    f"TDATA dir {tdata.is_dir()}, TitleMeta {meta_ok}, TitleImage {image_ok}"
                    + ("" if xtimage is not None else " (size only, no --xbe)")))
    # 4 _cinit: pool + cluster size
    pool = first(r"game memory pool .*0x2600000 bytes \(38 MiB\)")
    cluster = has(r'XGetDiskClusterSizeA\("U:\\"\) = 0x4000')
    cinit = has(r"boot: _cinit done")
    results.append(("4 _cinit: 38 MiB pool, XGetDiskClusterSizeA(\"U:\\\") = 0x4000", bool(pool) and cluster and cinit,
                    f"pool {'yes' if pool else 'no'}, cluster 0x4000 {'yes' if cluster else 'no'}, _cinit done {'yes' if cinit else 'no'}"))
    # 5 main hook + exit 0 (exit code checked by the caller)
    results.append(("5 main hook reached", has(r"STOP-AT 0x000D29B0 .* reached"), strip_timestamp(first(r"STOP-AT"))))
    # general M1 rule: no error lines, no UNIMPLEMENTED, no TRAP
    bad = [ln for ln in lines if re.search(r" (ERROR|WARN) |UNIMPLEMENTED|TRAP ", ln)]
    results.append(("log without error/warn/UNIMPLEMENTED/TRAP", not bad, bad[0][:160] if bad else "clean"))
    return results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--config", required=True, type=Path)
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--xbe", type=Path, help="original default.xbe for the TitleImage comparison")
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    config = args.config.resolve()
    userdata = read_userdata(config)
    out_dir = userdata / "m1_check"
    shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True)
    xtimage = xbe_section_bytes(args.xbe, XTIMAGE_VA, XTIMAGE_SIZE) if args.xbe else None

    normalized_runs = []
    raw_runs = []
    all_ok = True
    for i in range(1, args.runs + 1):
        if args.fresh or i == 1:
            for sub in ("hdd", "cache", "logs"):
                shutil.rmtree(userdata / sub, ignore_errors=True)
        proc = subprocess.run([str(args.exe.resolve()), f"--config={config}"], cwd=userdata,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=args.timeout)
        log_path = userdata / "logs" / "futurama.log"
        log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        (out_dir / f"run_{i:02d}.log").write_text(log, encoding="utf-8")
        results = check_run(log, userdata / "hdd" / "partition1", xtimage)
        results.insert(4, ("5 exit code 0", proc.returncode == 0, f"exit code {proc.returncode}"))
        run_ok = all(ok for _, ok, _ in results)
        all_ok &= run_ok
        print(f"run {i:2d}: exit {proc.returncode}, {'OK' if run_ok else 'FAILED'}")
        if i == 1 or not run_ok:
            for name, ok, detail in results:
                print(f"    [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        normalized_runs.append([normalize(ln) for ln in log.splitlines()])
        raw_runs.append([strip_timestamp(ln) for ln in log.splitlines()])

    # 6 identical logs up to timestamps and addresses
    ref = normalized_runs[0]
    diffs = []
    for i, run in enumerate(normalized_runs[1:], start=2):
        if run != ref:
            n = next((k for k, (a, b) in enumerate(zip(ref, run)) if a != b), min(len(ref), len(run)))
            diffs.append(f"run {i} differs from run 1 at line {n + 1} ({len(run)} vs {len(ref)} lines): "
                         f"{run[n] if n < len(run) else '<eof>'!r} vs {ref[n] if n < len(ref) else '<eof>'!r}")
    (out_dir / "normalized_run_01.log").write_text("\n".join(ref) + "\n", encoding="utf-8")
    ok6 = not diffs
    all_ok &= ok6
    print(f"[{'PASS' if ok6 else 'FAIL'}] 6 {args.runs} runs give identical logs after masking timestamps/addresses"
          f" ({len(ref)} lines each)")
    for d in diffs:
        print("    " + d)
    # Information only: lines that differ before masking addresses (run 1 vs. all others).
    volatile = sorted({k for run in raw_runs[1:] for k, (a, b) in enumerate(zip(raw_runs[0], run)) if a != b})
    print(f"    lines that differ only in addresses/thread ids: {len(volatile)} of {len(ref)}")
    for k in volatile[:12]:
        print(f"      {raw_runs[0][k][:150]}")
    print(f"logs: {out_dir}")
    print("M1 check: " + ("ALL PASS" if all_ok else "FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
