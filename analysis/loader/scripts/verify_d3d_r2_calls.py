"""verify_d3d_r2_calls.py - recount external D3D call sites (review pass 2).

1. linear sweep of .text/DSOUND/XMV/XPP: every call/jmp/jcc with an immediate target
   anywhere in the D3D code range (not only Ghidra function starts), plus every
   call/jmp [abs] whose memory slot holds a D3D code address.
2. brute-force raw byte scan for E8/E9/0F8x rel32 into the D3D code range, to see whether
   the sweep missed something (instruction desync).
3. compare per-callee counts and caller groups with d3d_functions.tsv and the tables of
   d3d.md section 3 (rows '| 0x... | ... | ret | a/b | ...')."""
import re, csv, collections
from verify_d3d_r2_common import *

sites = []   # (insn, target, mnemonic, caller_start, caller_name)
insn_starts = set()
for s in [".text", "DSOUND", "XMV", "XPP"]:
    for ins in sweep(s):
        insn_starts.add(ins.address)
        if ins.mnemonic == "call" or ins.mnemonic.startswith("j"):
            op = ins.operands[0] if ins.operands else None
            if op is None:
                continue
            t = None
            if op.type == X86_OP_IMM:
                t = op.imm & 0xffffffff
            elif op.type == X86_OP_MEM and not op.mem.base and not op.mem.index:
                slot = op.mem.disp & 0xffffffff
                if XB.read(slot, 4):
                    t = u32(slot)
                    if not (D3D_LO <= t < D3D_CODE_END):
                        t = None
                    else:
                        print("INDIRECT via slot %08x -> %08x at %08x" % (slot, t, ins.address))
            if t is not None and D3D_LO <= t < D3D_CODE_END:
                o = owner(ins.address)
                sites.append((ins.address, t, ins.mnemonic, o[0], o[1]))

print("sweep: sites into D3D code:", len(sites))
nonstart = [x for x in sites if x[1] not in D3DFN]
print("  of which target is NOT a Ghidra D3D function start:", len(nonstart))
for x in nonstart:
    print("    %08x %s -> %08x" % (x[0], x[2], x[1]))
print("  kinds:", collections.Counter(x[2] for x in sites))
tgts = collections.defaultdict(list)
for x in sites:
    tgts[x[1]].append(x)
print("  distinct targets:", len(tgts), " named:", sum(1 for t in tgts if not D3DFN[t][3].startswith("FUN_")))
callers = {}
for x in sites:
    callers[x[3]] = x[4]
print("  distinct caller functions:", len(callers))
g = collections.Counter(group(a, n) for a, n in callers.items())
print("  caller groups:", dict(g))
gaps = sorted({(a, n) for a, n in callers.items() if n.startswith("gap_")})
print("  callers that are gaps behind Ghidra functions:", len(gaps), [n for a, n in gaps])

# raw scan
raw = []
for s in [".text", "DSOUND", "XMV", "XPP", "DOLBY"]:
    d = SECS[s]
    b = XB.b[d["raw"]:d["raw"] + d["rsz"]]
    for i in range(len(b) - 6):
        op = b[i]
        if op in (0xE8, 0xE9):
            t = (d["va"] + i + 5 + struct.unpack_from("<i", b, i + 1)[0]) & 0xffffffff
            if D3D_LO <= t < D3D_CODE_END:
                raw.append((d["va"] + i, t, s, "e8" if op == 0xE8 else "e9"))
        if op == 0x0F and 0x80 <= b[i + 1] <= 0x8F:
            t = (d["va"] + i + 6 + struct.unpack_from("<i", b, i + 2)[0]) & 0xffffffff
            if D3D_LO <= t < D3D_CODE_END:
                raw.append((d["va"] + i, t, s, "jcc"))
swept = {x[0] for x in sites}
print("raw rel32 hits into D3D code:", len(raw), collections.Counter(r[3] for r in raw))
extra = [r for r in raw if r[0] not in swept]
print("raw hits not found by sweep:", len(extra))
for r in extra:
    print("   %08x %s -> %08x (%s) insn_start=%s target_is_func=%s" % (r[0], r[3], r[1], r[2], r[0] in insn_starts, r[1] in D3DFN))

# compare with TSV
tsv = {int(r["address"], 16): r for r in csv.DictReader(open(ROOT + r"\analysis\loader\d3d_functions.tsv"), delimiter="\t")}
bad = 0
for a, r in tsv.items():
    lst = tgts.get(a, [])
    ns, nf = len(lst), len({x[3] for x in lst})
    if ns != int(r["ext_call_sites"]) or nf != int(r["ext_callers"]):
        bad += 1
        print("TSV MISMATCH %08x %s tsv %s/%s mine %d/%d" % (a, r["name"], r["ext_call_sites"], r["ext_callers"], ns, nf))
    gg = collections.Counter(group(c, n) for c, n in {(x[3], x[4]) for x in lst})
    for col, key in (("callers_NiXBoxRenderer", "R"), ("callers_NiXBoxLib", "X"), ("callers_Game", "G"), ("callers_XMV", "XMV")):
        if int(r[col]) != gg.get(key, 0):
            bad += 1
            print("TSV GROUP MISMATCH %08x %s %s tsv %s mine %d" % (a, r["name"], col, r[col], gg.get(key, 0)))
print("TSV rows:", len(tsv), "mismatches:", bad)
cat = collections.defaultdict(lambda: [0, 0])
for a, r in tsv.items():
    if int(r["ext_call_sites"]):
        cat[r["category"]][0] += 1
        cat[r["category"]][1] += len(tgts.get(a, []))
print("categories (functions/sites, sites recomputed):", {k: tuple(v) for k, v in cat.items()})

# compare with d3d.md section-3 tables
md = open(ROOT + r"\analysis\loader\d3d.md", encoding="utf-8").read()
rows = re.findall(r"^\| (0x[0-9a-f]+) \| `?([^|`]+)`?[^|]*\| ([0-9a-fx–-]+) \| (\d+)/(\d+) \|", md, re.M)
mm = 0
for ad, nm, ret, s_, f_ in rows:
    a = int(ad, 16)
    lst = tgts.get(a, [])
    if (len(lst), len({x[3] for x in lst})) != (int(s_), int(f_)):
        mm += 1
        print("MD MISMATCH", ad, nm, s_, f_, "mine", len(lst), len({x[3] for x in lst}))
print("md table rows checked:", len(rows), "mismatches:", mm)
