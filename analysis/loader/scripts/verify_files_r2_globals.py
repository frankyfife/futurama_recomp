"""Every code reference to a global, decoded, with read/write classification (claims 2.4 / 1.2)."""
from verify_files_r2_common import *
for g in [int(a, 16) for a in sys.argv[1:]] or [0x30E598, 0x30EDD4, 0x2D7AE4, 0x2D7AE8, 0x2D7AF0, 0x2D7AF8, 0x30E68C, 0x2EDB0C, 0x2EDB88, 0x30E594]:
    print("== %08x section=%s" % (g, X.section(g)))
    for sec, va in raw_dword_refs(g, CODE_SECTIONS):
        ins = None
        cands = []
        for back in range(1, 8):
            c = list(md.disasm(X.read(va - back, 16), va - back, count=1))
            if c and c[0].address + c[0].size == va + 4 and ("0x%x" % g) in c[0].op_str:
                cands.append(c[0])
        # prefer a decode that also starts on an instruction boundary of the owning function
        f = owner(va)
        bounds = set()
        if f and not f[2].endswith("+out"):
            bounds = {i.address for i in dis(f[0], f[0] + f[1])}
        for c in cands:
            if c.address in bounds:
                ins = c; break
        if ins is None and cands:
            ins = sorted(cands, key=lambda c: -c.size)[0]
        if ins is None:
            print("   %08x ?" % va); continue
        ops = ins.op_str.split(", ")
        kind = "WRITE" if ins.mnemonic in ("mov", "inc", "dec", "add", "sub", "or", "and", "xor") and ("0x%x]" % g) in ops[0] else ("ADDR" if ins.mnemonic in ("push", "lea") or ("[0x%x]" % g) not in ins.op_str and "+ 0x%x]" % g not in ins.op_str else "READ")
        print("   %-5s %s   (%s)" % (kind, fmt(ins), owner(ins.address)[2]))
