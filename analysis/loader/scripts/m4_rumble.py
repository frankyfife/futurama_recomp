"""m4_rumble.py - every call of SetRumble FUN_0009b6b0(this=pad, float left, float right, bool force),
of SetVibrationOption FUN_0009ba40(bool on, float delay), of the player rumble queue
AddRumbleEffect FUN_0005ef70(this=player+?, left, right, duration, shape 0..3) and SetRumbleMute
FUN_0005ef60, with the instructions in front of the call
(arguments are pushed right to left: the last push before the call is the first argument) and the
float values of pushed immediates. Output: m4_rumble.out"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m4_common import raw_calls, insns_before, owner_name, write_out  # noqa: E402

TARGETS = {0x9b6b0: "SetRumble(left,right,force)", 0x9ba40: "SetVibrationOption(on,delay)",
           0x5ef70: "AddRumbleEffect(left,right,duration,shape)", 0x5ef60: "SetRumbleMute(on)"}


def fmt(ins):
    s = "%s %s" % (ins.mnemonic, ins.op_str)
    m = re.fullmatch(r"push (0x[0-9a-f]{6,8})", s)
    if m:
        v = int(m.group(1), 16)
        f = struct.unpack("<f", struct.pack("<I", v))[0]
        s += "  (=%gf)" % f
    m = re.search(r"dword ptr \[(0x[0-9a-f]+)\]", s)
    if m:
        from m4_common import f32, XBE
        a = int(m.group(1), 16)
        if XBE.read(a, 4) is not None:
            s += "  ([%s]=%gf)" % (m.group(1), f32(a))
    return s


def main():
    hits = raw_calls(TARGETS)
    lines = []
    for tgt in TARGETS:
        lines.append("# %s: %d call sites" % (TARGETS[tgt], len(hits[tgt])))
        for s in sorted(hits[tgt]):
            prev = insns_before(s, 12)
            lines.append("== %08x in %s" % (s, owner_name(s)))
            for ins in prev:
                lines.append("     %08x  %s" % (ins.address, fmt(ins)))
        lines.append("")
    print(write_out("m4_rumble.out", "\n".join(lines) + "\n"))


if __name__ == "__main__":
    main()
