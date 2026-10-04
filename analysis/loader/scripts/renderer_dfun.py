"""Helper: index decompiled functions of the Ghidra export.
Usage: python renderer_dfun.py 146350 146400 ...   (prints decomp of given addrs)"""
import sys, os, re, pickle
D = r"D:\FuturamaDecomp\analysis\retail_eu_export\decomp"
CACHE = os.path.join(os.environ.get("TEMP", "."), "fut_decomp_idx.pkl")
HDR = re.compile(r"^// ==== FUNC ([0-9a-f]{8}) (\S+) size=(\d+) ====", re.M)
def load():
    if os.path.exists(CACHE):
        return pickle.load(open(CACHE, "rb"))
    idx = {}
    for fn in sorted(os.listdir(D)):
        t = open(os.path.join(D, fn), encoding="utf-8", errors="replace").read()
        ms = list(HDR.finditer(t))
        for i, m in enumerate(ms):
            end = ms[i + 1].start() if i + 1 < len(ms) else len(t)
            idx[int(m.group(1), 16)] = (m.group(2), int(m.group(3)), t[m.start():end], fn)
    pickle.dump(idx, open(CACHE, "wb"))
    return idx
if __name__ == "__main__":
    idx = load()
    for a in sys.argv[1:]:
        a = int(a, 16)
        if a in idx:
            print(idx[a][2])
        else:
            print("// no func", hex(a))
