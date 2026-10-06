"""M3d interpolation analysis (analysis/loader/m3d_interpolation.md 1, 4, 6): evaluates a draw capture of the
instrumented scratch build (run/analyse_m3d_interp/tree, a git-archive copy of HEAD with a capture patch in
gfx/nipc_renderer.cpp; nothing of it is in the repo).

Capture records (little endian, written by the patch at the slot entries; one frame = the records before its 'F'):
  'F' u32 frame, u64 fixed steps so far (xapi::GetGameTickStats), i64 QPC                        slot 16 (back buffer)
  'C' u32 camera (= location - 0xA0), u64 steps, u32 render target, loc[3] dir[3] up[3] right[3],
      frustum[6], port[4]                                                                          slot 47
  'D' u8 kind, u32 transform, data, skin, propertyState, target, vtable(transform - 0x70),
      parent(+0x14), u16 flags(+0xB0), u16 vertex count, u16 revision, NiTransform[16 f], NiBound[4 f],
      parent world NiTransform[16 f]; skin: u32 root, root world[16 f], u32 bones, bones x (u32, world[16 f])
                                                                                                   slots 48-51
  'S' u8 slot (54/55), u32 object                                                                  slots 54/55
Usage: python m3d_interp_capture.py <capture.bin> [--first-frame N]
Prints the statistics the report quotes."""
import argparse
import collections
import math
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])


def read_records(path):
    data = open(path, "rb").read()
    o = 0
    n = len(data)
    frame = []
    while o < n:
        tag = data[o:o + 1]
        o += 1
        if tag == b"F":
            fr, steps, qpc = struct.unpack_from("<IQq", data, o)
            o += 20
            yield dict(frame=fr, steps=steps, qpc=qpc, records=frame)
            frame = []
        elif tag == b"C":
            cam, steps, target = struct.unpack_from("<IQI", data, o)
            o += 16
            f = struct.unpack_from("<22f", data, o)
            o += 88
            frame.append(("C", dict(cam=cam, steps=steps, target=target, loc=f[0:3], dir=f[3:6], up=f[6:9],
                                    right=f[9:12], frustum=f[12:18], port=f[18:22])))
        elif tag == b"D":
            kind = data[o]
            o += 1
            transform, dat, skin, ps, target, vtable, parent = struct.unpack_from("<7I", data, o)
            o += 28
            flags, vcount, rev = struct.unpack_from("<3H", data, o)
            o += 6
            xf = struct.unpack_from("<16f", data, o)
            o += 64
            bound = struct.unpack_from("<4f", data, o)
            o += 16
            pxf = struct.unpack_from("<16f", data, o)
            o += 64
            d = dict(kind=kind, transform=transform, data=dat, skin=skin, ps=ps, target=target, vtable=vtable,
                     parent=parent, flags=flags, vcount=vcount, rev=rev, xf=xf, bound=bound, pxf=pxf)
            if skin:
                root = struct.unpack_from("<I", data, o)[0]
                o += 4
                d["root"] = root
                d["rootxf"] = struct.unpack_from("<16f", data, o)
                o += 64
                nb = struct.unpack_from("<I", data, o)[0]
                o += 4
                bones = []
                for _ in range(nb):
                    bp = struct.unpack_from("<I", data, o)[0]
                    o += 4
                    bones.append((bp, struct.unpack_from("<16f", data, o)))
                    o += 64
                d["bones"] = bones
            frame.append(("D", d))
        elif tag == b"S":
            kind, obj = struct.unpack_from("<BI", data, o)
            o += 5
            frame.append(("S", dict(kind=kind, obj=obj)))
        else:
            raise ValueError("bad tag %r at %d" % (tag, o - 1))


# ---- NiTransform helpers (layout: rotate[3][4] rows, translate at 12..14, scale at 15) ----------------------

def rot(xf):
    return ((xf[0], xf[1], xf[2]), (xf[4], xf[5], xf[6]), (xf[8], xf[9], xf[10]))


def trans(xf):
    return xf[12:15]


def det3(m):
    return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
            m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))


def ortho_err(m):
    e = 0.0
    for i in range(3):
        for j in range(3):
            d = sum(m[i][k] * m[j][k] for k in range(3)) - (1.0 if i == j else 0.0)
            e = max(e, abs(d))
    return e


def rel_angle_deg(a, b):
    """Angle of the rotation a^T b (rows are axis images, row-vector convention)."""
    # trace of a * b^T (same angle as a^T b for orthonormal matrices)
    tr = sum(a[i][k] * b[i][k] for i in range(3) for k in range(3))
    c = max(-1.0, min(1.0, (tr - 1.0) * 0.5))
    return math.degrees(math.acos(c))


def dist(p, q):
    return math.sqrt(sum((p[i] - q[i]) ** 2 for i in range(3)))


def inv_mul(a, b):
    """NiTransform inverse(a) * b (rows = axes, v' = s * v R + t). Returns (R, t, s) of the relative transform."""
    ra, rb = rot(a), rot(b)
    sa, sb = a[15], b[15]
    ta, tb = trans(a), trans(b)
    # world(v) = s v R + t ; a^-1(w) = ((w - ta) Ra^T) / sa ; rel(v) = a^-1(b(v)) = (sb v Rb + tb - ta) Ra^T / sa
    r = tuple(tuple(sum(rb[i][k] * ra[j][k] for k in range(3)) for j in range(3)) for i in range(3))
    d = tuple(tb[i] - ta[i] for i in range(3))
    t = tuple(sum(d[k] * ra[j][k] for k in range(3)) / sa for j in range(3))
    return r, t, sb / sa


def pct(values, q):
    if not values:
        return float("nan")
    v = sorted(values)
    return v[min(len(v) - 1, int(q * len(v)))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--first-frame", type=int, default=0)
    args = ap.parse_args()

    frames = [f for f in read_records(args.capture) if f["frame"] >= args.first_frame]
    print("frames: %d (%d .. %d)" % (len(frames), frames[0]["frame"], frames[-1]["frame"]))
    steps_per_frame = collections.Counter()
    for i in range(1, len(frames)):
        steps_per_frame[frames[i]["steps"] - frames[i - 1]["steps"]] += 1
    print("fixed steps per frame:", dict(sorted(steps_per_frame.items())))

    # ---- per frame counts ----------------------------------------------------------------------------------
    draws = [sum(1 for t, _ in f["records"] if t == "D") for f in frames]
    skinned = [sum(1 for t, d in f["records"] if t == "D" and d["skin"]) for f in frames]
    screens = [sum(1 for t, _ in f["records"] if t == "S") for f in frames]
    cams = [sum(1 for t, _ in f["records"] if t == "C") for f in frames]
    nz = [d for d in draws if d]
    print("draws per frame (frames with draws %d): min %d, median %d, p99 %d, max %d" % (
        len(nz), min(nz), pct(nz, 0.5), pct(nz, 0.99), max(nz)))
    print("skinned draws per frame: median %d, max %d; screen polygon/texture draws per frame: max %d" % (
        pct(skinned, 0.5), max(skinned), max(screens)))
    print("SetCameraData per frame:", dict(collections.Counter(cams)))
    bones_per_skin = collections.Counter()
    vtables = collections.Counter()
    targets = collections.Counter()
    kinds = collections.Counter()
    for f in frames:
        for t, d in f["records"]:
            if t == "D":
                vtables[d["vtable"]] += 1
                targets[d["target"]] += 1
                kinds[d["kind"]] += 1
                if d["skin"]:
                    bones_per_skin[len(d["bones"])] += 1
    print("draw kinds:", dict(kinds), "render targets:", {hex(k): v for k, v in targets.items()})
    print("object vtables:", {hex(k): v for k, v in vtables.most_common(8)})
    print("bones per skinned draw:", dict(sorted(bones_per_skin.items())))

    # ---- cameras -------------------------------------------------------------------------------------------
    cam_ids = collections.Counter()
    for f in frames:
        for t, c in f["records"]:
            if t == "C":
                cam_ids[c["cam"]] += 1
    print("cameras:", {hex(k): v for k, v in cam_ids.most_common(10)})

    # ---- keys: (transform, camera) and duplicates within a frame --------------------------------------------
    dup_frames = 0
    dup_draws = 0
    dup_differ = 0
    max_occ = 0
    for f in frames:
        cam = 0
        seen = collections.defaultdict(list)
        for t, d in f["records"]:
            if t == "C":
                cam = d["cam"]
            elif t == "D":
                seen[(d["transform"], cam)].append(d["xf"])
        dups = {k: v for k, v in seen.items() if len(v) > 1}
        if dups:
            dup_frames += 1
            for v in dups.values():
                dup_draws += len(v) - 1
                max_occ = max(max_occ, len(v))
                if any(x != v[0] for x in v[1:]):
                    dup_differ += 1
    print("keys (transform, camera) drawn more than once in a frame: %d frames, %d extra draws, max %d per key, "
          "%d with differing transforms" % (dup_frames, dup_draws, max_occ, dup_differ))

    # ---- per key history over consecutive frames --------------------------------------------------------------
    last = {}  # key -> (frame index, steps, draw)
    first_sight = []
    lost = []
    identity_change = collections.Counter()
    static = moving = 0
    same_gen_changed = 0
    same_gen_checked = 0
    same_gen_changed_keys = collections.Counter()
    rel_t = []  # |dt| / radius per tick
    abs_t = []
    angles = []
    scale_changes = 0
    ortho = []
    mirrored = 0
    scales = collections.Counter()
    cam_last = {}
    cam_moves = []
    cam_angles = []
    cam_frustum_changes = 0
    prev_keys = set()
    bone_rel_moving = 0
    bone_rel_static = 0
    root_vs_shape = []
    parent_static_child_moving = 0
    for fi, f in enumerate(frames):
        cam = 0
        keys = set()
        for t, d in f["records"]:
            if t == "C":
                cam = d["cam"]
                pc = cam_last.get(cam)
                if pc is not None and f["steps"] - pc[0] == 1:
                    c0 = pc[1]
                    cam_moves.append(dist(c0["loc"], d["loc"]))
                    a = (c0["dir"], c0["up"], c0["right"])
                    b = (d["dir"], d["up"], d["right"])
                    cam_angles.append(rel_angle_deg(a, b))
                    if c0["frustum"] != d["frustum"] or c0["port"] != d["port"]:
                        cam_frustum_changes += 1
                cam_last[cam] = (f["steps"], d)
                continue
            if t != "D":
                continue
            key = (d["transform"], cam)
            keys.add(key)
            r = rot(d["xf"])
            ortho.append(ortho_err(r))
            if det3(r) < 0:
                mirrored += 1
            scales[round(d["xf"][15], 3)] += 1
            p = last.get(key)
            if p is None or key not in prev_keys:
                first_sight.append(fi)
            if p is not None:
                pd = p[2]
                if (pd["vtable"], pd["data"], pd["skin"], pd["ps"]) != (d["vtable"], d["data"], d["skin"], d["ps"]):
                    what = []
                    if pd["vtable"] != d["vtable"]:
                        what.append("vtable")
                    if pd["data"] != d["data"]:
                        what.append("data")
                    if pd["skin"] != d["skin"]:
                        what.append("skin")
                    if pd["ps"] != d["ps"]:
                        what.append("propertyState")
                    identity_change["+".join(what)] += 1
                gap = f["steps"] - p[1]
                if gap == 0 and p[0] == fi - 1:
                    same_gen_checked += 1
                    if pd["xf"] != d["xf"]:
                        same_gen_changed += 1
                        same_gen_changed_keys[key] += 1
                if gap == 1 and p[0] == fi - 1:
                    if pd["xf"] == d["xf"]:
                        static += 1
                    else:
                        moving += 1
                        dt = dist(trans(pd["xf"]), trans(d["xf"]))
                        abs_t.append(dt)
                        rad = max(d["bound"][3], 1e-6)
                        rel_t.append(dt / rad)
                        angles.append(rel_angle_deg(rot(pd["xf"]), r))
                        if pd["xf"][15] != d["xf"][15]:
                            scale_changes += 1
                        if pd["pxf"] == d["pxf"]:
                            parent_static_child_moving += 1
                    if d["skin"] and pd["skin"] == d["skin"] and len(pd["bones"]) == len(d["bones"]):
                        for (b0, x0), (b1, x1) in zip(pd["bones"], d["bones"]):
                            rel0 = inv_mul(pd["rootxf"], x0)
                            rel1 = inv_mul(d["rootxf"], x1)
                            if rel0 == rel1:
                                bone_rel_static += 1
                            else:
                                bone_rel_moving += 1
                        root_vs_shape.append(dist(trans(d["rootxf"]), trans(d["xf"])))
            last[key] = (fi, f["steps"], d)
        lost_now = len(prev_keys - keys)
        if fi:
            lost.append(lost_now)
        prev_keys = keys
    fs_per_frame = collections.Counter(first_sight)
    fsv = [fs_per_frame.get(i, 0) for i in range(1, len(frames))]
    print("first sight per frame (from frame 2 on): median %d, p99 %d, max %d, frames with any %d" % (
        pct(fsv, 0.5), pct(fsv, 0.99), max(fsv), sum(1 for v in fsv if v)))
    print("keys lost per frame: median %d, p99 %d, max %d" % (pct(lost, 0.5), pct(lost, 0.99), max(lost)))
    big = [(frames[i]["frame"], fs_per_frame[i]) for i in sorted(fs_per_frame) if fs_per_frame[i] > 50]
    print("frames with > 50 first-sight keys:", big[:20])
    print("same key, identity fields changed between draws:", dict(identity_change))
    print("consecutive-tick pairs: static %d, moving %d (%.1f %% moving); scale changed %d; moving with static "
          "parent world %d" % (static, moving, 100.0 * moving / max(1, static + moving), scale_changes,
                               parent_static_child_moving))
    if abs_t:
        print("moving objects per tick: |dt| p50 %.4g p90 %.4g p99 %.4g max %.4g; |dt|/radius p50 %.3g p90 %.3g "
              "p99 %.3g max %.3g; rotation deg p50 %.3g p90 %.3g p99 %.3g max %.3g" % (
                  pct(abs_t, .5), pct(abs_t, .9), pct(abs_t, .99), max(abs_t), pct(rel_t, .5), pct(rel_t, .9),
                  pct(rel_t, .99), max(rel_t), pct(angles, .5), pct(angles, .9), pct(angles, .99), max(angles)))
    print("world rotations: orthonormality error p50 %.3g p99.9 %.3g max %.3g; mirrored %d; scales %s" % (
        pct(ortho, .5), pct(ortho, .999), max(ortho), mirrored, dict(scales.most_common(6))))
    if cam_moves:
        print("camera per tick: moves %d, |dloc| p50 %.4g max %.4g, angle deg p50 %.3g max %.3g, frustum/port "
              "changes %d" % (sum(1 for m in cam_moves if m > 0), pct(cam_moves, .5), max(cam_moves),
                              pct(cam_angles, .5), max(cam_angles), cam_frustum_changes))
    print("skinned: relative bone transforms (root^-1 bone) per tick static %d, moving %d; |root - shape| p50 %.3g"
          % (bone_rel_static, bone_rel_moving, pct(root_vs_shape, .5) if root_vs_shape else float("nan")))
    print("same generation (0-step frame) pairs checked %d, transform changed %d in %d keys" % (
        same_gen_checked, same_gen_changed, len(same_gen_changed_keys)))
    if same_gen_changed_keys:
        print("  keys:", [(hex(k[0]), hex(k[1]), n) for k, n in same_gen_changed_keys.most_common(8)])


if __name__ == "__main__":
    main()
