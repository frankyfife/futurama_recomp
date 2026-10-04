"""Evidence for the loader design (docs/loader_design.md, "Header-Polyglott").

1. Which XBE header bytes (VA 0x10000..0x10FFF) does code reference directly?
   Linear capstone sweep over all Ghidra function bodies plus a raw dword scan
   of all executable sections (catches code outside Ghidra functions).
2. Layout facts: SizeOfHeaders, signature range, where a PE e_lfanew (0x3C)
   would land, free space behind the XBE header in the first page.
3. PE header size of the toolchain probe launcher (what must be relocated).

Read-only. Usage: python design_header_refs.py
"""
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe  # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402
from capstone.x86 import X86_OP_IMM, X86_OP_MEM  # noqa: E402

XBE = r"D:\FuturamaDecomp\orig\retail_eu\default.xbe"
FUNCS = r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"
LAUNCHER = r"D:\FuturamaDecomp\analysis\loader\toolchain_probe\launcher.exe"
LO, HI = 0x10000, 0x11000

x = Xbe(XBE)
b = x.b
size_hdr = struct.unpack_from("<I", b, 0x108)[0]
cert = struct.unpack_from("<I", b, 0x118)[0]
nsect, secthdr = struct.unpack_from("<II", b, 0x11C)
print(f"SizeOfHeaders      = 0x{size_hdr:x}  (header VA 0x10000..0x{0x10000+size_hdr:x})")
print(f"Certificate        = 0x{cert:x}")
print(f"Sections           = {nsect} at 0x{secthdr:x}..0x{secthdr+nsect*0x38:x}")
print("Signature bytes    = 0x10004..0x10103 (RSA, 256 bytes)")
print(f"Byte at 0x1003C    = {b[0x3C:0x40].hex()} (inside signature -> usable as e_lfanew)")
print(f"Free in page 0x10000: 0x{0x10000+size_hdr:x}..0x10fff = 0x{0x1000-size_hdr:x} bytes")

# --- 1a. capstone sweep over Ghidra function bodies -------------------------
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
hits = defaultdict(list)  # header VA -> [(insn addr, text)]
for line in open(FUNCS, encoding="utf-8"):
    if line.startswith("address"):
        continue
    a, size, block = line.split("\t")[:3]
    a, size = int(a, 16), int(size)
    if size <= 0 or block in (".rdata", ".data"):
        continue
    code = x.read(a, size)
    if not code:
        continue
    for ins in md.disasm(code, a):
        for op in ins.operands:
            v = None
            kind = None
            if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                v, kind = op.mem.disp & 0xFFFFFFFF, "MEM"
            elif op.type == X86_OP_IMM:
                v, kind = op.imm & 0xFFFFFFFF, "IMM"
            if v is not None and LO <= v < HI:
                hits[(kind, v)].append((ins.address, f"{ins.mnemonic} {ins.op_str}"))

print("\nHeader references from code (Ghidra bodies, capstone; MEM = absolute memory operand):")
for kind, v in sorted(hits, key=lambda k: (k[0], k[1])):
    sites = hits[(kind, v)]
    ex = ", ".join(f"0x{s:x}" for s, _ in sites[:6])
    texts = sorted({t for _, t in sites})[:3]
    print(f"  {kind} 0x{v:05x}: {len(sites):3d}x  e.g. {ex}  {texts}")
mem = [v for k, v in hits if k == "MEM"]
print(f"  lowest MEM-referenced header VA: 0x{min(mem):x}" if mem else "  no MEM refs")

# --- 1b. raw dword scan of executable sections (incl. code outside Ghidra) --
raw = Counter()
possible_mem = []  # (insn va, text) where some decode start makes the dword an absolute MEM disp
for name, va, vsz, rawoff, rsz in x.sections:
    if name in (".rdata", ".data", "DOLBY", "$$XTIMAGE", ".XTLID"):
        continue
    data = b[rawoff:rawoff + rsz]
    for i in range(len(data) - 3):
        v = struct.unpack_from("<I", data, i)[0]
        if LO <= v < 0x10108:
            raw[v] += 1
            # superset check: any instruction starting 1..7 bytes before that
            # uses this dword as an absolute memory displacement?
            for back in range(1, 8):
                s = i - back
                if s < 0:
                    continue
                ins = next(md.disasm(data[s:s + 15], va + s), None)
                if ins is None or ins.size < back + 4:
                    continue
                for op in ins.operands:
                    if (op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0
                            and (op.mem.disp & 0xFFFFFFFF) == v):
                        possible_mem.append((ins.address, f"{ins.mnemonic} {ins.op_str}"))
print("\nSuperset scan (any decode start) for absolute MEM operands into 0x10000..0x10107:")
for a, t in possible_mem:
    print(f"  0x{a:x}: {t}")
if not possible_mem:
    print("  none")
print("\nRaw dwords in code sections with value in 0x10000..0x10107: "
      f"{sum(raw.values())} unaligned candidates (byte noise; only decoded MEM operands above count)")

# --- 3. PE header of the probe launcher ---------------------------------------
if os.path.exists(LAUNCHER):
    pe = open(LAUNCHER, "rb").read(0x1000)
    lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    nsec = struct.unpack_from("<H", pe, lfanew + 6)[0]
    soh = struct.unpack_from("<H", pe, lfanew + 20)[0]
    size_hdrs = struct.unpack_from("<I", pe, lfanew + 24 + 60)[0]
    nt_end = lfanew + 24 + soh + nsec * 40
    print(f"\nProbe launcher: e_lfanew=0x{lfanew:x}, sections={nsec}, SizeOfOptionalHeader=0x{soh:x}, "
          f"NT headers+section table = 0x{lfanew:x}..0x{nt_end:x} (0x{nt_end-lfanew:x} bytes), SizeOfHeaders=0x{size_hdrs:x}")
