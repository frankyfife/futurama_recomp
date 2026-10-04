"""Review 2: _rtinit/_cinit tables, first C++ initializer -> pool, counts of RegisterLoader stubs, non-Ni initializers."""
from verify_startup_r2_common import *
import collections
print("== _cinit 0xF9783 / _rtinit 0xF97DB")
pdis(0xF97DB, 0x40, stop_ret=True)
print("--")
pdis(0xF9783, 0x58, stop_ret=True)
print("\n_rtinit table 0x2BDEE0..0x2BDEEC:", [hex(X.u32(a)) for a in range(0x2BDEE0, 0x2BDEEC, 4)])
print("C init 0x2D5B34..0x2D5B48:", [hex(X.u32(a)) for a in range(0x2D5B34, 0x2D5B48, 4)])
print("[0x2D85FC] =", hex(X.u32(0x2D85FC)))
ents = [X.u32(a) for a in range(0x2BDEF0, 0x2D5B30, 4)]
nz = [e for e in ents if e]
print("C++ init entries %d, nonzero %d, distinct nonzero %d, first entries %s" % (len(ents), len(nz), len(set(nz)), [hex(e) for e in ents[:3]]))
inrange = [e for e in nz if 0x17E010 <= e <= 0x233880]
print("in 0x17E010..0x233880: %d ; outside: %s" % (len(inrange), [(hex(0x2BDEF0 + 4 * i), hex(e), X.section(e)) for i, e in enumerate(ents) if e and not (0x17E010 <= e <= 0x233880)]))
# count initializers that call RegisterLoader 0x1087D0 within the first 6 instructions
cnt_rl4 = cnt_rl_any = 0
for e in nz:
    ins = dis(e, 0x30)
    calls = [i for i in ins[:8] if i.mnemonic == "call"]
    if any(i.op_str == "0x1087d0" for i in ins[:8]):
        cnt_rl_any += 1
    if len(ins) >= 4 and ins[3].mnemonic == "call" and ins[3].op_str == "0x1087d0":
        cnt_rl4 += 1
print("RegisterLoader call as 4th instruction: %d ; within first 8 instructions: %d" % (cnt_rl4, cnt_rl_any))
print("\n== first executed C++ initializer 0x%X" % ents[1])
pdis(ents[1], 0x30, stop_ret=True)
print("== NiStream::RegisterLoader 0x1087D0 (start)")
pdis(0x1087D0, 0x60)
print("callgraph callees of 0x1087D0:", [p[2] + " " + p[3] for p in CALLG if p[0] == "001087d0"])
print("\n== XDK initializers outside NetImmerse range")
for i, e in enumerate(ents):
    if e and not (0x17E010 <= e <= 0x233880):
        print("entry 0x%X -> 0x%X (%s)" % (0x2BDEF0 + 4 * i, e, X.section(e)))
        pdis(e, 0x40, stop_ret=True)
