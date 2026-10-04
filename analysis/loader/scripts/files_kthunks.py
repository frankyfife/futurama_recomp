"""File-system related xboxkrnl imports of retail_eu default.xbe.
For each thunk slot (table at 0x293560): raw ordinal from the XBE, the name Ghidra (ghidra-xbe) attached
(taken from callgraph.tsv callee names 'xboxkrnl.exe::X' resolved to the slot by disassembling each caller),
number of direct call sites 'call dword ptr [slot]', and the push count before each call (linear backward scan
inside the basic block; stdcall kernel APIs clean their own args, so pushes == argument count)."""
import collections, struct, sys
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
EXP = r"D:\FuturamaDecomp\analysis\retail_eu_export"
WANT = ("Nt", "Io", "Ob", "Rtl", "Ex", "Xc", "Xbox", "Hal", "Launch", "Xe", "Mm")
slots = []
va = 0x293560
while True:
    raw = X.u32(va)
    if raw == 0: break
    slots.append((va, raw & 0x7fffffff)); va += 4
funcs = []
for line in open(EXP + r"\functions.tsv", encoding="utf-8"):
    p = line.rstrip("\n").split("\t")
    if p[0] != "address": funcs.append((int(p[0], 16), int(p[1]), p[3]))
md = Cs(CS_ARCH_X86, CS_MODE_32)
text = [s for s in X.sections if s[0] in (".text", "D3D", "DSOUND", "XMV", "XPP")]
sites = collections.defaultdict(list)
for name, sva, vsz, raw, rsz in text:
    blob = X.b[raw:raw + rsz]
    for slot, _ in slots:
        needle = b"\xff\x15" + struct.pack("<I", slot)
        i = blob.find(needle)
        while i >= 0:
            sites[slot].append(sva + i); i = blob.find(needle, i + 1)
# Ghidra names: callee names in callgraph that are kernel imports, matched via caller functions
gname = {}
cg = collections.defaultdict(set)
for line in open(EXP + r"\callgraph.tsv", encoding="utf-8"):
    p = line.rstrip("\n").split("\t")
    if p[3].startswith("xboxkrnl.exe::"): cg[int(p[0], 16)].add(p[3][14:])
fstart = sorted(funcs)
import bisect
starts = [f[0] for f in fstart]
def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return fstart[i][0] if i >= 0 else None
def pushes_before(site):
    f = owner(site)
    best = None
    for back in range(1, 160):
        s = site - back
        if f is not None and s < f: break
        ins = list(md.disasm(X.read(s, back), s))
        if ins and sum(x.size for x in ins) == back and ins[-1].address + ins[-1].size == site:
            n = 0
            for x in reversed(ins):
                if x.mnemonic in ("call", "ret", "jmp") or x.mnemonic.startswith("j"): break
                if x.mnemonic == "push": n += 1
                if x.mnemonic == "pop": n -= 1
            best = n
    return best
for slot, ordn in slots:
    cands = collections.Counter()
    for s in sites[slot]:
        for n in cg.get(owner(s), ()): cands[n] += 1
    gname[slot] = cands
rows = []
for slot, ordn in slots:
    c = gname[slot]
    # pick the name that appears in all callers of this slot
    nm = "?"
    if sites[slot]:
        allowners = [owner(s) for s in sites[slot]]
        common = set.intersection(*[cg.get(o, set()) for o in allowners]) if allowners else set()
        if len(common) == 1: nm = common.pop()
        elif common: nm = "/".join(sorted(common))
    rows.append((slot, ordn, nm))
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from kernel_common import XBOXKRNL_EXPORTS  # candidate ordinal->name table (other agent), cross-checked below
allrefs = collections.Counter()
for name, sva, vsz, raw, rsz in X.sections:
    blob = X.b[raw:raw + rsz]
    for slot, _ in slots:
        allrefs[slot] += blob.count(struct.pack("<I", slot))
print("slot      ord  candidate-name (kernel_common)       ghidra-agrees  call[slot]-sites  all-refs  pushes")
for slot, ordn in slots:
    cand = XBOXKRNL_EXPORTS.get(ordn, "?")
    if not any(cand.startswith(w) for w in WANT): continue
    owners = [owner(x) for x in sites[slot]]
    agree = any(cand in cg.get(o, ()) for o in owners) if owners else None
    pc = dict(collections.Counter(pushes_before(x) for x in sites[slot]))
    print(f"{slot:08x}  {ordn:3d}  {cand:36s} {str(agree):13s} {len(sites[slot]):3d}  {allrefs[slot]:3d}  {pc}")
