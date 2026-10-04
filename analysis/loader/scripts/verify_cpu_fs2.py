"""verify_cpu_fs2.py - recount fs_sites.tsv (key facts 1, 2, 6, 7) from the raw bytes:
 * offsets / encodings / disp position for the patch (+2/+3/+4) of the 38 non-zero sites
 * fs:[0] owners: distinct 'func' values, distinct Ghidra functions (inside), gap pieces, per lib
 * fs:[0] in D3D/DSOUND/XMV/XPP?  gs: anywhere in fs_sites?
 * the 3 absolute fs/gs hits that verify_cpu_fs.py found outside Ghidra bodies: are they inside a
   decodable instruction of surrounding code (i.e. artefacts)?"""
import sys, collections, struct
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *

rows = []
for l in open(LOADER + r"\fs_sites.tsv", encoding="utf-8"):
    p = l.rstrip("\n").split("\t")
    if p[0] == "addr":
        hdr = p
        continue
    rows.append(dict(zip(hdr, p)))
print("rows:", len(rows))
bad = 0
for r in rows:
    a = int(r["addr"], 16)
    b = bytes.fromhex(r["bytes"])
    if X.read(a, len(b)) != b:
        bad += 1
print("rows whose bytes differ from the XBE:", bad)

nz = [r for r in rows if r["offset"] != "0x0"]
print("non-zero rows:", len(nz), collections.Counter(r["offset"] for r in nz))
print("\nnon-zero: disp position check (disp32 == offset at the claimed +2/+3/+4):")
pos_ok = 0
for r in nz:
    b = bytes.fromhex(r["bytes"])
    off = int(r["offset"], 16)
    hits = [k for k in range(len(b) - 3) if struct.unpack_from("<I", b, k)[0] == off and k + 4 == len(b)]
    i = dis(int(r["addr"], 16), 1)[0]
    # capstone detail for disp offset
    di = list(md.disasm(b, int(r["addr"], 16), 1))[0]
    dpos = di.disp_offset
    ok = hits == [dpos] and b[0] == 0x64
    pos_ok += ok
    print("  %s %-18s len=%d disp@+%d %s  %s" % (r["addr"], r["bytes"], len(b), dpos, "OK" if ok else "MISMATCH", r["func"]))
print("disp positions consistent:", pos_ok, "/", len(nz))
enc = collections.Counter(bytes.fromhex(r["bytes"])[:-4].hex() for r in nz)
print("non-zero encodings:", enc)

z = [r for r in rows if r["offset"] == "0x0"]
print("\nfs:[0] rows:", len(z))
print("  by section:", collections.Counter(r["section"] for r in z))
print("  by seg:", collections.Counter(r["seg"] for r in rows))
print("  distinct 'func' column values:", len(set(r["func"] for r in z)))
inside = set(r["func"] for r in z if r["inside"] == "True")
gaps = set(r["func"] for r in z if r["inside"] != "True")
print("  distinct func with inside=True:", len(inside), " distinct func values with inside=False:", len(gaps))
# independent owner (Ghidra function body that contains the site)
own = collections.Counter()
own_set = set()
nogh = 0
for r in z:
    o = owner(int(r["addr"], 16))
    if o and o[3]:
        own_set.add(o[0])
    else:
        nogh += 1
print("  independent: distinct Ghidra functions containing fs:[0] sites:", len(own_set), " sites outside any body:", nogh)
lib = collections.Counter(r["lib"] for r in z)
print("  sites by lib column:", lib)
libf = collections.defaultdict(set)
for r in z:
    libf[r["lib"]].add(r["func"])
print("  distinct func per lib:", {k: len(v) for k, v in libf.items()})
# names: Ni* functions counted as game/unknown?
ni_in_game = sum(1 for r in z if r["lib"] == "game/unknown" and r["func"].startswith("Ni"))
print("  game/unknown rows whose func starts with 'Ni':", ni_in_game)
print("  NetImmerse rows whose func does not contain 'Ni':", sum(1 for r in z if r["lib"] == "NetImmerse" and "Ni" not in r["func"]))

print("\n== the 3 out-of-body absolute fs/gs byte hits from verify_cpu_fs.out")
for a in (0x1dac8, 0xab34f, 0x17c80e):
    o = owner(a)
    print("-- %x owner=%s inside=%s" % (a, o[2], o[3]))
    # find a decode start in the 32 bytes before that runs through 'a' without landing on it
    for back in range(1, 40):
        ins = dis_range(a - back, a + 8)
        starts = [i.address for i in ins]
        if ins and a not in starts and ins[-1].address + ins[-1].size >= a:
            pass
    show(a - 0x14, a + 0x10)
