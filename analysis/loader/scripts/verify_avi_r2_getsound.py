"""Round-2: all direct call sites of the GetSound wrapper 0xc97d0 (cdecl
(name, bStream)) with the arguments recovered from the preceding pushes.

Uses linear-sweep instruction starts to walk back from each call and tracks
the last two 'push' instructions; string immediates are resolved, register
pushes are traced back to a 'mov reg, imm32' / 'lea' in the same block when
possible. Output: site, function, bStream, name.
"""
import collections
import re
import struct
from verify_avi_r2_common import X, SECT, func_of, linear_starts, MD, Tee

T = Tee("verify_avi_r2_getsound.out")
TGT = 0xC97D0
va, vsz, raw, rsz = SECT[".text"]
data = X.read(va, rsz)
starts = linear_starts(va, va + rsz)
order = sorted(starts)
idx = {a: i for i, a in enumerate(order)}

sites = []
for off in range(len(data) - 5):
    if data[off] != 0xE8:
        continue
    s = va + off
    if s in starts and (s + 5 + struct.unpack_from("<i", data, off + 1)[0]) & 0xFFFFFFFF == TGT:
        sites.append(s)


def insn_at(a):
    return next(MD.disasm(X.read(a, 16), a, 1))


def resolve(op, before):
    """resolve a push operand to a value if possible"""
    op = op.strip()
    if re.fullmatch(r"0x[0-9a-f]+|\d+", op):
        return int(op, 0)
    if re.fullmatch(r"e[abcd]x|e[sd]i|ebp|ebx", op):
        for ins in reversed(before):
            m = re.fullmatch(r"(\w+), (0x[0-9a-f]+|\d+)", ins.op_str)
            if ins.mnemonic == "mov" and m and m.group(1) == op:
                return int(m.group(2), 0)
            if ins.mnemonic == "xor" and ins.op_str == "%s, %s" % (op, op):
                return 0
            if ins.op_str.startswith(op + ",") and ins.mnemonic not in ("cmp", "test", "push"):
                return None
    return None


stream_vals = collections.Counter()
rows = []
for s in sites:
    i = idx[s]
    prev = [insn_at(order[j]) for j in range(max(0, i - 40), i)]
    far = [insn_at(order[j]) for j in range(max(0, i - 600), i)]
    pushes = [k for k, ins in enumerate(prev) if ins.mnemonic == "push"]
    if len(pushes) < 2:
        rows.append((s, None, None))
        continue
    pn, ps = prev[pushes[-1]], prev[pushes[-2]]
    name = resolve(pn.op_str, prev[:pushes[-1]])
    bs = resolve(ps.op_str, prev[:pushes[-2]])
    if bs is None and ps.op_str in ("ebx", "esi", "edi", "ebp"):
        # callee-saved register: search further back (up to 600 insns) for the last write
        k = len(far) - (len(prev) - pushes[-2])
        bs = resolve(ps.op_str, far[:k])
        if bs is not None:
            bs = "%s(%s,far)" % (bs, ps.op_str)
    nm = None
    if name is not None and X.section(name) in (".rdata", ".data"):
        nm = X.cstr(name)
    rows.append((s, bs if bs is not None else "?(%s)" % ps.op_str, nm if nm is not None else "?(%s)" % pn.op_str))

for s, bs, nm in rows:
    fa, fn = func_of(s)
    stream_vals[bs] += 1
    T("%08x %-14s bStream=%-12s name=%s" % (s, fn or "nofunc", bs, nm))
T("\nsites:", len(sites), "in Ghidra funcs:", sum(1 for s in sites if func_of(s)[0]),
  "nofunc:", sum(1 for s in sites if not func_of(s)[0]))
T("bStream distribution:", dict(stream_vals))
