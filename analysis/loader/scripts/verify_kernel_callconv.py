"""Reviewer check: calling convention / argument count per kernel import, from a simple
per-site heuristic that is independent of the original stack solver:
  pushes = #push between the previous call/ret/jmp (linear, inside the containing function or a
           0x200-byte window before the site) and the call;  pops are subtracted
  cleanup = N of an 'add esp,N' directly after the call (or after one intervening mov/test)
  ecx/edx set = an instruction writing cl/cx/ecx (dl/dx/edx) between the previous call and the call
Site list: verify_kernel_sites.json + the 10 sites that only the CFG walker of the original analysis
finds (reg calls after a branch, 1 ss-prefix ambiguity) - each validated in verify_kernel_sites.py notes.
"""
import csv
import json
import os
import re
from collections import Counter, defaultdict

from verify_kernel_common import LOADER, XBE, Funcs, fmt, insn, md

HERE = os.path.dirname(os.path.abspath(__file__))
v = json.load(open(os.path.join(HERE, "verify_kernel_sites.json")))
sites = defaultdict(list)
for k, l in v["sites"].items():
    for kind, a in l:
        sites[int(k)].append((kind, int(a, 16)))
EXTRA = [(1, 0x292d4c), (3, 0xf792a), (3, 0xf9bc0), (7, 0x291a65), (26, 0xf7b3f), (41, 0xf9278),
         (41, 0xf950c), (93, 0x240735), (93, 0x240797), (104, 0x24ae48)]
for s, a in EXTRA:
    sites[s].append(("call_x", a))
tsv = {int(r["slot"]): r for r in csv.DictReader(open(os.path.join(LOADER, "kernel_imports.tsv"), encoding="utf-8"), delimiter="\t")}
F = Funcs()
print("total sites:", sum(len(l) for l in sites.values()))

WR = {"ecx": re.compile(r"^(ecx|cx|cl|ch)\b"), "edx": re.compile(r"^(edx|dx|dl|dh)\b")}


def linear(start, end):
    out = []
    a = start
    for i in md.disasm(XBE.read(start, end - start + 16), start):
        if i.address > end:
            break
        out.append(i)
    return out


def analyse(site):
    fr = F.containing(site)
    start = fr[0] if fr else site - 0x100
    if site - start > 0x400:
        start = site - 0x400
    ins = linear(start, site)
    # resync: keep only if site is a boundary
    idx = [k for k, i in enumerate(ins) if i.address == site]
    if not idx:
        # try other start offsets
        for d in range(1, 16):
            ins = linear(start + d, site)
            idx = [k for k, i in enumerate(ins) if i.address == site]
            if idx:
                break
    if not idx:
        return None
    k = idx[0]
    pushes = 0
    ecx = edx = False
    j = k - 1
    while j >= 0:
        i = ins[j]
        if i.mnemonic in ("call", "ret", "jmp", "int3") or i.mnemonic.startswith("ret"):
            break
        if i.mnemonic == "push":
            pushes += 1
        elif i.mnemonic == "pop":
            pushes -= 1
        elif i.mnemonic in ("pushal", "pushad"):
            pushes += 8
        if i.mnemonic not in ("cmp", "test", "push") and WR["ecx"].match(i.op_str):
            ecx = True
        if i.mnemonic not in ("cmp", "test", "push") and WR["edx"].match(i.op_str):
            edx = True
        j -= 1
    call = ins[k]
    nxt = list(md.disasm(XBE.read(site + call.size, 24), site + call.size))[:2]
    cleanup = 0
    for n in nxt:
        m = re.match(r"esp, (0x[0-9a-f]+|\d+)$", n.op_str)
        if n.mnemonic == "add" and m:
            cleanup = int(m.group(1), 0)
            break
        if n.mnemonic == "pop" and n.op_str == "ecx":
            cleanup = 4
            break
    return pushes, cleanup, ecx, edx, call.mnemonic


res = {}
for s in sorted(sites):
    l = sites[s]
    prof = Counter()
    ecxn = edxn = cl = 0
    tails = 0
    for kind, a in l:
        i = insn(a)
        if i.mnemonic == "jmp":
            tails += 1
            continue
        r = analyse(a)
        if r is None:
            prof["?"] += 1
            continue
        p, c, e, d, m = r
        prof[(p, c)] += 1
        ecxn += e
        edxn += d
        cl += c != 0
    res[s] = prof
    t = tsv[s]
    print("%3d %-30s tsv:%-8s args=%-12s | sites %3d tails %d | (pushes,cleanup): %s | ecx %d edx %d" %
          (s, t["name"], t["convention"], t["args"], len(l), tails, dict(prof.most_common(4)), ecxn, edxn))
