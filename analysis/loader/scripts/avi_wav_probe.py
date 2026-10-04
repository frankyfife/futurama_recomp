"""avi_wav_probe.py - read (in memory only, nothing written) a few game WAVs out of the .img
archives inside the PAL ISO and check how ffmpeg/PyAV decodes Xbox ADPCM (format tag 0x69):
codec name, decoded samples vs. data_bytes/36/ch*64 (Xbox: 64 samples per 36-byte block/channel)."""
import sys, io, struct
import av
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS, SECTOR
from futimg import entries
v = XDVDFS(r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso")
WANT = {"sounds/button.wav", "sounds/footsteps/gravel1.wav", "sounds/theme22.wav", "sounds/misc12.wav"}
for p, s, z, d in sorted(v.walk()):
    if not p.lower().endswith(".img"): continue
    v.f.seek(v.base + s * SECTOR); hs = struct.unpack("<I", v.f.read(4))[0]
    v.f.seek(v.base + s * SECTOR); hdr = v.f.read(hs)
    for name, fsz, foff in entries(hdr):
        if fsz is None or name.lower() not in WANT: continue
        v.f.seek(v.base + s * SECTOR + foff); data = v.f.read(fsz)
        o = 12; fmt = None; dlen = None
        while o + 8 <= len(data):
            cid, cl = data[o:o + 4], struct.unpack_from("<I", data, o + 4)[0]
            if cid == b"fmt ": fmt = struct.unpack_from("<HHIIHH", data, o + 8); fext = data[o + 8:o + 8 + cl]
            if cid == b"data": dlen = cl
            o += 8 + cl + (cl & 1)
        tag, ch, rate, avg, ba, bits = fmt
        try:
            c = av.open(io.BytesIO(data), format="wav")
            st = c.streams.audio[0]
            n = sum(fr.samples for pkt in c.demux(st) for fr in pkt.decode())
            codec = st.codec_context.name
        except Exception as e:
            codec, n = "ERR %s" % e, -1
        exp = (dlen // (36 * ch)) * 64 if tag == 0x69 else dlen // (ch * bits // 8)
        print("%-32s tag=%#x ch=%d rate=%d blockalign=%d bits=%d fmtlen=%d data=%d | ffmpeg codec=%s samples=%d | expected(64/blk)=%d" % (
            name, tag, ch, rate, ba, bits, len(fext), dlen, codec, n, exp))
