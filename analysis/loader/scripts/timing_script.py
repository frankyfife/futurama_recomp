"""Script VM timing (analysis/loader/timing60.md, section 4).

1. Compares the compiled level scripts of the NTSC prototype (orig/proto/img/FutNTSC01, 60 Hz) with the EU retail
   PAL scripts extracted by timing_extract.py (run/timing60/pal/FutPAL01/scripts).
2. Walks the bytecode of every .ful (header "L2iM", 32-bit words, opcode = top byte, operand lengths from the
   interpreter FUN_000b97d0) and collects the operands of the wait opcode 0x18 (`pop -> thread+0x20; yield`)
   when the value comes from an immediate push (opcode 0x08), plus timer writes (opcode 0x03, `pop -> thread+0x1C`).
   The scheduler FUN_000ba060 subtracts its float argument (= dt [0x2D6748], call 0xC8DD5) from +0x20/+0x1C, so the
   operands are float seconds; the script prints how many decode as plausible float seconds vs. small integers.
3. Classifies loops (backward jumps 0x0F/0x10/0x11) by body content: Wait(0), short waits <= 0.05 s, timer reads
   (opcode 0x02) and float add/sub natives (class 0, ids 9/10 in FUN_000b0c40) with an immediate operand.
4. Tick quantization of Wait(t) at 50 and 60 Hz (float32 like FUN_000ba060).
The linear walk is a heuristic (data words between functions decode as noise; counted in section 2).
Usage: python timing_script.py   (needs timing_extract.py once)
"""
import glob
import hashlib
import os
import struct
from collections import Counter

PROTO = r"D:\FuturamaDecomp\orig\proto\img\FutNTSC01"
PAL = r"D:\FuturamaDecomp\run\timing60\pal\FutPAL01\scripts"

# opcode -> total length in 32-bit words (without inline data), from FUN_000b97d0 (retail EU)
LEN = {0x02: 1, 0x03: 1, 0x04: 3, 0x05: 2, 0x06: 2, 0x08: 2, 0x0A: 3, 0x0B: 2, 0x0C: 3, 0x0E: 2, 0x10: 2,
       0x11: 2, 0x12: 3, 0x13: 3, 0x14: 4, 0x15: 1, 0x16: 1, 0x17: 1, 0x18: 1, 0x19: 1, 0x1A: 2, 0x1B: 3}
# 0x07 / 0x09: 2 words + inline data of length word[1]; 0x0F: jump over word[1] bytes + 8


def f32(v):
    return struct.unpack("<f", struct.pack("<I", v))[0]


def proto_path(name):
    for p in (os.path.join(PROTO, "scripts", name), os.path.join(PROTO, name)):
        if os.path.exists(p):
            return p
    return None


def code_range(d):
    # header: "L2iM", u32 version(300), ... ; the interpreter starts at hdr+0x28+[hdr+0x1C] (FUN_000b9f10)
    # and the init entry at hdr+0x28+[hdr+0x20] (FUN_000b9f90); code runs to the end of the file.
    assert d[:4] == b"L2iM"
    return 0x28, len(d)


def walk(d):
    """Linear walk from the first code word; returns list of (offset, opcode, words). Stops at undecodable words."""
    start, end = code_range(d)
    out = []
    pc = start
    while pc + 4 <= end:
        w = struct.unpack_from("<I", d, pc)[0]
        op = w >> 24
        if op in (0x07, 0x09):
            n = struct.unpack_from("<I", d, pc + 4)[0]
            out.append((pc, op, [w, n]))
            pc += 8 + n
            continue
        if op == 0x0F:  # unconditional relative jump: pc = pc + 8 + word[1] (FUN_000b97d0)
            n = struct.unpack_from("<I", d, pc + 4)[0]
            out.append((pc, op, [w, n]))
            pc += 8  # linear walk: continue after the jump (the target is decoded in sequence anyway)
            continue
        if op not in LEN:
            out.append((pc, op, [w]))
            pc += 4
            continue
        k = LEN[op]
        ws = list(struct.unpack_from("<%dI" % k, d, pc)) if pc + 4 * k <= end else [w]
        out.append((pc, op, ws))
        pc += 4 * k
    return out


def main():
    names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(PAL, "*.ful")))
    if not names:
        print("no PAL scripts; run timing_extract.py first")
        return
    print("== 1. PAL (retail EU FutPAL01) vs. NTSC prototype (FutNTSC01)")
    same = 0
    for n in names:
        for ext in (".ful", ".dbg"):
            a = open(os.path.join(PAL, n[:-4] + ext), "rb").read()
            pp = proto_path(n[:-4] + ext)
            b = open(pp, "rb").read() if pp else None
            st = "missing" if b is None else ("identical" if a == b else "differs (%d vs %d bytes)" % (len(a), len(b)))
            if ext == ".ful" and a == b:
                same += 1
            print("  %-16s %s  sha1=%s" % (n[:-4] + ext, st, hashlib.sha1(a).hexdigest()[:12]))
    print("  identical .ful: %d of %d" % (same, len(names)))

    print("\n== 2. wait (0x18) and timer (0x03) operands from immediate pushes (0x08), all PAL scripts")
    waits, timers, unk = Counter(), Counter(), Counter()
    nwait = nwait_imm = 0
    for n in names:
        d = open(os.path.join(PAL, n), "rb").read()
        ins = walk(d)
        for i, (pc, op, ws) in enumerate(ins):
            if op in (0x18, 0x03):
                if op == 0x18:
                    nwait += 1
                prev = ins[i - 1] if i else None
                if prev and prev[1] == 0x08 and len(prev[2]) == 2:
                    v = prev[2][1]
                    if op == 0x18:
                        nwait_imm += 1
                        waits[round(f32(v), 4) if 0x30000000 < v < 0x48000000 or v == 0 else ("int", v)] += 1
                    else:
                        timers[round(f32(v), 4) if 0x30000000 < v < 0x48000000 or v == 0 else ("int", v)] += 1
            if op not in LEN and op not in (0x07, 0x09, 0x0F):
                unk[op] += 1
    print("  wait opcodes: %d, with immediate operand: %d" % (nwait, nwait_imm))
    print("  wait operands (float seconds unless marked int):", sorted(waits.items(), key=lambda kv: -kv[1])[:30])
    print("  wait operands that are not float seconds:", {k: v for k, v in waits.items() if isinstance(k, tuple)})
    print("  timer (0x03) operands:", sorted(timers.items(), key=lambda kv: -kv[1])[:20])
    print("  undecoded opcode words (linear walk noise):", dict(unk.most_common(8)))


def wait_ticks(t, dt):
    """Ticks until a thread with wait t runs again: FUN_000BA060 does w = f32(w - dt) each step and runs the
    thread when w <= 0; opcode 0x18 assigns (not adds) the next wait, so the remainder is dropped."""
    w = struct.unpack("<f", struct.pack("<f", t))[0]
    d = struct.unpack("<f", struct.pack("<f", dt))[0]
    n = 0
    while True:
        n += 1
        w = struct.unpack("<f", struct.pack("<f", w - d))[0]
        if w <= 0.0:
            return n


def wait_table():
    print("\n== 4. wait quantization (ticks / seconds until the thread resumes)")
    print("  %-8s %-18s %-18s %s" % ("Wait(t)", "50 Hz", "60 Hz", "change"))
    for t in (0.0, 0.02, 0.04, 0.05, 0.1, 0.2, 0.25, 0.3, 0.5, 0.98, 1.0, 1.5, 2.0, 3.0, 5.0, 10.0):
        a, b = wait_ticks(t, 0.02), wait_ticks(t, 1 / 60)
        print("  %-8g %3d = %7.4f s      %3d = %7.4f s      %+.1f ms" % (t, a, a * 0.02, b, b / 60,
                                                                       (b / 60 - a * 0.02) * 1000))


def loops(d):
    """Backward jumps (0x0F/0x10/0x11 with negative offset): body = [target, jump]. Classifies bodies:
    wait0  : contains Wait(0) (0x08 imm 0 ; 0x18)
    waitS  : contains a short immediate wait (0 < t <= 0.05 s)
    timer  : reads the thread timer (opcode 0x02, seconds, decremented by dt in FUN_000ba060)
    step   : float add/sub (class-0 natives 9/10, FUN_000b0c40) with an immediate float operand pushed just before
    """
    ins = walk(d)
    at = {pc: i for i, (pc, op, ws) in enumerate(ins)}
    res = []
    for i, (pc, op, ws) in enumerate(ins):
        if op in (0x0F, 0x10, 0x11) and len(ws) == 2 and ws[1] & 0x80000000:
            tgt = pc + 8 + ws[1] - (1 << 32)
            j = at.get(tgt)
            if j is None or j > i:
                continue
            body = ins[j:i + 1]
            f = {"wait0": False, "waitS": False, "timer": False, "step": False}
            for k in range(len(body)):
                p, o, w = body[k]
                if o == 0x18 and k and body[k - 1][1] == 0x08:
                    v = body[k - 1][2][1]
                    if v == 0:
                        f["wait0"] = True
                    elif 0 < f32(v) <= 0.0501:
                        f["waitS"] = True
                if o == 0x02:
                    f["timer"] = True
                if o == 0x13 and len(w) == 3 and w[1] == 0 and w[2] in (9, 10) and k and body[k - 1][1] == 0x08:
                    f["step"] = True
            res.append((pc, tgt, len(body), f))
    return res


def loop_report():
    names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(PAL, "*.ful")))
    tot = Counter()
    examples = []
    for n in names:
        d = open(os.path.join(PAL, n), "rb").read()
        for pc, tgt, size, f in loops(d):
            key = tuple(k for k in ("wait0", "waitS", "timer", "step") if f[k])
            tot[key] += 1
            if ("step" in key) and ("timer" not in key) and ("wait0" in key or "waitS" in key):
                examples.append((n, tgt, pc, size, key))
    print("\n== 3. loops (backward jumps) by body content, all PAL scripts")
    for k, v in sorted(tot.items(), key=lambda kv: -kv[1]):
        print("  %-32s %d" % ("+".join(k) or "(none)", v))
    print("  per-tick stepping candidates (wait<=0.05 s, immediate float add/sub, no timer read): %d" % len(examples))
    for e in examples[:40]:
        print("   %s loop %06X..%06X (%d ops) %s" % e)


if __name__ == "__main__":
    main()
    loop_report()
    wait_table()
