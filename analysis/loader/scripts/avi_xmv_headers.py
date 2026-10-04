"""avi_xmv_headers.py - read (never modify) the first 4 KB of every /video/*.xmv in the PAL ISO and
decode the header fields the XMV decoder in default.xbe consumes (see FUN_00264360):
 +0x00 u32 next packet size, +0x04 u32 this (first) packet size (game reads it, dec+0x2c/+0x50),
 +0x08 u32 max packet size (game XPhysicalAlloc's 2 buffers of this size)
 +0x0c magic 'xobX' (ffmpeg xmv demuxer convention), +0x10 u32 version
 +0x14 u32 width, +0x18 u32 height, +0x1c u32 duration ms (ffmpeg naming)
 +0x20 u16 audio stream count, +0x22 u16 lip-sync? (ffmpeg: 'lip sync' / reserved)
 +0x24 audio descriptors, 12 bytes each: u16 format, u16 channels, u32 rate, u16 bits, u16 flags
"""
import sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
v = XDVDFS(ISO)
for path, start, size, is_dir in sorted(v.walk()):
    if is_dir or not path.lower().endswith(".xmv"): continue
    h = v.read(start, 0x1000)
    nxt, first, mx, magic, ver, w, hgt, dur, nas, ls = struct.unpack_from("<III4sIIIIHH", h, 0)
    auds = []
    for i in range(nas):
        fmt, ch, rate, bits, fl = struct.unpack_from("<HHIHH", h, 0x24 + 12 * i)
        auds.append("%s/%dch/%dHz/%db/fl%d" % ({0x69: "XADPCM", 1: "PCM"}.get(fmt, hex(fmt)), ch, rate, bits, fl))
    print("%-30s %10d pkt(next/first/max)=%x/%x/%x %s v%d %dx%d dur=%.1fs aud=%d [%s]" % (
        path, size, nxt, first, mx, magic.decode("latin-1"), ver, w, hgt, dur / 1000.0, nas, "; ".join(auds)))
