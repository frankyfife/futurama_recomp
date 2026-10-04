"""Reviewer check: thunk table, ordinal/name mapping vs Ghidra, TSV consistency, section flags.
Usage: python verify_kernel_thunks.py
"""
import csv
import os
import re
import glob
from collections import defaultdict

from verify_kernel_common import (EXP, LOADER, XBE, callgraph, hdr_u32, section_headers,
                                  thunk_table, u32)

enc, va, ents = thunk_table()
print("hdr[0x158]=%08x  ^ retail key 5B6D40B6 = %08x" % (enc, va))
print("debug key would give %08x" % (enc ^ 0xEFB1F152))
print("entries:", len(ents), " terminator at %06x value %08x" % (va + 4 * len(ents), u32(va + 4 * len(ents))))
print("all high-bit:", all(v & 0x80000000 for _, _, v in ents))
print("next dwords after terminator:", ["%08x" % u32(va + 4 * len(ents) + 4 * k) for k in range(1, 5)])
ords = [v & 0x7FFFFFFF for _, _, v in ents]
print("distinct ordinals:", len(set(ords)), " min/max", min(ords), max(ords))

# TSV
tsv = list(csv.DictReader(open(os.path.join(LOADER, "kernel_imports.tsv"), encoding="utf-8"), delimiter="\t"))
bad = 0
for r in tsv:
    s = int(r["slot"])
    if int(r["thunk_va"], 16) != va + 4 * s or int(r["ordinal"]) != ords[s] or int(r["ordinal_hex"], 16) != ords[s]:
        print("TSV MISMATCH", r["slot"], r["name"], r["ordinal"], ords[s])
        bad += 1
print("TSV rows:", len(tsv), " slot/thunk/ordinal mismatches:", bad)
name_by_slot = {int(r["slot"]): r["name"] for r in tsv}

# Ghidra EXTERNAL:<n>
ext = {}
for r in callgraph():
    m = re.match(r"EXTERNAL:([0-9a-f]+)", r["callee"])
    if m:
        ext[int(m.group(1), 16)] = r["callee_name"].split("::")[-1]
ok = mism = 0
for idx, nm in sorted(ext.items()):
    s = idx - 1
    if name_by_slot.get(s) == nm:
        ok += 1
    else:
        mism += 1
        print("EXTERNAL MISMATCH idx", idx, nm, name_by_slot.get(s))
print("Ghidra EXTERNAL idx entries:", len(ext), " ok(slot=idx-1):", ok, " mismatch:", mism)

# Ghidra stub names (functions.tsv 'xboxkrnl.exe::X') -> jmp [slot]
from verify_kernel_common import insn
stubok = 0
for r in csv.DictReader(open(os.path.join(EXP, "functions.tsv"), encoding="utf-8"), delimiter="\t"):
    if r["name"].startswith("xboxkrnl.exe::"):
        a = int(r["address"], 16)
        i = insn(a)
        m = re.search(r"\[(0x[0-9a-f]+)\]", i.op_str)
        sl = (int(m.group(1), 16) - va) // 4
        good = name_by_slot.get(sl) == r["name"].split("::")[1]
        stubok += good
        print("stub %06x %-45s -> %s slot %d (%s) %s" % (a, r["name"], i.mnemonic + " " + i.op_str, sl, name_by_slot.get(sl), "OK" if good else "MISMATCH"))
print("stub names OK:", stubok)

# _exref names in decomp -> which slot? check decomp text contains name, and names covered
exref = defaultdict(int)
for f in glob.glob(os.path.join(EXP, "decomp", "*.c")):
    for m in re.finditer(r"([A-Za-z_]\w*)_exref", open(f, encoding="utf-8", errors="replace").read()):
        exref[m.group(1)] += 1
print("exref names:", dict(exref))
print("exref names not in TSV:", [n for n in exref if n not in name_by_slot.values()])

# names covered by any Ghidra evidence
covered = set(ext.values()) | set(exref)
for r in csv.DictReader(open(os.path.join(EXP, "functions.tsv"), encoding="utf-8"), delimiter="\t"):
    if r["name"].startswith("xboxkrnl.exe::"):
        covered.add(r["name"].split("::")[1])
# decomp call names: name( appears in decomp
alltxt = "".join(open(f, encoding="utf-8", errors="replace").read() for f in glob.glob(os.path.join(EXP, "decomp", "*.c")))
for s, nm in sorted(name_by_slot.items()):
    if nm not in covered and re.search(r"\b%s\b" % nm, alltxt):
        covered.add(nm)
print("names with some Ghidra evidence:", len(covered & set(name_by_slot.values())))
print("names without:", [nm for nm in name_by_slot.values() if nm not in covered])

# Ordinals claimed NOT imported
claimed_absent = {156: "KeTickCount", 126: "KeQueryPerformanceCounter", 127: "KeQueryPerformanceFrequency",
                  321: "XboxEEPROMKey", 45: "HalReadSMBusValue", 50: "HalWriteSMBusValue", 177: "MmMapIoSpace",
                  326: "XeImageFileName", 340: "XcHMAC", 189: "NtCreateEvent", 90: "IoDismountVolume",
                  108: "KeInitializeEvent", 254: "PsCreateSystemThread", 163: "KiUnlockDispatcherDatabase",
                  251: "ObfReferenceObject"}
for o, n in claimed_absent.items():
    print("absent check ord %3d %-28s imported=%s" % (o, n, o in ords))

# section flags
for s in section_headers():
    fl = s["flags"]
    names = [n for b, n in ((1, "WRITABLE"), (2, "PRELOAD"), (4, "EXECUTABLE"), (8, "INSERTED_FILE"),
                            (0x10, "HEAD_PAGE_RO"), (0x20, "TAIL_PAGE_RO")) if fl & b]
    print("section %-10s va %06x vsz %06x flags %02x %s" % (s["name"], s["va"], s["vsz"], fl, "|".join(names)))
