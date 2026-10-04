"""Raw scan: every reference to a kernel thunk slot (0x293560..) from code sections.
Prints per slot: ordinal, ghidra-xbe name(s) seen at the same caller functions in callgraph.tsv,
number of refs, kind (call/push/mov), owning function. Answers key facts 1, 5, 17 and table 2.3."""
from verify_files_r2_common import *

TT = thunk_table()
print("thunk entries:", len(TT), "first", hex(min(TT)), "last", hex(max(TT)))
ords = sorted(TT.values())
print("ordinal 29 (ExSaveNonVolatileSetting per XDK numbering) present:", 29 in ords)

# decode referencing instructions
refs = collections.defaultdict(list)
for slot in TT:
    for sec, va in raw_dword_refs(slot, CODE_SECTIONS):
        ins = None
        for back in (2, 1, 3, 4):
            try:
                cand = list(md.disasm(X.read(va - back, 16), va - back, count=1))
            except Exception:
                cand = []
            if cand and cand[0].address + cand[0].size >= va + 4 and ("0x%x" % slot) in cand[0].op_str:
                ins = cand[0]
                break
        refs[slot].append((va, ins))

# ghidra names per caller function from callgraph
cg_names = collections.defaultdict(set)
for p in callgraph():
    if p[3].startswith("xboxkrnl.exe::"):
        cg_names[int(p[0], 16)].add(p[3].split("::")[1])

FILE_ORDS = {190, 202, 219, 236, 211, 226, 218, 207, 210, 198, 187, 196, 200, 67, 203, 215, 232, 233, 234}
want = set(int(a, 16) for a in sys.argv[1:]) if len(sys.argv) > 1 else None
for slot in sorted(TT):
    o = TT[slot]
    if want and slot not in want and o not in want:
        continue
    lst = refs[slot]
    kinds = collections.Counter(i.mnemonic if i else "?" for _, i in lst)
    owners = collections.Counter()
    gnames = collections.Counter()
    for va, i in lst:
        f = owner(va)
        owners[(f[0], f[2]) if f else (0, "?")] += 1
        if f and i is not None and i.mnemonic == "call":
            for n in cg_names.get(f[0], ()):
                gnames[n] += 1
    print("slot %06x ord %3d refs %2d kinds %s" % (slot, o, len(lst), dict(kinds)))
    if o in FILE_ORDS or want:
        for (fa, fn), c in sorted(owners.items()):
            print("      owner %08x %-45s x%d" % (fa, fn, c))
        print("      ghidra names at calling funcs:", dict(gnames.most_common(4)))
