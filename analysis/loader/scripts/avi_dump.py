"""avi_dump.py - print decompiled C of functions from the Ghidra export.
Usage: python avi_dump.py <addr_hex|lo-hi> ...   (range = all functions starting in [lo,hi))"""
import sys, os, re, glob
EXP = r"D:\FuturamaDecomp\analysis\retail_eu_export\decomp"
hdr = re.compile(r"^// ==== FUNC ([0-9a-fA-F]+) ")
want = []
for a in sys.argv[1:]:
    if "-" in a:
        lo, hi = a.split("-"); want.append((int(lo, 16), int(hi, 16)))
    else:
        v = int(a, 16); want.append((v, v + 1))
def hit(x): return any(lo <= x < hi for lo, hi in want)
for fn in sorted(glob.glob(os.path.join(EXP, "*.c"))):
    on = False
    with open(fn, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = hdr.match(line)
            if m: on = hit(int(m.group(1), 16))
            if on: sys.stdout.write(line)
