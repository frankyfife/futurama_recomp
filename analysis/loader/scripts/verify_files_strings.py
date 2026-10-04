"""Verify string addresses quoted in files.md: print the C string at each VA and its raw references."""
from verify_files_common import *
VAS = [0x2A3F50,0x2A3F44,0x2A3F34,0x2A3F24,0x2D7AD8,0x2A5748,0x2A5700,0x2A1838,0x2A1840,0x2A3618,0x2A35CC,0x2A35EC,
       0x2A355C,0x2A3C3C,0x2A3318,0x2A3328,0x2A34F0,0x294654,0x293DF0,0x294F74,0x293D64,0x2A18CC,0x2A505C,0x2A5040,
       0x2A59EC,0x2951DC,0x2A56E8,0x2A12D4,0x2A14C8,0x293DD8,0x2D9400,0x2AB008,0x2D85E0,0x2AC0D8,0x2A4CD8,0x2A4E04,
       0x2A4E10,0x2A4D94,0x2A4D84,0x2A4D80,0x2A4D60,0x2A4D38,0x2A4D10,0x2A5B5C,0x2A5B7C,0x2D70A0,0x2D70A8,0x2D7090,0x2D7098,
       0x2A9E6C,0x2B246]
for v in VAS:
    sec = X.section(v)
    s = None
    if sec in (".rdata", ".data") and X.va2off(v) is not None:
        raw = X.read(v, 96)
        if raw[1:2] == b"\0" and raw[0:1] != b"\0":
            # UTF-16?
            try:
                s = "L" + repr(raw.decode("utf-16le").split("\0")[0])
            except Exception: s = None
        if s is None or len(s) < 4:
            s = repr(X.cstr(v))
    refs = xrefs(v)
    print(f"{v:08x} {sec}: {s}  refs={[hex(h) for n,h in refs][:8]}")
