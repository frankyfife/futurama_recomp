"""Scan all Ghidra functions for memory operands with absolute displacement outside the XBE image
(< 0x10000 or >= 0x31a300), i.e. hard-coded hardware/kernel addresses (NV2A 0xFD000000, APU 0xFE800000,
USB 0xFED00000, contiguous 0x80000000+...). Prints per section/function."""
import sys, collections, re
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
res = collections.defaultdict(list)
for l in open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8"):
    p = l.rstrip("\n").split("\t")
    if p[0] == "address": continue
    a, n, blk, name = int(p[0], 16), int(p[1]), p[2], p[3]
    code = X.read(a, n)
    if not code: continue
    for i in md.disasm(code, a):
        if i.prefix[1] in (0x64,): continue  # fs:
        for op in i.operands:
            if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                d = op.mem.disp & 0xffffffff
                if d < 0x10000 or d >= 0x31a300:
                    res[(blk, name)].append("%x:%s %s" % (i.address, i.mnemonic, i.op_str))
for k in sorted(res):
    print(k[0], k[1], len(res[k]), res[k][:3])
