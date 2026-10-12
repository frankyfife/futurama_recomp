"""Sound use in the level scripts (scripts/*.ful in FutPAL01.img; read-only on the ISO).

Heuristic bytecode reading (instruction = 4-byte header [flags u8][line u16][opcode u8] + operands):
  0x09 <len u32> <string, NUL-padded>   push string        0x08 <u32>   push constant
  0x13 <u32 0> <u32 id>                 call native id     0x0b <u32>   store global
  0x05 <u32>                            load global
Native ids = .dbg symbol index - 1 (FsLoadSound 0x55, FsPlaySFX 0x5a, FsPlay3DSFX 0x5b, FsPlayMusic 0x5c,
FsPlaySpeech 0x5d, FsSetReverb 0x66). The dispatcher FUN_000B0C40 maps id - 2 to its jump table
(case = dbg index - 4, m5_audio.md 3.8).
For every FsLoadSound: file, bStream constant, global it is stored in, and which play natives load that global.
Output: scripts/out/m5_script_sound.out (file names only)."""
import collections
import re
import struct
import sys

sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS  # noqa: E402
import futimg  # noqa: E402
from m5_common import Tee, out_path  # noqa: E402

ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"
T = Tee(out_path("m5_script_sound.out"))
NATIVE = {0x55: "FsLoadSound", 0x58: "FsPlaySound", 0x5a: "FsPlaySFX", 0x5b: "FsPlay3DSFX", 0x5c: "FsPlayMusic",
          0x5d: "FsPlaySpeech", 0x66: "FsSetReverb"}
v = XDVDFS(ISO)
tot = collections.Counter()
for ip, s, sz, d in sorted(v.walk()):
    if d or not ip.lower().endswith("futpal01.img"):
        continue
    hs = struct.unpack("<I", v.read(s, 4))[0]
    for path, fsz, foff in futimg.entries(v.read(s, hs)):
        if not fsz or not path.lower().endswith(".ful"):
            continue
        data = v.read(s, fsz, foff)
        calls = collections.Counter()
        for nid, nm in NATIVE.items():
            calls[nm] = len(re.findall(re.escape(b"\x13\x00\x00\x00\x00" + struct.pack("<I", nid)), data))
            tot[nm] += calls[nm]
        loads = []
        for m in re.finditer(re.escape(b"\x13\x00\x00\x00\x00\x55\x00\x00\x00"), data):
            i = m.start()
            back = data[max(0, i - 160):i]
            sm = list(re.finditer(rb"([ -~]{4,120}\.wav)\x00", back, re.I))
            cm = list(re.finditer(rb"\x08([\x00\x01])\x00\x00\x00", back))
            name = sm[-1].group(1).decode() if sm else "?"
            bstream = cm[-1].group(1)[0] if cm else None
            fw = data[i + 9:i + 40]
            st = re.search(rb"\x0b(.{4})", fw, re.S)
            var = struct.unpack("<I", st.group(1))[0] if st else None
            loads.append((name, bstream, var))
        users = collections.defaultdict(set)
        for nid in (0x58, 0x5a, 0x5b, 0x5c, 0x5d):
            for m in re.finditer(re.escape(b"\x13\x00\x00\x00\x00" + struct.pack("<I", nid)), data):
                back = data[max(0, m.start() - 40):m.start()]
                for lm in re.finditer(rb"\x05(.{4})", back, re.S):
                    users[struct.unpack("<I", lm.group(1))[0]].add(NATIVE[nid])
        T("== %s: %s" % (path, ", ".join("%s %d" % (k, n) for k, n in calls.items() if n)))
        for name, bs, var in loads:
            if bs == 1 or "music" in name.lower() or "ambient" in name.lower():
                T("   FsLoadSound(%-55s, bStream=%s) -> global %s used by %s" % (
                    name, bs, ("0x%x" % var) if var is not None else "?", sorted(users.get(var, []))))
        T("   (FsLoadSound with bStream=0: %d)" % sum(1 for l in loads if l[1] == 0))
T("\ntotal calls over all level scripts:", dict(tot))
