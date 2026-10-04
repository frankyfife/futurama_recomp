"""verify_cpu_priv.py - independent re-check of privileged / special instructions (cpu.md key
facts 10, 14) by a linear sweep of every Ghidra function body (functions.tsv) plus the
non-Ghidra code regions the report names (main thread 0xf8469, XapiThreadStartup 0xf99da,
_freeptd 0x169352, fiber start 0x17086b, 0xf6384 tail of FUN_000f5c77).
Compares with cpu_priv_sites.tsv.
"""
import sys, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *

PRIV = {"in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd", "cli", "sti", "hlt",
        "lidt", "lgdt", "lldt", "ltr", "sidt", "sgdt", "sldt", "str", "rdmsr", "wrmsr", "wbinvd",
        "invd", "invlpg", "sysenter", "sysexit", "syscall", "sysret", "iret", "iretd", "clts",
        "lmsw", "smsw", "rdpmc", "int", "into", "int1", "rdtsc", "cpuid", "pushfd", "popfd",
        "pushal", "popal", "lds", "les", "lfs", "lgs", "lss", "ljmp", "lcall", "arpl", "lar",
        "lsl", "verr", "verw", "fxsave", "fxrstor", "ldmxcsr", "stmxcsr", "fldcw", "fnstcw",
        "fninit", "fnclex", "fldenv", "fnstenv", "frstor", "fnsave", "cmpxchg", "cmpxchg8b",
        "xadd", "femms", "emms", "movntq", "movntps", "prefetchnta", "sfence", "ud2"}

extra_regions = [(0xf8469, 0xf84dd), (0xf99da, 0xf9a72), (0xf6380, 0xf63ad), (0x169352, 0x169467),
                 (0x17086b, 0x1708b0), (0x1d880, 0x1d8a0)]
hits = {}
lock = []
mn_sec = collections.Counter()
xmm = collections.Counter()


def scan(a, b, src):
    for i in mdn.disasm(X.read(a, b - a), a):
        m = i.mnemonic
        if m.startswith("lock"):
            lock.append((i.address, i.bytes.hex(), m, i.op_str, src))
        if m.startswith("rep ") and ("ins" in m or "outs" in m):
            hits[i.address] = (i.bytes.hex(), m, i.op_str, src)
        if m in PRIV or (m == "mov" and any(r in i.op_str for r in ("cr0", "cr2", "cr3", "cr4", "dr0", "dr1", "dr2", "dr3", "dr6", "dr7"))) \
                or (m in ("mov", "push", "pop") and i.op_str.split(",")[0].strip() in ("ds", "es", "fs", "gs", "ss", "cs")) \
                or (m == "mov" and i.op_str.split(",")[-1].strip() in ("ds", "es", "fs", "gs", "ss", "cs")):
            hits[i.address] = (i.bytes.hex(), m, i.op_str, src)
        if "xmm" in i.op_str:
            xmm[(X.section(i.address), m)] += 1


for (s, sz, nm) in FUNCS:
    if X.section(s) in CODE and sz > 0:
        scan(s, s + sz, nm)
for a, b in extra_regions:
    scan(a, b, "extra@%x" % a)

rep = {}
for l in open(LOADER + r"\cpu_priv_sites.tsv", encoding="utf-8"):
    p = l.rstrip("\n").split("\t")
    if p[0] == "addr":
        hdr = p
        continue
    rep[int(p[0], 16)] = dict(zip(hdr, p))

agg = collections.Counter((X.section(a), v[1]) for a, v in hits.items())
print("Ghidra-body sweep, special instructions by (section, mnemonic):")
for k, n in sorted(agg.items()):
    print("   %-7s %-12s %d" % (k[0], k[1], n))
print("\nprivileged (in/out/cli/sti/hlt/wbinvd/int!=3/cr/dr/msr/...) hits:")
TRULY = {"in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd", "cli", "sti", "hlt", "lidt",
         "lgdt", "lldt", "ltr", "rdmsr", "wrmsr", "wbinvd", "invd", "invlpg", "sysenter", "sysexit",
         "iret", "iretd", "clts", "lmsw", "int", "into", "int1", "rdpmc"}
for a in sorted(hits):
    v = hits[a]
    if v[1] in TRULY or v[1].startswith("rep") or v[1] == "mov":
        print("   %08x %-10s %-8s %-14s %-40s inReport=%s" % (a, v[0], v[1], v[2], v[3][:40], a in rep))
print("\nrdtsc / cpuid / pushfd / popfd / fldcw / ldmxcsr hits:")
for a in sorted(hits):
    v = hits[a]
    if v[1] in ("rdtsc", "cpuid", "pushfd", "popfd", "ldmxcsr", "stmxcsr", "fldcw", "fninit", "cmpxchg", "cmpxchg8b", "xadd"):
        print("   %08x %-10s %-8s %-30s %-40s inReport=%s" % (a, v[0], v[1], v[2], v[3][:40], a in rep))
print("\nlock-prefixed instructions in Ghidra bodies:", len(lock))
for x in lock[:20]:
    print("   %08x %s %s %s %s" % x)
print("\nxmm-register instructions by (section, mnemonic):")
for k, n in sorted(xmm.items()):
    print("   %-7s %-12s %d" % (k[0], k[1], n))
fldcw_by_owner = collections.Counter(v[3] for a, v in hits.items() if v[1] == "fldcw")
print("\nfldcw by owning function (top 15):", fldcw_by_owner.most_common(15))
print("total fldcw:", sum(fldcw_by_owner.values()))
