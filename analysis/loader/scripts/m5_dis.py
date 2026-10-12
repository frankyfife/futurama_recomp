"""Annotated disassembly of address ranges: python m5_dis.py <start> <end> [<start> <end> ...]
Optional: OUT=<name> writes run/analyse_m5/<name>.lst as well."""
import os
import sys
from m5_common import listing, name_of, owner, run_path

args = sys.argv[1:]
lines = []
for i in range(0, len(args), 2):
    a, b = int(args[i], 16), int(args[i + 1], 16)
    o = owner(a)
    lines.extend(listing(a, b, "in %s" % (name_of(o) if o else "nofunc")))
text = "\n".join(lines)
print(text)
if os.environ.get("OUT"):
    open(run_path(os.environ["OUT"] + ".lst"), "w", encoding="utf-8").write(text + "\n")
