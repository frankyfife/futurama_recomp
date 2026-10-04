"""verify_cpu_fixtsv.py - adds the two APU immediates that cpu_scan.py missed to cpu_hw_refs.tsv
(found by verify_cpu_mmio.py: 'add [edi+0x10],0xfe836000' and the loop bound 'cmp [ebp-4],0xfe82043c').
Idempotent: rows already present are not added twice. Bytes are taken from the XBE."""
import sys
sys.path.insert(0, r"D:\FuturamaDecomp\analysis\loader\scripts")
from verify_cpu_common import *

P = LOADER + r"\cpu_hw_refs.tsv"
lines = [l.rstrip("\n") for l in open(P, encoding="utf-8")]
hdr, rows = lines[0], lines[1:]
have = {r.split("\t")[0] for r in rows}
new = []
for a, val, func in ((0x24a1f9, 0xfe836000, "FUN_0024a0f2"), (0x24cb6b, 0xfe82043c, "FUN_0024caca")):
    i = dis(a, 1)[0]
    o = owner(a)
    assert o[2] == func and o[3], o
    assert ("0x%x" % val) in i.op_str, i.op_str
    key = "%08x" % a
    if key in have:
        continue
    new.append("\t".join([key, i.bytes.hex(), i.mnemonic + " " + i.op_str, "imm", "%08x" % val, "APU MMIO",
                          "mmio-imm", func, "DSOUND", "True", "code", "DSOUND"]))
rows = sorted(rows + new, key=lambda r: int(r.split("\t")[0], 16))
open(P, "w", encoding="utf-8", newline="\n").write("\n".join([hdr] + rows) + "\n")
print("added", len(new), "rows; total data rows now", len(rows))
