"""Common helpers for the second (independent) avi.md verification round.

Independent of the avi_*.py / verify_avi_*.py scripts of the earlier rounds.
"""
import bisect
import os
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe  # noqa: E402
import capstone  # noqa: E402

ROOT = r"D:\FuturamaDecomp"
EXP = os.path.join(ROOT, r"analysis\retail_eu_export")
OUT = os.path.join(ROOT, r"analysis\loader\scripts\out")
X = Xbe(os.path.join(ROOT, r"orig\retail_eu\default.xbe"))

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = False

SECT = {n: (va, vsz, raw, rsz) for n, va, vsz, raw, rsz in X.sections}


def sect_range(name):
    va, vsz, raw, rsz = SECT[name]
    return va, va + vsz


def load_funcs():
    funcs = []
    with open(os.path.join(EXP, "functions.tsv"), encoding="utf-8") as f:
        next(f)
        for line in f:
            p = line.rstrip("\n").split("\t")
            funcs.append((int(p[0], 16), int(p[1]), p[2], p[3]))
    funcs.sort()
    return funcs


FUNCS = load_funcs()
FSTART = [f[0] for f in FUNCS]
FNAME = {f[0]: f[3] for f in FUNCS}


def func_of(va):
    i = bisect.bisect_right(FSTART, va) - 1
    if i >= 0:
        a, sz, blk, nm = FUNCS[i]
        if a <= va < a + max(sz, 1):
            return a, nm
    return None, None


def u32(va):
    return X.u32(va)


def f32(va):
    return struct.unpack("<f", X.read(va, 4))[0]


def dis(va, n=40, stop_ret=False):
    """Disassemble n instructions starting at va."""
    code = X.read(va, n * 15)
    res = []
    for ins in MD.disasm(code, va):
        res.append(ins)
        if len(res) >= n:
            break
        if stop_ret and ins.mnemonic in ("ret", "retn"):
            break
    return res


def dis_range(a, b):
    code = X.read(a, b - a + 16)
    res = []
    for ins in MD.disasm(code, a):
        if ins.address >= b:
            break
        res.append(ins)
    return res


def fmt(ins):
    return "%08x  %-24s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str)


def linear_starts(a, b):
    """Linear-sweep instruction start set over [a,b) (skip 1 byte on invalid)."""
    code = X.read(a, b - a)
    starts = set()
    off = 0
    n = len(code)
    while off < n:
        got = False
        for ins in MD.disasm(code[off:off + 0x4000], a + off):
            starts.add(ins.address)
            off = ins.address - a + ins.size
            got = True
            if off >= n:
                break
        if not got:
            off += 1
    return starts


def decomp_func(addr):
    """Return Ghidra C text of the function starting at addr (or None)."""
    d = os.path.join(EXP, "decomp")
    tag = "// ==== FUNC %08x " % addr
    for fn in os.listdir(d):
        txt = open(os.path.join(d, fn), encoding="utf-8", errors="replace").read()
        i = txt.find(tag)
        if i >= 0:
            j = txt.find("// ==== FUNC ", i + 10)
            return txt[i:j if j > 0 else len(txt)]
    return None


def out_path(name):
    os.makedirs(OUT, exist_ok=True)
    return os.path.join(OUT, name)


class Tee:
    def __init__(self, name):
        self.f = open(out_path(name), "w", encoding="utf-8")

    def __call__(self, *a):
        s = " ".join(str(x) for x in a)
        print(s)
        self.f.write(s + "\n")
        self.f.flush()
