"""Generate the xboxkrnl thunk table description and logging stubs.

Input:  analysis/loader/kernel_imports.tsv (111 rows, one per thunk slot; verified report kernel.md)
        milestone plan from docs/loader_design.md, Anhang A (encoded below, checked for completeness)
Output: src/host/kernel/gen/kernel_table.gen.h    constexpr export table + per-export trait structs
        src/host/kernel/gen/kernel_table.gen.cpp  one logging stub per FUNC slot (exact convention and
                                                  stack purge, DWORD parameters), placeholders for
                                                  DATA/ADDR slots

Usage:  python tools/gen/gen_kernel_table.py [--check]
"""
import csv
import re
import sys

from gen_common import ANALYSIS, SRC, c_string, comment_text, emit, header, parse_args

TSV = ANALYSIS / "loader" / "kernel_imports.tsv"
OUT_H = SRC / "host" / "kernel" / "gen" / "kernel_table.gen.h"
OUT_CPP = SRC / "host" / "kernel" / "gen" / "kernel_table.gen.cpp"

THUNK_TABLE_VA = 0x293560
SLOT_COUNT = 111

# docs/loader_design.md, Anhang A (Kernel-Implementierungsplan nach Meilenstein).
# All 11 DATA + 2 ADDR slots belong to M1 (design 1.5); slots 8 and 18 are also used only by
# replaced libraries, which is recorded separately in onlyReplacedLibs.
PLAN_FUNC = {
    "M1": [0, 1, 3, 6, 7, 9, 10, 14, 11, 12, 13, 15, 16, 17, 22, 23, 24, 25, 26, 27, 28, 29, 34, 35,
           36, 37, 41, 43, 48, 54, 55, 58, 59, 60, 72, 74, 76, 77, 84, 85, 86, 105, 106],
    "M2": [20, 21, 33, 38, 45, 46, 51, 52, 56, 107, 108, 109],
    "M7": [42, 44, 49, 50],
    "Defensive": [4, 5, 40, 78],
    "ReplacedLibs": [2, 19, 57, 61, 62, 63, 65, 66, 67, 68, 69, 70, 71, 79, 80, 81, 82, 83]
                    + list(range(88, 105)),
}


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def load_rows():
    with open(TSV, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    if len(rows) != SLOT_COUNT:
        die(f"expected {SLOT_COUNT} rows in {TSV}, got {len(rows)}")
    out = []
    for r in rows:
        slot = int(r["slot"])
        ordinal = int(r["ordinal"])
        name = r["name"].strip()
        kind = r["kind"].strip()
        conv = r["convention"].strip()
        args = r["args"].strip()
        purge = r["purge_bytes"].strip()
        thunk = int(r["thunk_va"], 16)
        if thunk != THUNK_TABLE_VA + 4 * slot:
            die(f"slot {slot}: thunk_va {thunk:#x} != table + 4*slot")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            die(f"slot {slot}: bad name {name!r}")
        e = dict(slot=slot, ordinal=ordinal, name=name, kind=kind, conv="None", arg_dwords=0,
                 reg_args=0, purge=0, variadic=False,
                 only_replaced=r["only_replaced_libs"].strip() == "ja",
                 plan=comment_text(r["host_plan"]), difficulty=r["difficulty"].strip(),
                 args_text=args)
        if kind == "FUNC":
            if conv == "stdcall":
                n = int(args)
                if int(purge) != 4 * n:
                    die(f"slot {slot} {name}: purge {purge} != 4*args {n}")
                e.update(conv="Stdcall", arg_dwords=n, purge=4 * n)
            elif conv == "fastcall":
                m = re.fullmatch(r"(\d) \((ecx|ecx,edx)\)", args)
                if not m or int(purge or 0) != 0:
                    die(f"slot {slot} {name}: unexpected fastcall args {args!r} / purge {purge!r}")
                n = int(m.group(1))
                e.update(conv="Fastcall", arg_dwords=n, reg_args=n, purge=0)
            elif conv == "cdecl":
                if not args.startswith("var"):
                    die(f"slot {slot} {name}: unexpected cdecl args {args!r}")
                # DbgPrint(Format, ...): one fixed DWORD, caller cleans up.
                e.update(conv="Cdecl", arg_dwords=1, purge=0, variadic=True)
            else:
                die(f"slot {slot} {name}: unknown convention {conv!r}")
        elif kind in ("DATA", "ADDR"):
            if conv or args or purge:
                die(f"slot {slot} {name}: DATA/ADDR row with convention/args")
        else:
            die(f"slot {slot} {name}: unknown kind {kind!r}")
        out.append(e)
    out.sort(key=lambda e: e["slot"])
    if [e["slot"] for e in out] != list(range(SLOT_COUNT)):
        die("slots are not exactly 0..110")
    if len({e["ordinal"] for e in out}) != SLOT_COUNT:
        die("duplicate ordinals")
    # milestone assignment (exactly one per slot)
    plan = {}
    for ms, slots in PLAN_FUNC.items():
        for s in slots:
            if s in plan:
                die(f"slot {s} assigned twice in PLAN_FUNC")
            plan[s] = ms
    for e in out:
        if e["kind"] != "FUNC":
            if e["slot"] in plan:
                die(f"slot {e['slot']} {e['name']} is DATA/ADDR but listed in PLAN_FUNC")
            e["milestone"] = "M1"
        else:
            if e["slot"] not in plan:
                die(f"FUNC slot {e['slot']} {e['name']} missing from PLAN_FUNC")
            e["milestone"] = plan[e["slot"]]
    if len(plan) != sum(1 for e in out if e["kind"] == "FUNC"):
        die("PLAN_FUNC contains slots that are not FUNC")
    return out


def gen_header(rows):
    L = [header("gen_kernel_table.py", ["analysis/loader/kernel_imports.tsv",
                                        "docs/loader_design.md (Anhang A milestone plan)"])]
    L.append("""//
// Thunk table of the retail EU XBE at 0x293560: 111 slots, each 0x80000000|ordinal, terminator at 0x29371C
// (kernel.md section 1). For every export this header provides:
//   - a row in krnl::gen::kExports (indexed by slot), and
//   - a trait struct krnl::exp::<Name> with constexpr slot/ordinal/kind/conv/purge used by the
//     registration macros in kernel/kernel_api.h (static_assert on convention and stack purge).
// purgeBytes = bytes the CALLEE removes from the stack (stdcall: 4*args, fastcall: 0, cdecl: 0).
#pragma once

#include <cstdint>

namespace krnl::gen {

enum class Kind : std::uint8_t { Func, Data, Addr };
enum class Conv : std::uint8_t { None, Stdcall, Fastcall, Cdecl };
// Implementation plan per docs/loader_design.md Anhang A.
enum class Milestone : std::uint8_t { M1, M2, M7, Defensive, ReplacedLibs };

struct ExportInfo {
    std::uint16_t slot;
    std::uint16_t ordinal;
    Kind kind;
    Conv conv;
    std::uint8_t argDwords;   // DWORD parameters of the generated stub
    std::uint8_t regArgs;     // fastcall register arguments (ecx, edx)
    std::uint16_t purgeBytes; // callee stack cleanup
    bool variadic;            // cdecl DbgPrint
    bool onlyReplacedLibs;    // all callers are in D3D/DSOUND/XMV/XPP (kernel.md section 6)
    Milestone milestone;
    const char* name;
};

inline constexpr std::uint32_t kSlotCount = %d;
inline constexpr std::uint32_t kThunkTableVa = 0x%08Xu;
inline constexpr std::uint32_t kThunkTableEnd = 0x%08Xu;  // terminator DWORD (0)
""" % (SLOT_COUNT, THUNK_TABLE_VA, THUNK_TABLE_VA + 4 * SLOT_COUNT))
    L.append("inline constexpr ExportInfo kExports[kSlotCount] = {")
    for e in rows:
        conv = f"Conv::{e['conv']},"
        L.append(f"    {{{e['slot']:3d}, {e['ordinal']:3d}, Kind::{e['kind'].capitalize()}, "
                 f"{conv:15s} {e['arg_dwords']:2d}, {e['reg_args']}, {e['purge']:2d}, "
                 f"{'true' if e['variadic'] else 'false'}, {'true' if e['only_replaced'] else 'false'}, "
                 f"Milestone::{e['milestone']}, {c_string(e['name'])}}},")
    L.append("};")
    L.append("")
    L.append("// Address of the generated stub (FUNC), zeroed placeholder buffer (DATA) or trap routine (ADDR).")
    L.append("std::uintptr_t StubAddress(std::uint32_t slot) noexcept;")
    L.append("")
    L.append("}  // namespace krnl::gen")
    L.append("")
    L.append("namespace krnl::exp {")
    L.append("")
    for e in rows:
        sig = ""
        if e["kind"] == "FUNC":
            sig = f" {e['conv'].lower()}, args {e['args_text']}, purge {e['purge']}"
        L.append(f"// slot {e['slot']}, Ord {e['ordinal']} (0x{e['ordinal']:03X}), {e['kind']}{sig}, "
                 f"plan {e['milestone']}{', only replaced libs' if e['only_replaced'] else ''}")
        L.append(f"struct {e['name']} {{")
        L.append(f"    static constexpr std::uint32_t slot = {e['slot']}, ordinal = {e['ordinal']};")
        L.append(f"    static constexpr gen::Kind kind = gen::Kind::{e['kind'].capitalize()};")
        L.append(f"    static constexpr gen::Conv conv = gen::Conv::{e['conv']};")
        L.append(f"    static constexpr std::uint32_t argDwords = {e['arg_dwords']}, regArgs = {e['reg_args']}, "
                 f"purgeBytes = {e['purge']};")
        L.append(f"    static constexpr const char* name = {c_string(e['name'])};")
        L.append("};")
    L.append("")
    L.append("}  // namespace krnl::exp")
    L.append("")
    return "\n".join(L)


def gen_cpp(rows):
    L = [header("gen_kernel_table.py", ["analysis/loader/kernel_imports.tsv"])]
    L.append("""//
// Logging stubs for every FUNC slot that has no registered implementation (design 1.5, 5.4).
// Each stub has exactly the guest calling convention and stack purge of its slot, takes DWORD
// parameters, logs "UNIMPLEMENTED Ord <n> <Name>(args) ret=<caller>" once per return address and
// returns STATUS_NOT_IMPLEMENTED (0xC0000002). DATA slots point to a zeroed, writable placeholder
// buffer; ADDR slots to a routine that aborts when called (its convention is unknown).
#include "kernel/kernel_api.h"

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace krnl::gen {
namespace {

using detail::ReportUnimplemented;
using detail::ReportAddrPlaceholderCall;
using detail::kDataPlaceholderSize;
""")
    for e in rows:
        n, s = e["name"], e["slot"]
        L.append(f"// slot {s}, Ord {e['ordinal']}: {e['plan']}" if e["plan"] else f"// slot {s}, Ord {e['ordinal']}")
        if e["kind"] == "FUNC":
            cc = {"Stdcall": "__stdcall", "Fastcall": "__fastcall", "Cdecl": "__cdecl"}[e["conv"]]
            k = e["arg_dwords"]
            params = ", ".join(f"std::uint32_t a{i + 1}" for i in range(k)) or "void"
            L.append(f"std::uint32_t {cc} Stub_{n}({params}) noexcept {{")
            if k:
                L.append(f"    const std::uint32_t args[{k}] = {{{', '.join(f'a{i + 1}' for i in range(k))}}};")
                L.append(f"    return ReportUnimplemented({s}, args, {k}, _ReturnAddress());")
            else:
                L.append(f"    return ReportUnimplemented({s}, nullptr, 0, _ReturnAddress());")
            L.append("}")
        elif e["kind"] == "DATA":
            L.append(f"alignas(16) unsigned char Placeholder_{n}[kDataPlaceholderSize];")
        else:
            L.append(f"void __cdecl AddrPlaceholder_{n}() noexcept {{ ReportAddrPlaceholderCall({s}, _ReturnAddress()); }}")
        L.append("")
    L.append("}  // namespace")
    L.append("")
    L.append("std::uintptr_t StubAddress(std::uint32_t slot) noexcept {")
    L.append("    switch (slot) {")
    for e in rows:
        n, s = e["name"], e["slot"]
        if e["kind"] == "FUNC":
            target = f"&Stub_{n}"
        elif e["kind"] == "DATA":
            target = f"&Placeholder_{n}[0]"
        else:
            target = f"&AddrPlaceholder_{n}"
        L.append(f"    case {s}: return reinterpret_cast<std::uintptr_t>({target});")
    L.append("    default: return 0;")
    L.append("    }")
    L.append("}")
    L.append("")
    L.append("}  // namespace krnl::gen")
    L.append("")
    return "\n".join(L)


def main():
    args = parse_args(__doc__)
    rows = load_rows()
    ok = emit(OUT_H, gen_header(rows), args.check)
    ok = emit(OUT_CPP, gen_cpp(rows), args.check) and ok
    kinds = {k: sum(1 for e in rows if e["kind"] == k) for k in ("FUNC", "DATA", "ADDR")}
    convs = {c: sum(1 for e in rows if e["conv"] == c) for c in ("Stdcall", "Fastcall", "Cdecl")}
    print(f"kernel table: {kinds} {convs}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
