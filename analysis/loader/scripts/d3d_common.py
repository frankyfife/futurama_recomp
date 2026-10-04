"""d3d_common.py - shared helpers for the d3d_*.py scripts.

attribute(addr): returns (func_start, func_name) of the function that really contains
an instruction. functions.tsv misses some functions that are only reachable through
vtables (e.g. 0x1558e0). If addr lies behind the end of the preceding Ghidra function,
the gap is split at MSVC padding (0x90/0xCC after ret/jmp) and the block containing
addr is reported as 'sub_XXXXXXXX' (not defined in Ghidra).
"""
import bisect, csv, sys
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

XBE = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
FUNCS = sorted((int(r["address"], 16), int(r["size"]), r["block"], r["name"]) for r in
               csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"),
                              delimiter="\t"))
_STARTS = [f[0] for f in FUNCS]
_md = Cs(CS_ARCH_X86, CS_MODE_32)
_cache = {}

def _split_gap(start, end):
    """split [start,end) into blocks separated by padding after ret/jmp"""
    if (start, end) in _cache:
        return _cache[(start, end)]
    blocks = []
    data = XBE.read(start, end - start) or b""
    p = 0
    while p < len(data):
        while p < len(data) and data[p] in (0x90, 0xCC):
            p += 1
        if p >= len(data):
            break
        s = p
        q = p
        for ins in _md.disasm(data[p:], start + p):
            q = ins.address - start + ins.size
            if ins.mnemonic in ("ret", "jmp") and q < len(data) and data[q] in (0x90, 0xCC):
                break
        if q <= s:
            q = s + 1
        blocks.append((start + s, start + q))
        p = q
    _cache[(start, end)] = blocks
    return blocks

def attribute(addr):
    i = bisect.bisect_right(_STARTS, addr) - 1
    if i < 0:
        return None, "?"
    a, sz, blk, nm = FUNCS[i]
    if addr < a + sz:
        return a, nm
    nxt = FUNCS[i + 1][0] if i + 1 < len(FUNCS) else addr + 1
    for s, e in _split_gap(a + sz, nxt):
        if s <= addr < e:
            return s, "sub_%08x" % s
    return a, nm
