"""Independent check of the NiRenderer/NiXBoxRenderer vtable claims in renderer.md
and nixboxrenderer_vtable.tsv: slot count (what follows the last slot), address
per slot, override classification, ret-N per slot, and the RTTI init stub."""
import sys, csv, re, struct
from verify_renderer_common import *
NIR, NIX, PURE = 0x2a9ef0, 0x2aa7c0, 0x1668b6
def is_code(v): return X.section(v) == ".text"
def count(vt):
    n = 0
    while is_code(X.u32(vt + 4 * n)): n += 1
    return n
print("NiRenderer slots", count(NIR), "next dword", hex(X.u32(NIR + 4 * count(NIR))), X.cstr(X.u32(NIR + 4 * count(NIR))) if X.section(X.u32(NIR + 4*count(NIR))) else None)
print("NiXBoxRenderer slots", count(NIX), "next dword", hex(X.u32(NIX + 4 * count(NIX))), X.cstr(X.u32(NIX + 4 * count(NIX))) if X.section(X.u32(NIX + 4*count(NIX))) else None)
# what precedes the vtables (RTTI/locator?)
print("dword before NIR", hex(X.u32(NIR - 4)), "before NIX", hex(X.u32(NIX - 4)))
# _purecall body
print("purecall body:", "; ".join(f"{i.mnemonic} {i.op_str}" for i in dis(PURE, 12)))
def retn(a, depth=0):
    sz = max(F.get(a, ("", 0, ""))[1], 8)
    ins = dis(a, sz + 8)
    if ins and ins[0].mnemonic == "jmp" and depth < 3:
        try: return retn(int(ins[0].op_str, 16), depth + 1)
        except ValueError: pass
    return sorted({(int(i.op_str, 0) if i.op_str else 0) for i in dis(a, sz) if i.mnemonic == "ret"})
tsv = {int(r["slot"]): r for r in csv.DictReader(open(r"D:\FuturamaDecomp\analysis\loader\nixboxrenderer_vtable.tsv"), delimiter="\t")}
cnt = {"no": 0, "yes": 0, "yes(pure)": 0, "new": 0}
mism = []
for s in range(75):
    a = X.u32(NIX + 4 * s)
    p = X.u32(NIR + 4 * s) if s < 72 else None
    ov = "new" if p is None else "no" if p == a else "yes(pure)" if p == PURE else "yes"
    cnt[ov] += 1
    r = tsv[s]
    if int(r["address"], 16) != a or r["overrides_parent"] != ov or (p is not None and int(r["parent_impl"], 16) != p):
        mism.append((s, r["address"], r["overrides_parent"], hex(a), ov))
    print(f"{s:2d} {a:08x} parent={p if p is None else hex(p)} {ov:9s} ret={retn(a)} {F.get(a,('?',))[0]}")
print("counts", cnt, "TSV mismatches", mism)
