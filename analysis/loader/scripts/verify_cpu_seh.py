"""verify_cpu_seh.py - independent re-check of key fact 17 (C++ EH thunks / SEH handlers).
 * byte scan of .text for 'B8 imm32 E9 rel32' thunks whose jmp lands on ___CxxFrameHandler 0x166977
 * for every 'mov eax, fs:[0]' (64 A1 00000000) in fs_sites.tsv: the nearest preceding 'push imm32'
   (68 imm32) within 16 bytes -> handler; classify handler as thunk / other
 * __SEH_prolog (0x167dc8): what handler does it install"""
import sys, struct, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *
(n, lo, hi, raw) = sec_ranges((".text",))[0]
d = X.b[raw:raw + hi - lo]
thunks = set()
for p in range(len(d) - 10):
    if d[p] == 0xB8 and d[p + 5] == 0xE9:
        t = (lo + p + 10 + struct.unpack_from("<i", d, p + 6)[0]) & 0xFFFFFFFF
        if t == 0x166977:
            thunks.add(lo + p)
print("thunks 'mov eax,imm; jmp ___CxxFrameHandler(0x166977)':", len(thunks))
print("0x166977 is:", owner(0x166977))
rows = [l.rstrip("\n").split("\t") for l in open(LOADER + r"\fs_sites.tsv", encoding="utf-8")][1:]
loads = [int(r[0], 16) for r in rows if r[1] == "64a100000000"]
cls = collections.Counter()
used = set()
for a in loads:
    b = X.read(a - 16, 16)
    h = None
    for k in range(11, -1, -1):
        if b[k] == 0x68:
            h = struct.unpack_from("<I", b, k + 1)[0]
            break
    # also push imm after the load (pattern push -1; mov eax,fs:0; push handler)
    if h is None:
        b2 = X.read(a + 6, 8)
        for k in range(0, 4):
            if b2[k] == 0x68:
                h = struct.unpack_from("<I", b2, k + 1)[0]
                break
    if h is None:
        cls["no push imm nearby"] += 1
    elif h in thunks:
        cls["C++EH thunk"] += 1
        used.add(h)
    else:
        cls["other %s" % (owner(h)[2] if owner(h) else hex(h))] += 1
print("mov eax,fs:[0] sites:", len(loads), dict(cls.most_common(12)))
print("distinct thunks used as handler:", len(used))
show(0x167dc8, 0x167e03)
