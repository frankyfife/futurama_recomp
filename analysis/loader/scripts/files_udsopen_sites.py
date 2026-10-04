"""List all call sites of NiUDSStream::NiUDSStream (0x13f250, __thiscall(name, asyncOwner)) in retail_eu
with the two preceding push operands (name, async owner/callback). Non-zero 2nd arg => fiber-based async stream."""
import struct, sys, bisect
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = Cs(CS_ARCH_X86, CS_MODE_32)
funcs = []
for line in open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8"):
    p = line.rstrip("\n").split("\t")
    if p[0] != "address": funcs.append((int(p[0], 16), p[3]))
funcs.sort(); starts = [f[0] for f in funcs]
TARGET = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x13f250
name, sva, vsz, raw, rsz = [s for s in X.sections if s[0] == ".text"][0]
blob = X.b[raw:raw + rsz]
for i in range(len(blob) - 5):
    if blob[i] == 0xE8 and (sva + i + 5 + struct.unpack_from("<i", blob, i + 1)[0]) & 0xffffffff == TARGET:
        site = sva + i
        fi = bisect.bisect_right(starts, site) - 1
        f0 = funcs[fi][0]
        ins = list(md.disasm(X.read(f0, site - f0), f0))
        pushes = [x for x in ins if x.mnemonic == "push"][-2:]
        desc = []
        for x in pushes:
            s = x.op_str
            try:
                v = int(s, 16); c = X.cstr(v) if X.section(v) == ".rdata" else None
                if c: s += f" '{c}'"
            except ValueError:
                pass
            desc.append(s)
        print(f"{site:08x} in {funcs[fi][1]:14s} pushes(last two, 2nd=name,1st=async): {desc}")
