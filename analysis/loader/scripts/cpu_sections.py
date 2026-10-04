"""cpu_sections.py - code that touches the XBE image header (0x10000..0x10000+hdrsize),
the section loading API (XeLoadSection/XeUnloadSection thunks, XAPI wrappers) and
the names of non-preloaded sections.
usage: python cpu_sections.py"""
import sys, struct, re, bisect, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from cpu_rd import RD
rd = RD().run()
X = rd.x
hdrsize = struct.unpack_from("<I", X.b, 0x108)[0]
nsect, secthdr = struct.unpack_from("<II", X.b, 0x11C)
print("header size %#x, %d sections, section headers at %#x" % (hdrsize, nsect, secthdr))
for i in range(nsect):
    o = secthdr - X.base + i * 0x38
    fl, va, vsz, raw, rsz, na, nrc, hp, tp = struct.unpack_from("<9I", X.b, o)
    print("  hdr %#x  %-10s flags %#04x va %#x vsz %#x namerefcnt@%#x headpage@%#x tailpage@%#x  preload=%d"
          % (secthdr + i * 0x38, X.sections[i][0], fl, va, vsz, nrc if False else secthdr + i*0x38 + 0x18, hp, tp, (fl >> 1) & 1))
fst = [f[0] for f in rd.funcs]
def owner(va):
    k = bisect.bisect_right(fst, va) - 1
    return rd.funcs[k][2] if k >= 0 else "-"
print("\ninstructions referencing the XBE header region 0x10000-%#x:" % (0x10000 + hdrsize))
for va in sorted(rd.insn):
    sz, mn, ops, bb = rd.insn[va]
    for m in re.finditer(r"0x([0-9a-f]+)", ops):
        v = int(m.group(1), 16)
        if 0x10000 <= v < 0x10000 + hdrsize:
            print("  %08x %-28s %-40s %s" % (va, bb.hex(), mn + " " + ops, owner(va)))
            break
print("\nreferences to section-API thunks / wrappers:")
targets = {0x2936b4: "XeLoadSection(thunk)", 0x2936b8: "XeUnloadSection(thunk)", 0xf9bf5: "XLoadSectionByHandle",
           0xf9c16: "XFreeSectionByHandle", 0xf9c34: "XGetSectionSize"}
for va in sorted(rd.insn):
    sz, mn, ops, bb = rd.insn[va]
    for t, nm in targets.items():
        if ("0x%x" % t) in ops:
            print("  %08x %-40s -> %-22s in %s" % (va, mn + " " + ops, nm, owner(va)))
