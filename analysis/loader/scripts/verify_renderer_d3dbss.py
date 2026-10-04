"""Independent re-count of accesses to D3D-internal globals (renderer.md section 6,
key fact 12). Uses capstone detail mode: every instruction (outside the D3D
section) with a memory operand whose absolute displacement (no base or base
register present) lies in the D3D BSS [0x243890, 0x246cf8) -- and also in the
initialised D3D data/code [0x2342c0, 0x243890) -- is classified as read/write
(capstone access flags) and attributed to REN / NI / GAME / XLIB.
Also lists immediate operands (e.g. push imm / mov reg, imm) pointing into the D3D
BSS, which would be address-taking.
Usage: python verify_renderer_d3dbss.py"""
import collections, capstone
from capstone import x86
from verify_renderer_common import *
md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md2.detail = True
D3D_LO, BSS_LO, D3D_HI = 0x2342c0, 0x243890, 0x2342c0 + 0x12a38
def region(a):
    if 0x146020 <= a < 0x14cd50 or 0x1552c0 <= a < 0x166871: return "REN"
    if 0xfd000 <= a < 0x146020 or 0x14cd50 <= a < 0x1552c0: return "NI"
    if 0x11000 <= a < 0xfd000: return "GAME"
    if 0x166871 <= a < 0x2342c0: return "CRT"
    return "XLIB"
stat = collections.defaultdict(lambda: {"r": 0, "w": 0, "funcs": set(), "globals": collections.Counter(), "imm": []})
for fa, (nm, sz, blk) in F.items():
    if D3D_LO <= fa < D3D_HI or sz <= 0: continue
    code = X.read(fa, sz)
    if not code: continue
    reg = region(fa)
    for i in md2.disasm(code, fa):
        for op in i.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                d = op.mem.disp & 0xffffffff
                if BSS_LO <= d < D3D_HI:
                    s = stat[reg]
                    if op.access & capstone.CS_AC_WRITE: s["w"] += 1
                    if op.access & capstone.CS_AC_READ: s["r"] += 1
                    s["funcs"].add(fa); s["globals"][d] += 1
            elif op.type == x86.X86_OP_IMM and BSS_LO <= (op.imm & 0xffffffff) < D3D_HI:
                stat[reg]["imm"].append((i.address, i.mnemonic, i.op_str))
for reg in ("REN", "NI", "GAME", "CRT", "XLIB"):
    s = stat.get(reg)
    if not s: print(reg, ": 0 accesses"); continue
    print(f"{reg}: {len(s['funcs'])} functions, writes {s['w']}, reads {s['r']}, distinct globals {len(s['globals'])}, imm refs {len(s['imm'])}")
    if reg == "REN":
        print("   top globals:", ", ".join(f"{g:#x}x{n}" for g, n in s["globals"].most_common(12)))
        print("   functions:", " ".join(hex(f) for f in sorted(s["funcs"])))
    for x in s["imm"][:10]: print("   imm", hex(x[0]), x[1], x[2])
