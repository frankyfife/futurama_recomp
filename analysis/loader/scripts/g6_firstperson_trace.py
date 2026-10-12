"""G6 (analysis/loader/g6_firstperson.md 6): measure from the 'first person trace' lines (log with [debug] trace =
game) that the player moves along the view: per tick with left-stick input, displacement (x, z) of the next tick vs. the intended direction
stickY * forward + stickX * right (view rows, flattened). Prints per input phase (same stick values) the number of
ticks, the mean cosine between displacement and intent, the speed and the yaw.
Usage: python analysis/loader/scripts/g6_firstperson_trace.py LOG [LOG ...]"""
import math
import re
import sys

RX = re.compile(r"first person trace: pos (\S+) (\S+) (\S+) eye \S+ yaw (\S+) pitch (\S+) fwd (\S+) (\S+) (\S+) "
                r"right (\S+) (\S+) move \S+ \S+ stick (\S+) (\S+)")


def main():
    for path in sys.argv[1:]:
        rows = []
        for line in open(path, encoding="utf-8", errors="replace"):
            m = RX.search(line)
            if m:
                rows.append([float(v) for v in m.groups()])
        print(f"== {path}: {len(rows)} trace ticks")
        phases = []
        for a, b in zip(rows, rows[1:]):
            sx, sy = a[10], a[11]
            if sx == 0 and sy == 0:
                continue
            fx, fz = a[5], a[7]
            fl = math.hypot(fx, fz)
            rx, rz = a[8], a[9]
            ix = sy * fx / fl + sx * rx
            iz = sy * fz / fl + sx * rz
            il = math.hypot(ix, iz)
            dx, dz = b[0] - a[0], b[2] - a[2]
            dl = math.hypot(dx, dz)
            key = (sx, sy)
            if not phases or phases[-1]["key"] != key:
                phases.append({"key": key, "n": 0, "cos": [], "speed": [], "yaw": a[3], "dy": 0.0})
            ph = phases[-1]
            ph["n"] += 1
            ph["dy"] = max(ph["dy"], abs(b[1] - a[1]))
            if dl > 1e-4 and il > 1e-4:
                ph["cos"].append((dx * ix + dz * iz) / (dl * il))
                ph["speed"].append(dl * 60.0)
        for ph in phases:
            c = ph["cos"]
            s = ph["speed"]
            if not c:
                print(f"  stick {ph['key']}: {ph['n']} ticks, no movement")
                continue
            moving = sum(1 for v in s if v > 1.0)
            print(f"  stick x {ph['key'][0]:+.2f} y {ph['key'][1]:+.2f}: {ph['n']:3d} ticks, moving {moving:3d}, "
                  f"cos mean {sum(c) / len(c):.3f} min {min(c):.3f}, speed median {sorted(s)[len(s) // 2]:.1f} u/s, "
                  f"yaw {ph['yaw']:.1f}, max |dy| {ph['dy']:.2f}")


if __name__ == "__main__":
    main()
