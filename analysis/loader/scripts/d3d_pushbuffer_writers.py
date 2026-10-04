"""d3d_pushbuffer_writers.py - which functions touch the NV2A push buffer?
A function counts as push-buffer writer if it references g_Device.m_pPut (0x244970) /
m_pThreshold (0x244974) as absolute operand, loads D3D__pDevice (0x244968), or calls
D3DDevice_MakeSpace (0x2392b0) / D3D_MakeRequestedSpace_8 (0x239130) / BeginStateBig (0x2392c0).
Scans ALL x86 sections. Usage: python d3d_pushbuffer_writers.py"""
import csv, sys, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
sec = {n: (va, vsz, raw, rsz) for n, va, vsz, raw, rsz in x.sections}
funcs = sorted((int(r["address"], 16), r["block"], r["name"]) for r in
               csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"), delimiter="\t"))
PUT, THR, PDEV = 0x244970, 0x244974, 0x244968
MS = {0x2392b0: "MakeSpace", 0x239130: "MakeRequestedSpace", 0x2392c0: "BeginStateBig"}
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
res = collections.defaultdict(collections.Counter)
for i, (a, blk, nm) in enumerate(funcs):
    if blk not in sec or blk in (".rdata", ".data", "DOLBY"):
        continue
    va, vsz, raw, rsz = sec[blk]
    end = min(funcs[i + 1][0] if i + 1 < len(funcs) else va + rsz, va + rsz)
    if end <= a: continue
    for ins in md.disasm(x.read(a, end - a), a):
        for op in ins.operands:
            if op.type == CS_OP_IMM:
                v = op.imm & 0xffffffff
                if v in MS and (ins.mnemonic == "call" or ins.mnemonic.startswith("j")):
                    res[(a, blk, nm)][MS[v]] += 1
                elif v in (PUT, PDEV):
                    res[(a, blk, nm)]["imm_dev"] += 1
            elif op.type == CS_OP_MEM:
                v = op.mem.disp & 0xffffffff
                if v == PUT: res[(a, blk, nm)]["m_pPut"] += 1
                elif v == THR: res[(a, blk, nm)]["m_pThreshold"] += 1
                elif v == PDEV: res[(a, blk, nm)]["D3D__pDevice"] += 1
byblk = collections.Counter(k[1] for k in res)
print("functions referencing push buffer / device by section:", dict(byblk))
pb = {k: c for k, c in res.items() if c["m_pPut"] or c["MakeSpace"] or c["MakeRequestedSpace"] or c["BeginStateBig"]}
print("push-buffer writers (m_pPut or MakeSpace*) by section:", dict(collections.Counter(k[1] for k in pb)))
for k, c in sorted(res.items()):
    if k[1] != "D3D":
        print("OUTSIDE D3D:", "%08x" % k[0], k[2], dict(c))
