"""xboxkrnl import analysis for retail_eu default.xbe.

Produces:
  analysis/loader/kernel_raw.json   - everything (refs, call sites, purge solver profile)
Run:  python kernel_analyze.py

Steps
 1. decode kernel thunk table (0x293560), ordinal -> name (kernel_common table)
 2. find every code reference to a thunk slot (raw byte scan of the code sections,
    then capstone decode; only memory operands with disp == slot, no base/index reg)
 3. find thunk stubs (6-byte 'jmp dword ptr [slot]') and every call to them
 4. per containing function: recursive-descent CFG walk with stack-depth tracking
    (push/pop/sub/add esp, ebp frames, __SEH_prolog/__SEH_epilog, callee 'ret N')
    -> for each kernel call site: pushes since previous call, depth, cleanup after
       the call, ecx/edx set before call, eax/al used after call
 5. solve the stack purge (bytes popped by callee) of every kernel function:
    coordinate descent over candidates 0..0x30, minimizing stack inconsistencies
    (depth != 0 at ret/tail-jmp, conflicting depths at CFG joins, negative depth)
 6. data refs: how the pointer loaded from the slot is dereferenced
"""
import collections
import json
import os
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import (X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_EAX, X86_REG_EBP,
                          X86_REG_EBX, X86_REG_ECX, X86_REG_EDI, X86_REG_EDX, X86_REG_ESI,
                          X86_REG_ESP, X86_REG_INVALID)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kernel_common import (OUT, THUNK_VA, XBOXKRNL_EXPORTS, Functions, load_xbe,  # noqa: E402
                           read_thunks)

CODE_SECTIONS = (".text", "D3D", "DSOUND", "XMV", "XPP")
SEH_PROLOG = 0x167DC8
SEH_EPILOG = 0x167E03

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

x = load_xbe()
F = Functions()
FUNC_ENTRIES = set(F.addrs)
thunks = read_thunks(x)
NSLOT = len(thunks)
SLOT_LO, SLOT_HI = THUNK_VA, THUNK_VA + 4 * NSLOT


def slot_of(v):
    v &= 0xFFFFFFFF
    if SLOT_LO <= v < SLOT_HI and (v - SLOT_LO) % 4 == 0:
        return (v - SLOT_LO) // 4
    return None


def in_code(va):
    s = x.section(va)
    return s in CODE_SECTIONS


_icache = {}


def insn_at(a):
    r = _icache.get(a)
    if r is None:
        code = x.read(a, 16)
        if code is None:
            return None
        r = next(md.disasm(code, a), None)
        _icache[a] = r if r is not None else False
    return r or None


REG32 = {X86_REG_EAX: "eax", X86_REG_ECX: "ecx", X86_REG_EDX: "edx", X86_REG_EBX: "ebx",
         X86_REG_ESI: "esi", X86_REG_EDI: "edi", X86_REG_EBP: "ebp", X86_REG_ESP: "esp"}


def reg32(r):
    """capstone reg id -> canonical 32-bit register name (al/ax/eax -> eax)."""
    n = md.reg_name(r)
    if n is None:
        return None
    m = {"al": "eax", "ah": "eax", "ax": "eax", "eax": "eax", "cl": "ecx", "ch": "ecx",
         "cx": "ecx", "ecx": "ecx", "dl": "edx", "dh": "edx", "dx": "edx", "edx": "edx",
         "bl": "ebx", "bh": "ebx", "bx": "ebx", "ebx": "ebx", "si": "esi", "esi": "esi",
         "di": "edi", "edi": "edi", "bp": "ebp", "ebp": "ebp", "sp": "esp", "esp": "esp"}
    return m.get(n)


def mem_slot(ins):
    """slot index if ins has a [disp32] memory operand pointing into the thunk table
    (no base and no index register)."""
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_INVALID and \
                op.mem.index == X86_REG_INVALID and op.mem.segment == X86_REG_INVALID:
            s = slot_of(op.mem.disp)
            if s is not None:
                return s
    return None


# ---------------------------------------------------------------- 2. raw refs
def raw_refs():
    refs = []
    rejected = []
    for name, va, vsz, raw, rsz in x.sections:
        if name not in CODE_SECTIONS:
            continue
        b = x.b[raw:raw + rsz]
        for i in range(len(b) - 3):
            v = struct.unpack_from("<I", b, i)[0]
            s = slot_of(v)
            if s is None:
                continue
            a = va + i
            hit = None
            for k in range(1, 4):
                ins = insn_at(a - k)
                if ins is None or ins.address + ins.size < a + 4:
                    continue
                for op in ins.operands:
                    if op.type == X86_OP_MEM and (op.mem.disp & 0xFFFFFFFF) == v:
                        hit = (ins, op)
                        break
                if hit:
                    break
            if hit is None:
                rejected.append({"addr": a, "slot": s, "why": "no instruction with this disp"})
                continue
            ins, op = hit
            if op.mem.base != X86_REG_INVALID or op.mem.index != X86_REG_INVALID:
                rejected.append({"addr": ins.address, "slot": s,
                                 "why": "indexed operand (table artefact): %s %s" % (ins.mnemonic, ins.op_str)})
                continue
            refs.append({"ins": ins.address, "slot": s, "mnem": ins.mnemonic, "ops": ins.op_str,
                         "bytes": ins.bytes.hex()})
    return refs, rejected


# ---------------------------------------------------------------- purge of internal functions
_purge = {}


def flow_succ(ins, func_entry):
    """returns (successors, kind) kind in {'fall','ret','tail','stop'}"""
    m = ins.mnemonic
    nxt = ins.address + ins.size
    if m in ("ret", "retn", "retf", "iret", "iretd"):
        return [], "ret"
    if m in ("int3", "hlt", "ud2"):
        return [], "stop"
    if m == "jmp":
        op = ins.operands[0]
        if op.type == X86_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            if t != func_entry and t in FUNC_ENTRIES:
                return [], "tail"
            return [t], "jmp"
        # indirect jump: try switch table  jmp [reg*4+table]
        if op.type == X86_OP_MEM and op.mem.index != X86_REG_INVALID and op.mem.scale == 4 \
                and op.mem.base == X86_REG_INVALID:
            tbl = op.mem.disp & 0xFFFFFFFF
            out = []
            for j in range(256):
                try:
                    t = x.u32(tbl + 4 * j)
                except Exception:
                    break
                if not in_code(t) or abs(t - ins.address) > 0x10000:
                    break
                out.append(t)
            return out, "switch"
        return [], "tail"  # jmp [slot] / jmp reg -> leaves function
    if m.startswith("j") or m in ("loop", "loope", "loopne", "jecxz", "jcxz"):
        op = ins.operands[0]
        if op.type == X86_OP_IMM:
            return [op.imm & 0xFFFFFFFF, nxt], "cond"
        return [nxt], "fall"
    return [nxt], "fall"


def internal_purge(entry, depth=0):
    """bytes popped by 'ret N' of an internal function, None if unknown."""
    if entry in _purge:
        return _purge[entry]
    _purge[entry] = None
    if depth > 8 or not in_code(entry):
        return None
    seen = set()
    work = [entry]
    rets = set()
    tails = set()
    n = 0
    while work and n < 30000:
        a = work.pop()
        if a in seen:
            continue
        seen.add(a)
        ins = insn_at(a)
        n += 1
        if ins is None:
            continue
        succ, kind = flow_succ(ins, entry)
        if kind == "ret":
            rets.add(ins.operands[0].imm if ins.operands else 0)
        elif kind == "tail" and ins.operands and ins.operands[0].type == X86_OP_IMM:
            tails.add(ins.operands[0].imm & 0xFFFFFFFF)
        elif kind == "tail" and mem_slot(ins) is not None:
            tails.add(("slot", mem_slot(ins)))
        for s in succ:
            if in_code(s):
                work.append(s)
    if len(rets) == 1:
        r = rets.pop()
    elif not rets and len(tails) == 1:
        t = tails.pop()
        r = None if isinstance(t, tuple) else internal_purge(t, depth + 1)
        if isinstance(t, tuple):
            r = ("slot", t[1])
    else:
        r = None
    _purge[entry] = r
    return r


# ---------------------------------------------------------------- stubs
def find_stubs():
    stubs = {}
    for a, sz, blk, name in F.rows:
        ins = insn_at(a)
        if ins is not None and ins.mnemonic == "jmp" and mem_slot(ins) is not None and \
                ins.operands[0].type == X86_OP_MEM:
            stubs[a] = (mem_slot(ins), name, sz)
    return stubs


# ---------------------------------------------------------------- owner resolution
_reach = {}


def reach(entry, limit=20000):
    """instruction addresses reachable from entry (recursive descent, calls not followed)."""
    if entry in _reach:
        return _reach[entry]
    seen = set()
    work = [entry]
    while work and len(seen) < limit:
        a = work.pop()
        if a in seen or not in_code(a):
            continue
        ins = insn_at(a)
        if ins is None:
            continue
        seen.add(a)
        succ, kind = flow_succ(ins, entry)
        work.extend(succ)
    _reach[entry] = seen
    return seen


_code_refs = None


def code_refs():
    """code addresses that are E8/E9 targets or appear as a dword anywhere in the image."""
    global _code_refs
    if _code_refs is None:
        lo = min(va for n, va, vsz, raw, rsz in x.sections if n in CODE_SECTIONS)
        hi = max(va + vsz for n, va, vsz, raw, rsz in x.sections if n in CODE_SECTIONS)
        refs = set()
        for name, va, vsz, raw, rsz in x.sections:
            b = x.b[raw:raw + rsz]
            for i in range(len(b) - 3):
                v = struct.unpack_from("<I", b, i)[0]
                if lo <= v < hi:
                    refs.add(v)
                if name in CODE_SECTIONS and b[i] in (0xE8, 0xE9) and i + 5 <= len(b):
                    refs.add((va + i + 5 + struct.unpack_from("<i", b, i + 1)[0]) & 0xFFFFFFFF)
        _code_refs = refs
    return _code_refs


_owner = {}


def owner(va):
    """(entry, kind) of the function containing va.
    kind: 'ghidra' (inside functions.tsv range), 'ghidra-chunk' (reachable from the
    preceding Ghidra function), 'nofunc' (code Ghidra did not turn into a function;
    entry = first candidate in the gap from which va is reachable)."""
    if va in _owner:
        return _owner[va]
    import bisect
    i = bisect.bisect_right(F.addrs, va) - 1
    res = (None, None)
    if i >= 0:
        a, sz, blk, name = F.rows[i]
        if a <= va < a + max(sz, 1):
            res = (a, "ghidra")
        elif va in reach(a):
            res = (a, "ghidra-chunk")
        else:
            gs = a + sz
            refd, padded = [], []
            for c in range(gs, va + 1):
                pb = x.read(c - 1, 1)[0]
                cb = x.read(c, 1)[0]
                if c in code_refs():
                    refd.append(c)
                if (pb in (0xCC, 0x90, 0xC3) or (c - 3 >= gs and x.read(c - 3, 1)[0] == 0xC2))                         and cb not in (0xCC, 0x90):
                    padded.append(c)
            # priority: address referenced elsewhere (call target / pointer), then
            # first byte after padding or ret, then the gap start
            def is_table(c):  # bytes at c form a code pointer (switch table entry)
                v = x.u32(c)
                return in_code(v) and abs(v - c) < 0x10000

            for group in (refd, padded, [gs]):
                hit = next((c for c in group if c <= va and not is_table(c) and va in reach(c)), None)
                if hit is not None:
                    res = (hit, "nofunc")
                    break
    _owner[va] = res
    return res


def func_name(entry):
    n = F.name(entry)
    return n if n else "sub_%06x(no Ghidra func)" % entry


# ---------------------------------------------------------------- simulation
_rwcache = {}


def regs_rw(ins):
    k = ins.address
    if k in _rwcache:
        return _rwcache[k]
    try:
        r, w = ins.regs_access()
        res = ({reg32(i) for i in r} - {None}, {reg32(i) for i in w} - {None})
    except Exception:
        res = (set(), set())
    _rwcache[k] = res
    return res


class Sim:
    """Recursive descent over one function with stack depth tracking."""

    def __init__(self, entry, stubs):
        self.entry = entry
        self.stubs = stubs

    def kernel_target(self, ins, regmap):
        """slot index if this call/jmp transfers to a kernel import."""
        op = ins.operands[0] if ins.operands else None
        if op is None:
            return None, None
        if op.type == X86_OP_MEM:
            s = mem_slot(ins)
            if s is not None:
                return s, "direct"
        elif op.type == X86_OP_REG:
            r = reg32(op.reg)
            if r in regmap:
                return regmap[r], "reg:" + r
        elif op.type == X86_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            if t in self.stubs:
                return self.stubs[t][0], "stub:%06x" % t
        return None, None

    def run(self, kpurge, collect=False, saves=None):
        """Walk the CFG. The stack is modelled symbolically: a tuple with one entry per
        dword above the return address; entry = address of the push that stored it,
        or None (locals from sub esp, SEH frame, ...).
        saves: set of push addresses known to be callee-saved register spills or
        'push ecx' local allocations (paired with a pop of the same register); they
        are not counted as call arguments."""
        entry = self.entry
        saves = saves or set()
        viol = collections.Counter()
        sites = []
        dataref = []
        found_saves = set()
        visited = {}
        # state: addr, symbolic stack, ebp frame length, pushes since last call,
        #        regs holding a kernel slot, previous insn was a call,
        #        regs written since entry, still in prologue
        work = [(entry, (), None, 0, (), False, frozenset(), True)]
        steps = 0
        saves = set(saves)

        def drop(st, n):
            if st is None:
                return None
            if n > len(st):
                viol["neg"] += 1
                return None
            return st[:len(st) - n] if n else st

        def to_len(st, n):
            """frame reset (mov esp,ebp / leave / SEH epilog): entries above the frame
            are locals or spills, not call arguments."""
            if st is None or n is None:
                return None
            if n <= len(st):
                found_saves.update(e for e in st[n:] if e is not None)
                return st[:n]
            return st + (None,) * (n - len(st))

        while work and steps < 40000:
            a, sym, ebpl, psc, regmap_t, after_call, wr, prolog = work.pop()
            if a in visited:
                vd = visited[a]
                if sym is not None and vd is not None and vd != len(sym):
                    viol["join"] += 1
                continue
            visited[a] = None if sym is None else len(sym)
            ins = insn_at(a)
            steps += 1
            if ins is None:
                viol["bad_decode"] += 1
                continue
            regmap = dict(regmap_t)
            m = ins.mnemonic
            ops = ins.operands
            nxt = a + ins.size
            nsym, nebpl, npsc = sym, ebpl, psc
            rr, ww = regs_rw(ins)
            s_mem = mem_slot(ins)
            if s_mem is not None and m not in ("call", "jmp") and collect:
                dataref.append({"ins": a, "slot": s_mem, "mnem": m, "ops": ins.op_str,
                                "uses": self.follow_uses(ins, s_mem)})
            for r in ww:
                regmap.pop(r, None)
            if m == "mov" and s_mem is not None and ops[0].type == X86_OP_REG:
                regmap[reg32(ops[0].reg)] = s_mem

            nprolog = prolog and (m == "push" and ops[0].type == X86_OP_REG or
                                  (m == "mov" and ins.op_str == "ebp, esp") or
                                  (m == "sub" and ins.op_str.startswith("esp,")))
            if m == "push":
                nsym = None if sym is None else sym + (a,)
                if ops[0].type == X86_OP_REG:
                    pr = reg32(ops[0].reg)
                    # structural spill: callee-saved reg pushed before it was ever
                    # written, or 'push ecx' in the prologue (local allocation)
                    if (pr in ("ebx", "esi", "edi", "ebp") and pr not in wr) or                             (pr == "ecx" and prolog):
                        saves.add(a)
                        found_saves.add(a)
                if a not in saves:
                    npsc = psc + 1
            elif m == "pushfd":
                nsym = None if sym is None else sym + (None,)
            elif m == "pushal":
                nsym = None if sym is None else sym + (None,) * 8
            elif m == "pop":
                if sym:
                    top = sym[-1]
                    if top is not None and not after_call and ops and ops[0].type == X86_OP_REG:
                        pi = insn_at(top)
                        if pi is not None and pi.operands and pi.operands[0].type == X86_OP_REG and \
                                reg32(pi.operands[0].reg) == reg32(ops[0].reg):
                            found_saves.add(top)
                nsym = drop(sym, 1)
                npsc = max(0, psc - 1)
            elif m == "popfd":
                nsym = drop(sym, 1)
            elif m == "popal":
                nsym = drop(sym, 8)
            elif m in ("sub", "add") and ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_ESP:
                if ops[1].type == X86_OP_IMM and ops[1].imm % 4 == 0:
                    imm = ops[1].imm // 4
                    if m == "add":
                        nsym = drop(sym, imm)
                        npsc = max(0, psc - imm)
                    else:
                        nsym = None if sym is None else sym + (None,) * imm
                else:
                    nsym = None
            elif m == "lea" and ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_ESP:
                mm = ops[1].mem
                if mm.base == X86_REG_ESP and mm.index == X86_REG_INVALID and mm.disp % 4 == 0:
                    k = mm.disp // 4
                    nsym = drop(sym, k) if k >= 0 else (None if sym is None else sym + (None,) * (-k))
                elif mm.base == X86_REG_EBP and mm.index == X86_REG_INVALID and ebpl is not None \
                        and mm.disp % 4 == 0:
                    nsym = to_len(sym, ebpl - mm.disp // 4)
                else:
                    nsym = None
            elif m == "mov" and ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_EBP and \
                    ops[1].type == X86_OP_REG and ops[1].reg == X86_REG_ESP:
                nebpl = None if sym is None else len(sym)
            elif m == "mov" and ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_ESP:
                if ops[1].type == X86_OP_REG and ops[1].reg == X86_REG_EBP:
                    nsym = to_len(sym, ebpl)
                else:
                    nsym = None
            elif m == "leave":
                t = to_len(sym, ebpl)
                nsym = t[:-1] if t else None
                nebpl = None
            elif m == "and" and ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_ESP:
                pass  # stack alignment; a frame reset (mov esp,ebp) follows
            elif "esp" in ww and m not in ("call", "ret", "push", "pop"):
                nsym = None
            # --- control flow
            n_after_call = False
            if m == "call":
                slot, via = self.kernel_target(ins, regmap)
                nins = insn_at(nxt)
                cleanup = 0
                if nins is not None and nins.mnemonic == "add" and nins.operands[0].type == X86_OP_REG \
                        and nins.operands[0].reg == X86_REG_ESP and nins.operands[1].type == X86_OP_IMM:
                    cleanup = nins.operands[1].imm
                if slot is not None:
                    p = kpurge.get(slot)
                    if p is None:
                        p = 0 if cleanup else 4 * psc
                    if collect:
                        pend = None if sym is None else sum(1 for e in sym if e is not None and e not in saves)
                        sites.append(self.site_info(ins, slot, via, sym, psc, pend, cleanup, nins))
                else:
                    p = self.call_purge(ins, cleanup, psc)
                if p == "seh_prolog":
                    p1 = insn_at_prev_push(a, 2)
                    nl = (p1 or 0) // 4
                    if sym is not None and len(sym) >= 2:
                        nsym = sym[:-2] + (None,) * (8 + nl)
                        nebpl = len(sym) - 1
                    else:
                        nsym, nebpl = None, None
                elif p == "seh_epilog":
                    t = to_len(sym, ebpl)
                    nsym = t[:-1] if t else None
                elif p is None:
                    nsym = None
                else:
                    nsym = drop(sym, p // 4)
                npsc = 0
                n_after_call = True
                for r in ("eax", "ecx", "edx"):
                    regmap.pop(r, None)
                succ, kind = [nxt], "fall"
            else:
                if m == "jmp" and collect:
                    slot, via = self.kernel_target(ins, regmap)
                    if slot is not None:
                        pend = None if sym is None else sum(1 for e in sym if e is not None and e not in saves)
                        sites.append(self.site_info(ins, slot, "tailjmp-" + via, sym, psc, pend, 0, None))
                succ, kind = flow_succ(ins, entry)
            if kind in ("ret", "tail"):
                if kind == "tail" and m == "jmp" and nsym is not None and len(nsym) != 0:
                    viol["tail_depth"] += 1
                if kind == "ret" and nsym is not None and len(nsym) != 0:
                    viol["ret_depth"] += 1
                continue
            for s2 in succ:
                if in_code(s2):
                    work.append((s2, nsym, nebpl, npsc, tuple(sorted(regmap.items())), n_after_call,
                                 wr | (ww - {"esp"}), nprolog))
        self.found_saves = found_saves
        return viol, sites, dataref

    def call_purge(self, ins, cleanup, psc):
        op = ins.operands[0]
        if op.type == X86_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            if t == SEH_PROLOG:
                return "seh_prolog"
            if t == SEH_EPILOG:
                return "seh_epilog"
            p = internal_purge(t)
            if isinstance(p, tuple):  # tail jumps into a kernel slot
                return None
            if p is not None:
                return p
        # unknown callee (indirect / no ret): cdecl if add esp follows, else heuristic
        return 0 if cleanup else 4 * psc

    def site_info(self, ins, slot, via, sym, psc, pend, cleanup, nins):
        a = ins.address
        # look back for ecx/edx writes between previous call/label and this call
        ecx_set = edx_set = False
        prev = linear_back(a, 12)
        for p in reversed(prev):
            if p.mnemonic == "call":
                break
            r, w = regs_rw(p)
            if "ecx" in w:
                ecx_set = True
            if "edx" in w:
                edx_set = True
        # eax used after the call (read before written) in straight-line code
        eax_used = None
        b = a + ins.size
        for _ in range(8):
            q = insn_at(b)
            if q is None:
                break
            if q.mnemonic == "ret":
                eax_used = "ret"  # value handed back to the caller
                break
            r, w = regs_rw(q)
            idiom = q.mnemonic in ("xor", "sub") and q.op_str in ("eax, eax",)
            if "eax" in r and not idiom:
                eax_used = True
                break
            if "eax" in w:
                eax_used = False
                break
            if q.mnemonic == "call" or q.mnemonic.startswith("j"):
                break
            b += q.size
        top = []
        if sym is not None:
            for e in reversed(sym[-14:]):
                if e is None:
                    top.append("<local>")
                else:
                    pi = insn_at(e)
                    top.append("%06x:%s %s" % (e, pi.mnemonic, pi.op_str) if pi else "?")
        return {"site": a, "slot": slot, "via": via, "func": self.entry, "stack_top": top,
                "depth": None if sym is None else 4 * len(sym),
                "pushes": psc, "pend": pend, "cleanup": cleanup, "ecx_set": ecx_set, "edx_set": edx_set,
                "eax_used": eax_used, "bytes": ins.bytes.hex(),
                "next": (nins.mnemonic + " " + nins.op_str) if nins is not None else ""}

    def follow_uses(self, ins, slot):
        """after 'mov reg,[slot]' list uses of reg in the next instructions."""
        uses = []
        ops = ins.operands
        if ins.mnemonic != "mov" or ops[0].type != X86_OP_REG:
            return uses
        r = reg32(ops[0].reg)
        b = ins.address + ins.size
        for _ in range(16):
            q = insn_at(b)
            if q is None:
                break
            for k, op in enumerate(q.operands):
                if op.type == X86_OP_MEM and reg32(op.mem.base) == r:
                    acc = "w" if (k == 0 and q.mnemonic in ("mov", "and", "or", "add", "sub", "inc", "dec")) else "r"
                    uses.append({"ins": q.address, "op": q.mnemonic + " " + q.op_str,
                                 "disp": op.mem.disp, "size": op.size, "acc": acc})
                elif op.type == X86_OP_REG and reg32(op.reg) == r and q.mnemonic in ("push", "call", "mov", "cmp", "test", "add"):
                    uses.append({"ins": q.address, "op": q.mnemonic + " " + q.op_str, "disp": None,
                                 "size": op.size, "acc": "val"})
            rr, ww = regs_rw(q)
            if r in ww and not (q.mnemonic == "mov" and q.operands[0].type == X86_OP_MEM):
                break
            if q.mnemonic in ("ret", "jmp"):
                break
            b += q.size
        return uses


def linear_back(a, n):
    """decode up to n instructions that end exactly at a (best effort, via function start)."""
    f = F.containing(a)
    start = f[0] if f else max(a - 0x80, 0)
    if a - start > 0x400:
        start = a - 0x400
    out = []
    b = start
    while b < a:
        q = insn_at(b)
        if q is None:
            return out[-n:]
        out.append(q)
        b += q.size
    if b != a:
        return []
    return out[-n:]


def insn_at_prev_push(a, k):
    """imm of the k-th push before a (linear)."""
    prev = linear_back(a, 6)
    cnt = 0
    for p in reversed(prev):
        if p.mnemonic == "push":
            cnt += 1
            if cnt == k:
                return p.operands[0].imm if p.operands[0].type == X86_OP_IMM else None
    return None


# ---------------------------------------------------------------- main
def main():
    refs, rejected = raw_refs()
    stubs = find_stubs()
    print("refs", len(refs), "rejected", len(rejected), "stubs", len(stubs))
    funcs = set()
    unowned = []
    for r in refs:
        fe, fk = owner(r["ins"])
        if fe is None:
            unowned.append(r)
        else:
            funcs.add(fe)
        r["func"] = fe
        r["func_kind"] = fk
    stub_callers = collections.defaultdict(set)
    for name, va, vsz, raw, rsz in x.sections:
        if name not in CODE_SECTIONS:
            continue
        b = x.b[raw:raw + rsz]
        for i in range(len(b) - 4):
            if b[i] not in (0xE8, 0xE9):  # call rel32 / jmp rel32 (tail call)
                continue
            t = (va + i + 5 + struct.unpack_from("<i", b, i + 1)[0]) & 0xFFFFFFFF
            if t in stubs:
                fe, fk = owner(va + i)
                if fe is not None and va + i in reach(fe):
                    stub_callers[t].add(fe)
                    funcs.add(fe)
    funcs -= set(stubs)
    print("functions to simulate", len(funcs), "unowned refs", len(unowned))
    sims = {f: Sim(f, stubs) for f in funcs}

    def collect_all(kp, saves_by_func):
        sites, drs, viols, fsaves = [], [], {}, {}
        for f, sm in sims.items():
            v, st, dr = sm.run(kp, collect=True, saves=saves_by_func.get(f))
            sites += st
            drs += dr
            viols[f] = dict(v)
            fsaves[f] = sm.found_saves
        return sites, drs, viols, fsaves

    def estimate(sites):
        """purge estimate per slot from the call sites: cdecl if 'add esp' follows
        every call, else mode of the pending-argument count (pend), using only sites
        where pend == pushes-since-previous-call when such sites exist."""
        est = {}
        by = collections.defaultdict(list)
        for st in sites:
            if not st["via"].startswith("tailjmp"):
                by[st["slot"]].append(st)
        for slot, ss in by.items():
            if all(st["cleanup"] for st in ss):
                est[slot] = 0
                continue
            good = [st["pend"] for st in ss if st["pend"] is not None and st["pend"] == st["pushes"]]
            pool = good or [st["pend"] for st in ss if st["pend"] is not None] or [st["pushes"] for st in ss]
            est[slot] = 4 * collections.Counter(pool).most_common(1)[0][0]
        return est

    # pass A: heuristic purges -> save slots
    sites, _, _, saves = collect_all({}, {})
    # pass B: with saves -> argument estimates
    sites, _, _, saves2 = collect_all(estimate(sites), saves)
    for f in saves2:
        saves2[f] |= saves[f]
    est = estimate(sites)
    sites, _, _, saves3 = collect_all(est, saves2)
    for f in saves3:
        saves3[f] |= saves2[f]
    est = estimate(sites)
    init = dict(est)
    kpurge = dict(est)

    slot_funcs = collections.defaultdict(set)
    for st in sites:
        slot_funcs[st["slot"]].add(st["func"])
    cache = {}

    def fviol(f, items):
        key = (f, items)
        if key not in cache:
            v, _, _ = sims[f].run(dict(items), saves=saves3.get(f))
            cache[key] = sum(v[k] for k in ("join", "ret_depth", "tail_depth", "neg"))
        return cache[key]

    func_slots = collections.defaultdict(set)
    for st in sites:
        func_slots[st["func"]].add(st["slot"])

    def items_for(f, kp):
        return tuple(sorted((s_, kp[s_]) for s_ in func_slots[f] if s_ in kp))

    profiles = {}
    for rnd in range(4):
        changed = 0
        for slot in sorted(slot_funcs):
            if slot not in kpurge:
                continue
            prof = {}
            for c in sorted(set(range(0, 0x34, 4)) | {init[slot], kpurge[slot]}):
                kp = dict(kpurge)
                kp[slot] = c
                prof[c] = sum(fviol(f, items_for(f, kp)) for f in slot_funcs[slot])
            profiles[slot] = prof
            # only move away from the current value on a strict improvement
            best = min(prof.values())
            if prof[kpurge[slot]] > best:
                cands = sorted(c for c in prof if prof[c] == best)
                newc = min(cands, key=lambda c: abs(c - init[slot]))
                kpurge[slot] = newc
                changed += 1
        print("round", rnd, "changed", changed)
        if not changed:
            break
    sites, datarefs, final_viol, _ = collect_all(kpurge, saves3)
    # a site can be reached from two walks (fall-through into code Ghidra did not
    # split); keep the copy whose function is the owner of the site address
    best = {}
    for st in sites:
        k = (st["site"], st["via"].startswith("tailjmp"))
        own = owner(st["site"])[0]
        if k not in best or (st["func"] == own and best[k]["func"] != own):
            best[k] = st
    sites = sorted(best.values(), key=lambda st: st["site"])
    out = {
        "thunks": [{"slot": i, "va": va, "raw": raw, "ordinal": o, "name": XBOXKRNL_EXPORTS.get(o)}
                   for i, va, raw, o in thunks],
        "refs": refs, "rejected": rejected, "unowned": unowned,
        "stubs": {str(a): {"slot": v[0], "name": v[1], "callers": sorted(stub_callers.get(a, []))}
                  for a, v in stubs.items()},
        "sites": sites, "datarefs": datarefs,
        "purge_init": init, "purge": kpurge,
        "profiles": {str(k): v for k, v in profiles.items()},
        "final_viol": {str(k): v for k, v in final_viol.items()},
        "func_names": {str(f): func_name(f) for f in funcs},
        "func_kind": {str(f): ("ghidra" if f in FUNC_ENTRIES else "nofunc") for f in funcs},
    }
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "kernel_raw.json"), "w") as fh:
        json.dump(out, fh, indent=1, default=str)
    print("sites", len(sites), "datarefs", len(datarefs))


if __name__ == "__main__":
    main()
