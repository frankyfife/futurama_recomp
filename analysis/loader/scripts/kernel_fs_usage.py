"""Scan the code of retail_eu default.xbe for fs:-segment accesses (Xbox KPCR).

On the Xbox, fs points to the KPCR: fs:[0] = NtTib.ExceptionList, fs:[0x24] = Irql,
fs:[0x28] = PrcbData.CurrentThread (KTHREAD*), KTHREAD+0x28 = TlsData.
The host must provide this layout (Windows' TEB has different fields at these offsets).
Uses kernel_analyze's owner()/reach() to only count instructions on real code paths.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kernel_analyze as ka  # noqa: E402
from capstone.x86 import X86_OP_MEM, X86_REG_FS, X86_REG_INVALID  # noqa: E402

x = ka.x
hits = collections.defaultdict(list)
for name, va, vsz, raw, rsz in x.sections:
    if name not in ka.CODE_SECTIONS:
        continue
    b = x.b[raw:raw + rsz]
    i = 0
    while True:
        i = b.find(b"\x64", i)
        if i < 0:
            break
        a = va + i
        ins = ka.insn_at(a)
        i += 1
        if ins is None:
            continue
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.segment == X86_REG_FS and \
                    op.mem.base == X86_REG_INVALID and op.mem.index == X86_REG_INVALID:
                fe, fk = ka.owner(a)
                if fe is None or a not in ka.reach(fe):
                    break
                hits[op.mem.disp].append((a, ins.mnemonic + " " + ins.op_str, name, ka.func_name(fe)))
                break
for disp in sorted(hits):
    lst = hits[disp]
    secs = collections.Counter(h[2] for h in lst)
    print("fs:[0x%x]  %4d accesses  sections=%s" % (disp, len(lst), dict(secs)))
    for h in lst[:6]:
        print("     %06x  %-40s %s" % (h[0], h[1], h[3]))
