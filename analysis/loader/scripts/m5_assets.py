"""M5 audio asset inventory (read-only on the ISO; nothing is extracted).

For every RIFF/WAVE file in the .img archives: full chunk walk (RIFF-correct with pad bytes and the game's
own walk without pad bytes, SoundData::vf0 0xCB960), fmt fields, data size, duration, smpl/cue/LIST/fact
chunks and loop points, block alignment, nAvgBytesPerSec check, the game's stream decision for bStream=1
(dataSize >= raw nAvgBytesPerSec, packet = rate*blockAlign*3/10 rounded down to blockAlign) and the
silence gap the stream pump inserts at every loop (zero-padded last packet, 0xCB616..0xCB63E; the pump
loops over dataSize - dataOffset bytes, i.e. it drops the last dataOffset bytes of the data).
Also: RIFF blobs embedded in .nif files (NiUDSFileObject), and level scripts that name an I3DL2
environment (strings of Snd_EnvironmentName 0xC96E0).

Outputs: scripts/out/m5_assets.out (summary), run/analyse_m5/wav_inventory.tsv (one row per file).
"""
import collections
import os
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS  # noqa: E402
import futimg  # noqa: E402
from m5_common import Tee, out_path, run_path  # noqa: E402

ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
T = Tee(out_path("m5_assets.out"))
v = XDVDFS(ISO)
allent = list(v.walk())
imgs = sorted([e for e in allent if e[0].lower().endswith(".img")])


def rd(start, off, n):
    return v.read(start, n, off)


def chunk_walk(start, foff, fsz, pad):
    """List of (id, size, offset) walking from offset 12; pad=True rounds odd sizes up (RIFF rule)."""
    out = []
    o = 12
    guard = 0
    while o + 8 <= fsz and guard < 64:
        h = rd(start, foff + o, 8)
        cid, csz = h[:4], struct.unpack_from("<I", h, 4)[0]
        out.append((cid.decode("latin-1"), csz, o))
        o += 8 + csz + ((csz & 1) if pad else 0)
        guard += 1
    return out


def category(path):
    p = path.lower()
    if p.startswith("sounds/music/"):
        return "music"
    if p.startswith("sounds/localized/"):
        return "localized/" + p.split("/")[2]
    if "/ambient/" in p:
        return "ambient"
    if p.startswith("sounds/menu/"):
        return "menu"
    if p.startswith("sounds/footsteps/"):
        return "footsteps"
    if p.startswith("sounds/"):
        parts = p.split("/")
        return "sounds/" + (parts[1] if len(parts) > 2 else "(root)")
    return "other:" + p.split("/")[0]


rows = []
fmt_count = collections.Counter()
fmt_bytes = collections.Counter()
fmt_secs = collections.Counter()
cat_count = collections.Counter()
cat_fmt = collections.defaultdict(collections.Counter)
chunk_ids = collections.Counter()
anomalies = []
nif_riff = collections.Counter()
nif_files = 0
scripts_env = []
ENV = [b"PADDED ROOM", b"SMALL ROOM", b"LARGE ROOM", b"LARGE HALL", b"OPERA"]

for ip, istart, isize, isdir in imgs:
    hs = struct.unpack("<I", rd(istart, 0, 4))[0]
    hdr = rd(istart, 0, hs)
    for path, fsz, foff in futimg.entries(hdr):
        if fsz is None:
            continue
        lp = path.lower()
        if lp.endswith(".nif"):
            nif_files += 1
            data = rd(istart, foff, fsz)
            n = data.count(b"RIFF")
            if n:
                nif_riff[path] = n
            continue
        if lp.startswith("scripts/"):
            data = rd(istart, foff, fsz)
            hits = [e.decode() for e in ENV if e in data]
            if hits:
                scripts_env.append((ip.rsplit("/", 1)[-1], path, hits))
            continue
        if not lp.endswith(".wav"):
            continue
        h = rd(istart, foff, 12)
        if h[:4] != b"RIFF" or h[8:12] != b"WAVE":
            anomalies.append((path, "not RIFF/WAVE"))
            continue
        riff_size = struct.unpack_from("<I", h, 4)[0]
        cw = chunk_walk(istart, foff, fsz, pad=True)
        gw = chunk_walk(istart, foff, fsz, pad=False)
        ids = [c[0] for c in cw]
        for c in ids:
            chunk_ids[c] += 1
        fmtc = [c for c in cw if c[0] == "fmt "]
        datac = [c for c in cw if c[0] == "data"]
        gdata = [c for c in gw if c[0] == "data"]
        if not fmtc or not datac:
            anomalies.append((path, "missing fmt/data %s" % ids))
            continue
        fsz_fmt, fo = fmtc[0][1], fmtc[0][2]
        fb = rd(istart, foff + fo + 8, fsz_fmt)
        tag, ch, rate, avg, align, bits = struct.unpack_from("<HHIIHH", fb, 0)
        cb = struct.unpack_from("<H", fb, 16)[0] if fsz_fmt >= 18 else None
        spb = struct.unpack_from("<H", fb, 18)[0] if fsz_fmt >= 20 else None
        dsize, doff = datac[0][1], datac[0][2] + 8
        game_ok = bool(gdata) and gdata[0][2] + 8 == doff
        odd = [c for c in cw if c[1] & 1]
        if tag == 0x69:
            samples = (dsize // (36 * ch)) * 64
            exp_avg = rate * 36 * ch // 64
            fmtname = "XADPCM"
        else:
            samples = dsize // align if align else 0
            exp_avg = rate * align
            fmtname = "PCM%d" % bits
        dur = samples / rate if rate else 0
        smpl = ""
        for c in cw:
            if c[0] == "smpl":
                sb = rd(istart, foff + c[2] + 8, c[1])
                nloops = struct.unpack_from("<I", sb, 28)[0]
                loops = []
                for i in range(nloops):
                    lt, ls, le = struct.unpack_from("<III", sb, 36 + 24 * i + 4)
                    loops.append("%d:%d-%d" % (lt, ls, le))
                smpl = ";".join(loops) if loops else "smpl(no loops)"
        # game stream decision for bStream=1 (0xCBAAB..0xCBAE7) and loop gap of the stream pump
        stream = dsize >= avg
        pkt = (rate * align * 3) // 10
        pkt -= pkt % align if align else 0
        # pump 0xCB5E0..0xCB68E compares pos + dataOffset + packet against the data *size* (+0x18) and wraps
        # modulo (size - dataOffset): the loop covers the first (size - dataOffset) bytes of the data, the
        # last dataOffset bytes are never played, and the last packet is zero-padded to the packet size.
        loop_len = dsize - doff
        gap_bytes = (pkt - loop_len % pkt) % pkt if (stream and pkt) else 0
        gap_ms = (gap_bytes / align * (64 if tag == 0x69 else 1) / rate * 1000) if (stream and align) else 0
        cat = category(path)
        key = (fmtname, ch, rate)
        fmt_count[key] += 1
        fmt_bytes[key] += fsz
        fmt_secs[key] += dur
        cat_count[cat] += 1
        cat_fmt[cat][key] += 1
        if avg != exp_avg:
            anomalies.append((path, "nAvgBytesPerSec %d != expected %d" % (avg, exp_avg)))
        if align and dsize % align:
            anomalies.append((path, "data size %d not a multiple of nBlockAlign %d" % (dsize, align)))
        if not game_ok:
            anomalies.append((path, "game chunk walk (no pad) misses the data chunk: %s" % ids))
        if riff_size + 8 != fsz:
            anomalies.append((path, "RIFF size %d+8 != file size %d" % (riff_size, fsz)))
        if tag == 0x69 and (spb != 64 or align != 36 * ch or bits != 4):
            anomalies.append((path, "ADPCM fmt spb=%s align=%d bits=%d" % (spb, align, bits)))
        rows.append((ip.rsplit("/", 1)[-1], path, cat, fmtname, ch, rate, bits, align, avg, cb, spb, fsz_fmt,
                     dsize, doff, samples, "%.3f" % dur, ",".join(ids), smpl, len(odd), int(stream), pkt,
                     gap_bytes, "%.1f" % gap_ms))

hdr = ("archive\tpath\tcategory\tformat\tch\trate\tbits\tblockalign\tavgbps\tcbsize\tspb\tfmtsize\tdatasize\t"
       "dataoff\tsamples\tseconds\tchunks\tsmpl_loops\todd_chunks\tstream_if_requested\tpacket\tloopgap_bytes\t"
       "loopgap_ms")
with open(run_path("wav_inventory.tsv"), "w", encoding="utf-8") as f:
    f.write(hdr + "\n")
    for r in rows:
        f.write("\t".join(str(x) for x in r) + "\n")

T("WAV files: %d (inventory: run/analyse_m5/wav_inventory.tsv)" % len(rows))
T("\n== format x channels x rate: files, MB, minutes")
for k in sorted(fmt_count, key=lambda k: -fmt_count[k]):
    T("  %-7s ch=%d %6d Hz  %5d files  %8.1f MB  %8.1f min" % (k[0], k[1], k[2], fmt_count[k],
                                                              fmt_bytes[k] / 1e6, fmt_secs[k] / 60))
T("\n== category: files and formats")
for c in sorted(cat_count):
    T("  %-28s %5d  %s" % (c, cat_count[c], dict(cat_fmt[c])))
T("\n== chunk ids over all files:", dict(chunk_ids))
T("files with smpl chunk:", sum(1 for r in rows if r[17]))
T("files with odd-sized chunks:", sum(1 for r in rows if r[18]))
T("\n== anomalies (%d)" % len(anomalies))
kinds = collections.Counter(a[1].split(" ")[0] + " " + a[1].split(" ")[1] for a in anomalies)
for k, n in kinds.most_common():
    T("  %5d x %s" % (n, k))
for a in anomalies[:40]:
    T("   ", a)
T("\n== music / ambience (PCM) and other long files: stream decision and loop gap of the pump")
for r in rows:
    if r[2] in ("music", "ambient") or float(r[15]) > 30:
        T("  %-50s %-6s ch=%d %5d Hz %7.1f s stream=%d packet=%6d gap=%6d B = %6.1f ms" % (
            r[1], r[3], r[4], r[5], float(r[15]), r[19], r[20], r[21], float(r[22])))
loc = [r for r in rows if r[2].startswith("localized/")]
T("\n== localized speech: %d files; per language:" % len(loc))
langs = collections.Counter(r[2] for r in loc)
for l, n in sorted(langs.items()):
    sel = [r for r in loc if r[2] == l]
    T("  %-22s %4d files, %6.1f min, would stream (>=1 s): %d, formats %s" % (
        l, n, sum(float(r[15]) for r in sel) / 60, sum(r[19] for r in sel),
        dict(collections.Counter((r[3], r[4], r[5]) for r in sel))))
T("\n== NIF files: %d, with embedded RIFF: %d (blobs %d)" % (nif_files, len(nif_riff), sum(nif_riff.values())))
for p, n in sorted(nif_riff.items())[:30]:
    T("   %3d  %s" % (n, p))
T("\n== level scripts naming an I3DL2 environment (Snd_EnvironmentName strings):")
for a, p, hits in scripts_env:
    T("   %-12s %-30s %s" % (a, p, hits))
