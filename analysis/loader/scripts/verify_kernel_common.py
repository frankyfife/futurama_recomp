"""Independent helpers for the reviewer's verification of kernel.md.

Deliberately does NOT import kernel_common.py / kernel_analyze.py (the analysed
agent's code); only tools/xbe.py and the Ghidra export TSVs are used.
"""
import bisect
import csv
import os
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_INVALID

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe  # noqa: E402

ROOT = r"D:\FuturamaDecomp"
XBE = Xbe(os.path.join(ROOT, r"orig\retail_eu\default.xbe"))
EXP = os.path.join(ROOT, r"analysis\retail_eu_export")
LOADER = os.path.join(ROOT, r"analysis\loader")

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

CODE_SECTIONS = (".text", "D3D", "DSOUND", "XMV", "XPP")


def u32(va):
    return XBE.u32(va)


def hdr_u32(off):
    return struct.unpack_from("<I", XBE.b, off)[0]


def insn(va):
    code = XBE.read(va, 16)
    if not code:
        return None
    return next(md.disasm(code, va), None)


def disasm_range(va, n):
    code = XBE.read(va, n)
    return list(md.disasm(code, va)) if code else []


def section_headers():
    """Return list of dicts with flags etc. straight from the XBE header."""
    b = XBE.b
    base = hdr_u32(0x104)
    nsect, sect = struct.unpack_from("<II", b, 0x11C)
    out = []
    for i in range(nsect):
        o = sect - base + i * 0x38
        fl, va, vsz, raw, rsz, na = struct.unpack_from("<6I", b, o)
        name = b[na - base:b.index(b"\0", na - base)].decode()
        refcnt_addr = struct.unpack_from("<I", b, o + 0x1C)[0]
        out.append(dict(name=name, flags=fl, va=va, vsz=vsz, raw=raw, rsz=rsz, hdr_va=base + (o), refcnt_addr=refcnt_addr))
    return out


def thunk_table():
    """Read thunk table address from header (XOR retail key) and decode until 0."""
    enc = hdr_u32(0x158)
    va = enc ^ 0x5B6D40B6
    ents = []
    i = 0
    while True:
        v = u32(va + 4 * i)
        if v == 0:
            break
        ents.append((i, va + 4 * i, v))
        i += 1
    return enc, va, ents


class Funcs:
    def __init__(self):
        self.rows = []
        with open(os.path.join(EXP, "functions.tsv"), newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f, delimiter="\t"):
                self.rows.append((int(r["address"], 16), int(r["size"]), r["block"], r["name"]))
        self.rows.sort()
        self.addrs = [r[0] for r in self.rows]
        self.by = {r[0]: r for r in self.rows}

    def containing(self, va):
        i = bisect.bisect_right(self.addrs, va) - 1
        if i < 0:
            return None
        r = self.rows[i]
        if r[0] <= va < r[0] + max(r[1], 1):
            return r
        return None

    def name_or(self, va):
        r = self.containing(va)
        return r[3] if r else "<no func>"


def callgraph():
    out = []
    with open(os.path.join(EXP, "callgraph.tsv"), newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            out.append(r)
    return out


def module_of(va):
    """Same module boundaries as the report (to reproduce its counts); see verify_kernel_modules.py
    for the check of the boundaries themselves."""
    s = XBE.section(va)
    if s != ".text":
        return s
    if 0xF3DE0 <= va < 0xFA5A0 or 0x1707A0 <= va < 0x170ABA:
        return "XAPI"
    if 0x1668CC <= va < 0x171000:
        return "LIBC"
    return "GAME"


def mem_disp_only(op):
    return op.type == X86_OP_MEM and op.mem.base == X86_REG_INVALID and op.mem.index == X86_REG_INVALID


def fmt(i):
    return "%06x %-22s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str)
