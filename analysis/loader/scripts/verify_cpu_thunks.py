"""verify_cpu_thunks.py - kernel thunk slots cited in cpu.md: slot -> ordinal straight from the
thunk table (0x293560, ordinal|0x80000000), plus the Ghidra name used at a call site in the
decompilate (evidence for the name) and argument count / cleanup at one call site."""
import sys, re, glob, struct, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *

T = 0x293560
slots = {}
k = 0
while True:
    v = X.u32(T + 4 * k)
    if v == 0:
        break
    slots[T + 4 * k] = v
    k += 1
print("thunk entries until terminator:", len(slots), " all have bit31:", all(v & 0x80000000 for v in slots.values()))

# name evidence: Ghidra decompilate lines '(*(code *)PTR_xxx)' are not used; instead find
# 'call dword ptr [slot]' sites and the Ghidra function name called there via callgraph.tsv
# external callees per caller function (callgraph.tsv rows with EXTERNAL:...)
ext = collections.defaultdict(set)
for l in open(EXP + r"\callgraph.tsv", encoding="utf-8"):
    p = l.rstrip("\n").split("\t")
    if p[2].startswith("EXTERNAL"):
        ext[int(p[0], 16)].add(p[3].split("::")[-1])
callee_names = collections.defaultdict(collections.Counter)

cited = {
    0x2935a0: "RtlRaiseException 302", 0x293694: "PsCreateSystemThreadEx 255", 0x293690: "PsTerminateSystemThread 258",
    0x29370c: "MmCreateKernelStack 169", 0x293708: "KeBugCheck? (_getptd)", 0x29363c: "HalReturnToFirmware? (0xf82d2)",
}
# find all 'call dword ptr [slot]' / 'jmp dword ptr [slot]' in code sections by byte pattern
sites = collections.defaultdict(list)
for (n, lo, hi, raw) in sec_ranges():
    data = X.b[raw:raw + (hi - lo)]
    for pat in (b"\xff\x15", b"\xff\x25"):
        p = data.find(pat)
        while p >= 0:
            tgt = struct.unpack_from("<I", data, p + 2)[0] if p + 6 <= len(data) else 0
            if tgt in slots:
                sites[tgt].append(lo + p)
            p = data.find(pat, p + 1)
# also 'mov reg,[slot]' (8b 05/0d/15/1d/35/3d + a1)
for (n, lo, hi, raw) in sec_ranges():
    data = X.b[raw:raw + (hi - lo)]
    p = data.find(b"\xa1")
    while p >= 0:
        if p + 5 <= len(data):
            tgt = struct.unpack_from("<I", data, p + 1)[0]
            if tgt in slots:
                sites[tgt].append(lo + p)
        p = data.find(b"\xa1", p + 1)
    for m in (0x05, 0x0d, 0x15, 0x1d, 0x35, 0x3d):
        pat = bytes([0x8b, m])
        p = data.find(pat)
        while p >= 0:
            if p + 6 <= len(data):
                tgt = struct.unpack_from("<I", data, p + 2)[0]
                if tgt in slots:
                    sites[tgt].append(lo + p)
            p = data.find(pat, p + 1)

want = [0x2935a0, 0x293694, 0x293690, 0x29370c, 0x293708, 0x29363c]
# slots by ordinal for the ordinals cited in cpu.md
ordw = {302, 255, 258, 169, 170, 160, 161, 129, 142, 139, 324, 322, 179, 182, 327, 328, 46, 312, 44, 109, 98, 95, 49, 1, 2}
for s, v in slots.items():
    if (v & 0x7fffffff) in ordw and s not in want:
        want.append(s)
for s in sorted(want):
    v = slots[s]
    o = owner(sites[s][0]) if sites[s] else None
    # intersect external callee names of all owning Ghidra functions of the sites
    inter = None
    for a in sites[s]:
        ow = owner(a)
        if ow and ow[3] and ext.get(ow[0]):
            inter = set(ext[ow[0]]) if inter is None else inter & ext[ow[0]]
    print("slot %08x ord %3d  sites=%3d  name(intersection of ext. callees)=%s  first-site=%s %s" % (
        s, v & 0x7fffffff, len(sites[s]), sorted(inter) if inter is not None else None,
        hex(sites[s][0]) if sites[s] else "-", o[2] if o else ""))

print("\n== RtlUnwind stub 0x170aae and its callers")
show(0x170a9f, 0x170ac0)
for (n, lo, hi, raw) in sec_ranges():
    data = X.b[raw:raw + (hi - lo)]
    for p in range(len(data) - 5):
        if data[p] in (0xe8, 0xe9):
            t = (lo + p + 5 + struct.unpack_from("<i", data, p + 1)[0]) & 0xffffffff
            if t == 0x170aae:
                o = owner(lo + p)
                print("  caller %08x %s" % (lo + p, o[2] if o else "-"))
                show(lo + p - 12, lo + p + 5)

print("\n== RaiseException 0xf98ed context")
show(0xf98c0, 0xf9930)

