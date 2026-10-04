"""Check renderer.md claims about the geometry Display functions (vtable slot 33):
which renderer slot each one calls, and how renderer+0xc/+0x10 are written."""
import json
from verify_renderer_common import *
C = json.load(open(r"D:\FuturamaDecomp\analysis\retail_eu_ni_classes.json"))["classes"]
for cls in ("NiTriShape", "NiTriangles", "NiTriStrip", "NiTriStrips", "NiParticles", "NiAutoNormalParticles", "NiLines", "NiTriBasedGeom", "NiGeometry", "NiAVObject", "NiNode"):
    c = C.get(cls)
    if not c: print(cls, "missing"); continue
    vt = c["vtable"]
    a = X.u32(vt + 33 * 4)
    print(f"== {cls} vtable {vt:#x} slot33 -> {a:#x} {F.get(a, ('?',))[0]}")
    if not cls.startswith("NiTri") and cls not in ("NiParticles", "NiAutoNormalParticles", "NiLines"): continue
    ins = dis(a)
    for i, x in enumerate(ins):
        if x.mnemonic == "call" and "ptr [" in x.op_str:
            print("   vcall", fmt(x))
        if x.mnemonic == "mov" and x.op_str.startswith("dword ptr [") and ("+ 0xc]" in x.op_str.split(",")[0] or "+ 0x10]" in x.op_str.split(",")[0]):
            print("   store", fmt(x))
        if x.mnemonic == "call" and not "ptr" in x.op_str:
            try: t = int(x.op_str, 16); print("   call ", fmt(x), F.get(t, ("?",))[0])
            except ValueError: pass
