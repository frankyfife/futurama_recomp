"""verify_cpu_sse.py - where does non-library code (.text) use SSE (xmm) instructions, and with
which memory operands (alignment-sensitive movaps / packed ops on memory)."""
import sys, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *
byf = collections.defaultdict(list)
for (s, sz, nm) in FUNCS:
    if X.section(s) == ".text" and sz > 0:
        for i in mdn.disasm(X.read(s, sz), s):
            if "xmm" in i.op_str:
                byf[(s, nm)].append(i)
for (s, nm), L in sorted(byf.items()):
    mem = [i for i in L if "ptr" in i.op_str]
    print("%08x %-40s xmm-insns=%3d  mem-operands=%d" % (s, nm, len(L), len(mem)))
    for i in mem[:6]:
        print("      %08x %s %s" % (i.address, i.mnemonic, i.op_str))
    # alignment prologue?
    pro = [i for i in mdn.disasm(X.read(s, 0x40), s)][:12]
    al = [i for i in pro if i.mnemonic == "and" and i.op_str.startswith("esp")]
    print("      stack-align prologue:", ["%s %s" % (i.mnemonic, i.op_str) for i in al])
