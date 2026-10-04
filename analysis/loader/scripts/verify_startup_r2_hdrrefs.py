"""Review 2: code that reads the XBE image header directly (absolute operands 0x10000..0x10968), and
D3D callback/VBlank APIs used outside D3D; DisplayFrame (0x146460) callers in the Ghidra callgraph."""
from verify_startup_r2_common import *
from verify_startup_r2_common import _FUNCS
import re, collections
hdr_end = 0x10000 + 0x968
refs = collections.defaultdict(set)
for a, size, name in _FUNCS:
    sec = X.section(a)
    if sec not in CODESEC:
        continue
    for ins in CS.disasm(X.read(a, size), a):
        for m in re.finditer(r"0x([0-9a-f]+)", ins.op_str):
            v = int(m.group(1), 16)
            if 0x10000 <= v < hdr_end and ("[" in ins.op_str or ins.mnemonic in ("push", "mov")):
                refs[v].add("%s(0x%X)" % (name, a))
# non-function stubs we know about
for st, ln in ((0xF8469, 0x74), (0xF99DA, 0x98)):
    for ins in CS.disasm(X.read(st, ln), st):
        for m in re.finditer(r"0x([0-9a-f]+)", ins.op_str):
            v = int(m.group(1), 16)
            if 0x10000 <= v < hdr_end:
                refs[v].add("stub 0x%X" % st)
for v in sorted(refs):
    print("0x%X (hdr+0x%X): %s" % (v, v - 0x10000, sorted(refs[v])))
print()
for a, s, n in _FUNCS:
    if re.search(r"VerticalBlank|SwapCallback|VBlank|SetVerticalBlankCallback", n):
        out = [(hex(c), fdesc(c)) for c, k in rawrefs(a)]
        print(hex(a), n, "callers:", out)
print("\nDisplayFrame 0x146460 callers in callgraph:", callers_cg(0x146460))
print("ClearBuffer 0x146490 callers in callgraph:", callers_cg(0x146490))
print("abs refs to 0x146460:", [(n, hex(v)) for n, v in absrefs(0x146460)])
