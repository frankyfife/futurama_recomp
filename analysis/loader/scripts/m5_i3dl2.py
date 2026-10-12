"""I3DL2 listener presets of the game (table 0x2D6818, 24 entries x 0x34 bytes: name pointer + 12-field
DSI3DL2LISTENER) and their XAudio2 equivalent.

The table and the five entries SetEnvironment (0xCBFA0, jump table 0xCC010) uses are read from the XBE.
`to_native` is a Python port of the Windows SDK inline function ReverbConvertI3DL2ToNative (xaudio2fx.h,
XAudio2 2.9, sevenDotOneReverb = TRUE) so the document can show the native parameters; the host calls the
SDK function itself. WetDryMix = 100 because the reverb sits on a send submix (wet only).
Output: scripts/out/m5_i3dl2.out"""
import math
import struct

from m5_common import X, Tee, out_path, u32, cstr

T = Tee(out_path("m5_i3dl2.out"))
FIELDS = ["lRoom", "lRoomHF", "flRoomRolloffFactor", "flDecayTime", "flDecayHFRatio", "lReflections",
          "flReflectionsDelay", "lReverb", "flReverbDelay", "flDiffusion", "flDensity", "flHFReference"]


def entry(va):
    vals = struct.unpack("<iifffifif fff".replace(" ", ""), X.read(va, 48))
    return dict(zip(FIELDS, vals))


def to_native(p, wet=100.0):
    n = {}
    n["WetDryMix"] = wet
    n["RearDelay"] = 20       # XAUDIO2FX_REVERB_DEFAULT_7POINT1_REAR_DELAY
    n["SideDelay"] = 5        # XAUDIO2FX_REVERB_DEFAULT_7POINT1_SIDE_DELAY
    n["PositionLeft"] = n["PositionRight"] = 6
    n["PositionMatrixLeft"] = n["PositionMatrixRight"] = 27
    n["RoomSize"] = 100.0
    n["LowEQCutoff"], n["HighEQCutoff"] = 4, 6
    n["RoomFilterMain"] = p["lRoom"] / 100.0
    n["RoomFilterHF"] = p["lRoomHF"] / 100.0
    r = p["flDecayHFRatio"]
    if r >= 1.0:
        idx = int(-4.0 * math.log10(r))
        idx = max(idx, -8)
        n["LowEQGain"] = idx + 8 if idx < 0 else 8
        n["HighEQGain"] = 8
        n["DecayTime"] = p["flDecayTime"] * r
    else:
        idx = int(4.0 * math.log10(r))
        idx = max(idx, -8)
        n["LowEQGain"] = 8
        n["HighEQGain"] = idx + 8 if idx < 0 else 8
        n["DecayTime"] = p["flDecayTime"]
    rd = p["flReflectionsDelay"] * 1000.0
    rd = 299.0 if rd >= 300 else (1.0 if rd <= 1 else rd)
    n["ReflectionsDelay"] = int(rd)
    vd = p["flReverbDelay"] * 1000.0
    vd = 84.0 if vd >= 85 else vd
    n["ReverbDelay"] = int(vd)
    n["ReflectionsGain"] = p["lReflections"] / 100.0
    n["ReverbGain"] = p["lReverb"] / 100.0
    n["EarlyDiffusion"] = n["LateDiffusion"] = int(15.0 * p["flDiffusion"] / 100.0)
    n["Density"] = p["flDensity"]
    n["RoomFilterFreq"] = p["flHFReference"]
    n["DisableLateField"] = 0
    return n


T("I3DL2 table 0x2D6818 (24 entries, 0x34 bytes each):")
table = {}
for i in range(24):
    va = 0x2D6818 + 0x34 * i
    name = cstr(u32(va))
    p = entry(va + 4)
    table[va + 4] = (name, p)
    T("  [%2d] @%08x %-16s %s" % (i, va + 4, name, ", ".join("%g" % p[f] for f in FIELDS)))
used = [(0, 0x2D6884), (1, 0x2D68B8), (2, 0x2D6920), (3, 0x2D6954), (4, 0x2D69BC)]
T("\nPresets used by SetEnvironment(0..4) (0xCBFB0..0xCC007) and the init (0xCCDC1 -> index 0):")
for idx, va in used:
    name, p = table[va]
    T("  SetEnvironment(%d) -> %s @%08x" % (idx, name, va))
    nat = to_native(p)
    T("     native: " + ", ".join("%s=%s" % (k, ("%g" % v) if isinstance(v, float) else v) for k, v in nat.items()))
# check against the jump table of SetEnvironment
jt = [u32(0xCC010 + 4 * i) for i in range(5)]
T("\njump table 0xCC010:", " ".join("%x" % a for a in jt))
