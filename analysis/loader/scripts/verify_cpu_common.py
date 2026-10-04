"""verify_cpu_common.py - shared helpers for the independent re-check of cpu.md.

Deliberately does NOT import cpu_rd/cpu_scan (the scripts under review).
"""
import sys, struct, bisect
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

XBE = r"D:\FuturamaDecomp\orig\retail_eu\default.xbe"
EXP = r"D:\FuturamaDecomp\analysis\retail_eu_export"
LOADER = r"D:\FuturamaDecomp\analysis\loader"
X = Xbe(XBE)
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
mdn = Cs(CS_ARCH_X86, CS_MODE_32)   # no detail, fast

CODE = (".text", "D3D", "DSOUND", "XMV", "XPP")


def sec_ranges(names=CODE):
    out = []
    for (n, va, vsz, raw, rsz) in X.sections:
        if n in names:
            out.append((n, va, va + rsz, raw))
    return out


def load_funcs():
    fs = []
    for l in open(EXP + r"\functions.tsv", encoding="utf-8"):
        p = l.rstrip("\n").split("\t")
        if p[0] == "address":
            continue
        fs.append((int(p[0], 16), int(p[1]), p[3]))
    fs.sort()
    return fs


FUNCS = load_funcs()
FSTARTS = [f[0] for f in FUNCS]
FBYNAME = {f[2]: f[0] for f in FUNCS}


def owner(va):
    i = bisect.bisect_right(FSTARTS, va) - 1
    if i < 0:
        return None
    s, sz, nm = FUNCS[i]
    return (s, sz, nm, va < s + sz)


def dis(va, n=1, size=None):
    b = X.read(va, size or 16 * n)
    return list(mdn.disasm(b, va, n)) if size is None else list(mdn.disasm(b, va))


def dis_range(a, b):
    return list(mdn.disasm(X.read(a, b - a), a))


def show(a, b):
    for i in dis_range(a, b):
        print("  %08x  %-22s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))


def callgraph():
    cg = []
    for l in open(EXP + r"\callgraph.tsv", encoding="utf-8"):
        p = l.rstrip("\n").split("\t")
        if p[0] == "caller":
            continue
        cg.append((int(p[0], 16), p[1], int(p[2], 16), p[3]))
    return cg
