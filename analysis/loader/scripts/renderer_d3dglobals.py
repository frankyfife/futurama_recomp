"""Count .text instructions that access D3D-library data (D3D section bss/data,
0x243890-0x246cf8: render-state shadow, deferred texture state, dirty flags,
index base, device pointer...) grouped by module (REN / NI / GAME / CRT)."""
import sys, csv, re, bisect, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
import capstone
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
F = sorted((int(r["address"], 16), int(r["size"]), r["name"]) for r in csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"), delimiter="\t") if r["block"] == ".text")
REN = [(0x146020, 0x14cd50), (0x1552c0, 0x166871)]
def cls(a):
    if any(lo <= a < hi for lo, hi in REN): return "REN"
    if 0xfd000 <= a < 0x166871: return "NI"
    if a >= 0x166871: return "CRT"
    return "GAME"
LO, HI = 0x2342c0 + 0xf5d0, 0x2342c0 + 0x12a38
memre = re.compile(r"\[(0x[0-9a-f]+)\]")
cnt = collections.Counter(); funcs = collections.defaultdict(set); addrs = collections.defaultdict(collections.Counter)
for a, sz, name in F:
    if sz <= 0: continue
    code = X.read(a, sz)
    if not code: continue
    for ins in md.disasm(code, a):
        for m in memre.finditer(ins.op_str):
            v = int(m.group(1), 16)
            if LO <= v < HI:
                k = cls(a)
                w = "W" if ins.op_str.startswith("dword ptr [" + m.group(1)) or ins.op_str.startswith("byte ptr [" + m.group(1)) else "R"
                cnt[(k, w)] += 1; funcs[k].add(a); addrs[k][v] += 1
for k in sorted(funcs):
    print(k, "functions:", len(funcs[k]), "accesses W/R:", cnt[(k, "W")], cnt[(k, "R")], "distinct globals:", len(addrs[k]))
    if k != "REN":
        print("   funcs:", " ".join(f"{f:08x}" for f in sorted(funcs[k])[:30]))
        print("   globals:", " ".join(f"{g:08x}" for g, _ in addrs[k].most_common(20)))
print("REN top globals:", " ".join(f"{g:08x}x{c}" for g, c in addrs["REN"].most_common(25)))
