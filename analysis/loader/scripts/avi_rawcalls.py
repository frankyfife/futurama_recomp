"""avi_rawcalls.py - raw byte scan for direct CALL/JMP rel32 (E8/E9) from code sections into
DSOUND / XMV / XPP function starts. Complements callgraph.tsv, which misses calls located in
code Ghidra never turned into functions (e.g. virtual methods only reachable through vtables).
Each hit is validated by disassembling (capstone) the instruction at the hit address and by
checking that a linear disassembly from the containing function / 16-byte aligned start
reaches that address on an instruction boundary (best effort).
Usage: python avi_rawcalls.py [SECTION ...]"""
import sys, csv, struct, bisect, collections
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
import capstone
x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
funcs = {}
with open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv", encoding="utf-8") as f:
    r = csv.reader(f, delimiter="\t"); next(r)
    for row in r: funcs[int(row[0], 16)] = (row[3], row[2], int(row[1]))
starts = sorted(funcs)
def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    if i < 0: return None
    s = starts[i]
    return s if a < s + max(funcs[s][2], 1) else None
targets_secs = sys.argv[1:] or ["DSOUND", "XMV", "XPP"]
code_secs = [s for s in x.sections if s[0] in (".text", "D3D", "DSOUND", "XMV", "XPP")]
def boundary_ok(a):
    # walk back up to 64 bytes and see whether some start decodes onto a
    for back in range(1, 64):
        st = a - back
        ok = False
        for ins in md.disasm(x.read(st, back + 5), st):
            if ins.address == a: ok = True; break
            if ins.address > a: break
        if ok: return True
    return False
hits = collections.defaultdict(list)
for name, va, vsz, raw, rsz in code_secs:
    data = x.b[raw:raw + rsz]
    for i in range(len(data) - 5):
        op = data[i]
        if op not in (0xE8, 0xE9): continue
        src = va + i
        tgt = (src + 5 + struct.unpack_from("<i", data, i + 1)[0]) & 0xffffffff
        if tgt not in funcs: continue
        tsec = funcs[tgt][1]
        if tsec not in targets_secs or tsec == name: continue
        ins = next(md.disasm(data[i:i + 5], src), None)
        if ins is None: continue
        if not boundary_ok(src): continue
        o = owner(src)
        hits[tgt].append((src, ins.mnemonic, o))
for tgt in sorted(hits):
    lst = hits[tgt]
    print("%08x %-50s [%s] sites=%d" % (tgt, funcs[tgt][0], funcs[tgt][1], len(lst)))
    for src, mn, o in lst:
        on = ("%08x %s" % (o, funcs[o][0])) if o else "NO-GHIDRA-FUNCTION"
        print("     %s @%08x in %s" % (mn, src, on))
