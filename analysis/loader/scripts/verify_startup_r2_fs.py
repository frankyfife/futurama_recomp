"""Review 2: key fact 13 - every fs: access with offset != 0, found two ways:
(a) linear disassembly of every Ghidra function, (b) raw scan for 0x64 prefix bytes in code sections outside
Ghidra functions, decoded and kept only when the decode is plausible (instruction stream from up to 0x30 bytes
before reaches the candidate exactly). Prints groups by offset and owning function."""
from verify_startup_r2_common import *
import collections, re
from verify_startup_r2_common import _FUNCS

hits = {}
secs = {n: (va, va + rsz) for n, va, vsz, raw, rsz in X.sections}
def in_code(a):
    for n in CODESEC:
        if secs[n][0] <= a < secs[n][1]:
            return n
for a, size, name in _FUNCS:
    if not in_code(a):
        continue
    for ins in CS.disasm(X.read(a, size), a):
        if "fs:" in ins.op_str:
            hits[ins.address] = (ins, "%s(0x%X)" % (name, a))
# raw: 0x64 byte not inside a Ghidra function
for n, va, d in code_sections():
    p = d.find(b"\x64")
    while p >= 0:
        a = va + p
        if not func_of(a) and a not in hits:
            ins = list(CS.disasm(X.read(a, 16), a, 1))
            if ins and "fs:" in ins[0].op_str:
                b = before(a, 1, 0x30)
                if b:  # plausible: some decode from before ends exactly here
                    hits[a] = (ins[0], "<keine Ghidra-Funktion>")
        p = d.find(b"\x64", p + 1)
by = collections.defaultdict(list)
for a in sorted(hits):
    ins, f = hits[a]
    m = re.search(r"fs:\[(0x[0-9a-f]+|\d+)\]", ins.op_str)
    off = int(m.group(1), 0) if m else None
    if m is None:
        off = "reg"
    by[off].append((a, ins, f))
for off in sorted(by, key=lambda x: (isinstance(x, str), x)):
    lst = by[off]
    fs_ = sorted({f for a, i, f in lst})
    print("== fs:[%s]  %d sites in %d functions" % (hex(off) if isinstance(off, int) else off, len(lst), len(fs_)))
    if off != 0:
        for a, ins, f in lst:
            print("   0x%X %-40s %s  [%s]" % (a, ins.mnemonic + " " + ins.op_str, f, in_code(a)))
