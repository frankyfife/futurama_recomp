"""Disassemble a VA range of retail_eu default.xbe with capstone, annotating string/xref targets.
usage: python files_disasm.py <start_hex> <end_hex>"""
import sys, re
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
names = {}
for line in open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8"):
    p = line.rstrip("\n").split("\t")
    if p[0] != "address":
        names[int(p[0], 16)] = p[3]
md = Cs(CS_ARCH_X86, CS_MODE_32)
s, e = int(sys.argv[1], 16), int(sys.argv[2], 16)
for i in md.disasm(X.read(s, e - s), s):
    ann = []
    for m in re.findall(r"0x[0-9a-f]+", i.op_str):
        v = int(m, 16)
        if v in names:
            ann.append(names[v])
        elif X.section(v) in (".rdata", ".data") and X.va2off(v) is not None:
            c = X.cstr(v)
            if c and len(c) >= 3 and all(32 <= ord(ch) < 127 for ch in c):
                ann.append(repr(c))
    if i.mnemonic == "call" and "dword ptr [0x2935" in i.op_str:
        ann.append("kernel thunk")
    print(f"{i.address:08x} {i.bytes.hex():24s} {i.mnemonic:6s} {i.op_str:40s} {'; ' + ' | '.join(ann) if ann else ''}")
