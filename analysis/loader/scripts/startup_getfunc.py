"""Print decompiled C of functions by address from the Ghidra export.
usage: python startup_getfunc.py 0xf84dd [0x...]"""
import sys, os, re, glob
D = r"D:\FuturamaDecomp\analysis\retail_eu_export\decomp"
_index = None
def index():
    global _index
    if _index is None:
        _index = {}
        for f in glob.glob(os.path.join(D, "*.c")):
            with open(f, encoding="utf-8", errors="replace") as fh:
                data = fh.read()
            for m in re.finditer(r"^// ==== FUNC ([0-9a-fA-F]+) (\S+) size=(\d+) ====$", data, re.M):
                _index[int(m.group(1), 16)] = (f, m.start())
    return _index
def get(addr):
    idx = index()
    if addr not in idx:
        return None
    f, pos = idx[addr]
    with open(f, encoding="utf-8", errors="replace") as fh:
        data = fh.read()
    nxt = data.find("\n// ==== FUNC ", pos + 10)
    return data[pos: nxt if nxt > 0 else len(data)]
if __name__ == "__main__":
    for a in sys.argv[1:]:
        s = get(int(a, 16))
        print(s if s else "// not found %s" % a)
