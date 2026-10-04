"""verify_cpu_fiber.py - key fact 8 / section 2.5: who calls the XAPI fiber API (callgraph.tsv and
raw E8/E9 byte scan), inline GetCurrentFiber sites in game code, QPC/QPF/OutputDebugStringA callers."""
import sys, struct, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *

cg = collections.defaultdict(set)
for l in open(EXP + r"\callgraph.tsv", encoding="utf-8"):
    p = l.rstrip("\n").split("\t")
    if p[0] == "caller" or p[2].startswith("EXTERNAL"):
        continue
    cg[int(p[2], 16)].add((int(p[0], 16), p[1]))


def raw_calls(t):
    out = []
    for (n, lo, hi, raw) in sec_ranges():
        data = X.b[raw:raw + (hi - lo)]
        for p in range(len(data) - 5):
            if data[p] in (0xe8, 0xe9) and ((lo + p + 5 + struct.unpack_from("<i", data, p + 1)[0]) & 0xffffffff) == t:
                out.append(lo + p)
    return out


for nm, t in [("CreateFiber", 0x1708b0), ("DeleteFiber", 0x17093c), ("SwitchToFiber", 0x17094f),
              ("ConvertThreadToFiber", 0x17098e), ("FUN_0013f250", 0x13f250), ("FUN_0013f590", 0x13f590),
              ("FUN_0013f1a0", 0x13f1a0), ("QPC", 0xf641b), ("QPF", 0xf642c), ("OutputDebugStringA", 0xf3de0),
              ("XapiThreadStartup", 0xf99da), ("fiber start", 0x17086b), ("IsBadReadPtr", 0x1709c9)]:
    rc = raw_calls(t)
    owners = collections.Counter()
    for a in rc:
        o = owner(a)
        owners[o[2] if o and o[3] else "?(gap after %s)" % (o[2] if o else "-")] += 1
    print("%-22s %08x callgraph callers=%3d  raw call/jmp sites=%3d in %3d owners" % (nm, t, len(cg[t]), len(rc), len(owners)))
    if len(owners) <= 8:
        print("     ", dict(owners))

print("\n== FUN_0013f250 around the fiber calls and inline GetCurrentFiber")
for a, b in [(0x13f3e0, 0x13f460)]:
    show(a, b)
print("\n== FUN_0013f590 around 0x13f5d5")
show(0x13f5c0, 0x13f600)
print("\n== QueryPerformanceCounter/Frequency 0xf641b..0xf643c")
show(0xf641b, 0xf643c)
print("\n== OutputDebugStringA 0xf3de0..0xf3e12")
show(0xf3de0, 0xf3e12)
print("\n== FUN_000d2b90 (game timer init) 0xd2b90..0xd2bd8")
show(0xd2b90, 0xd2bd8)
