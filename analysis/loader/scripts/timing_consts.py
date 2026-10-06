"""Hard-coded time-like constants in the game code (timing60.md, section 4.8).

1. All .rdata dwords/qwords whose float32/float64 value is one of the tick-related values (0.02, 1/60, 1/50,
   0.04, 1/30, 1/25, 50, 60, 25, 30) and every code instruction (Ghidra functions + gap code) that uses them.
2. Float immediates with these values in game code (mov m32, imm32 / mov r32, imm32 / push imm32).
Each hit is triaged by hand in timing60.md (most are distances or parameters, not tick lengths).
Usage: python timing_consts.py
"""
import struct

from timing_common import R, index, mem_refs, fmt

VALUES = {0.02: "0.02 (PAL dt)", 1 / 60: "1/60 (NTSC dt)", 0.04: "0.04 (2 PAL ticks)", 1 / 30: "1/30",
          1 / 25: "1/25", 50.0: "50", 60.0: "60", 25.0: "25", 30.0: "30"}
GAME_END = 0x100000


def close(a, b):
    return abs(a - b) <= abs(b) * 1e-6


def main():
    ro = [s for s in R.sections if s[0] == ".rdata"][0]
    data = R.b[ro[3]:ro[3] + ro[4]]
    print("== 1. .rdata constants and their game-code users")
    for off in range(0, len(data) - 3, 4):
        va = ro[1] + off
        f = struct.unpack_from("<f", data, off)[0]
        hits = [n for v, n in VALUES.items() if close(f, v)]
        kind = "f32"
        if not hits and off + 8 <= len(data):
            d = struct.unpack_from("<d", data, off)[0]
            hits = [n for v, n in VALUES.items() if close(d, v)]
            kind = "f64"
        if not hits:
            continue
        users = [e for e in mem_refs(va) if e[0] < GAME_END]
        if not users:
            continue
        print("  [0x%06X] %s %-20s %d game-code users" % (va, kind, hits[0], len(users)))
        for e in users:
            print("      " + fmt(e))

    print("\n== 2. float immediates in game code")
    for e in index():
        if e[0] >= GAME_END or e[2] not in ("mov", "push"):
            continue
        imm = e[3].split(", ")[-1] if e[2] == "mov" else e[3]
        if not imm.startswith("0x") or len(imm) < 9:
            continue
        f = struct.unpack("<f", struct.pack("<I", int(imm, 16)))[0]
        hits = [n for v, n in VALUES.items() if close(f, v) and v < 1.0]
        if hits:
            print("  %-20s %s" % (hits[0], fmt(e)))


if __name__ == "__main__":
    main()
