"""verify_cpu_seh2.py - key fact 17 / section 2.1+2.5: which handlers the fs:[0] frames register.
Own pattern scan (independent of cpu_sehhandlers.py and verify_cpu_seh.py):
 * EH thunks: 'B8 imm32 E9 rel32' whose jmp target is ___CxxFrameHandler (name looked up in functions.tsv)
 * for every 'mov eax, fs:[0]' (64 A1 00000000) site of fs_sites.tsv: the pushed handler = the imm32 of
   the nearest 'push imm32' (68) within 12 bytes before or 12 bytes after the load
 * __SEH_prolog: callers and the handler it pushes."""
import sys, struct, collections, re
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *

cxx = [a for (a, sz, nm) in FUNCS if nm == "___CxxFrameHandler"]
print("___CxxFrameHandler in functions.tsv:", [hex(a) for a in cxx])
CXX = cxx[0]
thunks = set()
for (n, lo, hi, raw) in sec_ranges():
    d = X.b[raw:raw + (hi - lo)]
    p = d.find(b"\xb8")
    while p >= 0:
        if p + 10 <= len(d) and d[p + 5] == 0xE9:
            t = (lo + p + 10 + struct.unpack_from("<i", d, p + 6)[0]) & 0xffffffff
            if t == CXX:
                thunks.add(lo + p)
        p = d.find(b"\xb8", p + 1)
print("EH thunks 'mov eax,imm; jmp ___CxxFrameHandler':", len(thunks))

rows = [l.rstrip("\n").split("\t") for l in open(LOADER + r"\fs_sites.tsv", encoding="utf-8")][1:]
loads = [int(r[0], 16) for r in rows if r[1] == "64a100000000"]
print("'mov eax, fs:[0]' sites:", len(loads))
cat = collections.Counter()
used = set()
other = collections.Counter()
for a in loads:
    win = X.read(a - 12, 30)
    cands = []
    for k in range(len(win) - 4):
        if win[k] == 0x68:
            v = struct.unpack_from("<I", win, k + 1)[0]
            cands.append((abs(k - 12), a - 12 + k, v))
    cands.sort()
    h = None
    for dist, pa, v in cands:
        if X.section(v) in CODE:
            h = v
            break
    if h is None:
        cat["no push imm(code) within +-12"] += 1
    elif h in thunks:
        cat["C++EH thunk"] += 1
        used.add(h)
    else:
        o = owner(h)
        cat["other handler"] += 1
        other[(hex(h), o[2] if o else "-")] += 1
print("handler classes:", dict(cat))
print("distinct thunks used:", len(used))
print("other handlers:", other.most_common(10))

# __SEH_prolog
seh = [a for (a, sz, nm) in FUNCS if nm == "__SEH_prolog"]
print("\n__SEH_prolog in functions.tsv:", [hex(a) for a in seh])
show(0x167dc8, 0x167dd3)
n = 0
for (s, lo, hi, raw) in sec_ranges():
    d = X.b[raw:raw + (hi - lo)]
    for p in range(len(d) - 5):
        if d[p] == 0xE8 and ((lo + p + 5 + struct.unpack_from("<i", d, p + 1)[0]) & 0xffffffff) == 0x167dc8:
            n += 1
print("raw E8 calls to 0x167dc8:", n)
print("0x168dcc owner:", owner(0x168dcc))
