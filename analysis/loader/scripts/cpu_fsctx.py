"""cpu_fsctx.py - context of every fs: site with a non-zero offset (next instructions),
plus all references to the TLS index variable (TLS directory AddressOfIndex) and the
TLS directory itself.  usage: python cpu_fsctx.py"""
import sys, re, bisect, struct
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from cpu_rd import RD
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
rd = RD().run()
X = rd.x
md = Cs(CS_ARCH_X86, CS_MODE_32)
fst = [f[0] for f in rd.funcs]
def owner(va):
    k = bisect.bisect_right(fst, va) - 1
    s, sz, nm = rd.funcs[k]
    return nm if va < s + max(sz, 1) else "?(after %s)" % nm
tlsdir = struct.unpack_from("<I", X.b, 0x12C)[0]
f = [X.u32(tlsdir + 4 * i) for i in range(6)]
print("TLS directory @%#x: StartRaw=%#x EndRaw=%#x AddressOfIndex=%#x Callbacks=%#x SizeOfZeroFill=%#x Characteristics=%#x" % (tlsdir, *f))
idx = f[2]
rows = [l.rstrip("\n").split("\t") for l in open(r"D:\FuturamaDecomp\analysis\loader\fs_sites.tsv", encoding="utf-8")][1:]
for r in rows:
    if r[3] == "0x0":
        continue
    a = int(r[0], 16)
    code = X.read(a, 40)
    ins = list(md.disasm(code, a))[:5]
    print("%08x %-34s %-44s | %s" % (a, r[1] + " " + r[3], owner(a), " ; ".join("%s %s" % (i.mnemonic, i.op_str) for i in ins[1:])))
print("\nreferences to TLS index variable %#x and TLS directory %#x:" % (idx, tlsdir))
for va in sorted(rd.insn):
    sz, mn, ops, bb = rd.insn[va]
    if ("0x%x" % idx) in ops or re.search(r"0x2a5d(2c|30|34|38|3c|40)\b", ops):
        print("  %08x %-40s %s" % (va, mn + " " + ops, owner(va)))
