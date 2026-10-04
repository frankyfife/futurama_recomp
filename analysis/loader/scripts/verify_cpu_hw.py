"""verify_cpu_hw.py - independent re-check of the hardware/MMIO claims (cpu.md key facts 12, 13).
Linear sweep of every Ghidra function body + the non-Ghidra regions; every immediate or absolute
displacement in Xbox hardware ranges; plus a dword scan of .rdata/.data.
"""
import sys, collections, struct
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *
from capstone import CS_OP_IMM, CS_OP_MEM
from capstone.x86 import X86_REG_INVALID

R = [(0xFD000000, 0xFDFFFFFF, "NV2A"), (0xFE800000, 0xFE8FFFFF, "APU"), (0xFEC00000, 0xFEC00FFF, "AC97"),
     (0xFED00000, 0xFED0FFFF, "OHCI"), (0xFEF00000, 0xFEF003FF, "NIC"), (0xFF000000, 0xFFFDFFFF, "FLASH"),
     (0xF0000000, 0xF7FFFFFF, "F0alias"), (0x80000000, 0x83FFFFFF, "phys80"), (0xFE000000, 0xFEFFFFFF, "otherFE")]


def cls(v):
    for lo, hi, n in R:
        if lo <= v <= hi:
            return n


def libof(a, nm):
    s = X.section(a)
    if s != ".text":
        return s
    if nm.startswith("Ni"):
        return "NetImmerse"
    if 0xf3de0 <= a < 0xf9cea or 0x1707a0 <= a < 0x170aba:
        return "XAPILIB"
    if 0x1668cc <= a < 0x170c01:
        return "CRT"
    if 0x17e010 <= a < 0x233880:
        return "initstub"
    return "game"


rows = []
ports = []
seen = set()


def scan(a, b, nm):
    for i in md.disasm(X.read(a, b - a), a):
        if i.address in seen:
            continue
        seen.add(i.address)
        if i.mnemonic in ("in", "out"):
            ports.append((i.address, i.mnemonic, i.op_str, nm))
        for op in i.operands:
            if op.type == CS_OP_IMM:
                v, kind, ab = op.imm & 0xFFFFFFFF, "imm", None
            elif op.type == CS_OP_MEM:
                v, kind = op.mem.disp & 0xFFFFFFFF, "disp"
                ab = op.mem.base == X86_REG_INVALID and op.mem.index == X86_REG_INVALID
                if not ab and op.mem.disp < 0 and op.mem.disp > -0x01000000:
                    continue
            else:
                continue
            c = cls(v)
            if c:
                rows.append((i.address, c, kind, ab, v, i.mnemonic, i.op_str, nm, libof(i.address, nm)))


for (s, sz, nm) in FUNCS:
    if X.section(s) in CODE and sz > 0:
        scan(s, s + sz, nm)
for a, b in [(0xf8469, 0xf84dd), (0xf99da, 0xf9a72), (0xf6380, 0xf63ad), (0x169352, 0x169467), (0x17086b, 0x1708b0)]:
    scan(a, b, "extra@%x" % a)

# MMIO accesses (absolute disp or imm in a pure MMIO range)
mm = [r for r in rows if r[1] in ("NV2A", "APU", "AC97", "OHCI", "NIC", "otherFE")]
print("MMIO-range operands by (lib, class, kind, absolute):")
for k, n in sorted(collections.Counter((r[8], r[1], r[2], r[3]) for r in mm).items()):
    print("   %-10s %-7s %-4s abs=%s  %d" % (k + (n,)))
apu_ds = [r for r in mm if r[8] == "DSOUND" and r[1] == "APU"]
print("DSOUND APU operands: %d in %d functions" % (len(apu_ds), len(set(r[7] for r in apu_ds))))
print("DSOUND APU operands that are memory accesses (disp): %d in %d functions" % (
    len([r for r in apu_ds if r[2] == "disp"]), len(set(r[7] for r in apu_ds if r[2] == "disp"))))
ac = [r for r in mm if r[1] == "AC97"]
print("AC97 operands: %d in %d functions" % (len(ac), len(set(r[7] for r in ac))))
print("non-DSOUND MMIO rows:")
for r in mm:
    if r[8] != "DSOUND":
        print("   %08x %-6s %-4s %-30s %s %s [%s]" % (r[0], r[1], r[2], r[5] + " " + r[6], r[7], r[8], hex(r[4])))
print("\nphys 0x80000000 window / F0 alias, non-library code (game/NetImmerse/initstub/CRT):")
for r in rows:
    if r[1] in ("phys80", "F0alias") and r[8] in ("game", "NetImmerse", "initstub", "CRT"):
        print("   %08x %-7s %-4s %-36s %s %s" % (r[0], r[1], r[2], r[5] + " " + r[6], r[7], r[8]))
print("\nabsolute memory accesses into phys80 window anywhere:")
for r in rows:
    if r[1] == "phys80" and r[2] == "disp" and r[3]:
        print("   %08x %-36s %s %s" % (r[0], r[5] + " " + r[6], r[7], r[8]))
print("\nin/out:", ports)

# 0x2bb5c755 anywhere in the image (int), and as float/double 733333333
img = X.b
for name, pat in [("int 0x2bb5c755", struct.pack("<I", 0x2bb5c755)), ("float", struct.pack("<f", 733333333.0)),
                  ("double", struct.pack("<d", 733333333.0)), ("int 733333 (kHz)", struct.pack("<I", 733333)),
                  ("int 733 (MHz)", struct.pack("<I", 733))]:
    offs = []
    p = img.find(pat)
    while p >= 0:
        offs.append(X.off2va(p))
        p = img.find(pat, p + 1)
    print("pattern %-18s found at VA:" % name, [hex(v) if v else None for v in offs][:12], "(count %d)" % len(offs))
print("0x2bb5c755 =", 0x2bb5c755)

# data sections: dwords in MMIO 0xFD000000..0xFEFFFFFF
for (n, lo, hi, raw) in sec_ranges((".rdata", ".data")):
    d = X.b[raw:raw + hi - lo]
    hits = []
    for o in range(0, len(d) - 3, 4):
        v = struct.unpack_from("<I", d, o)[0]
        if 0xFD000000 <= v <= 0xFEFFFFFF:
            hits.append((lo + o, v))
    print("data %s: dwords in 0xFD000000-0xFEFFFFFF (aligned): %d" % (n, len(hits)), [(hex(a), hex(v)) for a, v in hits[:10]])

# timing constants that would also encode a 733 MHz TSC (float/double, tolerance 1e-4 relative)
import math
targets = {"733.333e6": 733333333.0, "1/733.333e6": 1 / 733333333.0, "733.333 MHz": 733.3333333,
           "1/733.333 MHz": 1 / 733.3333333, "733333.333 kHz": 733333.3333, "1/733333.333": 1 / 733333.3333}
for (n, lo, hi, raw) in sec_ranges((".rdata", ".data", ".text")):
    d = X.b[raw:raw + hi - lo]
    for o in range(0, len(d) - 7):
        f = struct.unpack_from("<f", d, o)[0]
        g = struct.unpack_from("<d", d, o)[0]
        for k, t in targets.items():
            if f == f and f != 0 and abs(f - t) / t < 1e-4:
                print("float  %-16s at %08x (%s) = %r" % (k, lo + o, n, f))
            if g == g and g != 0 and abs(g - t) / t < 1e-6:
                print("double %-16s at %08x (%s) = %r" % (k, lo + o, n, g))
print("timing-constant scan done")
