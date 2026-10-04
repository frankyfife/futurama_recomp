"""Check the 'ret N' column of table 2.2 in files.md: linear disassembly of each function (size from functions.tsv,
falling back to 0x400 bytes) and the set of 'ret' immediates found before the next function start."""
from verify_files_common import *
CLAIMS = {0xF76E0: 0x1C, 0xF7195: 0x14, 0xF7603: 0x14, 0xF7654: 0x14, 0xF7282: 0x14, 0xF76A5: 8, 0xF75C5: 8,
          0xF73DC: 0x10, 0xF7358: 4, 0xF7B4D: 0xC, 0xF786B: 0x18, 0xF7B6C: 8, 0xF7C12: 4, 0xF7C5D: 0x10, 0xF8081: 4,
          0xF8120: 4, 0xF85AA: 8, 0xF8616: 4, 0xF8961: 8, 0xF8A6E: 8, 0xF8886: 8, 0xF6987: 0x18, 0xF6BEA: 8,
          0xF6CBA: 8, 0xF6DC1: 8, 0xF6E08: 4, 0xF7163: 4, 0xF70EC: 0xC, 0xF7106: 8, 0xF7F7C: 4, 0xF946D: 8,
          0x13F1D0: 0xC, 0xF706F: 8, 0xF64BE: 8, 0xF647A: 0xC}
import bisect
for a, want in sorted(CLAIMS.items()):
    i = bisect.bisect_right(STARTS, a)
    nxt = STARTS[i] if i < len(STARTS) else a + 0x400
    if a not in NAMES: nxt = a + 0x80
    rets = set()
    for ins in md.disasm(X.read(a, nxt - a), a):
        if ins.mnemonic == "ret":
            rets.add(int(ins.op_str, 16) if ins.op_str else 0)
    ok = rets == {want}
    print(f"{a:#x} {NAMES.get(a, '?'):40s} claimed ret {want:#x} found {sorted(hex(r) for r in rets)} {'OK' if ok else 'CHECK'}")
