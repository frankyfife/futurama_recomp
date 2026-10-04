"""Count indirect calls/jumps 'call/jmp [reg+OFF]' in all code sections for the
renderer-specific high vtable offsets (slots >= 56) and list the call sites.
Only classes with >= 57 vtable slots can be receivers of these offsets; we list
which NI classes qualify, so a hit outside them points to the renderer
(or to a non-NI game class)."""
import json, re, collections
from verify_renderer_common import *
C = json.load(open(r"D:\FuturamaDecomp\analysis\retail_eu_ni_classes.json"))["classes"]
big = sorted((c["vtable_len"], n) for n, c in C.items() if c.get("vtable_len", 0) >= 49)
print("NI classes with >=49 vtable slots:", big)
pat = re.compile(r"^(?:dword ptr )?\[(\w+) \+ (0x[0-9a-f]+)\]$")
hits = collections.defaultdict(list)
for fa, (nm, sz, blk) in F.items():
    if blk != ".text" or sz <= 0: continue
    for ins in dis(fa, sz):
        if ins.mnemonic in ("call", "jmp"):
            m = pat.match(ins.op_str)
            if m:
                off = int(m.group(2), 16)
                if 0xc4 <= off <= 0x12c and off % 4 == 0:
                    hits[off // 4].append((fa, ins.address))
for s in sorted(hits):
    print(f"slot {s} (+{4*s:#x}): {len(hits[s])} sites: " + ", ".join(f"{f:#x}@{a:#x}" for f, a in hits[s][:25]))
