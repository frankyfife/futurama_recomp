"""Reviewer helper: linear listing of all instructions in [start,end) that touch a register,
plus all calls/jumps, to validate 'call reg' sites that follow a branch.
Usage: python verify_kernel_regtrace.py start:end:reg [...]
"""
import re
import sys

from verify_kernel_common import XBE, fmt, md

for arg in sys.argv[1:]:
    s, e, reg = arg.split(":")
    s, e = int(s, 16), int(e, 16)
    print("---- %06x..%06x reg %s" % (s, e, reg))
    for i in md.disasm(XBE.read(s, e - s), s):
        if re.search(r"\b%s\b" % reg, i.op_str) or i.mnemonic in ("call", "ret") or i.mnemonic.startswith("j"):
            print("   " + fmt(i))
