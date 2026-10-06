"""M3d interpolation analysis (m3d_interpolation.md 3.5): world-space interpolation against one-level hierarchical
interpolation for the real alpha 1/2 frame.

For each drawn object and two consecutive fixed steps (S0, S1) the capture holds the world transform W and the world
transform P of the parent node (+0x14). Hierarchical: L = P^-1 W (local relation; constant when only the parent moves),
W(1/2) = interp(P0, P1, 1/2) * L(1/2) with L interpolated too. World: W(1/2) = interp(W0, W1, 1/2) (lerp translation,
slerp rotation, lerp scale). The script reports how often the local relation is constant, the difference between
both results in world units and in pixels at 640 px width (capture camera), and the worst cases.
Usage: python m3d_interp_hier.py <capture.bin>"""
import math
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from m3d_interp_capture import read_records, rot, trans, dist, pct  # noqa: E402
from m3d_interp_midpoint import quat_from_rows, rows_from_quat, slerp  # noqa: E402


def decompose(xf):
    return quat_from_rows(rot(xf)), trans(xf), xf[15]


def interp(a, b, t):
    qa, ta, sa = a
    qb, tb, sb = b
    return slerp(qa, qb, t), tuple(x + (y - x) * t for x, y in zip(ta, tb)), sa + (sb - sa) * t


def to_xf(d):
    q, t, s = d
    r = rows_from_quat(q)
    return (r, t, s)


def compose(p, c):
    """world(child) = parent * child: v' = s_p (s_c v R_c + t_c) R_p + t_p (row vectors)."""
    rp, tp, sp = p
    rc, tc, sc = c
    r = tuple(tuple(sum(rc[i][k] * rp[k][j] for k in range(3)) for j in range(3)) for i in range(3))
    t = tuple(sp * sum(tc[k] * rp[k][j] for k in range(3)) + tp[j] for j in range(3))
    return r, t, sp * sc


def inverse(x):
    r, t, s = x
    rt = tuple(tuple(r[j][i] for j in range(3)) for i in range(3))
    ti = tuple(-sum(t[k] * rt[k][j] for k in range(3)) / s for j in range(3))
    return rt, ti, 1.0 / s


def from_raw(xf):
    return rot(xf), trans(xf), xf[15]


def main():
    last = {}
    diffs, pix, worst = [], [], []
    local_const = local_moving = 0
    for fi, f in enumerate(read_records(sys.argv[1])):
        cam = None
        for t, d in f["records"]:
            if t == "C":
                cam = d
                continue
            if t != "D" or cam is None or d["parent"] == 0:
                continue
            key = (d["transform"], cam["cam"])
            p = last.get(key)
            last[key] = (f["steps"], d)
            if p is None or f["steps"] - p[0] != 1:
                continue
            pd = p[1]
            if pd["xf"] == d["xf"]:
                continue
            w0, w1 = from_raw(pd["xf"]), from_raw(d["xf"])
            p0, p1 = from_raw(pd["pxf"]), from_raw(d["pxf"])
            l0, l1 = compose(inverse(p0), w0), compose(inverse(p1), w1)
            if dist(l0[1], l1[1]) < 1e-6 and all(abs(l0[0][i][j] - l1[0][i][j]) < 1e-6 for i in range(3)
                                                  for j in range(3)):
                local_const += 1
            else:
                local_moving += 1
            ww = to_xf(interp(decompose(pd["xf"]), decompose(d["xf"]), 0.5))
            pm = to_xf(interp((quat_from_rows(p0[0]), p0[1], p0[2]), (quat_from_rows(p1[0]), p1[1], p1[2]), 0.5))
            lm = to_xf(interp((quat_from_rows(l0[0]), l0[1], l0[2]), (quat_from_rows(l1[0]), l1[1], l1[2]), 0.5))
            wh = compose(pm, lm)
            e = dist(ww[1], wh[1])
            diffs.append(e)
            v = tuple(wh[1][i] - cam["loc"][i] for i in range(3))
            z = sum(v[i] * cam["dir"][i] for i in range(3))
            if z > cam["frustum"][4]:
                tan_r = (cam["frustum"][1] - cam["frustum"][0]) * 0.5
                px = e / z * 320.0 / tan_r
                pix.append(px)
                worst.append((round(px, 2), f["frame"], hex(key[0]), round(dist(w0[1], w1[1]), 3)))
    print("moving draws with a parent: %d; local relation constant (only the parent moved) %d, local moving %d" % (
        local_const + local_moving, local_const, local_moving))
    if diffs:
        print("world vs hierarchical at alpha 1/2: |dt| p50 %.3g p99 %.3g max %.3g" % (pct(diffs, .5), pct(diffs, .99),
                                                                                    max(diffs)))
        print("  pixels at 640 px: p50 %.3g p99 %.3g p99.9 %.3g max %.3g; > 1 px: %d of %d" % (
            pct(pix, .5), pct(pix, .99), pct(pix, .999), max(pix), sum(1 for x in pix if x > 1), len(pix)))
        worst.sort(reverse=True)
        print("  worst (px, frame, transform, world step):", worst[:6])


if __name__ == "__main__":
    main()
