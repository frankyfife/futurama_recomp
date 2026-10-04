"""Build kernel_imports.tsv and the tables of kernel.md from kernel_raw.json.

Run after kernel_analyze.py:
    python kernel_analyze.py && python kernel_report.py
Outputs:
    analysis/loader/kernel_imports.tsv
    analysis/loader/scripts/kernel_tables.md   (tables, included into kernel.md)
    analysis/loader/scripts/kernel_crosscheck.txt (Ghidra name cross-check details)
"""
import collections
import csv
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kernel_common import EXPORT, OUT, Functions, load_xbe  # noqa: E402
from kernel_host import HOST  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
raw = json.load(open(os.path.join(HERE, "kernel_raw.json")))
x = load_xbe()
F = Functions()
thunks = raw["thunks"]
NAME = {t["slot"]: t["name"] for t in thunks}
fname = {int(k): v for k, v in raw["func_names"].items()}
for a, v in raw["stubs"].items():
    fname[int(a)] = F.name(int(a)) or "stub_%06x" % int(a)

REPLACED = {"D3D", "DSOUND", "XMV", "XPP"}
# Funktionsexporte, die nie aufgerufen, sondern nur als Adresse weitergegeben werden:
# IoInvalidDeviceRequest -> 10 Dispatch-Slots ab 0x28c004 (FUN_0028d254),
# NtUserIoApcDispatcher -> ApcRoutine-Argument von NtReadFile/NtWriteFile (0x0F7625/0x0F7676).
FUNC_ADDR_ONLY = {"IoInvalidDeviceRequest", "NtUserIoApcDispatcher"}


def module(f):
    sec = x.section(f)
    if sec in REPLACED:
        return sec
    if 0xF3DE0 <= f < 0xFA5A0 or 0x1707A0 <= f < 0x170ABA:
        return "XAPI"
    if 0x1668CC <= f < 0x171000:
        return "LIBC"
    return "GAME"


# ------------------------------------------------------------ Ghidra cross-check
decomp = {}
for path in glob.glob(os.path.join(EXPORT, "decomp", "*.c")):
    cur, buf = None, []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("// ==== FUNC "):
                if cur is not None:
                    decomp[cur] = "".join(buf)
                cur = int(line.split()[3], 16)
                buf = []
            else:
                buf.append(line)
    if cur is not None:
        decomp[cur] = "".join(buf)

ext = collections.defaultdict(lambda: collections.defaultdict(set))  # idx -> name -> callers
with open(os.path.join(EXPORT, "callgraph.tsv"), encoding="utf-8") as fh:
    for row in csv.DictReader(fh, delimiter="\t"):
        if row["callee"].startswith("EXTERNAL:"):
            idx = int(row["callee"].split(":")[1], 16)
            ext[idx][row["callee_name"].replace("xboxkrnl.exe::", "")].add(int(row["caller"], 16))

refs_by_slot = collections.defaultdict(list)
for r in raw["refs"]:
    refs_by_slot[r["slot"]].append(r)
sites_by_slot = collections.defaultdict(list)
for st in raw["sites"]:
    sites_by_slot[st["slot"]].append(st)
stub_by_slot = collections.defaultdict(list)
for a, v in raw["stubs"].items():
    stub_by_slot[v["slot"]].append((int(a), v))

cross = {}
cc_lines = []
for t in thunks:
    s, nm = t["slot"], t["name"]
    parts, ok = [], True
    names_ext = set(ext.get(s + 1, {}).keys())
    if names_ext:
        good = names_ext == {nm}
        ok &= good
        parts.append("callgraph EXTERNAL:%02x=%s%s" % (s + 1, "/".join(sorted(names_ext)), "" if good else " MISMATCH"))
    aliases = {nm} | {v["name"].split("::")[-1] for a, v in stub_by_slot[s]}
    pat = re.compile(r"\b(" + "|".join(re.escape(n) for n in sorted(aliases)) + r")(_exref)?\b")
    funcs = set()
    for r in refs_by_slot[s]:
        if r["func"] in decomp:
            funcs.add(r["func"])
    for a, v in stub_by_slot[s]:
        for c in v["callers"]:
            if c in decomp:
                funcs.add(c)
    hit = sum(1 for f in funcs if pat.search(decomp[f]))
    if funcs:
        parts.append("decomp %d/%d" % (hit, len(funcs)))
        ok &= hit == len(funcs)
    for a, v in stub_by_slot[s]:
        g = v["name"]
        good = g.split("::")[-1] == nm or g == "XAPILIB::XPhysicalFree"
        parts.append("stub %06x=%s" % (a, g))
        ok &= good
    if not parts:
        cross[s] = "n/a: keine Ghidra-Referenz (Code liegt ausserhalb von Ghidra-Funktionen)"
    else:
        cross[s] = ("OK: " if ok else "CHECK: ") + "; ".join(parts)
    cc_lines.append("%3d %-30s %s" % (s, nm, cross[s]))
with open(os.path.join(HERE, "kernel_crosscheck.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(cc_lines) + "\n")

# ------------------------------------------------------------ per slot rows
purge = {int(k): v for k, v in raw["purge"].items()}
prof = {int(k): {int(c): n for c, n in v.items()} for k, v in raw["profiles"].items()}

rows = []
for t in thunks:
    s, nm, o = t["slot"], t["name"], t["ordinal"]
    ss = sites_by_slot[s]
    calls = [st for st in ss if not st["via"].startswith("tailjmp")]
    tails = [st for st in ss if st["via"].startswith("tailjmp")]
    rf = refs_by_slot[s]
    drs = [d for d in raw["datarefs"] if d["slot"] == s]
    callers = collections.defaultdict(set)
    for st in ss:
        callers[module(st["func"])].add(st["func"])
    for d in drs:
        f = next((r["func"] for r in rf if r["ins"] == d["ins"]), None)
        if f is not None:
            callers[module(f)].add(f)
    mods = sorted(callers)
    # kind
    # Reviewer-Korrektur (verify_kernel_*.py): die fruehere Heuristik "dereferenziert mit Displacement"
    # hat Datenexporte, deren Adresse nur weitergegeben oder indiziert wird (ExEventObjectType,
    # XboxHDKey, XboxSignatureKey, XboxAlternateSignatureKeys, XePublicKeyData), als ADDR gefuehrt.
    # ADDR = Funktionsexport, dessen Adresse nur weitergegeben wird; alles andere ohne Call = DATA.
    if ss:
        kind = "FUNC"
    elif nm in FUNC_ADDR_ONLY:
        kind = "ADDR"
    else:
        kind = "DATA"
    # convention / args
    conv, args, pev = "", "", ""
    if ss:
        n = len(ss)
        ecx = sum(st["ecx_set"] for st in ss) / n
        edx = sum(st["edx_set"] for st in ss) / n
        if calls and all(st["cleanup"] for st in calls):
            conv = "cdecl"
            args = "var (%s Push an der Site)" % ",".join(sorted({str(st["pushes"]) for st in calls}))
            pev = "Caller raeumt auf: " + ", ".join("add esp,0x%x" % st["cleanup"] for st in calls)
        elif purge.get(s, 0) == 0 and ecx >= 0.75:
            conv = "fastcall"
            args = "2 (ecx,edx)" if edx >= 0.75 else "1 (ecx)"
            pev = "ecx gesetzt %d/%d, edx %d/%d, keine Stack-Args" % (
                sum(st["ecx_set"] for st in ss), n, sum(st["edx_set"] for st in ss), n)
        else:
            conv = "stdcall"
            args = str(purge.get(s, 0) // 4)
            p = prof.get(s)
            if p:
                best = p[purge[s]]
                others = [v for c, v in p.items() if c != purge[s]]
                if others and best < min(others):
                    pev = "Stack-Solver eindeutig (%d vs. >=%d Inkonsistenzen)" % (best, min(others))
                else:
                    pev = "Push-Zaehlung (Solver ohne Constraint)"
            else:
                pev = "Push-Zaehlung"
            pc = collections.Counter(st["pend"] for st in calls if st["pend"] is not None)
            if pc:
                pev += "; pend=" + ",".join("%s:%d" % kv for kv in sorted(pc.items()))
    ret_used = ""
    if calls:
        ru = sum(1 for st in calls if st["eax_used"] in (True, "ret"))
        ret_used = "%d/%d" % (ru, len(calls))
    ev = ""
    if calls:
        st = calls[0]
        ev = "%06x %s" % (st["site"], st["bytes"])
    elif tails:
        ev = "%06x %s (jmp)" % (tails[0]["site"], tails[0]["bytes"])
    elif rf:
        ev = "%06x %s %s" % (rf[0]["ins"], rf[0]["mnem"], rf[0]["ops"])
    callers_txt = "; ".join("%s: %s" % (m, ", ".join(fname.get(f, "%06x" % f) for f in sorted(callers[m])))
                            for m in mods)
    callers_short = ", ".join("%s(%d)" % (m, len(callers[m])) for m in mods)
    only_rep = bool(mods) and set(mods) <= REPLACED
    h = HOST[nm]
    rows.append({
        "slot": s, "thunk_va": "0x%06x" % t["va"], "ordinal": o, "ordinal_hex": "0x%03x" % o, "name": nm,
        "kind": kind, "convention": conv, "args": args,
        "purge_bytes": "" if conv in ("", "cdecl") else str(purge.get(s, 0)),
        "call_sites": len(calls) + len(tails), "tail_jumps": len(tails), "refs": len(rf),
        "callers_short": callers_short, "callers": callers_txt, "modules": ",".join(mods),
        "only_replaced_libs": "ja" if only_rep else "nein", "ret_used": ret_used,
        "purge_evidence": pev, "ghidra_check": cross[s], "evidence_site": ev,
        "host_plan": h[0], "difficulty": h[1],
    })

cols = ["ordinal", "ordinal_hex", "name", "kind", "convention", "args", "purge_bytes", "call_sites",
        "tail_jumps", "refs", "callers_short", "callers", "modules", "only_replaced_libs", "ret_used",
        "purge_evidence", "ghidra_check", "slot", "thunk_va", "evidence_site", "host_plan", "difficulty"]
with open(os.path.join(OUT, "kernel_imports.tsv"), "w", newline="", encoding="utf-8") as fh:
    w = csv.writer(fh, delimiter="\t", lineterminator="\n")
    w.writerow(cols)
    for r in rows:
        w.writerow([str(r[c]).replace("\t", " ").replace("\n", " ") for c in cols])


# ------------------------------------------------------------ markdown tables
def md_escape(v):
    return str(v).replace("|", "\\|")


out = []
out.append("| Slot | Thunk | Ord | Name | Art | Konv. | Args | Sites | Aufrufer (Modul: Anzahl Fkt.) | nur ersetzte Libs | Host-Plan | Aufwand |")
out.append("|---:|---|---:|---|---|---|---|---:|---|---|---|---|")
for r in rows:
    out.append("| %d | %s | %d (%s) | %s | %s | %s | %s | %d | %s | %s | %s | %s |" % (
        r["slot"], r["thunk_va"], r["ordinal"], r["ordinal_hex"], r["name"], r["kind"], r["convention"],
        md_escape(r["args"]), r["call_sites"], r["callers_short"], r["only_replaced_libs"],
        md_escape(r["host_plan"]), r["difficulty"]))
tables = "\n".join(out) + "\n"

# statistics
st = collections.Counter(r["kind"] for r in rows)
cv = collections.Counter(r["convention"] for r in rows if r["kind"] == "FUNC")
mods_total = collections.Counter()
for stt in raw["sites"]:
    mods_total[module(stt["func"])] += 1
needed = [r for r in rows if r["only_replaced_libs"] == "nein"]
only = [r for r in rows if r["only_replaced_libs"] == "ja"]
diff = collections.Counter(r["difficulty"] for r in needed)
stats = {
    "kinds": dict(st), "conventions": dict(cv), "sites_total": len(raw["sites"]),
    "sites_by_module": dict(mods_total), "needed_without_replaced": len(needed),
    "only_replaced": len(only), "difficulty_needed": dict(diff),
    "crosscheck_ok": sum(1 for s in cross.values() if s.startswith("OK")),
    "only_replaced_names": [r["name"] for r in only],
    "needed_names": [r["name"] for r in needed],
}
with open(os.path.join(HERE, "kernel_tables.md"), "w", encoding="utf-8") as fh:
    fh.write(tables)
with open(os.path.join(HERE, "kernel_stats.json"), "w", encoding="utf-8") as fh:
    json.dump(stats, fh, indent=1)
print(json.dumps(stats, indent=1))
