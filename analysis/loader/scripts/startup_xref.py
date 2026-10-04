"""Callees, callers and referenced strings of functions. usage: startup_xref.py [-r] 0xADDR ..."""
import sys, collections
E = r"D:\FuturamaDecomp\analysis\retail_eu_export"
callees = collections.defaultdict(list); callers = collections.defaultdict(list); names = {}
for l in open(E + r"\callgraph.tsv", encoding="utf-8", errors="replace"):
    p = l.rstrip("\r\n").split("\t")
    if len(p) != 4 or p[0] == "caller": continue
    callees[p[0]].append((p[2], p[3])); callers[p[2]].append((p[0], p[1])); names[p[0]] = p[1]
strs = collections.defaultdict(list)
for l in open(E + r"\strings.tsv", encoding="utf-8", errors="replace"):
    p = l.rstrip("\r\n").split("\t", 2)
    if len(p) != 3 or p[0] == "address": continue
    for f in p[1].split(","):
        f = f.replace("(data)", "")
        strs[f].append((p[0], p[2]))
def key(a): return "%08x" % int(a, 16)
if __name__ == "__main__":
    rev = "-r" in sys.argv
    for a in [x for x in sys.argv[1:] if x != "-r"]:
        k = key(a)
        print("==", k, names.get(k, ""))
        if rev:
            for c in sorted(set(callers[k])): print("   caller", c[0], c[1])
        else:
            for c in callees[k]: print("   ->", c[0], c[1])
        for s in strs[k]: print("   str", s[0], repr(s[1][:80]))
