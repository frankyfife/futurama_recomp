"""RIFF/WAVE blobs embedded in .nif files (NiUDSFileObject, loaded via SoundMgr vf5 0xCCE40 /
SoundData vf10 0xCB450 / vf11 0xCB720). Read-only on the ISO.
Output: scripts/out/m5_nif_audio.out (summary), run/analyse_m5/nif_wav_inventory.tsv."""
import collections
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS  # noqa: E402
import futimg  # noqa: E402
from m5_common import Tee, out_path, run_path  # noqa: E402

ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
T = Tee(out_path("m5_nif_audio.out"))
v = XDVDFS(ISO)
rows = []
fmts = collections.Counter()
secs = collections.Counter()
bydir = collections.defaultdict(collections.Counter)
bad = 0
for ip, istart, isize, isdir in sorted(v.walk()):
    if isdir or not ip.lower().endswith(".img"):
        continue
    hs = struct.unpack("<I", v.read(istart, 4))[0]
    for path, fsz, foff in futimg.entries(v.read(istart, hs)):
        if fsz is None or not path.lower().endswith(".nif"):
            continue
        data = v.read(istart, fsz, foff)
        i = data.find(b"RIFF")
        while i >= 0:
            ok = data[i + 8:i + 12] == b"WAVE"
            if ok:
                rsize = struct.unpack_from("<I", data, i + 4)[0]
                o = i + 12
                fmt = None
                dsize = None
                while o + 8 <= min(len(data), i + 8 + rsize):
                    cid = data[o:o + 4]
                    csz = struct.unpack_from("<I", data, o + 4)[0]
                    if cid == b"fmt ":
                        fmt = struct.unpack_from("<HHIIHH", data, o + 8)
                    elif cid == b"data":
                        dsize = csz
                        break
                    o += 8 + csz
                if fmt and dsize is not None:
                    tag, ch, rate, avg, align, bits = fmt
                    smp = (dsize // (36 * ch)) * 64 if tag == 0x69 else (dsize // align if align else 0)
                    dur = smp / rate if rate else 0
                    name = "XADPCM" if tag == 0x69 else "PCM%d" % bits
                    key = (name, ch, rate)
                    fmts[key] += 1
                    secs[key] += dur
                    d = path.rsplit("/", 1)[0]
                    bydir[d.split("/")[0] + "/" + path.rsplit("/", 1)[-1].split("_")[0]][key] += 1
                    rows.append((ip.rsplit("/", 1)[-1], path, i, name, ch, rate, dsize, "%.3f" % dur))
                else:
                    bad += 1
            i = data.find(b"RIFF", i + 4)
with open(run_path("nif_wav_inventory.tsv"), "w", encoding="utf-8") as f:
    f.write("archive\tnif\toffset\tformat\tch\trate\tdatasize\tseconds\n")
    for r in rows:
        f.write("\t".join(str(x) for x in r) + "\n")
T("embedded WAVs: %d in %d NIFs (unparsed RIFF: %d)" % (len(rows), len(set(r[1] for r in rows)), bad))
for k in sorted(fmts, key=lambda k: -fmts[k]):
    T("  %-7s ch=%d %6d Hz %5d blobs %7.1f min" % (k[0], k[1], k[2], fmts[k], secs[k] / 60))
T("by NIF name prefix:")
for d in sorted(bydir):
    T("  %-40s %s" % (d, dict(bydir[d])))
