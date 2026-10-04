"""Review 2: evidence for the additions of the second review round.
sections: dt (dt/fixed-step globals and their code refs), splash (PAL/NTSC splash selection in the Intro code),
pool (first allocation writes pool_start-4), exit (XapiBootToDash returns), d3dpp (present parameters), ctx (context names)."""
import sys
from verify_startup_r2_common import *
sec = sys.argv[1:] or ["dt", "splash", "pool", "exit", "d3dpp", "ctx"]
if "dt" in sec:
    for g in (0x2D674C, 0x2D6748, 0x2D6744, 0x2E7A58, 0x2E7A5C, 0x2E7A60, 0x2E7B6C, 0x2E7B68):
        r = absrefs(g, CODESEC)
        print("0x%X: %d code refs; first: %s" % (g, len(r), [(hex(a), fdesc(a)) for n, a in r[:4]]))
    d = decomp(0xA26A0)
    print("\n".join(l for l in d.splitlines()[:60] if "DAT_002" in l and "=" in l))
if "splash" in sec:
    for a in (0x2B65B, 0x2B814):
        pdis(a, 0x40)
        print("--")
    for s_ in (0x29526C, 0x295248, 0x2952AC, 0x2952C8):
        print(hex(s_), X.cstr(s_), [hex(v) for n, v in absrefs(s_, CODESEC)])
if "pool" in sec:
    print(decomp(0xA4020).splitlines()[0:20][-8:])
    pdis(0xA37B0, 0x80)
if "exit" in sec:
    pdis(0xF9013, 0x1C)
    pdis(0xF9362, 0x14)
if "d3dpp" in sec:
    for i in before(0xD26DA, 10):
        print("%08X %s %s" % (i.address, i.mnemonic, i.op_str))
    print(decomp(0x146180).splitlines()[-25:])
    print(decomp(0x146260))
if "ctx" in sec:
    import re
    for a in (0x2B440, 0x22650, 0x2BA80, 0x2A1F0, 0x2BF50, 0xD37C0):
        d = decomp(a) or ""
        print(hex(a), [l.strip() for l in d.splitlines() if "FUN_0002c320" in l][:1])
    print("0x2940CC =", X.cstr(0x2940CC))
