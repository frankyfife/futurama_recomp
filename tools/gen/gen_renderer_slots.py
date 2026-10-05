"""Generate the NiXBoxRenderer slot table for the NiPCRenderer null renderer (design 2.2, M2).

Input:  analysis/loader/nixboxrenderer_vtable.tsv - slot, vtable offset, original address, override
        kind and name of all 75 slots of vtable 0x2AA7C0 (renderer.md 2)
        STACK_ARGS below - stack arguments (ret N / 4, thiscall) of every slot, i.e. the output of
        analysis/loader/scripts/renderer_retn.py plus the four slots that script cannot resolve
        (renderer.md 2 and Verifikation "ret N": 31 jmp [eax+0xC] -> 0x15F7E0 ret 0xC at 0x15F871,
        42 ret at 0x14BC18, 52 ret 0xC at 0x149526, 59 jmp chain 0x158BA0 -> 0x1618A0 ret 4 at 0x1618D5)
        optional cross-check against orig/retail_eu/default.xbe with capstone if both are present:
        every vtable entry equals the TSV address and every ret N found in the original code
        (renderer_retn.py algorithm, the four special slots through the instructions named above)
        equals STACK_ARGS.
Output: src/host/gen/renderer_slots.gen.inc - one row per slot:
        FUT_RENDERER_SLOT(slot, vtableOffset, originalAddress, stackArgs, overrides, "name")
        overrides: 0 = inherited from NiRenderer/NiObject, 1 = overridden, 2 = implements a pure
        virtual slot, 3 = new in NiXBoxRenderer (columns overrides_parent of the TSV).

Usage:  python tools/gen/gen_renderer_slots.py [--check]
"""
import csv
import sys

from gen_common import ANALYSIS, ROOT, SRC, c_string, comment_text, emit, header, parse_args

TSV = ANALYSIS / "loader" / "nixboxrenderer_vtable.tsv"
OUT = SRC / "host" / "gen" / "renderer_slots.gen.inc"
XBE = ROOT / "orig" / "retail_eu" / "default.xbe"
FUNCTIONS = ANALYSIS / "retail_eu_export" / "functions.tsv"

VTABLE = 0x2AA7C0
SLOT_COUNT = 75

# Stack arguments per slot (thiscall: ecx = this, callee pops 4 * n bytes). Source: renderer_retn.py
# 2aa7c0 75 (run 2026-10-05) and renderer.md 2 for 31/42/52/59.
STACK_ARGS = [
    1, 0, 0, 1, 1, 1, 1, 1, 1, 0,   # 0..9
    0, 0, 0, 1, 2, 1, 0, 2, 0, 1,   # 10..19
    1, 0, 0, 0, 1, 4, 1, 1, 1, 0,   # 20..29
    1, 3, 1, 1, 2, 6, 0, 1, 0, 5,   # 30..39
    2, 2, 0, 4, 4, 0, 0, 6, 4, 3,   # 40..49
    3, 4, 3, 3, 1, 1, 4, 1, 1, 1,   # 50..59
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,   # 60..69
    1, 0, 1, 1, 5,                  # 70..74
]
assert len(STACK_ARGS) == SLOT_COUNT

# Slots whose ret is not reachable by renderer_retn.py: (slot, [(address, expected bytes)]) - the
# instruction chain from the slot entry to the ret that ends it (renderer.md Verifikation).
SPECIAL = {
    31: [(0x146990, "8b89bc050000"), (0x146996, "8b01"), (0x146998, "ff600c"), (0x15F871, "c20c00")],
    42: [(0x14BC18, "c3")],
    52: [(0x149526, "c20c00")],
    59: [(0x146E90, "8b89c0050000"), (0x146E96, "e9051d0100"), (0x158BA0, "81c100020000"),
         (0x158BA6, "e9f58c0000"), (0x1618D5, "c20400")],
}
TEXTURE_MANAGER_VTABLE = 0x2ABD90  # slot 31 jumps through [vt+0xC] of the manager at +0x5BC
TEXTURE_MANAGER_VF3 = 0x15F7E0

OVERRIDES = {"no": 0, "yes": 1, "yes(pure)": 2, "new": 3}


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def cross_check(rows):
    """Cross-check the TSV and STACK_ARGS against the original XBE (needs capstone)."""
    if not XBE.exists():
        print(f"note: {XBE} not present - XBE cross-check skipped")
        return
    try:
        import capstone  # noqa: F401
    except ImportError:
        print("note: capstone not installed - XBE cross-check skipped")
        return
    sys.path.insert(0, str(ROOT / "tools"))
    from xbe import Xbe  # noqa: E402
    x = Xbe(str(XBE))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    sizes = {}
    with open(FUNCTIONS, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            sizes[int(r["address"], 16)] = int(r["size"])

    def rets(a, depth=0):
        # analysis/loader/scripts/renderer_retn.py: follow a leading direct jmp (2 levels), collect
        # the ret immediates of the function body.
        size = max(sizes.get(a, 0), 16)
        first = next(md.disasm(x.read(a, size + 16), a), None)
        if first and first.mnemonic == "jmp" and depth < 2:
            try:
                return rets(int(first.op_str, 16), depth + 1)
            except ValueError:
                pass
        found = set()
        for ins in md.disasm(x.read(a, size), a):
            if ins.mnemonic == "ret":
                found.add(int(ins.op_str, 0) if ins.op_str else 0)
        return found

    if x.u32(TEXTURE_MANAGER_VTABLE + 0xC) != TEXTURE_MANAGER_VF3:
        die("TextureManager vtable slot 3 is not 0x15F7E0 (slot 31 chain)")
    for r in rows:
        slot = r["slot"]
        entry = x.u32(VTABLE + 4 * slot)
        if entry != r["address"]:
            die(f"slot {slot}: vtable entry 0x{entry:08X} != TSV 0x{r['address']:08X}")
        if slot in SPECIAL:
            for addr, hexbytes in SPECIAL[slot]:
                if x.read(addr, len(hexbytes) // 2).hex() != hexbytes:
                    die(f"slot {slot}: bytes at 0x{addr:08X} differ from {hexbytes}")
            last = bytes.fromhex(SPECIAL[slot][-1][1])
            purge = 0 if last[0] == 0xC3 else last[1] | (last[2] << 8)
            if purge != 4 * STACK_ARGS[slot]:
                die(f"slot {slot}: special chain ends in ret 0x{purge:X}, table says {STACK_ARGS[slot]} args")
            continue
        found = rets(entry)
        if found != {4 * STACK_ARGS[slot]}:
            die(f"slot {slot} (0x{entry:08X}): ret {sorted(found)} != {4 * STACK_ARGS[slot]}")
    print(f"XBE cross-check: {len(rows)} vtable entries and ret N match")


def main():
    args = parse_args(__doc__)
    with open(TSV, newline="", encoding="utf-8") as f:
        raw = list(csv.DictReader(f, delimiter="\t"))
    if len(raw) != SLOT_COUNT:
        die(f"expected {SLOT_COUNT} rows in {TSV}, got {len(raw)}")
    rows = []
    for i, r in enumerate(raw):
        slot = int(r["slot"])
        if slot != i or int(r["offset"], 16) != 4 * i:
            die(f"row {i}: slot/offset mismatch ({r['slot']}, {r['offset']})")
        if r["overrides_parent"] not in OVERRIDES:
            die(f"slot {slot}: unknown overrides_parent '{r['overrides_parent']}'")
        rows.append({"slot": slot, "address": int(r["address"], 16), "kind": OVERRIDES[r["overrides_parent"]],
                     "name": r["guessed_name"]})
    cross_check(rows)

    lines = [header("gen_renderer_slots.py",
                    ["analysis/loader/nixboxrenderer_vtable.tsv", "renderer_retn.py (STACK_ARGS)"]),
             "// FUT_RENDERER_SLOT(slot, vtableOffset, originalAddress, stackArgs, overrides, name)",
             "// overrides: 0 inherited, 1 overridden, 2 implements a pure virtual, 3 new in NiXBoxRenderer.",
             f"// {SLOT_COUNT} slots of the NiXBoxRenderer vtable 0x{VTABLE:08X} (renderer.md 2); thiscall, the",
             "// callee pops 4 * stackArgs bytes (ret N).", ""]
    for r in rows:
        s = r["slot"]
        lines.append(f"FUT_RENDERER_SLOT({s}, 0x{4 * s:03X}, 0x{r['address']:08X}, {STACK_ARGS[s]}, {r['kind']}, "
                     f"{c_string(comment_text(r['name']))})")
    text = "\n".join(lines) + "\n"
    ok = emit(OUT, text, args.check)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
