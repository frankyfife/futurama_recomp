"""G1 outlines (analysis/loader/g1_outlines.md 2): QDOutline inverted-hull shells of one character file.

Reads one .ucf/.nif read-only from the ISO archive (no copy), dumps the 'QDOutline' NiLODNode (LOD ranges), the
properties of the shell shapes (NiStencilProperty draw mode, NiMaterialProperty, NiAlphaProperty flags) and
pairs every shell with its body mesh by triangle order (shell = body with welded vertices, moved along the body
normal). Usage: python g1_shells.py FutPAL00.img chars/fry-1-1.ucf"""
import collections
import math
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from xdvdfs import XDVDFS  # noqa: E402
import futimg  # noqa: E402
from m3d_interp_nifscan import blocks  # noqa: E402

ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
img, want = sys.argv[1], sys.argv[2]
v = XDVDFS(ISO)
sector = {p.lstrip("/"): st for p, st, n, d in v.walk() if not d}[img]


class ImgFile:
    pos = 0

    def seek(self, p):
        self.pos = p

    def read(self, k):
        d = v.read(sector, k, self.pos)
        self.pos += len(d)
        return d


data = next(v.read(sector, size, off) for path, size, off in futimg.entries(futimg.open_header(ImgFile()))
            if path == want)
bl = blocks(data)


def cls(i):
    return bl[i][1] if 0 <= i < len(bl) else None


def body(i):
    return bl[i][0] + 4 + len(bl[i][1])


def objnet(o):
    n = struct.unpack_from("<I", data, o)[0]
    return data[o + 4:o + 4 + n].decode("latin-1"), o + 12 + n  # name, extra data ref, controller ref


def av(i):  # NiAVObject (NIF 4.2.1, references are block indices)
    name, o = objnet(body(i))
    o += 2 + 12 + 36 + 4 + 12  # flags, translation, rotation, scale, velocity
    n = struct.unpack_from("<I", data, o)[0]
    d = dict(name=name, props=list(struct.unpack_from("<%di" % n, data, o + 4)))
    o += 4 + 4 * n
    o += 1 + (64 if data[o] else 0)  # bounding box
    if cls(i) in ("NiNode", "NiLODNode"):
        n = struct.unpack_from("<I", data, o)[0]
        d["children"] = list(struct.unpack_from("<%di" % n, data, o + 4))
        o += 4 + 4 * n
        o += 4 + 4 * struct.unpack_from("<I", data, o)[0]  # effects
        if cls(i) == "NiLODNode":  # u32 index, NiPoint3 center, u32 count, (near, far)[count]
            cnt = struct.unpack_from("<I", data, o + 16)[0]
            d["lod"] = [struct.unpack_from("<2f", data, o + 20 + 8 * k) for k in range(cnt)]
    elif cls(i) == "NiTriShape":
        d["data"], d["skin"] = struct.unpack_from("<ii", data, o)
    return d


def prop(i):
    _, o = objnet(body(i))
    r = {"flags": hex(struct.unpack_from("<H", data, o)[0])}
    o += 2
    if cls(i) == "NiMaterialProperty":
        f = struct.unpack_from("<14f", data, o)
        r.update(ambient=f[0:3], diffuse=f[3:6], emissive=f[9:12], alpha=f[13])
    elif cls(i) == "NiStencilProperty":
        r.update(enable=data[o], func_ref_mask_fail_zfail_pass_drawmode=struct.unpack_from("<7I", data, o + 1))
    return cls(i), r


def geo(i):  # NiTriShapeData 4.2.1 -> positions, normals, triangles
    o = body(i)
    n = struct.unpack_from("<H", data, o)[0]
    o += 2
    pos = [struct.unpack_from("<3f", data, o + 1 + 12 * k) for k in range(n)] if data[o] else []
    o += 1 + (12 * n if data[o] else 0)
    nrm = [struct.unpack_from("<3f", data, o + 1 + 12 * k) for k in range(n)] if data[o] else []
    o += 1 + (12 * n if data[o] else 0)
    o += 16  # center, radius
    o += 1 + (16 * n if data[o] else 0)  # vertex colors
    o += 2 + 8 * n * (struct.unpack_from("<H", data, o)[0] & 0x3F)  # UV sets
    nt = struct.unpack_from("<H", data, o)[0]
    o += 6  # triangle count, point count
    return pos, nrm, [struct.unpack_from("<3H", data, o + 6 * k) for k in range(nt)]


shapes = {i: av(i) for i, (_, c) in enumerate(bl) if c == "NiTriShape"}
for i, (_, c) in enumerate(bl):
    if c != "NiLODNode" or av(i)["name"] != "QDOutline":
        continue
    q = av(i)
    print("#%d NiLODNode 'QDOutline': LOD ranges %s, children %s" % (i, q["lod"], [cls(ch) for ch in q["children"]]))
    shells = [ch for n in q["children"] if cls(n) == "NiNode" for ch in av(n)["children"] if ch in shapes]
    for p in shapes[shells[0]]["props"] if shells else []:
        print("    shell property", prop(p))
    hist = collections.Counter()
    for sh in shells:
        pos, nrm, tri = geo(shapes[sh]["data"])
        match = None
        for b, d in shapes.items():
            if b in shells:
                continue
            bpos, bnrm, btri = geo(d["data"])
            if len(btri) == len(tri) and all(math.dist(pos[x[k]], bpos[y[k]]) < 0.06
                                             for x, y in zip(tri, btri) for k in range(3)):
                match = (b, d["name"], bpos, bnrm, btri)
                break
        if match is None:
            print("    shell #%d (%d vertices, %d triangles): no body mesh" % (sh, len(pos), len(tri)))
            continue
        b, bname, bpos, bnrm, btri = match
        cosines = []
        for x, y in zip(tri, btri):
            for k in range(3):
                dv = [pos[x[k]][j] - bpos[y[k]][j] for j in range(3)]
                length = math.sqrt(sum(c * c for c in dv))
                hist[round(length, 3)] += 1
                if length > 1e-4 and bnrm:
                    cosines.append(sum(dv[j] * bnrm[y[k]][j] for j in range(3)) / length)
        print("    shell #%d (%d vertices, %d triangles, normals %d) = body #%d '%s' (%d vertices), "
              "offset.normal avg %.3f" % (sh, len(pos), len(tri), bool(nrm), b, bname, len(bpos),
                                          sum(cosines) / len(cosines) if cosines else float("nan")))
    print("    |shell - body| per triangle corner (rounded):", sorted(hist.items()))
