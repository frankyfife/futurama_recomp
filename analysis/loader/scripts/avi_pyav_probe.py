"""avi_pyav_probe.py - check whether PyAV (ffmpeg) can demux/decode the XMV samples.
Prints container/stream/codec info, decodes all video frames and audio frames, reports counts,
pixel formats, timestamps and decode errors. Saves the first decoded video frame as PNG if PIL exists."""
import sys, os, traceback
import av
SAMPLES = r"D:\FuturamaDecomp\analysis\loader\samples"
print("PyAV", av.__version__, "| libav", getattr(av, "library_versions", {}))
for fn in sorted(os.listdir(SAMPLES)):
    if not fn.lower().endswith(".xmv"): continue
    p = os.path.join(SAMPLES, fn)
    print("=" * 10, fn)
    try:
        c = av.open(p)
    except Exception as e:
        print("open failed:", e); continue
    print("format:", c.format.name, "|", c.format.long_name, "| duration(us):", c.duration)
    for s in c.streams:
        cc = s.codec_context
        extra = ""
        if s.type == "video": extra = "%dx%d pix=%s" % (cc.width, cc.height, cc.pix_fmt)
        if s.type == "audio": extra = "rate=%s ch=%s fmt=%s" % (cc.sample_rate, getattr(cc, "channels", None) or cc.layout.nb_channels, cc.format.name if cc.format else None)
        print("  stream #%d %s codec=%s (%s) tag=%r tb=%s %s" % (s.index, s.type, cc.name, cc.codec.long_name, s.codec_tag, s.time_base, extra))
    counts = {}; errs = {}; first = {}; last = {}
    saved = False
    for pkt in c.demux():
        try:
            for fr in pkt.decode():
                i = pkt.stream.index
                counts[i] = counts.get(i, 0) + 1
                first.setdefault(i, fr.time); last[i] = fr.time
                if pkt.stream.type == "video" and not saved:
                    try:
                        fr.to_image().save(os.path.join(SAMPLES, fn + ".frame0.png")); saved = True
                    except Exception as e:
                        print("  (png save skipped:", e, ")"); saved = True
        except Exception as e:
            errs[pkt.stream.index] = errs.get(pkt.stream.index, 0) + 1
            if errs[pkt.stream.index] <= 2: print("  decode error stream", pkt.stream.index, type(e).__name__, e)
    for i in sorted(set(counts) | set(errs)):
        print("  stream #%d decoded frames=%d errors=%d t=[%s..%s]" % (i, counts.get(i, 0), errs.get(i, 0), first.get(i), last.get(i)))
