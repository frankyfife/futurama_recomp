"""Review 2: key facts 16/17 - contiguous allocations, XPhysicalAlloc users, exit path (XapiBootToDash,
XLaunchNewImageA, HalReturnToFirmware), malloc/LocalAlloc/pool caller counts."""
from verify_startup_r2_common import *

def show_site(a, n=7):
    for i in before(a, n):
        print("      %08X %s %s" % (i.address, i.mnemonic, i.op_str))

for o in (166, 165, 171, 169, 49, 15):
    s = slot_of_ord(o)
    if s is None:
        print("ord %d not imported" % o); continue
    c, other = kcalls(s)
    print("== ord %d slot %d %s : %d call sites, other refs %s" % (o, s, KNAME.get(s), len(c), [hex(x) for x in other]))
    for a in c:
        print("   0x%X in %s [%s]" % (a, fdesc(a), X.section(a)))
        if o in (166, 165, 49):
            show_site(a, 6 if o != 49 else 3)
            nx = dis(a + 6, 8)
            print("      next: %s" % " | ".join("%s %s" % (i.mnemonic, i.op_str) for i in nx[:1]))

print("\n== XPhysicalAlloc 0xF8420 callers")
for a, k in rawrefs(0xF8420):
    print("   %s 0x%X in %s" % (k, a, fdesc(a)))
    show_site(a, 5)
print("== XMemAlloc 0x1707E2 callers", [(hex(a), fdesc(a)) for a, k in rawrefs(0x1707E2)])

print("\n== XapiBootToDash 0xF8FCA")
pdis(0xF8FCA, 0x80, stop_ret=False)
print("callers:", [(hex(a), fdesc(a)) for a, k in rawrefs(0xF8FCA)])
print("\n== XLaunchNewImageA callers")
for a, s, n in __import__("verify_startup_r2_common")._FUNCS:
    if "XLaunchNewImage" in n:
        print(hex(a), n, [(hex(c), fdesc(c)) for c, k in rawrefs(a)])

print("\n== counts")
for t, nm in ((0xA4900, "pool new"), (0xA48E0, "pool aligned"), (0xA3880, "pool free"), (0x16A083, "malloc"), (0xF63E0, "LocalAlloc"), (0x166F42, "_atexit")):
    r = rawrefs(t)
    cg = callers_cg(t)
    print("%-12s 0x%X  raw call/jmp sites %d   Ghidra caller functions %d   name=%s" % (nm, t, len(r), len(cg), FNAME.get(t)))
mr = rawrefs(0x16A083)
print("malloc callers:", sorted({fdesc(a) for a, k in mr}))
