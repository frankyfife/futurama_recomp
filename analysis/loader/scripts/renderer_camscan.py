"""Heuristic binary scan for virtual calls through a renderer pointer obtained
as [reg+0x1a8] (NiCamera::m_spRenderer, set by FUN_00013d40) or via
[[0x2eb310]+0x104] (app->renderer). Linear per-function register tracking;
callee-saved regs survive calls, eax/ecx/edx are dropped at calls.
Output: function, call site, vtable offset, slot."""
import sys, csv, re
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
import capstone
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
F = {int(r["address"], 16): (r["name"], int(r["size"])) for r in csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"), delimiter="\t")}
R32 = ["eax","ecx","edx","ebx","esi","edi","ebp"]
memre = re.compile(r"dword ptr \[(\w+)(?: \+ (0x[0-9a-f]+|\d+))?\]")
out = []
for fa, (name, size) in sorted(F.items()):
    if not (0x11000 <= fa < 0x2342c0) or size <= 0: continue
    code = X.read(fa, size)
    if code is None: continue
    ren, vt, app = set(), set(), set()
    for ins in md.disasm(code, fa):
        m, ops = ins.mnemonic, ins.op_str
        dst = ops.split(",")[0].strip() if "," in ops else ""
        src = ops.split(",", 1)[1].strip() if "," in ops else ops
        if m in ("call", "jmp"):
            mm = memre.fullmatch(ops)
            if mm and mm.group(1) in vt:
                off = int(mm.group(2), 0) if mm.group(2) else 0
                out.append((fa, name, ins.address, off))
            for r in ("eax", "ecx", "edx"):
                ren.discard(r); vt.discard(r); app.discard(r)
            continue
        if m == "mov" and dst in R32:
            for s in (ren, vt, app): s.discard(dst)
            ms = memre.fullmatch(src)
            if src == "dword ptr [0x2eb310]":
                app.add(dst)
            elif ms:
                base, d = ms.group(1), (int(ms.group(2), 0) if ms.group(2) else 0)
                if d == 0x1a8 or (base in app and d == 0x104):
                    ren.add(dst)
                elif base in ren and d == 0:
                    vt.add(dst)
            elif src in R32:
                if src in ren: ren.add(dst)
                if src in vt: vt.add(dst)
        elif dst in R32 and m not in ("cmp", "test", "push"):
            for s in (ren, vt, app): s.discard(dst)
for fa, name, at, off in out:
    print(f"{fa:08x}\t{name}\t{at:08x}\t{off:#x}\tslot{off//4}")
