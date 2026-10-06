"""M3d interpolation analysis: evaluates snap rules on a draw capture (m3d_interp_capture.py format).

Reference ("discontinuity", non-causal): an isolated spike - a tick whose translation step is more than 2 bound
radii and more than 4x both neighbouring steps (previous and next tick), or whose rotation step is more than 30
degrees and more than 4x both neighbouring rotation steps (+1 degree): keyframe loop wraps, teleports. Every other
moving tick counts as continuous motion (also sustained fast motion or spinning). For each candidate rule the script
counts discontinuities it snaps (hit), discontinuities it interpolates (miss) and continuous ticks it snaps
(false snap). Usage: python m3d_interp_snaprules.py <capture.bin>"""
import collections
import statistics
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from m3d_interp_capture import read_records, rot, trans, dist, rel_angle_deg  # noqa: E402


def main():
    path = sys.argv[1]
    last = {}
    series = collections.defaultdict(list)  # key -> [(dt, radius, angle)] per consecutive tick, in order
    scale_events = collections.Counter()
    for fi, f in enumerate(read_records(path)):
        cam = 0
        for t, d in f["records"]:
            if t == "C":
                cam = d["cam"]
                continue
            if t != "D":
                continue
            key = (d["transform"], cam)
            p = last.get(key)
            if p is not None and f["steps"] == p[1] and p[0] == fi - 1:
                last[key] = (fi, f["steps"], d)  # same fixed step (0-step frame): not a new sample
                continue
            if p is not None and f["steps"] - p[1] == 1 and p[0] == fi - 1:
                pd = p[2]
                dt = dist(trans(pd["xf"]), trans(d["xf"]))
                ang = rel_angle_deg(rot(pd["xf"]), rot(d["xf"]))
                s0, s1 = pd["xf"][15], d["xf"][15]
                if s0 != s1:
                    scale_events[(round(s0, 3) == 0.0, round(s1 / s0 if s0 else 0.0, 2))] += 1
                series[key].append((dt, max(d["bound"][3], 1e-6), ang, s0, s1))
            else:
                series[key].append(None)  # history break (first sight / gap)
            last[key] = (fi, f["steps"], d)
    rules = {
        "R1 dt > 4r": lambda dt, r, a, pdt, pa: dt > 4 * r,
        "R2 dt > 2r and dt > 4 dt_prev + 0.01r": lambda dt, r, a, pdt, pa: dt > 2 * r and dt > 4 * pdt + 0.01 * r,
        "R3 angle > 90": lambda dt, r, a, pdt, pa: a > 90,
        "R4 angle > 45 and angle > 4 angle_prev + 1": lambda dt, r, a, pdt, pa: a > 45 and a > 4 * pa + 1,
        "R2 or R4 (proposal)": lambda dt, r, a, pdt, pa: (dt > 2 * r and dt > 4 * pdt + 0.01 * r) or
        (a > 45 and a > 4 * pa + 1),
        "R2 or R3": lambda dt, r, a, pdt, pa: (dt > 2 * r and dt > 4 * pdt + 0.01 * r) or a > 90,
    }
    res = {k: collections.Counter() for k in rules}
    disc_total = cont_total = 0
    examples = collections.defaultdict(list)
    for key, s in series.items():
        moving = [x for x in s if x is not None and (x[0] > 0 or x[2] > 0)]
        if not moving:
            continue
        med_dt = statistics.median(x[0] for x in moving)
        med_a = statistics.median(x[2] for x in moving)
        prev = None
        for i, x in enumerate(s):
            if x is None:
                prev = None
                continue
            dt, r, a, s0, s1 = x
            if dt == 0 and a == 0:
                prev = x
                continue
            nxt = s[i + 1] if i + 1 < len(s) else None
            ndt = nxt[0] if nxt else 0.0
            na = nxt[2] if nxt else 0.0
            pdt0 = prev[0] if prev else 0.0
            pa0 = prev[2] if prev else 0.0
            disc = (dt > 2 * r and dt > 4 * max(pdt0, ndt) + 0.01 * r) or (a > 30 and a > 4 * max(pa0, na) + 1)
            pdt = prev[0] if prev else 0.0
            pa = prev[2] if prev else 0.0
            if disc:
                disc_total += 1
            else:
                cont_total += 1
            if disc and len(examples[key]) < 3:
                examples[key].append((round(dt, 3), round(r, 3), round(a, 1), round(pdt, 3), round(pa, 1), round(med_dt, 4)))
            for name, rule in rules.items():
                snap = rule(dt, r, a, pdt, pa)
                if disc and snap:
                    res[name]["hit"] += 1
                elif disc:
                    res[name]["miss"] += 1
                elif snap:
                    res[name]["false snap"] += 1
            prev = x
    print("discontinuities %d, continuous moving ticks %d" % (disc_total, cont_total))
    for name in rules:
        c = res[name]
        print("%-45s hit %5d  miss %5d  false snap %5d (%.3f %% of continuous ticks)" % (
            name, c["hit"], c["miss"], c["false snap"], 100.0 * c["false snap"] / max(1, cont_total)))
    print("discontinuity examples (dt, radius, angle, dt_prev, angle_prev, median dt) of %d keys:" % len(examples))
    for k, ex in sorted(examples.items(), key=lambda kv: -len(kv[1]))[:12]:
        print("  0x%08X cam 0x%08X: %s" % (k[0], k[1], ex))
    print("scale changes (from 0?, ratio):", dict(scale_events.most_common(10)))


if __name__ == "__main__":
    main()
