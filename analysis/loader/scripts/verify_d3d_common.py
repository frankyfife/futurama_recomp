"""verify_d3d_common.py - independent helpers for the verify_d3d_*.py scripts (reviewer).
Does NOT import d3d_common.py on purpose."""
import csv, struct, sys, bisect
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

ROOT = r"D:\FuturamaDecomp"
EXP = ROOT + r"\analysis\retail_eu_export"
X = Xbe(ROOT + r"\orig\retail_eu\default.xbe")
SEC = {n: (va, vsz, raw, rsz) for n, va, vsz, raw, rsz in X.sections}
D3D_LO = SEC["D3D"][0]
D3D_RAWHI = D3D_LO + SEC["D3D"][3]
D3D_HI = D3D_LO + SEC["D3D"][1]
FUNCS = []
for r in csv.DictReader(open(EXP + r"\functions.tsv"), delimiter="\t"):
    FUNCS.append((int(r["address"], 16), int(r["size"]), r["block"], r["name"]))
FUNCS.sort()
STARTS = [f[0] for f in FUNCS]
BYADDR = {f[0]: f for f in FUNCS}
D3DF = [f for f in FUNCS if f[2] == "D3D"]
D3DSET = {f[0] for f in D3DF}
MD = Cs(CS_ARCH_X86, CS_MODE_32); MD.detail = True
CODE_SECS = [".text", "DSOUND", "XMV", "XPP", "D3D"]

def func_of(a):
    i = bisect.bisect_right(STARTS, a) - 1
    if i < 0: return None
    return FUNCS[i]

def sec_bytes(name):
    va, vsz, raw, rsz = SEC[name]
    return va, X.b[raw:raw + rsz]

def dis(a, n):
    return list(MD.disasm(X.read(a, n), a))
