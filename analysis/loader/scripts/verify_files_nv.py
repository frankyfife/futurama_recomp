"""ExQueryNonVolatileSetting (thunk slot 0x2935a4, ord 24) call sites: which NV index is pushed; plus
check that ExSaveNonVolatileSetting (ord 29) is not imported; count thunk entries."""
from verify_files_common import *
slots = []; va = 0x293560
while X.u32(va): slots.append((va, X.u32(va) & 0x7fffffff)); va += 4
print("thunk entries:", len(slots), "ord 29 present:", any(o == 29 for _, o in slots), "ord 24 slot:", [hex(s) for s, o in slots if o == 24])
for c, n in callers(0xf9cc6):
    f = owner(c)
    ins = dis(f[0], f[0] + f[1])
    for k, i in enumerate(ins):
        if i.mnemonic == "call" and "0xf9cc6" in i.op_str:
            ctx = ins[max(0, k - 7):k + 1]
            print(f"{f[2]} @{i.address:x}: " + " ; ".join(f"{x.mnemonic} {x.op_str}" for x in ctx))
print(dis(0xf9cc6, 0xf9ccc))
for x in dis(0xf9cc6, 0xf9ccc): print(hex(x.address), x.mnemonic, x.op_str)
