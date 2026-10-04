"""Print the 'ret N' stack-cleanup of each NiXBoxRenderer vtable function
(thiscall: N/4 = number of stack arguments). Follows a leading jmp thunk."""
import sys
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
import capstone, csv
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
F = {int(r["address"], 16): int(r["size"]) for r in csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"), delimiter="\t")}
def rets(a, depth=0):
    size = max(F.get(a, 0), 16)
    code = X.read(a, size + 16)
    first = next(md.disasm(code, a), None)
    if first and first.mnemonic == "jmp" and depth < 2:
        try: return rets(int(first.op_str, 16), depth + 1)
        except ValueError: pass
    s = set()
    for ins in md.disasm(X.read(a, size), a):
        if ins.mnemonic == "ret":
            s.add(int(ins.op_str, 0) if ins.op_str else 0)
    return s
if __name__ == "__main__":
    vt = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x2aa7c0
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 75
    for i in range(n):
        a = X.u32(vt + 4 * i)
        r = rets(a)
        print(f"{i}\t{a:08x}\t" + ",".join(f"ret {x:#x} ({x // 4} args)" for x in sorted(r)))
