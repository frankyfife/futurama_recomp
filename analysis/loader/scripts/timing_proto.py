"""Retail EU vs. NTSC prototype (17 June 2003) for the timing-relevant code (timing60.md, section 5).

For each retail function/gap segment of interest the script builds a byte pattern in which absolute addresses
(imm/disp values inside the image, 0x10000..0x330000) and rel32 branch targets are wildcards, and searches it in
the prototype .text. "identical" = same instructions and constants, only relocated addresses differ.
Also prints the prototype's timing init (0xA2620, the counterpart of 0xA26A0) and its .data initial fps/dt.
Usage: python timing_proto.py
"""
import re

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone import x86_const as X86

from timing_common import R, P, FUNCS, f32

CSD = Cs(CS_ARCH_X86, CS_MODE_32)
CSD.detail = True
LO, HI = 0x10000, 0x330000

# (retail start, size or None = Ghidra size, label) - code paths that matter for 60 Hz (timing60.md)
TARGETS = [
    (0xA26A0, 0xD0, "Game::Init timing block 0xA26A0..0xA2770"),
    (0xA2C20, None, "Game::Frame fixed step"),
    (0x22B30, 0x346, "Frontend::Update (gap, 60 s attract, 30-tick input lock)"),
    (0x22550, None, "Frontend fade (alpha -= 0.02 per tick)"),
    (0x21A80, None, "Frontend activate (+0x84 = 30 ticks)"),
    (0x2B5F0, 0x43C, "Intro::Update (QPC seconds, 20-tick counter)"),
    (0x2B360, 0xD8, "Intro image helper (min/max seconds)"),
    (0xEEC80, None, "timed image vf1 (+8 += real seconds)"),
    (0xE4AA0, None, "pause object ctor (repeat = trunc(fps*0.25))"),
    (0x9B120, None, "analog repeat"),
    (0x9B210, None, "button repeat"),
    (0xE2DB0, None, "menu debounce 5 ticks"),
    (0xBA060, None, "script scheduler (wait -= dt)"),
    (0xB97D0, None, "script interpreter"),
    (0x94560, None, "physics integration (dt)"),
    (0x61A70, None, "player/camera (0.25 smoothing, 1/60)"),
    (0x77E50, None, "splash spawn (pos - vel*0.02)"),
    (0xC0AC0, None, "flicker smoothing 0.8/0.2"),
    (0xA46D0, None, "memory maintenance every 41/201 frames"),
]


def pattern(x, start, size):
    data = x.read(start, size)
    pat = b""
    n = 0
    for i in CSD.disasm(data, start):
        b = bytearray(i.bytes)
        mask = [False] * len(b)
        for op in i.operands:
            if op.type == X86.X86_OP_IMM and (LO <= (op.imm & 0xFFFFFFFF) < HI) and i.imm_size == 4:
                for k in range(i.imm_offset, i.imm_offset + 4):
                    mask[k] = True
            if op.type == X86.X86_OP_MEM and i.disp_size == 4 and LO <= (op.mem.disp & 0xFFFFFFFF) < HI:
                for k in range(i.disp_offset, i.disp_offset + 4):
                    mask[k] = True
        if i.group(X86.X86_GRP_BRANCH_RELATIVE) or i.mnemonic == "call":
            if len(b) >= 5 and b[0] in (0xE8, 0xE9):
                for k in range(1, 5):
                    mask[k] = True
            elif len(b) == 6 and b[0] == 0x0F and 0x80 <= b[1] <= 0x8F:
                for k in range(2, 6):
                    mask[k] = True
        pat += b"".join(b"." if m else re.escape(bytes([c])) for c, m in zip(b, mask))
        n += i.size
    return pat, n


def rdata_refs(x, start, size):
    out = []
    for i in CSD.disasm(x.read(start, size), start):
        for op in i.operands:
            if op.type == X86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                a = op.mem.disp & 0xFFFFFFFF
                if x.section(a) == ".rdata":
                    out.append(x.u32(a))
    return out


def rdata_compare(rstart, pstart, size):
    a, b = rdata_refs(R, rstart, size), rdata_refs(P, pstart, size)
    if a == b:
        return "equal (%d)" % len(a)
    return "DIFFER: " + ", ".join("%08X/%08X" % (u, v) for u, v in zip(a, b) if u != v)


def text_of(x):
    s = [s for s in x.sections if s[0] == ".text"][0]
    return s[1], x.b[s[3]:s[3] + s[4]]


def main():
    pva, ptext = text_of(P)
    print("== timing-relevant code: retail EU vs. NTSC prototype")
    for start, size, label in TARGETS:
        if size is None:
            size = [f[1] for f in FUNCS if f[0] == start][0]
        pat, n = pattern(R, start, size)
        hits = [m.start() + pva for m in re.finditer(pat, ptext, re.S)]
        st = ("identical (relocated) at proto 0x%X" % hits[0]) if len(hits) == 1 else \
            ("%d matches" % len(hits) if hits else "NOT identical")
        if len(hits) == 1:
            st += ", .rdata constants %s" % rdata_compare(start, hits[0], n)
        print("  0x%06X %-5d %-48s %s" % (start, n, label, st))

    print("\n== whole game code (.text < 0x100000, Ghidra functions >= 16 bytes)")
    tot = same = 0
    for start, size, name in FUNCS:
        if not (0x11000 <= start < 0x100000) or size < 16:
            continue
        pat, n = pattern(R, start, size)
        tot += 1
        if re.search(pat, ptext, re.S):
            same += 1
    print("  %d of %d functions have an identical (relocated) counterpart in the prototype (%.1f %%)" % (
        same, tot, 100.0 * same / tot))

    print("\n== prototype timing init 0xA2620 (counterpart of 0xA26A0)")
    for i in CSD.disasm(P.read(0xA2694, 0x50), 0xA2694):
        print("  %08X %-24s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))
        if i.address >= 0xA26CE:
            break
    print("  proto .data fps 0x2D68C4 = %r, dt 0x2D68C8 = %r (retail .data 0x2D6744 = %r, 0x2D6748 = %r)" % (
        f32(P.u32(0x2D68C4)), f32(P.u32(0x2D68C8)), f32(R.u32(0x2D6744)), f32(R.u32(0x2D6748))))


if __name__ == "__main__":
    main()
