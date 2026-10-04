"""Round-2: dump the DSOUND I3DL2 preset table around 0x2d681c (0x34-byte
entries: const char* name + DSI3DL2LISTENER 12 fields) and mark the 5
presets used by 0xcbfa0 / 0xccd00. Compares against the standard I3DL2
preset values (reference list typed in below from the public I3DL2 /
XAudio2 preset macros; used only as a cross-check)."""
import struct
from verify_avi_r2_common import X, Tee
T = Tee("verify_avi_r2_i3dl2.out")
USED = {0x2d6884: 0, 0x2d68b8: 1, 0x2d6920: 2, 0x2d6954: 3, 0x2d69bc: 4}
# start: walk back from first used preset in 0x34 steps while name ptr is a string
a = 0x2d6884 - 4
while True:
    p = X.u32(a - 0x34)
    s = X.cstr(p) if X.section(p) in (".rdata", ".data") else None
    if not s or not s.isprintable():
        break
    a -= 0x34
n = 0
while True:
    p = X.u32(a)
    s = X.cstr(p) if X.section(p) in (".rdata", ".data") else None
    if not s or not s.isprintable() or len(s) > 40:
        break
    f = struct.unpack("<iifffififfff", X.read(a + 4, 48))
    # field order (XDK DSI3DL2LISTENER, Vermutung): lRoom lRoomHF flRoomRolloffFactor
    # flDecayTime flDecayHFRatio lReflections flReflectionsDelay lReverb flReverbDelay
    # flDiffusion flDensity flHFReference
    mark = "USED idx %d" % USED[a + 4] if (a + 4) in USED else ""
    T("%08x %-22s %s %s" % (a + 4, s, ", ".join(("%g" % x) for x in f), mark))
    a += 0x34
    n += 1
T("entries:", n)
