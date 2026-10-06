"""M3d half-tick frame clock: exact guest arithmetic + clock variants under measured jitter
(analysis/loader/m3d_frameclock.md 2).

1. Guest step threshold: Game::Frame steps while sec(elapsed) >= dt with sec(e) = fild(e) * f32(1/QPF) on the x87
   (0xA2D94 FUN_000D2B80, fcomp dt 0xA2D99) and subtracts ticks(dt) = trunc(QPF * dt) (FUN_000D2B50). The first
   section computes, for several QPF values, ticks(dt), the smallest elapsed that gives one step (53- and 64-bit
   x87 precision), the remainder after a 6 dt cap (0xA2CE8..0xA2D1F) and the halves q_a/q_b.
2. Clock variants for 120 frames/s on a 60 Hz tick (2 frames per tick), simulated with the guest loop:
     minimal   : M3b rule (LockstepClock::Advance) in quanta: nominal +1 quantum, corrections to just inside 3/4 q
     recenter  : corrections land on |real - V| <= q/2 (round instead of minimal)
     +align A/B: keep the guest remainder at phi (mod q): A phi = mu (alpha ~0 / ~0.5), B phi = q - mu (~0.5 / ~1)
   Real time = frame starts at 1/120 s with the measured jitter of the game's accumulator sample on the 120 Hz
   test machine (m3d_clock_trace.out: 3 % of the frames start 4.3 ms early/late as a bimodal phase, sample delay
   p99.9 1 ms), plus events: a missed vblank every ~7 s, a context switch (8 empty frames + 40 ms Enter) every
   20 s, a 300 ms stall every 60 s. Reported: corrections, pattern breaks, alpha set, shown-time increments.
"""
import math
import os
import random
import struct
from fractions import Fraction

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "m3d_clock_sim.out")
lines = []


def p(*a):
    s = " ".join(str(x) for x in a)
    print(s)
    lines.append(s)


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def ext_round(fr: Fraction, bits: int) -> Fraction:
    """Round a positive rational to `bits` significant bits (x87 precision control 53/64), nearest-even."""
    if fr == 0:
        return fr
    e = math.floor(math.log2(fr))
    # correct e for floating error of log2
    while Fraction(2) ** e > fr:
        e -= 1
    while Fraction(2) ** (e + 1) <= fr:
        e += 1
    scale = Fraction(2) ** (bits - 1 - e)
    v = fr * scale
    n = math.floor(v)
    rem = v - n
    if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and n % 2 == 1):
        n += 1
    return Fraction(n) / scale


DT = Fraction(f32(1.0 / 60.0))  # 0x3C888889


def guest_threshold(qpf, bits):
    inv = Fraction(f32(1.0 / qpf))
    step = math.floor(qpf * DT)  # FUN_000D2B50: fild qword QPF * float dt, _ftol (trunc); exact enough here
    e = step - 2
    while ext_round(e * inv, bits) < DT:
        e += 1
    return step, e


p("## 1. guest step arithmetic (dt = f32(1/60) = %.10f)" % float(DT))
p("QPF        ticks(dt)  min elapsed for a step (53 bit / 64 bit)  q_a/q_b   cap remainder ticks(6dt)-6*ticks(dt)")
for qpf in (10_000_000, 3_579_545, 2_929_687, 24_000_000, 733_333_333):
    step, t53 = guest_threshold(qpf, 53)
    _, t64 = guest_threshold(qpf, 64)
    six = Fraction(f32(float(DT) * 6.0)) if False else ext_round(DT * 6, 64)  # fld dt; fmul 6.0f -> x87
    cap = math.floor(qpf * six) - 6 * step
    p("%-10d %-10d %-20d %-20d %d/%d   %d" % (qpf, step, t53, t64, step // 2, step - step // 2, cap))
p()

# ---- 2. clock variants ------------------------------------------------------------------------------------------
QPF = 10_000_000
STEP, THRESH = guest_threshold(QPF, 53)
CAPT = math.floor(QPF * ext_round(DT * 6, 64))


class Guest:
    """Game::Frame accumulator: last (CApp+0xC8), steps per sample."""

    def __init__(self, last):
        self.last = last

    def frame(self, now):
        elapsed = now - self.last
        if elapsed > CAPT:  # sec(elapsed) > 6 dt (approximately; exact compare irrelevant here)
            elapsed = CAPT
        n = 0
        while elapsed >= THRESH and n < 100:
            elapsed -= STEP
            n += 1
        self.last = now - elapsed
        return n, elapsed


class Clock:
    def __init__(self, recenter, align, phi_mode):
        self.recenter = recenter
        self.align = align
        self.phi_mode = phi_mode
        self.started = False
        self.v = 0
        self.parity = 0
        self.corrections = 0
        self.aligns = 0
        self.qa, self.qb = STEP // 2, STEP - STEP // 2

    def quantum(self):
        q = self.qb if self.parity else self.qa
        self.parity ^= 1
        return q

    def advance(self, real, last_prev):
        if not self.started:
            self.started = True
            self.v = real
            return self.v
        q = self.qa
        m = q * 3 // 4
        v = self.v + self.quantum()
        d = real - v
        if abs(d) > m:
            self.corrections += 1
            if self.recenter:
                n = round(d / q)
            else:
                n = math.ceil((d - m) / q) if d > 0 else -math.ceil((-d - m) / q)
            n = max(n, -1)  # never behind the previous frame (at most a hold)
            for _ in range(abs(n)):
                v += self.quantum() if n > 0 else -(self.qa if self.parity else self.qb)
                if n < 0:
                    self.parity ^= 1
        if self.align:
            mu = max(16, STEP // 1024)
            phi = mu if self.phi_mode == "A" else q - mu
            el = v - last_prev
            if 0 < el < 5 * STEP:
                delta = (phi - el) % q
                if delta > q / 2:
                    delta -= q
                if abs(delta) > mu / 2:
                    # choose among delta, delta +- q the one nearest to real (keeps |real - V| <= q/2)
                    cands = [delta, delta - q, delta + q]
                    delta = min(cands, key=lambda c: abs(real - (v + c)))
                    if v + delta < self.v:
                        delta += q
                    v += delta
                    self.aligns += 1
        self.v = v
        return v


def simulate(recenter, align, phi_mode, seconds=600, seed=1):
    rnd = random.Random(seed)
    clock = Clock(recenter, align, phi_mode)
    period = QPF / 120.0
    t = 1_000_000_000.0
    guest = Guest(int(t) - 3_000_000)
    stats = dict(frames=0, breaks=0, alphas={}, inc={}, back=0, maxph=0.0, minph=0.0)
    prev_steps = None
    shown_prev = None
    total = 0
    mode_off = 0.0
    next_ctx, next_stall, next_miss = 20.0, 60.0, 7.0
    for i in range(int(seconds * 120)):
        sec = i / 120.0
        extra = 0.0
        if sec >= next_miss:
            extra += period
            next_miss += 7.0
        if sec >= next_ctx:
            extra += 8 * period + 0.040 * QPF
            next_ctx += 20.0
        if sec >= next_stall:
            extra += 0.300 * QPF
            next_stall += 60.0
        t += period + extra
        # bimodal frame start phase (measured: 97 % at the main phase, 3 % 4.3 ms earlier) + sample delay
        if rnd.random() < 0.03:
            mode_off = -0.0043 * QPF
        else:
            mode_off = 0.0
        delay = rnd.expovariate(1 / 7e-6) * QPF if rnd.random() < 0.999 else rnd.uniform(0, 0.001) * QPF
        real = int(t + mode_off + delay)
        last_prev = guest.last
        v = clock.advance(real, last_prev)
        steps, left = guest.frame(v)
        ph = (real - v) / QPF * 1000
        stats["maxph"] = max(stats["maxph"], ph)
        stats["minph"] = min(stats["minph"], ph)
        if prev_steps is not None and steps == prev_steps:
            stats["breaks"] += 1
        prev_steps = steps
        a = round(left / STEP, 3)
        stats["alphas"][a] = stats["alphas"].get(a, 0) + 1
        total += steps
        shown = (total - 1) * STEP + left
        if shown_prev is not None:
            dlt = shown - shown_prev
            if dlt < 0:
                stats["back"] += 1
            stats["inc"][dlt] = stats["inc"].get(dlt, 0) + 1
        shown_prev = shown
        stats["frames"] += 1
    return clock, stats


p("## 2. clock variants, 600 s at 120 frames/s, QPF 10 MHz, ticks(dt) %d, step threshold %d, cap %d" % (
    STEP, THRESH, CAPT))
p("variant              corrections aligns breaks  phase real-V min/max ms   alpha (rounded, top 3)        "
  "shown-time increments (top 3)            backwards")
for name, rc, al, ph in (("minimal", False, False, "A"), ("recenter", True, False, "A"),
                         ("recenter+align A", True, True, "A"), ("recenter+align B", True, True, "B"),
                         ("minimal+align B", False, True, "B")):
    c, s = simulate(rc, al, ph)
    top = sorted(s["alphas"].items(), key=lambda kv: -kv[1])[:3]
    inc = sorted(s["inc"].items(), key=lambda kv: -kv[1])[:3]
    p("%-20s %-11d %-6d %-7d %8.2f / %-8.2f   %-30s %-40s %d" % (
        name, c.corrections, c.aligns, s["breaks"], s["minph"], s["maxph"],
        ", ".join("%.3f x%d" % kv for kv in top), ", ".join("%d x%d" % kv for kv in inc), s["back"]))

open(OUT, "w", encoding="utf-8").write("\n".join(lines) + "\n")
