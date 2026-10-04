"""XBE section header flags (Writable=1, Preload=2, Executable=4, InsertedFile=8, HeadPageRO=0x10, TailPageRO=0x20)
and who references DOLBY / $$XTIMAGE section headers."""
from verify_startup_common import *
b = X.b
nsect, sect = struct.unpack_from("<II", b, 0x11C)
for i in range(nsect):
    o = sect - 0x10000 + i * 0x38
    fl, va, vsz, raw, rsz, na = struct.unpack_from("<6I", b, o)
    name = b[na - 0x10000:b.index(b"\0", na - 0x10000)].decode()
    flags = [n for bit, n in ((1, "W"), (2, "Preload"), (4, "X"), (8, "Inserted"), (0x10, "HeadRO"), (0x20, "TailRO")) if fl & bit]
    hdr = sect + i * 0x38
    refs = [(n, hex(a)) for n, a in abs_refs(hdr, CODE)]
    print("%-10s hdr %#x flags %#04x %-28s va %#x  code refs to header: %s" % (name, hdr, fl, ",".join(flags), va, refs))
