"""M3d interpolation analysis: the large per-tick jumps of a draw capture (m3d_interp_capture.py format).

Lists keys whose world transform moves more than --rel radii or rotates more than --deg degrees between two
consecutive fixed steps, with the frames, so loop wraps / teleports / flips can be told apart from motion.
Usage: python m3d_interp_outliers.py <capture.bin> [--rel 4] [--deg 30]"""
import argparse
import collections
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from m3d_interp_capture import read_records, rot, trans, dist, rel_angle_deg  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--rel", type=float, default=4.0)
    ap.add_argument("--deg", type=float, default=30.0)
    args = ap.parse_args()
    last = {}
    events = collections.defaultdict(list)
    motion = collections.defaultdict(list)
    for fi, f in enumerate(read_records(args.capture)):
        cam = 0
        for t, d in f["records"]:
            if t == "C":
                cam = d["cam"]
                continue
            if t != "D":
                continue
            key = (d["transform"], cam)
            p = last.get(key)
            if p is not None and f["steps"] - p[1] == 1 and p[0] == fi - 1:
                pd = p[2]
                dt = dist(trans(pd["xf"]), trans(d["xf"]))
                rel = dt / max(d["bound"][3], 1e-6)
                ang = rel_angle_deg(rot(pd["xf"]), rot(d["xf"]))
                motion[key].append((f["frame"], dt, rel, ang))
                if rel > args.rel or ang > args.deg:
                    events[key].append((f["frame"], round(dt, 3), round(rel, 2), round(ang, 1)))
            last[key] = (fi, f["steps"], d)
    print("keys with jumps: %d" % len(events))
    for key, ev in sorted(events.items(), key=lambda kv: -len(kv[1]))[:25]:
        m = motion[key]
        typical = sorted(x[1] for x in m)[len(m) // 2]
        typ_ang = sorted(x[3] for x in m)[len(m) // 2]
        frames = [e[0] for e in ev]
        gaps = sorted(set(b - a for a, b in zip(frames, frames[1:])))
        print("transform 0x%08X: %d jumps of %d ticks (median |dt| %.4g, median angle %.3g deg); gaps between jumps %s;"
              " first: %s" % (key[0], len(ev), len(m), typical, typ_ang, gaps[:6], ev[:4]))


if __name__ == "__main__":
    main()
