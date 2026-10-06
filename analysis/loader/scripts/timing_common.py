"""Shared helpers for the timing analysis (analysis/loader/timing60.md).

Independent of the startup_*/verify_* helpers. Read-only on the XBE images and the Ghidra export.

R / P        : Xbe objects for retail EU (orig/retail_eu/default.xbe) and the NTSC prototype (orig/proto/default.xbe)
FUNCS        : Ghidra functions of retail EU (start, size, name) from functions.tsv
func_of(va)  : Ghidra function containing va (or None)
decomp(va)   : decompiled text of a Ghidra function start
insns(x, a, n): capstone disassembly of n bytes at a
index()      : linear capstone disassembly of every retail Ghidra function, cached in out/timing_index.pkl.
               Each entry: (address, size, mnemonic, op_str, bytes, function start). Gives exact instruction
               boundaries for every absolute operand, unlike raw byte scans.
mem_refs(t)  : instructions of the index whose operand text contains the absolute address t
absrefs(x, v): raw little-endian dword hits of v in all sections of image x (catches data tables and gaps)
f32(v)/u32f  : float <-> dword conversions
"""
import bisect
import glob
import os
import pickle
import re
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe  # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402

ROOT = r"D:\FuturamaDecomp"
EXP = os.path.join(ROOT, r"analysis\retail_eu_export")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
R = Xbe(os.path.join(ROOT, r"orig\retail_eu\default.xbe"))
P = Xbe(os.path.join(ROOT, r"orig\proto\default.xbe"))
CODESEC = (".text", "D3D", "DSOUND", "XMV", "XPP")
CS = Cs(CS_ARCH_X86, CS_MODE_32)

# Named guest addresses of the timing block written by FUN_000a26a0 (CApp::InitGame, startup.md 3.4)
G_FPS = 0x2D6744          # float fps (50.0 / 60.0)
G_DT = 0x2D6748           # float dt (0.02 / 1/60)
G_FIXED = 0x2D674C        # byte fixed-step flag (1)
G_DT_MIN = 0x2E7A58       # float clamp min for the (dead) variable step
G_DT_MAX = 0x2E7A5C       # float clamp max for the (dead) variable step
G_FPS2 = 0x2E7A60         # float fps copy
G_B68 = 0x2E7B68          # float 1/25.4 (PAL) / 1/30.5 (NTSC)
G_FPS3 = 0x2E7B6C         # float fps copy
G_TAG = 0x2E7B70          # char[8] "PAL" / "NTSC"
G_GAME = 0x2E7B84         # CApp* g_pGame
G_FRAME_DELTA = 0x2E7B88  # float real duration of the previous rendered frame (Game::Frame 0xA2C20)


def f32(v):
    return struct.unpack("<f", struct.pack("<I", v & 0xFFFFFFFF))[0]


def u32f(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


def f64(q):
    return struct.unpack("<d", struct.pack("<Q", q))[0]


FUNCS = []
for _line in open(os.path.join(EXP, "functions.tsv"), encoding="utf-8"):
    _p = _line.rstrip("\n").split("\t")
    if _p[0] == "address":
        continue
    FUNCS.append((int(_p[0], 16), int(_p[1]), _p[3]))
FUNCS.sort()
_STARTS = [f[0] for f in FUNCS]
FNAME = {a: n for a, s, n in FUNCS}

CALLG = []
for _line in open(os.path.join(EXP, "callgraph.tsv"), encoding="utf-8"):
    _p = _line.rstrip("\n").split("\t")
    if _p[0] != "caller":
        CALLG.append(_p)


def func_of(va):
    i = bisect.bisect_right(_STARTS, va) - 1
    if i >= 0 and FUNCS[i][0] <= va < FUNCS[i][0] + FUNCS[i][1]:
        return FUNCS[i]
    return None


def fdesc(va):
    f = func_of(va)
    return "%s" % f[2] if f else "<keine Ghidra-Funktion>"


def callers(callee):
    t = "%08x" % callee
    return sorted({int(p[0], 16) for p in CALLG if p[2] == t})


def callees(caller):
    t = "%08x" % caller
    return sorted({int(p[2], 16) for p in CALLG if p[0] == t and not p[2].startswith("EXTERNAL")})


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


def insns(x, a, n):
    return list(CS.disasm(x.read(a, n), a))


def pdis(x, a, n, file=None, mark=()):
    for i in insns(x, a, n):
        print("%s%08X: %-24s %s %s" % ("*" if i.address in mark else " ", i.address, i.bytes.hex(),
                                       i.mnemonic, i.op_str), file=file)


def _decode_block(start, size, owner, idx):
    data = R.read(start, size)
    if data is None:
        return
    off = 0
    while off < size:
        got = False
        for i in CS.disasm(data[off:], start + off):
            idx.append((i.address, i.size, i.mnemonic, i.op_str, bytes(i.bytes), owner))
            off = i.address + i.size - start
            got = True
        if not got:
            off += 1


def _gap_segments():
    """Code between Ghidra functions in .text (vtable-only functions such as the context Update
    methods 0x22B30/0x2B5F0 are not Ghidra functions). Split at nop/int3 padding; each segment gets a
    pseudo owner = its first address (GAP_STARTS)."""
    segs = []
    text = [s for s in R.sections if s[0] == ".text"][0]
    tend = text[1] + text[4]
    prev_end = text[1]
    bounds = [(a, a + s) for a, s, n in FUNCS if R.section(a) == ".text"] + [(tend, tend)]
    for a, e in bounds:
        if a > prev_end:
            d = R.read(prev_end, a - prev_end)
            p = 0
            while p < len(d):
                while p < len(d) and d[p] in (0x90, 0xCC, 0x00):
                    p += 1
                q = p
                # segment ends at the next run of >= 2 padding bytes
                while q < len(d) and not (d[q] in (0x90, 0xCC) and q + 1 < len(d) and d[q + 1] == d[q]):
                    q += 1
                if q > p:
                    segs.append((prev_end + p, q - p))
                p = q
        prev_end = max(prev_end, e)
    return segs


GAP_STARTS = set()


def _build_index():
    idx = []
    for start, size, name in FUNCS:
        if R.section(start) not in CODESEC or size <= 0:
            continue
        _decode_block(start, size, start, idx)
    for start, size in _gap_segments():
        _decode_block(start, size, -start, idx)   # negative owner marks gap code
    return idx


_IDX = None


def index():
    global _IDX
    if _IDX is None:
        os.makedirs(OUT, exist_ok=True)
        p = os.path.join(OUT, "timing_index_v2.pkl")
        if os.path.exists(p) and os.path.getmtime(p) > os.path.getmtime(os.path.join(EXP, "functions.tsv")):
            _IDX = pickle.load(open(p, "rb"))
        else:
            _IDX = _build_index()
            pickle.dump(_IDX, open(p, "wb"))
    return _IDX


_ADDR_INDEX = None


def by_operand_addr():
    """dict absolute value (>= 0x10000) appearing in an operand -> list of index entries"""
    global _ADDR_INDEX
    if _ADDR_INDEX is None:
        _ADDR_INDEX = {}
        for e in index():
            for m in re.finditer(r"0x([0-9a-f]{5,8})", e[3]):
                _ADDR_INDEX.setdefault(int(m.group(1), 16), []).append(e)
    return _ADDR_INDEX


def mem_refs(t):
    return by_operand_addr().get(t, [])


def absrefs(x, value, secs=None):
    pat = struct.pack("<I", value)
    out = []
    for name, va, vsz, raw, rsz in x.sections:
        if secs and name not in secs:
            continue
        d = x.b[raw:raw + rsz]
        p = d.find(pat)
        while p >= 0:
            out.append((name, va + p))
            p = d.find(pat, p + 1)
    return out


def owner_name(o):
    return FNAME.get(o, "?") if o >= 0 else "gap_%06X" % -o


def fmt(e):
    return "%08X  %-22s %-7s %-40s %s" % (e[0], e[4].hex(), e[2], e[3], owner_name(e[5]))


def before(x, addr, n=8, back=0x40):
    """instructions directly before addr (tries all start offsets, picks the one that ends at addr)"""
    code = x.read(addr - back, back)
    for st in range(back):
        ins = list(CS.disasm(code[st:], addr - back + st))
        if ins and ins[-1].address + ins[-1].size == addr:
            return ins[-n:]
    return []
