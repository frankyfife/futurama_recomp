"""Heuristic scan for per-tick integer counters in the game code (.text 0x11000..0x100000, game part before NI).

Patterns (per function or gap segment, operands compared textually):
  DEC  : `dec dword ptr [M]` | `sub dword ptr [M], 1` | `add dword ptr [M], -1`
         | load r,[M] ... (dec r | lea r2,[r - 1]) ... store [M],r/r2   (within 6 instructions)
  INC  : the same with +1, followed within 8 instructions by `cmp [M]|r, imm` (imm >= 2)
  INIT : for every DEC field M = [base + D]: all `mov dword ptr [x + D], imm` (imm 2..100000) anywhere in the
         game code, grouped (only listed when they live in a function that also uses the same field name family)
Output: timing_tickscan.out next to this script. Filtered: stack slots (esp/ebp), NiObject reference counts
([reg + 4]) and plain [reg] operands. False positives remain (container sizes, ammo, lives); the hits are
triaged by hand in timing60.md 4.9.
Usage: python timing_tickscan.py
"""
import os
import re
from collections import defaultdict

from timing_common import index, owner_name, mem_refs, G_DT

GAME_END = 0x100000  # NetImmerse/XDK code starts near 0x100000 (renderer.md); scripts VM etc. are below
MEM = re.compile(r"(?:dword|word|byte) ptr \[([^\]]+)\]")


def is_stack(m):
    return "esp" in m or "ebp" in m or re.fullmatch(r"e.. \+ 4", m) is not None or re.fullmatch(r"e..", m) is not None


def main():
    idx = [e for e in index() if 0x11000 <= e[0] < GAME_END]
    by_owner = defaultdict(list)
    for e in idx:
        by_owner[e[5]].append(e)
    dt_owners = {e[5] for e in mem_refs(G_DT)}
    dec_hits, inc_hits = [], []
    for o, L in by_owner.items():
        for k, e in enumerate(L):
            mn, op = e[2], e[3]
            m = MEM.search(op)
            # direct memory forms
            if m and not is_stack(m.group(1)):
                if mn == "dec" or (mn in ("sub", "add") and op.endswith(", 1") and mn == "sub") or \
                        (mn == "add" and op.endswith(", -1")) or (mn == "add" and op.endswith(", 0xffffffff")):
                    dec_hits.append((o, e, m.group(1)))
                    continue
                if mn == "inc" or (mn == "add" and op.endswith(", 1")):
                    for f in L[k + 1:k + 9]:
                        if f[2] == "cmp" and m.group(1) in f[3] and re.search(r", (0x[0-9a-f]+|\d+)$", f[3]):
                            imm = int(f[3].split(", ")[-1], 0)
                            if imm >= 2:
                                inc_hits.append((o, e, m.group(1), imm, f))
                            break
                    continue
            # load-modify-store via register
            if mn == "mov" and m and not is_stack(m.group(1)) and op.startswith("e") and op.index(",") == 3:
                r = op[:3]
                field = m.group(1)
                mod, val = None, None
                for f in L[k + 1:k + 7]:
                    if f[2] == "dec" and f[3] == r:
                        mod, val = r, -1
                    elif f[2] == "inc" and f[3] == r:
                        mod, val = r, +1
                    elif f[2] == "lea" and re.match(r"e.., \[%s - 1\]$" % r, f[3]):
                        mod, val = f[3][:3], -1
                    elif f[2] == "lea" and re.match(r"e.., \[%s \+ 1\]$" % r, f[3]):
                        mod, val = f[3][:3], +1
                    elif mod and f[2] == "mov" and f[3] == "dword ptr [%s], %s" % (field, mod):
                        if val < 0:
                            dec_hits.append((o, e, field))
                        else:
                            for g in L[k:k + 9]:
                                if g[2] == "cmp" and (g[3].startswith(r + ",") or g[3].startswith(mod + ",") or
                                                      field in g[3]) and re.search(r", (0x[0-9a-f]+|\d+)$", g[3]):
                                    imm = int(g[3].split(", ")[-1], 0)
                                    if imm >= 2:
                                        inc_hits.append((o, e, field, imm, g))
                                    break
                        break
    # initial values for DEC fields: displacement match
    disp_of = {}
    for o, e, field in dec_hits:
        mm = re.search(r"\+ (0x[0-9a-f]+)$", field)
        disp_of[(o, e[0])] = int(mm.group(1), 16) if mm else (int(field, 16) if field.startswith("0x") else None)
    inits = defaultdict(list)
    for e in idx:
        if e[2] == "mov" and e[3].startswith("dword ptr [") and not is_stack(e[3]):
            mm = re.match(r"dword ptr \[(?:[a-z]{3} \+ )?(0x[0-9a-f]+)\], (0x[0-9a-f]+|\d+)$", e[3])
            if mm:
                imm = int(mm.group(2), 0)
                if 2 <= imm <= 100000:
                    inits[int(mm.group(1), 16)].append((e, imm))
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "timing_tickscan.out"), "w",
              encoding="utf-8") as fo:
        fo.write("# DEC (count-down) sites in game code; dt? = owner also reads [0x2D6748]\n")
        for o, e, field in sorted(dec_hits, key=lambda t: t[1][0]):
            d = disp_of.get((o, e[0]))
            iv = sorted({imm for x, imm in inits.get(d, [])}) if d is not None else []
            fo.write("%08X %-14s dt?=%d  [%s]  init-imm(same disp): %s\n" % (
                e[0], owner_name(o), o in dt_owners, field, iv[:12]))
        fo.write("\n# INC + CMP imm (count-up) sites\n")
        for o, e, field, imm, f in sorted(inc_hits, key=lambda t: t[1][0]):
            fo.write("%08X %-14s dt?=%d  [%s]  cmp %d at %08X\n" % (e[0], owner_name(o), o in dt_owners, field,
                                                                    imm, f[0]))
    print("dec sites", len(dec_hits), "functions", len({o for o, e, f in dec_hits}))
    print("inc+cmp sites", len(inc_hits), "functions", len({h[0] for h in inc_hits}))


if __name__ == "__main__":
    main()
