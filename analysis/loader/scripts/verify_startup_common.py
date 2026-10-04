"""Independent helpers for the verification of startup.md (reviewer scripts, not shared with startup_*.py).
Provides: X (Xbe), dis(va, n, stop_ret), kslot(addr)->(slot, ordinal), func_of(va), decomp(addr), raw_calls(target),
abs_refs(value), thunk table listing."""
import sys, struct, re, glob, os, bisect
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
E = r"D:\FuturamaDecomp\analysis\retail_eu_export"
CODE = (".text", "D3D", "DSOUND", "XMV", "XPP", "DOLBY")
THUNK = 0x293560
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = False

def thunks():
    out = []; i = 0
    while X.u32(THUNK + 4 * i):
        out.append(X.u32(THUNK + 4 * i) & 0x7fffffff); i += 1
    return out
TH = thunks()

def kslot(a):
    if THUNK <= a < THUNK + 4 * len(TH):
        s = (a - THUNK) // 4
        return s, TH[s]
    return None

_funcs = None
def funcs():
    global _funcs
    if _funcs is None:
        _funcs = []
        for l in open(E + r"\functions.tsv", encoding="utf-8"):
            p = l.rstrip("\n").split("\t")
            if p[0] == "address": continue
            _funcs.append((int(p[0], 16), int(p[1]), p[3]))
        _funcs.sort()
    return _funcs
def fname(a):
    for s, n, nm in funcs():
        if s == a: return nm
    return None
_starts = None
def func_of(v):
    global _starts
    f = funcs()
    if _starts is None: _starts = [x[0] for x in f]
    i = bisect.bisect_right(_starts, v) - 1
    if i >= 0 and f[i][0] <= v < f[i][0] + f[i][1]: return f[i]
    return None

def dis(va, n=0x100, stop_ret=False):
    out = []
    for i in md.disasm(X.read(va, n), va):
        c = ""
        m = re.search(r"\[0x([0-9a-f]+)\]", i.op_str)
        if m:
            k = kslot(int(m.group(1), 16))
            if k: c = "  ; slot %d ord %d" % k
        if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x"):
            nm = fname(int(i.op_str, 16))
            if nm: c = "  ; " + nm
        out.append((i.address, i.bytes.hex(), i.mnemonic, i.op_str, c))
        if stop_ret and i.mnemonic in ("ret", "int3"): break
    return out
def pdis(va, n=0x100, stop_ret=False):
    for a, b, m, o, c in dis(va, n, stop_ret):
        print("%08x: %-22s %s %s%s" % (a, b, m, o, c))

_dec = None
def decomp(addr):
    global _dec
    if _dec is None:
        _dec = {}
        for f in glob.glob(os.path.join(E, "decomp", "*.c")):
            data = open(f, encoding="utf-8", errors="replace").read()
            heads = list(re.finditer(r"^// ==== FUNC ([0-9a-fA-F]+) (\S+) size=(\d+) ====$", data, re.M))
            for k, m in enumerate(heads):
                end = heads[k + 1].start() if k + 1 < len(heads) else len(data)
                _dec[int(m.group(1), 16)] = data[m.start():end]
    return _dec.get(addr)

def sec_bytes():
    for name, va, vsz, raw, rsz in X.sections:
        if name in CODE:
            yield name, va, X.b[raw:raw + rsz]

def raw_calls(target):
    """all E8/E9 rel32 whose destination == target, plus FF15/FF25/68(push imm)/B8-BF (mov reg,imm) absolute refs"""
    res = []
    for name, va, d in sec_bytes():
        for i in range(len(d) - 5):
            if d[i] in (0xE8, 0xE9):
                if va + i + 5 + struct.unpack_from("<i", d, i + 1)[0] == target:
                    res.append((va + i, "call" if d[i] == 0xE8 else "jmp"))
    return res

def abs_refs(value, sections=None):
    pat = struct.pack("<I", value); out = []
    for name, va, vsz, raw, rsz in X.sections:
        if sections and name not in sections: continue
        d = X.b[raw:raw + rsz]; p = d.find(pat)
        while p >= 0:
            out.append((name, va + p)); p = d.find(pat, p + 1)
    return out

def kcall_sites(slot):
    pat = [b"\xff\x15" + struct.pack("<I", THUNK + 4 * slot), b"\xff\x25" + struct.pack("<I", THUNK + 4 * slot)]
    res = []
    for name, va, d in sec_bytes():
        for p_ in pat:
            p = d.find(p_)
            while p >= 0:
                res.append(va + p); p = d.find(p_, p + 1)
    # also mov reg,[slot] / push [slot]
    other = []
    for name, va in abs_refs(THUNK + 4 * slot, CODE):
        if va - 2 not in res: other.append(va)
    return sorted(res), other

def back_dis(addr, back=0x40, fwd=8):
    code = X.read(addr - back, back + fwd)
    for start in range(back):
        ins = list(md.disasm(code[start:], addr - back + start))
        if any(i.address == addr for i in ins):
            return [i for i in ins if i.address < addr][-10:], [i for i in ins if i.address >= addr][:3]
    return [], []
