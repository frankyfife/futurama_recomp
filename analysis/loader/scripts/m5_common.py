"""Shared helpers of the M5 audio analysis (analysis/loader/m5_audio.md).

Read-only access to the retail EU XBE (capstone), the Ghidra export (functions.tsv, callgraph.tsv,
strings.tsv, decomp/*.c) and the DSOUND API table (avi_api_surface.tsv). Small summaries go to
analysis/loader/scripts/out/m5_*.out, large listings and anything derived from game data to
run/analyse_m5/ (git-ignored).
"""
import bisect
import csv
import os
import pickle
import re
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe  # noqa: E402
import capstone  # noqa: E402
from capstone import x86 as cx86  # noqa: E402

ROOT = r"D:\FuturamaDecomp"
EXPORT = os.path.join(ROOT, r"analysis\retail_eu_export")
LOADER = os.path.join(ROOT, r"analysis\loader")
OUT = os.path.join(LOADER, r"scripts\out")
RUN = os.path.join(ROOT, r"run\analyse_m5")
X = Xbe(os.path.join(ROOT, r"orig\retail_eu\default.xbe"))

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = True

SECT = {n: (va, vsz, raw, rsz) for n, va, vsz, raw, rsz in X.sections}
CODE_SECTIONS = (".text", "D3D", "DSOUND", "XMV", "XPP")


def sect_range(name):
    va, vsz, raw, rsz = SECT[name]
    return va, va + vsz


# ---------------------------------------------------------------------------- names
def _load_funcs():
    funcs = {}
    with open(os.path.join(EXPORT, "functions.tsv"), encoding="utf-8") as f:
        r = csv.reader(f, delimiter="\t")
        next(r)
        for row in r:
            funcs[int(row[0], 16)] = (row[3], int(row[1]))
    return funcs


FUNCS = _load_funcs()
FSTARTS = sorted(FUNCS)


def _load_api():
    api = {}
    with open(os.path.join(LOADER, "avi_api_surface.tsv"), encoding="utf-8") as f:
        r = csv.reader(f, delimiter="\t")
        next(r)
        for row in r:
            if row[0].startswith("vtbl"):
                continue
            api[int(row[0], 16)] = row[2].split("::")[-1]
    return api


API = _load_api()

# Names given in this analysis (avi.md 2.4.1 plus new ones). Methods that Ghidra did not recognise are
# included with their vtable slot so that listings read like source.
NAMES = {
    # SoundManager (vtable 0x2a3b80, base 0x2a3978)
    0xccd00: "SoundMgr::ctor_Init",
    0xcbc30: "SoundMgr::dtor",
    0xcce20: "SoundMgr::vf0_deldtor",
    0xcbcb0: "SoundMgr::vf1_Update",
    0xc9630: "SoundMgr::vf3",
    0xccee0: "SoundMgr::vf4_GetSound",
    0xcce40: "SoundMgr::vf5_CreateFromFileObject",
    0xca3f0: "SoundMgrBase::vf6",
    0xca410: "SoundMgrBase::vf7",
    0xca500: "SoundMgrBase::vf8",
    0xc9650: "SoundMgrBase::vf9_GetCategoryVolume",
    0xca420: "SoundMgrBase::vf10",
    0xca430: "SoundMgrBase::vf12",
    0xca440: "SoundMgrBase::vf13",
    0xca450: "SoundMgrBase::vf14",
    0xca460: "SoundMgrBase::vf15",
    0xcb4f0: "SoundMgr::vf19",
    0xc96a0: "SoundMgrBase::vf20",
    0xc96d0: "SoundMgrBase::vf21",
    0xcbfa0: "SoundMgr::vf22_SetEnvironment",
    0xcbeb0: "SoundMgr::LoadEffectsImage",
    0xc95f0: "SoundMgrBase::vf1_UpdateVoices",
    0xca310: "SoundMgrBase::ctor",
    0xca470: "SoundMgrBase::dtor",
    0xc9690: "SoundMgrBase::vf19",
    0xc9680: "SoundMgrBase::vf22",
    # SoundData (vtable 0x2a3be8, base 0x2a3920)
    0xcca70: "SoundData::ctor",
    0xca160: "SoundDataBase::ctor",
    0xca240: "SoundDataBase::ctor2",
    0xcb960: "SoundData::vf0_ParseWavFile",
    0xcd080: "SoundData::vf1_deldtor",
    0xcb300: "SoundData::vf2_CreateVoice",
    0xccaa0: "SoundData::vf3_CreateVoiceB",
    0xcb3b0: "SoundData::vf4_CreateVoiceLimited",
    0xc9520: "SoundDataBase::vf5",
    0xc9540: "SoundDataBase::vf6",
    0xccb80: "SoundData::vf7",
    0xccbc0: "SoundData::vf8",
    0xccc00: "SoundData::vf9_LoadByName",
    0xcb450: "SoundData::vf10_LoadFromUDSFileObject",
    0xcb720: "SoundData::vf11_ParseWavMemory",
    0xc9f60: "SoundBase::vf15_GetMinDistance",
    0xc9f70: "SoundBase::vf16_GetMaxDistance",
    0xcb290: "SoundData::vf17",
    0xc95c0: "SoundDataBase::vf18",
    # SoundVoice (vtable 0x2a3b18, base 0x2a38b8)
    0xcb240: "SoundVoice::ctor0",
    0xc9da0: "SoundVoiceBase::ctor",
    0xc9fc0: "SoundVoiceBase::ctor2",
    0xcb6e0: "SoundVoice::vf0_StreamFiber",
    0xccfc0: "SoundVoice::vf1_deldtor",
    0xcc030: "SoundVoice::dtor",
    0xcc150: "SoundVoice::CreateDsObject",
    0xcc390: "SoundVoice::vf5_Play",
    0xcc4b0: "SoundVoice::vf6_Play",
    0xcc410: "SoundVoice::vf7_Play",
    0xcb540: "SoundVoice::vf8_Stop",
    0xca0c0: "SoundVoiceBase::vf9",
    0xcb580: "SoundVoice::vf10_Reset",
    0xcc5e0: "SoundVoice::vf11_Update",
    0xc9f40: "SoundVoiceBase::vf12",
    0xc9430: "SoundVoiceBase::vf13",
    0xc9f50: "SoundVoiceBase::vf14",
    0xca110: "SoundVoiceBase::vf16",
    0xc9f80: "SoundVoiceBase::vf18",
    0xc9f90: "SoundBase::vf19",
    0xc9fa0: "SoundVoiceBase::vf20",
    0xc9fb0: "SoundVoiceBase::vf21",
    0xcb5b0: "SoundVoice::StreamPump",
    0xcb680: "SoundVoice::SubmitPacket",
    0xcb500: "SoundVoice::OwnerVelocity",
    # wrappers / misc
    0xc97d0: "Snd_GetSound(name,bStream)",
    0xc96e0: "Snd_EnvironmentName",
    0xcad70: "Snd_PlayTextKey",
    0xcaf60: "Snd_TextKeyParser",
    0xcaf40: "Snd_BeforeMovie",
    0x13f250: "FileObj::ctor",
    0x13f590: "FileObj::Wait",
    0xd8720: "Speech_Play",
    0xee220: "Movie::Open",
    0xede90: "Movie::Update",
    0xee080: "Movie::Close",
    0xa3880: "MemFree",
}

GLOBALS = {
    0x2eb310: "g_pApp",
    0x2ead3c: "g_rolloffCurve[6]",
    0x2d6818: "g_i3dl2Table",
    0x2f90bc: "g_zeroVector",
    0x2939f8: "1.0f",
    0x293a18: "0.0f",
    0x2a3be4: "-1000.0f",
}


def name_of(va):
    if va in NAMES:
        return NAMES[va]
    if va in API:
        return "DS::" + API[va]
    if va in FUNCS:
        return FUNCS[va][0]
    if va in GLOBALS:
        return GLOBALS[va]
    return None


def owner(va):
    i = bisect.bisect_right(FSTARTS, va) - 1
    if i < 0:
        return None
    s = FSTARTS[i]
    return s if va < s + max(FUNCS[s][1], 1) else None


# ---------------------------------------------------------------------------- data access
def u8(va):
    return X.read(va, 1)[0]


def u16(va):
    return struct.unpack("<H", X.read(va, 2))[0]


def u32(va):
    return X.u32(va)


def f32(va):
    return struct.unpack("<f", X.read(va, 4))[0]


def cstr(va):
    try:
        return X.cstr(va)
    except Exception:
        return None


def as_float(v):
    return struct.unpack("<f", struct.pack("<I", v & 0xFFFFFFFF))[0]


def printable_str(va):
    if X.section(va) not in (".rdata", ".data", ".text"):
        return None
    s = cstr(va)
    if s and len(s) >= 2 and all(32 <= ord(c) < 127 or c in "\t\n\r" for c in s[:40]):
        return s
    return None


# ---------------------------------------------------------------------------- disassembly
def dis_range(a, b):
    code = X.read(a, b - a + 16)
    out = []
    for ins in MD.disasm(code, a):
        if ins.address >= b:
            break
        out.append(ins)
    return out


def dis_n(a, n):
    code = X.read(a, n * 15 + 16)
    out = []
    for ins in MD.disasm(code, a):
        out.append(ins)
        if len(out) >= n:
            break
    return out


def annot(ins):
    notes = []
    for m in re.finditer(r"0x([0-9a-f]{4,8})", ins.op_str):
        v = int(m.group(1), 16)
        nm = name_of(v)
        if nm:
            notes.append(nm)
            if v in GLOBALS or X.section(v) in (".rdata", ".data") and v not in FUNCS:
                sec = X.section(v)
                if sec in (".rdata", ".data") and v not in API and v not in NAMES:
                    try:
                        notes.append("=%g" % f32(v))
                    except Exception:
                        pass
            continue
        sec = X.section(v)
        if sec in (".rdata", ".data"):
            s = printable_str(v)
            if s and len(s) >= 3:
                notes.append(repr(s[:70]))
            else:
                try:
                    w = u32(v)
                    fv = f32(v)
                    notes.append("[%08x]=%08x f=%g" % (v, w, fv))
                except Exception:
                    pass
        elif sec is None and v >= 0x2e0000:
            notes.append("bss@%08x" % v)
        if 0x30000000 <= v <= 0x4f000000 or 0xb0000000 <= v <= 0xcf000000:
            if ins.mnemonic in ("push", "mov") and m.start() == ins.op_str.rfind("0x"):
                notes.append("f=%g" % as_float(v))
    return ("   ; " + " | ".join(notes)) if notes else ""


def fmt(ins):
    return "%08x  %-22s %-6s %s%s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, annot(ins))


def listing(a, b, title=None):
    lines = []
    if title:
        lines.append("---- %s %08x..%08x ----" % (title, a, b))
    for ins in dis_range(a, b):
        lines.append(fmt(ins))
    return lines


def func_end(a, limit=0x2000):
    """Recursive-descent end of a function at a: last byte reached through fall-through and conditional
    jumps (no calls), stopping at ret/jmp-out. Good enough for the small methods of the sound module."""
    seen = set()
    todo = [a]
    hi = a
    while todo:
        p = todo.pop()
        while p not in seen and a <= p < a + limit:
            ins = dis_n(p, 1)
            if not ins:
                break
            ins = ins[0]
            seen.add(p)
            hi = max(hi, ins.address + ins.size)
            mn = ins.mnemonic
            if mn in ("ret", "retn"):
                break
            if mn == "jmp":
                op = ins.operands[0]
                if op.type == cx86.X86_OP_IMM and a <= op.imm < a + limit:
                    todo.append(op.imm)
                break
            if mn.startswith("j"):
                op = ins.operands[0]
                if op.type == cx86.X86_OP_IMM:
                    todo.append(op.imm)
            p = ins.address + ins.size
    return hi


# ---------------------------------------------------------------------------- whole-image index
_INDEX = None


def code_index():
    """Linear sweep of all code sections: list of (addr, size, mnemonic, op_str, imm_list, disp_list,
    call_target). Cached in run/analyse_m5/code_index.pkl."""
    global _INDEX
    if _INDEX is not None:
        return _INDEX
    os.makedirs(RUN, exist_ok=True)
    cache = os.path.join(RUN, "code_index.pkl")
    if os.path.exists(cache):
        with open(cache, "rb") as f:
            _INDEX = pickle.load(f)
        return _INDEX
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    idx = []
    for sec in CODE_SECTIONS:
        a, b = sect_range(sec)
        code = X.read(a, b - a)
        off = 0
        n = len(code)
        while off < n:
            got = False
            for ins in md.disasm(code[off:off + 0x10000], a + off):
                imms, disps = [], []
                tgt = None
                for op in ins.operands:
                    if op.type == cx86.X86_OP_IMM:
                        imms.append(op.imm & 0xFFFFFFFF)
                    elif op.type == cx86.X86_OP_MEM:
                        disps.append(op.mem.disp & 0xFFFFFFFF)
                if ins.mnemonic in ("call", "jmp") and imms:
                    tgt = imms[0]
                idx.append((ins.address, ins.size, ins.mnemonic, ins.op_str, tuple(imms), tuple(disps), tgt))
                off = ins.address - a + ins.size
                got = True
                if off >= n:
                    break
            if not got:
                off += 1
    _INDEX = idx
    with open(cache, "wb") as f:
        pickle.dump(idx, f)
    return idx


def xrefs(value, kinds=("call", "imm", "disp")):
    """All instructions in the linear-sweep index that reference `value`."""
    res = []
    for rec in code_index():
        addr, size, mn, ops, imms, disps, tgt = rec
        if "call" in kinds and tgt == value:
            res.append(rec)
        elif "imm" in kinds and value in imms and tgt != value:
            res.append(rec)
        elif "disp" in kinds and value in disps:
            res.append(rec)
    return res


def vtable(va, n):
    return [u32(va + 4 * i) for i in range(n)]


# ---------------------------------------------------------------------------- Ghidra C
_DECOMP = None


def decomp(addr):
    global _DECOMP
    if _DECOMP is None:
        _DECOMP = {}
        d = os.path.join(EXPORT, "decomp")
        for fn in os.listdir(d):
            txt = open(os.path.join(d, fn), encoding="utf-8", errors="replace").read()
            for m in re.finditer(r"// ==== FUNC ([0-9a-f]{8}) ", txt):
                s = m.start()
                e = txt.find("// ==== FUNC ", s + 10)
                _DECOMP[int(m.group(1), 16)] = txt[s:e if e > 0 else len(txt)]
    return _DECOMP.get(addr)


# ---------------------------------------------------------------------------- output
class Tee:
    def __init__(self, path, echo=True):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.f = open(path, "w", encoding="utf-8", newline="\n")
        self.echo = echo

    def __call__(self, *a):
        s = " ".join(str(x) for x in a)
        if self.echo:
            print(s)
        self.f.write(s + "\n")
        self.f.flush()


def out_path(name):
    os.makedirs(OUT, exist_ok=True)
    return os.path.join(OUT, name)


def run_path(*parts):
    p = os.path.join(RUN, *parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p
