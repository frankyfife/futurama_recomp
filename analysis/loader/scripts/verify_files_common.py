"""Shared helpers for the verify_files_*.py scripts (reviewer of files.md)."""
import sys, os, re, struct, glob, bisect, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
EXP = r"D:\FuturamaDecomp\analysis\retail_eu_export"
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = False
FUNCS = []
NAMES = {}
for line in open(EXP + r"\functions.tsv", encoding="utf-8"):
    p = line.rstrip("\n").split("\t")
    if p[0] == "address": continue
    a = int(p[0], 16); FUNCS.append((a, int(p[1]), p[3])); NAMES[a] = p[3]
FUNCS.sort()
STARTS = [f[0] for f in FUNCS]
def owner(a):
    i = bisect.bisect_right(STARTS, a) - 1
    return FUNCS[i] if i >= 0 else None
def decomp(addr):
    """Return decompiled C text of function at addr."""
    tag = "// ==== FUNC %08x" % addr
    tag2 = "// ==== FUNC 0x%08x" % addr
    for fn in glob.glob(EXP + r"\decomp\*.c"):
        t = open(fn, encoding="utf-8", errors="replace").read()
        for tg in (tag, tag2, "// ==== FUNC %x " % addr):
            i = t.find(tg)
            if i >= 0:
                j = t.find("// ==== FUNC", i + 10)
                return t[i:j if j > 0 else len(t)]
    return None
def dis(start, end):
    return list(md.disasm(X.read(start, end - start), start))
def callers(callee):
    out = []
    for line in open(EXP + r"\callgraph.tsv", encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        if p[0] == "caller": continue
        try:
            if int(p[2], 16) == callee: out.append((int(p[0], 16), p[1]))
        except ValueError: pass
    return out
def callers_by_name(name):
    out = []
    for line in open(EXP + r"\callgraph.tsv", encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        if len(p) > 3 and p[3] == name: out.append((p[0], p[1]))
    return out
def xrefs(v):
    needle = struct.pack("<I", v); hits = []
    for n, va, vsz, raw, rsz in X.sections:
        blob = X.b[raw:raw + rsz]; i = blob.find(needle)
        while i >= 0:
            hits.append((n, va + i)); i = blob.find(needle, i + 1)
    return hits
def calls_to(target):
    """raw E8 call sites to target across code sections"""
    hits = []
    for n, va, vsz, raw, rsz in X.sections:
        if n not in (".text", "D3D", "DSOUND", "XMV", "XPP"): continue
        blob = X.b[raw:raw + rsz]
        for i in range(len(blob) - 5):
            if blob[i] == 0xE8 and (va + i + 5 + struct.unpack_from("<i", blob, i + 1)[0]) & 0xffffffff == target:
                hits.append(va + i)
    return hits
