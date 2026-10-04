"""Round-2: dump vtables named in avi.md and find who installs them.

For each vtable: slots until a non-code pointer, Ghidra function name of
each slot, and every instruction in .text/DSOUND that uses the vtable
address as imm32 (constructor stores).
"""
import struct
from verify_avi_r2_common import X, SECT, FNAME, func_of, Tee, linear_starts, MD

T = Tee("verify_avi_r2_vtables.out")
VT = {
    0x2a3978: "SoundManager base", 0x2a3b80: "SoundManager", 0x2a3920: "SoundData base",
    0x2a3be8: "SoundData", 0x2a38b8: "SoundVoice base", 0x2a3b18: "SoundVoice",
    0x2a14f0: "Controller", 0x2a5f5c: "CDirectSoundStream", 0x2a1810: "App",
    0x2a5fac: "DSOUND vtbl w/ thunk",
}
code = []
for n in (".text", "D3D", "DSOUND", "XMV", "XPP"):
    va, vsz, raw, rsz = SECT[n]
    code.append((va, va + rsz))


def is_code(p):
    return any(a <= p < b for a, b in code)


for vt, nm in sorted(VT.items()):
    T("\n== vtable %08x (%s) ==" % (vt, nm))
    i = 0
    while i < 64:
        p = X.u32(vt + 4 * i)
        if not is_code(p):
            break
        fa, fn = func_of(p)
        tag = FNAME.get(p) or ("<inside %s>" % fn if fn else "<no Ghidra function>")
        T("  [%2d] +0x%02x %08x %s" % (i, 4 * i, p, tag))
        i += 1
    # imm32 refs
    pat = struct.pack("<I", vt)
    for n in (".text", "DSOUND"):
        va, vsz, raw, rsz = SECT[n]
        d = X.b[raw:raw + rsz]
        o = d.find(pat)
        while o >= 0:
            # find an instruction that contains this imm (try starts -1..-7)
            hit = None
            for back in range(1, 8):
                ins = list(MD.disasm(d[o - back:o - back + 15], va + o - back, 1))
                if ins and ins[0].size >= back + 4 and ins[0].address + ins[0].size >= va + o + 4:
                    hit = ins[0]
                    break
            fa, fn = func_of(va + o)
            T("  ref %08x in %s %s: %s" % (va + o, n, fn or "nofunc",
                                           "%s %s" % (hit.mnemonic, hit.op_str) if hit else "?"))
            o = d.find(pat, o + 1)
