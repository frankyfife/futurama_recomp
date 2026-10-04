"""Binary scan: find code that loads the app object (global 0x2eb310) and then
reads [reg+0x104] (= NiXBoxRenderer* stored by FUN_000d26a0) and the camera
renderer field [reg+0x1a8]. Prints function + instruction."""
import sys, csv, bisect, struct
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
import capstone
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
F = {int(r["address"], 16): (r["name"], int(r["size"])) for r in csv.DictReader(open(r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"), delimiter="\t")}
DISP = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x104
GLOB = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x2eb310
hits = []
for fa, (name, size) in sorted(F.items()):
    if not (0x11000 <= fa < 0x2342c0) or size <= 0: continue
    code = X.read(fa, size)
    if code is None: continue
    regs = set()
    for ins in md.disasm(code, fa):
        ops = ins.op_str
        if ins.mnemonic == "mov" and f"[{GLOB:#x}]" in ops:
            regs.add(ops.split(",")[0].strip())
            continue
        if regs and f"+ {DISP:#x}]" in ops:
            base = ops.split("[")[1].split("+")[0].strip() if "[" in ops else ""
            if base in regs:
                hits.append((fa, name, ins.address, ins.mnemonic, ops))
        if ins.mnemonic in ("call", "ret"):
            regs = set()
for h in hits:
    print(f"{h[0]:08x}\t{h[1]}\t{h[2]:08x}\t{h[3]} {h[4]}")
