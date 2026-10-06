"""M3d interpolation analysis: geometry, skin and node counts per NIF/UCF file (upper bound of the draws a
scene can issue; the ingame scenes are not reachable before M4). Read-only; prints the largest files per group."""
import collections
import re
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from m3d_interp_isofiles import iter_files  # noqa: E402

PAT = {c: re.compile(re.escape(struct.pack("<I", len(c)) + c.encode())) for c in
       ("NiTriShape", "NiSkinInstance", "NiNode", "NiLODNode", "NiRoom", "NiBillboardNode", "NiKeyframeController")}


def main():
    rows = []
    for arc, path, data in iter_files({"nif", "ucf"}):
        if not data.startswith(b"NetImmerse File Format"):
            continue
        rows.append((path, {c: len(p.findall(data)) for c, p in PAT.items()}))
    by_group = collections.defaultdict(list)
    for path, c in rows:
        by_group[path.split("/")[0]].append((path, c))
    for g in ("levels", "chars", "effects", "menu", "hud", "cockpit"):
        lst = sorted(by_group[g], key=lambda r: -r[1]["NiTriShape"])
        if not lst:
            continue
        shapes = sorted(r[1]["NiTriShape"] for r in lst)
        print("%s: %d files, NiTriShape per file median %d, max %d" % (g, len(lst), shapes[len(shapes) // 2],
                                                                     shapes[-1]))
        for path, c in lst[:6]:
            print("   %-50s %s" % (path, c))


if __name__ == "__main__":
    main()
