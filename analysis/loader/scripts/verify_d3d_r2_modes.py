"""verify_d3d_r2_modes.py - dump the CMiniport display mode table 0x242910 (0xb9 entries x 12
bytes, bound 0x8ac in FUN_0023b09f) for 640x480 entries (review pass 2).
entry: dw0 = flags (low byte AV-pack/standard group, 0xff00 group, 0x400000/0x800000
refresh-class bits tested against caps & 0xc00000), w0/w1 of dw1 = width/height, dw2 = mode value."""
from verify_d3d_r2_common import *
T = 0x242910
n = 0x8ac // 12
print("entries:", n)
for i in range(n):
    d0, d1, d2 = u32(T + 12 * i), u32(T + 12 * i + 4), u32(T + 12 * i + 8)
    w, h = d1 & 0xffff, d1 >> 16
    if (w, h) == (640, 480):
        print("  #%3d flags=%08x  %dx%d  val=%08x  b22=%d b23=%d b21=%d b16=%d" % (i, d0, w, h, d2,
              (d0 >> 22) & 1, (d0 >> 23) & 1, (d0 >> 21) & 1, (d0 >> 16) & 1))
