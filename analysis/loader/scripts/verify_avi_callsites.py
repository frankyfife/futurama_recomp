"""verify_avi_callsites.py - raw E8 call-site counts for game functions named in avi.md
(SetRumble 0x9b6b0, vibration toggle 0x9ba40, GetSound wrapper 0xc97d0, ...), capstone validated,
compared with callgraph.tsv. Usage: python verify_avi_callsites.py <hex> ..."""
import struct, collections, csv, os
from verify_avi_common import *

def boundary_ok(a):
    o = owner(a)
    if o is not None and a - o < 0x8000:
        for ins in md.disasm(x.read(o, a - o + 16), o):
            if ins.address == a: return True
            if ins.address > a: break
    for back in range(1, 48):
        st = a - back
        for ins in md.disasm(x.read(st, back + 8), st):
            if ins.address == a: return True
            if ins.address > a: break
    return False

cg = collections.defaultdict(set)
with open(os.path.join(EXP, "callgraph.tsv"), encoding="utf-8") as f:
    r = csv.reader(f, delimiter="\t"); next(r)
    for row in r:
        try: cg[int(row[2], 16)].add(int(row[0], 16))
        except ValueError: pass

_, va, vsz, raw, rsz = SEC[".text"]
d = x.b[raw:raw + rsz]
for arg in sys.argv[1:]:
    t = int(arg, 16)
    sites = []
    for i in range(len(d) - 5):
        if d[i] != 0xE8: continue
        src = va + i
        if (src + 5 + struct.unpack_from("<i", d, i + 1)[0]) & 0xffffffff != t: continue
        if boundary_ok(src): sites.append(src)
    owners = collections.Counter(owner(s) for s in sites)
    print("%08x %s: raw sites=%d in %d owners (incl. %d sites outside Ghidra functions); callgraph callers=%d" % (
        t, name(t), len(sites), len([o for o in owners if o is not None]), owners.get(None, 0), len(cg.get(t, ()))))
    print("   sites:", " ".join("%x" % s for s in sites))
