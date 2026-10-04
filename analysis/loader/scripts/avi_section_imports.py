"""avi_section_imports.py - what DSOUND / XMV / XPP code itself calls outside its own section
(kernel imports via thunks, XAPI/CRT functions in .text, D3D, other libs). Two sources:
callgraph.tsv (Ghidra) and a raw scan for 'call dword ptr [thunk]' (FF 15 imm32) into the kernel
thunk table 0x293560..0x2936fc."""
import sys, csv, struct, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
EXP = r"D:\FuturamaDecomp\analysis\retail_eu_export"
fsec, fname = {}, {}
for row in csv.reader(open(EXP + r"\functions.tsv", encoding="utf-8"), delimiter="\t"):
    if row[0] == "address": continue
    a = int(row[0], 16); fsec[a] = row[2]; fname[a] = row[3]
import bisect
starts = sorted(fsec)
def sec_of(a):
    return x.section(a)
out = collections.defaultdict(collections.Counter)
for row in csv.reader(open(EXP + r"\callgraph.tsv", encoding="utf-8"), delimiter="\t"):
    if row[0] == "caller": continue
    try: c = int(row[0], 16)
    except ValueError: continue
    cs = fsec.get(c)
    if cs not in ("DSOUND", "XMV", "XPP"): continue
    callee = row[2]
    if callee.startswith("EXTERNAL"):
        out[cs]["kernel:" + row[3]] += 1; continue
    t = int(callee, 16)
    if fsec.get(t) != cs: out[cs]["%s:%s" % (fsec.get(t), row[3])] += 1
# raw FF15 scan for thunk calls
TH0, TH1 = 0x293560, 0x293560 + 4 * 112
thname = {}
for row in csv.reader(open(EXP + r"\callgraph.tsv", encoding="utf-8"), delimiter="\t"):
    pass
raw = collections.defaultdict(collections.Counter)
for n, va, vsz, r0, rsz in x.sections:
    if n not in ("DSOUND", "XMV", "XPP"): continue
    d = x.b[r0:r0 + rsz]
    for i in range(len(d) - 6):
        if d[i] == 0xFF and d[i + 1] in (0x15, 0x25):
            p = struct.unpack_from("<I", d, i + 2)[0]
            if TH0 <= p < TH1:
                raw[n]["ord%d" % (x.u32(p) & 0x7fffffff)] += 1
for s in ("DSOUND", "XMV", "XPP"):
    print("==", s, "callgraph external callees:")
    for k, c in sorted(out[s].items()): print("   %3d  %s" % (c, k))
    print("   raw thunk calls (ordinal:count):", dict(sorted(raw[s].items(), key=lambda kv: int(kv[0][3:]))))
