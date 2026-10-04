"""verify_cpu_tls.py - re-check cpu.md key facts 3/4/5 and section 2.2 (TLS directory, _tls_index,
XapiThreadStartup, GetLastError/SetLastError, _getptd/_freeptd, KTHREAD/KPRCB field use).
Pure disassembly dumps + a few computed values; read-only."""
import sys, struct
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *

print("== XBE header TLS pointer / section flags")
tls = struct.unpack_from("<I", X.b, 0x12C)[0]
print("hdr+0x12C TLS dir VA = %08x" % tls)
d = X.read(tls, 24)
s, e, idx, cb, zf, ch = struct.unpack("<6I", d)
print("TLS dir: StartRaw=%x EndRaw=%x AddressOfIndex=%x Callbacks=%x SizeOfZeroFill=%x Characteristics=%x" % (s, e, idx, cb, zf, ch))
print("index var section:", X.section(idx), " raw-backed:", X.va2off(idx) is not None)
nsect, shdr = struct.unpack_from("<II", X.b, 0x11C)
for k in range(nsect):
    o = shdr - X.base + k * 0x38
    fl, va, vsz, raw, rsz, na = struct.unpack_from("<6I", X.b, o)
    nm = X.b[na - X.base:X.b.index(b"\0", na - X.base)].decode()
    print("  sect %d hdr@%08x %-10s flags=%02x va=%08x vsz=%06x raw=%06x rsz=%06x" % (k, shdr + k * 0x38, nm, fl, va, vsz, raw, rsz))
print("hdr: SizeOfHeaders=%x SizeOfImage=%x SizeOfImageHeader=%x cert=%x" % (struct.unpack_from("<3I", X.b, 0x108) + (struct.unpack_from("<I", X.b, 0x118)[0],)))

print("\n== entry 0xf84dd .. 0xf8540")
show(0xf84dd, 0xf8545)
# emulate TlsDataSize computation
tds = ((zf + (e - s) + 0xF) & ~0xF) + 4
print("computed TlsDataSize = %#x ; idiv(-4) -> %d" % (tds, int(tds / -4)))

print("\n== main thread 0xf8463 .. 0xf84dd")
show(0xf8463, 0xf84dd)

print("\n== XapiThreadStartup 0xf99da .. 0xf9a72")
show(0xf99da, 0xf9a72)

print("\n== CreateThread 0xf9a72 .. 0xf9ae0 + GetCurrentThreadId")
show(0xf9a72, 0xf9aee)

print("\n== GetLastError / SetLastError 0xf6e54 .. 0xf6eb0")
show(0xf6e54, 0xf6eb0)

print("\n== _getptd 0x1692d0 .. 0x169352")
show(0x1692d0, 0x169352)
print("\n== 0x169352 .. 0x1693c0 (_freeptd?)")
show(0x169352, 0x1693c0)
print("\n== _mtinit 0x169467 .. 0x1694b0")
show(0x169467, 0x1694b0)

print("\n== all fs:[0x20] sites with the following 6 instructions")
for a in (0xf5a41, 0xf5ab8, 0xf634f, 0xf6384, 0xf82b9, 0xf8473, 0xf999e, 0x23a1e5):
    print("-- %x" % a)
    for i in dis(a, 9):
        print("  %08x  %-22s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))

print("\n== KTHREAD field offsets used after fs:[0x28] (next 6 insns, look for [eax+..])")
for a in (0xf6e60, 0xf6e88, 0xf848f, 0xf99ea, 0xf9ac3, 0xf9ae0, 0x1692e6, 0x169372, 0x1709a9):
    ins = dis(a, 6)
    print("-- %x: " % a + " | ".join("%s %s" % (i.mnemonic, i.op_str) for i in ins))

