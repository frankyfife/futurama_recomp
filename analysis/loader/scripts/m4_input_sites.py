"""m4_input_sites.py - every call site of the controller accessors (raw E8 scan, so it also finds
call sites in code Ghidra did not turn into functions), with the channel argument and the
instructions in front of the call. Output: m4_input_sites.out (grouped by caller and by channel).

Accessor names are ours (derived from the decomp of each accessor, see m4_input.md section 3)."""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m4_common import raw_calls, insns_before, owner_key, owner_name, write_out  # noqa: E402

ACCESSORS = {
    0x9b550: "Held(ch)            value>=thr",
    0x9b580: "Pressed(ch)         value>=thr && prev<thr",
    0x9b5c0: "Released(ch)        value<thr && prev>=thr",
    0x9b8d0: "HeldFree(ch)        Held && !consumed && !locked",
    0x9b990: "PressedFree(ch)     Pressed && !consumed && !locked",
    0x9b9d0: "ValueFree(ch)       value if !consumed && !locked else 0",
    0x9b910: "Consume(ch)         slot+0xc = 1 (until value<thr)",
    0x9b930: "SetLock(ch,n)       slot+0xd = n (counter)",
    0x9b960: "DecLocks(n)         all locks -= n",
    0x9b600: "StickX(i)           deadzone curve",
    0x9b630: "StickY(i)           deadzone curve",
    0x9b690: "ActivePad()         [0x2e6fa4] or pad 0",
    0x9b6b0: "SetRumble(l,r,force)",
    0x9ba40: "SetVibrationOption(on, delay)",
    0x9b790: "IsDisconnected()",
    0x9ba10: "NoPadConnected()",
    0x9bac0: "IsIdle()",
    0x9bde0: "AllIdle()",
    0x9b340: "MenuRepeat::Update()",
    0x9b2f0: "MenuRepeat::Init(period)",
    0x9b0d0: "MenuRepeat::Fire(ch)",
    0x9b0e0: "MenuRepeat::StickNeg(i,x)",
    0x9b0f0: "MenuRepeat::StickPos(i,x)",
    0x9b100: "MenuRepeat::StickNeg(i,y)",
    0x9b110: "MenuRepeat::StickPos(i,y)",
    0x9be10: "Pads::UpdateAll()",
    0x9c140: "Pads::Hotplug()",
    0x9c2a0: "Pads::Init()",
}

CH_ARG = {0x9b550, 0x9b580, 0x9b5c0, 0x9b8d0, 0x9b990, 0x9b9d0, 0x9b910, 0x9b930,
          0x9b600, 0x9b630, 0x9b0d0, 0x9b0e0, 0x9b0f0, 0x9b100, 0x9b110}


def channel_arg(prev):
    """The first stack argument: the last push before the call (thiscall pushes args right to left,
    so for one-argument accessors the last push is the channel)."""
    for ins in reversed(prev):
        if ins.mnemonic == "push":
            m = re.fullmatch(r"(0x[0-9a-f]+|\d+)", ins.op_str)
            return int(m.group(1), 0) if m else ins.op_str
    return None


def main():
    hits = raw_calls(ACCESSORS)
    lines = []
    by_caller = collections.defaultdict(list)
    by_channel = collections.defaultdict(list)
    for tgt in sorted(hits):
        for site in sorted(hits[tgt]):
            prev = insns_before(site, 6)
            ch = channel_arg(prev) if tgt in CH_ARG else None
            ctx = " | ".join("%s %s" % (i.mnemonic, i.op_str) for i in prev[-4:])
            by_caller[owner_key(site)].append((site, tgt, ch, ctx))
            if tgt in CH_ARG:
                by_channel[(tgt, ch)].append(site)
    lines.append("# Call sites per accessor (raw E8 scan)")
    for tgt in sorted(hits):
        lines.append("%08x %-48s %3d sites" % (tgt, ACCESSORS[tgt], len(hits[tgt])))
    lines.append("")
    lines.append("# Per caller (owner = Ghidra function, nofunc = code outside Ghidra functions)")
    for key in sorted(by_caller):
        rows = by_caller[key]
        name = owner_name(rows[0][0])
        lines.append("== %s" % name)
        for site, tgt, ch, ctx in rows:
            chs = "" if ch is None else ("ch=%s" % ch)
            lines.append("   %08x -> %-28s %-8s  [%s]" % (site, ACCESSORS[tgt].split()[0], chs, ctx))
    lines.append("")
    lines.append("# Per accessor and channel")
    for (tgt, ch) in sorted(by_channel, key=lambda k: (k[0], str(k[1]))):
        sites = by_channel[(tgt, ch)]
        lines.append("%-22s ch=%-6s %s" % (ACCESSORS[tgt].split()[0], ch,
                                            " ".join("%08x(%s)" % (s, owner_name(s)) for s in sites)))
    print(write_out("m4_input_sites.out", "\n".join(lines) + "\n"))


if __name__ == "__main__":
    main()
