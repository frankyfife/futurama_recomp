"""verify_avi_common.py - shared helpers for the independent verification of avi.md.
Loads the retail_eu XBE, Ghidra function list, and offers capstone disassembly helpers."""
import sys, csv, struct, bisect, re, os
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
import capstone

XBE = r"D:\FuturamaDecomp\orig\retail_eu\default.xbe"
EXP = r"D:\FuturamaDecomp\analysis\retail_eu_export"
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"

x = Xbe(XBE)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = False

F = {}  # addr -> (name, size, block)
with open(os.path.join(EXP, "functions.tsv"), encoding="utf-8") as f:
    r = csv.reader(f, delimiter="\t"); next(r)
    for row in r:
        F[int(row[0], 16)] = (row[3], int(row[1]), row[2])
FS = sorted(F)

SEC = {s[0]: s for s in x.sections}  # name -> (name, va, vsz, raw, rsz)

def sec_of(v):
    return x.section(v)

def owner(a):
    i = bisect.bisect_right(FS, a) - 1
    if i < 0: return None
    s = FS[i]
    return s if a < s + max(F[s][1], 1) else None

def name(a):
    return F[a][0] if a in F else "?"

def u32(v): return x.u32(v)
def u16(v): return struct.unpack("<H", x.read(v, 2))[0]
def f32(v): return struct.unpack("<f", x.read(v, 4))[0]
def f32i(i): return struct.unpack("<f", struct.pack("<I", i & 0xffffffff))[0]

def is_init(v):
    """True if VA lies in raw (initialized) part of a section."""
    for n, va, vsz, raw, rsz in x.sections:
        if va <= v < va + rsz: return True
    return False

def dis(a, b):
    return list(md.disasm(x.read(a, b - a), a))

def pdis(a, b, out=print):
    for i in dis(a, b):
        nm = ""
        for m in re.findall(r"0x[0-9a-f]+", i.op_str):
            v = int(m, 16)
            if v in F: nm += " ; " + F[v][0]
            elif 0x293560 <= v < 0x293560 + 112 * 4:
                nm += " ; thunk[%d]=%08x" % ((v - 0x293560) // 4, u32(v))
        out("%08x  %-24s %-7s %s%s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str, nm))

def rets(a, size=None):
    """set of 'ret N' immediates found by linear disassembly of the Ghidra function body."""
    sz = size or (F[a][1] if a in F else 0x400)
    out = set()
    for i in md.disasm(x.read(a, max(sz, 5)), a):
        if i.mnemonic == "ret":
            out.add(int(i.op_str, 16) if i.op_str else 0)
    return out

def decomp_func(addr):
    """Return Ghidra C text of the function at addr from the export (or None)."""
    tag = "// ==== FUNC %08x " % addr
    for fn in os.listdir(os.path.join(EXP, "decomp")):
        p = os.path.join(EXP, "decomp", fn)
        t = open(p, encoding="utf-8", errors="replace").read()
        i = t.find(tag)
        if i >= 0:
            j = t.find("// ==== FUNC ", i + 10)
            return t[i:j if j > 0 else len(t)]
    return None
