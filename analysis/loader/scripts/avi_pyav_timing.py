"""avi_pyav_timing.py - video frame interval statistics and audio sample totals of the XMV samples."""
import os, collections, av
SAMPLES = r"D:\FuturamaDecomp\analysis\loader\samples"
for fn in sorted(f for f in os.listdir(SAMPLES) if f.endswith(".xmv")):
    c = av.open(os.path.join(SAMPLES, fn))
    vts = []; asamp = collections.Counter(); kf = 0
    for pkt in c.demux():
        for fr in pkt.decode():
            if pkt.stream.type == "video":
                vts.append(fr.pts); kf += 1 if fr.key_frame else 0
            else:
                asamp[pkt.stream.index] += fr.samples
    d = collections.Counter(b - a for a, b in zip(vts, vts[1:]))
    print(fn, "video frames", len(vts), "keyframes", kf, "pts deltas(ms):", dict(d.most_common(5)))
    for i, n in sorted(asamp.items()): print("   audio stream", i, "samples", n, "= %.3f s" % (n / 44100.0))
