"""Review M3b (phase 5): evidence for the corrections of analysis/loader/timing60.md section 12.

Checks the review findings on tick-bound game code that the first pass of timing60.md missed or misread:
  1. Rigid-body damping FUN_000AFA30 (VTable 0x2A1F28 slot 5): momentum x0.98 / x0.9 and angular momentum x0.9
     once per call without dt, called once per fixed step through the physics world (FUN_000C8BA0 0xC8D25/0xC8D30
     -> world vf2 0xAE7E0 -> body slot 5 at 0xAE80F).
  2. Chicken Walker aim/turn step: ctor FUN_00043D80 stores 0.02f at +0x64 (0x43EEC), FUN_000445A0 integrates
     the stick with that fixed step (0x44BA5 / 0x44BB2).
  3. Camera spring FUN_00015990: fixed 0.01 s sub-steps `do { ...; t -= 0.01 } while (t > 0)` (0x179B3..0x179C4),
     called with dt from FUN_00019C80 (0x19CC2); overlay fade FUN_000A8490 (+0x28 +-0.03 per update up to 0.75).
     Rejected part: the "pause menu input lock" +0x84 in FUN_000A55D0 / FUN_000A62B0 is an element count.
  4. Level scripts: every unconditional loop back-jump (opcode 0x0F) follows a compiler-inserted `Wait(0)`
     (push_imm 0; WAIT), so one iteration costs q(t) + 1 ticks; table of the loop periods at 50 and 60 Hz and a
     classification of the loops by explicit waits, timer reads, step updates (immediate or variable operand)
     and calls of non-arithmetic natives.
  5. Retail vs. NTSC prototype for the functions of 1-3 (relocated byte patterns, .rdata values; timing_proto).
Needs timing_extract.py once (run/timing60/pal). Usage: python timing_review.py > timing_review.out
"""
import bisect
import glob
import os
import re
import struct
from collections import Counter

from timing_common import R, callers, f32, func_of, index, insns, mem_refs
from timing_proto import P, pattern, rdata_compare, text_of

PAL = r"D:\FuturamaDecomp\run\timing60\pal\FutPAL01\scripts"


def dis(a, n, title, stop=None):
    print("  ---- %s" % title)
    for i in insns(R, a, n):
        extra = ""
        m = re.search(r"\[(0x[0-9a-f]+)\]", i.op_str)
        if m:
            t = int(m.group(1), 16)
            b = R.read(t, 4)
            if b and R.section(t) in (".rdata", ".data"):
                extra = "   ; [%#x] = %r" % (t, f32(struct.unpack("<I", b)[0]))
        print("  %08X %-22s %s %s%s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str, extra))
        if stop is not None and i.address >= stop:
            break


def users(addr):
    refs = mem_refs(addr)
    owners = sorted({e[5] for e in refs})
    return "%d instructions in %d functions/segments" % (len(refs), len(owners))


def half_life_ticks(factor):
    import math
    return math.log(0.5) / math.log(factor)


def callers_of(callee):
    return ["FUN_%08X" % a for a in callers(callee)]


def section1():
    print("== 1. rigid-body damping FUN_000AFA30 (VTable 0x2A1F28 slot 5)")
    slots = [struct.unpack("<I", R.read(0x2A1F28 + 4 * i, 4))[0] for i in range(8)]
    print("  VTable 0x2A1F28 slots 0..7: %s" % ", ".join("0x%X" % s for s in slots))
    dis(0xAFB07, 0xAFBEF - 0xAFB07, "damping block 0xAFB07..0xAFBEE (no dt operand; the time argument [ebp+8] goes "
        "only to FUN_000AF770 at 0xAFBE5)")
    print("  users of 0.9f @0x293B68: %s; of 0.98f @0x293B74: %s (shared .rdata: never patch the value)" % (
        users(0x293B68), users(0x293B74)))
    dis(0xC8D19, 0x1A, "FUN_000C8BA0 (session update, once per fixed step): world [0x2E99DC] vf1 then vf2")
    dis(0xAE7E0, 0x54, "world vf2 0xAE7E0: for each body of [0x2E9BA0] call slot 5 (0xAE80F) with +0x24, solver "
        "0xAE0D0, +0x24 = 0", stop=0xAE833)
    print("  body ctor FUN_000AF920 is called from: %s" % ", ".join(callers_of(0xAF920)))
    for f in (0.98, 0.9):
        h = half_life_ticks(f)
        print("  x%.2f per tick: half-life %.1f ticks = %.3f s at 50 Hz, %.3f s at 60 Hz" % (f, h, h / 50, h / 60))
    print("  coasting distance from v0 (sum of v*dt, x0.98): %.3f s*v0 at 50 Hz, %.3f s*v0 at 60 Hz; terminal "
          "velocity under a constant acceleration a: %.3f*a vs %.3f*a (-%.0f %%)" % (
              0.98 * 0.02 / 0.02, 0.98 / 60 / 0.02, 0.98 * 0.02 / 0.02, 0.98 / 60 / 0.02,
              100 * (1 - (0.98 / 60 / 0.02) / 0.98)))
    print("  PAL parity would need own constants 0.9^(50/60) = %.5f and 0.98^(50/60) = %.5f for the nine fmul "
          "operands 0xAFB0D..0xAFBCB" % (0.9 ** (50 / 60), 0.98 ** (50 / 60)))
    print()


def section2():
    print("== 2. Chicken Walker aim/turn step (FUN_00043D80 / FUN_000445A0)")
    idx = index()
    w = [e for e in idx if e[5] in (0x43D80, 0x445A0) and re.search(r"\[e\w\w \+ 0x64\]", e[3])]
    for e in w:
        print("  %08X %-6s %-32s (%s)" % (e[0], e[2], e[3], "ctor" if e[5] == 0x43D80 else "update"))
    print("  0x43EEC imm32 0x3CA3D70A = %r (the PAL tick length)" % f32(0x3CA3D70A))
    dis(0x44B96, 0x44BD5 - 0x44B96, "FUN_000445A0: pitch +0x50 -= stickY*k*[+0x64] (clamp +-0.7), yaw +0x54 += "
        "stickX*[+0x64]")
    dt_users = [hex(e[0]) for e in idx if e[5] == 0x445A0 and "0x2d6748" in e[3]]
    print("  FUN_000445A0 reads dt at %s (timers -= dt: one call per fixed step); caller FUN_00057940 (0x5805F)" %
          ", ".join(dt_users))
    print("  full stick: %.2f rad/s at 50 Hz, %.2f rad/s at 60 Hz; auto-turn clamp 0.05 per update: %.1f vs %.1f "
          "rad/s" % (50 * 0.02, 60 * 0.02, 50 * 0.05, 60 * 0.05))
    print()


def section3():
    print("== 3. camera spring sub-steps FUN_00015990, overlay fade FUN_000A8490, rejected pause-menu lock")
    dis(0x19CC2, 0xE, "FUN_00019C80: camera mode 0x15990 called with dt")
    dis(0x179B3, 0x179CA - 0x179B3, "FUN_00015990 loop end: t -= 0.01f; while (t > 0) -> loop head 0x17720")
    for name, dt in (("0.02f", 0x3CA3D70A), ("1/60f", 0x3C888889)):
        t, n = f32(dt), 0
        step = f32(0x3C23D70A)
        while True:
            n += 1
            t = struct.unpack("<f", struct.pack("<f", t - step))[0]
            if not t > 0.0:
                break
        print("  dt %s: %d sub-steps per tick (float32 model; 0.02f is exactly 2 x 0.01f)" % (name, n))
    print("  -> 100 sub-steps/s at 50 Hz, 120 at 60 Hz: the spring runs 20 % faster")
    dis(0xA84A6, 0xA84E4 - 0xA84A6, "FUN_000A8490 fade in: +0x28 += 0.03 per update, clamp 0.75 (25 ticks: 0.50 s "
        "-> 0.42 s)")
    print("  rejected: 'pause menu input lock +0x84 = 30' - the only writer of +0x84 = 30 is the frontend 0x21AEB;")
    for a in (0xA5640, 0xA64E4):
        print("    0x%X in %s: dec [esi+0x84] inside the element-removal loop (+0x78 array, +0x7C capacity, +0x80 "
              "size) - an element count, not a tick counter" % (a, func_of(a)[2] if func_of(a) else "?"))
    print()


# ---- level scripts ---------------------------------------------------------------------------------------------

LEN = {0x02: 1, 0x03: 1, 0x04: 3, 0x05: 2, 0x06: 2, 0x08: 2, 0x0A: 3, 0x0B: 2, 0x0C: 3, 0x0E: 2, 0x10: 2,
       0x11: 2, 0x12: 3, 0x13: 3, 0x14: 4, 0x15: 1, 0x16: 1, 0x17: 1, 0x18: 1, 0x19: 1, 0x1A: 2, 0x1B: 3}
# class-0 natives of FUN_000B0C40 used as plain arithmetic / comparison (fadd 9, fsub 10, fdiv 11, fmul 12,
# integer and mixed variants; comparisons) - everything else is an engine call
ARITH = {9: "fadd", 10: "fsub", 11: "fdiv", 12: "fmul", 23: "iadd", 24: "isub", 25: "idiv", 26: "imul",
         36: "fiadd", 37: "fisub", 38: "fidiv", 39: "fimul"}
PURE = set(ARITH) | set(range(13, 19)) | set(range(27, 35)) | set(range(40, 46)) | {
    4, 5, 6, 7, 8, 19, 20, 21, 22, 35, 176, 177, 178, 179}


def decode(d, start, end):
    pc, out = start, []
    while pc + 4 <= end:
        w = struct.unpack_from("<I", d, pc)[0]
        op = w >> 24
        if op in (0x07, 0x09):
            n = struct.unpack_from("<I", d, pc + 4)[0]
            out.append((pc, op, [w, n]))
            pc += 8 + n
            continue
        if op == 0x0F:
            out.append((pc, op, [w, struct.unpack_from("<I", d, pc + 4)[0]]))
            pc += 8
            continue
        k = LEN.get(op)
        if k is None:
            out.append((pc, op, [w]))
            pc += 4
            continue
        out.append((pc, op, list(struct.unpack_from("<%dI" % k, d, pc))))
        pc += 4 * k
    return out


def dbg_functions(path):
    """(code offset, name) of the script functions from the .dbg symbols (name, 128-byte source file, code)."""
    d = open(path, "rb").read()
    found = {}
    for m in re.finditer(rb"([A-Za-z_@][\x20-\x7e]{1,126})\x00", d):
        f = m.start() + 128
        if f + 140 > len(d):
            continue
        src = d[f:f + 128].split(b"\0")[0]
        if not re.match(rb"^[\x20-\x7e]+\.(fis|fih)$", src):
            continue
        code = struct.unpack_from("<I", d, f + 128)[0]
        found.setdefault(code + 0x28, m.group(1).decode())
    return sorted(found.items())


def q(t, dt):
    """Ticks of Wait(t): the scheduler FUN_000BA060 subtracts dt (float32) until the wait is <= 0."""
    w = struct.unpack("<f", struct.pack("<f", t))[0]
    d = struct.unpack("<f", struct.pack("<f", dt))[0]
    n = 0
    while True:
        n += 1
        w = struct.unpack("<f", struct.pack("<f", w - d))[0]
        if w <= 0:
            return n


def loops_of(path):
    d = open(path, "rb").read()
    ins = decode(d, 0x28, len(d))
    at = {e[0]: i for i, e in enumerate(ins)}
    fl = dbg_functions(path[:-4] + ".dbg")
    starts = [f[0] for f in fl]
    res = []
    for i, (pc, op, ws) in enumerate(ins):
        if op not in (0x0F, 0x10, 0x11) or len(ws) != 2 or not ws[1] & 0x80000000:
            continue
        tgt = pc + 8 + ws[1] - (1 << 32)
        j = at.get(tgt)
        if j is None or j > i:
            continue
        body = ins[j:i + 1]
        waits, timer, steps, natives = [], False, [], Counter()
        for k, (p, o, w) in enumerate(body):
            if o == 0x18:
                pv = body[k - 1]
                if pv[1] == 0x08:
                    waits.append(round(f32(pv[2][1]), 4) if pv[2][1] else 0.0)
                else:
                    waits.append("var" if pv[1] == 0x05 else "expr")
            elif o == 0x02:
                timer = True
            elif o == 0x13 and w[1] == 0:
                natives[w[2]] += 1
                if w[2] in ARITH and k + 1 < len(body) and body[k + 1][1] == 0x0B and k >= 2:
                    dst = (body[k + 1][2][0] & 0xFF, body[k + 1][2][1])
                    srcs = []
                    for qq in (body[k - 2], body[k - 1]):
                        srcs.append(("var", (qq[2][0] & 0xFF, qq[2][1])) if qq[1] == 0x05 else
                                    ("imm", qq[2][1]) if qq[1] == 0x08 else ("expr", None))
                    if any(s[0] == "var" and s[1] == dst for s in srcs):
                        other = [s for s in srcs if not (s[0] == "var" and s[1] == dst)]
                        steps.append((ARITH[w[2]], other[0][0] if other else "?"))
            elif o == 0x13:
                natives[(w[1], w[2])] += 1
        fi = bisect.bisect_right(starts, tgt) - 1
        res.append(dict(op=op, waits=waits, timer=timer, steps=steps,
                        engine=sum(c for n, c in natives.items() if n not in PURE),
                        func=fl[fi][1] if fi >= 0 else "?",
                        trailing_wait0=i >= 2 and ins[i - 1][1] == 0x18 and ins[i - 2][1] == 0x08 and ins[i - 2][2][1] == 0))
    return res


def section4():
    print("== 4. level scripts: loop back-jumps and the compiler-inserted Wait(0)")
    files = sorted(glob.glob(os.path.join(PAL, "*.ful")))
    if not files:
        print("  no scripts under %s - run timing_extract.py first" % PAL)
        return
    loops = []
    for p in files:
        loops += loops_of(p)
    unc = [l for l in loops if l["op"] == 0x0F]
    print("  %d .ful files, %d backward jumps: %d unconditional (0x0F), %d conditional (0x10/0x11)" % (
        len(files), len(loops), len(unc), len(loops) - len(unc)))
    print("  unconditional back-jumps directly after 'push_imm 0; WAIT': %d of %d" % (
        sum(1 for l in unc if l["trailing_wait0"]), len(unc)))
    print("  -> one iteration of a loop with Wait(t) costs q(t) + 1 ticks (q = ceil(t/dt) in float32, at least 1)")
    print()
    print("  loop period per iteration, loops with exactly one constant explicit wait and no timer read")
    rows, funcs = Counter(), {}
    for l in loops:
        ex = [w for w in l["waits"] if w != 0.0]
        if len(ex) == 1 and isinstance(ex[0], float) and not l["timer"]:
            z = sum(1 for w in l["waits"] if w == 0.0)
            rows[(ex[0], z)] += 1
            funcs.setdefault((ex[0], z), Counter())[l["func"]] += 1
    print("  %-22s %6s %11s %17s %8s  examples" % ("waits per iteration", "loops", "ticks 50|60", "seconds 50|60",
                                                    "change"))
    for (t, z), c in sorted(rows.items()):
        a, b = q(t, 0.02) + z, q(t, 1 / 60) + z
        print("  Wait(%g) + %d x Wait(0)%s %6d %5d|%-5d %8.4f|%-8.4f %+7.1f %%  %s" % (
            t, z, " " * max(0, 8 - len("%g" % t)), c, a, b, a * 0.02, b / 60, (b / 60 / (a * 0.02) - 1) * 100,
            ", ".join("%s %d" % kv for kv in funcs[(t, z)].most_common(4))))
    print()
    print("  classification of all %d loops" % len(loops))
    cls = Counter()
    for l in loops:
        ex = [w for w in l["waits"] if w != 0.0]
        wait = "timer read" if l["timer"] else ("no explicit wait" if not ex else
                                                ("variable wait" if any(not isinstance(w, float) for w in ex) else
                                                 ("explicit wait <= 0.1 s" if max(ex) <= 0.1 else "explicit wait > 0.1 s")))
        step = "step imm" if any(s[1] == "imm" for s in l["steps"]) else (
            "step var/expr" if l["steps"] else "no step")
        eng = "engine calls" if l["engine"] else "no engine call"
        cls[(wait, step, eng)] += 1
    for k, c in sorted(cls.items(), key=lambda kv: -kv[1]):
        print("  %5d  %-24s %-14s %s" % (c, *k))
    tickbound = [l for l in loops if not l["timer"] and all(w == 0.0 for w in l["waits"])
                 and (l["steps"] or l["engine"])]
    print("  loops that advance something once per tick (only Wait(0), no timer read, a step update or an engine "
          "call): %d; most frequent functions: %s" % (
              len(tickbound), ", ".join("%s %d" % kv for kv in Counter(l["func"] for l in tickbound).most_common(10))))
    print()


def section5():
    print("== 5. retail EU vs. NTSC prototype (relocated byte patterns, timing_proto.pattern)")
    pva, ptext = text_of(P)
    for start, size, label in ((0xAFA30, 0x1C1, "rigid-body slot 5 (damping)"), (0xAE7E0, 0x54, "world vf2"),
                               (0x43D80, None, "Chicken Walker ctor"), (0x445A0, None, "Chicken Walker update"),
                               (0x15990, None, "camera mode (0.01 s sub-steps)"), (0xA8490, None, "overlay fade")):
        if size is None:
            f = func_of(start)
            size = f[1] if f else 0x100
        pat, n = pattern(R, start, size)
        hits = [m.start() + pva for m in re.finditer(pat, ptext, re.S)]
        st = ("identical (relocated) at proto 0x%X, .rdata %s" % (hits[0], rdata_compare(start, hits[0], n))
              if len(hits) == 1 else ("%d matches" % len(hits) if hits else "NOT identical"))
        print("  0x%06X %-5d %-34s %s" % (start, n, label, st))


def main():
    section1()
    section2()
    section3()
    section4()
    section5()


if __name__ == "__main__":
    main()
