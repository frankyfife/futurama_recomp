"""Find raw references (imm32 / disp32) to given VAs in .text via byte search
and capstone disassembly of the containing instruction."""
import sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
import capstone
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
def code_sections():
    for n, va, vsz, raw, rsz in X.sections:
        if n in (".text", "D3D", "DSOUND", "XMV", "XPP", "DOLBY"):
            yield n, va, raw, rsz
def find_refs(target):
    pat = struct.pack("<I", target)
    out = []
    for n, va, raw, rsz in code_sections():
        blob = X.b[raw:raw + rsz]
        i = blob.find(pat)
        while i >= 0:
            # try to decode an instruction that contains this imm (start 1..7 bytes before)
            hit = None
            for back in range(1, 8):
                s = i - back
                if s < 0: continue
                for ins in md.disasm(blob[s:s + 16], va + s):
                    if ins.address == va + s and ins.address + ins.size > va + i >= ins.address:
                        hit = ins
                    break
                if hit: break
            out.append((va + i, hit))
            i = blob.find(pat, i + 1)
    return out
if __name__ == "__main__":
    for t in sys.argv[1:]:
        t = int(t, 16)
        for loc, ins in find_refs(t):
            print(f"{t:08x} @ {loc:08x}: {ins.address:08x} {ins.mnemonic} {ins.op_str}" if ins else f"{t:08x} @ {loc:08x}: ?")
