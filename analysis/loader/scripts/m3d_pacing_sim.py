"""M3d pacing options for refresh rates up to 240 Hz (analysis/loader/m3d_frameclock.md 3).

For each display refresh R and each presentation option the script simulates 2 s of frames and reports
  fps            distinct pictures per second
  hold (ms)      how long each picture stays on screen (min/max; equal = no persistence judder)
  pos p-p (ms)   positional judder: peak-to-peak of (content time of the picture - time its scanout starts),
                 i.e. how far the shown motion is off the even motion of a tracking eye (0 = perfect)
Options:
  vblank k       Present(k) on every k-th vblank, content time advances by k/R per picture (uniform)
  cadence 120    one picture per Bresenham slot for 120 fps on a fixed R (1 or 2 refreshes), content time
                 advances by a constant 1/120 s ("fixed quantum") or by the refreshes the picture stays
                 ("scanout aware": the clock adds s_n/R)
  vrr 120        timer at 120 Hz, the panel follows (VRR range >= 120), content 1/120 s per picture
  60 Hz path     M3b behaviour for comparison (vblank k = R/60, or display-rate Present(1) with 60 ticks/s
                 without interpolation: pictures repeat irregularly)
"""
import math
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "m3d_pacing_sim.out")


def stats(frames):
    """frames: list of (scanout_start_s, hold_s, content_s)."""
    holds = [h for _, h, _ in frames]
    off = [c - s for s, _, c in frames]
    m = sum(off) / len(off)
    pp = (max(off) - min(off)) * 1000
    return len(frames) / (frames[-1][0] + frames[-1][1] - frames[0][0]), min(holds) * 1000, max(holds) * 1000, pp


def vblank(R, k, secs=2.0):
    P = 1.0 / R
    n = int(secs * R / k)
    return [(i * k * P, k * P, i * k * P) for i in range(n)]


def cadence(R, target, aware, secs=2.0):
    P = 1.0 / R
    frames = []
    t_ref = 0
    i = 0
    content = 0.0
    while t_ref * P < secs:
        a = math.floor(i * R / target + 0.5)
        b = math.floor((i + 1) * R / target + 0.5)
        s = max(1, b - a)
        frames.append((t_ref * P, s * P, content if aware else i / target))
        content += s * P
        t_ref += s
        i += 1
    return frames


def display_rate_60(R, secs=2.0):
    """M3b display-rate path: Present(1) every refresh, 60 ticks/s, no interpolation: picture = state of the
    last tick; repeated pictures merge into one longer hold."""
    P = 1.0 / R
    frames = []
    last_tick = -1
    for j in range(int(secs * R)):
        t = j * P
        tick = math.floor(t * 60 + 1e-9)
        if tick != last_tick:
            frames.append([t, P, tick / 60.0])
            last_tick = tick
        else:
            frames[-1][1] += P
    return [tuple(f) for f in frames]


def main():
    rows = []
    for R in (60.0, 75.0, 100.0, 119.88, 120.0, 144.0, 165.0, 180.0, 240.0):
        opts = []
        k60 = round(R / 60)
        if abs(R / k60 - 60) / 60 <= 0.002:
            opts.append(("60: vblank k=%d (M3b)" % k60, vblank(R, k60)))
        else:
            opts.append(("60: display-rate Present(1), no interp. (M3b)", display_rate_60(R)))
        for k in range(1, 5):
            if R / k <= 120.0 + 1e-6 and R / k >= 59.0:
                opts.append(("vblank k=%d (%.1f fps, interpolated)" % (k, R / k), vblank(R, k)))
        if R > 120.5 and abs(R / 120 - round(R / 120)) > 0.01:
            opts.append(("cadence 120, fixed quantum", cadence(R, 120.0, False)))
            opts.append(("cadence 120, scanout aware", cadence(R, 120.0, True)))
        if R >= 120:
            opts.append(("vrr timer 120 (panel follows)", vblank(120.0, 1)))
        for name, fr in opts:
            fps, hmin, hmax, pp = stats(fr)
            rows.append((R, name, fps, hmin, hmax, pp))
    with open(OUT, "w", encoding="utf-8") as f:
        hdr = "%-8s %-48s %8s %16s %10s" % ("refresh", "option", "fps", "hold min/max ms", "pos p-p ms")
        print(hdr)
        print(hdr, file=f)
        for R, name, fps, hmin, hmax, pp in rows:
            line = "%-8.2f %-48s %8.2f %7.2f/%-8.2f %10.2f" % (R, name, fps, hmin, hmax, pp)
            print(line)
            print(line, file=f)


if __name__ == "__main__":
    main()
