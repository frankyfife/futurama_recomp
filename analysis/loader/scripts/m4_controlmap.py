"""m4_controlmap.py - the game's action -> channel tables (player object +0x7bc).

FUN_0005e220 (constructor, only caller FUN_0005d660) writes 10 configurations of 0x21 bytes each:
config N = bytes N*0x21 .. N*0x21+0x20 of the object, config 0 = the active one (FUN_0005df80
copies config N into 0; FUN_0005e220 ends with FUN_0005df80(1)). FUN_0005dfc0(dst, src) copies
config src into dst. Lookup: FUN_0005df10(action) = byte[action % 0x21] of the active config.

The script replays the byte stores of FUN_0005e220 from its disassembly (capstone), applies the
copies and prints the three distinct presets. Output: m4_controlmap.out"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m4_common import MD, XBE, write_out  # noqa: E402

START, END = 0x5e220, 0x5e220 + 860
CHANNEL = {0: "DPad-L", 1: "DPad-R", 2: "DPad-U", 3: "DPad-D", 4: "X", 5: "B", 6: "Y", 7: "A",
           8: "LT", 9: "RT", 10: "START", 11: "BACK", 12: "LS-Klick", 13: "RS-Klick",
           14: "WHITE", 15: "BLACK", 0xff: "-"}


def replay():
    mem = bytearray(0x300)
    reg = {}
    regval = {}
    calls = []
    pending = []
    for ins in MD.disasm(XBE.read(START, END - START), START):
        if ins.address >= END:
            break
        m = re.fullmatch(r"byte ptr \[(\w+)(?: \+ (0x[0-9a-f]+|\d+))?\], (0x[0-9a-f]+|\d+|\w+)", ins.op_str)
        if ins.mnemonic == "mov" and m:
            base, off, val = m.group(1), int(m.group(2) or "0", 0), m.group(3)
            if base in reg:
                off += reg[base]
            v = int(val, 0) if re.fullmatch(r"0x[0-9a-f]+|\d+", val) else regval.get(val)
            if v is None:
                raise SystemExit("unknown source %s at %08x" % (val, ins.address))
            mem[off] = v & 0xff
            continue
        # track constant registers (xor r,r / mov r8,imm) so stores from bl/al resolve
        m2 = re.fullmatch(r"(\w+), (\w+)", ins.op_str)
        if ins.mnemonic == "xor" and m2 and m2.group(1) == m2.group(2):
            r = m2.group(1)
            regval[r] = 0
            regval[r[1] + "l"] = 0
        if ins.mnemonic == "mov" and m2 and m2.group(1) in ("al", "bl", "cl", "dl") \
                and re.fullmatch(r"0x[0-9a-f]+|\d+", m2.group(2)):
            regval[m2.group(1)] = int(m2.group(2), 0)
        m = re.fullmatch(r"(\w+), (\w+)", ins.op_str)
        if ins.mnemonic == "mov" and m and m.group(2) == "ecx" and m.group(1) in ("esi", "edi", "ebx", "ebp"):
            reg[m.group(1)] = 0
        if ins.mnemonic == "push" and re.fullmatch(r"0x[0-9a-f]+|\d+", ins.op_str):
            pending.append(int(ins.op_str, 0))
        if ins.mnemonic == "call":
            calls.append((ins.address, int(ins.op_str, 16), list(reversed(pending))))
            pending = []
    return mem, calls


def main():
    mem, calls = replay()
    out = []
    for site, tgt, args in calls:
        out.append("call %08x -> %08x args %s" % (site, tgt, args))
        if tgt == 0x5dfc0:
            dst, src = args[0], args[1]
            mem[dst * 0x21:(dst + 1) * 0x21] = mem[src * 0x21:(src + 1) * 0x21]
        elif tgt == 0x5df80:
            n = args[0]
            mem[0:0x21] = mem[n * 0x21:(n + 1) * 0x21]
    out.append("")
    out.append("action | " + " | ".join("cfg%d" % c for c in range(10)))
    for a in range(0x21):
        row = []
        for c in range(10):
            v = mem[c * 0x21 + a]
            row.append(("stick%d" % v if v != 0xff else "-") if a < 9 else CHANNEL.get(v, "%d" % v))
        out.append("%6d | %s" % (a, " | ".join("%-8s" % r for r in row)))
    print(write_out("m4_controlmap.out", "\n".join(out) + "\n"))


if __name__ == "__main__":
    main()
