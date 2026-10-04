"""Search the whole XBE for drive-letter path literals (ASCII, UTF-16, and as 'X:\' dword immediates)
to check 'T: and Z: are never used by game code' and list every D:/U: literal."""
from verify_files_r2_common import *
b = X.b
for L in "CDEFTUWXYZ":
    for case in (L, L.lower()):
        pats = [(case + ":\\").encode(), (case + ":/").encode(), (case + ":\\").encode("utf-16le")]
        for p in pats:
            i = b.find(p)
            while i >= 0:
                va = X.off2va(i)
                sec = X.section(va) if va else None
                ctx = b[max(0, i - 4):i + 40]
                s = X.cstr(va) if va else None
                print("%-6r off=%06x va=%s %s %r" % (p, i, hex(va) if va else None, sec, (s or "")[:60]))
                i = b.find(p, i + 1)
        imm = struct.pack("<I", ord(case) | 0x3A00 | 0x5C0000)
        for sec, va in raw_dword_refs(struct.unpack("<I", imm)[0], CODE_SECTIONS):
            print("imm %r at %x owner %s" % (imm, va, owner(va)[2]))
