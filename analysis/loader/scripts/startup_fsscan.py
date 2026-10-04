"""Linear-sweep disassembly of all code sections (function ranges from functions.tsv) and list every
fs:-segment access, grouped by fs offset and by function. Needed for the loader's KPCR/TEB emulation."""
import sys, collections, re
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
funcs = []
for l in open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8"):
    p = l.rstrip("\n").split("\t")
    if p[0] == "address": continue
    funcs.append((int(p[0], 16), int(p[1]), p[2], p[3]))
md = Cs(CS_ARCH_X86, CS_MODE_32)
byoff = collections.defaultdict(list)
for a, n, blk, name in funcs:
    code = X.read(a, n)
    if not code: continue
    for i in md.disasm(code, a):
        if "fs:" in i.op_str:
            m = re.search(r"fs:\[([^\]]+)\]", i.op_str)
            byoff[m.group(1)].append((name, a, i.address, i.mnemonic + " " + i.op_str))
if __name__ == "__main__":
    for off in sorted(byoff, key=lambda k: (len(k), k)):
        lst = byoff[off]
        fn = collections.Counter("%s@%x" % (x[0], x[1]) for x in lst)
        nonseh = [k for k in fn]
        print("fs:[%s]  %d accesses in %d functions" % (off, len(lst), len(fn)))
        if off not in ("0",):
            for k, c in sorted(fn.items()):
                print("     %-60s %d" % (k, c))

def raw_outside():
    """fs: instructions in code sections that lie outside every Ghidra function (e.g. 0xf8469, 0xf99da)."""
    import bisect
    starts = sorted((a, a + n) for a, n, _, _ in funcs)
    sa = [s for s, _ in starts]
    def covered(v):
        i = bisect.bisect_right(sa, v) - 1
        return i >= 0 and starts[i][0] <= v < starts[i][1]
    res = []
    for name, va, vsz, raw, rsz in X.sections:
        if name not in (".text", "D3D", "DSOUND", "XMV", "XPP", "DOLBY"): continue
        data = X.b[raw:raw + rsz]
        p = data.find(b"\x64")
        while p >= 0:
            v = va + p
            if not covered(v):
                ins = list(md.disasm(data[p:p + 15], v))
                if ins and "fs:" in ins[0].op_str and ins[0].size >= 6:
                    res.append("%08x %s %s %s" % (v, ins[0].bytes.hex(), ins[0].mnemonic, ins[0].op_str))
            p = data.find(b"\x64", p + 1)
    return res
if __name__ == "__main__" and "--raw" in sys.argv:
    print("\n".join(raw_outside()))
