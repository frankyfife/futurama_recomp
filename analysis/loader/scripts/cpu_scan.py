"""cpu_scan.py - find code in retail_eu default.xbe that cannot run unchanged in a
32-bit Windows user-mode process.

  * every instruction with an fs: (and gs:) segment override
  * privileged / I/O / special instructions (in/out/cli/sti/hlt/lidt/cr/dr/msr/...),
    plus rdtsc/cpuid/int/pushfd/popfd for information
  * immediates and memory displacements pointing into Xbox hardware / physical
    memory / kernel address ranges

Method
  1. cpu_rd.RD: recursive descent from all 31337 Ghidra function entries + entry
     point + code pointers found in data/immediates (class "code").
  2. Linear sweep (capstone) over every byte range of the code sections .text/D3D/
     DSOUND/XMV/XPP that RD did not reach (class "gap-codelike" if the run decodes
     without invalid opcodes and contains a ret, else "gap-data").
  .rdata/.data are data; DOLBY is DSP56300 microcode (24-bit words) - both not
  disassembled as x86 (see cpu.md).

Outputs (D:\\FuturamaDecomp\\analysis\\loader\\):
  fs_sites.tsv        all fs:/gs: sites
  cpu_priv_sites.tsv  privileged / special instructions
  cpu_hw_refs.tsv     hardware-range immediates/displacements in code
usage: python cpu_scan.py [--all]   (--all also writes the "gap-data" pseudo-instructions,
                                    i.e. data bytes that merely decode as fs:/privileged opcodes)
"""
import sys, bisect, collections, os
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from cpu_rd import RD
from cpu_gaps import gaps, classify
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM
from capstone.x86 import X86_REG_FS, X86_REG_GS, X86_REG_INVALID

OUT = r"D:\FuturamaDecomp\analysis\loader"

rd = RD().run()
X = rd.x
funcs = rd.funcs
fstarts = [f[0] for f in funcs]


def owner(va):
    i = bisect.bisect_right(fstarts, va) - 1
    if i < 0:
        return ("-", 0, False)
    s, sz, nm = funcs[i]
    return (nm, s, va < s + max(sz, 1))


# address blocks inferred from the XbSymbolDatabase/FID names in functions.tsv:
#   XAPILIB 0xf3de0 (OutputDebugStringA) .. 0xf9cea (last kernel thunk stub),
#           0x1707a0 (XGetAudioFlags) .. 0x170aba (DbgBreakPoint stub + 6)
#   LIBCMT  0x1668cc (_rand) .. 0x1707a0 and 0x170aba (__strnicmp) .. 0x170c01
#   C++ initializer stubs 0x17e010 .. 0x233880 (XAPILIB::_cinit tables)
XAPI_BLOCKS = [(0xf3de0, 0xf9cea), (0x1707a0, 0x170aba)]
CRT_BLOCKS = [(0x1668cc, 0x1707a0), (0x170aba, 0x170c01)]


def lib_of(va, fname):
    sec = X.section(va)
    if sec in ("D3D", "DSOUND", "XMV", "XPP"):
        return sec
    if any(lo <= va < hi for lo, hi in XAPI_BLOCKS):
        return "XAPILIB"
    if any(lo <= va < hi for lo, hi in CRT_BLOCKS):
        return "LIBCMT"
    if 0x17e010 <= va < 0x233880:
        return "initstub"
    if fname.startswith("XAPILIB::") or fname.startswith("xboxkrnl"):
        return "XAPILIB"
    if fname.startswith("D3D8::"):
        return "D3D8"
    if fname.startswith("DSOUND::"):
        return "DSOUND"
    if fname.startswith("Ni"):
        return "NetImmerse"
    if not fname.startswith("FUN_") and fname != "-":
        return "CRT/named"
    return "game/unknown"


md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

PRIV = {
    "in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd",
    "cli", "sti", "hlt", "lidt", "lgdt", "lldt", "ltr", "sidt", "sgdt", "sldt", "str",
    "rdmsr", "wrmsr", "wbinvd", "invd", "invlpg", "sysenter", "sysexit", "syscall",
    "sysret", "iret", "iretd", "iretw", "clts", "lmsw", "smsw", "rdpmc", "rsm",
    "int", "into", "int1", "int3", "rdtsc", "rdtscp", "cpuid", "ud2", "lar", "lsl",
    "verr", "verw", "arpl", "xgetbv", "xsetbv", "pushfd", "popfd", "pushf", "popf",
    "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "sfence", "lfence", "mfence",
    "movntq", "movntps", "maskmovq", "fxsave", "fxrstor", "ldmxcsr", "stmxcsr", "emms", "femms",
}
SEGREGS = ("ss", "cs", "ds", "es", "fs", "gs")

HW_RANGES = [
    (0xFD000000, 0xFDFFFFFF, "NV2A GPU MMIO"),
    (0xFE800000, 0xFE8FFFFF, "APU MMIO"),
    (0xFEC00000, 0xFEC00FFF, "AC97 MMIO"),
    (0xFED00000, 0xFED0FFFF, "USB OHCI MMIO"),
    (0xFEF00000, 0xFEF003FF, "NIC MMIO"),
    (0xFF000000, 0xFFFEFFFF, "Flash/BIOS window"),
    (0xF0000000, 0xF7FFFFFF, "phys alias 0xF0000000 (WC/tiled)"),
    (0x80000000, 0x83FFFFFF, "phys window 0x80000000 / kernel"),
    (0x84000000, 0x8FFFFFFF, "kernel/system VA 0x84-0x8F"),
    (0xB0000000, 0xBFFFFFFF, "system VA 0xB0-0xBF"),
    (0xC0000000, 0xC0FFFFFF, "page tables 0xC0000000"),
    (0xD0000000, 0xDFFFFFFF, "system/instance mem 0xD0-0xDF"),
    (0xFE000000, 0xFE7FFFFF, "MMIO 0xFE0-0xFE7"),
    (0xFE900000, 0xFEBFFFFF, "MMIO 0xFE9-0xFEB"),
    (0xFEC01000, 0xFECFFFFF, "MMIO 0xFEC0-0xFECF"),
    (0xFED10000, 0xFEEFFFFF, "MMIO 0xFED1-0xFEEF"),
    (0xFEF00400, 0xFEFFFFFF, "MMIO 0xFEF0-0xFEFF"),
]
IO_PORTS = {0xCF8: "PCI CONFIG_ADDRESS", 0xCFC: "PCI CONFIG_DATA", 0xC000: "SMBus",
            0xC002: "SMBus", 0xC004: "SMBus", 0xC006: "SMBus", 0xC008: "SMBus",
            0x8000: "MCPX ACPI", 0x8008: "MCPX ACPI PM timer", 0x80C0: "MCPX ACPI-I/O +0xC0"}


def hw_class(v):
    v &= 0xFFFFFFFF
    for lo, hi, nm in HW_RANGES:
        if lo <= v <= hi:
            return nm
    return None


fs_rows, priv_rows, hw_rows = [], [], []


def analyse(ins, cls):
    mn = ins.mnemonic
    a = ins.address
    ops = ins.operands
    # ---------------- segment overrides
    seg = None
    for op in ops:
        if op.type == CS_OP_MEM and op.mem.segment in (X86_REG_FS, X86_REG_GS):
            seg = op
            break
    has_pre = (0x64 in ins.prefix) or (0x65 in ins.prefix)
    if seg is not None or has_pre:
        if seg is not None:
            sname = "fs" if seg.mem.segment == X86_REG_FS else "gs"
            disp = seg.mem.disp & 0xFFFFFFFF
            based = (seg.mem.base != X86_REG_INVALID) or (seg.mem.index != X86_REG_INVALID)
        else:  # prefix on an instruction without explicit memory operand
            sname = "fs" if 0x64 in ins.prefix else "gs"
            disp, based = 0, True
        fname, fstart, inside = owner(a)
        fs_rows.append(dict(addr=a, bytes=ins.bytes.hex(), ins=f"{mn} {ins.op_str}".strip(),
                            seg=sname, offset=disp, based=based, size=ins.size, func=fname,
                            fstart=fstart, inside=inside, cls=cls, section=X.section(a),
                            lib=lib_of(a, fname), memop=seg is not None))
    # ---------------- privileged / special
    is_priv = mn in PRIV or mn.startswith("rep ins") or mn.startswith("rep outs")
    first = ins.op_str.split(",")[0].strip()
    last = ins.op_str.split(",")[-1].strip()
    if mn == "mov" and any(r in ins.op_str.replace(" ", "").split(",")
                           for r in ("cr0", "cr2", "cr3", "cr4", "dr0", "dr1", "dr2", "dr3", "dr6", "dr7")):
        is_priv = True
    if mn in ("mov", "push", "pop") and (first in SEGREGS or (mn == "mov" and last in SEGREGS)):
        is_priv = True
    if mn in ("lds", "les", "lfs", "lgs", "lss", "ljmp", "lcall"):
        is_priv = True
    if is_priv:
        fname, fstart, inside = owner(a)
        priv_rows.append(dict(addr=a, bytes=ins.bytes.hex(), ins=f"{mn} {ins.op_str}".strip(),
                              func=fname, inside=inside, cls=cls, section=X.section(a),
                              lib=lib_of(a, fname)))
    # ---------------- I/O ports
    if mn in ("in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd"):
        port = None
        for op in ops:
            if op.type == CS_OP_IMM:
                port = op.imm & 0xFFFF
        if port is None and cls == "code":      # port in dx: look back for mov dx/edx, imm
            p = a
            for _k in range(6):
                q = prev_insn.get(p)
                if q is None:
                    break
                qsz, qmn, qops, qbb = rd.insn[q]
                if qmn == "mov" and (qops.startswith("dx, 0x") or qops.startswith("edx, 0x")):
                    port = int(qops.split(",")[1], 16) & 0xFFFF
                    break
                p = q
        fname, fstart, inside = owner(a)
        hw_rows.append(dict(addr=a, bytes=ins.bytes.hex(), ins=f"{mn} {ins.op_str}".strip(),
                            kind="port", value=port if port is not None else 0xFFFFFFFF,
                            cls="ioport " + IO_PORTS.get(port, "?") if port is not None else "ioport (dx unknown)",
                            rel="ioport", func=fname, inside=inside, code=cls,
                            section=X.section(a), lib=lib_of(a, fname)))
    # ---------------- hardware addresses
    for op in ops:
        absmem = False
        if op.type == CS_OP_IMM:
            kind, v = "imm", op.imm & 0xFFFFFFFF
        elif op.type == CS_OP_MEM:
            kind, v = "disp", op.mem.disp & 0xFFFFFFFF
            absmem = op.mem.base == X86_REG_INVALID and op.mem.index == X86_REG_INVALID
        else:
            continue
        c = hw_class(v)
        if not c:
            continue
        if kind == "disp" and not absmem and -0x01000000 <= op.mem.disp < 0:
            continue    # small negative displacement off a base register, not an address
        if kind == "imm" and v in (0x80070000,):
            continue    # HRESULT_FROM_WIN32 facility mask
        if kind == "imm" and v == 0xFD050F80:
            continue    # -50000000 = 5 s relative DueTime for KeSetTimer (XPP 0x28d295/0x28f41f)
        mmio = c.split()[0] in ("NV2A", "APU", "AC97", "USB", "NIC")
        if kind == "disp" and (mmio or absmem or v >= 0xF0000000 or (0x80000000 <= v < 0x84000000 and abs(op.mem.disp) > 0x10000000)):
            rel = "mem-access"
        elif kind == "imm" and mmio and not (mn in ("and", "test", "cmp", "add", "sub") ):
            rel = "mmio-imm"
        elif kind == "imm" and mn == "or" and v in (0x80000000, 0xF0000000):
            rel = "phys-or"
        elif kind == "imm" and 0x80010000 <= v < 0x80100000 and (v & 0xFFFF) not in (0x000E, 0x0057, 0x4005):
            rel = "kernel-imm"
        else:
            rel = "const"
        fname, fstart, inside = owner(a)
        hw_rows.append(dict(addr=a, bytes=ins.bytes.hex(), ins=f"{mn} {ins.op_str}".strip(),
                            kind=kind, value=v, cls=c, rel=rel, func=fname, inside=inside, code=cls,
                            section=X.section(a), lib=lib_of(a, fname)))


# --- 1. recursive-descent code
_sorted = sorted(rd.insn)
prev_insn = {}
for _i in range(1, len(_sorted)):
    if _sorted[_i - 1] + rd.insn[_sorted[_i - 1]][0] == _sorted[_i]:
        prev_insn[_sorted[_i]] = _sorted[_i - 1]
for va in _sorted:
    b = rd.bytes_at(va, rd.insn[va][0])
    for ins in md.disasm(b, va, 1):
        analyse(ins, "code")

# --- 2. linear sweep of the gaps
gap_stats = collections.Counter()
for a, b, n in gaps(rd):
    c = classify(rd, a, b)
    if c == "pad" or c == "ascii":
        gap_stats[(n, c)] += b - a
        continue
    cls = "gap-codelike" if c == "codelike" else "gap-data"
    gap_stats[(n, cls)] += b - a
    d = X.read(a, b - a)
    p = 0
    while p < len(d):
        ins = list(md.disasm(d[p:p + 16], a + p, 1))
        if not ins:
            p += 1
            continue
        analyse(ins[0], cls)
        p += ins[0].size


def w(path, rows, cols):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\t".join(cols) + "\n")
        for r in rows:
            out = []
            for c in cols:
                v = r[c]
                if c in ("addr", "value", "fstart"):
                    v = "%08x" % v
                elif c == "offset":
                    v = "0x%x" % v
                out.append(str(v))
            f.write("\t".join(out) + "\n")


ALL = "--all" in sys.argv
for r in fs_rows + priv_rows:
    if not r["inside"]:
        r["func"] = "?(after %s)" % r["func"]
fs_out = [r for r in fs_rows if ALL or r["cls"] != "gap-data"]
priv_out = [r for r in priv_rows if ALL or r["cls"] != "gap-data"]
fs_rows.sort(key=lambda r: r["addr"])
fs_out.sort(key=lambda r: r["addr"])
priv_rows.sort(key=lambda r: r["addr"])
hw_rows.sort(key=lambda r: r["addr"])
w(os.path.join(OUT, "fs_sites.tsv"), fs_out,
  ["addr", "bytes", "ins", "offset", "func", "section", "seg", "based", "memop", "size",
   "fstart", "inside", "cls", "lib"])
w(os.path.join(OUT, "cpu_priv_sites.tsv"), priv_out,
  ["addr", "bytes", "ins", "func", "section", "inside", "cls", "lib"])
hw_keep = [r for r in hw_rows if r["code"] != "gap-data" and r["rel"] != "const"]
w(os.path.join(OUT, "cpu_hw_refs.tsv"), hw_keep,
  ["addr", "bytes", "ins", "kind", "value", "cls", "rel", "func", "section", "inside", "code", "lib"])

print("RD instructions:", len(rd.insn))
print("gap bytes:", dict(gap_stats))
for cls in ("code", "gap-codelike", "gap-data"):
    rows = [r for r in fs_rows if r["cls"] == cls]
    print("fs/gs sites [%s]: %d" % (cls, len(rows)))
    c = collections.Counter((r["seg"], r["offset"], r["based"]) for r in rows)
    for k, n in sorted(c.items()):
        print("   %s:[0x%x] based=%s  %d" % (k[0], k[1], k[2], n))
for cls in ("code", "gap-codelike", "gap-data"):
    print("priv [%s]:" % cls, dict(collections.Counter(r["ins"].split()[0] for r in priv_rows if r["cls"] == cls)))
for cls in ("code", "gap-codelike", "gap-data"):
    print("hw [%s]:" % cls, dict(collections.Counter((r["rel"], r["cls"]) for r in hw_rows if r["code"] == cls)))
print("hw kept (non-const, not gap-data) by rel/lib:")
for k, n in sorted(collections.Counter((r["rel"], r["cls"], r["lib"]) for r in hw_keep).items()):
    print("   %-12s %-36s %-14s %d" % (k[0], k[1], k[2], n))
