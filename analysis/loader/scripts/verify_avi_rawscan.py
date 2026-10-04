"""verify_avi_rawscan.py - independent re-check of the DSOUND/XMV/XPP API surface (avi.md 2.2/3.1/4.1).
Differences to avi_rawcalls.py:
  * E8/E9 targets are accepted ANYWHERE inside DSOUND/XMV/XPP (not only Ghidra function starts)
  * boundary validated by linear disassembly from the owning Ghidra function start when possible,
    else by a backward walk (as in the original)
  * additionally: FF15/FF25 [abs] calls, and every unaligned imm32/disp32 in non-target code equal
    to a DSOUND/XMV/XPP Ghidra function start (function pointers passed around)
  * section function statistics (named vs FUN_)
"""
import struct, collections
from verify_avi_common import *

TS = ("DSOUND", "XMV", "XPP")
CODE = (".text", "D3D", "DSOUND", "XMV", "XPP")
rng = {n: (SEC[n][1], SEC[n][1] + SEC[n][2]) for n in TS}
def tsec(v):
    for n, (a, b) in rng.items():
        if a <= v < b: return n

def boundary_ok(a):
    o = owner(a)
    if o is not None and a - o < 0x4000:
        for ins in md.disasm(x.read(o, a - o + 16), o):
            if ins.address == a: return True
            if ins.address > a: break
        # fall through: function body may contain data; try backward walk
    for back in range(1, 64):
        st = a - back
        for ins in md.disasm(x.read(st, back + 8), st):
            if ins.address == a: return True
            if ins.address > a: break
    return False

# 1) section statistics
print("== section function statistics (functions.tsv)")
for n in TS:
    fl = [a for a in FS if F[a][2] == n]
    named = [a for a in fl if not F[a][0].startswith("FUN_")]
    print("  %-7s functions=%d named=%d FUN_=%d" % (n, len(fl), len(named), len(fl) - len(named)))

# 2) direct E8/E9
hits = collections.defaultdict(list)
nonstart = []
for sname in CODE:
    _, va, vsz, raw, rsz = SEC[sname]
    d = x.b[raw:raw + rsz]
    for i in range(len(d) - 5):
        if d[i] not in (0xE8, 0xE9): continue
        src = va + i
        tgt = (src + 5 + struct.unpack_from("<i", d, i + 1)[0]) & 0xffffffff
        t = tsec(tgt)
        if not t or t == sname: continue
        if not boundary_ok(src): continue
        kind = "call" if d[i] == 0xE8 else "jmp"
        if tgt in F: hits[tgt].append((src, kind, sname))
        else: nonstart.append((src, kind, tgt))
print("\n== direct E8/E9 into DSOUND/XMV/XPP from other sections")
tot = collections.Counter()
for tgt in sorted(hits):
    print("%08x %-46s rets=%s sites=%d" % (tgt, name(tgt), sorted(rets(tgt)), len(hits[tgt])))
    for src, kind, sn in hits[tgt]:
        o = owner(src)
        print("     %-4s @%08x [%s] in %s" % (kind, src, sn, ("%08x %s" % (o, name(o))) if o else "NO-GHIDRA-FUNCTION"))
        tot[(F[tgt][2], sn)] += 1
print("  totals (target section, source section):", dict(tot))
print("  distinct targets per section:", collections.Counter(F[t][2] for t in hits))
print("  E8/E9 into non-function-start addresses:", [(hex(a), k, hex(t)) for a, k, t in nonstart])

# 3) FF15 / FF25 through absolute pointers
print("\n== FF15/FF25 [abs] whose pointer slot holds a DSOUND/XMV/XPP address")
for sname in (".text", "D3D"):
    _, va, vsz, raw, rsz = SEC[sname]
    d = x.b[raw:raw + rsz]
    for i in range(len(d) - 6):
        if d[i] == 0xFF and d[i + 1] in (0x15, 0x25):
            p = struct.unpack_from("<I", d, i + 2)[0]
            if is_init(p):
                v = u32(p)
                if tsec(v): print("  @%08x -> [%08x]=%08x %s" % (va + i, p, v, name(v)))

# 4) any imm32 / disp32 in .text/D3D equal to a DSOUND/XMV/XPP function start
print("\n== imm32 values in .text/D3D equal to DSOUND/XMV/XPP function starts (instruction-validated)")
starts = set(a for a in FS if F[a][2] in TS)
for sname in (".text", "D3D"):
    _, va, vsz, raw, rsz = SEC[sname]
    d = x.b[raw:raw + rsz]
    for i in range(len(d) - 4):
        v = struct.unpack_from("<I", d, i)[0]
        if v in starts:
            a = va + i
            # find instruction containing this imm
            found = None
            for back in range(1, 12):
                st = a - back
                if not boundary_ok(st): continue
                ins = next(md.disasm(x.read(st, 16), st), None)
                if ins and ins.address + ins.size >= a + 4:
                    found = ins; break
            print("  @%08x value %08x %-40s insn: %s" % (a, v, name(v),
                  ("%08x %s %s" % (found.address, found.mnemonic, found.op_str)) if found else "(not an instruction operand / data)"))
