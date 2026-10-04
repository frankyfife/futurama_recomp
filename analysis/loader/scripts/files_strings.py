"""List path-like strings (ASCII + UTF-16LE) in retail_eu default.xbe with VA and code xrefs.

Xrefs = raw occurrences of the 32-bit VA inside .text (push imm32 / mov r,imm32 / data ptrs),
reported as the address of the instruction-ish location (occurrence VA). Use decomp/*.c for context.
"""
import re, struct, sys
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe

X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
PAT = re.compile(r"(?i)([a-z]:\|\device|\.img|\.xmv|\.ful|\.ldb|\.txt|\.xbx|\.dat|\.nif|\.ucf|\.wav|\.key|\.tga|\.fnt|\|/|save|meta|title|\.ini|\.cfg|\.bin|\.sav|\.dbg)")

def sect_bytes(name):
    for n, va, vsz, raw, rsz in X.sections:
        if n == name:
            return va, X.b[raw:raw + rsz]

text_va, text = sect_bytes(".text")
other = [sect_bytes(n) for n in (".rdata", ".data", "XPP", "XMV", "D3D", "DSOUND")]

def xrefs(v):
    needle = struct.pack("<I", v)
    out = []
    for base, blob in [(text_va, text)] + other:
        i = blob.find(needle)
        while i >= 0:
            out.append(base + i)
            i = blob.find(needle, i + 1)
    return out

def scan(blob, base, wide=False):
    if wide:
        rx = re.compile(rb"(?:[\x20-\x7e]\x00){4,}")
    else:
        rx = re.compile(rb"[\x20-\x7e]{4,}")
    for m in rx.finditer(blob):
        s = m.group().decode("utf-16le" if wide else "latin-1")
        if PAT.search(s):
            yield base + m.start(), s

for name in (".rdata", ".data", "XPP", "XMV", "DSOUND", "D3D"):
    va, blob = sect_bytes(name)
    for wide in (False, True):
        for v, s in scan(blob, va, wide):
            xr = xrefs(v)
            print(f"{v:08x}\t{name}\t{'W' if wide else 'A'}\t{','.join('%x' % a for a in xr[:8])}\t{s!r}")
