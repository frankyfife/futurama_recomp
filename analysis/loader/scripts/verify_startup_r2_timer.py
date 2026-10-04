"""Review 2: key fact 5 (QPC/QPF), timer users, rdtsc sites, KeQueryPerformanceCounter/KeTickCount imports."""
from verify_startup_r2_common import *
print("== QPC 0xF641B / QPF 0xF642C")
pdis(0xF641B, 0x30, stop_ret=False)
for t in (0xF641B, 0xF642C):
    r = rawrefs(t)
    print("callers of 0x%X:" % t)
    for a, k in r:
        print("   %s 0x%X in %s" % (k, a, fdesc(a)))
    print("   abs refs:", [(n, hex(v)) for n, v in absrefs(t)])
print("\n== rdtsc (0F 31) byte occurrences in code sections, with containing function")
for name, va, d in code_sections():
    p = d.find(b"\x0f\x31")
    while p >= 0:
        a = va + p
        f = func_of(a)
        # check whether a linear disassembly from the function start hits a rdtsc exactly here
        hit = False
        if f:
            for ins in CS.disasm(X.read(f[0], f[1]), f[0]):
                if ins.address == a and ins.mnemonic == "rdtsc":
                    hit = True
        print("  %s 0x%X  func=%s  decoded_as_rdtsc_in_func=%s  ctx=%s" % (name, a, fdesc(a), hit, X.read(a - 4, 10).hex()))
        p = d.find(b"\x0f\x31", p + 1)
print("\n== imports 126 KeQueryPerformanceCounter / 127 KeQueryPerformanceFrequency / 156 KeTickCount / 128 KeQuerySystemTime")
for o in (126, 127, 156, 128):
    print(o, slot_of_ord(o))
print("\n== game timer FUN_000d2b90 and helpers")
for a in (0xD2B10, 0xD2B30, 0xD2B50, 0xD2B80, 0xD2B90):
    pdis(a, 0x30, stop_ret=True)
    print("--")
print("[0x2EB450] users (1/QPF):", [hex(v) for n, v in absrefs(0x2EB450, CODESEC)])
