"""G5: evaluates the camera trace of an input-script run (`game camera_trace on`, xapi/camera_response.cpp).

Reads the "camera trace: <tick> mode M yaw Y ... stick S" lines of a futurama.log, unwraps the yaw, computes the
turn rate per tick (deg/s at the given tick rate) and reports per stick phase (constant stick value): steady rate,
time to 50/90 % of it, and after the release the time until the camera stops (< 1 deg/s) and the overshoot angle.
Usage: python g5_camera_trace.py <futurama.log> [--rate 60] [--csv out.csv]
"""
import argparse
import re

LINE = re.compile(r"camera trace: (\d+) mode (\d+) yaw (-?[\d.]+) .* stick (-?[\d.]+) ctrl (\d)")


def load(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if m:
                rows.append((int(m.group(1)), int(m.group(2)), float(m.group(3)), float(m.group(4))))
    return rows


def unwrap(values):
    out, prev, off = [], None, 0.0
    for v in values:
        if prev is not None:
            d = v - prev
            if d > 180:
                off -= 360
            elif d < -180:
                off += 360
        out.append(v + off)
        prev = v
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--rate", type=float, default=60.0)
    ap.add_argument("--csv")
    a = ap.parse_args()
    rows = load(a.log)
    if not rows:
        print("no camera trace lines")
        return
    yaw = unwrap([r[2] for r in rows])
    # the trace at tick n shows the state after the camera update of tick n-1 and the stick read in tick n-1:
    # rate[n] = (yaw[n+1] - yaw[n]) belongs to stick[n+1]
    rate = [(yaw[i + 1] - yaw[i]) * a.rate for i in range(len(yaw) - 1)] + [0.0]
    stick = [r[3] for r in rows]
    if a.csv:
        with open(a.csv, "w", encoding="utf-8") as f:
            f.write("tick,stick,yaw_deg,rate_deg_s\n")
            for i, r in enumerate(rows):
                f.write(f"{r[0]},{stick[i]:.4f},{yaw[i]:.4f},{rate[i]:.3f}\n")
    # phases of constant stick
    phases, start = [], 0
    for i in range(1, len(rows) + 1):
        if i == len(rows) or stick[i] != stick[start]:
            phases.append((start, i))
            start = i
    print(f"{len(rows)} ticks, yaw {yaw[0]:.1f} -> {yaw[-1]:.1f} deg")
    for s, e in phases:
        n = e - s
        st = stick[s]
        seg = rate[s:e - 1] if e - 1 > s else rate[s:e]
        if st != 0.0:
            tail = seg[len(seg) // 2:] or seg
            steady = sum(tail) / len(tail)
            def reach(frac):
                for k, v in enumerate(seg):
                    if abs(v) >= frac * abs(steady):
                        return (k + 1) / a.rate
                return float("nan")
            print(f"stick {st:+.3f}: {n} ticks, steady {steady:+.1f} deg/s (last half), peak {max(seg, key=abs):+.1f}, "
                  f"50 % after {reach(0.5) * 1000:.0f} ms, 90 % after {reach(0.9) * 1000:.0f} ms, "
                  f"turned {yaw[e - 1] - yaw[s]:+.1f} deg")
        else:
            stop = next((k for k, v in enumerate(seg) if abs(v) < 1.0), None)
            glide = yaw[min(s + (stop if stop is not None else n - 1), len(yaw) - 1)] - yaw[s]
            print(f"stick  0     : {n} ticks, stops (< 1 deg/s) after "
                  f"{(stop + 1) / a.rate * 1000 if stop is not None else float('nan'):.0f} ms, glides on {glide:+.1f} deg")


if __name__ == "__main__":
    main()
