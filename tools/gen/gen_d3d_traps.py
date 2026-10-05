"""Generate the D3D trap list (design 2.1 "D3D8" row, 2.2, 3.2 "Falle", 5.4, Anhang B M2).

Input:  analysis/loader/d3d_functions.tsv - every D3D section function with ret N, external call
        sites and notes (d3d.md 3). A trap is generated for every row with ext_call_sites > 0 that is
        - not one of the 8 D3D shim functions of design 2.2 (SHIMS below, implemented natively in
          src/host/gfx/d3d_shim.cpp), and
        - a D3D function, not the kernel import thunk "xboxkrnl.exe::MmFreeContiguousMemory" 0x2417B0
          (`jmp [MmFreeContiguousMemory slot]`, 6 bytes): it only forwards to the HLE kernel export
          (slot 54), so trapping it would hide no D3D call; its single caller is REN code.
        optional cross-check against orig/retail_eu/default.xbe with capstone if both are present:
        the ret N of every trap (tail jumps followed) equals the purge in the table.
Output: src/host/gen/d3d_traps.gen.inc - one row per trap:
        FUT_D3D_TRAP(index, address, purgeBytes, conv, extCallSites, "hook name")
        conv is Stdcall (callee pops purgeBytes) or Fastcall (ecx/edx, D3DDevice_SetRenderState_Simple,
        d3d.md 3.4); the hook name is "trap:<function name>" ([hooks] disable = trap:* disables all).

ret column of the TSV: a number (hex with 0x or decimal) = purge bytes; "tail:ADDR" = the function
ends in a jump to ADDR, purge = ret of ADDR (a TSV row or KNOWN_TAILS); "N,tail:ADDR" = both, they
must agree.

Usage:  python tools/gen/gen_d3d_traps.py [--check]
"""
import csv
import sys

from gen_common import ANALYSIS, ROOT, SRC, c_string, emit, header, parse_args

TSV = ANALYSIS / "loader" / "d3d_functions.tsv"
OUT = SRC / "host" / "gen" / "d3d_traps.gen.inc"
XBE = ROOT / "orig" / "retail_eu" / "default.xbe"

# Design 2.2 shim table (guest.h kD3D*): shimmed, not trapped.
SHIMS = {
    0x234440: "D3D8::D3DDevice_AddRef",
    0x234460: "D3D8::D3DDevice_Release",
    0x2370D0: "D3D8::D3DDevice_CreateTexture2",
    0x2371A0: "D3D8::D3DTexture_GetSurfaceLevel2",
    0x2373B0: "D3D8::D3DResource_AddRef",
    0x2373F0: "D3D8::D3DResource_Release",
    0x236DC0: "D3D8::D3DDevice_SetRenderState_YuvEnable",
    0x2354F0: "D3D8::D3DDevice_SetScreenSpaceOffset",
}
EXCLUDED = {0x2417B0: "kernel import thunk (xboxkrnl.exe::MmFreeContiguousMemory)"}
# Tail-jump targets outside the TSV: XAPILIB::LocalFree 0xF63FD ends in `ret 4` (0xF6418).
KNOWN_TAILS = {0xF63FD: 4}
EXPECTED_TRAPS = 79  # 88 external D3D entries (d3d.md 3) - 8 shims - 1 kernel thunk


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def parse_ret(text, by_addr, seen=()):
    """Purge bytes of a TSV ret column value (follows tail jumps)."""
    text = text.strip()
    if not text:
        return None
    values = set()
    for part in text.split(","):
        part = part.strip()
        if part.startswith("tail:"):
            target = int(part[5:], 16)
            if target in seen:
                die(f"tail cycle at 0x{target:08X}")
            if target in KNOWN_TAILS:
                values.add(KNOWN_TAILS[target])
            elif target in by_addr:
                v = parse_ret(by_addr[target]["ret"], by_addr, (*seen, target))
                if v is None:
                    die(f"tail target 0x{target:08X} has no ret")
                values.add(v)
            else:
                die(f"unknown tail target 0x{target:08X}")
        else:
            values.add(int(part, 0))
    if len(values) != 1:
        die(f"inconsistent ret values {text} -> {sorted(values)}")
    return values.pop()


def cross_check(traps):
    if not XBE.exists():
        print(f"note: {XBE} not present - XBE cross-check skipped")
        return
    try:
        import capstone
    except ImportError:
        print("note: capstone not installed - XBE cross-check skipped")
        return
    sys.path.insert(0, str(ROOT / "tools"))
    from xbe import Xbe  # noqa: E402
    x = Xbe(str(XBE))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def purges(addr, size, depth=0):
        found = set()
        for ins in md.disasm(x.read(addr, size), addr):
            if ins.mnemonic == "ret":
                found.add(int(ins.op_str, 0) if ins.op_str else 0)
            elif ins.mnemonic == "jmp" and ins.op_str.startswith("0x") and depth < 3:
                target = int(ins.op_str, 16)
                if not addr <= target < addr + size:  # tail jump out of the function
                    if target in KNOWN_TAILS:
                        found.add(KNOWN_TAILS[target])
                    else:
                        found |= purges(target, 0x400, depth + 1) if target not in sizes else \
                            purges(target, sizes[target], depth + 1)
        return found

    sizes = {t["address"]: t["size"] for t in traps}
    with open(TSV, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            sizes.setdefault(int(r["address"], 16), int(r["size"]))
    def first_ret_after(addr, size):
        # Ghidra truncated some functions before their epilogue (d3d_functions.tsv note on
        # sub_0023b644, the tail of D3DDevice_Clear): take the first ret behind the recorded end.
        for ins in md.disasm(x.read(addr, size + 0x400), addr):
            if ins.mnemonic == "ret" and ins.address >= addr + size:
                return {int(ins.op_str, 0) if ins.op_str else 0}
        return set()

    for t in traps:
        if t["size"] < 5:
            die(f"{t['name']}: only {t['size']} bytes, a 5-byte detour would hit the next function")
        found = purges(t["address"], t["size"]) or first_ret_after(t["address"], t["size"])
        if found != {t["purge"]}:
            die(f"{t['name']} 0x{t['address']:08X}: ret {sorted(found)} != table {t['purge']}")
    print(f"XBE cross-check: ret N of {len(traps)} traps match")


def main():
    args = parse_args(__doc__)
    with open(TSV, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    by_addr = {int(r["address"], 16): r for r in rows}
    for addr, name in SHIMS.items():
        if addr not in by_addr or by_addr[addr]["name"] != name:
            die(f"shim 0x{addr:08X} {name} not found in {TSV}")
    traps = []
    for r in rows:
        addr = int(r["address"], 16)
        if int(r["ext_call_sites"]) <= 0 or addr in SHIMS or addr in EXCLUDED:
            continue
        purge = parse_ret(r["ret"], by_addr)
        if purge is None or purge % 4:
            die(f"{r['name']}: unusable ret '{r['ret']}'")
        conv = "Fastcall" if "__fastcall" in r["note"] else "Stdcall"
        traps.append({"address": addr, "size": int(r["size"]), "purge": purge, "conv": conv,
                      "sites": int(r["ext_call_sites"]), "name": r["name"]})
    traps.sort(key=lambda t: t["address"])
    if len(traps) != EXPECTED_TRAPS:
        die(f"expected {EXPECTED_TRAPS} traps, got {len(traps)}")
    cross_check(traps)

    lines = [header("gen_d3d_traps.py", ["analysis/loader/d3d_functions.tsv"]),
             "// FUT_D3D_TRAP(index, address, purgeBytes, conv, extCallSites, hookName)",
             f"// {len(traps)} D3D entries with external callers that are neither shimmed (design 2.2: "
             f"{len(SHIMS)} functions)",
             "// nor the MmFreeContiguousMemory import thunk 0x2417B0. Each becomes a detour to a trap stub",
             "// with the same convention and purge that logs \"TRAP <name> @<address> from <return>\" and",
             "// aborts ([render] d3d_traps = log: returns 0 instead).", ""]
    for i, t in enumerate(traps):
        lines.append(f"FUT_D3D_TRAP({i}, 0x{t['address']:08X}, 0x{t['purge']:02X}, {t['conv']}, {t['sites']}, "
                     f"{c_string('trap:' + t['name'])})")
    text = "\n".join(lines) + "\n"
    ok = emit(OUT, text, args.check)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
