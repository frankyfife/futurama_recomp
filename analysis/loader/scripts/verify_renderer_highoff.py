"""Independent check of renderer.md 3.2 "Keine Aufrufer gefunden" for the high slots.
Type-agnostic raw scan: linear-sweep every function in functions.tsv (.text and
the XDK sections) and list every indirect `call/jmp dword ptr [reg + off]` with
off in the given slot set. NI classes other than NiRenderer/NiXBoxRenderer have
at most 50 vtable slots (retail_eu_ni_classes.json), so for slots >= 50 any hit
is either a renderer call or a call on a non-NI (game) class with a long vtable.
Also lists all vtables (runs of >= 41 code pointers in .rdata) to know which
non-NI classes could receive such calls.
Usage: python verify_renderer_highoff.py [slot,slot,...]"""
import sys, collections
from verify_renderer_common import *
slots = [int(s) for s in sys.argv[1].split(",")] if len(sys.argv) > 1 else \
    [40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74]
offs = {4 * s: s for s in slots}
hits = collections.defaultdict(list)
for fa, (nm, sz, blk) in F.items():
    if sz <= 0: continue
    for i in dis(fa, sz):
        if i.mnemonic not in ("call", "jmp") or not i.op_str.startswith("dword ptr ["): continue
        op = i.op_str[len("dword ptr ["):-1]
        if "+" not in op: continue
        base, _, d = op.partition(" + ")
        if base not in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp") or "*" in d: continue
        try: d = int(d, 0)
        except ValueError: continue
        if d in offs: hits[offs[d]].append((fa, nm, i.address))
for s in slots:
    h = hits.get(s, [])
    print(f"slot {s:2d} (+{4*s:#x}): {len(h)} sites:", " ".join(f"{a:#x}@{nm}" for fa, nm, a in h[:40]))
# long vtables in .rdata (code pointer runs)
print("\nruns of >=41 .text pointers in .rdata (possible long vtables):")
sec = [s for s in X.sections if s.name == ".rdata"][0] if hasattr(X.sections[0], "name") else None
lo, hi = 0x293560, 0x293560 + 0x2a96c
va = lo
while va < hi:
    n = 0
    while va + 4 * n < hi and 0x11000 <= X.u32(va + 4 * n) < 0x2342c0 and X.u32(va + 4 * n) in F: n += 1
    if n >= 41: print(f"  {va:#x} len {n}")
    va += 4 * max(n, 1)
