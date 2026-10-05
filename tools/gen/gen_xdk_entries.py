"""Generate the XDK entry point table (DSOUND/XMV/XPP stubs, design E7 + Anhang B, avi.md 2.2/3.1/4.1).

Input:  analysis/loader/avi_api_surface.tsv - 54 entry points (40 DSOUND, 7 XMV, 7 XPP) with
        `callconv` = "stdcall ret N" or "stdcall jmp-><impl> ret N", plus 5 rows "vtbl+0xN" for the
        XMediaObject methods of the DSOUND stream object (vtable 0x2A5F5C) that the game calls.
        optional cross-check against orig/retail_eu/default.xbe (needs capstone): every `ret` inside
        each entry (after following a 5-byte jmp thunk) must purge exactly N bytes, the entry must lie in
        its section, and the stream vtable slots must have the purges listed in STREAM_METHODS.
Output: src/host/gen/xdk_entries.gen.inc - X-macro rows consumed by src/host/audio/xdk_entries.h:
          XDK_ENTRY(id, address, lib, hookName, purgeBytes, flags, sitesGame, sitesXmv, ghidraName, callers)
          XDK_STREAM_METHOD(id, slot, purgeBytes, inTsv)

All entries are __stdcall (avi.md 0/2.2: `ret N` of every entry, avi_callconv.out); purgeBytes is the
size of the argument block (one DWORD per argument, except the REFERENCE_TIME of FlushEx). The output
does not depend on the XBE (the cross-check only fails the run), so --check works without game data.

Usage:  python tools/gen/gen_xdk_entries.py [--check]
"""
import csv
import re
import sys

from gen_common import ANALYSIS, ROOT, SRC, c_string, comment_text, emit, header, parse_args

TSV = ANALYSIS / "loader" / "avi_api_surface.tsv"
FUNCTIONS = ANALYSIS / "retail_eu_export" / "functions.tsv"
OUT = SRC / "host" / "gen" / "xdk_entries.gen.inc"
XBE = ROOT / "orig" / "retail_eu" / "default.xbe"

EXPECTED = {"DSOUND": 40, "XMV": 7, "XPP": 7}
LIB_ENUM = {"DSOUND": "Dsound", "XMV": "Xmv", "XPP": "Xpp"}

# XMediaObject vtable of CDirectSoundStream (avi.md 2.2 + Verifikation Runde 2 #3: constructor 0x248C91
# stores 0x2A5F5C at +0; slot 4 = CDirectSoundStream_Process 0x247676). Slot order AddRef, Release,
# GetInfo, GetStatus, Process, Discontinuity, Flush (XDK XMediaObject; names of slots other than 4 are
# an assumption of avi.md). The game calls slots 0/1/3/4/5 (TSV rows); GetInfo and Flush are listed so
# that a host stream object can fill a complete 7-slot table. Purges verified against the XBE below.
STREAM_VTABLE = 0x2A5F5C
STREAM_METHODS = [  # (id, slot, purge)
    ("AddRef", 0, 4),
    ("Release", 1, 4),
    ("GetInfo", 2, 8),
    ("GetStatus", 3, 8),
    ("Process", 4, 12),
    ("Discontinuity", 5, 4),
    ("Flush", 6, 4),
]


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def ident_of(name):
    """C++ identifier from the TSV name ("DSOUND::DirectSoundCreate" -> DirectSoundCreate,
    "XMVDecoder_GetAudioStream? (AddRef'd ...)" -> XMVDecoder_GetAudioStream)."""
    n = name.split(" ")[0].rstrip("?")
    n = n.split("::")[-1]
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", n):
        die(f"cannot derive an identifier from {name!r}")
    return n


def parse_conv(text, where):
    m = re.fullmatch(r"stdcall (?:jmp->(\S+) )?ret (\d+)", text)
    if not m:
        die(f"{where}: unsupported callconv {text!r}")
    purge = int(m.group(2))
    if purge % 4:
        die(f"{where}: purge {purge} is not a multiple of 4")
    return m.group(1), purge


def load_rows():
    with open(TSV, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    entries, methods = [], []
    for r in rows:
        where = f"{r['address']} {r['name']}"
        if r["address"].startswith("vtbl+"):
            off = int(r["address"][5:], 16)
            if r["section"] != "DSOUND" or "vtable 0x2a5f5c" not in r["name"]:
                die(f"{where}: unexpected vtable row")
            if r["callconv"] != "stdcall(this on stack)":
                die(f"{where}: unexpected vtable callconv {r['callconv']!r}")
            methods.append((off, r["name"].split(" ")[0].split("::")[-1]))
            continue
        addr = int(r["address"], 16)
        lib = r["section"]
        if lib not in EXPECTED:
            die(f"{where}: unknown section {lib}")
        thunk, purge = parse_conv(r["callconv"], where)
        flags = []
        if thunk:
            flags.append("kFlagJmpThunk")
        if "?" in r["name"]:
            flags.append("kFlagNameAssumed")
        entries.append({
            "addr": addr, "lib": lib, "id": ident_of(r["name"]), "purge": purge, "thunk": thunk,
            "flags": "|".join(flags) if flags else "0", "sites_game": int(r["sites_game"]),
            "sites_xmv": int(r["sites_xmv"]), "callers": r["callers"], "tsv_name": r["name"],
        })
    # consistency checks
    counts = {k: sum(1 for e in entries if e["lib"] == k) for k in EXPECTED}
    if counts != EXPECTED:
        die(f"entry counts {counts} != {EXPECTED} (design Anhang B)")
    ids = [e["id"] for e in entries]
    if len(set(ids)) != len(ids):
        die("duplicate identifiers")
    addrs = [e["addr"] for e in entries]
    if len(set(addrs)) != len(addrs) or addrs != sorted(addrs):
        die("addresses must be unique and sorted")
    for e in entries:
        if e["sites_game"] + e["sites_xmv"] == 0:
            die(f"{e['addr']:#x}: no call sites")
    by_slot = {slot * 4: (mid, purge) for mid, slot, purge in STREAM_METHODS}
    for off, name in methods:
        if off not in by_slot or by_slot[off][0] != name:
            die(f"vtable row +{off:#x} {name} does not match STREAM_METHODS")
    if len(methods) != 5:
        die(f"expected 5 vtable rows, got {len(methods)}")
    return entries, {off for off, _ in methods}


def load_functions():
    funcs = {}
    with open(FUNCTIONS, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            funcs[int(r["address"], 16)] = (r["name"], int(r["size"]), r["block"])
    return funcs


def xbe_crosscheck(entries, funcs):
    """Returns a status string; exits on a mismatch."""
    if not XBE.exists():
        return "XBE not found (ret N not cross-checked)"
    try:
        import capstone  # noqa: PLC0415
    except ImportError:
        return "capstone not installed (ret N not cross-checked)"
    sys.path.insert(0, str(ROOT / "tools"))
    from xbe import Xbe  # noqa: PLC0415
    xbe = Xbe(str(XBE))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def rets(start, size):
        out = set()
        first = None
        for ins in md.disasm(xbe.read(start, size), start):
            if first is None:
                first = ins
            if ins.mnemonic == "ret":
                out.add(int(ins.op_str, 16) if ins.op_str else 0)
        return first, out

    for e in entries:
        a = e["addr"]
        if a not in funcs:
            die(f"{a:#x} {e['id']}: not a function start in functions.tsv")
        name, size, block = funcs[a]
        if block != e["lib"] or xbe.section(a) != e["lib"]:
            die(f"{a:#x} {e['id']}: section {block}/{xbe.section(a)} != {e['lib']}")
        if size < 5:
            die(f"{a:#x} {e['id']}: function shorter than the 5-byte hook")
        first, out = rets(a, size)
        if e["thunk"]:
            if first is None or first.mnemonic != "jmp" or size != 5:
                die(f"{a:#x} {e['id']}: expected a 5-byte jmp thunk")
            target = int(first.op_str, 16)
            tname, tsize, _ = funcs.get(target, ("?", 0, ""))
            if tname != e["thunk"]:
                die(f"{a:#x} {e['id']}: thunk target {tname} != {e['thunk']}")
            _, out = rets(target, tsize)
        if out != {e["purge"]}:
            die(f"{a:#x} {e['id']}: ret {sorted(out)} in the XBE != ret {e['purge']} in the TSV")
        e["ghidra"] = name
    # stream vtable: the 7 methods are contiguous; the extent of each is up to the next start.
    starts = [xbe.u32(STREAM_VTABLE + 4 * slot) for _, slot, _ in STREAM_METHODS]
    process = starts[4]
    if funcs.get(process, ("",))[0] != "DSOUND::CDirectSoundStream_Process":
        die(f"stream vtable slot 4 = {process:#x}, expected CDirectSoundStream_Process")
    ends = sorted(starts) + [process + funcs[process][1]]
    for (mid, slot, purge), start in zip(STREAM_METHODS, starts):
        if xbe.section(start) != "DSOUND":
            die(f"stream vtable slot {slot} -> {start:#x} outside DSOUND")
        end = ends[ends.index(start) + 1]
        _, out = rets(start, end - start)
        if out != {purge}:
            die(f"stream vtable slot {slot} ({mid}) at {start:#x}: ret {sorted(out)} != {purge}")
    return "verified against XBE (ret N of 54 entries + 7 stream vtable slots)"


def main():
    args = parse_args(__doc__)
    entries, tsv_slots = load_rows()
    funcs = load_functions()
    for e in entries:  # Ghidra name from functions.tsv (documentation only)
        e["ghidra"] = funcs.get(e["addr"], ("?",))[0]
    status = xbe_crosscheck(entries, funcs)

    L = [header("gen_xdk_entries.py", ["analysis/loader/avi_api_surface.tsv",
                                       "analysis/retail_eu_export/functions.tsv (Ghidra names)"])]
    L.append("//")
    L.append("// XDK library entry points the game calls (design E7, Anhang B; avi.md 2.2, 3.1, 4.1). All are")
    L.append("// __stdcall; purgeBytes = `ret N` of the original = size of the argument block (avi_callconv.out,")
    L.append("// re-verified with capstone against the XBE by the generator). Consumed by audio/xdk_entries.h:")
    L.append("//   XDK_ENTRY(id, address, lib, hookName, purgeBytes, flags, sitesGame, sitesXmv, ghidraName, callers)")
    L.append("//   XDK_STREAM_METHOD(id, slot, purgeBytes, inTsv)  - XMediaObject vtable 0x2A5F5C of CDirectSoundStream")
    L.append("// flags: kFlagJmpThunk = the entry is a 5-byte jmp to the implementation (the hook replaces the jmp),")
    L.append("//        kFlagNameAssumed = name derived from behaviour (XMV has no symbols, avi.md 3.1).")
    L.append("// sitesGame/sitesXmv: call sites in game code / inside the XMV library (avi_rawcalls.out).")
    L.append("#ifndef XDK_ENTRY")
    L.append("#define XDK_ENTRY(id, address, lib, hookName, purgeBytes, flags, sitesGame, sitesXmv, ghidraName, callers)")
    L.append("#endif")
    L.append("#ifndef XDK_STREAM_METHOD")
    L.append("#define XDK_STREAM_METHOD(id, slot, purgeBytes, inTsv)")
    L.append("#endif")
    L.append(f"// {len(entries)} entries: " + ", ".join(f"{k} {v}" for k, v in EXPECTED.items()))
    for e in entries:
        hook = f"{e['lib']}::{e['id']}"
        if e["thunk"]:
            L.append(f"// {comment_text(e['tsv_name'])}: jmp -> {comment_text(e['thunk'])}")
        elif e["id"] not in e["tsv_name"].split("::")[-1] or "?" in e["tsv_name"]:
            L.append(f"// {comment_text(e['tsv_name'])}")
        L.append(f"XDK_ENTRY({e['id']}, 0x{e['addr']:08X}u, {LIB_ENUM[e['lib']]}, {c_string(hook)}, "
                 f"{e['purge']}, {e['flags']}, {e['sites_game']}, {e['sites_xmv']}, {c_string(e['ghidra'])}, "
                 f"{c_string(e['callers'])})")
    L.append(f"// stream vtable 0x{STREAM_VTABLE:08X}: {len(STREAM_METHODS)} XMediaObject slots "
             f"({len(tsv_slots)} called by the game/XMV, rows vtbl+0xN of the TSV)")
    for mid, slot, purge in STREAM_METHODS:
        L.append(f"XDK_STREAM_METHOD({mid}, {slot}, {purge}, {1 if slot * 4 in tsv_slots else 0})")
    L.append("#undef XDK_ENTRY")
    L.append("#undef XDK_STREAM_METHOD")
    L.append("")
    ok = emit(OUT, "\n".join(L), args.check)
    print(f"xdk entries: {len(entries)} entries + {len(STREAM_METHODS)} stream methods, {status}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
