"""Companion to verify_renderer_highoff.py: for every function with an indirect
call at a given vtable offset, print the decompiled call expressions with that
offset (receiver expression) so one can judge whether the receiver can be the
renderer. Also prints the 'this' vtable (if the function belongs to a vtable).
Usage: python verify_renderer_highoff_recv.py <slot,slot,...>"""
import sys, re, collections
from verify_renderer_common import *
slots = [int(s) for s in sys.argv[1].split(",")]
D = alldecomp()
# map function -> vtables that contain it
vtref = collections.defaultdict(list)
for va in range(0x293560, 0x293560 + 0x2a96c - 4, 4):
    v = X.u32(va)
    if v in F: vtref[v].append(va)
for s in slots:
    off = 4 * s
    pat = re.compile(r"\(\*\*\(code \*\*\)\(([^;]*?) \+ " + hex(off) + r"\)\)")
    funcs = set()
    for fa, (nm, sz, blk) in F.items():
        if sz <= 0: continue
        for i in dis(fa, sz):
            if i.mnemonic in ("call", "jmp") and re.fullmatch(r"dword ptr \[e[a-z]{2} \+ " + hex(off) + r"\]", i.op_str):
                funcs.add(fa)
    print(f"=== slot {s} (+{off:#x}): {len(funcs)} functions")
    for fa in sorted(funcs):
        t = D.get(fa, "")
        recv = collections.Counter(m.group(1).strip() for m in pat.finditer(t))
        print(f"  {fa:#x} {F[fa][0]:28s} recv={dict(recv)}")
