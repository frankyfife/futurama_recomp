"""verify_cpu_stack.py - stack probing: does the image contain an MSVC _chkstk/_alloca_probe, and
do large stack frames (sub esp, >= 0x1000) probe the pages? Relevant for running Xbox code on
Windows thread stacks that grow through a guard page."""
import sys, collections, re
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *
pats = {"chkstk cmp/lea": bytes.fromhex("3d001000008d4c2408"), "chkstk sub/test": bytes.fromhex("2d0010000085"),
        "probe test [ecx],eax": bytes.fromhex("8501")}
for k, p in list(pats.items())[:2]:
    o = X.b.find(p); hits = []
    while o >= 0:
        hits.append(hex(X.off2va(o) or 0)); o = X.b.find(p, o + 1)
    print(k, hits)
big = []
probe_calls = collections.Counter()
for (s, sz, nm) in FUNCS:
    if X.section(s) not in CODE or sz <= 0:
        continue
    ins = list(mdn.disasm(X.read(s, min(sz, 0x60)), s))[:20]
    for j, i in enumerate(ins):
        if i.mnemonic == "sub" and i.op_str.startswith("esp, 0x"):
            v = int(i.op_str.split(",")[1], 16)
            if v >= 0x1000:
                big.append((s, nm, v, i.address))
        if i.mnemonic == "call" and j > 0 and ins[j - 1].mnemonic == "mov" and ins[j - 1].op_str.startswith("eax, 0x"):
            v = int(ins[j - 1].op_str.split(",")[1], 16)
            if v >= 0x1000:
                probe_calls[i.op_str] += 1
print("functions with 'sub esp, >=0x1000' in the first 20 instructions:", len(big))
for b in big[:30]:
    print("   %08x %-36s sub esp,0x%x @%08x" % b)
print("'mov eax,>=0x1000; call X' (probe-style) targets:", probe_calls.most_common(5))
