"""Summarize retail_eu .img archive listings (scripts/files_<img>_gamelist.txt from files_imgheader.py, game-accurate dir parsing):
top-level dirs per archive, localized languages, duplicate paths across archives (search order
SysPAL, FutPAL00..04 as in FUN_000d26a0 / FUN_0013fe10), alignment of offsets."""
import collections, os
D = r"D:\FuturamaDecomp\analysis\loader\scripts"
order = ["SysPAL", "FutPAL00", "FutPAL01", "FutPAL02", "FutPAL03", "FutPAL04"]
seen = {}
dups = collections.defaultdict(list)
for a in order:
    tops = collections.Counter(); langs = collections.Counter(); total = 0; misaligned = 0; n = 0
    for line in open(os.path.join(D, "files_" + a + "_gamelist.txt"), encoding="latin-1"):
        p = line.split(None, 2)
        if len(p) < 3: continue
        size, off, path = int(p[0]), int(p[1], 16), p[2].strip()
        n += 1; total += size
        if off % 0x800: misaligned += 1
        parts = path.lower().split("/")
        tops[parts[0] if len(parts) > 1 else "(root)"] += 1
        if "localized" in parts:
            i = parts.index("localized"); langs[parts[i + 1]] += 1
        key = path.lower()
        if key in seen: dups[key].append(a)
        else: seen[key] = a
    print(f"{a}: {n} files, {total} bytes payload, offsets not 2048-aligned: {misaligned}")
    print("   top:", dict(tops.most_common(12)))
    if langs: print("   localized:", dict(langs))
print("paths present in more than one archive (first wins):")
for k, v in sorted(dups.items()):
    print(f"   {k}: first={seen[k]} also={v}")
