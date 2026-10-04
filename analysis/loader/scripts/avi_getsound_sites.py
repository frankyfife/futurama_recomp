"""avi_getsound_sites.py - (HEURISTIC argument recovery, "?" = not resolved) all direct call sites of the game's GetSound wrapper FUN_000c97d0(name, bStream)
and the pushed arguments (arg2 = stream flag stored at sounddata+0x27, arg1 = name)."""
import sys, struct, capstone
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
n, va, vsz, raw, rsz = [s for s in x.sections if s[0] == ".text"][0]
d = x.b[raw:raw + rsz]
TGT = 0xc97d0
sites = [va + i for i in range(len(d) - 5) if d[i] == 0xE8 and (va + i + 5 + struct.unpack_from("<i", d, i + 1)[0]) & 0xffffffff == TGT]
for s in sites:
    best = None
    for back in range(6, 64):
        ins = list(md.disasm(x.read(s - back, back + 5), s - back))
        if ins and ins[-1].address == s:
            best = ins; break
    pushes = [i for i in best if i.mnemonic == "push"] if best else []
    def fmt(i):
        try:
            v = int(i.op_str, 16); c = x.cstr(v) if x.section(v) == ".rdata" else None
            return '"%s"' % c if c else i.op_str
        except ValueError: return i.op_str
    args = [fmt(p) for p in pushes[-2:]]
    print("%08x  arg2(stream)=%-6s arg1=%s" % (s, args[0] if len(args) == 2 else "?", args[-1] if args else "?"))
