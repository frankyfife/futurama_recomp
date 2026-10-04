"""Reviewer helper: print the argument pushes of every call site of selected imports
(last <args> 'push' instructions before the call inside a resynchronised linear window that does
not cross another call/ret/jmp). Argument 1 = last push. Pushes of callee-saved registers that are
popped again around the call are NOT filtered - read the output with that in mind.
Usage: python verify_kernel_args.py <slot> [<slot> ...]
"""
import csv
import json
import os
import sys

from verify_kernel_common import LOADER, XBE, md

HERE = os.path.dirname(os.path.abspath(__file__))
v = json.load(open(os.path.join(HERE, "verify_kernel_sites.json")))
TSV = {int(r["slot"]): r for r in csv.DictReader(open(os.path.join(LOADER, "kernel_imports.tsv"), encoding="utf-8"), delimiter="\t")}
EXTRA = {1: [0x292d4c], 3: [0xf792a, 0xf9bc0], 7: [0x291a65], 26: [0xf7b3f], 41: [0xf9278, 0xf950c],
         93: [0x240735, 0x240797], 104: [0x24ae48]}


def window(site):
    for d in range(0x60, 0, -1):
        for k in range(16):
            st = site - d - k
            ins = list(md.disasm(XBE.read(st, site - st + 16), st))
            idx = [n for n, i in enumerate(ins) if i.address == site]
            if idx:
                return ins[:idx[0]]
    return []


for s in map(int, sys.argv[1:]):
    n = int(TSV[s]["purge_bytes"] or 0) // 4
    l = [int(a, 16) for k, a in v["sites"].get(str(s), [])] + EXTRA.get(s, [])
    print("==== slot %d %s (%d args) sites %d" % (s, TSV[s]["name"], n, len(l)))
    for a in sorted(l):
        pushes = []
        for i in reversed(window(a)):
            if i.mnemonic in ("call", "ret", "jmp") or i.mnemonic.startswith("ret"):
                break
            if i.mnemonic == "push":
                pushes.append(i.op_str)
        print("  %06x  args: %s" % (a, " | ".join(pushes[:n]) if n else "-"))
