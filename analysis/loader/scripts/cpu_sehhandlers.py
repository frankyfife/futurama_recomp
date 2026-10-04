"""cpu_sehhandlers.py - which exception handlers do the fs:[0] registrations install?
Looks at the 'push imm' right before 'mov eax, fs:[0]' (MSVC C++ EH / SEH frames) and at
__SEH_prolog callers; classifies the handler targets.  usage: python cpu_sehhandlers.py"""
import sys, re, bisect, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from cpu_rd import RD
rd = RD().run()
names = {f[0]: f[2] for f in rd.funcs}
srt = sorted(rd.insn)
pos = {a: i for i, a in enumerate(srt)}
handlers = collections.Counter()
for a in srt:
    sz, mn, ops, bb = rd.insn[a]
    if bb[:2] == b"\x64\xa1" and bb[2:6] == b"\0\0\0\0":
        i = pos[a]
        for j in range(i - 1, max(i - 4, 0), -1):
            q = srt[j]
            qs, qm, qo, qb = rd.insn[q]
            if qm == "push" and qo.startswith("0x"):
                handlers[int(qo, 16)] += 1
                break
cls = collections.Counter()
detail = collections.defaultdict(int)
for h, n in handlers.items():
    i1 = rd.decode(h)      # handler thunks are often not reached by RD; decode directly
    if i1 is None:
        cls["(undecodable %#x)" % h] += n
        continue
    if i1.mnemonic == "mov" and i1.op_str.startswith("eax, 0x"):
        i2 = rd.decode(h + i1.size)
        if i2 is not None and i2.mnemonic == "jmp" and i2.op_str.startswith("0x"):
            t = int(i2.op_str, 16)
            cls["thunk 'mov eax,FuncInfo; jmp' -> %s (%#x)" % (names.get(t, "?"), t)] += n
            continue
    cls["direct %s (%#x)" % (names.get(h, "?"), h)] += n
for k, n in cls.most_common():
    print("%6d  %s" % (n, k))
# __SEH_prolog users (handler passed implicitly)
sp = [f[0] for f in rd.funcs if f[2] == "__SEH_prolog"]
if sp:
    n = sum(1 for a in srt if rd.insn[a][1] == "call" and rd.insn[a][2] == "0x%x" % sp[0])
    print("calls to __SEH_prolog (%#x): %d" % (sp[0], n))
