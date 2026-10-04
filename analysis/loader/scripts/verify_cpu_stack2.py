"""verify_cpu_stack2.py - stack-related facts not covered by cpu.md:
 * every 'sub esp, imm' >= 0x1000 anywhere in Ghidra bodies (not only in the prologue)
 * every call of the _chkstk-like probe FUN_00167150 with the size in eax
 * stack sizes requested from CreateThread/CreateFiber call sites (immediate pushes)."""
import sys, struct, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *

big = []
probe = []
for (s, sz, nm) in FUNCS:
    if X.section(s) not in CODE or sz <= 0:
        continue
    ins = list(mdn.disasm(X.read(s, sz), s))
    for j, i in enumerate(ins):
        if i.mnemonic == "sub" and i.op_str.startswith("esp, 0x"):
            v = int(i.op_str.split(",")[1], 16)
            if v >= 0x1000:
                big.append((i.address, v, nm))
        if i.mnemonic == "add" and i.op_str.startswith("esp, -"):
            v = -int(i.op_str.split(",")[1], 0)
            if v >= 0x1000:
                big.append((i.address, v, nm))
        if i.mnemonic == "call" and i.op_str == "0x167150":
            prev = ins[j - 1] if j else None
            probe.append((i.address, prev.mnemonic + " " + prev.op_str if prev else "", nm))
print("'sub esp,>=0x1000' / 'add esp,-x' anywhere in bodies:", len(big))
for b in big:
    print("   %08x size=%#x %s" % b)
print("calls to FUN_00167150 (_chkstk-like):", len(probe))
for p in probe:
    print("   %08x %-24s %s" % p)


def raw_calls(t):
    out = []
    for (n, lo, hi, raw) in sec_ranges():
        d = X.b[raw:raw + (hi - lo)]
        for p in range(len(d) - 5):
            if d[p] == 0xE8 and ((lo + p + 5 + struct.unpack_from("<i", d, p + 1)[0]) & 0xffffffff) == t:
                out.append(lo + p)
    return out


for nm, t, argi in (("CreateThread", 0xf9a72, 2), ("CreateFiber", 0x1708b0, 1)):
    for a in raw_calls(t):
        # walk back over the pushes
        ins = [i for i in mdn.disasm(X.read(a - 40, 45), a - 40) if i.address <= a]
        pushes = [i for i in ins if i.mnemonic == "push"][-6:]
        o = owner(a)
        print("%s call %08x in %s; last pushes: %s" % (nm, a, o[2] if o else "-", " | ".join(p.op_str for p in pushes)))
print("XBE default stack size [0x10130] = %#x" % struct.unpack_from("<I", X.b, 0x130)[0])
