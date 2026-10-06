"""M3d interpolation analysis (m3d_interpolation.md 3.6): how well lerp/slerp reproduces real intermediate states.

Without a game state between two ticks, the test interpolates over TWO ticks and compares with the real state of the
tick in between: e = interp(S[G-1], S[G+1], 1/2) - S[G]. For motion with continuous acceleration the midpoint error of
linear interpolation over an interval h is h^2 |a| / 8, so the error of the real 1-tick interpolation (alpha 1/2 at the
tick frames) is about e / 4. Rotations: slerp of the quaternions of the NiTransform rows (Shepperd), error = angle
between slerp(R0, R2, 1/2) and R1. Triples containing a snap (rule of 3.4: step > 2 radii and > 4x the previous step,
or angle > 45 deg and > 4x the previous angle + 1) are left out. Pixel errors use the capture's camera (depth along the
view direction, frustum tangent) at 640 px (guest) width.
Usage: python m3d_interp_midpoint.py <capture.bin>"""
import math
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from m3d_interp_capture import read_records, rot, trans, dist, rel_angle_deg, pct  # noqa: E402


def quat_from_rows(m):
    """Unit quaternion (w, x, y, z) of the rotation whose rows are the axis images (row-vector convention)."""
    # Use the column-vector matrix M = m^T (standard formulas).
    a = [[m[j][i] for j in range(3)] for i in range(3)]
    tr = a[0][0] + a[1][1] + a[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        q = (0.25 * s, (a[2][1] - a[1][2]) / s, (a[0][2] - a[2][0]) / s, (a[1][0] - a[0][1]) / s)
    elif a[0][0] > a[1][1] and a[0][0] > a[2][2]:
        s = math.sqrt(1.0 + a[0][0] - a[1][1] - a[2][2]) * 2
        q = ((a[2][1] - a[1][2]) / s, 0.25 * s, (a[0][1] + a[1][0]) / s, (a[0][2] + a[2][0]) / s)
    elif a[1][1] > a[2][2]:
        s = math.sqrt(1.0 + a[1][1] - a[0][0] - a[2][2]) * 2
        q = ((a[0][2] - a[2][0]) / s, (a[0][1] + a[1][0]) / s, 0.25 * s, (a[1][2] + a[2][1]) / s)
    else:
        s = math.sqrt(1.0 + a[2][2] - a[0][0] - a[1][1]) * 2
        q = ((a[1][0] - a[0][1]) / s, (a[0][2] + a[2][0]) / s, (a[1][2] + a[2][1]) / s, 0.25 * s)
    n = math.sqrt(sum(c * c for c in q))
    return tuple(c / n for c in q)


def rows_from_quat(q):
    w, x, y, z = q
    a = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return tuple(tuple(a[j][i] for j in range(3)) for i in range(3))  # transpose back to rows


def slerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b = tuple(-c for c in b)
        d = -d
    if d > 0.9995:
        r = tuple(x + (y - x) * t for x, y in zip(a, b))
    else:
        th = math.acos(d)
        s0 = math.sin((1 - t) * th) / math.sin(th)
        s1 = math.sin(t * th) / math.sin(th)
        r = tuple(s0 * x + s1 * y for x, y in zip(a, b))
    n = math.sqrt(sum(c * c for c in r))
    return tuple(c / n for c in r)


def quat_angle_deg(a, b):
    """Robust angle between two unit quaternions (no acos near 1)."""
    d = abs(sum(x * y for x, y in zip(a, b)))
    d = min(1.0, d)
    return math.degrees(2.0 * math.atan2(math.sqrt(max(0.0, 1.0 - d * d)), d))


def main():
    worst = []
    seq = {}  # key -> list of (steps, xf, radius) of consecutive ticks
    cams = {}
    roundtrip = 0.0
    et, er, rel, pix, steps = [], [], [], [], []
    for fi, f in enumerate(read_records(sys.argv[1])):
        cam = None
        for t, d in f["records"]:
            if t == "C":
                cam = d
                cams[d["cam"]] = d
                continue
            if t != "D" or cam is None:
                continue
            key = (d["transform"], cam["cam"])
            s = seq.setdefault(key, [])
            if s and s[-1][0] == f["steps"]:
                continue  # 0-step frame: same state
            if s and s[-1][0] != f["steps"] - 1:
                s.clear()
            s.append((f["steps"], d["xf"], d["bound"][3]))
            if len(s) > 4:
                s.pop(0)
            if len(s) < 4:
                continue
            (_, xa, ra), (_, x0, r0), (_, x1, r1), (_, x2, r2) = s
            ta, t0, t1, t2 = trans(xa), trans(x0), trans(x1), trans(x2)
            d0, d1, d2 = dist(ta, t0), dist(t0, t1), dist(t1, t2)
            a0, a1, a2 = (rel_angle_deg(rot(xa), rot(x0)), rel_angle_deg(rot(x0), rot(x1)),
                          rel_angle_deg(rot(x1), rot(x2)))
            r = max(r1, 1e-6)

            def snap(dd, dp, aa, ap):
                return (dd > 2 * r and dd > 4 * dp + 0.01 * r) or (aa > 45 and aa > 4 * ap + 1)
            if snap(d1, d0, a1, a0) or snap(d2, d1, a2, a1):
                continue
            if d1 == 0 and d2 == 0 and a1 == 0 and a2 == 0:
                continue
            q0, q1, q2 = quat_from_rows(rot(x0)), quat_from_rows(rot(x1)), quat_from_rows(rot(x2))
            roundtrip = max(roundtrip, quat_angle_deg(quat_from_rows(rows_from_quat(q1)), q1))
            mid_t = tuple((p + q) * 0.5 for p, q in zip(t0, t2))
            e = dist(mid_t, t1)
            ang = quat_angle_deg(slerp(q0, q2, 0.5), q1)
            et.append(e)
            er.append(ang)
            step = 0.5 * dist(t0, t2)
            steps.append(step)
            rel.append(e / step if step > 0 else 0.0)
            # depth along the view direction and pixels at 640 px width
            v = tuple(t1[i] - cam["loc"][i] for i in range(3))
            z = sum(v[i] * cam["dir"][i] for i in range(3))
            if z > cam["frustum"][4]:
                tan_r = (cam["frustum"][1] - cam["frustum"][0]) * 0.5
                pix.append(e / z * 320.0 / tan_r)
                worst.append((pix[-1], f["frame"], hex(key[0]), round(d0, 3), round(d1, 3), round(d2, 3), round(a1, 1), round(a2, 1), round(r, 3)))
    worst.sort(reverse=True)
    print("largest pixel errors (px, frame, transform, step G-2..G-1, G-1..G, G..G+1, angles, radius):")
    for w in worst[:8]:
        print("  ", w)
    big = sum(1 for p in pix if p > 2.0)
    print("triples with > 2 px (640) midpoint error: %d of %d" % (big, len(pix)))
    print("triples evaluated: %d (quaternion round trip max %.2g deg)" % (len(et), roundtrip))
    if et:
        print("2-tick midpoint error |t|: p50 %.3g p99 %.3g max %.3g (per-tick step p50 %.3g p99 %.3g)" % (
            pct(et, .5), pct(et, .99), max(et), pct(steps, .5), pct(steps, .99)))
        print("  relative to the step: p50 %.3g p99 %.3g" % (pct(rel, .5), pct(rel, .99)))
        print("  rotation error deg: p50 %.3g p99 %.3g max %.3g" % (pct(er, .5), pct(er, .99), max(er)))
        print("  pixels at 640 px: p50 %.3g p99 %.3g max %.3g -> 1-tick interpolation about 1/4: p99 %.3g px "
              "(x4.5 at 2880x2160: %.3g px)" % (pct(pix, .5), pct(pix, .99), max(pix), pct(pix, .99) / 4,
                                               pct(pix, .99) / 4 * 4.5))


if __name__ == "__main__":
    main()
