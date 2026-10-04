"""d3d_inventory.py - inventory of the statically linked Xbox D3D8 library.

For every function in the D3D section (functions.tsv) count direct call/jmp
sites (capstone, all functions in all x86 sections) and classify callers:
  NiXBoxRenderer   named NiXBoxRenderer::* virtuals
  NiXBoxLib        FUN_ in the NetImmerse Xbox renderer library ranges
                   0x146020-0x14cd50 (NiXBoxRenderer TU) and
                   0x155000-0x166871 (NiXBox* helper classes, CRT starts 0x166871)
  NiOther          other Ni* classes / NetImmerse core 0xfa000-0x146020, 0x14cd50-0x155000
  Game             .text 0x11000-0xf3000 (FUN_ and game classes)
  XMV / DSOUND / XPP   library sections
  OtherLib         XAPILIB / CRT / rest of .text
  D3D              internal (D3D section)
Also counts pointer references to D3D code from D3D raw data, .rdata, .data
(function tables, e.g. complex render state table 0x2420b0).

Output: analysis/loader/d3d_functions.tsv
Usage:  python d3d_inventory.py
"""
import csv, os, struct, sys, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from d3d_common import attribute

ROOT = r"D:\FuturamaDecomp"
EXP = os.path.join(ROOT, r"analysis\retail_eu_export")
OUTF = os.path.join(ROOT, r"analysis\loader\d3d_functions.tsv")

x = Xbe(os.path.join(ROOT, r"orig\retail_eu\default.xbe"))
sec = {n: (va, vsz, raw, rsz) for n, va, vsz, raw, rsz in x.sections}
D3D_LO = sec["D3D"][0]
D3D_CODE_HI = D3D_LO + sec["D3D"][3]

funcs = []
for r in csv.DictReader(open(os.path.join(EXP, "functions.tsv")), delimiter="\t"):
    funcs.append((int(r["address"], 16), int(r["size"]), r["block"], r["name"]))
funcs.sort()
byaddr = {f[0]: f for f in funcs}
d3dfuncs = [f for f in funcs if f[2] == "D3D"]
d3dset = {f[0] for f in d3dfuncs}

def owner(a, blk, nm):
    if blk == "D3D":
        return "D3D"
    if blk in ("XMV", "DSOUND", "XPP"):
        return blk
    if nm.startswith("NiXBoxRenderer::"):
        return "NiXBoxRenderer"
    if 0x146020 <= a < 0x14cd50 or 0x155000 <= a < 0x166871:
        return "NiXBoxLib"
    if nm.startswith("XAPILIB::") or nm.startswith("xboxkrnl"):
        return "OtherLib"
    if 0xfa000 <= a < 0x146020 or 0x14cd50 <= a < 0x155000:
        return "NiOther"
    if 0x11000 <= a < 0xf3000:
        return "Game"
    return "OtherLib"

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
sites = collections.defaultdict(list)      # callee -> [(caller, insn_addr, kind)]
for i, (a, sz, blk, nm) in enumerate(funcs):
    if blk not in sec or blk in (".rdata", ".data", "DOLBY"):
        continue
    va, vsz, raw, rsz = sec[blk]
    end = funcs[i + 1][0] if i + 1 < len(funcs) else va + rsz
    end = min(end, va + rsz)
    if end <= a:
        continue
    for ins in md.disasm(x.read(a, end - a), a):
        if ins.mnemonic == "call" or ins.mnemonic.startswith("j"):
            op = ins.operands[0] if ins.operands else None
            if op is not None and op.type == CS_OP_IMM:
                t = op.imm & 0xffffffff
                if t in d3dset and t != a:
                    ca, cn = attribute(ins.address)
                    if ca not in byaddr:   # function missing in functions.tsv
                        byaddr[ca] = (ca, 0, blk, cn)
                    sites[t].append((ca, ins.address, "call" if ins.mnemonic == "call" else "jmp"))

# ret N of every D3D function (stack cleanup -> stdcall arg bytes; 'ret' = cdecl/no args)
rets = {}
va_, vsz_, raw_, rsz_ = sec["D3D"]
for i, (a, sz, blk, nm) in enumerate(funcs):
    if blk != "D3D":
        continue
    end = min(funcs[i + 1][0] if i + 1 < len(funcs) else va_ + rsz_, va_ + rsz_, a + max(sz, 1))
    rs = set()
    for ins in md.disasm(x.read(a, end - a), a):
        if ins.mnemonic == "ret":
            rs.add(ins.op_str or "0")
        elif ins.mnemonic == "jmp" and ins.operands and ins.operands[0].type == CS_OP_IMM:
            t = ins.operands[0].imm & 0xffffffff
            if not (a <= t < end):
                rs.add("tail:%08x" % t)
    if not rs:   # body not contiguous within Ghidra size -> fall back to next function start
        end2 = min(funcs[i + 1][0] if i + 1 < len(funcs) else va_ + rsz_, va_ + rsz_)
        for ins in md.disasm(x.read(a, end2 - a), a):
            if ins.mnemonic == "ret":
                rs.add(ins.op_str or "0")
    rets[a] = ",".join(sorted(rs))

# pointer references (tables) to D3D function starts
ptrs = collections.defaultdict(list)
for blk in ("D3D", ".rdata", ".data"):
    va, vsz, raw, rsz = sec[blk]
    data = x.b[raw:raw + rsz]
    for off in range(0, rsz - 3, 4):
        v = struct.unpack_from("<I", data, off)[0]
        if v in d3dset:
            ptrs[v].append(va + off)

NOTES = {
    0x23a520: "Vermutung D3DIndexBuffer_GetDesc (writes Format=0x65, Type=7; ret 8)",
    0x240390: "Vermutung Direct3DCreate8 (mov eax,1; ret 4; result stored by NiXBoxRenderer::Create 0x14c170)",
    0x237190: "5-byte tail jmp to Get2DSurfaceDesc 0x23aaa0",
    0x2357d0: "__fastcall ecx=NV2A method header, edx=value; header table 0x293828; 27/28 external sites pass a constant header, only 0x147904 is variable [verify_d3d]",
    0x2392b0: "push-buffer MakeSpace -> D3D_MakeRequestedSpace_8(0x239130); 77 internal sites in 63 real functions; int_callers=64 is an artifact: attribute() counts sub_0023b644 (truncated tail of D3DDevice_Clear) and sub_0023fe67 (tail of FUN_0023fdb0 behind jump table 0x23fe67) as functions [verify_d3d_r2]",
    0x23b790: "GPU interrupt service routine: passed as immediate to KeInitializeInterrupt by CMiniport_InitHardware (push 0x23b790 at 0x23c926); ptr_refs only counts data sections [verify_d3d]",
    0x23c1d0: "GPU DPC routine: passed as immediate to KeInitializeDpc by CMiniport_InitHardware (push 0x23c1d0 at 0x23c898); ptr_refs only counts data sections [verify_d3d]",
    0x241c40: "D3D built-in movie capture to cache file (strings 0x2437c0..0x243864), only if dev+0x498/0x49c set",
    0x235770: "static initializer, pointer in C++ init table at 0x2d3e6c",
    0x239d20: "CDevice init (called by Direct3D_CreateDevice)",
    0x23b118: "CMiniport video mode selection (Width,Height,Refresh,Flags,Format,Interval,...)",
}
DEV = {"Direct3D_CreateDevice", "D3D_SetPushBufferSize", "FUN_00240390", "D3D_GetAdapterIdentifier", "D3D_GetDeviceCaps",
       "D3D_CheckDeviceFormat", "D3DDevice_AddRef", "D3DDevice_Release", "D3DDevice_Swap", "D3DDevice_Clear",
       "D3DDevice_SetRenderTarget", "D3DDevice_GetRenderTarget2", "D3DDevice_GetDepthStencilSurface2",
       "D3DDevice_SetViewport", "D3DDevice_SetScreenSpaceOffset", "D3DDevice_GetDisplayMode",
       "D3DDevice_GetDisplayFieldStatus", "D3DDevice_InsertFence", "D3DDevice_BlockOnFence", "D3DDevice_IsFencePending"}
SHD = {"D3DDevice_CreateVertexShader", "D3DDevice_SetVertexShader", "D3DDevice_CreatePixelShader",
       "D3DDevice_DeletePixelShader", "D3DDevice_SetPixelShader"}
DRW = {"D3DDevice_DrawVertices", "D3DDevice_DrawIndexedVertices"}
STA = {"D3DDevice_SetTransform", "D3DDevice_SetMaterial", "D3DDevice_SetLight", "D3DDevice_LightEnable",
       "D3DDevice_SetTexture", "D3DDevice_SetPalette", "D3DDevice_SetIndices", "D3DDevice_SetStreamSource",
       "D3DDevice_SetTextureState_TexCoordIndex", "D3DDevice_SetTextureState_BumpEnv"}
def category(nm, ext):
    n = nm.replace("D3D8::", "").replace("xboxkrnl.exe::", "")
    if not ext:
        return "internal"
    if n in DEV: return "device/frame"
    if n in SHD: return "shader"
    if n in DRW: return "draw"
    if n in STA: return "state"
    if "SetRenderState" in n: return "renderstate"
    return "resource"

KINDS = ["NiXBoxRenderer", "NiXBoxLib", "NiOther", "Game", "XMV", "DSOUND", "XPP", "OtherLib"]
rows = []
for a, sz, blk, nm in d3dfuncs:
    s = sites.get(a, [])
    ext = [z for z in s if byaddr[z[0]][2] != "D3D"]
    intl = [z for z in s if byaddr[z[0]][2] == "D3D"]
    extcallers = sorted({z[0] for z in ext})
    cnt = collections.Counter(owner(c, byaddr[c][2], byaddr[c][3]) for c in extcallers)
    rows.append({
        "address": "%08x" % a, "section": blk, "size": sz, "name": nm, "ret": rets.get(a, ""),
        "category": category(nm, len(extcallers)), "note": NOTES.get(a, ""),
        "ext_call_sites": len(ext), "ext_callers": len(extcallers),
        **{"callers_" + k: cnt.get(k, 0) for k in KINDS},
        "int_callers": len({z[0] for z in intl}), "int_call_sites": len(intl),
        "ptr_refs": len(ptrs.get(a, [])),
        "ptr_ref_addrs": ",".join("%08x" % p for p in ptrs.get(a, [])[:6]),
        "ext_caller_list": ",".join("%08x:%s" % (c, byaddr[c][3]) for c in extcallers),
    })

cols = ["address", "section", "size", "name", "category", "ret", "ext_call_sites", "ext_callers"] + \
       ["callers_" + k for k in KINDS] + ["int_callers", "int_call_sites", "ptr_refs", "ptr_ref_addrs", "note", "ext_caller_list"]
with open(OUTF, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=cols, delimiter="\t")
    w.writeheader()
    for r in rows:
        w.writerow(r)

# summary
named = [r for r in rows if r["name"].startswith("D3D8::")]
print("D3D section functions:", len(rows), " named D3D8::", len(named))
print("with external callers:", sum(1 for r in rows if r["ext_callers"]),
      " named:", sum(1 for r in named if r["ext_callers"]))
print("external call sites total:", sum(r["ext_call_sites"] for r in rows))
print("only internal/table:", sum(1 for r in rows if not r["ext_callers"] and (r["int_callers"] or r["ptr_refs"])))
print("no call/data-pointer refs (code immediates not scanned, see verify_d3d):", [r["address"] + " " + r["name"] for r in rows if not r["ext_callers"] and not r["int_callers"] and not r["ptr_refs"]])
allc = collections.defaultdict(set)
for a, sz, blk, nm in d3dfuncs:
    for c, ia, k in sites.get(a, []):
        if byaddr[c][2] != "D3D":
            allc[owner(c, byaddr[c][2], byaddr[c][3])].add(c)
for k in KINDS:
    print("%-15s distinct caller functions: %d" % (k, len(allc[k])))
for k in KINDS:
    used = sorted({byaddr[a][3] for a, sz, blk, nm in d3dfuncs for c, ia, kk in sites.get(a, [])
                   if byaddr[c][2] != "D3D" and owner(c, byaddr[c][2], byaddr[c][3]) == k})
    print(k, len(used), [u.replace("D3D8::", "") for u in used])
