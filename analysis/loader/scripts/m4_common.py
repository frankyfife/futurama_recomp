"""Shared helpers of the M4 input analysis (analysis/loader/m4_input.md).

Read-only access to the retail EU XBE (capstone), the Ghidra export (functions.tsv, callgraph.tsv,
decomp/*.c) and strings. Nothing here writes outside analysis/loader/scripts/*.out."""
import bisect
import csv
import os
import re
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe  # noqa: E402
import capstone  # noqa: E402

ROOT = r"D:\FuturamaDecomp"
XBE = Xbe(os.path.join(ROOT, r"orig\retail_eu\default.xbe"))
EXPORT = os.path.join(ROOT, r"analysis\retail_eu_export")
OUTDIR = os.path.dirname(os.path.abspath(__file__))
MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = False

CODE_SECTIONS = (".text", "D3D", "DSOUND", "XMV", "XPP")

_funcs = None
_starts = None


def funcs():
    """{start: (name, size)} from functions.tsv."""
    global _funcs, _starts
    if _funcs is None:
        _funcs = {}
        with open(os.path.join(EXPORT, "functions.tsv"), encoding="utf-8") as f:
            r = csv.reader(f, delimiter="\t")
            next(r)
            for row in r:
                _funcs[int(row[0], 16)] = (row[3], int(row[1]))
        _starts = sorted(_funcs)
    return _funcs


def owner(a):
    """Ghidra function containing a, else None."""
    fs = funcs()
    i = bisect.bisect_right(_starts, a) - 1
    if i < 0:
        return None
    s = _starts[i]
    return s if a < s + max(fs[s][1], 1) else None


def guess_start(a):
    """Start of the code block containing a when Ghidra has no function there: the nearest
    16-byte aligned address at or below a that follows padding (90/CC) or a ret (C3/C2 imm16)
    and from which a linear disassembly reaches a. Heuristic (vtable-only methods)."""
    st = a & ~0xF
    while st > a - 0x4000:
        b1 = XBE.read(st - 1, 1)[0]
        b3 = XBE.read(st - 3, 1)[0]
        if b1 in (0x90, 0xCC, 0xC3) or b3 == 0xC2:
            reach = any(ins.address == a for ins in MD.disasm(XBE.read(st, a - st + 16), st))
            if reach:
                return st
        st -= 0x10
    return a


def owner_name(a):
    s = owner(a)
    if s is None:
        return "nofunc_%08x" % guess_start(a)
    return "%s" % funcs()[s][0]


def owner_key(a):
    s = owner(a)
    return s if s is not None else guess_start(a)


def u8(va):
    return XBE.read(va, 1)[0]


def u16(va):
    return struct.unpack("<H", XBE.read(va, 2))[0]


def u32(va):
    return XBE.u32(va)


def f32(va):
    return struct.unpack("<f", XBE.read(va, 4))[0]


def cstr(va):
    return XBE.cstr(va)


def code_ranges():
    for name, va, vsz, raw, rsz in XBE.sections:
        if name in CODE_SECTIONS:
            yield name, va, raw, rsz


def boundary_ok(a):
    """True if a linear disassembly from some point up to 64 bytes before reaches a exactly."""
    for back in range(1, 64):
        st = a - back
        for ins in MD.disasm(XBE.read(st, back + 5), st):
            if ins.address == a:
                return True
            if ins.address > a:
                break
    return False


def raw_calls(targets):
    """All validated E8 rel32 call sites into the given target set: {target: [site, ...]}."""
    targets = set(targets)
    hits = {t: [] for t in targets}
    for name, va, raw, rsz in code_ranges():
        data = XBE.b[raw:raw + rsz]
        for i in range(len(data) - 5):
            if data[i] != 0xE8:
                continue
            site = va + i
            tgt = (site + 5 + struct.unpack_from("<i", data, i + 1)[0]) & 0xFFFFFFFF
            if tgt in targets and boundary_ok(site):
                hits[tgt].append(site)
    return hits


def insns_before(a, n=6):
    """Up to n instructions that end exactly at a (best-effort backward sync)."""
    best = []
    for back in range(48, 0, -1):
        st = a - back
        seq = []
        ok = False
        for ins in MD.disasm(XBE.read(st, back), st):
            seq.append(ins)
            if ins.address + ins.size == a:
                ok = True
                break
        if ok and len(seq) >= len(best):
            best = seq
            if len(seq) >= n:
                break
    return best[-n:]


def dis(va, n=0x100, stop_ret=False):
    out = []
    fs = funcs()
    for i in MD.disasm(XBE.read(va, n), va):
        extra = ""
        if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x"):
            t = int(i.op_str, 16)
            if t in fs:
                extra = "   ; " + fs[t][0]
        out.append("%08x: %-24s %s %s%s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str, extra))
        if stop_ret and i.mnemonic.startswith("ret"):
            break
    return "\n".join(out)


def decomp_index():
    """{start: (name, text)} over all decomp/*.c files."""
    idx = {}
    d = os.path.join(EXPORT, "decomp")
    hdr = re.compile(r"^// ==== FUNC ([0-9a-f]{8}) (\S+) size=(\d+) ====$")
    for fn in sorted(os.listdir(d)):
        cur = None
        buf = []
        for line in open(os.path.join(d, fn), encoding="utf-8", errors="replace"):
            m = hdr.match(line.rstrip("\n"))
            if m:
                if cur is not None:
                    idx[cur[0]] = (cur[1], "".join(buf))
                cur = (int(m.group(1), 16), m.group(2))
                buf = []
            else:
                buf.append(line)
        if cur is not None:
            idx[cur[0]] = (cur[1], "".join(buf))
    return idx


def write_out(name, text):
    p = os.path.join(OUTDIR, name)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return p
