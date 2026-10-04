"""Reviewer check: individual claims of kernel.md, each printed with its instruction evidence.
Usage: python verify_kernel_facts.py > verify_kernel_facts.txt
"""
import csv
import json
import os
import re
import struct
from collections import Counter, defaultdict

from capstone.x86 import X86_OP_MEM, X86_REG_FS, X86_REG_INVALID

from verify_kernel_common import (CODE_SECTIONS, LOADER, XBE, Funcs, callgraph, disasm_range, fmt,
                                  hdr_u32, insn, md, module_of, u32)

HERE = os.path.dirname(os.path.abspath(__file__))
F = Funcs()
CG = callgraph()
v = json.load(open(os.path.join(HERE, "verify_kernel_sites.json")))
SITES = defaultdict(list)
for k, l in v["sites"].items():
    for kind, a in l:
        SITES[int(k)].append(int(a, 16))
for s, a in [(1, 0x292d4c), (3, 0xf792a), (3, 0xf9bc0), (7, 0x291a65), (26, 0xf7b3f), (41, 0xf9278),
             (41, 0xf950c), (93, 0x240735), (93, 0x240797), (104, 0x24ae48)]:
    SITES[s].append(a)
TSV = {int(r["slot"]): r for r in csv.DictReader(open(os.path.join(LOADER, "kernel_imports.tsv"), encoding="utf-8"), delimiter="\t")}
BYNAME = {r["name"]: s for s, r in TSV.items()}


def hdr(t):
    print("\n==== " + t)


def back_pushes(site, n):
    """Return the last n push instructions (closest first) before site, linear within the function."""
    fr = F.containing(site)
    start = fr[0] if fr and site - fr[0] < 0x600 else site - 0x200
    for d in range(16):
        ins = list(md.disasm(XBE.read(start + d, site - start - d + 16), start + d))
        idx = [k for k, i in enumerate(ins) if i.address == site]
        if idx:
            break
    else:
        return []
    out = []
    for i in reversed(ins[:idx[0]]):
        if i.mnemonic in ("call", "ret", "jmp") or i.mnemonic.startswith("ret"):
            break
        if i.mnemonic == "push":
            out.append(i)
            if len(out) == n:
                break
    return out


def args_at(name, nargs):
    s = BYNAME[name]
    res = []
    for a in sorted(SITES[s]):
        p = back_pushes(a, nargs)
        res.append((a, [x.op_str for x in p]))
    return res


# ---- fact 14 + extra: argument values
hdr("ExQueryNonVolatileSetting: 1st argument (index) per site")
for a, p in args_at("ExQueryNonVolatileSetting", 5):
    print("  %06x %-40s args=%s" % (a, F.name_or(a), p))

hdr("NtAllocateVirtualMemory: 5th argument (Protect)")
for a, p in args_at("NtAllocateVirtualMemory", 5):
    print("  %06x %-32s protect=%s  all=%s" % (a, F.name_or(a), p[4] if len(p) > 4 else "?", p))

hdr("MmAllocateContiguousMemoryEx: args (Size, Lowest, Highest, Alignment, Protect)")
for a, p in args_at("MmAllocateContiguousMemoryEx", 5):
    print("  %06x %-6s %-36s %s" % (a, module_of(a), F.name_or(a), p))

for nm, n in (("HalReturnToFirmware", 1), ("KeBugCheck", 1), ("HalGetInterruptVector", 2),
              ("AvSendTVEncoderOption", 4), ("NtQueryVolumeInformationFile", 5), ("NtQueryInformationFile", 5),
              ("NtSetInformationFile", 5), ("NtQueryDirectoryFile", 10), ("NtFsControlFile", 10),
              ("NtDeviceIoControlFile", 10), ("KeSetTimer", 4), ("MmSetAddressProtect", 3),
              ("HalRegisterShutdownNotification", 2), ("MmPersistContiguousMemory", 3),
              ("NtWaitForSingleObjectEx", 4), ("KeDelayExecutionThread", 3), ("NtWaitForSingleObject", 3),
              ("MmCreateKernelStack", 2), ("KeWaitForSingleObject", 5)):
    hdr("%s: pushed args (arg1 first)" % nm)
    for a, p in args_at(nm, n):
        print("  %06x %-6s %-40s %s" % (a, module_of(a), F.name_or(a)[:40], p))

# ---- fact 15: rdtsc + 733 MHz constant
hdr("QueryPerformanceCounter/Frequency, rdtsc and 0x2BB5C755")
for i in disasm_range(0xF641B, 0x20):
    print("  " + fmt(i))
const = struct.pack("<I", 0x2BB5C755)
for name, va, vsz, raw, rsz in XBE.sections:
    d = XBE.b[raw:raw + rsz]
    o = d.find(const)
    while o >= 0:
        print("  constant 0x2BB5C755 at %06x (%s)" % (va + o, name))
        o = d.find(const, o + 1)
rd = []
for name, va, vsz, raw, rsz in XBE.sections:
    if name not in CODE_SECTIONS:
        continue
    d = XBE.b[raw:raw + rsz]
    o = d.find(b"\x0f\x31")
    while o >= 0:
        a = va + o
        fr = F.containing(a)
        ok = False
        if fr:
            ok = any(i.address == a and i.mnemonic == "rdtsc" for i in md.disasm(XBE.read(fr[0], fr[1]), fr[0]))
        rd.append((a, name, F.name_or(a), ok))
        o = d.find(b"\x0f\x31", o + 1)
print("  0F 31 byte pairs in code:", len(rd))
for r in rd:
    if r[3]:
        print("  rdtsc (confirmed by linear sweep of Ghidra func) %06x %s %s" % (r[0], r[1], r[2]))
for r in rd:
    if not r[3] and r[2] == "<no func>":
        i = insn(r[0])
        print("  0F31 outside Ghidra func %06x %s: %s" % (r[0], r[1], fmt(i) if i else None))

# ---- fact 11: fs usage
hdr("fs: accesses (byte-scan for 0x64 prefix, decode, only [disp] without base/index)")
fs = defaultdict(list)
for name, va, vsz, raw, rsz in XBE.sections:
    if name not in CODE_SECTIONS:
        continue
    d = XBE.b[raw:raw + rsz]
    o = d.find(b"\x64")
    while o >= 0:
        a = va + o
        i = insn(a)
        o = d.find(b"\x64", o + 1)
        if not i or i.address != a:
            continue
        for op in i.operands:
            if op.type == X86_OP_MEM and op.mem.segment == X86_REG_FS and op.mem.base == X86_REG_INVALID and op.mem.index == X86_REG_INVALID:
                fr = F.containing(a)
                bound = None
                if fr:
                    bound = any(x.address == a for x in md.disasm(XBE.read(fr[0], fr[1]), fr[0]))
                fs[op.mem.disp].append((a, name, fmt(i), F.name_or(a), bound))
                break
for disp in sorted(fs):
    l = fs[disp]
    good = [x for x in l if x[4] is not False]
    print("fs:[0x%x]: %d hits (%d excluding those not on a linear-sweep boundary of their Ghidra function); sections %s"
          % (disp, len(l), len(good), dict(Counter(x[1] for x in good))))
    if disp != 0:
        for x in good:
            print("    %s  [%s]" % (x[2], x[3]))

# follow-up offsets after fs:[0x20]/fs:[0x28] loads (which KPCR/KTHREAD fields are needed)
hdr("dereference offsets following 'mov reg, fs:[0x20|0x28]'")
for disp in (0x20, 0x28):
    offs = Counter()
    for x in fs.get(disp, []):
        if x[4] is False:
            continue
        i = insn(x[0])
        if i.mnemonic != "mov":
            continue
        reg = i.op_str.split(",")[0]
        a = x[0] + i.size
        for _ in range(8):
            j = insn(a)
            if j is None:
                break
            m = re.search(r"\[%s(?: \+ (0x[0-9a-f]+|\d+))?\]" % reg, j.op_str)
            if m:
                offs[int(m.group(1), 0) if m.group(1) else 0] += 1
                print("    fs:[0x%x] -> %s   (%s)" % (disp, fmt(j), F.name_or(j.address)))
            if j.op_str.startswith(reg + ",") and j.mnemonic in ("mov", "lea", "xor", "movzx"):
                if not m:
                    break
                break
            a += j.size
    print("  fs:[0x%x] offsets: %s" % (disp, {hex(k): c for k, c in offs.items()}))

# ---- fact 12/13: callers in the callgraph
hdr("callgraph: callers of ReadFileEx/WriteFileEx/CreateFiber/SwitchToFiber/ConvertThreadToFiber/DeleteFiber")
for r in CG:
    if any(k in r["callee_name"] for k in ("ReadFileEx", "WriteFileEx", "Fiber")):
        print("  %s %-40s -> %s" % (r["caller"], r["caller_name"], r["callee_name"]))

# ---- fact 17: indexed accesses
hdr("indexed accesses [esi*4+0x293608] with bounds check")
for a in (0x14791D, 0x235831):
    ins = disasm_range(a - 0x12, 0x20)
    for i in ins:
        if a - 0x12 <= i.address <= a:
            print("  " + fmt(i))
    print("  in", F.name_or(a))
    # what table is at 0x293608 - is it used as a table of 0x88 entries? the slot range ends at 0x29371c
    print("  0x293608 + 0x88*4 = %06x (thunk table ends at 0x29371c)" % (0x293608 + 0x88 * 4))

# ---- extra: data exports
hdr("data export accesses")
for a in (0x28DA06, 0x0F7004, 0x0F70C6, 0x0F70CE, 0x0F7DD4, 0x0F7E2B, 0x0F81BE, 0x0F9459, 0x0F945E, 0x0F933D, 0x0F9342,
          0x23C846, 0x23C84C, 0x28C8F6, 0x28C8FB, 0x0F90BE, 0x170AA5, 0x28D254, 0x0F9C63, 0x0F9C80):
    i = insn(a)
    print("  %s   [%s]" % (fmt(i), F.name_or(a)))
hdr("XboxHardwareInfo: all uses (mov reg,[0x293660] then next 3 insns)")
for r in v["refs"]:
    if r["slot"] == 64:
        a = int(r["va"], 16)
        seq = disasm_range(a, 0x14)[:3]
        print("  " + " | ".join(fmt(i)[7:].strip() for i in seq), " [%s %s]" % (module_of(a), F.name_or(a)))
hdr("IoInvalidDeviceRequest stores in FUN_0028d254")
for i in disasm_range(0x28D254, 0x50):
    if "0x28c0" in i.op_str or "0x293580" in i.op_str:
        print("  " + fmt(i))
hdr("HalRegisterShutdownNotification record 0x2D7530")
print("  dwords:", ["%08x" % u32(0x2D7530 + 4 * k) for k in range(4)])
hdr("LaunchDataPage uses")
for r in v["refs"]:
    if r["slot"] == 53:
        a = int(r["va"], 16)
        seq = disasm_range(a, 0x10)[:2]
        print("  " + " | ".join(fmt(i)[7:].strip() for i in seq), " [%s]" % F.name_or(a))

# ---- register use for call-reg sites
hdr("registers used for 'call reg' sites (from kernel_raw.json of the analysed agent)")
raw = json.load(open(os.path.join(HERE, "kernel_raw.json")))
print(" ", Counter(s["via"] for s in raw["sites"] if s["via"].startswith("reg")))

# ---- header fields read by code
hdr("XBE header fields read via absolute address 0x10000..0x10200")
cnt = Counter()
for name, va, vsz, raw_, rsz in XBE.sections:
    if name not in CODE_SECTIONS:
        continue
    d = XBE.b[raw_:raw_ + rsz]
    for o in range(len(d) - 3):
        val = struct.unpack_from("<I", d, o)[0]
        if 0x10100 <= val < 0x10200:
            for k in range(1, 4):
                i = insn(va + o - k)
                if i and i.address + i.size >= va + o + 4 and any(op.type == X86_OP_MEM and (op.mem.disp & 0xffffffff) == val and op.mem.base == X86_REG_INVALID for op in i.operands):
                    fr = F.containing(i.address)
                    print("  %s  [%s %s]" % (fmt(i), module_of(i.address), F.name_or(i.address)))
                    cnt[val] += 1
                    break
print("  fields:", {hex(k): c for k, c in cnt.items()})

# ---- kernel-space constants (dynamic export lookup?)
hdr("imm32 constants 0x80010000..0x8001FFFF (kernel image header) in code")
for name, va, vsz, raw_, rsz in XBE.sections:
    if name not in CODE_SECTIONS:
        continue
    d = XBE.b[raw_:raw_ + rsz]
    for o in range(len(d) - 3):
        val = struct.unpack_from("<I", d, o)[0]
        if 0x80010000 <= val < 0x80020000:
            for k in range(1, 7):
                i = insn(va + o - k)
                if i and i.address + i.size == va + o + 4:
                    print("  %s  [%s %s]" % (fmt(i), module_of(i.address), F.name_or(i.address)))
                    break

# ---- XAPI GetTickCount etc.
hdr("functions named *TickCount* / *SystemTime* in functions.tsv")
for a, sz, blk, nm in F.rows:
    if "TickCount" in nm or "SystemTime" in nm or "FileTime" in nm:
        print("  %06x %s" % (a, nm))
