"""Generate the XBE function table used for backtraces and hook validation (design 1.11, 3.2).

Input:  analysis/retail_eu_export/functions.tsv (Ghidra snapshot)
        tools/gen/extra_function_starts.tsv (hand-maintained starts outside Ghidra functions, with evidence)
Output: src/host/gen/xbe_functions.gen.inc - initializer rows for guest::FunctionInfo (core/guest.h):
        { address, size, flags, "name" },   sorted by address
        flags: 0 = Ghidra function, 1 = extra start (extra_function_starts.tsv),
               2 = synthetic range (NOT a function start; hooks are rejected there)

The ~24,300 C++ initializer stubs in 0x17E010..0x233880 (startup.md 2.4) are collapsed into one
synthetic range entry to keep the table small.

Usage:  python tools/gen/gen_functions.py [--check]
"""
import csv
import sys

from gen_common import ANALYSIS, ROOT, SRC, c_string, emit, header, parse_args

FUNCTIONS = ANALYSIS / "retail_eu_export" / "functions.tsv"
EXTRA = ROOT / "tools" / "gen" / "extra_function_starts.tsv"
OUT = SRC / "host" / "gen" / "xbe_functions.gen.inc"

INIT_LO, INIT_HI = 0x17E010, 0x233880


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def main():
    args = parse_args(__doc__)
    entries = {}
    skipped = 0
    with open(FUNCTIONS, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            a = int(r["address"], 16)
            if INIT_LO <= a < INIT_HI:
                skipped += 1
                continue
            if a in entries:
                die(f"duplicate function start {a:#x}")
            entries[a] = (int(r["size"]), 0, r["name"])
    if INIT_LO in entries:
        die("init range start collides with a function")
    entries[INIT_LO] = (INIT_HI - INIT_LO, 2, "<cinit_initializer_stubs>")
    with open(EXTRA, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            a = int(r["address"], 16)
            if a in entries:
                die(f"extra start {a:#x} already in functions.tsv ({entries[a][2]})")
            entries[a] = (int(r["size"], 0), 1, r["name"])
    rows = sorted(entries.items())
    # sanity: no entry may start inside the previous one (except Ghidra overlaps, which we report)
    overlaps = 0
    for (a0, (s0, _, n0)), (a1, (_, fl1, n1)) in zip(rows, rows[1:]):
        if a0 + s0 > a1:
            if fl1 == 1:
                die(f"extra start {a1:#x} {n1} lies inside {n0} ({a0:#x}+{s0:#x})")
            overlaps += 1
    L = [header("gen_functions.py", ["analysis/retail_eu_export/functions.tsv",
                                     "tools/gen/extra_function_starts.tsv"])]
    L.append("//")
    L.append("// Initializer rows for guest::FunctionInfo (core/guest.h), sorted by address.")
    L.append(f"// {len(rows)} entries; {skipped} initializer stubs in 0x{INIT_LO:X}..0x{INIT_HI:X} collapsed into one range.")
    L.append("// { address, size, flags (0 ghidra, 1 extra start, 2 synthetic range), name },")
    for a, (s, fl, n) in rows:
        L.append(f"{{ 0x{a:08X}u, 0x{s:X}u, {fl}, {c_string(n)} }},")
    L.append("")
    ok = emit(OUT, "\n".join(L), args.check)
    print(f"functions: {len(rows)} entries ({skipped} init stubs collapsed, {overlaps} Ghidra overlaps)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
