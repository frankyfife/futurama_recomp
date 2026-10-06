"""M3d interpolation analysis: first-sight draws of a capture (m3d_interp_capture.py format) and whether the
parent fallback (interpolate the parent's world transform, keep the child's local relation) would apply:
the parent node was the parent of a drawn object in the previous fixed step and its world transform moved.
Frames with more than 50 first-sight keys (context switches) are excluded.
Usage: python m3d_interp_firstsight.py <capture.bin>"""
import collections
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from m3d_interp_capture import read_records  # noqa: E402


def main():
    prev_keys = set()
    prev_parents = {}  # parent -> world of the previous step
    prev_steps = None
    stats = collections.Counter()
    for f in read_records(sys.argv[1]):
        cam = 0
        keys = set()
        parents = {}
        fs = []
        for t, d in f["records"]:
            if t == "C":
                cam = d["cam"]
            elif t == "D":
                key = (d["transform"], cam)
                keys.add(key)
                parents[d["parent"]] = d["pxf"]
                if key not in prev_keys:
                    fs.append(d)
        if prev_steps is not None and f["steps"] - prev_steps == 1 and 0 < len(fs) <= 50:
            for d in fs:
                stats["first sight"] += 1
                pw = prev_parents.get(d["parent"])
                if pw is None:
                    stats["parent not seen in the previous step"] += 1
                elif pw == d["pxf"]:
                    stats["parent seen, static (fallback = snap)"] += 1
                else:
                    stats["parent seen and moving (fallback helps)"] += 1
        if prev_steps is None or f["steps"] != prev_steps:
            prev_keys, prev_parents, prev_steps = keys, parents, f["steps"]
        else:
            prev_keys |= keys
            prev_parents.update(parents)
    for k, v in stats.most_common():
        print("%-45s %d" % (k, v))


if __name__ == "__main__":
    main()
