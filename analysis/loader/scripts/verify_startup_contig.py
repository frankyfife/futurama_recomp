"""MmAllocateContiguousMemoryEx (ord 166) / MmAllocateContiguousMemory (165) / XPhysicalAlloc call sites with the
pushed arguments; checks the caller does not clean the stack (stdcall)."""
from verify_startup_common import *
def show(site, n=8):
    # disassemble the containing function (or 0x60 bytes before) linearly to get exact pushes
    f = func_of(site)
    start = f[0] if f and site - f[0] < 0x800 else site - 0x60
    ins = [i for i in md.disasm(X.read(start, site - start + 12), start)]
    idx = [k for k, i in enumerate(ins) if i.address == site]
    if not idx:
        pre, post = back_dis(site, 0x60, 12); seq = pre + post
        k0 = len(pre)
    else:
        seq = ins; k0 = idx[0]
    pushes = []
    for i in reversed(seq[:k0]):
        if i.mnemonic == "push": pushes.append(i.op_str)
        if i.mnemonic in ("call", "ret") or len(pushes) >= n: break
    nxt = seq[k0 + 1] if k0 + 1 < len(seq) else None
    return pushes, (nxt.mnemonic + " " + nxt.op_str) if nxt else "?"
for o in (166, 165):
    s = TH.index(o)
    print("=== ord %d slot %d" % (o, s))
    calls, other = kcall_sites(s)
    for c in calls:
        f = func_of(c)
        p, nx = show(c, 5 if o == 166 else 1)
        print("  %08x %-45s args(1st..): %s | next: %s" % (c, ("%x %s" % (f[0], f[2])) if f else "<nofunc>", p, nx))
    for c in other:
        print("  other ref", hex(c), func_of(c))
xp = [x for x in funcs() if x[2] == "XAPILIB::XPhysicalAlloc"]
print("XPhysicalAlloc:", xp)
for a, sz, nm in xp:
    for c, k in raw_calls(a):
        f = func_of(c)
        p, nx = show(c, 4)
        print("  %08x %s %-40s args: %s | next: %s" % (c, k, ("%x %s" % (f[0], f[2])) if f else "<nofunc>", p, nx))
