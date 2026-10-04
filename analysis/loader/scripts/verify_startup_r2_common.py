"""Review round 2 for startup.md: independent helper module (does not import startup_* or verify_startup_common).
X       : Xbe of retail_eu
TH      : list of kernel ordinals in the thunk table (slot -> ordinal)
KNAME   : slot -> Ghidra import name taken from callgraph.tsv ("EXTERNAL:<slot+1>  xboxkrnl.exe::<name>")
dis/pdis: capstone disassembly with thunk/function annotation
func_of : Ghidra function containing a VA
decomp  : decompiled text of a function start
rawrefs : E8/E9 rel32 callers of a target (linear byte scan over all code sections)
absrefs : all little-endian dword occurrences of a value in the image
kcalls  : all `call/jmp [slot]` sites plus other references to a thunk slot
"""
import sys, struct, re, glob, os, bisect
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
EXP = r"D:\FuturamaDecomp\analysis\retail_eu_export"
CODESEC = (".text", "D3D", "DSOUND", "XMV", "XPP")
THUNK = 0x293560
CS = Cs(CS_ARCH_X86, CS_MODE_32)

TH = []
while X.u32(THUNK + 4 * len(TH)):
    TH.append(X.u32(THUNK + 4 * len(TH)) & 0x7FFFFFFF)

KNAME = {}
_FUNCS = []
for line in open(EXP + r"\functions.tsv", encoding="utf-8"):
    p = line.rstrip("\n").split("\t")
    if p[0] == "address":
        continue
    _FUNCS.append((int(p[0], 16), int(p[1]), p[3]))
_FUNCS.sort()
_STARTS = [f[0] for f in _FUNCS]
FNAME = {a: n for a, s, n in _FUNCS}

CALLG = []
for line in open(EXP + r"\callgraph.tsv", encoding="utf-8"):
    p = line.rstrip("\n").split("\t")
    if p[0] == "caller":
        continue
    CALLG.append(p)
    if p[2].startswith("EXTERNAL:"):
        KNAME[int(p[2][9:], 16) - 1] = p[3].split("::")[-1]


def slot_of_ord(o):
    return TH.index(o) if o in TH else None


def kslot(addr):
    if THUNK <= addr < THUNK + 4 * len(TH) and (addr - THUNK) % 4 == 0:
        s = (addr - THUNK) // 4
        return s
    return None


def func_of(va):
    i = bisect.bisect_right(_STARTS, va) - 1
    if i >= 0 and _FUNCS[i][0] <= va < _FUNCS[i][0] + _FUNCS[i][1]:
        return _FUNCS[i]
    return None


def fdesc(va):
    f = func_of(va)
    return "%s(0x%X)" % (f[2], f[0]) if f else "<keine Ghidra-Funktion>"


def annotate(ins):
    c = ""
    for m in re.finditer(r"0x([0-9a-f]+)", ins.op_str):
        v = int(m.group(1), 16)
        s = kslot(v)
        if s is not None:
            c += "  ; slot %d ord %d %s" % (s, TH[s], KNAME.get(s, "?"))
        elif v in FNAME and ins.mnemonic in ("call", "jmp", "push", "mov"):
            c += "  ; " + FNAME[v]
    return c


def dis(va, n=0x80, stop_ret=False):
    out = []
    for ins in CS.disasm(X.read(va, n), va):
        out.append(ins)
        if stop_ret and ins.mnemonic in ("ret", "int3"):
            break
    return out


def pdis(va, n=0x80, stop_ret=False, file=None):
    for ins in dis(va, n, stop_ret):
        print("%08X: %-24s %s %s%s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, annotate(ins)), file=file)


_DEC = None


def decomp(addr):
    global _DEC
    if _DEC is None:
        _DEC = {}
        for fn in glob.glob(os.path.join(EXP, "decomp", "*.c")):
            data = open(fn, encoding="utf-8", errors="replace").read()
            hs = list(re.finditer(r"^// ==== FUNC ([0-9a-fA-F]+) (\S+) size=(\d+) ====$", data, re.M))
            for k, m in enumerate(hs):
                e = hs[k + 1].start() if k + 1 < len(hs) else len(data)
                _DEC[int(m.group(1), 16)] = data[m.start():e]
    return _DEC.get(addr)


def code_sections():
    for name, va, vsz, raw, rsz in X.sections:
        if name in CODESEC:
            yield name, va, X.b[raw:raw + rsz]


def rawrefs(target):
    res = []
    for name, va, d in code_sections():
        for i in range(len(d) - 4):
            if d[i] in (0xE8, 0xE9) and va + i + 5 + struct.unpack_from("<i", d, i + 1)[0] == target:
                res.append((va + i, "call" if d[i] == 0xE8 else "jmp"))
    return res


def absrefs(value, secs=None):
    pat = struct.pack("<I", value)
    out = []
    for name, va, vsz, raw, rsz in X.sections:
        if secs and name not in secs:
            continue
        d = X.b[raw:raw + rsz]
        p = d.find(pat)
        while p >= 0:
            out.append((name, va + p))
            p = d.find(pat, p + 1)
    return out


def kcalls(slot):
    a = THUNK + 4 * slot
    calls, other = [], []
    for name, va in absrefs(a, CODESEC):
        b = X.read(va - 2, 2)
        if b in (b"\xff\x15", b"\xff\x25"):
            calls.append(va - 2)
        else:
            other.append(va)
    return calls, other


def before(addr, n=8, back=0x40):
    """instructions directly before addr (tries all start offsets, picks the one that hits addr)"""
    code = X.read(addr - back, back)
    for st in range(back):
        ins = list(CS.disasm(code[st:], addr - back + st))
        if ins and ins[-1].address + ins[-1].size == addr:
            return ins[-n:]
    return []


def callers_cg(callee):
    t = "%08x" % callee
    return sorted({(p[0], p[1]) for p in CALLG if p[2] == t})
