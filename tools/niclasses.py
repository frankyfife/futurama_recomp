"""Recover NetImmerse class metadata from the Futurama XBE.

Walks the CRT static-initializer table and pattern-matches two kinds of stubs:
  * NiRTTI ctor:          push parent; push size; push "Name"; mov ecx, rtti; call NIRTTI_CTOR
  * stream registration:  mov eax, [pName]; push CreateObject; push eax; call REGISTER_LOADER
From CreateObject it follows the allocation to the constructor, and from the
constructor the vtable store. Emits a JSON summary plus a symbol TSV for
ghidra_scripts/ApplySymbols.java.

Usage: python tools/niclasses.py <default.xbe> <config> <out_prefix>
  config: retail_eu (addresses below are for that build)
"""
import json
import re
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from xbe import Xbe

CONFIGS = {
    "retail_eu": dict(
        xc_table=(0x2BDEF0, 0x2D5B30),
        nirtti_ctor=0x141E60,
        register_loader=0x1087D0,
        alloc=0x0A4900,
    ),
}

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True


def disasm(x, va, maxlen=0x400, stop_at_ret=True):
    code = x.read(va, maxlen)
    out = []
    for ins in md.disasm(code, va):
        out.append(ins)
        if stop_at_ret and ins.mnemonic == "ret":
            break
    return out


def call_target(ins):
    if ins.mnemonic == "call" and ins.operands[0].type == X86_OP_IMM:
        return ins.operands[0].imm
    return None


def parse_stub(x, cfg, va):
    ins = disasm(x, va, 0x40)
    pushes, ecx, eax_mem = [], None, None
    for i in ins:
        if i.mnemonic == "push" and i.operands[0].type == X86_OP_IMM:
            pushes.append(i.operands[0].imm & 0xFFFFFFFF)
        elif i.mnemonic == "push" and i.operands[0].type == X86_OP_REG:
            pushes.append(("reg", i.reg_name(i.operands[0].reg)))
        elif i.mnemonic == "mov" and len(i.operands) == 2:
            dst, src = i.operands
            if dst.type == X86_OP_REG and i.reg_name(dst.reg) == "ecx" and src.type == X86_OP_IMM:
                ecx = src.imm & 0xFFFFFFFF
            if dst.type == X86_OP_REG and i.reg_name(dst.reg) == "eax" and src.type == X86_OP_MEM \
                    and src.mem.base == 0 and src.mem.index == 0:
                eax_mem = src.mem.disp & 0xFFFFFFFF
        t = call_target(i)
        if t == cfg["nirtti_ctor"] and len(pushes) >= 3 and ecx:
            parent, size, name = pushes[-3], pushes[-2], pushes[-1]
            return dict(kind="rtti", rtti=ecx, name=x.cstr(name), size=size,
                        parent=parent if isinstance(parent, int) else None)
        if t == cfg["register_loader"] and len(pushes) >= 2 and eax_mem:
            return dict(kind="loader", name=x.cstr(x.u32(eax_mem)), create=pushes[-2])
        if t is not None:
            break
    return dict(kind="other")


def find_ctor(x, cfg, create_fn):
    """CreateObject: p = alloc(size); if (p) p = Ctor(p); -> (size, ctor)."""
    ins = disasm(x, create_fn, 0x100)
    size, seen_alloc, last_push = None, False, None
    for i in ins:
        if i.mnemonic == "push" and i.operands[0].type == X86_OP_IMM:
            last_push = i.operands[0].imm
        t = call_target(i)
        if t is None:
            continue
        if t == cfg["alloc"]:
            size, seen_alloc = last_push, True
        elif seen_alloc:
            return size, t
    return size, None


def stored_vtables(x, fn):
    """Immediates stored via 'mov dword [reg], imm' (offset 0) that point into .rdata."""
    out = []
    for i in disasm(x, fn, 0x600):
        if i.mnemonic == "mov" and len(i.operands) == 2:
            dst, src = i.operands
            if dst.type == X86_OP_MEM and dst.mem.disp == 0 and dst.mem.index == 0 and dst.mem.base != 0 \
                    and src.type == X86_OP_IMM and x.section(src.imm & 0xFFFFFFFF) == ".rdata":
                out.append(src.imm & 0xFFFFFFFF)
    return out


def vtable_store_targets(x):
    """Immediates of 'mov dword [reg], imm32' that point into .rdata at a code pointer.

    Constructors/destructors store vtables this way, so these are vtable starts.
    """
    text = next(t for t in x.sections if t[0] == ".text")
    tb = x.b[text[3]:text[3] + text[4]]
    out = set()
    for m in re.finditer(rb"\xC7[\x00\x01\x02\x03\x06\x07](....)", tb, re.S):
        imm = struct.unpack("<I", m.group(1))[0]
        if x.section(imm) == ".rdata" and x.section(x.u32(imm)) == ".text":
            out.add(imm)
    return out


def rdata_refs(x, value):
    rd = next(t for t in x.sections if t[0] == ".rdata")
    rb = x.b[rd[3]:rd[3] + rd[4]]
    pat, out = struct.pack("<I", value), []
    o = rb.find(pat)
    while o != -1:
        if o % 4 == 0:
            out.append(rd[1] + o)
        o = rb.find(pat, o + 1)
    return out


def vtable_len(x, vt, starts):
    n = 0
    while True:
        a = vt + 4 * n
        if n and a in starts:
            break
        if x.section(x.u32(a)) != ".text":
            break
        n += 1
    return n


def main(xbe_path, config, out_prefix):
    x = Xbe(xbe_path)
    cfg = CONFIGS[config]
    a, e = cfg["xc_table"]
    rtti, loaders, other = {}, {}, 0
    tu_blocks, current = [], []
    for slot in range(a, e, 4):
        fn = x.u32(slot)
        if fn in (0, 0xFFFFFFFF):
            continue
        s = parse_stub(x, cfg, fn)
        if s["kind"] == "rtti":
            rtti[s["rtti"]] = s
        elif s["kind"] == "loader":
            if s["name"] in {l["name"] for l in current}:
                tu_blocks.append(current)
                current = []
            current.append(s)
            loaders.setdefault(s["name"], s["create"])
        else:
            other += 1
    if current:
        tu_blocks.append(current)

    by_name = {r["name"]: r for r in rtti.values()}
    classes = {}
    for name in sorted(set(by_name) | set(loaders)):
        c = dict(name=name)
        r = by_name.get(name)
        if r:
            c.update(rtti=r["rtti"], size=r["size"],
                     parent=rtti[r["parent"]]["name"] if r["parent"] in rtti else None)
        if name in loaders:
            c["create"] = loaders[name]
            alloc_size, ctor = find_ctor(x, cfg, loaders[name])
            c["alloc_size"], c["ctor"] = alloc_size, ctor
        classes[name] = c

    # GetRTTI implementations: "mov eax, offset rtti; ret"
    text = next(s for s in x.sections if s[0] == ".text")
    tb = x.b[text[3]:text[3] + text[4]]
    getrtti = {}
    for r in rtti:
        pat = b"\xB8" + struct.pack("<I", r) + b"\xC3"
        o = tb.find(pat)
        while o != -1:
            getrtti.setdefault(r, []).append(text[1] + o)
            o = tb.find(pat, o + 1)
    stores = vtable_store_targets(x)
    for c in classes.values():
        if c.get("rtti") in getrtti:
            c["getrtti"] = getrtti[c["rtti"]]
    # Vtable = table whose slot 1 is the class's own GetRTTI.
    cands = {}
    for c in classes.values():
        cands[c["name"]] = sorted({r - 4 for g in c.get("getrtti", []) for r in rdata_refs(x, g)})
    starts = set(stores) | {v for vs in cands.values() for v in vs}
    usage = {}
    for vt in starts:
        for i in range(vtable_len(x, vt, starts)):
            fn = x.u32(vt + 4 * i)
            usage[fn] = usage.get(fn, 0) + 1
    for c in classes.values():
        vs = cands[c["name"]]
        if not vs:
            continue
        # Several tables share a GetRTTI when a subclass does not override it; the
        # base table is the one whose methods are inherited most widely.
        score = lambda vt: sum(usage.get(x.u32(vt + 4 * i), 0) for i in range(vtable_len(x, vt, starts)))
        c["vtable"] = max(vs, key=score)
        c["vtable_len"] = vtable_len(x, c["vtable"], starts)
        if len(vs) > 1:
            c["vtable_alternatives"] = [v for v in vs if v != c["vtable"]]

    # A constructor belongs to the class whose vtable it stores. When a derived ctor is
    # inlined into CreateObject, the call found there is the *base* ctor instead.
    vt2cls = {c["vtable"]: c["name"] for c in classes.values() if c.get("vtable")}
    candidates = {c.pop("ctor") for c in classes.values() if c.get("ctor")}
    for fn in sorted(candidates):
        owners = [vt2cls[v] for v in stored_vtables(x, fn) if v in vt2cls]
        if owners and not classes[owners[-1]].get("ctor"):
            classes[owners[-1]]["ctor"] = fn
    for c in classes.values():
        if c.get("create") and not c.get("ctor"):
            c["ctor_inlined"] = True

    # Symbols for Ghidra. Virtual slots are named after the class that first introduces
    # the implementation (walking parents), e.g. NiNode::vf12.
    syms = []
    stats = {}
    def add(addr, kind, name):
        if addr:
            syms.append((addr, kind, name))
    for c in classes.values():
        n = c["name"]
        add(c.get("rtti"), "label", f"{n}::ms_RTTI")
        add(c.get("create"), "func", f"{n}::CreateObject")
        add(c.get("ctor"), "func", f"{n}::{n}")
        add(c.get("vtable"), f"vtable:{c.get('vtable_len', 0)}", f"{n}::vftable")
        for g in c.get("getrtti", []):
            add(g, "func", f"{n}::GetRTTI")

    def ancestors(name):
        out = []
        while name in classes:
            out.append(name)
            name = classes[name].get("parent")
        return out

    users = {}
    for c in classes.values():
        vt = c.get("vtable")
        for i in range(c.get("vtable_len", 0)):
            users.setdefault((x.u32(vt + 4 * i), i), []).append(c["name"])
    getrtti_addrs = {g for c in classes.values() for g in c.get("getrtti", [])}
    ctor_addrs = {c.get("ctor") for c in classes.values()}
    named = set()
    for (fn, i), us in sorted(users.items()):
        if fn in getrtti_addrs or fn in ctor_addrs or fn in named:
            continue
        common = set(ancestors(us[0]))
        for u in us[1:]:
            common &= set(ancestors(u))
        lca = next((a for a in ancestors(us[0]) if a in common), None)
        if lca and lca in us:  # owner really has this implementation in its own vtable
            add(fn, "func", f"{lca}::vf{i:02d}")
            named.add(fn)
    stats["vslots_named"] = len(named)

    with open(out_prefix + "_classes.json", "w") as f:
        json.dump(dict(classes=classes, tu_count=len(tu_blocks),
                       tu_sizes=[len(b) for b in tu_blocks], other_stubs=other), f, indent=1)
    with open(out_prefix + "_symbols.tsv", "w") as f:
        for c in classes.values():
            if c.get("size"):
                f.write(f"00000000\tclass:{c['size']}:{c.get('parent') or ''}\t{c['name']}\n")
        for addr, kind, name in sorted(set(syms)):
            f.write(f"{addr:08x}\t{kind}\t{name}\n")

    print(f"rtti={len(rtti)} loader_classes={len(loaders)} classes={len(classes)} "
          f"tu_blocks={len(tu_blocks)} other_stubs={other} symbols={len(set(syms))}")
    print("virtual slots named:", stats["vslots_named"])
    print("no vtable found:", sorted(n for n, c in classes.items() if not c.get("vtable")))
    print("ambiguous (picked base by usage):", {n: hex(c["vtable"]) for n, c in classes.items() if c.get("vtable_alternatives")})
    print("rtti without loader:", sorted(n for n, c in classes.items() if "create" not in c))


if __name__ == "__main__":
    main(*sys.argv[1:4])
