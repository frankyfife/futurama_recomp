"""Caller counts quoted in startup.md: game allocator new/delete, CRT malloc, dt global references, Critical
Section users, LocalAlloc users."""
from verify_startup_common import *
import collections
cg = collections.defaultdict(set)
for l in open(E + r"\callgraph.tsv", encoding="utf-8", errors="replace"):
    p = l.rstrip("\r\n").split("\t")
    if len(p) == 4 and p[0] != "caller":
        cg[p[2]].add(p[0])
def callers_cg(a): return cg.get("%08x" % a, set())
for a in (0xa4900, 0xa48e0, 0xa3880, 0x16a083, 0x16a030, 0xa4520, 0xf63e0):
    r = raw_calls(a)
    print("%08x %-30s callgraph callers %4d | raw E8/E9 sites %4d (in %d functions)" % (
        a, fname(a), len(callers_cg(a)), len(r), len(set((func_of(c) or (c,))[0] for c, k in r))))
print("dt [0x2d6748] code refs:", len(abs_refs(0x2d6748, CODE)), " fps [0x2d6744]:", len(abs_refs(0x2d6744, CODE)),
      " fixed flag [0x2d674c]:", [(hex(a), (func_of(a) or (0, 0, '-'))[2]) for n, a in abs_refs(0x2d674c, CODE)])
pdis(0x16a083, 0x30, True)
print(decomp(0x16a030))
for o in (277, 294, 291):
    s = TH.index(o)
    calls, other = kcall_sites(s)
    fs_ = collections.Counter(("%x %s" % (func_of(c)[0], func_of(c)[2])) if func_of(c) else "<nofunc %x>" % c for c in calls)
    print("ord %d sites %d:" % (o, len(calls)), dict(fs_), "other refs", [hex(x) for x in other])
