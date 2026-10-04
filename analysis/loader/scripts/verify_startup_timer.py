"""Fact 5: QueryPerformanceCounter/Frequency bytes and all callers (raw E8/E9 + absolute refs), direct rdtsc
sites, and the game timer helpers FUN_000d2b10..d2b90."""
from verify_startup_common import *
pdis(0xf641b, 0x11 + 0x13, False)
for t in (0xf641b, 0xf642c):
    print(fname(t), [(hex(a), k, ("%x %s" % (func_of(a)[0], func_of(a)[2])) if func_of(a) else "-") for a, k in raw_calls(t)],
          "abs:", [(n, hex(a)) for n, a in abs_refs(t)])
print("rdtsc (0f 31) sites in code sections:")
for name, va, d in sec_bytes():
    p = d.find(b"\x0f\x31")
    while p >= 0:
        a = va + p
        # validate by disassembling from function start if possible
        f = func_of(a)
        ok = False
        if f:
            ok = any(i.address == a and i.mnemonic == "rdtsc" for i in md.disasm(X.read(f[0], f[1]), f[0]))
        print("  %08x %-7s %s %s" % (a, name, ("%x %s" % (f[0], f[2])) if f else "<no func>", "VALID" if ok else "(unverified/likely data)"))
        p = d.find(b"\x0f\x31", p + 1)
for a in (0xd2b10, 0xd2b30, 0xd2b50, 0xd2b80, 0xd2b90):
    print(decomp(a))
print("[0x2EB450] refs:", [(n, hex(a), (func_of(a)[2] if func_of(a) else '-')) for n, a in abs_refs(0x2eb450, CODE)])
