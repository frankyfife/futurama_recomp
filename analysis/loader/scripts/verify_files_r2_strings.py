"""Check every string address quoted in files.md: content at VA + raw code references (push imm / mov imm)."""
from verify_files_r2_common import *

ADDRS = [0x2D7AD8, 0x2A3F50, 0x2A3F44, 0x2A3F34, 0x2A3F24, 0x2A5748, 0x2A1838, 0x2A5700,
         0x2A4D60, 0x2A4D38, 0x2A4D10, 0x2A1840, 0x2A3618, 0x2A35CC, 0x2A35EC, 0x2A355C, 0x2A3C3C,
         0x2A3318, 0x2A3328, 0x2A34F0, 0x294654, 0x293DF0, 0x294F74, 0x293D64, 0x2A18CC, 0x2A505C,
         0x2A5040, 0x2A59EC, 0x2951DC, 0x2A56E8, 0x2A12D4, 0x2A14C8, 0x2A12E8, 0x2A133C, 0x2A1358,
         0x293DD8, 0x2D9400, 0x2AB008, 0x2AC0D8, 0x2A4CD8, 0x2A4E04, 0x2A4E10, 0x2A4D94, 0x2A4D84,
         0x2A4D80, 0x2A5B5C, 0x2A5B7C, 0x2A1864, 0x2D5BC0]


def show(va):
    sec = X.section(va)
    raw = X.read(va, 64)
    a = X.cstr(va)
    # utf16?
    w = None
    if raw and raw[1:2] == b"\0":
        try:
            end = raw.index(b"\0\0")
            end += end & 1
            w = raw[:end].decode("utf-16le", "replace")
        except ValueError:
            pass
    refs = [hex(r) for s, r in raw_dword_refs(va, CODE_SECTIONS)]
    drefs = [hex(r) for s, r in raw_dword_refs(va, (".rdata", ".data"))]
    print("%08x %-7s a=%r w=%r\n          coderefs=%s datarefs=%s" % (va, sec, a, w, refs[:8], drefs[:6]))


for va in (int(x, 16) for x in sys.argv[1:]) if len(sys.argv) > 1 else ADDRS:
    show(va)
