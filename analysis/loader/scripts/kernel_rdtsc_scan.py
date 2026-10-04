"""Find rdtsc (0F 31) instructions on real code paths and constant 733 MHz frequency refs."""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kernel_analyze as ka
x = ka.x
for name, va, vsz, raw, rsz in x.sections:
    if name not in ka.CODE_SECTIONS:
        continue
    b = x.b[raw:raw + rsz]
    i = 0
    while True:
        i = b.find(b"\x0f\x31", i)
        if i < 0:
            break
        a = va + i
        i += 1
        ins = ka.insn_at(a)
        if ins is None or ins.mnemonic != "rdtsc":
            continue
        fe, fk = ka.owner(a)
        if fe is None or a not in ka.reach(fe):
            continue
        print("rdtsc %06x %-6s %s" % (a, name, ka.func_name(fe)))
pat = struct.pack("<I", 0x2BB5C755)
o = x.b.find(pat)
while o >= 0:
    print("const 0x2BB5C755 (733333333) at", hex(x.off2va(o) or 0), x.section(x.off2va(o) or 0))
    o = x.b.find(pat, o + 1)
