"""Compare tools/futimg.py listings (orig/retail_eu/<img>_files.txt, absolute dir-end interpretation) with the
game-accurate listings (files_<img>_gamelist.txt, dir-end relative to the u32 field as in FUN_0013fc40)."""
import os
O = r"D:\FuturamaDecomp\orig\retail_eu"; S = r"D:\FuturamaDecomp\analysis\loader\scripts"
def load(p):
    r = {}
    for line in open(p, encoding="latin-1"):
        a = line.split(None, 2)
        if len(a) == 3: r[(int(a[0]), int(a[1], 16))] = a[2].strip()
    return r
for a in ["SysPAL", "FutPAL00", "FutPAL01", "FutPAL02", "FutPAL03", "FutPAL04"]:
    old = load(os.path.join(O, a + "_files.txt")); new = load(os.path.join(S, f"files_{a}_gamelist.txt"))
    diff = [(old[k], new[k]) for k in new if k in old and old[k].lower() != new[k].lower()]
    print(f"{a}: {len(new)} files, same (size,offset) keys {sum(1 for k in new if k in old)}, path differs {len(diff)}")
    for o, n in diff[:3]: print(f"    futimg: {o:50s} game: {n}")
