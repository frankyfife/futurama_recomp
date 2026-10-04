"""Brute-force byte scan (no function boundaries) of .text for E8/E9 rel32 whose
target is the START of a D3D-section function (functions.tsv). Compares the
set of call sites with the linear-sweep result per region (REN/NI/GAME), and
lists sites that lie outside every Ghidra function (gaps), e.g. 0x15f7e0.
Possible false positives (E8 inside other instructions) are flagged by
checking whether a linear decode from the containing function reaches the site.
Usage: python verify_renderer_bytescan.py"""
import collections, struct
from verify_renderer_common import *
D3D_LO, D3D_HI = 0x2342c0, 0x2342c0 + 0x12a38
starts = {a for a in F if D3D_LO <= a < D3D_HI}
def region(a):
    if 0x146020 <= a < 0x14cd50 or 0x1552c0 <= a < 0x166871: return "REN"
    if 0xfd000 <= a < 0x146020 or 0x14cd50 <= a < 0x1552c0: return "NI"
    if 0x11000 <= a < 0xfd000: return "GAME"
    return "other"
text = X.read(0x11000, 0x2232c0)
raw = []
for off in range(len(text) - 5):
    if text[off] in (0xE8, 0xE9):
        t = (0x11000 + off + 5 + struct.unpack_from("<i", text, off + 1)[0]) & 0xffffffff
        if t in starts: raw.append(0x11000 + off)
lin = set()
for fa, (nm, sz, blk) in F.items():
    if blk != ".text" or sz <= 0: continue
    for i in dis(fa, sz):
        if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x") and int(i.op_str, 16) in starts: lin.add(i.address)
cnt = collections.Counter(region(a) for a in raw)
print("raw E8/E9 sites into D3D function starts:", len(raw), dict(cnt), " linear-sweep sites:", len(lin))
for a in raw:
    if a not in lin:
        c = containing(a)
        print(f"  raw-only {a:#x} region={region(a)} in={hex(c) if c else 'GAP'} -> target {F[(a + 5 + struct.unpack('<i', X.read(a + 1, 4))[0]) & 0xffffffff][0]}")
