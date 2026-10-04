"""d3d_scan_refs.py - find every reference from non-D3D code into the D3D8 library.

Linear capstone disassembly of every function (functions.tsv) outside the D3D
section, from its start to the next function start. Records
  * direct call/jmp targets inside the D3D section          (kind=call/jmp)
  * absolute memory operands / immediates inside D3D range  (kind=mem_r/mem_w/imm)
  * memory operands hitting known D3D tables in .rdata      (kind=rdata)
Also scans aligned dwords in .rdata/.data for pointers into D3D code.

Output: analysis/loader/scripts/out/d3d_refs.tsv
        (func, func_name, block, insn_addr, kind, target, target_name, insn)
Usage:  python d3d_scan_refs.py
"""
import csv, os, struct, sys, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_MEM, CS_OP_IMM
from capstone.x86 import X86_REG_INVALID
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from d3d_common import attribute

ROOT = r"D:\FuturamaDecomp"
EXP = os.path.join(ROOT, r"analysis\retail_eu_export")
OUT = os.path.join(ROOT, r"analysis\loader\scripts\out")
os.makedirs(OUT, exist_ok=True)

x = Xbe(os.path.join(ROOT, r"orig\retail_eu\default.xbe"))
sec = {n: (va, vsz, raw, rsz) for n, va, vsz, raw, rsz in x.sections}
D3D_LO = sec["D3D"][0]
D3D_HI = D3D_LO + sec["D3D"][1]
D3D_RAW_HI = D3D_LO + sec["D3D"][3]          # start of D3D bss

# named D3D globals (derived from D3D8 disassembly, see d3d.md)
GLOBALS = {
    0x244968: "D3D__pDevice",
    0x244970: "g_Device.m_pPut (push buffer put)",
    0x244974: "g_Device.m_pThreshold",
    0x244978: "g_Device.m_StateFlags",
    0x2444c8: "D3D__DirtyFlags",
    0x2444c4: "D3D_dword_2444c4",
}
def gname(a):
    if a in GLOBALS:
        return GLOBALS[a]
    if 0x2444d0 <= a < 0x2446d0:
        o = (a - 0x2444d0) // 4
        return "D3D__TextureState[%d][%d]" % (o // 32, o % 32)
    if 0x2446d0 <= a < 0x2446d0 + 4 * 0xb0:
        return "D3D__RenderState[0x%x]" % ((a - 0x2446d0) // 4)
    if 0x2469b8 <= a < 0x2469b8 + 16 * 12:
        o = a - 0x2469b8
        return "D3D__Stream[%d]+%d" % (o // 12, o % 12)
    return None

funcs = []
names = {}
for r in csv.DictReader(open(os.path.join(EXP, "functions.tsv")), delimiter="\t"):
    a = int(r["address"], 16)
    funcs.append((a, int(r["size"]), r["block"], r["name"]))
    names[a] = r["name"]
funcs.sort()

def fname(a):
    if a in names:
        return names[a]
    g = gname(a)
    return g or ""

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
rows = []
for i, (a, sz, blk, nm) in enumerate(funcs):
    if blk == "D3D" or blk not in sec:
        continue
    va, vsz, raw, rsz = sec[blk]
    end = funcs[i + 1][0] if i + 1 < len(funcs) else va + rsz
    end = min(end, va + rsz)
    if end <= a:
        continue
    code = x.read(a, end - a)
    for ins in md.disasm(code, a):
        mn = ins.mnemonic
        hits = []
        for op in ins.operands:
            if op.type == CS_OP_IMM:
                v = op.imm & 0xffffffff
                if D3D_LO <= v < D3D_HI:
                    if mn == "call" or mn.startswith("j"):
                        hits.append(("call" if mn == "call" else "jmp", v))
                    else:
                        hits.append(("imm", v))
            elif op.type == CS_OP_MEM:
                m = op.mem
                v = m.disp & 0xffffffff
                if D3D_LO <= v < D3D_HI:
                    w = (op.access & 2) != 0  # CS_AC_WRITE
                    k = "mem_w" if w else "mem_r"
                    if m.base != X86_REG_INVALID or m.index != X86_REG_INVALID:
                        k += "_idx"
                    hits.append((k, v))
        for kind, v in hits:
            ca, cn = attribute(ins.address)
            rows.append((ca, cn, blk, ins.address, kind, v, fname(v),
                         "%s %s" % (mn, ins.op_str)))

# pointers to D3D code/data in data sections
for blk in (".rdata", ".data"):
    va, vsz, raw, rsz = sec[blk]
    data = x.b[raw:raw + rsz]
    for off in range(0, rsz - 3, 4):
        v = struct.unpack_from("<I", data, off)[0]
        if D3D_LO <= v < D3D_HI:
            rows.append((0, "(data)", blk, va + off, "dataptr", v, fname(v), ""))

with open(os.path.join(OUT, "d3d_refs.tsv"), "w", newline="") as f:
    w = csv.writer(f, delimiter="\t")
    w.writerow(["func", "func_name", "block", "insn_addr", "kind", "target", "target_name", "insn"])
    for r in rows:
        w.writerow(["%08x" % r[0], r[1], r[2], "%08x" % r[3], r[4], "%08x" % r[5], r[6], r[7]])

c = collections.Counter((r[4]) for r in rows)
print("total refs", len(rows), dict(c))
fs = collections.Counter(r[0] for r in rows if r[4] != "dataptr")
print("functions with refs", len(fs))
tg = collections.Counter((r[4], r[6] or "%08x" % r[5]) for r in rows if r[4] not in ("call", "jmp"))
for (k, t), n in sorted(tg.items(), key=lambda z: -z[1]):
    print("%-8s %-40s %d" % (k, t, n))
