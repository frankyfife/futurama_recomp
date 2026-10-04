"""Heuristic binary scan for virtual calls on the NiXBoxRenderer object.
Renderer sources tracked per function (linear, register + stack-slot tracking):
  * [reg+0x1a8]                       NiCamera renderer pointer (set by FUN_00013d40)
  * [[0x2eb310]+0x104], [[0x2e7b84]+0x104]  app->m_spRenderer (FUN_000d26a0 stores it)
  * eax after call [appvtbl+0x14]     app vfunc 5 = 0xd2690 'mov eax,[ecx+0x104]; ret'
  * [ecx/esi+0x104] inside app methods (FUN_000d2630, FUN_000a26a0, FUN_000d26a0)
  * this (ecx at entry) inside NiXBoxRenderer/NiRenderer methods (vtable functions)
  * the renderer list DAT_002d7b1c broadcast functions (handled by caller list)
Output TSV: func, name, callsite, offset, slot"""
import sys, csv, re
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
import capstone
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
F = {int(r["address"], 16): (r["name"], int(r["size"])) for r in csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"), delimiter="\t")}
NIX = 0x2aa7c0
REN_THIS = {X.u32(NIX + 4 * i) for i in range(75)} | {0x14c170, 0x148870}
APP_METHODS = {0xd2630, 0xa26a0, 0xd26a0}
R32 = ["eax", "ecx", "edx", "ebx", "esi", "edi", "ebp"]
memre = re.compile(r"dword ptr \[(\w+)(?: ([+-]) (0x[0-9a-f]+|\d+))?\]")
def scan_func(fa, name, size):
    code = X.read(fa, size)
    if code is None: return []
    ren, vt, app, appvt, stk = set(), set(), set(), set(), set()
    if fa in REN_THIS: ren.add("ecx")
    appthis = {"ecx"} if fa in APP_METHODS else set()
    out = []
    for ins in md.disasm(code, fa):
        m, ops = ins.mnemonic, ins.op_str
        parts = [p.strip() for p in ops.split(",", 1)]
        dst = parts[0] if len(parts) == 2 else ""
        src = parts[1] if len(parts) == 2 else ops
        if m in ("call", "jmp"):
            mm = memre.fullmatch(ops)
            if mm and mm.group(1) in vt:
                off = int(mm.group(3), 0) if mm.group(3) else 0
                out.append((fa, name, ins.address, off))
            got = bool(mm and mm.group(1) in appvt and mm.group(3) and int(mm.group(3), 0) == 0x14)
            for r in ("eax", "ecx", "edx"):
                for s in (ren, vt, app, appvt, appthis): s.discard(r)
            if got: ren.add("eax")
            continue
        if m == "mov" and dst in R32:
            for s in (ren, vt, app, appvt, appthis): s.discard(dst)
            ms = memre.fullmatch(src)
            if src in ("dword ptr [0x2eb310]", "dword ptr [0x2e7b84]"):
                app.add(dst)
            elif ms:
                base = ms.group(1); d = int(ms.group(3), 0) if ms.group(3) else 0
                if ms.group(2) == "-": d = -d
                if d == 0x1a8 or (d == 0x104 and (base in app or base in appthis)):
                    ren.add(dst)
                elif base in ren and d == 0:
                    vt.add(dst)
                elif base in app and d == 0:
                    appvt.add(dst)
                elif src in stk:
                    ren.add(dst)
            elif src in R32:
                for s in (ren, vt, app, appvt, appthis):
                    if src in s: s.add(dst)
        elif m == "mov" and dst.startswith("dword ptr [") and src in ren:
            stk.add(dst)
        elif dst in R32 and m not in ("cmp", "test", "push"):
            for s in (ren, vt, app, appvt, appthis): s.discard(dst)
    return out
if __name__ == "__main__":
    for fa, (name, size) in sorted(F.items()):
        if 0x11000 <= fa < 0x2342c0 and size > 0:
            for r in scan_func(fa, name, size):
                print(f"{r[0]:08x}\t{r[1]}\t{r[2]:08x}\t{r[3]:#x}\tslot{r[3] // 4}")
