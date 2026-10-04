"""verify_d3d_r2_common.py - helpers for the second, independent review pass
(verify_d3d_r2_*.py). Shares no code with d3d_common.py / d3d_inventory.py /
verify_d3d_common.py on purpose.

Function boundaries: functions.tsv (Ghidra). An instruction address that lies behind
the end of the Ghidra function (gap) is attributed to 'gap_<start of gap>' where the
gap starts at the first non-padding byte after the end of the preceding function."""
import csv, struct, sys, bisect, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

ROOT = r"D:\FuturamaDecomp"
EXP = ROOT + r"\analysis\retail_eu_export"
XB = Xbe(ROOT + r"\orig\retail_eu\default.xbe")
SECS = {n: dict(va=va, vsz=vsz, raw=raw, rsz=rsz) for n, va, vsz, raw, rsz in XB.sections}
D3D = SECS["D3D"]
D3D_LO, D3D_CODE_END, D3D_END = D3D["va"], D3D["va"] + D3D["rsz"], D3D["va"] + D3D["vsz"]
CODE = [".text", "D3D", "DSOUND", "XMV", "XPP"]

F = []
for r in csv.DictReader(open(EXP + r"\functions.tsv"), delimiter="\t"):
    F.append((int(r["address"], 16), int(r["size"]), r["block"], r["name"]))
F.sort()
FS = [f[0] for f in F]
FBY = {f[0]: f for f in F}
D3DFN = {f[0]: f for f in F if f[2] == "D3D"}

cs = Cs(CS_ARCH_X86, CS_MODE_32)
cs.detail = True

def u32(a):
    return struct.unpack("<I", XB.read(a, 4))[0]

def owner(a):
    """(start, name) of the function containing a; gaps -> gap_xxx"""
    i = bisect.bisect_right(FS, a) - 1
    if i < 0:
        return (None, "?")
    s, sz, blk, nm = F[i]
    if a < s + sz:
        return (s, nm)
    g = s + sz
    raw = XB.read(g, a - g) or b""
    k = 0
    while k < len(raw) and raw[k] in (0x90, 0xCC):
        k += 1
    # further split at ret/jmp followed by padding is not needed for counting groups
    return (g + k, "gap_%08x" % (g + k))

def sweep(secname, include_gap_before_first=True):
    """linear disassembly of a section, restarting at every function start."""
    s = SECS[secname]
    va, end = s["va"], s["va"] + s["rsz"]
    starts = sorted(set([f[0] for f in F if va <= f[0] < end] + ([va] if include_gap_before_first else [])))
    for i, a in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else end
        for ins in cs.disasm(XB.read(a, e - a), a):
            yield ins

def dis(a, n):
    return [(i.address, i.bytes.hex(), i.mnemonic, i.op_str) for i in cs.disasm(XB.read(a, n), a)]

def pdis(a, n, stop_at_ret=False):
    for ad, by, mn, op in dis(a, n):
        print("  %08x %-20s %s %s" % (ad, by, mn, op))
        if stop_at_ret and mn == "ret":
            break

def group(a, nm):
    """caller groups exactly as defined in d3d.md section 1"""
    sec = XB.section(a)
    if sec in ("XMV", "DSOUND", "XPP", "D3D"):
        return sec
    if nm.startswith("NiXBoxRenderer::"):
        return "R"
    if 0x146020 <= a < 0x14cd50 or 0x155000 <= a < 0x166871:
        return "X"
    if 0xfa000 <= a < 0x146020 or 0x14cd50 <= a < 0x155000:
        return "NiOther"
    if 0x11000 <= a < 0xf3de0:
        return "G"
    return "OtherLib"
