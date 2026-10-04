"""verify_d3d_operands.py - independent linear sweep of all non-D3D code sections
(every byte range from each functions.tsv start to the next start, plus the gap from
section start to the first function) collecting every instruction operand (imm / disp)
inside the whole D3D va range (code + bss). Then cross-checks every raw dword hit
(aligned or not) into D3D bss: is it covered by a decoded operand, or noise / undecoded?"""
import struct, collections
from capstone import CS_OP_IMM, CS_OP_MEM
from verify_d3d_common import *

ops = []          # (insn_addr, kind, value, func_start, text)
covered = set()   # (addr_of_operand_bytes) where a decoded operand equals a D3D value
decoded_bytes = set()
for s in [".text", "DSOUND", "XMV", "XPP"]:
    va, vsz, raw, rsz = SEC[s]
    starts = sorted([f[0] for f in FUNCS if va <= f[0] < va + rsz] + [va])
    starts = sorted(set(starts))
    for i, a in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else va + rsz
        for ins in MD.disasm(X.read(a, end - a), a):
            for op in ins.operands:
                v = None
                if op.type == CS_OP_IMM:
                    v = op.imm & 0xffffffff; k = "imm"
                    if ins.mnemonic == "call" or ins.mnemonic.startswith("j"):
                        k = "branch"
                elif op.type == CS_OP_MEM:
                    v = op.mem.disp & 0xffffffff; k = "mem"
                    if op.mem.base or op.mem.index: k = "mem_idx"
                if v is not None and D3D_LO <= v < D3D_HI:
                    f = func_of(ins.address)
                    ops.append((ins.address, k, v, f[0] if f else 0, f[3] if f else "?", s,
                                ins.mnemonic + " " + ins.op_str))
                    # locate operand bytes
                    raw4 = struct.pack("<I", v)
                    p = ins.bytes.find(raw4)
                    if p >= 0: covered.add(ins.address + p)
def cls(v):
    if v < D3D_RAWHI: return "code" if v in D3DSET else "code-nonstart"
    if v == 0x2444c4: return "IndexData"
    if v == 0x2444c8: return "DirtyFlags"
    if 0x2444d0 <= v < 0x2446d0: return "TextureState"
    if 0x2446d0 <= v < 0x2446d0 + 4 * 0xa6: return "RenderState"
    return "OTHER_BSS"
c = collections.Counter((k if k == "branch" else "data", cls(v)) for a, k, v, *_ in ops)
print("operand refs into D3D range:", len(ops))
for kk, n in sorted(c.items()): print("  ", kk, n)
other = [o for o in ops if cls(o[2]) in ("OTHER_BSS", "code-nonstart") or (o[1] != "branch" and cls(o[2]) == "code")]
print("refs outside {calls to func starts, 4 lazy globals}:")
for o in other: print("   %08x %-7s %08x %s(%08x) [%s] %s" % (o[0], o[1], o[2], o[4], o[3], o[5], o[6]))
fs = collections.defaultdict(lambda: collections.Counter())
for a, k, v, fa, fn, s, t in ops:
    if k != "branch" and cls(v) in ("IndexData", "DirtyFlags", "TextureState", "RenderState"):
        fs[(fa, fn)][cls(v)] += 1
print("functions with inline lazy-state refs:", len(fs), "instructions:", sum(sum(x.values()) for x in fs.values()))
for (fa, fn), cc in sorted(fs.items()):
    print("   %08x %-28s %s" % (fa, fn, dict(cc)))
# raw cross-check
unc = []
for s in [".text", "DSOUND", "XMV", "XPP"]:
    va, b = sec_bytes(s)
    for i in range(len(b) - 3):
        v = struct.unpack_from("<I", b, i)[0]
        if D3D_LO <= v < D3D_HI and cls(v) not in ("code-nonstart",) and (va + i) not in covered:
            if cls(v) == "code" and b[i-1] in (0xE8, 0xE9): continue
            unc.append((va + i, v, s))
# drop pure noise: dword whose bytes are inside a decoded instruction but not as operand
print("raw D3D dwords not explained by a decoded operand:", len(unc))
byv = collections.Counter(cls(v) for a, v, s in unc)
print("  by class:", dict(byv))
for a, v, s in unc:
    if cls(v) in ("IndexData", "DirtyFlags", "TextureState", "RenderState", "code"):
        print("   CHECK %08x -> %08x %s %s" % (a, v, cls(v), s))
