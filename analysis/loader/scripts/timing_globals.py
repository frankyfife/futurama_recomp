"""Readers and writers of the timing globals (timing60.md, sections 3 and 4.1).

For every global written by Game::Init 0xA26A0: all code instructions that use the address (capstone index of
Ghidra functions + gap code, timing_common.index) and all raw dword hits in every section (data tables,
pointers). Also scans for base-relative access: dword constants in 0x2E7A54..0x2E7B83 (a struct base near the
block would show up here). Then: dt users by instruction kind, users of the real frame delta 0x2E7B88, the
fps/dt readers reachable from CApp::Init 0xD26A0 before Game::Init 0xA26A0 (callgraph), and the C++ initializer
range (0x17E010..0x233880, startup.md 2.4) against all dt/fps users.
Usage: python timing_globals.py
"""
import re
import struct
from collections import Counter, defaultdict

from timing_common import (R, CALLG, G_DT, G_FPS, G_FIXED, G_DT_MIN, G_DT_MAX, G_FPS2, G_B68, G_FPS3, G_TAG,
                           G_FRAME_DELTA, mem_refs, absrefs, owner_name, fmt, f32, index)

NAMES = [(G_FPS, "fps"), (G_DT, "dt"), (G_FIXED, "fixed-step flag"), (G_DT_MIN, "dt clamp min"),
         (G_DT_MAX, "dt clamp max"), (G_FPS2, "fps copy"), (G_B68, "1/25.4 | 1/30.5"), (G_FPS3, "fps copy 2"),
         (G_TAG, "tag PAL/NTSC"), (G_TAG + 4, "tag byte 4"), (G_FRAME_DELTA, "real frame delta")]
WRITE = ("fst", "fstp")


def kind(e, a):
    op = e[3]
    first = op.split(",")[0]
    if e[2] in WRITE or (e[2] == "mov" and ("[0x%x]" % a) in first):
        return "W"
    if e[2] == "push" and op == "0x%x" % a:
        return "A"  # address taken
    return "R"


def main():
    print("== 1. code references per global (W write, R read, A address pushed)")
    for a, n in NAMES:
        refs = mem_refs(a)
        raw = absrefs(R, a)
        ks = Counter(kind(e, a) for e in refs)
        print("  0x%06X %-18s code %3d %-22s raw hits %3d (non-code: %s)" % (
            a, n, len(refs), dict(ks), len(raw), [(s, hex(v)) for s, v in raw if s in (".data", ".rdata")]))
        if len(refs) <= 12:
            for e in refs:
                print("      %s %s" % (kind(e, a), fmt(e)))

    print("\n== 2. dword constants 0x2E7A54..0x2E7B83 anywhere in the image (struct-base access check)")
    c = Counter()
    for name, va, vsz, raw, rsz in R.sections:
        d = R.b[raw:raw + rsz]
        for i in range(len(d) - 3):
            v = struct.unpack_from("<I", d, i)[0]
            if 0x2E7A54 <= v < 0x2E7B84:
                c[v] += 1
    print("  " + ", ".join("%X:%d" % kv for kv in sorted(c.items())))

    print("\n== 3. dt [0x2D6748] users")
    r = mem_refs(G_DT)
    owners = {e[5] for e in r}
    print("  %d instructions in %d functions/segments (%d gap segments), %s" % (
        len(r), len(owners), sum(1 for o in owners if o < 0), dict(Counter(e[2] for e in r))))
    print("  address range 0x%X..0x%X" % (min(e[0] for e in r), max(e[0] for e in r)))
    # simplest accumulator patterns: fld dt; fadd [x]; fstp [x] | fld [x]; fadd dt; fstp [x] | fld [x]; fsub dt; ...
    idx = index()
    pos = {e[0]: k for k, e in enumerate(idx)}
    acc, sub = Counter(), Counter()
    for e in r:
        k = pos[e[0]]
        s = " | ".join("%s %s" % (f[2], f[3]) for f in idx[k - 1:k + 3])
        m = (re.search(r"fld dword ptr \[0x2d6748\] \| fadd dword ptr \[([^\]]+)\] \| fstp dword ptr \[\1\]", s) or
             re.search(r"fld dword ptr \[([^\]]+)\] \| fadd dword ptr \[0x2d6748\] \| fstp dword ptr \[\1\]", s))
        if m:
            acc[re.sub(r"e[a-ds][xip]", "R", m.group(1))] += 1
        m = re.search(r"fld dword ptr \[([^\]]+)\] \| fsub dword ptr \[0x2d6748\] \| fstp dword ptr \[\1\]", s)
        if m:
            sub[re.sub(r"e[a-ds][xip]", "R", m.group(1))] += 1
    print("  accumulators x += dt: %d sites, %d field offsets: %s" % (sum(acc.values()), len(acc),
                                                                     dict(acc.most_common(12))))
    print("  count-downs  x -= dt: %d sites, %d field offsets" % (sum(sub.values()), len(sub)))
    nicalls = [e for e in idx if e[2] == "call" and e[3] == "0x101840"]
    print("  NiAVObject::Update FUN_00101840: %d call sites in %d functions/segments" % (
        len(nicalls), len({e[5] for e in nicalls})))

    print("\n== 4. real frame delta [0x2E7B88] (Game::Frame 0xA2FD8: QPC time between two Render calls)")
    for e in mem_refs(G_FRAME_DELTA):
        print("  " + fmt(e))

    print("\n== 5. fps/dt readers reachable before Game::Init (callgraph from CApp::Init 0xD26A0, 0xA26A0 cut)")
    succ = defaultdict(set)
    for p in CALLG:
        if not p[2].startswith("EXTERNAL"):
            succ[int(p[0], 16)].add(int(p[2], 16))
    seen, st = set(), [0xD26A0]
    while st:
        f = st.pop()
        if f in seen or f == 0xA26A0:
            continue
        seen.add(f)
        st.extend(succ[f])
    for g, n in ((G_DT, "dt"), (G_FPS, "fps")):
        hit = sorted({owner_name(e[5]) for e in mem_refs(g) if e[5] in seen and kind(e, g) == "R"})
        print("  %s: %d functions reachable, readers among them: %s" % (n, len(seen), hit))
    print("  C++ initializers 0x17E010..0x233880 reading dt/fps: %d" % sum(
        1 for g in (G_DT, G_FPS) for e in mem_refs(g) if 0x17E010 <= e[0] < 0x233880))
    print("  .data initial values: fps %r, dt %r, fixed %d" % (f32(R.u32(G_FPS)), f32(R.u32(G_DT)),
                                                                R.read(G_FIXED, 1)[0]))

    print("\n== 6. pause object ctor FUN_000E4AA0: fps * 0.25 -> _ftol -> FUN_0009B2F0 (+0x50, repeat period)")
    for e in index():
        if 0xE4AA0 <= e[0] < 0xE4AC0 or 0x9B32C <= e[0] < 0x9B33B:
            print("  " + fmt(e))
    print("  [0x293AB0] = %r; trunc(50*0.25) = %d, trunc(60*0.25) = %d" % (f32(R.u32(0x293AB0)), int(12.5), 15))

    print("\n== 7. time argument of NiAVObject::Update FUN_00101840 (push just before the call; register traced"
          " back to its last mov within 12 instructions)")
    idx = index()
    src = Counter()
    for k, e in enumerate(idx):
        if e[2] != "call" or e[3] != "0x101840":
            continue
        t = None
        for j in range(k - 1, max(k - 12, 0), -1):
            f = idx[j]
            if f[5] != e[5] or f[2] in ("call", "ret", "jmp"):
                break
            if f[2] == "push":
                t = (j, f[3])
                break
        if t is None:
            src["?"] += 1
            continue
        j, op = t
        if op == "0":
            src["0 (static scene)"] += 1
            continue
        if re.fullmatch(r"e[a-ds][xip]", op):
            d = next((f for f in reversed(idx[max(j - 12, 0):j]) if f[5] == e[5] and f[2] == "mov" and
                      f[3].startswith(op + ",")), None)
            op = d[3].split(", ", 1)[1] if d else "reg ?"
        m = re.search(r"\[e[a-ds][xip] \+ (0x[0-9a-f]+)\]", op)
        if m and "esp" not in op:
            src["field +%s" % m.group(1)] += 1
        elif "esp" in op:
            src["stack argument"] += 1
        else:
            src[op if len(op) < 20 else "other"] += 1
    print("  " + ", ".join("%s: %d" % kv for kv in src.most_common()))

    print("\n== 8. tag reader: CApp::PlayMovie 0xA2380 (vtable slot 6)")
    for e in index():
        if 0xA2424 <= e[0] < 0xA2439:
            print("  " + fmt(e))
    print("  format 0x2A1838 = %r" % R.cstr(0x2A1838))


if __name__ == "__main__":
    main()
