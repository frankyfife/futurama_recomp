"""Round-2: entries of the C/C++ init tables (_cinit, 0x2bdef0..0x2d5b30 and
0x2d5b34..0x2d5b48) that point into DSOUND / XMV / XPP (library static
initializers that run before main), with their disassembly."""
from verify_avi_r2_common import X, FNAME, dis, fmt, Tee
T = Tee("verify_avi_r2_inittab.out")
for lo, hi, nm in ((0x2bdef0, 0x2d5b34, "C++ init"), (0x2d5b34, 0x2d5b4c, "C init")):
    for a in range(lo, hi, 4):
        p = X.u32(a)
        s = X.section(p)
        if s in ("DSOUND", "XMV", "XPP", "D3D"):
            T("%s @%08x -> %08x (%s) %s" % (nm, a, p, s, FNAME.get(p, "")))
            for ins in dis(p, 16, stop_ret=True):
                t = ""
                if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                    t = FNAME.get(int(ins.op_str, 16), "")
                T("      " + fmt(ins) + ("  ; " + t if t else ""))
