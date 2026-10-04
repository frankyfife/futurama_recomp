"""Round-2: compare avi_api_surface.tsv (site counts, ret N) with the
independent results in out/verify_avi_r2_rawscan.out and
out/verify_avi_r2_callconv.out."""
import re, os
from verify_avi_r2_common import OUT, Tee
T = Tee("verify_avi_r2_tsvcheck.out")
L = r"D:\FuturamaDecomp\analysis\loader"
raw = {}
for line in open(os.path.join(OUT, "verify_avi_r2_rawscan.out"), encoding="utf-8"):
    m = re.match(r"([0-9a-f]{8}) \S+\s+(DSOUND|XMV|XPP)\s+n=(\d+)\s+(.*)", line)
    if m:
        sites = re.findall(r"([0-9a-f]{8})\(([^:]+):", m.group(4))
        raw[m.group(1)] = (sum(1 for a, s in sites if s == ".text"), sum(1 for a, s in sites if s == "XMV"))
rets = {}
for line in open(os.path.join(OUT, "verify_avi_r2_callconv.out"), encoding="utf-8"):
    m = re.match(r"([0-9a-f]{8}) .* ret \[(\d+)\]", line)
    if m:
        rets[m.group(1)] = int(m.group(2))
bad = 0
n = 0
for line in open(os.path.join(L, "avi_api_surface.tsv"), encoding="utf-8").read().splitlines()[1:]:
    c = line.split("\t")
    if c[0].startswith("vtbl"):
        continue
    n += 1
    g, x = int(c[4]), int(c[5])
    r = raw.get(c[0])
    rn = re.search(r"ret (\d+)", c[3])
    ok = r == (g, x) and rn and int(rn.group(1)) == rets.get(c[0])
    if not ok:
        bad += 1
    T("%s %-48s tsv=%d/%d raw=%s ret tsv=%s r2=%s %s" % (c[0], c[2][:48], g, x, r, rn.group(1) if rn else None, rets.get(c[0]), "OK" if ok else "MISMATCH"))
T("rows:", n, "mismatches:", bad, "raw targets not in tsv:", sorted(set(raw) - {l.split('\t')[0] for l in open(os.path.join(L, 'avi_api_surface.tsv'), encoding='utf-8')}))
