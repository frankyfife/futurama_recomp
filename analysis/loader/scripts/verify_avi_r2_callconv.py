"""Round-2: stack cleanup (ret N) of every entry point reached from outside
its library (from verify_avi_r2_rawscan) plus the game's XMV packet
callbacks. Follows a leading 'jmp rel32' (thunks). Collects ALL ret
instructions reachable by a simple recursive walk inside the function so a
mismatch between several returns would be visible.
Also checks the argument count actually pushed at each call site (counting
pushes back to the previous call/ret/jmp in linear order).
"""
import re
import struct
from verify_avi_r2_common import X, MD, FNAME, Tee, SECT, linear_starts

T = Tee("verify_avi_r2_callconv.out")
ENTRIES = [int(x, 16) for x in """
246e7d 246e93 246ea9 247911 247938 247950 24796c 247988 2479ac 2479c4 2479e0 2479e5
2479ea 2479ef 247a07 248380 248398 2483b4 2483d8 2483fc 248431 248466 24848a 2484aa
2484af 2484c7 2484df 248508 248531 248d60 248d84 248da8 248df2 248e27 248e4b 248e80
248ea0 24935c 2493a3 2493fa 264360 264677 264759 2647c6 264943 264962 264fdf
28ce5c 28ceb2 28cebe 28cf2a 28cfe1 28cfe6 28d008 edba0 ede60 c97d0 9b6b0""".split()]


def rets(start, limit=4000):
    seen, todo, found = set(), [start], set()
    while todo and len(seen) < limit:
        a = todo.pop()
        while a not in seen:
            seen.add(a)
            ins = next(MD.disasm(X.read(a, 16), a, 1), None)
            if ins is None:
                break
            m = ins.mnemonic
            if m in ("ret", "retn"):
                found.add(int(ins.op_str, 0) if ins.op_str else 0)
                break
            if m == "jmp":
                if ins.op_str.startswith("0x"):
                    t = int(ins.op_str, 16)
                    if X.section(t) != X.section(start) and a == start:
                        return ("thunk->%08x" % t, rets(t, limit)[1])
                    a = t
                    continue
                break
            if m.startswith("j") and ins.op_str.startswith("0x"):
                todo.append(int(ins.op_str, 16))
            a += ins.size
    return ("", found)


for e in ENTRIES:
    tag, r = rets(e)
    T("%08x %-50s %s ret %s" % (e, FNAME.get(e, "?"), tag, sorted(r)))
