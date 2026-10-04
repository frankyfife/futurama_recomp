"""verify_avi_misc.py - small, independent evidence checks for avi.md (Verifikation section).
Each block prints the raw instructions / data the corresponding claim rests on."""
import struct, re
from verify_avi_common import *

def hdr(t): print("\n== " + t)

hdr("E_PENDING 0x8000000A: every occurrence in .text (game never stores it)")
_, va, vsz, raw, rsz = SEC[".text"]
d = x.b[raw:raw + rsz]; pat = struct.pack("<I", 0x8000000A); i = d.find(pat)
while i >= 0:
    a = va + i
    for back in range(1, 8):
        ins = next(md.disasm(x.read(a - back, 16), a - back), None)
        if ins and ins.address + ins.size == a + 4:
            print("  %08x %s %s" % (ins.address, ins.mnemonic, ins.op_str)); break
    i = d.find(pat, i + 1)

hdr("I3DL2 preset table (name ptr + 12 x DSI3DL2LISTENER fields, stride 0x34) and presets used by 0xcbfa0")
used = {0x2d6884: 0, 0x2d68b8: 1, 0x2d6920: 2, 0x2d6954: 3, 0x2d69bc: 4}
for p in range(0x2d6850, 0x2d6a58, 0x34):
    nm = x.cstr(u32(p - 4))
    v = struct.unpack("<iifffififfff", x.read(p, 48))
    print("  %08x %-13s %s %s" % (p, nm, ", ".join("%g" % f for f in v), ("<- SetEnvironment(%d)" % used[p]) if p in used else ""))
print("  0xcbfa0 pushes:", [hex(int(m, 16)) for i_ in dis(0xcbfa0, 0xcc00c) if i_.mnemonic == "push" for m in re.findall(r"0x2d6[0-9a-f]+", i_.op_str)])

hdr("Region suffix 0x2e7b70 (FUN_000a26a0) and archive-based region flag (FUN_000d26a0 strings)")
pdis(0xa26bd, 0xa26cc); pdis(0xa2707, 0xa270c); pdis(0xa2744, 0xa274e)
for s in (0x2a3f34, 0x2a3f44, 0x2a3f50): print("  %08x %r" % (s, x.cstr(s)))

hdr("dsstdfx loader: return value ignores DownloadEffectsImage HRESULT")
pdis(0xcbf04, 0xcbf0b); pdis(0xcbf67, 0xcbf86)

hdr("Game ADPCM wfx at SoundData+0x59: nAvgBytesPerSec = 36*ch*ch/64 (bug)")
pdis(0xcb80b, 0xcb856)

hdr("Controller thresholds: base ctor FUN_0009b7d0 vs ctor 0x9bf50 (channels set)")
ch = []
ins = dis(0x9bf50, 0x9c140)
for k, i_ in enumerate(ins):
    if i_.mnemonic == "call" and i_.op_str == "0x9b660":
        # pattern: push <threshold>; push <channel>; mov ecx,esi; call 0x9b660 (first one interleaved)
        for prev in reversed(ins[max(0, k - 12):k]):
            if prev.mnemonic == "push" and prev.op_str.startswith("0x") or (prev.mnemonic == "push" and prev.op_str.isdigit()):
                v = int(prev.op_str, 16) if prev.op_str.startswith("0x") else int(prev.op_str)
                if v < 16: ch.append(v); break
print("  channels given 0x3df00000 by 0x9bf50:", sorted(ch), "missing:", sorted(set(range(16)) - set(ch)))
print("  base defaults:", [hex(int(m, 16)) for i_ in dis(0x9b7d0, 0x9b8be) for m in re.findall(r"0x3[be][0-9a-f]{6}", i_.op_str)])
print("  feedback dwStatus (+0x154) stores in input code:",
      ["%x %s %s" % (i_.address, i_.mnemonic, i_.op_str) for i_ in dis(0x9b300, 0x9ca00) if re.match(r"dword ptr \[e[a-z]{2} \+ 0x154\],", i_.op_str)])

hdr("Voice update 0xcc5e0: DSOUND call sites inside 0xcc5e0..0xcca63")
n = 0
for i_ in dis(0xcc5e0, 0xcca63):
    if i_.mnemonic == "call":
        t = int(i_.op_str, 16) if i_.op_str.startswith("0x") else None
        if t and F.get(t, ("", 0, ""))[2] == "DSOUND": n += 1
    if i_.mnemonic == "ret": print("  ret at %08x" % i_.address)
print("  DSOUND calls:", n)
