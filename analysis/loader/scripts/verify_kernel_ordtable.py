"""Reviewer check: compare the ordinal->name mapping of kernel_imports.tsv against the
kernelExports[] table of the installed Ghidra XBE loader extension (ghidra-xbe, source zip).
This is the table Ghidra used to name the thunk slots, so it is an independent source also for
the three slots that have no Ghidra code reference (PsTerminateSystemThread, HalInitiateShutdown,
IdexChannelObject). Read-only access to the extension; Ghidra itself is not started.
"""
import csv
import os
import re
import zipfile

from verify_kernel_common import LOADER, thunk_table

SRC = os.path.expandvars(r"%APPDATA%\ghidra\ghidra_12.1.2_PUBLIC\Extensions\ghidra-xbe\lib\ghidra-xbe-src.zip")
s = zipfile.ZipFile(SRC).read("src/main/java/XbeLoader/XbeLoader.java").decode()
body = s[s.index("kernelExports = {"):]
body = body[:body.index("};")]
names = re.findall(r'"([^"]*)"\s*,?\s*//\s*(\d+)', body)
tbl = {int(n): nm for nm, n in names}
print("ghidra-xbe kernelExports entries:", len(tbl), "max ordinal", max(tbl))

enc, va, ents = thunk_table()
tsv = {int(r["slot"]): r for r in csv.DictReader(open(os.path.join(LOADER, "kernel_imports.tsv"), encoding="utf-8"), delimiter="\t")}
mism = 0
for i, a, v in ents:
    o = v & 0x7FFFFFFF
    g = tbl.get(o)
    t = tsv[i]["name"]
    if g != t:
        mism += 1
        print("MISMATCH slot %d ord %d tsv=%s ghidra-xbe=%s" % (i, o, t, g))
print("compared", len(ents), "slots, mismatches:", mism)
for o in (258, 360, 357):
    print("ord %d -> %s" % (o, tbl.get(o)))

# the 'not imported' ordinals named in the task
for nm in ("KeTickCount", "XboxEEPROMKey", "HalReadSMBusValue", "HalWriteSMBusValue", "MmMapIoSpace",
           "XeImageFileName", "XcHMAC", "NtCreateEvent", "IoDismountVolume", "KeQueryPerformanceCounter"):
    o = [k for k, v in tbl.items() if v == nm]
    print("%-26s ord %s imported=%s" % (nm, o, any((v & 0x7FFFFFFF) in o for _, _, v in ents)))

# compare with the analysed agent's candidate table, if present
try:
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from kernel_common import XBOXKRNL_EXPORTS
    d = [(k, XBOXKRNL_EXPORTS.get(k), tbl.get(k)) for k in range(1, 367) if XBOXKRNL_EXPORTS.get(k) != tbl.get(k)]
    print("kernel_common table vs ghidra-xbe: %d differences (all ordinals 1..366)" % len(d))
    for x in d[:40]:
        print("   ", x)
except Exception as e:  # noqa
    print("kernel_common not comparable:", e)
