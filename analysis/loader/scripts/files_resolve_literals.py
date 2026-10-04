r"""Check which literal (non-format) file paths referenced by retail_eu code resolve inside the .img archives
(game-accurate listings) -- validates files_imgheader.py parsing and finds paths that must come from loose D:\ files."""
import ast, os, re
S = r"D:\FuturamaDecomp\analysis\loader\scripts"
names = {}
for a in ["SysPAL", "FutPAL00", "FutPAL01", "FutPAL02", "FutPAL03", "FutPAL04"]:
    for line in open(os.path.join(S, f"files_{a}_gamelist.txt"), encoding="latin-1"):
        p = line.split(None, 2)
        if len(p) == 3: names.setdefault(p[2].strip().lower(), a)
ok, miss = [], []
for line in open(os.path.join(S, "files_strings.out"), encoding="latin-1"):
    va, sec, kind, xr, lit = line.rstrip("\n").split("\t", 4)
    s = ast.literal_eval(lit)
    if kind != "A" or "%" in s or not re.search(r"\.(nif|wav|tga|txt|ldb|fnt|bin|ful|ucf|key|xbx|dat)$", s, re.I):
        continue
    if not xr: continue  # unreferenced / prefix-shifted strings
    if re.match(r"(?i)[a-z]:\|\\", s):
        miss.append((va, s, "absolute")); continue
    k = s.replace("\\", "/").lower()
    if k in names: ok.append((va, s, names[k]))
    else: miss.append((va, s, "not in any .img"))
print(f"resolved in archives: {len(ok)}")
for va, s, a in ok[:8]: print(f"   {va} {s} -> {a}")
print(f"not resolved: {len(miss)}")
for va, s, why in miss: print(f"   {va} {s}  ({why})")
