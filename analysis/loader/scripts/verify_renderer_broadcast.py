"""Check the renderer-list broadcast functions (renderer.md 3.2 / key fact 7):
for each candidate function: which vtable offset it calls, whether it walks
DAT_002d7b1c, and its callers (callgraph) with the class the caller belongs to
(via NI classes vtables: caller == some vtable slot -> Class::vfNN)."""
import json, re
from verify_renderer_common import *
C = json.load(open(r"D:\FuturamaDecomp\analysis\retail_eu_ni_classes.json"))["classes"]
slotof = {}
for n, c in C.items():
    vt, ln = c.get("vtable"), c.get("vtable_len", 0)
    if not vt: continue
    for s in range(ln):
        slotof.setdefault(X.u32(vt + 4 * s), []).append(f"{n}::vf{s:02d}")
def who(a):
    nm = F.get(a, ("?",))[0]
    return f"{a:#x} {nm}" + (f" [{','.join(slotof[a][:3])}]" if a in slotof else "")
# all functions that reference DAT_002d7b1c
refs = set()
for fa, (nm, sz, blk) in F.items():
    if blk != ".text" or sz <= 0: continue
    for ins in dis(fa, sz):
        if "0x2d7b1c" in ins.op_str: refs.add(fa); break
print("functions referencing DAT_002d7b1c:", " ".join(hex(r) for r in sorted(refs)))
for fa in sorted(refs):
    offs = sorted({ins.op_str for ins in dis(fa) if ins.mnemonic in ("call", "jmp") and "ptr [" in ins.op_str})
    print(f"\n== {who(fa)} size={F[fa][1]} vcalls={offs}")
    for c in sorted(callers(fa)):
        print("    <-", who(c))
