"""Classify every code reference to selected globals as read / write / address-taken by linear disassembly
from the owning function start (capstone). Used to check claims like 'DAT_0030EDD4 is only read' (NiTCache gate),
'DAT_0030E598 (path suffix) is empty bss', 'DAT_002D7AE8 archive-lookup enable'.
usage: python verify_files_globals.py <hexva> ..."""
from verify_files_common import *
import sys
from capstone import CS_OP_MEM
md.detail = True
def classify(v):
    out = []
    for n, h in xrefs(v):
        if n not in (".text", "D3D", "DSOUND", "XMV", "XPP"):
            out.append((h, n, "data-ref", "")); continue
        f = owner(h)
        start = f[0] if f else h - 32
        # walk forward from function start; if the walk does not land on the ref, try earlier sync points
        found = None
        for s in [start] + [h - k for k in range(1, 12)]:
            for i in md.disasm(X.read(s, (h - s) + 16), s):
                if i.address <= h < i.address + i.size:
                    found = i; break
                if i.address > h: break
            if found: break
        if not found:
            out.append((h, n, "?", "")); continue
        i = found
        kind = "addr/imm"
        if i.operands:
            for k, op in enumerate(i.operands):
                if op.type == CS_OP_MEM and op.mem.disp & 0xffffffff == v and op.mem.base == 0 and op.mem.index == 0:
                    if k == 0 and i.mnemonic not in ("cmp", "test", "push", "call", "jmp") and len(i.operands) == 2:
                        kind = "WRITE"
                    elif k == 0 and i.mnemonic in ("inc", "dec", "not", "neg", "pop") :
                        kind = "WRITE"
                    else:
                        kind = "read"
        out.append((i.address, n, kind, f"{i.mnemonic} {i.op_str}  [{owner(i.address)[2] if owner(i.address) else '?'}]"))
    return out
for a in sys.argv[1:]:
    v = int(a, 16)
    print(f"== {v:#x} ({X.section(v)}, initialised bytes: {X.va2off(v) is not None})")
    for addr, n, kind, txt in classify(v):
        print(f"   {addr:#x} {n:6s} {kind:8s} {txt}")
