"""Virtual calls on the SoundManager (CApp+0xF0, CApp* = [0x2EB310]) in all code.

Pattern: an instruction reading [reg+0xF0] where reg was loaded from [0x2EB310] shortly before, the
vtable loaded from [that register], then a `call dword ptr [vtable+N]` (N/4 = slot of vtable 0x2A3B80).
Calls through other objects reached from the manager (e.g. SoundData lists) are not counted. Prints slot
histogram and sites, plus the immediate arguments pushed right before the call.
Output: scripts/out/m5_mgr_calls.out
"""
import re
from collections import defaultdict
from m5_common import code_index, Tee, out_path, name_of, owner, FUNCS, NAMES

P = Tee(out_path("m5_mgr_calls.out"))
idx = code_index()
pos = {rec[0]: i for i, rec in enumerate(idx)}

SLOTNAMES = {0: "deldtor", 1: "Update", 2: "vf2 (0xB0390, leer)", 3: "vf3_ReleaseAllData", 4: "GetSound", 5: "CreateFromFileObject",
             6: "vf6(+0x28,+0x2c)", 7: "vf7(+0x30)", 8: "vf8_SetNode(+4)", 9: "GetCategoryVolume",
             10: "SetCatVol0(+0x34)", 11: "vf11", 12: "SetCatVol1(+0x38)", 13: "GetCatVol1", 14: "SetCatVol2(+0x3c)",
             15: "GetCatVol2", 19: "vf19(+0x40)", 20: "vf20(bool)", 21: "vf21", 22: "SetEnvironment"}

sites = defaultdict(list)
for i, rec in enumerate(idx):
    addr, size, mn, ops, imms, disps, tgt = rec
    if 0xF0 not in disps or mn != "mov":
        continue
    m = re.match(r"(\w+), dword ptr \[(\w+) \+ 0xf0\]", ops)
    if not m:
        continue
    dst, base = m.groups()
    ok = False
    for j in range(max(0, i - 4), i):
        a2, s2, mn2, ops2, im2, d2, t2 = idx[j]
        if 0x2EB310 in d2 and ops2.startswith(base + ","):
            ok = True
    if not ok:
        continue
    vreg = None
    pushes = []
    for j in range(i + 1, min(len(idx), i + 14)):
        a2, s2, mn2, ops2, im2, d2, t2 = idx[j]
        mm = re.match(r"(\w+), dword ptr \[" + dst + r"\]$", ops2)
        if mn2 == "mov" and mm:
            vreg = mm.group(1)
        if mn2 == "push":
            pushes.append(ops2)
        if re.match(dst + r",", ops2) and mn2 in ("mov", "lea", "xor", "pop"):
            break  # the register holding CApp+0xF0 was overwritten
        mc = re.match(r"dword ptr \[(\w+) \+ (0x[0-9a-f]+|\d+)\]", ops2)
        if mn2 in ("call", "jmp") and vreg and (mc or ops2 == "dword ptr [%s]" % vreg):
            if mc and mc.group(1) == vreg:
                slot = int(mc.group(2), 0) // 4
            elif not mc:
                slot = 0
            else:
                break
            o = owner(a2)
            sites[slot].append((a2, name_of(o) if o else "nofunc", pushes[-3:]))
            break
        if mn2 in ("ret", "call") and not mc:
            if mn2 == "call":
                break
for slot in sorted(sites):
    P("slot %2d %-26s %3d sites" % (slot, SLOTNAMES.get(slot, "?"), len(sites[slot])))
P("")
for slot in sorted(sites):
    P("== slot %d %s" % (slot, SLOTNAMES.get(slot, "?")))
    for a, fn, pu in sites[slot]:
        P("   %08x  in %-28s pushes(last3)=%s" % (a, fn, pu))
