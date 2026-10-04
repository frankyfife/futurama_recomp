"""verify_cpu_fs.py - independent re-check of the fs:/gs: site list (cpu.md key facts 1,2,6,7).

Method (independent of cpu_rd/cpu_scan):
  1. byte scan: every 0x64/0x65 byte in the raw ranges of .text/D3D/DSOUND/XMV/XPP;
     decode an instruction starting at that byte or up to 3 legacy-prefix bytes earlier.
  2. reachability cross-check: linear sweep of every Ghidra function body
     [start, start+size) from functions.tsv -> set of instruction starts.
  3. compare with fs_sites.tsv (the reviewed result).
"""
import sys, collections
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *
from capstone import CS_OP_MEM
from capstone.x86 import X86_REG_FS, X86_REG_GS, X86_REG_INVALID

PFX = {0x66, 0x67, 0xF2, 0xF3, 0xF0, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65}

# ---- 1. byte scan
cands = {}   # start -> (size, bytes, text, seg, disp, based)
for (n, lo, hi, raw) in sec_ranges():
    data = X.b[raw:raw + (hi - lo)]
    for p in range(len(data)):
        if data[p] not in (0x64, 0x65):
            continue
        for k in range(0, 4):
            q = p - k
            if q < 0:
                break
            if k and data[q] not in PFX:
                break
            ins = list(md.disasm(data[q:q + 16], lo + q, 1))
            if not ins:
                continue
            i = ins[0]
            if i.size <= k:
                continue
            seg = None
            for op in i.operands:
                if op.type == CS_OP_MEM and op.mem.segment in (X86_REG_FS, X86_REG_GS):
                    seg = op
            if seg is None and not (0x64 in i.prefix or 0x65 in i.prefix):
                continue
            if seg is not None:
                sname = "fs" if seg.mem.segment == X86_REG_FS else "gs"
                disp = seg.mem.disp & 0xFFFFFFFF
                based = seg.mem.base != X86_REG_INVALID or seg.mem.index != X86_REG_INVALID
            else:
                sname, disp, based = ("fs" if 0x64 in i.prefix else "gs"), None, None
            cands[lo + q] = (i.size, i.bytes.hex(), "%s %s" % (i.mnemonic, i.op_str), sname, disp, based, n)

print("byte-scan candidates (any decode containing fs/gs):", len(cands))

# ---- 2. Ghidra-body linear sweep
gstarts = set()
for (s, sz, nm) in FUNCS:
    if X.section(s) not in CODE or sz <= 0:
        continue
    b = X.read(s, sz)
    if not b:
        continue
    for i in mdn.disasm(b, s):
        gstarts.add(i.address)
print("Ghidra-body linear-sweep instruction starts:", len(gstarts))

# ---- 3. reviewed list
rep = {}
for l in open(LOADER + r"\fs_sites.tsv", encoding="utf-8"):
    p = l.rstrip("\n").split("\t")
    if p[0] == "addr":
        hdr = p
        continue
    rep[int(p[0], 16)] = dict(zip(hdr, p))
print("fs_sites.tsv rows:", len(rep))

missing_in_scan = [a for a in rep if a not in cands]
print("report sites not reproduced by byte scan:", len(missing_in_scan), [hex(a) for a in missing_in_scan[:10]])
bad = [a for a in rep if a in cands and cands[a][1] != rep[a]["bytes"]]
print("report sites with differing bytes:", len(bad))

G = {a for a in cands if a in gstarts}
A = set(rep)
print("byte-scan fs sites on Ghidra-body instruction boundaries:", len(G))
print("  of those in report:", len(G & A))
print("  in Ghidra bodies but NOT in report:", len(G - A))
for a in sorted(G - A):
    c = cands[a]
    o = owner(a)
    print("    %08x %-18s %-40s %s" % (a, c[1], c[2], o[2] if o else "-"))
print("report sites not on a Ghidra-body boundary:", len(A - G))

# offset statistics of the report list (recomputed from bytes, not from the TSV column)
cnt = collections.Counter()
enc = collections.Counter()
based_cnt = 0
for a in A:
    c = cands[a]
    cnt[(c[3], c[4])] += 1
    if c[5]:
        based_cnt += 1
    if c[4] == 0:
        b = bytes.fromhex(c[1])
        enc[b[:-4].hex() if b[-4:] == b"\0\0\0\0" else b.hex()] += 1
print("report sites by (seg, disp):", sorted(((k[0], hex(k[1]) if k[1] is not None else None), v) for k, v in cnt.items()))
print("report sites with base/index register:", based_cnt)
print("fs:[0] encodings (opcode part):", enc.most_common(12))

nz = sorted(a for a in A if cands[a][4] != 0)
print("non-zero sites:", len(nz))
print("non-zero sites in Ghidra bodies (boundary):", len([a for a in nz if a in G]))
print("non-zero sites NOT on Ghidra boundary:", [hex(a) for a in nz if a not in G])

# every absolute fs/gs candidate with disp != 0 anywhere in the code sections, not in report
print("\nbyte-scan absolute fs/gs disp!=0 NOT in report (all sections):")
for a in sorted(cands):
    c = cands[a]
    if a in A or c[5] or c[4] in (0, None):
        continue
    o = owner(a)
    print("  %08x %-7s %-18s %-44s owner=%s inside=%s ghidraBoundary=%s" % (
        a, c[6], c[1], c[2], o[2] if o else "-", o[3] if o else "-", a in gstarts))
print("\nbyte-scan BASED fs/gs (reg-relative) on Ghidra boundaries:")
for a in sorted(cands):
    c = cands[a]
    if c[5] and a in gstarts:
        print("  %08x %-18s %s" % (a, c[1], c[2]))
gs = [a for a in cands if cands[a][3] == "gs" and a in gstarts]
print("gs: on Ghidra boundaries:", [hex(a) for a in gs])

# fs:[0] functions / libraries (owner by Ghidra function)
funcs0 = set()
for a in A:
    if cands[a][4] == 0:
        o = owner(a)
        funcs0.add(o[0] if o and o[3] else ("gap", a))
print("distinct owning Ghidra functions of fs:[0] sites (gap sites counted individually):", len(funcs0))
lib0 = collections.Counter(rep[a]["lib"] for a in A if cands[a][4] == 0)
print("fs:[0] by lib column:", lib0)
sec0 = collections.Counter(X.section(a) for a in A if cands[a][4] == 0)
print("fs:[0] by section:", sec0)

# ---- 4. absolute fs:[0] candidates not in the report and outside every Ghidra body
print("\nabsolute fs:[0] candidates not in report, outside Ghidra bodies:")
n_in = 0
for a in sorted(cands):
    c = cands[a]
    if a in A or c[5] or c[4] != 0:
        continue
    o = owner(a)
    if o and o[3]:
        n_in += 1
        continue
    print("  %08x %-7s %-18s %-40s after=%s" % (a, c[6], c[1], c[2], o[2] if o else "-"))
print("absolute fs:[0] candidates not in report but inside Ghidra bodies (mid-instruction artefacts):", n_in)
