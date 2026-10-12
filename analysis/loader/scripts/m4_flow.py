"""m4_flow.py - disassembly of the code ranges the M4 input analysis cites (m4_input.md) and the
constants they use, in one file for review: controller update (vtable 0x2A14F0[0], no Ghidra
function), main menu update (vtable 0x2940D4[1]), Ingame session update (pause/cutscene skip),
Intro skip, jump/LT handlers, first-person toggle, rumble effect queue, original XPP
XInputGetState/XInputSetState. Output: m4_flow.out"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m4_common import dis, f32, u32, cstr, write_out  # noqa: E402

RANGES = [
    (0x9C3D0, 0x9C9AE, "controller update (vtable 0x2A14F0[0]); XINPUT_STATE at esp+8"),
    (0x9C140, 0x9C215, "hotplug FUN_0009C140"),
    (0x9C220, 0x9C29B, "rumble send FUN_0009C220"),
    (0x9C2A0, 0x9C3B3, "init FUN_0009C2A0"),
    (0x20580, 0x20806, "main menu update (vtable 0x2940D4 slot 1)"),
    (0x2AAB0, 0x2ADDF, "Ingame session update (cutscene skip, pause, idle auto-pause)"),
    (0x2B9B0, 0x2BA10, "Intro::Update input part (skip with START/A)"),
    (0x50D10, 0x50E73, "jump handler (vtable +0x104, action 24)"),
    (0x50E80, 0x50EF3, "LT handler (vtable +0x108, action 26)"),
    (0x588B0, 0x58970, "first-person toggle / camera input (vtable +0x11C, bit 0x20 = action 20)"),
    (0x5EF60, 0x5EFE0, "SetRumbleMute / AddRumbleEffect FUN_0005EF70"),
    (0x28A59, 0x28AF0, "options menu: vibration test rumble fade-out"),
    (0x2977C, 0x297D4, "options menu: vibration switched on -> SetRumble(1,1)"),
    (0x28CEBE, 0x28CF2A, "original XPP XInputGetState"),
    (0x28CF2A, 0x28CF5D, "original XPP XInputSetState"),
]

CONSTS = [
    (0x2A1500, "f", "button threshold of the derived ctor (30/256)"),
    (0x2A1504, "f", "1/256 (analog button scale, base threshold)"),
    (0x2A14FC, "f", "1/32768 (stick scale)"),
    (0x293A88, "f", "0.4 (stick threshold for 'active pad' and menus)"),
    (0x2A14F8, "f", "65535 (motor scale)"),
    (0x2D6748, "f", ".data dt (0.02; 1/60 with the M3b patch)"),
    (0x2D5BB8, "f", "0.33 s (release of the active pad while idle)"),
    (0x2951D8, "f", "1800 s (Ingame auto-pause after all pads idle)"),
    (0x2D659C, "b", "vibration gate (runtime), .data default"),
    (0x2D65A0, "4u", "port table for XInputOpen"),
    (0x2D6D18, "s", "unskippable-first-play movie 1"),
    (0x2D6D24, "s", "unskippable-first-play movie 2"),
    (0x2940CC, "c", "context name used by FUN_00020090"),
    (0x2940C0, "c", "level name used by FUN_00020090"),
]


def main():
    out = []
    for lo, hi, what in RANGES:
        out.append("==== %08x..%08x  %s" % (lo, hi, what))
        out.append(dis(lo, hi - lo))
        out.append("")
    out.append("==== constants")
    for a, kind, what in CONSTS:
        if kind == "f":
            v = "%r (0x%08x)" % (f32(a), u32(a))
        elif kind == "b":
            v = "%d" % (u32(a) & 0xFF)
        elif kind == "4u":
            v = str([u32(a + 4 * i) for i in range(4)])
        elif kind == "s":
            v = repr(cstr(u32(a)))
        else:
            v = repr(cstr(a))
        out.append("%08x = %s   ; %s" % (a, v, what))
    print(write_out("m4_flow.out", "\n".join(out) + "\n"))


if __name__ == "__main__":
    main()
