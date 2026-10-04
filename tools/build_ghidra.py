"""Rebuild the Ghidra database for one XBE from scratch, reproducibly.

Steps: import + auto-analysis (overwrite) -> create functions for the CRT
initializer tables -> recover NetImmerse classes -> apply symbols.

Usage: python tools/build_ghidra.py retail_eu
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
PROJECT_DIR = os.path.join(ROOT, "ghidra")
SCRIPTS = os.path.join(ROOT, "ghidra_scripts")
ANALYSIS = os.path.join(ROOT, "analysis")

TARGETS = {
    "retail_eu": dict(
        xbe=os.path.join(ROOT, "orig", "retail_eu", "default.xbe"),
        # (start, end) of __xi_a..__xi_z, __xc_a..__xc_z and the rtinit table, from XAPILIB::_cinit/_rtinit
        init_tables=["0x2d5b34", "0x2d5b48", "0x2bdef0", "0x2d5b30", "0x2bdee0", "0x2bdeec"],
    ),
}


def headless(target, *args, log):
    cmd = [sys.executable, os.path.join(TOOLS, "ghidra_headless.py"), PROJECT_DIR, f"Futurama/{target}", *args]
    with open(log, "w", encoding="utf-8") as f:
        rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT)
    report = [l.strip() for l in open(log, encoding="utf-8", errors="replace")
              if "REPORT" in l or "ERROR" in l or "> " in l and "INFO" in l]
    for l in report[-6:]:
        print("   ", l)
    if rc != 0:
        sys.exit(f"step failed (exit {rc}), see {log}")


def main(target):
    t = TARGETS[target]
    os.makedirs(ANALYSIS, exist_ok=True)
    prefix = os.path.join(ANALYSIS, f"{target}_ni")

    print("[1/3] import + auto-analysis")
    headless(target, "-import", t["xbe"], "-overwrite", "-loader", "XbeLoader", "-max-cpu", "16",
             "-analysisTimeoutPerFile", "7200", log=os.path.join(PROJECT_DIR, f"import_{target}.log"))

    print("[2/3] NetImmerse class recovery")
    subprocess.check_call([sys.executable, os.path.join(TOOLS, "niclasses.py"), t["xbe"], target, prefix])

    print("[3/3] initializer functions + symbols")
    headless(target, "-process", "default.xbe", "-noanalysis", "-scriptPath", SCRIPTS,
             "-postScript", "CreateFunctionsFromTable.java", *t["init_tables"],
             "-postScript", "ApplySymbols.java", prefix + "_symbols.tsv",
             "-postScript", "ExportFunctions.java", os.path.join(ANALYSIS, f"{target}_functions.tsv"),
             log=os.path.join(ANALYSIS, f"apply_{target}.log"))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "retail_eu")
