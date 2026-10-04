"""Like renderer_vcalls.py but resolves simple local receivers: for '*piVarN'
prints the nearest preceding assignment 'piVarN = ...;' in the same function."""
import sys, re
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from renderer_dfun import load
idx = load()
PAT = re.compile(r"\(\*\*\(code \*\*\)\((\*.+?) \+ (0x[0-9a-f]+)\)\)")
def scan(lo, hi):
    res = []
    for a, (name, size, text, fn) in idx.items():
        lines = text.split("\n")
        for li, line in enumerate(lines):
            for m in PAT.finditer(line):
                off = int(m.group(2), 16)
                if not (lo <= off <= hi): continue
                recv = m.group(1)
                src = ""
                mv = re.fullmatch(r"\*(?:\(int \*\))?(\w+)", recv)
                if mv:
                    v = mv.group(1)
                    for lj in range(li, -1, -1):
                        ma = re.search(r"\b" + re.escape(v) + r" = ([^;]+);", lines[lj])
                        if ma and not lines[lj].strip().startswith("*"):
                            src = ma.group(1); break
                res.append((a, name, off, recv, src, line.strip()))
    return res
if __name__ == "__main__":
    lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
    for a, name, off, recv, src, line in sorted(scan(lo, hi), key=lambda r: (r[2], r[0])):
        print(f"{off:#05x}\t{a:08x}\t{name}\t{recv}\t{src[:70]}\t{line[:120]}")
