"""verify_d3d_tables.py - recompute ret N, external sites / caller functions and caller
groups for every D3D function independently (raw E8/E9 scan; function attribution =
functions.tsv + the 11 vtable-only starts), compare with d3d_functions.tsv and the
section-3 tables of d3d.md."""
import struct, collections, csv, re
from capstone import CS_OP_IMM
from verify_d3d_common import *
EXTRA = [0x1556f0,0x1558e0,0x1561e0,0x156c30,0x15c530,0x15f8f0,0x15ffe0,0x160f70,0x160ff0,0x161180,0x165ed0]
starts = sorted(set(STARTS + EXTRA))
import bisect
def owner_start(a):
    return starts[bisect.bisect_right(starts, a) - 1]
def grp(c, s):
    nm = BYADDR[c][3] if c in BYADDR else "sub"
    if s == "XMV": return "M"
    if nm.startswith("NiXBoxRenderer::"): return "R"
    if 0x146020 <= c < 0x14cd50 or 0x155000 <= c < 0x166871: return "X"
    if c < 0xf3000: return "G"
    return "?"
sites = collections.defaultdict(list)
for s in [".text", "DSOUND", "XMV", "XPP"]:
    va, b = sec_bytes(s)
    for i in range(len(b) - 5):
        if b[i] in (0xE8, 0xE9):
            t = (va + i + 5 + struct.unpack_from("<i", b, i + 1)[0]) & 0xffffffff
            if t in D3DSET:
                sites[t].append((owner_start(va + i), s, b[i]))
def retn(a):
    i = STARTS.index(a)
    end = min(STARTS[i + 1] if i + 1 < len(STARTS) else D3D_RAWHI, D3D_RAWHI)
    r = set()
    for ins in dis(a, end - a):
        if ins.mnemonic == "ret": r.add(ins.op_str or "0")
    return ",".join(sorted(r)) or "-"
tsv = {int(r["address"], 16): r for r in csv.DictReader(open(ROOT + r"\analysis\loader\d3d_functions.tsv"), delimiter="\t")}
mismatch = 0
mine = {}
for a, sz, blk, nm in D3DF:
    l = sites.get(a, [])
    fs = sorted({c for c, s, op in l})
    g = collections.Counter(grp(c, s) for c, s in {(c, s) for c, s, op in l})
    mine[a] = (len(l), len(fs), g, retn(a))
    r = tsv[a]
    if int(r["ext_call_sites"]) != len(l) or int(r["ext_callers"]) != len(fs):
        mismatch += 1; print("TSV MISMATCH", hex(a), nm, r["ext_call_sites"], r["ext_callers"], len(l), len(fs))
    if l and r["ret"] != mine[a][3]:
        print("RET differs", hex(a), nm, "tsv", r["ret"], "mine", mine[a][3])
print("tsv mismatches:", mismatch)
# parse report tables
md = open(ROOT + r"\analysis\loader\d3d.md", encoding="utf-8").read()
for m in re.finditer(r"^\| (0x[0-9a-f]+) \| `([^`]+)`[^|]*\| ([^|]+) \| (\d+)/(\d+) \| ([^|]+) \|", md, re.M):
    a = int(m.group(1), 16); ret = m.group(3).strip(); s, f = int(m.group(4)), int(m.group(5))
    if a not in mine: print("not a D3D func?", m.group(0)); continue
    ms, mf, g, mr = mine[a]
    rr = mr.replace("0x", "")
    ok_ret = (ret in ("–", "-") and mr in ("-", "")) or ret.replace("0x", "") == rr or (ret == "0" and mr == "0")
    gs = " ".join("%s%d" % (k, v) for k, v in sorted(g.items()))
    flag = "" if (ms == s and mf == f) else "  <-- SITES"
    if not ok_ret: flag += "  <-- RET(%s)" % mr
    print("%08x %-38s rep ret=%-5s %d/%d  mine ret=%-5s %d/%d  groups %s | rep: %s%s" % (a, m.group(2), ret, s, f, mr, ms, mf, gs, m.group(6).strip(), flag))
