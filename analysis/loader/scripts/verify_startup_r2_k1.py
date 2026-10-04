"""Review 2: key facts 1, 2 (stub, threads, imports)."""
from verify_startup_r2_common import *

print("== thunk table: %d entries" % len(TH))
for o in (184, 166, 255, 254, 143, 148, 258, 357, 360, 74, 232, 126, 156, 169, 170, 24, 49, 95, 164, 322, 324, 323, 302, 47, 67, 189, 233, 219, 218):
    s = slot_of_ord(o)
    print("ord %3d -> slot %s  ghidra=%s" % (o, s, KNAME.get(s) if s is not None else "-"))

print("\n== stub 0xF8469")
pdis(0xF8469, 0xF84DD - 0xF8469)

print("\n== PsCreateSystemThreadEx (ord 255) references")
s = slot_of_ord(255)
c, o = kcalls(s)
for a in c:
    print("call 0x%X in %s" % (a, fdesc(a)))
    for i in before(a, 11):
        print("    %08X %s %s" % (i.address, i.mnemonic, i.op_str))
print("other refs:", [hex(x) for x in o])

print("\n== CreateThread 0xF9A72: raw E8/E9 callers and absolute refs")
print(rawrefs(0xF9A72))
print(absrefs(0xF9A72))
pdis(0xF9A72, 0x70, stop_ret=True)

print("\n== XapiThreadStartup 0xF99DA refs:", absrefs(0xF99DA), rawrefs(0xF99DA))
print("== entry 0xF84DD")
pdis(0xF84DD, 0x70, stop_ret=True)
