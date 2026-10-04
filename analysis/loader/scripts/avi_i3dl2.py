"""avi_i3dl2.py - decode the DSI3DL2LISTENER presets passed to IDirectSound_SetI3DL2Listener
(field order per XDK dsound.h: lRoom,lRoomHF,flRoomRolloffFactor,flDecayTime,flDecayHFRatio,
lReflections,flReflectionsDelay,lReverb,flReverbDelay,flDiffusion,flDensity,flHFReference)."""
import sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
names = {0x2d6884: "PADDED ROOM (idx0, also init)", 0x2d68b8: "SMALL ROOM (idx1)", 0x2d6920: "LARGE ROOM (idx2)",
         0x2d6954: "LARGE HALL (idx3)", 0x2d69bc: "OPERA (idx4)"}
for va, n in names.items():
    v = struct.unpack("<iifffififfff", x.read(va, 48))
    print("%08x %-30s %s" % (va, n, ", ".join(("%g" % f) for f in v)))
