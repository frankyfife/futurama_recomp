"""Independent fs: segment access scan. Every 0x64 byte in code sections is decoded as an instruction; hits with an
fs: memory operand are kept if (a) the address is an instruction boundary in a linear disassembly of its Ghidra
function, or (b) outside functions, a back-disassembly from up to 0x40 bytes earlier syncs onto it (marked '?').
Output: per fs offset the list of (function, address)."""
from verify_startup_common import *
import collections
bound_cache = {}
def boundaries(f):
    if f[0] not in bound_cache:
        bound_cache[f[0]] = set(i.address for i in md.disasm(X.read(f[0], f[1]), f[0]))
    return bound_cache[f[0]]
res = collections.defaultdict(list)
for name, va, d in sec_bytes():
    p = d.find(b"\x64")
    while p >= 0:
        a = va + p
        ins = next(md.disasm(d[p:p + 16], a), None)
        if ins and "fs:[" in ins.op_str:
            m = re.search(r"fs:\[(0x[0-9a-f]+|\w+)\]", ins.op_str)
            off = m.group(1) if m else "?"
            f = func_of(a)
            if f:
                ok = a in boundaries(f)
                tag = "%x %s" % (f[0], f[2]) if ok else None
            else:
                pre, post = back_dis(a, 0x40, 8)
                ok = bool(post) and post[0].address == a and len(pre) >= 3
                tag = "<nofunc>?" if ok else None
            if tag:
                res[off].append((a, tag, ins.mnemonic + " " + ins.op_str))
        p = d.find(b"\x64", p + 1)
for off in sorted(res, key=lambda s: int(s, 16) if s.startswith("0x") else -1):
    L = res[off]
    fs_ = sorted(set(t for a, t, i in L))
    print("fs:[%s]  %d accesses in %d functions/places" % (off, len(L), len(fs_)))
    if off not in ("0",) and len(L) < 80:
        for a, t, i in L: print("    %08x  %-45s %s" % (a, t, i))
