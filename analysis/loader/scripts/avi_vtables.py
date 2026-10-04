"""avi_vtables.py - dump vtables (list of function pointers) from retail_eu XBE with export names.
Usage: python avi_vtables.py <va_hex>[:count] ..."""
import sys, csv
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
names = {}
with open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8") as f:
    r = csv.reader(f, delimiter="\t"); next(r)
    for row in r: names[int(row[0], 16)] = (row[3], int(row[1]))
for arg in sys.argv[1:]:
    va, _, cnt = arg.partition(":")
    va = int(va, 16); cnt = int(cnt) if cnt else 64
    print("vtable %08x" % va)
    for i in range(cnt):
        p = x.u32(va + 4 * i)
        if not (0x11000 <= p < 0x293560):
            print("  [%2d] +%03x %08x  (outside code) -> stop" % (i, 4*i, p)); break
        n, sz = names.get(p, ("<no Ghidra function>", 0))
        print("  [%2d] +%03x %08x %s size=%d" % (i, 4 * i, p, n, sz))
