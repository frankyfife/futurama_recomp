"""avi_api_surface.py - API surface of DSOUND / XMV / XPP sections.
For every function in the given sections list callers that live OUTSIDE that section
(= API actually used by the game/NetImmerse/other libs). Uses Ghidra export TSVs.
Usage: python avi_api_surface.py [SECTION ...]
"""
import sys, csv, collections, os
EXP = r"D:\FuturamaDecomp\analysis\retail_eu_export"
funcs = {}
order = []
with open(os.path.join(EXP, "functions.tsv"), encoding="utf-8") as f:
    r = csv.reader(f, delimiter="\t"); next(r)
    for row in r:
        a = int(row[0], 16); funcs[a] = (int(row[1]), row[2], row[3]); order.append(a)
order.sort()
import bisect
def owner(a):
    i = bisect.bisect_right(order, a) - 1
    return order[i] if i >= 0 else None
callers = collections.defaultdict(set)
with open(os.path.join(EXP, "callgraph.tsv"), encoding="utf-8") as f:
    r = csv.reader(f, delimiter="\t"); next(r)
    for row in r:
        try: callers[int(row[2], 16)].add(int(row[0], 16))
        except ValueError: pass
secs = sys.argv[1:] or ["DSOUND", "XMV", "XPP"]
for sec in secs:
    print("=" * 20, sec)
    for a in order:
        sz, blk, name = funcs[a]
        if blk != sec: continue
        ext = sorted(c for c in callers.get(a, ()) if funcs.get(c, (0, "?", ""))[1] != sec)
        if not ext: continue
        print("%08x %-55s ext_callers=%d" % (a, name, len(ext)))
        for c in ext:
            print("      <- %08x %s [%s]" % (c, funcs[c][2], funcs[c][1]))
