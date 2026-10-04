"""Shared helpers for the verify_renderer_*.py scripts (independent review of renderer.md).
dis(va, n)       -> list of capstone instructions starting at va (n bytes)
func(va)         -> (name, size, block) from functions.tsv
decomp(va)       -> decompiled text of the function at va (Ghidra export)
callers(va)/callees(va) from callgraph.tsv
Usage as script: python verify_renderer_common.py dis <va> <nbytes> | dec <va> [...]"""
import sys, csv, re, os, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
import capstone
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
EXP = r"D:\FuturamaDecomp\analysis\retail_eu_export"
F = {}
for r in csv.DictReader(open(os.path.join(EXP, "functions.tsv")), delimiter="\t"):
    F[int(r["address"], 16)] = (r["name"], int(r["size"]), r["block"])
FS = sorted(F)
_cg = None
def _load_cg():
    global _cg
    if _cg is None:
        cr = collections.defaultdict(set); ce = collections.defaultdict(set)
        for r in csv.reader(open(os.path.join(EXP, "callgraph.tsv")), delimiter="\t"):
            try: a, b = int(r[0], 16), int(r[2], 16)
            except ValueError: continue
            cr[b].add(a); ce[a].add(b)
        _cg = (cr, ce)
    return _cg
def callers(va): return _load_cg()[0].get(va, set())
def callees(va): return _load_cg()[1].get(va, set())
def func(va): return F.get(va)
def containing(va):
    import bisect
    i = bisect.bisect_right(FS, va) - 1
    if i >= 0 and FS[i] <= va < FS[i] + max(F[FS[i]][1], 1): return FS[i]
    return None
def dis(va, n=None):
    if n is None: n = F.get(va, ("", 64, ""))[1]
    return list(md.disasm(X.read(va, n), va))
def fmt(ins): return f"{ins.address:08x}: {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}"
HDR = re.compile(r"^// ==== FUNC ([0-9a-f]{8}) (\S+) size=(\d+) ====", re.M)
_dec = None
def _load_dec():
    global _dec
    if _dec is None:
        _dec = {}
        D = os.path.join(EXP, "decomp")
        for fn in sorted(os.listdir(D)):
            t = open(os.path.join(D, fn), encoding="utf-8", errors="replace").read()
            ms = list(HDR.finditer(t))
            for i, m in enumerate(ms):
                end = ms[i + 1].start() if i + 1 < len(ms) else len(t)
                _dec[int(m.group(1), 16)] = t[m.start():end]
    return _dec
def decomp(va): return _load_dec().get(va, "")
def alldecomp(): return _load_dec()
if __name__ == "__main__":
    if sys.argv[1] == "dis":
        for i in dis(int(sys.argv[2], 16), int(sys.argv[3], 0) if len(sys.argv) > 3 else None): print(fmt(i))
    elif sys.argv[1] == "dec":
        for a in sys.argv[2:]: print(decomp(int(a, 16)) or f"// none {a}")
