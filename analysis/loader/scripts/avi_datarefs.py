"""avi_datarefs.py - scan all initialized data of the XBE for 32-bit values that point into the
DSOUND / XMV / XPP / DOLBY sections (function pointers, vtables, callbacks, init-table entries).
Reports referencing location (section) and target; groups by referencing section.
Also scans .text/D3D for 'push imm32' / 'mov reg, imm32' (opcode 68 / B8+r / C7 xx) immediates
that point into those sections (callbacks passed as arguments)."""
import sys, struct, csv, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
names = {}
for row in csv.reader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8"), delimiter="\t"):
    if row[0] != "address": names[int(row[0], 16)] = row[3]
T = {s[0]: (s[1], s[1] + s[2]) for s in x.sections if s[0] in ("DSOUND", "XMV", "XPP", "DOLBY")}
def tsec(v):
    for n, (a, b) in T.items():
        if a <= v < b: return n
res = collections.defaultdict(list)
for n, va, vsz, raw, rsz in x.sections:
    if n in ("$$XTIMAGE", ".XTLID"): continue
    d = x.b[raw:raw + rsz]
    if n in (".rdata", ".data") or n in T:
        for i in range(0, len(d) - 3, 4):
            v = struct.unpack_from("<I", d, i)[0]
            t = tsec(v)
            if t and t != n and (n not in T or True):
                if n in T and t == n: continue
                res[(n, t)].append((va + i, v))
    if n in (".text", "D3D"):
        for i in range(len(d) - 5):
            if d[i] == 0x68 or 0xB8 <= d[i] <= 0xBF:
                v = struct.unpack_from("<I", d, i + 1)[0]
                t = tsec(v)
                if t and v in names: res[(n + "(imm)", t)].append((va + i, v))
for (src, t), lst in sorted(res.items()):
    print("== refs from %s into %s: %d" % (src, t, len(lst)))
    for a, v in lst[:60]:
        print("   @%08x -> %08x %s" % (a, v, names.get(v, "")))
