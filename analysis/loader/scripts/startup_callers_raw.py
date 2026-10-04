"""Raw search for direct 'call rel32' (E8) and 'jmp rel32' (E9) to a target VA in all code sections,
including code Ghidra did not turn into functions. usage: startup_callers_raw.py 0xTARGET ..."""
import sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
def callers(t):
    res = []
    for name, va, vsz, raw, rsz in X.sections:
        if name not in (".text", "D3D", "DSOUND", "XMV", "XPP"): continue
        d = X.b[raw:raw + rsz]
        for i in range(len(d) - 5):
            if d[i] in (0xE8, 0xE9):
                rel = struct.unpack_from("<i", d, i + 1)[0]
                if va + i + 5 + rel == t:
                    res.append((va + i, "call" if d[i] == 0xE8 else "jmp"))
    return res
if __name__ == "__main__":
    for a in sys.argv[1:]:
        t = int(a, 16)
        print(hex(t), [(hex(x), k) for x, k in callers(t)])
