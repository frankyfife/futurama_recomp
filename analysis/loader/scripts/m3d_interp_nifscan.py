"""M3d interpolation analysis (analysis/loader/m3d_interpolation.md 2, 5): census of the disc's NIF/UCF files
for everything that matters to renderer-level interpolation.

Reads the ISO archives read-only (m3d_interp_isofiles.py). NIF 4.2.1.0: every block starts with its
length-prefixed class name, references are block indices (the M3 texconv census uses the same scheme).
Counts per directory group (chars, levels, effects, menu, hud, images, cockpit, ...):
  - classes (geometry, nodes with display-time logic, particles, controllers),
  - NiAVObject local transforms: determinant < 0 (mirrored), orthonormality error, scale != 1,
  - NiBillboardNode modes (flags bits 10..12, as NiBillboardNode::Display FUN_00110360 reads +0xB0),
  - NiSkinInstance / NiSkinData bone counts and influence counts,
  - particle data vertex counts (upper bound of the particles per system).
Output: summary on stdout (the report quotes it); nothing is written."""
import collections
import json
import math
import re
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from m3d_interp_isofiles import iter_files  # noqa: E402

CLASSES = sorted(json.load(open(r"D:\FuturamaDecomp\analysis\retail_eu_ni_classes.json"))["classes"], key=len,
                 reverse=True)
CLS_RE = re.compile(b"(" + b"|".join(re.escape(struct.pack("<I", len(c)) + c.encode()) for c in CLASSES) + b")")

AV_CLASSES = {"NiNode", "NiTriShape", "NiTriStrips", "NiBillboardNode", "NiLODNode", "NiSwitchNode", "NiOutline",
              "NiParticles", "NiAutoNormalParticles", "NiRotatingParticles", "NiParticleMeshes", "NiLines",
              "NiSortAdjustNode", "NiBSPNode", "NiCamera", "NiFltAnimationNode", "NiCollisionSwitch", "NiRoom",
              "NiRoomGroup", "NiTriangles", "NiTriStrip", "NiAmbientLight", "NiDirectionalLight", "NiPointLight",
              "NiSpotLight", "NiTextureEffect", "NiUDSNode"}
NODE_CLASSES = {"NiNode", "NiBillboardNode", "NiLODNode", "NiSwitchNode", "NiOutline", "NiSortAdjustNode",
                "NiBSPNode", "NiFltAnimationNode", "NiCollisionSwitch", "NiRoom", "NiRoomGroup"}


def blocks(data):
    if not data.startswith(b"NetImmerse File Format, Version 4.2"):
        return None
    nl = data.index(b"\n")
    pos = nl + 9
    return [(m.start(), m.group(0)[4:].decode()) for m in CLS_RE.finditer(data, pos)]


class Reader:
    def __init__(self, b, o):
        self.b, self.o = b, o

    def u8(self):
        v = self.b[self.o]
        self.o += 1
        return v

    def u16(self):
        v = struct.unpack_from("<H", self.b, self.o)[0]
        self.o += 2
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.o)[0]
        self.o += 4
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.o)[0]
        self.o += 4
        return v

    def f(self, n=1):
        v = struct.unpack_from("<%df" % n, self.b, self.o)
        self.o += 4 * n
        return v if n > 1 else v[0]

    def string(self):
        n = self.u32()
        if n > 4096:
            raise ValueError("string length %d" % n)
        s = self.b[self.o:self.o + n]
        self.o += n
        return s.decode("latin-1")


def parse_av(r):
    """NiObjectNET + NiAVObject (4.2.1.0). Returns dict; r stays behind the NiAVObject part."""
    name = r.string()
    r.i32()  # extra data
    r.i32()  # controller
    flags = r.u16()
    t = r.f(3)
    rot = r.f(9)
    scale = r.f()
    r.f(3)  # velocity
    nprop = r.u32()
    if nprop > 64:
        raise ValueError("properties %d" % nprop)
    for _ in range(nprop):
        r.i32()
    if r.u8():
        r.u32()
        r.f(3)
        r.f(9)
        r.f(3)
    return dict(name=name, flags=flags, t=t, rot=rot, scale=scale)


def mat_stats(m):
    a = [m[0:3], m[3:6], m[6:9]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    err = 0.0
    for i in range(3):
        for j in range(3):
            d = sum(a[i][k] * a[j][k] for k in range(3)) - (1.0 if i == j else 0.0)
            err = max(err, abs(d))
    return det, err


def group_of(path):
    return path.split("/")[0]


def main():
    cls_by_group = collections.defaultdict(collections.Counter)
    files_by_group = collections.Counter()
    files_with = collections.defaultdict(set)
    av_total = 0
    av_parsed = 0
    mirrored = collections.Counter()
    ortho_bad = collections.Counter()
    ortho_max = 0.0
    scale_ne1 = collections.Counter()
    scale_min, scale_max = 1e9, -1e9
    bb_modes = collections.Counter()
    skin_bones = []
    skin_infl = []
    skin_verts = []
    particle_verts = []
    outline_examples = []
    errors = collections.Counter()
    for arc, path, data in iter_files({"nif", "ucf"}):
        bl = blocks(data)
        if bl is None:
            continue
        g = group_of(path)
        files_by_group[g] += 1
        seen = set()
        for i, (o, c) in enumerate(bl):
            cls_by_group[g][c] += 1
            if c not in seen:
                files_with[c].add(path)
                seen.add(c)
            body = o + 4 + len(c)
            end = bl[i + 1][0] if i + 1 < len(bl) else len(data)
            r = Reader(data, body)
            try:
                if c in AV_CLASSES:
                    av_total += 1
                    av = parse_av(r)
                    det, err = mat_stats(av["rot"])
                    if not (0.5 < abs(det) < 2.0) or abs(av["scale"]) > 1e4:
                        errors["implausible transform " + c] += 1
                        continue
                    av_parsed += 1
                    if det < 0:
                        mirrored[c] += 1
                    if err > 1e-3:
                        ortho_bad[c] += 1
                    ortho_max = max(ortho_max, err)
                    if abs(av["scale"] - 1.0) > 1e-4:
                        scale_ne1[c] += 1
                    scale_min = min(scale_min, av["scale"])
                    scale_max = max(scale_max, av["scale"])
                    if c == "NiBillboardNode":
                        bb_modes[(av["flags"] >> 10) & 7] += 1
                    if c == "NiOutline" and len(outline_examples) < 12:
                        outline_examples.append(f"{path}:{av['name']}")
                elif c == "NiSkinData":
                    r.f(9)
                    r.f(3)
                    r.f()
                    nb = r.u32()
                    r.i32()  # skin partition (4.0.0.2 .. 10.1.0.0)
                    has_weights = r.u8()  # "Has Vertex Weights" (from 4.2.1.0 on, nif.xml)
                    if nb > 512:
                        raise ValueError("bones %d" % nb)
                    infl = 0
                    vset = set()
                    for _ in range(nb):
                        r.f(9)
                        r.f(3)
                        r.f()
                        r.f(4)
                        nv = r.u16()
                        for _ in range(nv if has_weights else 0):
                            vi = r.u16()
                            r.f()
                            vset.add(vi)
                        infl += nv
                    if r.o > end:
                        raise ValueError("NiSkinData overruns its block")
                    skin_bones.append(nb)
                    skin_infl.append(infl)
                    skin_verts.append(len(vset))
                elif c in ("NiParticlesData", "NiAutoNormalParticlesData", "NiRotatingParticlesData",
                           "NiParticleMeshesData"):
                    nv = r.u16()
                    particle_verts.append(nv)
            except Exception as e:  # noqa: BLE001 - census: count and continue
                errors[f"{c}: {type(e).__name__}"] += 1
    groups = sorted(files_by_group)
    print("files per group:", dict(files_by_group))
    interesting = ["NiTriShape", "NiTriStrips", "NiTriShapeDynamicData", "NiSkinInstance", "NiSkinData",
                   "NiSkinPartition", "NiOutline", "NiOutlineData", "NiBillboardNode", "NiLODNode", "NiSwitchNode",
                   "NiFltAnimationNode", "NiSortAdjustNode", "NiBSPNode", "NiCollisionSwitch", "NiRoom",
                   "NiRoomGroup", "NiParticles", "NiAutoNormalParticles", "NiRotatingParticles",
                   "NiParticleMeshes", "NiParticleSystemController", "NiParticleBomb", "NiGravity",
                   "NiParticleGrowFade", "NiParticleColorModifier", "NiParticleRotation", "NiPlanarCollider",
                   "NiSphericalCollider", "NiLines", "NiKeyframeController", "NiKeyframeData", "NiUDSKFController",
                   "NiPathController", "NiLookAtController", "NiRollController", "NiVisController",
                   "NiUVController", "NiFlipController", "NiAlphaController", "NiMaterialColorController",
                   "NiLightColorController", "NiGeomMorpherController", "NiMorpherController",
                   "NiBoneLODController", "NiSequence", "NiCamera", "NiTextureEffect", "NiPointLight",
                   "NiDirectionalLight", "NiSpotLight", "NiAmbientLight"]
    print("class\t" + "\t".join(groups) + "\ttotal\tfiles")
    for c in interesting:
        row = [cls_by_group[g][c] for g in groups]
        print(c + "\t" + "\t".join(str(x) for x in row) + "\t%d\t%d" % (sum(row), len(files_with[c])))
    print("NiAVObject-derived blocks: %d, parsed %d" % (av_total, av_parsed))
    print("  mirrored (det < 0):", dict(mirrored))
    print("  orthonormality error > 1e-3:", dict(ortho_bad), "max error %.3g" % ortho_max)
    print("  scale != 1:", dict(scale_ne1), "range %.4g .. %.4g" % (scale_min, scale_max))
    print("NiBillboardNode modes (flags >> 10 & 7):", dict(bb_modes))
    if skin_bones:
        sb = sorted(skin_bones)
        print("NiSkinData: %d, bones min/median/p90/max %d/%d/%d/%d, total influences %d, skinned vertices max %d"
              % (len(sb), sb[0], sb[len(sb) // 2], sb[int(len(sb) * 0.9)], sb[-1], sum(skin_infl),
                 max(skin_verts)))
    if particle_verts:
        pv = sorted(particle_verts)
        print("particle data: %d, vertices (max particles) min/median/max %d/%d/%d" % (len(pv), pv[0],
              pv[len(pv) // 2], pv[-1]))
    print("NiOutline examples:", outline_examples)
    for c in ("NiOutline", "NiBillboardNode", "NiLODNode", "NiParticleSystemController", "NiSkinInstance",
              "NiLines", "NiParticleMeshes", "NiGeomMorpherController", "NiUVController"):
        fs = sorted(files_with[c])
        print("files with %s (%d): %s" % (c, len(fs), ", ".join(fs[:10]) + (" ..." if len(fs) > 10 else "")))
    print("parse problems:", dict(errors))


if __name__ == "__main__":
    main()
