"""Independent re-parse of the retail EU ISO and its .img archives (read-only).
- XDVDFS root sector, file list (count per dir), case of names, sizes
- .img headers parsed exactly like FUN_0013fc40 walks them (dir: next = &rel+rel, children [&rel+4, &rel+rel-4))
- simulate the game's lookup (FUN_0013fc40) for every listed path and for code literals
- compare with scripts/files_<img>_gamelist.txt (previous agent) and orig/retail_eu/*_files.txt (futimg.py)"""
import struct, sys, os, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
S = r"D:\FuturamaDecomp\analysis\loader\scripts"
v = XDVDFS(ISO)
print("base", hex(v.base), "root sector", v.root_sector, "root size", v.root_size)
allf = list(v.walk())
print("entries", len(allf), "files", sum(1 for e in allf if not e[3]), "dirs", sum(1 for e in allf if e[3]))
cnt = collections.Counter(p.rsplit("/", 1)[0] or "/" for p, s, n, d in allf)
print("per dir:", dict(cnt))
for p, s, n, d in sorted(allf):
    if p.count("/") == 1: print("  root:", "D" if d else "F", n, p)
vids = [p for p, s, n, d in allf if p.lower().startswith("/video/")]
print("video files", len(vids), "PAL:", sum(1 for p in vids if "-pal" in p.lower()), "NTSC:", sum(1 for p in vids if "-ntsc" in p.lower()))
print("non -PAL videos:", [p for p in vids if "-pal" not in p.lower()])
files = {p.lstrip("/"): (sec, n) for p, sec, n, d in allf if not d}

def walk(h, start, end, prefix, out, dirs):
    off = start
    while off < end:
        z = h.index(b"\0", off)
        raw = bytearray(h[off:z])
        if not raw: break
        nxt = off + ((z - off + 4) & ~3)
        isdir = bool(raw[0] & 0x80); raw[0] &= 0x7F
        name = raw.decode("latin-1")
        if isdir:
            rel = struct.unpack_from("<I", h, nxt)[0]
            dirs.append((prefix + name, off, nxt, rel))
            walk(h, nxt + 4, nxt + rel - 4, prefix + name + "/", out, dirs)
            off = nxt + rel
        else:
            sz, fo = struct.unpack_from("<II", h, nxt)
            out.append((prefix + name, sz, fo, off))
            off = nxt + 8
    return off

def game_lookup(h, name):
    """literal port of FUN_0013fc40 (ASCII lowercase A-Z, '/'->'\', component[0]|=0x80, stricmp)."""
    b = bytearray(name.encode("latin-1"))
    b = bytearray(c + 32 if 65 <= c <= 90 else c for c in b)
    b = b.replace(b"/", b"\\")
    def stricmp(a, c):
        la = bytes(x + 32 if 65 <= x <= 90 else x for x in a); lc = bytes(x + 32 if 65 <= x <= 90 else x for x in c)
        return la == lc
    p = 4; end = struct.unpack_from("<I", h, 0)[0]
    comps = bytes(b).split(b"\\")
    for ci, comp in enumerate(comps):
        last = ci == len(comps) - 1
        if not last:
            comp = bytes([comp[0] | 0x80]) + comp[1:] if comp else comp
        if p >= end: return None
        while True:
            z = h.index(b"\0", p); ent = h[p:z]
            nxt = p + ((z - p + 4) & ~3)
            if stricmp(ent, comp): break
            p = (nxt + struct.unpack_from("<I", h, nxt)[0]) if ent and ent[0] & 0x80 else nxt + 8
            if p >= end: return None
        if last:
            if p >= end: return None
            return struct.unpack_from("<II", h, nxt)
        rel = struct.unpack_from("<I", h, nxt)[0]
        end = nxt + rel - 4; p = nxt + 4
    return None

imgs = sorted(k for k in files if k.lower().endswith(".img"))
print("img files on disc:", imgs)
order = ["SysPAL.img"] + [f"FutPAL{i:02d}.img" for i in range(6)]
mounted = []
for nm in order:
    hit = [k for k in files if k.lower() == nm.lower()]
    if not hit:
        print("mount", nm, "-> not found (loop stops)"); 
        if nm.lower().startswith("futpal"): break
        continue
    k = hit[0]; sec, size = files[k]
    hs = struct.unpack("<I", v.read(sec, 4))[0]
    h = v.read(sec, hs)
    out, dirs = [], []
    endoff = walk(h, 4, hs, "", out, dirs)
    upper = [p for p, *_ in out if p != p.lower()] + [d[0] for d in dirs if d[0] != d[0].lower()]
    mis = sum(1 for _, s, o, _ in out if o % 0x800)
    past = sum(1 for _, s, o, _ in out if o + s > size)
    top = collections.Counter(p.split("/")[0] for p, *_ in out if "/" in p)
    rootfiles = [p for p, *_ in out if "/" not in p]
    lookfail = [p for p, s, o, _ in out if game_lookup(h, p) != (s, o)]
    print(f"== {k}: disc sector {sec}, size {size} (size%2048={size%2048}), header {hs:#x}, walk ended {endoff:#x}, files {len(out)}, dirs {len(dirs)}, "
          f"uppercase names {len(upper)} {upper[:5]}, misaligned {mis}, past-EOF {past}, game-lookup mismatches {len(lookfail)}")
    print("   top dirs:", dict(top), "root files:", rootfiles)
    if k.lower().startswith("futpal04"):
        loc = collections.Counter(p.split("/")[2] for p, *_ in out if p.startswith("sounds/localized/"))
        print("   localized:", dict(loc))
    # compare to previous agent's gamelist
    gl = os.path.join(S, f"files_{k.split('.')[0]}_gamelist.txt")
    if os.path.exists(gl):
        prev = [l.split(None, 2) for l in open(gl, encoding="latin-1") if l.strip()]
        prev = [(a[2].strip(), int(a[0]), int(a[1], 16)) for a in prev]
        mine = [(p, s, o) for p, s, o, _ in out]
        print("   gamelist identical:", prev == mine, len(prev), len(mine))
    old = os.path.join(r"D:\FuturamaDecomp\orig\retail_eu", f"{k.split('.')[0]}_files.txt")
    if os.path.exists(old):
        od = {}
        for l in open(old, encoding="latin-1"):
            a = l.split(None, 2)
            if len(a) == 3: od[(int(a[0]), int(a[1], 16))] = a[2].strip()
        diff = sum(1 for p, s, o, _ in out if od.get((s, o), "").lower() != p.lower())
        print(f"   futimg listing: {len(od)} entries, path differs {diff}")
    mounted.append((k, h, out, size))
# kanjii example
for k, h, out, size in mounted:
    if k == "SysPAL.img":
        print("SysPAL @0x4c4:", h[0x4c4:0x4d0], "u32@0x4cc", hex(struct.unpack_from('<I', h, 0x4cc)[0]), "@0x51c:", h[0x51c:0x524])
        print("lookup fonts/kanjii/kanjii.fnt:", game_lookup(h, "fonts/kanjii/kanjii.fnt"), " fonts/kanjii.fnt:", game_lookup(h, "fonts/kanjii.fnt"),
              " FONTS\KANJII\KANJII.FNT:", game_lookup(h, "FONTS\KANJII\KANJII.FNT"))
# duplicates across archives
seen = collections.defaultdict(list)
for k, h, out, size in mounted:
    for p, *_ in out: seen[p.lower()].append(k)
dups = {p: a for p, a in seen.items() if len(a) > 1}
print("paths in >1 archive:", len(dups))
dd = collections.Counter(tuple(a) for a in dups.values())
for a, n in dd.most_common(): print("  ", n, a)
print("build.txt in:", seen.get("build.txt"))
tot = sum(len(o) for _, _, o, _ in mounted); print("total files in archives", tot)
json_out = os.path.join(S, "out", "verify_files_iso_index.tsv")
os.makedirs(os.path.dirname(json_out), exist_ok=True)
with open(json_out, "w", encoding="utf-8") as f:
    f.write("archive\tpath\tsize\toffset\n")
    for k, h, out, size in mounted:
        for p, s, o, _ in out: f.write(f"{k}\t{p}\t{s}\t{o:#x}\n")
