"""Decomp scan: direct field accesses on renderer-valued expressions outside the
renderer module (offsets other than the vtable). Seeds as in verify_renderer_vcalls.py.
Reports function, offset and the source line."""
import re, collections
from verify_renderer_common import *
D = alldecomp()
SEEDX = [r"\*\(int \*\)\(DAT_002eb310 \+ 0x104\)", r"\*\(int \*\)\(DAT_002e7b84 \+ 0x104\)",
         r"\(\*\*\(code \*\*\)\(\*(?:\(int \*\))?DAT_00(?:2eb310|2e7b84) \+ 0x14\)\)\(\)",
         r"\*\(int \*\)\(\w+ \+ 0x1a8\)"]
ASG = re.compile(r"^\s*(\w+) = (?:\([\w ]+\*\)\s*)?(.+);\s*$")
REN = [(0x146020, 0x14cd50), (0x1552c0, 0x166871)]
out = collections.defaultdict(list)
for a, t in D.items():
    if any(lo <= a < hi for lo, hi in REN) or not (0x11000 <= a < 0x2342c0): continue
    if not any(re.search(s, t) for s in SEEDX): continue
    lines = t.split("\n"); rv = set()
    for l in lines:
        m = ASG.match(l)
        if m and any(re.fullmatch(s, m.group(2).strip()) for s in SEEDX):
            rv.add(m.group(1))
    exprs = [s for s in SEEDX] + [re.escape(v) for v in rv]
    for l in lines:
        for e in exprs:
            # *(T *)(EXPR + 0xNN)   and   EXPR[N]
            for m in re.finditer(r"\(\s*(?:\([\w ]+\*\))?\(?" + e + r"\)? \+ (0x[0-9a-f]+)\)", l):
                off = int(m.group(1), 16)
                if "code **" in l[max(0, m.start() - 20):m.start()]: continue
                out[a].append((off, l.strip()[:140]))
            for m in re.finditer(r"\b" + e + r"\[(0x[0-9a-f]+|\d+)\]", l) if not e.startswith("\\") else []:
                out[a].append((int(m.group(1), 0) * 4, l.strip()[:140]))
for a in sorted(out):
    seen = set()
    for off, l in out[a]:
        if (off, l) in seen: continue
        seen.add((off, l))
        print(f"{a:#08x} {F[a][0]:28s} +{off:#x}\t{l}")
