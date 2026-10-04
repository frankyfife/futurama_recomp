"""avi_disasm.py - capstone disassembly of retail_eu XBE ranges with call-target names and
string annotations (for code Ghidra did not turn into functions).
Usage: python avi_disasm.py <start_hex> <end_hex> [...]"""
import sys, csv, re
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
import capstone
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
names = {}
with open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8") as f:
    r = csv.reader(f, delimiter="\t"); next(r)
    for row in r: names[int(row[0], 16)] = row[3]
def ann(v):
    if v in names: return names[v]
    sec = x.section(v)
    if sec in (".rdata", ".data") and x.va2off(v) is not None:
        try:
            s = x.cstr(v)
            if s and len(s) >= 3 and all(32 <= ord(c) < 127 for c in s[:40]): return '"%s"' % s[:60]
        except Exception: pass
    return None
args = sys.argv[1:]
for i in range(0, len(args), 2):
    a, b = int(args[i], 16), int(args[i + 1], 16)
    for ins in md.disasm(x.read(a, b - a), a):
        notes = []
        for m in re.findall(r"0x[0-9a-f]+", ins.op_str):
            v = int(m, 16)
            n = ann(v)
            if n: notes.append(n)
        mark = ("<<FUNC %s>> " % names[ins.address]) if ins.address in names else ""
        print("%s%08x  %-8s %-40s %s" % (mark, ins.address, ins.mnemonic, ins.op_str, ("; " + ", ".join(notes)) if notes else ""))
