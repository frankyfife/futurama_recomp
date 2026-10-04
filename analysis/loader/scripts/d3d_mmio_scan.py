"""d3d_mmio_scan.py - look for direct NV2A MMIO (0xFD000000-0xFDFFFFFF) / AGP aperture
accesses and kernel Av*/Mm*Contiguous imports outside the D3D section.
Usage: python d3d_mmio_scan.py"""
import csv, os, sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
sec = {n: (va, vsz, raw, rsz) for n, va, vsz, raw, rsz in x.sections}
funcs = sorted((int(r["address"], 16), r["block"], r["name"]) for r in
               csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"), delimiter="\t"))
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
hits = 0
for i, (a, blk, nm) in enumerate(funcs):
    if blk in ("D3D",) or blk not in sec or blk in (".rdata", ".data", "DOLBY"):
        continue
    va, vsz, raw, rsz = sec[blk]
    end = min(funcs[i + 1][0] if i + 1 < len(funcs) else va + rsz, va + rsz)
    if end <= a: continue
    for ins in md.disasm(x.read(a, end - a), a):
        for op in ins.operands:
            v = (op.imm if op.type == CS_OP_IMM else op.mem.disp if op.type == CS_OP_MEM else 0) & 0xffffffff
            if 0xfd000000 <= v < 0xfe000000:
                hits += 1
                print("%08x %-24s %08x %s %s" % (a, nm, ins.address, ins.mnemonic, ins.op_str))
print("NV2A MMIO hits outside D3D:", hits)
