"""Shared helpers for verify_files_r2_*.py (second, independent review of files.md).
Only reads: orig/retail_eu/default.xbe, analysis/retail_eu_export/*, the disc ISO (read-only)."""
import sys, os, re, struct, glob, bisect, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_IMM

XBE = r"D:\FuturamaDecomp\orig\retail_eu\default.xbe"
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
EXP = r"D:\FuturamaDecomp\analysis\retail_eu_export"
X = Xbe(XBE)
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
CODE_SECTIONS = (".text", "D3D", "DSOUND", "XMV", "XPP", "DOLBY")

FUNCS = []
NAMES = {}
for line in open(EXP + r"\functions.tsv", encoding="utf-8"):
    p = line.rstrip("\n").split("\t")
    if p[0] == "address":
        continue
    a = int(p[0], 16)
    FUNCS.append((a, int(p[1]), p[3]))
    NAMES[a] = p[3]
FUNCS.sort()
STARTS = [f[0] for f in FUNCS]


def owner(a):
    i = bisect.bisect_right(STARTS, a) - 1
    if i < 0:
        return None
    f = FUNCS[i]
    return f if a < f[0] + max(f[1], 1) else (f[0], f[1], f[2] + "+out")


_DEC = None


def _load_decomp():
    global _DEC
    if _DEC is None:
        _DEC = {}
        for fn in glob.glob(EXP + r"\decomp\*.c"):
            t = open(fn, encoding="utf-8", errors="replace").read()
            parts = re.split(r"(?m)^// ==== FUNC ([0-9a-f]{8}) ", t)
            for k in range(1, len(parts), 2):
                _DEC[int(parts[k], 16)] = "// ==== FUNC " + parts[k] + " " + parts[k + 1]
    return _DEC


def decomp(addr):
    return _load_decomp().get(addr)


def dis(start, end):
    return list(md.disasm(X.read(start, end - start), start))


def dis_func(addr):
    f = owner(addr)
    return dis(f[0], f[0] + f[1])


CG = None


def callgraph():
    global CG
    if CG is None:
        CG = []
        for line in open(EXP + r"\callgraph.tsv", encoding="utf-8"):
            p = line.rstrip("\n").split("\t")
            if p[0] == "caller":
                continue
            CG.append(p)
    return CG


def callers_of_name(sub):
    return sorted({(p[0], p[1]) for p in callgraph() if p[3].endswith(sub)})


def callers_of(addr):
    a = "%08x" % addr
    return sorted({(p[0], p[1]) for p in callgraph() if p[2] == a})


def raw_calls_to(target):
    """all E8 rel32 call sites to target in code sections"""
    hits = []
    for n, va, vsz, raw, rsz in X.sections:
        if n not in CODE_SECTIONS:
            continue
        blob = X.b[raw:raw + rsz]
        i = blob.find(b"\xe8")
        while i >= 0:
            if i + 5 <= len(blob):
                d = struct.unpack_from("<i", blob, i + 1)[0]
                if (va + i + 5 + d) & 0xFFFFFFFF == target:
                    hits.append(va + i)
            i = blob.find(b"\xe8", i + 1)
    return hits


def raw_dword_refs(v, sections=None):
    needle = struct.pack("<I", v)
    hits = []
    for n, va, vsz, raw, rsz in X.sections:
        if sections and n not in sections:
            continue
        blob = X.b[raw:raw + rsz]
        i = blob.find(needle)
        while i >= 0:
            hits.append((n, va + i))
            i = blob.find(needle, i + 1)
    return hits


def thunk_table():
    """slot VA -> raw ordinal"""
    out = {}
    va = 0x293560
    while True:
        v = X.u32(va)
        if v == 0:
            break
        out[va] = v & 0x7FFFFFFF
        va += 4
    return out


def pushes_before(insns, idx, maxn=14):
    """count push instructions immediately before insns[idx] (stop at call/ret/jmp/label-ish)."""
    n = 0
    j = idx - 1
    while j >= 0 and n < maxn:
        m = insns[j].mnemonic
        if m == "push":
            n += 1
        elif m in ("call", "ret", "jmp") or m.startswith("j"):
            break
        j -= 1
    return n


def fmt(i):
    return "%08x  %-22s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str)


def rstr(va):
    c = X.cstr(va)
    return c
