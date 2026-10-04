"""cpu_rd.py - simple recursive-descent disassembler for retail_eu default.xbe.

Seeds: every Ghidra function entry (functions.tsv), the XBE entry point, plus
(iteratively) code pointers found in data or as instruction immediates that point
to an address directly after padding (0x90/0xCC) or after a ret/jmp - the usual
"function only reached through a vtable / callback" case Ghidra missed.

Follows: fall-through, direct jmp/jcc/call/loop targets, switch jump tables of the
form  jmp dword ptr [reg*4 + table].

API:  rd = RD(); rd.run();  rd.insn  -> {va: (size, mnemonic, op_str, bytes)}
      rd.seed_src -> {va: "ghidra"|"entry"|"dataptr"|"immptr"}
"""
import sys, struct, bisect
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM
from capstone.x86 import X86_REG_INVALID

XBE = r"D:\FuturamaDecomp\orig\retail_eu\default.xbe"
FUNCS = r"D:\FuturamaDecomp\analysis\retail_eu_export\functions.tsv"
CODE_SECS = (".text", "D3D", "DSOUND", "XMV", "XPP")
DATA_SECS = (".rdata", ".data")

COND = {"je", "jne", "jz", "jnz", "ja", "jae", "jb", "jbe", "jg", "jge", "jl", "jle", "jo", "jno",
        "js", "jns", "jp", "jnp", "jcxz", "jecxz", "loop", "loope", "loopne"}
STOP = {"ret", "retf", "jmp", "iretd", "hlt", "ud2", "int3"}


class RD:
    def __init__(self):
        self.x = Xbe(XBE)
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.ranges = []
        for (n, va, vsz, raw, rsz) in self.x.sections:
            if n in CODE_SECS:
                self.ranges.append((va, va + rsz, raw, n))
        self.insn = {}
        self.seed_src = {}
        self.jumptables = {}   # table va -> list of targets
        self.funcs = []
        for l in open(FUNCS, encoding="utf-8"):
            p = l.rstrip("\n").split("\t")
            if p[0] == "address":
                continue
            self.funcs.append((int(p[0], 16), int(p[1]), p[3]))
        self.funcs.sort()

    def in_code(self, va):
        for lo, hi, raw, n in self.ranges:
            if lo <= va < hi:
                return True
        return False

    def bytes_at(self, va, n=16):
        for lo, hi, raw, nm in self.ranges:
            if lo <= va < hi:
                o = raw + va - lo
                return self.x.b[o:o + min(n, hi - va)]
        return None

    def decode(self, va):
        b = self.bytes_at(va, 16)
        if not b:
            return None
        for i in self.md.disasm(b, va, 1):
            return i
        return None

    def walk(self, seeds):
        work = list(seeds)
        while work:
            va = work.pop()
            while True:
                if va in self.insn or not self.in_code(va):
                    break
                i = self.decode(va)
                if i is None:
                    break
                mn = i.mnemonic
                self.insn[va] = (i.size, mn, i.op_str, bytes(i.bytes))
                ops = i.operands
                if mn in COND or mn in ("jmp", "call"):
                    if ops and ops[0].type == CS_OP_IMM:
                        t = ops[0].imm & 0xFFFFFFFF
                        if self.in_code(t):
                            work.append(t)
                    elif mn == "jmp" and ops and ops[0].type == CS_OP_MEM:
                        m = ops[0].mem
                        if m.base == X86_REG_INVALID and m.index != X86_REG_INVALID and m.scale == 4 \
                                and self.in_code(m.disp & 0xFFFFFFFF):
                            tbl = m.disp & 0xFFFFFFFF
                            tg = []
                            p = tbl
                            while True:
                                bb = self.bytes_at(p, 4)
                                if not bb or len(bb) < 4:
                                    break
                                t = struct.unpack("<I", bb)[0]
                                if not self.in_code(t) or abs(t - va) > 0x10000 or p in self.insn:
                                    break
                                tg.append(t)
                                p += 4
                            self.jumptables[tbl] = tg
                            work.extend(tg)
                if mn in STOP:
                    break
                va += i.size

    def data_ptr_seeds(self):
        """code pointers in .rdata/.data/code-imms that land after padding/ret"""
        cands = set()
        for (n, va, vsz, raw, rsz) in self.x.sections:
            if n in DATA_SECS:
                b = self.x.b[raw:raw + rsz]
                for o in range(0, len(b) - 3, 4):
                    v = struct.unpack_from("<I", b, o)[0]
                    if self.in_code(v):
                        cands.add((v, "dataptr"))
        for va, (sz, mn, ops, bb) in list(self.insn.items()):
            if mn in ("push", "mov") and "0x" in ops:
                # last operand immediate
                tok = ops.split(",")[-1].strip()
                if tok.startswith("0x"):
                    try:
                        v = int(tok, 16)
                    except ValueError:
                        continue
                    if self.in_code(v):
                        cands.add((v, "immptr"))
        out = []
        for v, src in cands:
            if v in self.insn:
                continue
            prev = self.bytes_at(v - 1, 1)
            prev3 = self.bytes_at(v - 3, 3)
            ok = False
            if prev and prev[0] in (0x90, 0xCC, 0xC3):
                ok = True
            elif prev3 and prev3[0] == 0xC2:   # ret imm16
                ok = True
            # also accept if previous byte ends a known instruction that stops flow
            if ok:
                # reject pointers to strings embedded in code sections (e.g. XPP 0x29352c
                # "\Device\MU_0"): >= 4 printable ASCII bytes at the target
                head = self.bytes_at(v, 64) or b""
                z = head.find(bytes([0]))
                if z >= 4 and all(0x20 <= c < 0x7f for c in head[:z]):
                    continue
                i = self.decode(v)
                if i is not None:
                    out.append((v, src))
        return out

    def run(self, rounds=4):
        seeds = [f[0] for f in self.funcs if self.in_code(f[0])]
        for s in seeds:
            self.seed_src[s] = "ghidra"
        ent = 0xf84dd
        self.seed_src.setdefault(ent, "entry")
        self.walk(seeds + [ent])
        for r in range(rounds):
            extra = self.data_ptr_seeds()
            extra = [(v, s) for v, s in extra if v not in self.insn]
            if not extra:
                break
            for v, s in extra:
                self.seed_src.setdefault(v, s)
            self.walk([v for v, s in extra])
        return self


if __name__ == "__main__":
    rd = RD().run()
    import collections
    print("instructions:", len(rd.insn))
    print("seeds:", collections.Counter(rd.seed_src.values()))
    cov = collections.Counter()
    for va, (sz, *_r) in rd.insn.items():
        cov[rd.x.section(va)] += sz
    for lo, hi, raw, n in rd.ranges:
        print("  %-7s covered %7d / %7d bytes" % (n, cov[n], hi - lo))
