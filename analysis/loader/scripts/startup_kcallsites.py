"""For given kernel thunk slots, find every 'call dword ptr [slot]' in code and print the preceding
instructions (argument pushes) and the following instruction (to see whether the caller cleans the stack).
usage: startup_kcallsites.py <slot> [<slot> ...]   (slot index into thunk table 0x293560)"""
import sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = Cs(CS_ARCH_X86, CS_MODE_32)
def sites(slot):
    pat = b"\xff\x15" + struct.pack("<I", 0x293560 + 4 * slot)
    pat2 = b"\xff\x25" + struct.pack("<I", 0x293560 + 4 * slot)
    out = []
    for name, va, vsz, raw, rsz in X.sections:
        if name not in (".text", "D3D", "DSOUND", "XMV", "XPP"): continue
        data = X.b[raw:raw + rsz]
        for p_ in (pat, pat2):
            p = data.find(p_)
            while p >= 0:
                out.append(va + p); p = data.find(p_, p + 1)
    return sorted(out)
def context(addr, back=0x30, fwd=8):
    code = X.read(addr - back, back + fwd)
    for start in range(0, back):
        ins = list(md.disasm(code[start:], addr - back + start))
        if any(i.address == addr for i in ins):
            pre = [i for i in ins if i.address < addr][-8:]
            post = [i for i in ins if i.address >= addr][:2]
            return pre + post
    return []
if __name__ == "__main__":
    for s in map(int, sys.argv[1:]):
        print("=== slot %d (ord %d)" % (s, X.u32(0x293560 + 4 * s) & 0x7fffffff))
        for a in sites(s):
            print("  site %08x" % a)
            for i in context(a):
                print("     %08x %s %s" % (i.address, i.mnemonic, i.op_str))
