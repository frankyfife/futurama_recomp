"""Independent decomp-based scan for virtual calls on the renderer object.
Renderer-valued expressions (seeds):
  *(... + 0x1a8)                    NiCamera::m_spRenderer
  *(DAT_002eb310 + 0x104), *(DAT_002e7b84 + 0x104), *(param_1 + 0x104) in app methods
  (**(code **)(*DAT_002eb310 + 0x14))() / (*DAT_002e7b84 + 0x14) / (*param_1 + 0x14) in app methods
  *piVar where piVar walks DAT_002d7b1c (renderer list)
Local variables assigned (transitively) from a seed are renderer-valued.
Output: per slot -> list of (function, count); plus slots with no hit."""
import re, collections, sys
from verify_renderer_common import *
D = alldecomp()
APP_METHODS = {0xd2630, 0xa26a0, 0xd26a0, 0xa24a0, 0xd2920, 0xa2c20}
SEED = [r"\+ 0x1a8\)", r"DAT_002eb310 \+ 0x104\)", r"DAT_002e7b84 \+ 0x104\)",
        r"\(\*\*\(code \*\*\)\(\*(?:\(int \*\))?DAT_002eb310 \+ 0x14\)\)\(\)",
        r"\(\*\*\(code \*\*\)\(\*(?:\(int \*\))?DAT_002e7b84 \+ 0x14\)\)\(\)"]
SEED_APP = [r"param_1 \+ 0x104\)", r"\(\*\*\(code \*\*\)\(\*param_1 \+ 0x14\)\)\(\)"]
VC = re.compile(r"\(\*\*\(code \*\*\)\((.+?) \+ (0x[0-9a-f]+)\)\)")
ASG = re.compile(r"^\s*(\w+) = (.+);\s*$")
res = collections.defaultdict(collections.Counter)
for a, t in D.items():
    if not (0x11000 <= a < 0x2342c0): continue
    seeds = SEED + (SEED_APP if a in APP_METHODS else [])
    if not any(re.search(s, t) for s in seeds) and "DAT_002d7b1c" not in t: continue
    lines = t.split("\n")
    renvars = set()
    # fixed point over assignments
    for _ in range(4):
        for l in lines:
            m = ASG.match(l)
            if not m: continue
            v, rhs = m.groups()
            if any(re.search(s, rhs) for s in seeds) or any(re.fullmatch(r"(?:\([\w ]+\*\))?" + re.escape(r), rhs.strip()) for r in renvars):
                renvars.add(v)
            if "DAT_002d7b1c" in t and re.fullmatch(r"\(int \*\)\*\w+", rhs.strip()):
                renvars.add(v)   # piVar1 = (int *)*piVar3 in list walkers
    for l in lines:
        for m in VC.finditer(l):
            recv, off = m.group(1), int(m.group(2), 16)
            r = recv.strip()
            inner = re.sub(r"^\*\s*(?:\([\w ]+\*\))?", "", r)   # strip leading deref + cast
            inner = re.sub(r"^\*\s*(?:\([\w ]+\*\))?", "", inner)
            hit = any(re.search(s, r) for s in seeds) or any(re.fullmatch(r"\(?(?:\([\w ]+\*\))?" + re.escape(v) + r"\)?", inner) for v in renvars)
            if hit and off <= 0x128:
                res[off // 4][a] += 1
mode = sys.argv[1] if len(sys.argv) > 1 else "slots"
for s in range(75):
    fs = res.get(s, {})
    cls = lambda f: "GAME" if f < 0xfd000 else ("REN" if (0x146020 <= f < 0x14cd50 or 0x1552c0 <= f < 0x166871) else "NI")
    txt = ", ".join(f"{cls(f)}:{f:#x}" + (f"({c}x)" if c > 1 else "") for f, c in sorted(fs.items()))
    print(f"slot {s:2d}: {txt if txt else '-'}")
