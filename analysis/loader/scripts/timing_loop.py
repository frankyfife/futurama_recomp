"""Model of the fixed-step loop of Game::Frame FUN_000A2C20 at 50 and 60 Hz (timing60.md, section 6).

Arithmetic as in the binary (startup.md 3.2, disassembly 0xA2C20..0xA2F73):
  now = QPC (FUN_000D2B90); elapsed = now - last (+0xC8), 64-bit ticks
  sec(t) = t * f32(1/QPF)                      FUN_000D2B80 (fild qword; fmul dword [0x2EB450])
  cap  : if sec(elapsed) > f32(6*dt): elapsed = trunc(QPF * f32(6*dt))   0xA2CE8..0xA2D1F, FUN_000D2B10 (_ftol)
  loop : edi = 100; while sec(elapsed) >= dt and edi > 0: step; elapsed -= trunc(QPF*dt)  0xA2D84..0xA2F22
  end  : last = now - elapsed (debt carried over)                                    0xA2F55..0xA2F73
The x87 is modelled with doubles (53-bit precision control, Windows default); float32 values are rounded via
struct. Present times: ideal VSync slots of the given display rate plus an optional pacer (frame_pacer.h).
Prints steps per presented frame, the long-run step rate and the share of frames with 0 or >= 2 steps.
Usage: python timing_loop.py
"""
import random
import struct
from collections import Counter


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def trunc(x):
    return int(x)  # _ftol: toward zero (0x167080)


def simulate(dt_f, qpf, present_times, phase=0.0):
    """phase: initial debt as a fraction of one step (the real start phase is arbitrary)."""
    inv = f32(1.0 / qpf)
    dt = f32(dt_f)
    cap = f32(6.0 * dt)
    step_ticks = trunc(qpf * dt)
    cap_ticks = trunc(qpf * cap)
    last = present_times[0] - int(phase * step_ticks)
    steps_hist = Counter()
    total = 0
    for now in present_times[1:]:
        elapsed = now - last
        if elapsed * inv > cap:
            elapsed = cap_ticks
        n = 0
        edi = 100
        while elapsed * inv >= dt and edi > 0:
            edi -= 1
            n += 1
            elapsed -= step_ticks
        last = now - elapsed
        steps_hist[n] += 1
        total += n
    span = (present_times[-1] - present_times[0]) / qpf
    return steps_hist, total / span, step_ticks


def present_times(display_hz, frames, qpf, jitter_ms, interval=1, pace_hz=0, seed=1):
    """QPC time stamps at which Game::Frame samples the clock, one per presented frame.
    interval: VSync sync interval (Present(interval)): a frame is shown every `interval` vblanks.
    pace_hz : CPU timer pacer (frame_pacer.h) releasing frames every 1/pace_hz s; the frame then waits for
              the next vblank (VSync 1), so the shown intervals snap to the vblank grid.
    jitter  : Gaussian jitter of the clock sample relative to the vblank (thread wake-up, ~0.1..0.5 ms)."""
    rnd = random.Random(seed)
    vb = 1.0 / display_hz
    out = []
    if pace_hz:
        for k in range(frames):
            slot = k / pace_hz
            v = (int(slot / vb + 1e-9) + 1) * vb      # first vblank after the slot
            out.append(int((v + rnd.gauss(0.0, jitter_ms / 1000.0)) * qpf))
    else:
        for k in range(frames):
            out.append(int((k * interval * vb + rnd.gauss(0.0, jitter_ms / 1000.0)) * qpf))
    return out


def main():
    qpf = 10_000_000
    print("QPF %d: ticks(dt) = trunc(QPF*dt): 50 Hz %d (%.5f Hz), 60 Hz %d (%.5f Hz)" % (
        qpf, trunc(qpf * f32(0.02)), qpf / trunc(qpf * f32(0.02)),
        trunc(qpf * f32(1 / 60)), qpf / trunc(qpf * f32(1 / 60))))
    print("Xbox QPF 733333333: 50 Hz %d ticks, 60 Hz %d ticks" % (trunc(733333333 * f32(0.02)),
                                                                trunc(733333333 * f32(1 / 60))))
    print("cap 6*dt: 50 Hz %.4f s, 60 Hz %.4f s\n" % (f32(6 * f32(0.02)), f32(6 * f32(1 / 60))))
    cases = [
        ("120 Hz display, CPU pacer 50 Hz + VSync (today)", 120, 1, 50, 0.02),
        ("120 Hz display, sync interval 2 (60 Hz)", 120, 2, 0, 1 / 60),
        ("60 Hz display, sync interval 1", 60, 1, 0, 1 / 60),
        ("120 Hz display, sync interval 1 (120 fps)", 120, 1, 0, 1 / 60),
        ("144 Hz display, sync interval 1 (144 fps)", 144, 1, 0, 1 / 60),
        ("144 Hz display, CPU pacer 60 Hz + VSync", 144, 1, 60, 1 / 60),
    ]
    for label, disp, interval, pace, dtv in cases:
        for jit, phase in ((0.3, 0.0), (0.3, 0.5)):
            pts = present_times(disp, 20000, qpf, jit, interval, pace)
            h, rate, st = simulate(dtv, qpf, pts, phase)
            n = sum(h.values())
            span = (pts[-1] - pts[0]) / qpf
            print("%-48s phase %.1f: present %.2f Hz, steps %.3f Hz, steps/frame %s, 0-step %.1f %%, "
                  ">=2-step %.1f %%" % (label, phase, (len(pts) - 1) / span, rate, dict(sorted(h.items())),
                                       100.0 * h[0] / n, 100.0 * sum(v for k, v in h.items() if k >= 2) / n))


if __name__ == "__main__":
    main()
