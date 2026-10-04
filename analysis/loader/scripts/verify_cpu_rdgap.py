"""verify_cpu_rdgap.py - own, independent reachability check for code that lies OUTSIDE Ghidra function
bodies (key facts 1/10/12: "real code" also includes gap code such as 0xf8469 or 0xf99da).

Seeds (only addresses inside gaps = code-section bytes not covered by any Ghidra body):
  S1 direct call/jmp/jcc targets found while sweeping Ghidra bodies
  S2 immediates of push/mov/lea in Ghidra bodies
  S3 every 4-byte-aligned dword in .rdata/.data/D3D/DSOUND/XMV/XPP/.text whose value lies in a gap
  S4 entry point 0xf84dd
Recursive descent from the seeds (follows direct branches; stops at ret/jmp/int3/undecodable; does
not continue into Ghidra bodies, those are already covered by the body sweep). Every reached
instruction is checked for fs:/gs:, privileged/special opcodes, absolute MMIO operands.
Output: per seed class counts and every special hit with the seed chain."""
import sys, struct, collections, bisect
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *
from capstone import CS_OP_MEM, CS_OP_IMM
from capstone.x86 import X86_REG_FS, X86_REG_GS, X86_REG_INVALID
sys.setrecursionlimit(10000)
from verify_cpu_classify import special

# body coverage
cover = sorted((s, s + sz) for (s, sz, nm) in FUNCS if X.section(s) in CODE and sz > 0)
cs = [a for a, b in cover]


def in_body(v):
    i = bisect.bisect_right(cs, v) - 1
    return i >= 0 and cover[i][0] <= v < cover[i][1]


def is_code(v):
    n = X.section(v)
    return n in CODE and X.va2off(v) is not None


seeds = collections.defaultdict(set)
for (s, sz, nm) in FUNCS:
    if X.section(s) not in CODE or sz <= 0:
        continue
    for i in md.disasm(X.read(s, sz), s):
        for op in i.operands:
            if op.type == CS_OP_IMM:
                v = op.imm & 0xFFFFFFFF
                if is_code(v) and not in_body(v):
                    seeds["S1-branch" if i.group(1) or i.group(2) or i.group(7) else "S2-imm"].add((v, i.address))
for (n, va, vsz, raw, rsz) in X.sections:
    if n in (".rdata", ".data") + CODE:
        d = X.b[raw:raw + rsz]
        for k in range(0, len(d) - 3, 4):
            v = struct.unpack_from("<I", d, k)[0]
            if is_code(v) and not in_body(v) and not (n in CODE and not in_body(va + k)):
                seeds["S3-dword"].add((v, va + k))
seeds["S4-entry"].add((0xf84dd, 0))
for k, v in seeds.items():
    print("seed class %-10s %6d (distinct targets %d)" % (k, len(v), len(set(t for t, _ in v))))

reached = {}
special_hits = []
fs_hits = []
mmio_hits = []


def walk(start, why):
    work = [start]
    while work:
        a = work.pop()
        while True:
            if a in reached or not is_code(a) or in_body(a):
                break
            b = X.read(a, 16)
            ins = list(md.disasm(b, a, 1))
            if not ins:
                break
            i = ins[0]
            reached[a] = why
            k = special(i)
            if k:
                special_hits.append((a, i.bytes.hex(), i.mnemonic + " " + i.op_str, k, why))
            for op in i.operands:
                if op.type == CS_OP_MEM and op.mem.segment in (X86_REG_FS, X86_REG_GS):
                    fs_hits.append((a, i.bytes.hex(), i.mnemonic + " " + i.op_str, why))
                if op.type == CS_OP_MEM and op.mem.base == X86_REG_INVALID:
                    d = op.mem.disp & 0xFFFFFFFF
                    if 0xFD000000 <= d < 0xFF000000 or 0x80000000 <= d < 0x84000000:
                        mmio_hits.append((a, i.mnemonic + " " + i.op_str, why))
            m = i.mnemonic
            if m in ("ret", "retf", "int3", "hlt", "ud2", "iretd"):
                break
            if (i.group(1) or i.group(2) or i.group(7)) and i.operands and i.operands[0].type == CS_OP_IMM:
                work.append(i.operands[0].imm & 0xFFFFFFFF)
                if m == "jmp":
                    break
            elif m == "jmp":
                break
            a += i.size


for cls in ("S4-entry", "S1-branch", "S2-imm", "S3-dword"):
    before = len(reached)
    for (t, src) in sorted(seeds[cls]):
        walk(t, "%s<-%08x" % (cls, src))
    print("after %-10s reached gap instructions: %d (+%d)" % (cls, len(reached), len(reached) - before))

print("\nfs/gs in reached gap code:", len(fs_hits))
offs = collections.Counter()
for h in fs_hits:
    offs[h[2].split("[")[-1]] += 1
print("  by operand tail:", dict(offs.most_common(12)))
for h in fs_hits:
    if "fs:[0]" not in h[2]:
        print("   %08x %-18s %-40s %s" % h)
print("\nspecial instructions in reached gap code (int3 excluded):")
for h in special_hits:
    if h[3] == "int 3":
        continue
    o = owner(h[0])
    print("   %08x %-14s %-36s %-14s %s  after=%s" % (h[0], h[1], h[2], h[3], h[4], o[2] if o else "-"))
print("\nabsolute MMIO / phys-window operands in reached gap code:")
for h in mmio_hits:
    print("   %08x %-40s %s" % h)

