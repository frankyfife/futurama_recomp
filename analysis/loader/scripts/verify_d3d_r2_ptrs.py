"""verify_d3d_r2_ptrs.py - data pointers and code immediates into D3D code from outside (review pass 2):
 * aligned dwords in .rdata/.data/DOLBY/XMV/DSOUND/XPP raw data equal to a D3D function start
 * non-branch immediates/displacements in non-D3D code that point into D3D code (0x2342c0..raw end)
 * is 0x2d3e6c inside the C++ init table 0x2bdef0..0x2d5b30, what does 0x235770 do"""
from verify_d3d_r2_common import *
for s in [".rdata", ".data", "DOLBY"]:
    d = SECS[s]
    for off in range(0, d["rsz"] - 3, 4):
        v = struct.unpack_from("<I", XB.b, d["raw"] + off)[0]
        if v in D3DFN:
            print("data ptr %s %08x -> %08x %s" % (s, d["va"] + off, v, D3DFN[v][3]))
for s in [".text", "DSOUND", "XMV", "XPP"]:
    for ins in sweep(s):
        if ins.mnemonic == "call" or ins.mnemonic.startswith("j"):
            continue
        for op in ins.operands:
            v = None
            if op.type == X86_OP_IMM: v = op.imm & 0xffffffff
            elif op.type == X86_OP_MEM: v = op.mem.disp & 0xffffffff
            if v is not None and D3D_LO <= v < D3D_CODE_END:
                print("code imm %08x %s %s  (%s)  target_is_func=%s" % (ins.address, ins.mnemonic, ins.op_str, owner(ins.address)[1], v in D3DFN))
print("0x2d3e6c in C++ init table:", 0x2bdef0 <= 0x2d3e6c < 0x2d5b30)
pdis(0x235770, 0x30)
