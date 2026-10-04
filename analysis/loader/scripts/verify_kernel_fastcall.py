"""Reviewer check: fastcall imports (KfLowerIrql, KfRaiseIrql, IofCompleteRequest, ObfDereferenceObject)
For each site print the last instruction(s) before the call that write ecx/cl (and edx/dl), searching
backwards in a resynchronised linear sweep up to the previous call/ret/jmp (or 12 instructions), plus
the instruction(s) after the call (al use / add esp).
Also: KeRaiseIrqlToDpcLevel return use (al read after call).
"""
import json
import os
import re

from verify_kernel_common import XBE, Funcs, fmt, md

HERE = os.path.dirname(os.path.abspath(__file__))
v = json.load(open(os.path.join(HERE, "verify_kernel_sites.json")))
F = Funcs()
EXTRA = {1: [0x292d4c], 7: [0x291a65]}
W = {"ecx": re.compile(r"^(ecx|cx|cl)\b"), "edx": re.compile(r"^(edx|dx|dl)\b")}


def window(site):
    for d in range(0x40, 0, -1):
        for k in range(16):
            st = site - d - k
            ins = list(md.disasm(XBE.read(st, site - st + 16), st))
            idx = [n for n, i in enumerate(ins) if i.address == site]
            if idx:
                return ins[:idx[0] + 1], ins[idx[0] + 1:idx[0] + 3]
    return None, None


for slot in (0, 80, 66, 62, 1):
    l = [int(a, 16) for k, a in v["sites"][str(slot)]] + EXTRA.get(slot, [])
    print("==== slot", slot, "sites", len(l))
    nec = ned = 0
    for a in sorted(l):
        before, after = window(a)
        wr = {"ecx": None, "edx": None}
        for i in reversed(before[:-1][-14:]):
            if i.mnemonic in ("call", "ret") or i.mnemonic.startswith("ret"):
                break
            for r in ("ecx", "edx"):
                if wr[r] is None and i.mnemonic not in ("cmp", "test", "push") and W[r].match(i.op_str):
                    wr[r] = i
        nec += wr["ecx"] is not None
        ned += wr["edx"] is not None
        print("  %06x %-40s | ecx<- %-38s | edx<- %-30s | next: %s" % (
            a, before[-1].mnemonic + " " + before[-1].op_str,
            (wr["ecx"].mnemonic + " " + wr["ecx"].op_str) if wr["ecx"] else "-",
            (wr["edx"].mnemonic + " " + wr["edx"].op_str) if wr["edx"] else "-",
            "; ".join(i.mnemonic + " " + i.op_str for i in after)))
    print("  ecx written: %d/%d  edx written: %d/%d" % (nec, len(l), ned, len(l)))
