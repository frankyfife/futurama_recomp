"""Generate the fs: segment patch list (design 1.6, strategy A of cpu.md 2.4).

Input:  analysis/loader/fs_sites.tsv - all rows with offset != 0x0 (exactly 38, cpu.md 2.3)
        optional cross-check against orig/retail_eu/default.xbe if present (bytes at each address)
Output: src/host/gen/fs_patch_table.gen.inc - initializer rows for guest::FsPatchSite (core/guest.h):
        { address, length, {original bytes, zero padded to 8}, dispOffset, originalDisp, tlsSlot,
          "function", "library" },

The disp32 position is derived from the encoding (design 1.6):
    64 A1 disp32           mov eax, fs:[disp]          -> +2
    64 8B 0D/3D disp32     mov ecx/edi, fs:[disp]      -> +3
    64 0F B6 05 disp32     movzx eax, byte fs:[disp]   -> +4
Target TLS slot by original displacement: 0x04 S_TLS, 0x20 S_PRCB, 0x24 S_IRQL, 0x28 S_KTHREAD,
0x58 S_DPC. At runtime the core patcher writes disp = 0xE10 + 4 * TlsIndex(slot).

Usage:  python tools/gen/gen_fs_patches.py [--check]
"""
import csv
import sys

from gen_common import ANALYSIS, ROOT, SRC, c_string, emit, header, parse_args

TSV = ANALYSIS / "loader" / "fs_sites.tsv"
OUT = SRC / "host" / "gen" / "fs_patch_table.gen.inc"
XBE = ROOT / "orig" / "retail_eu" / "default.xbe"

SLOT_BY_DISP = {0x04: "FUT_TLS_S_TLS", 0x20: "FUT_TLS_S_PRCB", 0x24: "FUT_TLS_S_IRQL",
                0x28: "FUT_TLS_S_KTHREAD", 0x58: "FUT_TLS_S_DPC"}
EXPECTED_COUNT = 38
EXPECTED_ENCODINGS = {2: 19, 3: 11, 4: 8}


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def disp_position(b: bytes) -> int:
    if b[0] != 0x64:
        die(f"no fs prefix in {b.hex()}")
    if b[1] == 0xA1 and len(b) == 6:
        return 2
    if b[1] == 0x8B and b[2] in (0x0D, 0x3D) and len(b) == 7:
        return 3
    if b[1:4] == b"\x0f\xb6\x05" and len(b) == 8:
        return 4
    die(f"unsupported encoding {b.hex()}")


def main():
    args = parse_args(__doc__)
    with open(TSV, newline="", encoding="utf-8") as f:
        rows = [r for r in csv.DictReader(f, delimiter="\t") if r["offset"] != "0x0"]
    if len(rows) != EXPECTED_COUNT:
        die(f"expected {EXPECTED_COUNT} sites with offset != 0, got {len(rows)}")
    xbe = None
    if XBE.exists():
        sys.path.insert(0, str(ROOT / "tools"))
        from xbe import Xbe  # noqa: E402
        xbe = Xbe(str(XBE))
    sites = []
    enc_count = {}
    for r in rows:
        addr = int(r["addr"], 16)
        b = bytes.fromhex(r["bytes"])
        if int(r["size"]) != len(b):
            die(f"{addr:#x}: size column {r['size']} != {len(b)} bytes")
        pos = disp_position(b)
        disp = int.from_bytes(b[pos:pos + 4], "little")
        if disp != int(r["offset"], 16):
            die(f"{addr:#x}: disp {disp:#x} != offset column {r['offset']}")
        if disp not in SLOT_BY_DISP:
            die(f"{addr:#x}: unexpected fs offset {disp:#x}")
        if xbe is not None and xbe.read(addr, len(b)) != b:
            die(f"{addr:#x}: bytes in {XBE} differ from fs_sites.tsv")
        enc_count[pos] = enc_count.get(pos, 0) + 1
        sites.append((addr, b, pos, disp, SLOT_BY_DISP[disp], r["func"], r["lib"]))
    if enc_count != EXPECTED_ENCODINGS:
        die(f"encoding histogram {enc_count} != {EXPECTED_ENCODINGS} (design 1.6)")
    sites.sort()
    L = [header("gen_fs_patches.py", ["analysis/loader/fs_sites.tsv (rows with offset != 0x0)"])]
    L.append("//")
    L.append("// Initializer rows for guest::FsPatchSite (core/guest.h). Include inside an array initializer:")
    L.append("//   static constexpr guest::FsPatchSite kSites[] = {")
    L.append("//   #include \"gen/fs_patch_table.gen.inc\"")
    L.append("//   };")
    L.append("// Prefer guest::FsPatchSites() which already does this.")
    L.append(f"// {len(sites)} sites; byte comparisons between original and decompiled code must exclude them")
    L.append("// (cpu.md 2.4 risk d).")
    L.append("// { address, length, { original bytes }, dispOffset, originalDisp, tlsSlot, function, library },")
    for addr, b, pos, disp, slot, func, lib in sites:
        bb = ", ".join(f"0x{x:02X}" for x in b.ljust(8, b"\0"))
        L.append(f"{{ 0x{addr:08X}u, {len(b)}, {{ {bb} }}, {pos}, 0x{disp:02X}u, {slot}, "
                 f"{c_string(func)}, {c_string(lib)} }},")
    L.append("")
    ok = emit(OUT, "\n".join(L), args.check)
    print(f"fs patches: {len(sites)} sites, encodings {dict(sorted(enc_count.items()))}"
          + (", verified against XBE" if xbe else ", XBE not found (bytes not cross-checked)"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
