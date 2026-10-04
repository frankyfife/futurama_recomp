"""verify_avi_pyav.py - independent re-run of the ffmpeg/PyAV decode test on the two extracted samples
(avi.md 3.4): container/codec names, decoded frame/sample counts, decode errors, video pts deltas,
and the average frame rate ffmpeg reports. Reads only loader\\samples\\*.xmv."""
import av, collections, os
SD = r"D:\FuturamaDecomp\analysis\loader\samples"
print("PyAV", av.__version__, {k: v for k, v in av.library_versions.items() if k in ("libavcodec", "libavformat")})
for fn in ("uds-PAL.xmv", "2-1-03-PAL.xmv"):
    p = os.path.join(SD, fn)
    c = av.open(p)
    print("==", fn, os.path.getsize(p), "format:", c.format.name)
    for s in c.streams:
        cc = s.codec_context
        print("   stream", s.index, s.type, cc.name, getattr(cc, "width", ""), getattr(cc, "height", ""),
              getattr(cc, "sample_rate", ""), getattr(cc, "channels", "") if hasattr(cc, "channels") else "",
              "avg_rate=%s" % getattr(s, "average_rate", None))
    counts = collections.Counter(); samples = collections.Counter(); errs = 0; pts = []
    for pkt in c.demux():
        try:
            for fr in pkt.decode():
                counts[pkt.stream.index] += 1
                if pkt.stream.type == "audio": samples[pkt.stream.index] += fr.samples
                elif fr.pts is not None: pts.append(fr.pts)
        except av.error.FFmpegError:
            errs += 1
    d = collections.Counter(b - a for a, b in zip(pts, pts[1:]))
    print("   frames:", dict(counts), "audio samples:", dict(samples), "decode errors:", errs)
    print("   video pts deltas(ms):", dict(d))
    c.close()
