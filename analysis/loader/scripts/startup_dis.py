"""Disassemble retail_eu default.xbe at VA. usage: startup_dis.py 0xf8469 [nbytes]"""
import sys
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
_names = None
def names():
    global _names
    if _names is None:
        _names = {}
        for l in open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8"):
            p = l.rstrip("\n").split("\t")
            if p[0] == "address": continue
            _names[int(p[0], 16)] = p[3]
    return _names
def dis(va, n=0x100, stop_ret=False):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    code = X.read(va, n)
    out = []
    nm = names()
    for i in md.disasm(code, va):
        extra = ""
        if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x"):
            t = int(i.op_str, 16)
            if t in nm: extra = "   ; " + nm[t]
        if "dword ptr [0x" in i.op_str:
            import re
            m = re.search(r"\[0x([0-9a-f]+)\]", i.op_str)
            t = int(m.group(1), 16)
            if 0x293560 <= t < 0x293560 + 112*4:
                extra = "   ; kthunk[%d] ord %d" % ((t-0x293560)//4, X.u32(t) & 0x7fffffff)
        out.append("%08x: %-24s %s %s%s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str, extra))
        if stop_ret and i.mnemonic == "ret": break
    return "\n".join(out)
if __name__ == "__main__":
    va = int(sys.argv[1], 16)
    n = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x100
    print(dis(va, n, stop_ret=len(sys.argv) > 3))
