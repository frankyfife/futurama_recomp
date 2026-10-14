"""M3e-Rest (docs/m8b_report.md): static census of the disc's NIF files for the texture pipeline's multi-map inputs.

Reads the ISO archives read-only (m3d_interp_isofiles.py). Counts NiTexturingProperty blocks per map slot (base, dark,
detail, gloss, glow, bump, decals; NiTexturingProperty::vf03 0x10A4B0: NiProperty, apply mode u32, map count u32, per
map a 1-byte flag (>= 4.1.0.0) and the map (FUN_0010A3B0: link, clamp, filter, UV set, PS2 L/K = 20 bytes; the bump
map FUN_0010A440 adds luma scale/offset and a 2 x 2 matrix = 24 bytes)), apply modes, and the NiTextureEffect blocks
(names, files). Output: summary on stdout; nothing is written."""
import collections
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from m3d_interp_isofiles import iter_files  # noqa: E402
from m3d_interp_nifscan import blocks, Reader, parse_av  # noqa: E402

SLOTS = ["base", "dark", "detail", "gloss", "glow", "bump", "decal0", "decal1", "decal2", "decal3"]


def parse_texturing(r):
    r.string()  # name
    r.i32()  # extra data
    r.i32()  # controller
    flags = r.u16()
    apply = r.u32()
    count = r.u32()
    if count > 16:
        raise ValueError("map count %d" % count)
    maps = []
    for i in range(count):
        has = r.u8()
        if has > 1:
            raise ValueError("map flag %d" % has)
        if not has:
            maps.append(None)
            continue
        link = r.i32()
        clamp = r.u32()
        filt = r.u32()
        uv = r.u32()
        r.u16()
        r.u16()
        if i == 5:
            r.f(6)
        maps.append((link, clamp, filt, uv))
    return flags, apply, maps


def main():
    slot_props = collections.Counter()
    slot_files = collections.defaultdict(set)
    slot_modes = collections.defaultdict(collections.Counter)
    apply_modes = collections.Counter()
    props = 0
    errors = collections.Counter()
    effects = collections.Counter()
    effect_files = collections.defaultdict(set)
    nifs = 0
    for arc, path, data in iter_files({"nif"}):
        bl = blocks(data)
        if bl is None:
            continue
        nifs += 1
        for i, (start, cls) in enumerate(bl):
            body = start + 4 + len(cls)
            if cls == "NiTexturingProperty":
                try:
                    flags, apply, maps = parse_texturing(Reader(data, body))
                except (ValueError, struct.error, IndexError) as e:
                    errors[str(e)[:40]] += 1
                    continue
                props += 1
                apply_modes[apply] += 1
                for s, m in enumerate(maps):
                    if m is None:
                        continue
                    name = SLOTS[s] if s < len(SLOTS) else "map%d" % s
                    slot_props[name] += 1
                    slot_files[name].add(path)
                    slot_modes[name][(m[1], m[2], m[3], apply)] += 1
            elif cls == "NiTextureEffect":
                try:
                    av = parse_av(Reader(data, body))
                    effects[av["name"]] += 1
                except (ValueError, struct.error, IndexError):
                    effects["<unparsed>"] += 1
                effect_files[path].add(cls)
    print("NIF files: %d, NiTexturingProperty blocks: %d (parse errors %s)" % (nifs, props, dict(errors)))
    print("apply modes (0 REPLACE, 1 DECAL, 2 MODULATE):", dict(apply_modes))
    for name in SLOTS:
        if slot_props[name] == 0:
            print("%-7s 0 properties" % name)
            continue
        files = sorted(slot_files[name])
        print("%-7s %d properties in %d files, (clamp, filter, uv set, apply mode): %s" %
              (name, slot_props[name], len(files), dict(slot_modes[name].most_common(6))))
        for f in files[:12]:
            print("          ", f)
    print("NiTextureEffect blocks by name:", dict(effects))
    for f in sorted(effect_files)[:40]:
        print("   effect file", f)
    print("files with NiTextureEffect: %d" % len(effect_files))


if __name__ == "__main__":
    main()
