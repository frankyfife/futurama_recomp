"""Raw `call/jmp rel32` scan (all functions) for the callers of given targets,
independent of callgraph.tsv. Usage: python verify_renderer_rawcallers.py <hexva> ..."""
import sys, collections
from verify_renderer_common import *
T = {int(a, 16) for a in sys.argv[1:]}
hits = collections.defaultdict(list)
for fa, (nm, sz, blk) in F.items():
    if sz <= 0: continue
    for i in dis(fa, sz):
        if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x") and int(i.op_str, 16) in T:
            hits[int(i.op_str, 16)].append((fa, i.address))
for t in sorted(T):
    fs = sorted({f for f, a in hits[t]})
    g = [f for f in fs if f < 0xfd000]
    print(f"{t:#x}: {len(hits[t])} sites in {len(fs)} functions ({len(g)} GAME): " + " ".join(hex(f) for f in fs))
