"""List virtual-call sites in the Ghidra decomp with a given vtable offset range,
together with the receiver expression and containing function."""
import sys, re
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from renderer_dfun import load
idx = load()
# matches (**(code **)(<expr> + 0xNN))  where <expr> is "*X" (vtable load)
PAT = re.compile(r"\(\*\*\(code \*\*\)\((\*.+?) \+ (0x[0-9a-f]+)\)\)")
PAT0 = re.compile(r"\(\*\(code \*\)\*\*?(.+?)\)")
def scan(lo, hi):
    res = []
    for a, (name, size, text, fn) in idx.items():
        for line in text.split("\n"):
            for m in PAT.finditer(line):
                off = int(m.group(2), 16)
                if lo <= off <= hi:
                    res.append((a, name, off, m.group(1), line.strip()))
    return res
if __name__ == "__main__":
    lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
    for a, name, off, recv, line in sorted(scan(lo, hi), key=lambda r: (r[2], r[0])):
        print(f"{off:#05x}\t{a:08x}\t{name}\t{recv}\t{line[:160]}")
