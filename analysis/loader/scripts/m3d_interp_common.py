"""Shared helpers of the M3d interpolation analysis (analysis/loader/m3d_interpolation.md).

Read-only access to the retail EU XBE (capstone), the Ghidra export (decomp index of renderer_dfun.py),
functions.tsv / callgraph.tsv and retail_eu_ni_classes.json. No game data leaves run/."""
import json
import os
import re
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbe import Xbe  # noqa: E402

ROOT = r"D:\FuturamaDecomp"
XBE = Xbe(os.path.join(ROOT, r"orig\retail_eu\default.xbe"))
EXPORT = os.path.join(ROOT, r"analysis\retail_eu_export")
CLASSES = json.load(open(os.path.join(ROOT, r"analysis\retail_eu_ni_classes.json")))["classes"]

_names = None
_callgraph = None


def names():
    global _names
    if _names is None:
        _names = {}
        for line in open(os.path.join(EXPORT, "functions.tsv"), encoding="utf-8"):
            p = line.rstrip("\n").split("\t")
            if p[0] == "address":
                continue
            _names[int(p[0], 16)] = p[3]
    return _names


def callgraph():
    """List of (caller_entry, caller_name, callee_entry, callee_name)."""
    global _callgraph
    if _callgraph is None:
        _callgraph = []
        for line in open(os.path.join(EXPORT, "callgraph.tsv"), encoding="utf-8"):
            p = line.rstrip("\n").split("\t")
            if len(p) < 4 or not re.fullmatch(r"[0-9a-fA-F]{8}", p[0]) or not re.fullmatch(r"[0-9a-fA-F]{8}", p[2]):
                continue
            _callgraph.append((int(p[0], 16), p[1], int(p[2], 16), p[3]))
    return _callgraph


def callers(addr):
    return sorted({(c, n) for c, n, t, _ in callgraph() if t == addr})


def u32(va):
    return XBE.u32(va)


def f32(va):
    return struct.unpack("<f", XBE.read(va, 4))[0]


def vtable_slot(cls, slot):
    c = CLASSES[cls]
    return u32(c["vtable"] + 4 * slot)


def class_of_vtable():
    return {c["vtable"]: name for name, c in CLASSES.items()}


def decomp(addr):
    import renderer_dfun
    idx = renderer_dfun.load()
    e = idx.get(addr)
    return e[2] if e else None


def dis(va, n=0x100, stop_ret=False):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    out = []
    nm = names()
    for i in md.disasm(XBE.read(va, n), va):
        extra = ""
        if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x"):
            t = int(i.op_str, 16)
            if t in nm:
                extra = "   ; " + nm[t]
        out.append("%08x: %-24s %s %s%s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str, extra))
        if stop_ret and i.mnemonic.startswith("ret"):
            break
    return "\n".join(out)
