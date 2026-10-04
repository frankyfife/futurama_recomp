"""_rtinit/_cinit tables: count entries, nulls, duplicates, how many initializers call NiStream::RegisterLoader
(0x1087D0) directly, and the first initializers. Also disassembles _cinit to get the table bounds from code."""
from verify_startup_common import *
pdis(0xf9783, 0x58, True)
print("---- _rtinit")
pdis(0xf97db, 0x30, True)
def table(a, b):
    return [X.u32(x) for x in range(a, b, 4)]
for nm, a, b in (("rtinit", 0x2bdee0, 0x2bdeec), ("C", 0x2d5b34, 0x2d5b48), ("C++", 0x2bdef0, 0x2d5b30)):
    t = table(a, b)
    nz = [x for x in t if x]
    print(nm, "entries", len(t), "nonzero", len(nz), "distinct", len(set(nz)), "first", [hex(x) for x in t[:4]], "last", [hex(x) for x in t[-3:]])
t = [x for x in table(0x2bdef0, 0x2d5b30) if x]
direct = 0; first_alloc = None
for k, f in enumerate(t):
    code = list(md.disasm(X.read(f, 0x40), f))
    hit = False
    for i in code:
        if i.mnemonic in ("call", "jmp") and i.op_str == "0x1087d0": hit = True
        if i.mnemonic in ("ret",) or (i.mnemonic == "jmp" and not i.op_str.startswith("0x1087d0")): break
    direct += hit
print("C++ initializers calling RegisterLoader 0x1087d0 directly (first 0x40 bytes):", direct)
inrange = sum(1 for f in t if 0x17e010 <= f <= 0x233880)
print("initializers in 0x17e010..0x233880:", inrange, " outside:", len(t) - inrange)
pdis(0x17e010, 0x20, True)
print([hex(a) for a in range(0x2bdef0, 0x2bdf00, 4)], [hex(X.u32(a)) for a in range(0x2bdef0, 0x2bdf00, 4)])
