"""avi_callconv.py - calling convention evidence for every DSOUND/XMV/XPP function the game calls:
'ret N' found by capstone in the function body (follows a single unconditional jmp for thunks)."""
import sys, csv, capstone, re
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
F = {}
for row in csv.reader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8"), delimiter="\t"):
    if row[0] != "address": F[int(row[0], 16)] = (row[3], int(row[1]))
TARGETS = [int(m.group(1), 16) for m in (re.match(r"([0-9a-f]{8}) ", l) for l in open(r"D:\FuturamaDecomp\analysis\loader\avi_rawcalls.out")) if m]
def rets(a, depth=0):
    name, sz = F.get(a, ("?", 0x200))
    out = set(); first = None
    for ins in md.disasm(x.read(a, max(sz, 5)), a):
        if first is None: first = ins
        if ins.mnemonic == "ret": out.add(int(ins.op_str, 16) if ins.op_str else 0)
    if not out and first is not None and first.mnemonic == "jmp" and depth < 2:
        try: return "jmp->%s %s" % (F.get(int(first.op_str, 16), ("?",))[0], rets(int(first.op_str, 16), depth + 1))
        except ValueError: pass
    return "ret " + "/".join(str(r) for r in sorted(out)) if out else "no ret found"
for t in TARGETS:
    print("%08x %-48s %s" % (t, F.get(t, ("?",))[0], rets(t)))
