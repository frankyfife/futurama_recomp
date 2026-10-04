"""Kernel thunk table (0x293560) -> ordinal -> name as Ghidra (ghidra-xbe) labelled it.
Name source: callgraph.tsv callee 'EXTERNAL:<n>' == thunk slot n-1 (verified e.g. slot 12 ord 184
NtAllocateVirtualMemory, slot 56 ord 166 MmAllocateContiguousMemoryEx, slot 77 ord 255 PsCreateSystemThreadEx).
Data imports (no callers) are reported with the *_exref names found in the decompiled C."""
import sys, re, glob, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
E = r"D:\FuturamaDecomp\analysis\retail_eu_export"
names = {}; callers = collections.defaultdict(set)
for l in open(E + r"\callgraph.tsv", encoding="utf-8", errors="replace"):
    p = l.rstrip("\r\n").split("\t")
    if len(p) == 4 and p[2].startswith("EXTERNAL:"):
        n = int(p[2][9:], 16)
        names[n - 1] = p[3].replace("xboxkrnl.exe::", "")
        callers[n - 1].add(p[0])
slots = []
i = 0
while X.u32(0x293560 + 4 * i):
    slots.append(X.u32(0x293560 + 4 * i) & 0x7fffffff); i += 1
exref = set()
for f in glob.glob(E + r"\decomp\*.c"):
    exref |= set(re.findall(r"\b(\w+)_exref\b", open(f, encoding="utf-8", errors="replace").read()))
if __name__ == "__main__":
    for s, o in enumerate(slots):
        print("%3d  %08x  ord %3d  %-40s callers=%d" % (s, 0x293560 + 4 * s, o, names.get(s, "?(data import)"), len(callers[s])))
    print("data imports seen as *_exref in decomp:", sorted(exref))
