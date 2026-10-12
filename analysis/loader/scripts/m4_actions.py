"""m4_actions.py - every lookup of the game's action table (FUN_0005df10(action) -> channel/stick) with
the accessor the result is passed to. Output: m4_actions.out

FUN_0005df10 is thiscall on the control map (player object +0x7bc, see m4_controlmap.py); the
action number is the pushed immediate in front of the call. The consumer is the next call to a
controller accessor (0x9b5xx..0x9b9xx) within the following 8 instructions."""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m4_common import MD, XBE, raw_calls, insns_before, owner_name, write_out  # noqa: E402

LOOKUP = 0x5df10
CONSUMERS = {0x9b550: "Held", 0x9b580: "Pressed", 0x9b5c0: "Released", 0x9b8d0: "HeldFree",
             0x9b990: "PressedFree", 0x9b9d0: "ValueFree", 0x9b910: "Consume", 0x9b930: "SetLock",
             0x9b600: "StickX", 0x9b630: "StickY"}
ACTIVE = {9: "DPad-L", 10: "DPad-U", 11: "DPad-D", 12: "DPad-L", 13: "DPad-R", 14: "LT", 15: "RT",
          20: "RS-Klick", 21: "LS-Klick", 22: "DPad-L", 23: "DPad-R", 24: "B", 25: "Y", 26: "LT",
          27: "WHITE", 28: "BLACK", 29: "BACK", 30: "START", 31: "A", 32: "X",
          0: "stick0", 1: "stick0", 2: "stick0", 4: "stick1", 5: "stick1", 6: "stick0", 7: "stick0",
          8: "stick1"}


def main():
    sites = raw_calls([LOOKUP])[LOOKUP]
    rows = []
    for s in sorted(sites):
        prev = insns_before(s, 4)
        act = None
        for ins in reversed(prev):
            if ins.mnemonic == "push":
                if re.fullmatch(r"0x[0-9a-f]+|\d+", ins.op_str):
                    act = int(ins.op_str, 0)
                else:
                    act = ins.op_str
                break
        cons = "-"
        n = 0
        for ins in MD.disasm(XBE.read(s + 5, 0x40), s + 5):
            n += 1
            if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                t = int(ins.op_str, 16)
                if t in CONSUMERS:
                    cons = "%s@%08x" % (CONSUMERS[t], ins.address)
                    break
                if t == LOOKUP:
                    cons = "(cmp 0xff / next lookup)"
                    break
            if n >= 8:
                break
        rows.append((s, owner_name(s), act, cons))
    lines = ["# FUN_0005df10 lookups: site, owner, action, active binding (cfg1), consumer"]
    per = collections.defaultdict(list)
    for s, own, act, cons in rows:
        lines.append("%08x %-16s action=%-4s %-9s -> %s" % (s, own, act, ACTIVE.get(act, "-"), cons))
        per[act].append("%s:%s" % (own, cons.split("@")[0]))
    lines.append("")
    lines.append("# per action")
    for act in sorted(per, key=lambda a: (isinstance(a, str), a)):
        lines.append("action %-4s %-9s %s" % (act, ACTIVE.get(act, "-"), ", ".join(sorted(set(per[act])))))
    print(write_out("m4_actions.out", "\n".join(lines) + "\n"))


if __name__ == "__main__":
    main()
