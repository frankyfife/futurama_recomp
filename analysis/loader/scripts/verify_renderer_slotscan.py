"""Independent per-statement data-flow scan (decompiled C) for virtual calls on the
renderer object -- re-check of renderer.md section 3 / key fact 8.
Differences to renderer_vcalls2.py / verify_renderer_vcalls.py:
  * statements are processed in order; an assignment to a variable from a
    non-renderer expression KILLS it (avoids the variable-reuse false positives
    of the earlier scans, e.g. FUN_0001f1a0 'piVar3 = FUN_000a3090("Frontend")')
  * the receiver must be exactly '*v' / '**(int **)(<src>)' -- accumulator calls
    through renderer+8 are not counted as renderer calls
  * renderer values that are STORED anywhere (global or field) are reported, so
    that further renderer sources can be discovered (output section 'stores')
Sources: app slot 5 (call [*app+0x14]) on DAT_002eb310/DAT_002e7b84 or on
param_1 inside app methods (vtables 0x2a3ef8 / 0x2a1810), *(x+0x104) on the
app, *(x+0x1a8) (NiCamera renderer), 'this' in NiXBoxRenderer/NiRenderer vtable
functions, list walkers of DAT_002d7b1c.
Usage: python verify_renderer_slotscan.py [slots|stores|func <hex>]"""
import re, sys, collections
from verify_renderer_common import *
D = alldecomp()
def vt_funcs(vt, n):
    return {X.u32(vt + 4 * i) for i in range(n)}
APP = vt_funcs(0x2a3ef8, 11) | vt_funcs(0x2a1810, 11)
REN_THIS = vt_funcs(0x2aa7c0, 75) - {0x114b0, 0xfdf90, 0xaed30, 0xfdfa0, 0xfdfb0, 0xfdfd0, 0xb0390, 0x13db0, 0x256c0, 0x1605f0, 0xaef60}
APPSRC = r"\(\*\*\(code \*\*\)\(\*(?:\(int \*\))?(?:DAT_002eb310|DAT_002e7b84) \+ 0x14\)\)\(\)"
APPSRC_THIS = r"\(\*\*\(code \*\*\)\(\*(?:\(int \*\))?param_1 \+ 0x14\)\)\(\)"
FIELD = r"\*\((?:int|undefined4|int \*|undefined4 \*) \*?\)\((?:[\w\[\]]+|\*\(int \*\)\([\w ]+\+ 0x[0-9a-f]+\)) \+ 0x1a8\)"
APPFLD = r"\*\((?:int|undefined4|int \*) \*?\)\((?:DAT_002eb310|DAT_002e7b84|param_1) \+ 0x104\)|(?:DAT_002eb310|DAT_002e7b84)\[0x41\]"
ASG = re.compile(r"^\s*(\w+) = (.+);\s*$")
VC = re.compile(r"\(\*\*\(code \*\*\)\((\*(?:\(int \*\))?\w+|\*\*\(int \*\*\)\(.+?\)|\*\(int \*\)\*?\(?.+?\)?) \+ (0x[0-9a-f]+)\)\)")
STORE = re.compile(r"^\s*(\*\(.+?\)|DAT_\w+|\w+\[[^\]]+\]) = (?:\([\w \*]+\))?(\w+);\s*$")
def is_src(expr, fa):
    e = expr.strip()
    e = re.sub(r"^\((?:int|undefined4|int \*|undefined4 \*|NiObject \*)\s*\*?\)\s*", "", e)
    if re.fullmatch(APPSRC, e): return True
    if fa in APP and re.fullmatch(APPSRC_THIS, e): return True
    if re.fullmatch(FIELD, e): return True
    if re.fullmatch(APPFLD, e): return True
    return False
def scan(fa):
    t = D.get(fa, "")
    ren = set()
    if fa in REN_THIS: ren |= {"param_1", "this"}
    walker = "DAT_002d7b1c" in t
    calls, stores = [], []
    for line in t.split("\n"):
        # vcalls first (they read the state before the assignment on this line)
        for m in VC.finditer(line):
            recv, off = m.group(1), int(m.group(2), 16)
            r = recv.strip()
            hit = False
            mm = re.fullmatch(r"\*(?:\(int \*\))?(\w+)", r)
            if mm and mm.group(1) in ren: hit = True
            mm = re.fullmatch(r"\*\*\(int \*\*\)\((.+)\)", r)
            if mm and is_src("*(int *)(" + mm.group(1) + ")", fa): hit = True
            if hit: calls.append((off, line.strip()))
        m = ASG.match(line)
        if m:
            v, rhs = m.groups()
            r2 = re.sub(r"^\((?:int|undefined4|int \*|undefined4 \*)\s*\*?\)\s*", "", rhs.strip())
            if is_src(rhs, fa) or r2 in ren or (walker and re.fullmatch(r"\*\w+", r2)):
                ren.add(v)
            else:
                ren.discard(v)
        m = STORE.match(line)
        if m and m.group(2) in ren and not ASG.match(line):
            stores.append(line.strip())
        elif m and m.group(2) in ren and m.group(1).startswith(("*(", "DAT_")):
            stores.append(line.strip())
    return calls, stores
if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "slots"
    if mode == "func":
        c, s = scan(int(sys.argv[2], 16))
        for x in c: print(f"slot {x[0]//4}: {x[1]}")
        for x in s: print("store:", x)
        sys.exit()
    res = collections.defaultdict(collections.Counter)
    allstores = []
    for fa in sorted(D):
        if not (0x11000 <= fa < 0x2342c0): continue
        c, s = scan(fa)
        for off, l in c:
            if off <= 0x128 and off % 4 == 0: res[off // 4][fa] += 1
        for x in s: allstores.append((fa, x))
    cls = lambda f: "GAME" if f < 0xfd000 else ("REN" if (0x146020 <= f < 0x14cd50 or 0x1552c0 <= f < 0x166871) else "NI")
    if mode == "slots":
        for s in range(75):
            fs = res.get(s, {})
            txt = ", ".join(f"{cls(f)}:{f:#x}" + (f"({n}x)" if n > 1 else "") for f, n in sorted(fs.items()))
            print(f"slot {s:2d}: {txt if txt else '-'}")
    if mode in ("stores", "slots"):
        print("\n# stores of renderer values into memory:")
        for fa, x in allstores: print(f"{fa:#x} {F.get(fa, ('?',))[0]}: {x}")
# --- 'passes' mode: direct calls that receive a renderer-valued variable as argument
def passes(fa):
    t = D.get(fa, "")
    ren = set()
    if fa in REN_THIS: ren |= {"param_1", "this"}
    out = []
    for line in t.split("\n"):
        for m in re.finditer(r"\b((?:FUN_|Ni)\w*(?:::\w+)?)\(([^()]*)\)", line):
            args = [a.strip() for a in m.group(2).split(",")]
            for a in args:
                a2 = re.sub(r"^\((?:int|undefined4|int \*|undefined4 \*|NiRenderer \*)\s*\*?\)\s*", "", a)
                if a2 in ren or is_src(a, fa): out.append(m.group(1)); break
        m = ASG.match(line)
        if m:
            v, rhs = m.groups()
            r2 = re.sub(r"^\((?:int|undefined4|int \*|undefined4 \*)\s*\*?\)\s*", "", rhs.strip())
            if is_src(rhs, fa) or r2 in ren: ren.add(v)
            else: ren.discard(v)
    return out
if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "passes":
    agg = collections.defaultdict(set)
    for fa in sorted(D):
        if not (0x11000 <= fa < 0xfd000): continue   # GAME only
        for callee in passes(fa): agg[callee].add(fa)
    for callee, fs in sorted(agg.items(), key=lambda x: -len(x[1])):
        print(f"{callee:32s} {len(fs):3d} GAME callers: " + " ".join(hex(f) for f in sorted(fs)[:12]))
# --- 'fields' mode: direct field accesses on renderer-valued variables / inline sources (GAME + NI)
def fields(fa):
    t = D.get(fa, "")
    ren = set()
    out = collections.Counter()
    srcs = [r"\*\(int \*\)\((?:DAT_002eb310|DAT_002e7b84) \+ 0x104\)", r"\*\(int \*\)\(param_1 \+ 0x1a8\)", r"\*\(int \*\)\(\w+ \+ 0x1a8\)"]
    for line in t.split("\n"):
        for v in ren:
            for m in re.finditer(r"\*\([\w ]+\*\)\(" + re.escape(v) + r" \+ (0x[0-9a-f]+|\d+)\)", line): out[int(m.group(1), 0)] += 1
            for m in re.finditer(r"\b" + re.escape(v) + r"\[(0x[0-9a-f]+|\d+)\]", line): out[4 * int(m.group(1), 0)] += 1
        for s in srcs:
            for m in re.finditer(r"\*\([\w ]+\*\)\(" + s + r" \+ (0x[0-9a-f]+|\d+)\)", line): out[int(m.group(1), 0)] += 1
        m = ASG.match(line)
        if m:
            v, rhs = m.groups()
            r2 = re.sub(r"^\((?:int|undefined4|int \*|undefined4 \*)\s*\*?\)\s*", "", rhs.strip())
            if is_src(rhs, fa) or r2 in ren: ren.add(v)
            else: ren.discard(v)
    return out
if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "fields":
    agg = collections.defaultdict(set)
    for fa in sorted(D):
        if not (0x11000 <= fa < 0x146020) or 0x146020 <= fa < 0x14cd50: continue
        for off in fields(fa): agg[off].add(fa)
    for off in sorted(agg):
        print(f"+{off:#x}: " + " ".join(hex(f) for f in sorted(agg[off])[:20]) + (" ..." if len(agg[off]) > 20 else ""))
