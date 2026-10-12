"""Annotated disassembly of the game's sound module (0xc9300..0xcdd00) into run/analyse_m5/sound_module.lst.

Function boundaries: Ghidra functions plus the vtable-only methods named in m5_common.NAMES.
Usage: python m5_dump_module.py [start end]
"""
import sys
from m5_common import NAMES, FUNCS, listing, run_path, func_end, name_of

a = int(sys.argv[1], 16) if len(sys.argv) > 2 else 0xc9300
b = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0xcdd00
starts = sorted(set([s for s in FUNCS if a <= s < b] + [s for s in NAMES if a <= s < b]))
out = []
for i, s in enumerate(starts):
    nxt = starts[i + 1] if i + 1 < len(starts) else b
    end = min(max(func_end(s), s + 1), nxt) if s not in FUNCS else min(s + FUNCS[s][1], nxt)
    out.extend(listing(s, end, name_of(s) or "FUN_%08x" % s))
    if end < nxt:
        out.append("     ... gap %08x..%08x" % (end, nxt))
path = run_path("sound_module.lst")
open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
print(path, len(out), "lines")
