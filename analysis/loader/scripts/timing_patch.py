"""Patch sites for 60 Hz in Game::Init FUN_000A26A0 (timing60.md, section 2).

1. Disassembles both branches of the timing block (0xA26BD..0xA276A) with exact bytes.
2. Lists every code reference of the four .rdata constants the block loads with fld (shared constant pool:
   the constants themselves must not be patched).
3. Emulates the block (tiny x87/mov interpreter for exactly the instructions used there) for
   PAL (CApp+0xE8 = 0), NTSC (= 1) and PAL with the proposed operand patches applied, and prints the final values
   of all globals the block writes. The patched PAL path must equal NTSC in every timing global and keep "PAL".
4. Prints the patch table (address, whole original instruction, new instruction, changed bytes) for the
   host's original-byte-verified patcher, plus the optional .data initial values (0x2D6744/0x2D6748).
Usage: python timing_patch.py
"""
import struct

from timing_common import R, CS, mem_refs, absrefs, owner_name, f32, CODESEC

BLOCK = (0xA26BD, 0xA276A)
# whole original instruction -> new instruction (operand bytes only; same length)
PATCHES = [
    (0xA2714, "d905ac552900", "d905cc422900", "fld [0x2955AC] (50.0f)  -> fld [0x2942CC] (60.0f): fps for 0x2E7A60/0x2D6744"),
    (0xA271A, "c7056c7b2e0000004842", "c7056c7b2e0000007042", "mov [0x2E7B6C], 50.0f -> 60.0f"),
    (0xA272A, "c7055c7a2e000ad7a33c", "c7055c7a2e008988883c", "mov [0x2E7A5C], 0.02f -> 1/60 (dt clamp max)"),
    (0xA2734, "d905c0422900", "d90590e32900", "fld [0x2942C0] (0.02f)  -> fld [0x29E390] (1/60): dt for 0x2E7A58/0x2D6748"),
    (0xA273A, "c705687b2e008542213d", "c705687b2e008a4b063d", "mov [0x2E7B68], 1/25.4 -> 1/30.5 (write-only)"),
]
DATA_PATCHES = [
    (0x2D6744, "00004842", "00007042", ".data initial fps 50.0f -> 60.0f (read before Game::Init by FUN_000E4AA0)"),
    (0x2D6748, "0ad7a33c", "8988883c", ".data initial dt 0.02f -> 1/60 (consistency; no reader before Game::Init)"),
]


def image(patched):
    b = bytearray(R.b)
    if patched:
        for va, old, new, _ in PATCHES:
            o = R.va2off(va)
            assert b[o:o + len(old) // 2].hex() == old, hex(va)
            b[o:o + len(new) // 2] = bytes.fromhex(new)
    return bytes(b)


def rd(img, va, n):
    o = R.va2off(va)
    return img[o:o + n]


def emulate(img, e8):
    mem = {}
    st = []
    regs = {"ebx": 1, "bl": 1}
    pc = BLOCK[0]
    # mov eax,[esi+0xe8] ; mov ebx,1 ; cmp eax,ebx ; jne PAL  -> handled by choosing the start
    pc = 0xA26CC if e8 == 1 else 0xA2714
    while pc < BLOCK[1]:
        i = next(CS.disasm(rd(img, pc, 16), pc))
        m, op = i.mnemonic, i.op_str
        nxt = pc + i.size

        def addr(s):
            return int(s.split("[")[1].rstrip("]"), 16)
        if m == "fld":
            st.insert(0, struct.unpack("<I", rd(img, addr(op), 4))[0])
        elif m in ("fst", "fstp"):
            mem[addr(op)] = ("f", st[0])
            if m == "fstp":
                st.pop(0)
        elif m == "fxch":
            st[0], st[1] = st[1], st[0]
        elif m == "mov" and op.startswith("dword ptr [0x") and ", 0x" in op:
            mem[addr(op.split(",")[0])] = ("d", int(op.split(", ")[1], 16))
        elif m == "mov" and op.startswith("dword ptr [0x") and op.endswith("eax"):
            mem[addr(op.split(",")[0])] = ("d", regs["eax"])
        elif m == "mov" and op.startswith("byte ptr [0x"):
            mem[addr(op.split(",")[0])] = ("b", regs[op.split(", ")[1]] & 0xFF)
        elif m == "mov" and op.startswith("eax, dword ptr [0x"):
            regs["eax"] = struct.unpack("<I", rd(img, addr(op), 4))[0]
        elif m == "mov" and op.startswith("cl, byte ptr [0x"):
            regs["cl"] = rd(img, addr(op), 1)[0]
        elif m == "jmp":
            nxt = int(op, 16)
        elif m == "mov" and op.startswith("ecx, esi"):
            pass
        else:
            raise RuntimeError("unhandled %08X %s %s" % (pc, m, op))
        pc = nxt
    return mem


def show(mem):
    out = {}
    for a in sorted(mem):
        k, v = mem[a]
        if k == "b":
            out[a] = "byte %d" % v
        elif a == 0x2E7B70:
            out[a] = "%08X %r" % (v, struct.pack("<I", v).split(b"\0")[0].decode())
        else:
            out[a] = "%08X (%.9g)" % (v, f32(v))
    return out


def main():
    print("== 1. timing block of FUN_000A26A0")
    for i in CS.disasm(R.read(BLOCK[0], BLOCK[1] - BLOCK[0]), BLOCK[0]):
        print("  %08X %-22s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))

    print("\n== 2. code references of the fld constants (shared pool -> patch the operand, not the constant)")
    for c in (0x2955AC, 0x2942C0, 0x2942CC, 0x29E390, 0x2A1864, 0x2A1868):
        refs = mem_refs(c)
        raw = absrefs(R, c, CODESEC)
        print("  [0x%06X] = %08X (%.9g): %d code refs (%d raw): %s" % (
            c, R.u32(c), f32(R.u32(c)), len(refs), len(raw),
            ", ".join("%X %s" % (e[0], owner_name(e[5])) for e in refs)))

    print("\n== 3. emulated final values")
    pal, ntsc, patched = (show(emulate(image(False), 0)), show(emulate(image(False), 1)),
                          show(emulate(image(True), 0)))
    keys = sorted(set(pal) | set(ntsc) | set(patched))
    print("  %-10s %-26s %-26s %-26s" % ("global", "PAL (orig)", "NTSC (orig)", "PAL + patch"))
    for k in keys:
        print("  0x%06X   %-26s %-26s %-26s" % (k, pal.get(k, "-"), ntsc.get(k, "-"), patched.get(k, "-")))
    timing = [0x2D6744, 0x2D6748, 0x2E7A58, 0x2E7A5C, 0x2E7A60, 0x2E7B68, 0x2E7B6C, 0x2D674C]
    ok = all(patched.get(k) == ntsc.get(k) for k in timing) and patched[0x2E7B70] == pal[0x2E7B70]
    print("  patched PAL == NTSC in all timing globals and tag stays PAL:", ok)

    print("\n== 4. patch table (original instruction verified as a whole)")
    for va, old, new, why in PATCHES:
        cur = R.read(va, len(old) // 2).hex()
        diff = [va + k for k in range(len(old) // 2) if old[2 * k:2 * k + 2] != new[2 * k:2 * k + 2]]
        print("  0x%06X  %s -> %s  bytes 0x%X..0x%X  %s  [%s]" % (
            va, old, new, diff[0], diff[-1], why, "orig ok" if cur == old else "MISMATCH " + cur))
    for va, old, new, why in DATA_PATCHES:
        cur = R.read(va, 4).hex()
        print("  0x%06X  %s -> %s  %s  [%s]" % (va, old, new, why, "orig ok" if cur == old else "MISMATCH " + cur))


if __name__ == "__main__":
    main()
