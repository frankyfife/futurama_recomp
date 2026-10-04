"""verify_avi_xmvrefs.py - are all 64 XMV-section functions referenced (direct E8/E9 call/jmp from
anywhere, or an absolute 32-bit pointer anywhere in the image, or as jump-table/callback)?
Checks the avi.md claim 'Alle 64 XMV-Funktionen werden referenziert'."""
import struct, collections
from verify_avi_common import *
xs = [a for a in FS if F[a][2] == "XMV"]
refs = collections.defaultdict(list)
for n, va, vsz, raw, rsz in x.sections:
    d = x.b[raw:raw + rsz]
    for i in range(len(d) - 4):
        op = d[i]
        if op in (0xE8, 0xE9) and n in (".text", "D3D", "DSOUND", "XMV", "XPP"):
            t = (va + i + 5 + struct.unpack_from("<i", d, i + 1)[0]) & 0xffffffff
            if t in F and F[t][2] == "XMV": refs[t].append(("rel", va + i))
        v = struct.unpack_from("<I", d, i)[0]
        if v in F and F[v][2] == "XMV": refs[v].append(("abs", va + i))
unref = [a for a in xs if not refs[a]]
print("XMV functions:", len(xs), "with some reference:", len(xs) - len(unref))
for a in unref: print("  unreferenced:", hex(a), name(a), F[a][1])
# also: Ghidra callgraph callers
import csv, os
cg = collections.defaultdict(set)
for row in csv.reader(open(os.path.join(EXP, "callgraph.tsv"), encoding="utf-8"), delimiter="\t"):
    try: cg[int(row[2], 16)].add(int(row[0], 16))
    except ValueError: pass
print("no callgraph caller and no raw ref:", [hex(a) for a in xs if not cg.get(a) and not refs[a]])
