"""Reviewer check: module attribution of every kernel reference, the '37 only replaced libs' split,
the module boundaries used by the report, and TSV column consistency (kind, modules, sites).
"""
import csv
import json
import os
from collections import Counter, defaultdict

from verify_kernel_common import LOADER, XBE, Funcs, module_of

HERE = os.path.dirname(os.path.abspath(__file__))
F = Funcs()
v = json.load(open(os.path.join(HERE, "verify_kernel_sites.json")))
TSV = {int(r["slot"]): r for r in csv.DictReader(open(os.path.join(LOADER, "kernel_imports.tsv"), encoding="utf-8"), delimiter="\t")}
sites = defaultdict(list)
for k, l in v["sites"].items():
    for kind, a in l:
        sites[int(k)].append(int(a, 16))
for s, a in [(1, 0x292d4c), (3, 0xf792a), (3, 0xf9bc0), (7, 0x291a65), (26, 0xf7b3f), (41, 0xf9278),
             (41, 0xf950c), (93, 0x240735), (93, 0x240797), (104, 0x24ae48)]:
    sites[s].append(a)
refs = defaultdict(list)
for r in v["refs"]:
    refs[r["slot"]].append(int(r["va"], 16))

# --- boundaries: names found inside the XAPI / LIBC ranges of the report and outside
print("== names in .text by range")
cnt = Counter()
for a, sz, blk, nm in F.rows:
    if blk != ".text":
        continue
    m = module_of(a)
    kind = "XAPILIB" if nm.startswith("XAPILIB::") else ("Ni/vf" if ("::vf" in nm or nm.startswith("Ni")) else
                                                          ("FUN" if nm.startswith("FUN_") else "other"))
    cnt[(m, kind)] += 1
for k in sorted(cnt):
    print("  ", k, cnt[k])
print("  XAPILIB names outside XAPI range:", [(hex(a), nm) for a, sz, blk, nm in F.rows if nm.startswith("XAPILIB::") and blk == ".text" and module_of(a) != "XAPI"])
print("  Ni*/vf names inside XAPI/LIBC ranges:", [(hex(a), nm) for a, sz, blk, nm in F.rows if blk == ".text" and module_of(a) in ("XAPI", "LIBC") and ("::vf" in nm or nm.startswith("Ni"))][:10])

# --- per-import module sets (sites + all refs, i.e. data uses too)
only_rep = []
needed = []
mods_by_slot = {}
for s in range(len(TSV)):
    ms = set(module_of(a) for a in sites[s]) | set(module_of(a) for a in refs[s] if True)
    # stub calls: caller module counts, the stub location itself does not
    ms_calls = set(module_of(a) for a in sites[s])
    ms_data = set(module_of(a) for a in refs[s]) if not sites[s] else set()
    m = ms_calls | ms_data
    mods_by_slot[s] = m
    if m and m <= {"D3D", "DSOUND", "XPP", "XMV"}:
        only_rep.append(TSV[s]["name"])
    else:
        needed.append(TSV[s]["name"])
    tsv_m = set(TSV[s]["modules"].split(",")) if TSV[s]["modules"] else set()
    if tsv_m != m:
        print("MODULE SET DIFF slot %d %s tsv=%s mine=%s" % (s, TSV[s]["name"], tsv_m, m))
    if (TSV[s]["only_replaced_libs"] == "ja") != (m <= {"D3D", "DSOUND", "XPP", "XMV"} and bool(m)):
        print("ONLY_REPLACED DIFF", s, TSV[s]["name"])
print("only replaced:", len(only_rep), " needed:", len(needed))

# --- sites by module per import, e.g. RtlLeaveCriticalSection
for nm in ("RtlLeaveCriticalSection", "RtlEnterCriticalSection", "MmFreeContiguousMemory", "KeRaiseIrqlToDpcLevel", "KfLowerIrql"):
    s = [k for k, r in TSV.items() if r["name"] == nm][0]
    print("%-26s sites by module: %s" % (nm, dict(Counter(module_of(a) for a in sites[s]))))

# --- kind column
print("== kind column:", Counter(r["kind"] for r in TSV.values()))
for s, r in TSV.items():
    if r["kind"] != "FUNC":
        print("   slot %3d %-28s kind=%s sites=%s refs=%d" % (s, r["name"], r["kind"], r["call_sites"], len(refs[s])))

# --- difficulty split among needed
dif = Counter(TSV[s]["difficulty"] for s in TSV if TSV[s]["name"] in needed)
print("difficulty among needed:", dict(dif))
print("difficulty among only-replaced:", dict(Counter(TSV[s]["difficulty"] for s in TSV if TSV[s]["name"] in only_rep)))

# --- host_plan text claims about callers vs modules
for s, r in TSV.items():
    hp = r["host_plan"]
    for tag in ("Nur D3D/DSOUND/XPP", "Nur DSOUND/XPP", "Nur D3D/DSOUND", "Nur XPP", "Nur DSOUND", "Nur D3D"):
        if hp.startswith(tag) or (" " + tag) in hp:
            claimed = set(tag.replace("Nur ", "").split("/"))
            if claimed != mods_by_slot[s]:
                print("HOSTPLAN TEXT vs CALLERS slot %d %s: text '%s' but callers %s" % (s, r["name"], tag, mods_by_slot[s]))
            break
