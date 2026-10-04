"""verify_cpu_gaptriage.py - triage of privileged/IO pseudo-instructions that a linear sweep finds in the
.text/D3D/DSOUND/XMV/XPP byte ranges NOT covered by a Ghidra function body (key fact 10).
For every hit: is the containing gap a pointer table (dwords pointing into code), a small-byte index
table, padding, or code-like?  Code-like candidates get a disassembly listing for manual review.
Also: does any DIRECT branch/call from a Ghidra body land in a gap that contains such a hit?"""
import sys, struct, collections, bisect
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *
from verify_cpu_classify import special
from capstone import CS_OP_IMM

HARD = ("in", "out", "insb", "insd", "insw", "outsb", "outsd", "outsw", "cli", "sti", "hlt", "wbinvd", "invd",
        "invlpg", "rdmsr", "wrmsr", "lidt", "lgdt", "lldt", "ltr", "clts", "sysenter", "syscall", "sysret",
        "iretd", "mov crN/drN", "lmsw", "rdpmc")


def is_hard(k):
    return k in HARD or (k.startswith("int ") and k not in ("int 3",))


cover = sorted((s, s + sz) for (s, sz, nm) in FUNCS if X.section(s) in CODE and sz > 0)
merged = []
for a, b in cover:
    if merged and a <= merged[-1][1]:
        merged[-1][1] = max(merged[-1][1], b)
    else:
        merged.append([a, b])
gaps = []
for (n, lo, hi, raw) in sec_ranges():
    cur = lo
    for a, b in merged:
        if b <= lo or a >= hi:
            continue
        if a > cur:
            gaps.append((n, cur, a))
        cur = max(cur, b)
    if cur < hi:
        gaps.append((n, cur, hi))

code_lo = 0x11000
code_hi = 0x293560


def classify_gap(a, b):
    d = X.read(a, b - a)
    n = len(d)
    if n == 0:
        return "empty"
    pad = sum(1 for x in d if x in (0xcc, 0x90, 0x00))
    if pad == n:
        return "pad"
    # pointer table: aligned dwords inside [a,b)
    a4 = (a + 3) & ~3
    dw = [struct.unpack_from("<I", d, k - a)[0] for k in range(a4, b - 3, 4)]
    if dw:
        ptr = sum(1 for v in dw if code_lo <= v < code_hi)
        if ptr >= 0.6 * len(dw):
            return "ptrtable(%d/%d)" % (ptr, len(dw))
    small = sum(1 for x in d if x < 0x40)
    if n >= 8 and small >= 0.9 * n:
        return "bytetable"
    asc = sum(1 for x in d if 0x20 <= x < 0x7f or x in (0, 9, 10, 13))
    if n >= 16 and asc >= 0.9 * n:
        return "ascii"
    return "other"


# direct branch targets from Ghidra bodies
btargets = set()
for (s, sz, nm) in FUNCS:
    if X.section(s) not in CODE or sz <= 0:
        continue
    for i in md.disasm(X.read(s, sz), s):
        if (i.group(1) or i.group(2) or i.group(7)) and i.operands and i.operands[0].type == CS_OP_IMM:
            btargets.add(i.operands[0].imm & 0xFFFFFFFF)

stats = collections.Counter()
review = []
for (n, a, b) in gaps:
    d = X.read(a, b - a)
    hits = []
    p = 0
    while p < len(d):
        got = False
        for i in mdn.disasm(d[p:], a + p):
            got = True
            k = special(i)
            if k and is_hard(k):
                hits.append((i.address, i.bytes.hex(), i.mnemonic + " " + i.op_str, k))
            p = i.address + i.size - a
        if not got or p < len(d):
            p += 1
    if not hits:
        continue
    cls = classify_gap(a, b)
    br = sorted(t for t in btargets if a <= t < b)
    stats[(n, cls.split("(")[0], bool(br))] += len(hits)
    if not cls.startswith(("ptrtable", "bytetable", "ascii", "pad")) or br:
        review.append((n, a, b, cls, br, hits))

print("hard pseudo-instruction hits in gaps by (section, gap class, gap is direct-branch target):")
for k, v in sorted(stats.items()):
    print("   %-7s %-12s branchTarget=%-5s %d" % (k[0], k[1], k[2], v))
print("\ngaps needing review (class 'other' or reached by a direct branch):", len(review))
for (n, a, b, cls, br, hits) in review:
    o = owner(a)
    print("-- %s %08x-%08x size=%d cls=%s after=%s branchTargets=%s" % (n, a, b, b - a, cls, o[2] if o else "-",
                                                                        [hex(x) for x in br[:6]]))
    for h in hits[:6]:
        print("      %08x %-16s %-34s %s" % h)
