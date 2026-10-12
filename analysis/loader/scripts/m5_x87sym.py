"""Tiny symbolic x87/stack evaluator for straight-line FPU code (used to derive the listener orientation
in SoundMgr::Update 0xCBCB0 and similar blocks). Tracks esp (push/pop/sub/add), symbolic memory for
[esp+N] slots, x87 register stack, and prints every fstp/fst/push of a symbolic value.

Usage: python m5_x87sym.py <start> <end> [name=value ...]   (initial [esp+off] names, e.g. 40=m00)
Output: stdout (copied into scripts/out/m5_listener_orient.out by the caller when needed)."""
import re
import sys
from m5_common import dis_range, f32, name_of, LOADER
import csv, os
PURGE = {}
with open(os.path.join(LOADER, "avi_api_surface.tsv"), encoding="utf-8") as f:
    r = csv.reader(f, delimiter="	"); next(r)
    for row in r:
        if not row[0].startswith("vtbl"):
            m = re.search(r"ret (\d+)", row[3])
            PURGE[int(row[0], 16)] = int(m.group(1)) if m else 0

start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
mem = {}  # stack offset (relative to the initial esp) -> symbolic string
for kv in sys.argv[3:]:
    k, v = kv.split("=")
    mem[int(k, 16)] = v
esp = 0
regs = {}
st = []


def sym_mem(op):
    m = re.match(r"dword ptr \[esp(?: \+ (0x[0-9a-f]+|\d+))?\]", op)
    if m:
        off = esp + int(m.group(1) or "0", 0)
        return mem.get(off, "S[%#x]" % off), ("stack", off)
    m = re.match(r"dword ptr \[(0x[0-9a-f]+)\]", op)
    if m:
        a = int(m.group(1), 16)
        try:
            return ("%g" % f32(a)), ("abs", a)
        except Exception:
            return "[%x]" % a, ("abs", a)
    return op, None


def simp_mul(a, b):
    for x, y in ((a, b), (b, a)):
        if x in ("0", "-0"):
            return "0"
        if x == "1":
            return y
        if x == "-1":
            return "-(%s)" % y
    return "(%s*%s)" % (a, b)


def simp_add(a, b, op="+"):
    if b == "0":
        return a
    if a == "0":
        return b if op == "+" else "-(%s)" % b
    return "(%s%s%s)" % (a, op, b)


for ins in dis_range(start, end):
    mn, ops = ins.mnemonic, ins.op_str
    a = [x.strip() for x in ops.split(",")] if ops else []
    line = "%08x %-6s %-30s" % (ins.address, mn, ops)
    note = ""
    if mn == "push":
        esp -= 4
        if a[0].startswith("0x") or a[0].isdigit():
            mem[esp] = a[0]
        else:
            mem[esp] = regs.get(a[0], a[0])
        note = "push %s" % mem[esp]
    elif mn == "pop":
        esp += 4
    elif mn == "sub" and a[0] == "esp":
        esp -= int(a[1], 0)
    elif mn == "add" and a[0] == "esp":
        esp += int(a[1], 0)
    elif mn == "mov" and a[0] in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"):
        v, loc = sym_mem(a[1])
        regs[a[0]] = v
    elif mn == "mov" and a[0].startswith("dword ptr [esp"):
        v, loc = sym_mem(a[0])
        if loc:
            mem[loc[1]] = regs.get(a[1], a[1])
            note = "mem[%#x] <- %s" % (loc[1], mem[loc[1]])
    elif mn == "fld":
        v, _ = sym_mem(a[0])
        st.insert(0, v)
    elif mn == "fild":
        v, _ = sym_mem(a[0])
        st.insert(0, "int(%s)" % v)
    elif mn == "fld1":
        st.insert(0, "1")
    elif mn in ("fmul", "fadd", "fsub", "fsubr"):
        if a[0].startswith("dword"):
            v, _ = sym_mem(a[0])
            if mn == "fmul":
                st[0] = simp_mul(st[0], v)
            elif mn == "fadd":
                st[0] = simp_add(st[0], v)
            elif mn == "fsub":
                st[0] = simp_add(st[0], v, "-")
            else:
                st[0] = simp_add(v, st[0], "-")
        else:
            i = int(re.match(r"st\((\d)\)", a[-1]).group(1)) if len(a) == 1 else None
            if len(a) == 1:
                if mn == "fmul":
                    st[0] = simp_mul(st[0], st[i])
            else:
                note = "unhandled 2-op"
    elif mn in ("faddp", "fmulp", "fsubp", "fsubrp"):
        i = int(re.match(r"st\((\d)\)", a[0]).group(1)) if a else 1
        if mn == "faddp":
            st[i] = simp_add(st[i], st[0])
        elif mn == "fmulp":
            st[i] = simp_mul(st[i], st[0])
        elif mn == "fsubp":
            st[i] = simp_add(st[i], st[0], "-")
        st.pop(0)
    elif mn == "fxch":
        i = int(re.match(r"st\((\d)\)", a[0]).group(1)) if a else 1
        st[0], st[i] = st[i], st[0]
    elif mn in ("fstp", "fst"):
        if a[0].startswith("st("):
            i = int(a[0][3])
            st[i] = st[0]
            if mn == "fstp":
                st.pop(0)
        else:
            v, loc = sym_mem(a[0])
            if loc and loc[0] == "stack":
                mem[loc[1]] = st[0]
            note = "store %s <- %s" % (a[0], st[0])
            if mn == "fstp":
                st.pop(0)
    elif mn == "call":
        tgt = int(ops, 16) if ops.startswith("0x") else None
        args = [mem.get(esp + 4 * k, "?") for k in range(8)]
        note = "CALL %s args(top..)=%s" % (name_of(tgt) if tgt else ops, args)
        if tgt in PURGE:
            esp += PURGE[tgt]
        if tgt == 0x167080:  # _ftol: pops st0 into eax
            regs["eax"] = "ftol(%s)" % st.pop(0)
            note = "ftol -> eax = %s" % regs["eax"]
    print(line, "|", note, "| st=", st[:4] if mn.startswith("f") else "")
