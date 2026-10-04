"""d3d_simple_sites.py - list all call sites of D3DDevice_SetRenderState_Simple (0x2357d0,
__fastcall ecx=NV2A method header, edx=value) outside the D3D section, decode the
NV2A method (ecx & 0x1ffc, count = ecx>>18) and map it back to D3DRENDERSTATETYPE via
the D3D table D3DSIMPLERENDERSTATEENCODE at 0x293828 (read by SetRenderStateNotInline 0x235800).
Usage: python d3d_simple_sites.py"""
import csv, os, sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
enc = [x.u32(0x293828 + 4 * i) for i in range(0x5c)]
inv = {}
for i, e in enumerate(enc):
    inv.setdefault(e, i)
print("encode table 0x293828 (state -> NV2A header):")
print(" ".join("%02x:%05x" % (i, e) for i, e in enumerate(enc)))
rows = list(csv.DictReader(open(os.path.join(os.path.dirname(__file__), "out", "d3d_refs.tsv")), delimiter="\t"))
md = Cs(CS_ARCH_X86, CS_MODE_32)
n = 0
for r in rows:
    if r["target"] != "002357d0" or r["kind"] not in ("call", "jmp"):
        continue
    ia = int(r["insn_addr"], 16)
    # walk back up to 40 bytes to find 'mov ecx, imm'
    ecx = None
    for back in range(40, 0, -1):
        ins = list(md.disasm(x.read(ia - back, back), ia - back))
        if ins and ins[-1].address + ins[-1].size == ia:
            for i in ins:
                if i.mnemonic == "mov" and i.op_str.startswith("ecx, 0x"):
                    ecx = int(i.op_str.split(",")[1], 16)
            break
    st = inv.get(ecx) if ecx is not None else None
    n += 1
    print("%s %-22s %s ecx=%s state=%s" % (r["insn_addr"], r["func_name"], r["kind"],
          "%05x" % ecx if ecx is not None else "var", "0x%02x" % st if st is not None else "?"))
print("sites:", n)
