"""cpu_kthunks.py - map kernel thunk slots (0x293560 + 4*i) to the import names
Ghidra (ghidra-xbe loader) shows in the exported decompilation.

No ordinal table from memory is used: for every function, the set of thunk slots
referenced by its instructions (call/jmp/mov [slot]) is matched against the set of
kernel import names appearing in that function's decompiled C. Unique matches are
propagated iteratively (constraint elimination).
Output: prints  slot_va  index  ordinal  name  evidence_function
usage: python cpu_kthunks.py [--tsv]
"""
import sys, re, glob, collections, bisect, struct
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from cpu_rd import RD

DECOMP = r"D:\FuturamaDecomp\analysis\retail_eu_export\decomp\*.c"
THUNK = 0x293560


def load(rd=None):
    rd = rd or RD().run()
    x = rd.x
    slots = {}
    i = 0
    while True:
        v = x.u32(THUNK + 4 * i)
        if v == 0:
            break
        slots[THUNK + 4 * i] = v & 0x7FFFFFFF
        i += 1
    # decomp text per function
    ftext = {}
    for fn in glob.glob(DECOMP):
        cur = None
        buf = []
        for line in open(fn, encoding="utf-8", errors="replace"):
            m = re.match(r"// ==== FUNC ([0-9a-f]+) (\S+) size=(\d+)", line)
            if m:
                if cur is not None:
                    ftext[cur] = "".join(buf)
                cur = int(m.group(1), 16)
                buf = []
            else:
                buf.append(line)
        if cur is not None:
            ftext[cur] = "".join(buf)
    funcs = rd.funcs
    fst = [f[0] for f in funcs]
    # slots referenced per function
    refs = collections.defaultdict(set)
    for va, (sz, mn, ops, bb) in rd.insn.items():
        for m in re.finditer(r"\[0x([0-9a-f]+)\]", ops):
            t = int(m.group(1), 16)
            if t in slots:
                k = bisect.bisect_right(fst, va) - 1
                if k >= 0:
                    refs[funcs[k][0]].add(t)
    # candidate names = identifiers in decomp that look like kernel exports
    namepat = re.compile(r"\b((?:Av|Dbg|Ex|Fsc|Hal|Io|Iof|Ke|Kf|Ki|Mm|Nt|Ob|Ps|Rtl|Xc|Xe|Xbox|Launch|Idex|Phy|Hal)[A-Z][A-Za-z0-9]*(?:_exref)?)\b")
    cands = {}
    for f, s in refs.items():
        names = set(n.replace("_exref", "") for n in namepat.findall(ftext.get(f, "")))
        cands[f] = (set(s), names)
    known = {}
    evid = {}
    # direct: Ghidra thunk functions "xboxkrnl.exe::Name" (jmp [slot])
    for (s, sz, nm) in funcs:
        if nm.startswith("xboxkrnl.exe::"):
            for t in refs.get(s, ()):
                known[t] = nm.split("::")[1]
                evid[t] = "%08x(thunkfunc)" % s
    changed = True
    while changed:
        changed = False
        for f, (ss, ns) in cands.items():
            rs = set(t for t in ss if t not in known)
            rn = set(n for n in ns if n not in known.values())
            if len(rs) == 1 and len(rn) == 1:
                t = rs.pop()
                known[t] = rn.pop()
                evid[t] = "%08x" % f
                changed = True
    return slots, known, evid


if __name__ == "__main__":
    slots, known, evid = load()
    for t in sorted(slots):
        print("%08x\t%d\t%d\t%s\t%s" % (t, (t - THUNK) // 4, slots[t], known.get(t, "?"), evid.get(t, "")))
    print("resolved %d / %d" % (len(known), len(slots)), file=sys.stderr)
