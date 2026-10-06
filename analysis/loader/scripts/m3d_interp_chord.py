"""M3d interpolation analysis (m3d_interpolation.md 3.5): upper bound of the chord error of world-space lerp at
alpha 1/2 for objects that move on circular paths (rigid parts of a rotating hierarchy).

If an object turns by theta per tick and moves by d, a rigid rotation about a fixed centre gives the arc midpoint;
the linear midpoint lies d * tan(theta / 4) / 2 inside it. The script evaluates that bound for every moving draw of
a capture (snaps of the 3.4 rule excluded) in pixels at 640 px width (capture camera).
Usage: python m3d_interp_chord.py <capture.bin>"""
import math
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from m3d_interp_capture import read_records, rot, trans, dist, rel_angle_deg, pct  # noqa: E402


def main():
    last = {}
    prevstep = {}
    pix = []
    worst = []
    for f in read_records(sys.argv[1]):
        cam = None
        for t, d in f["records"]:
            if t == "C":
                cam = d
                continue
            if t != "D" or cam is None:
                continue
            key = (d["transform"], cam["cam"])
            p = last.get(key)
            last[key] = (f["steps"], d)
            if p is None or f["steps"] - p[0] != 1 or p[1]["xf"] == d["xf"]:
                continue
            dd = dist(trans(p[1]["xf"]), trans(d["xf"]))
            a = rel_angle_deg(rot(p[1]["xf"]), rot(d["xf"]))
            r = max(d["bound"][3], 1e-6)
            pd, pa = prevstep.get(key, (0.0, 0.0))
            prevstep[key] = (dd, a)
            if (dd > 2 * r and dd > 4 * pd + 0.01 * r) or (a > 45 and a > 4 * pa + 1):
                continue
            e = dd * math.tan(math.radians(a) / 4) / 2
            v = tuple(trans(d["xf"])[i] - cam["loc"][i] for i in range(3))
            z = sum(v[i] * cam["dir"][i] for i in range(3))
            if z > cam["frustum"][4]:
                tan_r = (cam["frustum"][1] - cam["frustum"][0]) * 0.5
                px = e / z * 320.0 / tan_r
                pix.append(px)
                worst.append((round(px, 2), f["frame"], hex(key[0]), round(dd, 3), round(a, 1)))
    worst.sort(reverse=True)
    print("moving draws: %d; chord bound at 640 px: p50 %.3g p99 %.3g p99.9 %.3g max %.3g; > 0.5 px %d, > 1 px %d"
          % (len(pix), pct(pix, .5), pct(pix, .99), pct(pix, .999), max(pix), sum(1 for x in pix if x > .5),
             sum(1 for x in pix if x > 1)))
    print("worst (px, frame, transform, step, angle):", worst[:8])


if __name__ == "__main__":
    main()
