"""Independent check of table 2.3 in files.md: for the file-system kernel slots, count direct
'call dword ptr [slot]' (FF 15) sites, 'push dword ptr [slot]' (FF 35) and 'mov reg,[slot]' (8B xx) data uses,
and list the owner functions. Ordinal = raw thunk value & 0x7fffffff. The Ghidra name is taken from callgraph.tsv
(ghidra-xbe import names) for functions that contain a call through that slot (intersection over all owners)."""
from verify_files_common import *
import struct, collections
SLOTS = [0x2935C4, 0x29360C, 0x2935E8, 0x2935EC, 0x2935F0, 0x2935B8, 0x2935F4, 0x293618, 0x293614, 0x2935F8,
         0x2935C8, 0x293604, 0x293600, 0x293620, 0x293628, 0x293624, 0x2935FC, 0x2935E4, 0x2935B0, 0x2935B4,
         0x2935CC, 0x2935D0, 0x2935D4, 0x2935DC, 0x2935D8, 0x2935E0, 0x29361C, 0x2935A4, 0x29356C, 0x293660, 0x293634, 0x293688]
cg = collections.defaultdict(set)
for line in open(EXP + r"\callgraph.tsv", encoding="utf-8"):
    p = line.rstrip("\n").split("\t")
    if len(p) > 3 and p[3].startswith("xboxkrnl.exe::"): cg[int(p[0], 16)].add(p[3][14:])
code = [s for s in X.sections if s[0] in (".text", "D3D", "DSOUND", "XMV", "XPP")]
for slot in SLOTS:
    ordn = X.u32(slot) & 0x7fffffff
    calls, pushes, movs, jmps = [], [], [], []
    for n, va, vsz, raw, rsz in code:
        blob = X.b[raw:raw + rsz]
        sb = struct.pack("<I", slot)
        i = blob.find(sb)
        while i >= 0:
            a = va + i
            if blob[i - 2:i] == b"\xff\x15": calls.append(a - 2)
            elif blob[i - 2:i] == b"\xff\x35": pushes.append(a - 2)
            elif blob[i - 2:i] == b"\xff\x25": jmps.append(a - 2)
            else: movs.append(a)
            i = blob.find(sb, i + 1)
    owners = sorted({owner(c)[0] for c in calls + pushes + movs + jmps if owner(c)})
    common = None
    for o in {owner(c)[0] for c in calls if owner(c)}:
        common = cg.get(o, set()) if common is None else common & cg.get(o, set())
    # jmp thunks (stub functions like 0xf9cc6) -> ghidra function name
    jn = [NAMES.get(j) for j in jmps]
    print(f"{slot:#x} ord {ordn:3d} calls {len(calls):2d} push {len(pushes)} mov/other {len(movs)} jmp-stubs {[hex(j) for j in jmps]} {jn} "
          f"ghidra-common {sorted(common) if common else None} owners {[NAMES.get(o, hex(o)) for o in owners][:14]}")
