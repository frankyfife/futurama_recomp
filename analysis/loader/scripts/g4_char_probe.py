"""G4 character override experiment (analysis/loader/g4_select.md 4.2). Live patch of a running futurama.exe (guest identity-mapped).
Starts futurama.exe, waits until the XBE is mapped, then patches three guest functions with code caves:
  PlayerDef ctor FUN_0005E580(name)     : name FROM -> TO  (def name, Chars\\<name>.ucf, player subclass)
  Level::FindDef FUN_000C3E80(name)      : lookup FROM -> TO (instance "fry-1-1|002d::PLAYER" finds the TO def)
  Level::FindCharDef FUN_000C3F20(name)  : lookup FROM -> TO
usage: python g4_char_probe.py <exe> <ini> <from> <to>   (run dir = folder of the ini, e.g. run/g4_select/char)
example: g4_char_probe.py build/g4-select/bin/futurama.exe run/g4_select/char/futurama.ini fry-1-1 bender"""
import ctypes, ctypes.wintypes as w, os, struct, subprocess, sys, time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = w.HANDLE
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, w.DWORD, w.DWORD]
k32.VirtualProtectEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, w.DWORD, ctypes.POINTER(w.DWORD)]
k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.FlushInstructionCache.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t]

exe, ini, FROM, TO = sys.argv[1:5]
STRICMP = 0x168441
SITES = {0x5E580: bytes.fromhex("8B44240456"), 0xC3E80: bytes.fromhex("538B5C2408"), 0xC3F20: bytes.fromhex("538B5C2408")}

p = subprocess.Popen([exe, "--config=" + ini], cwd=os.path.dirname(os.path.abspath(ini)),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
h = k32.OpenProcess(0x1F0FFF, False, p.pid)

def rd(a, n):
    b = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(got)):
        return None
    return b.raw[:got.value]

def wr(a, data):
    old = w.DWORD()
    k32.VirtualProtectEx(h, ctypes.c_void_p(a), len(data), 0x40, ctypes.byref(old))
    got = ctypes.c_size_t()
    ok = k32.WriteProcessMemory(h, ctypes.c_void_p(a), data, len(data), ctypes.byref(got))
    k32.VirtualProtectEx(h, ctypes.c_void_p(a), len(data), old.value, ctypes.byref(old))
    k32.FlushInstructionCache(h, ctypes.c_void_p(a), len(data))
    assert ok and got.value == len(data), hex(a)

t0 = time.time()
while time.time() - t0 < 30:
    if all(rd(a, 5) == b for a, b in SITES.items()):
        break
    time.sleep(0.05)
else:
    print("XBE not mapped / bytes differ:", {hex(a): (rd(a, 5) or b"").hex() for a in SITES}); p.kill(); sys.exit(1)

cave = k32.VirtualAllocEx(h, None, 0x1000, 0x3000, 0x40)
assert cave and cave < 0x100000000, hex(cave or 0)
s_from, s_to = cave + 0x200, cave + 0x220
rel = lambda src_next, dst: struct.pack("<i", dst - src_next)
code = bytearray()

def cave_lookup(resume):  # entry of FindDef/FindCharDef: push ebx; mov ebx,[esp+8]; alias; jmp resume
    c = bytearray(b"\x53\x8B\x5C\x24\x08\x51\x68" + struct.pack("<I", s_from) + b"\x53")
    base = cave + len(code)
    c += b"\xE8" + rel(base + len(c) + 5, STRICMP)
    c += b"\x83\xC4\x08\x59\x85\xC0\x75\x05\xBB" + struct.pack("<I", s_to)
    c += b"\xE9" + rel(base + len(c) + 5, resume)
    return c

def cave_ctor():  # entry of PlayerDef ctor: alias [esp+4]; mov eax,[esp+4]; push esi; jmp 0x5E585
    c = bytearray(b"\x51\x68" + struct.pack("<I", s_from) + b"\xFF\x74\x24\x0C")
    base = cave + len(code)
    c += b"\xE8" + rel(base + len(c) + 5, STRICMP)
    c += b"\x83\xC4\x08\x59\x85\xC0\x75\x08\xC7\x44\x24\x04" + struct.pack("<I", s_to)
    c += b"\x8B\x44\x24\x04\x56"
    c += b"\xE9" + rel(base + len(c) + 5, 0x5E585)
    return c

entries = {}
entries[0x5E580] = cave + len(code); code += cave_ctor()
entries[0xC3E80] = cave + len(code); code += cave_lookup(0xC3E85)
entries[0xC3F20] = cave + len(code); code += cave_lookup(0xC3F25)
assert len(code) < 0x200
wr(cave, bytes(code))
wr(s_from, FROM.encode() + b"\0")
wr(s_to, TO.encode() + b"\0")
for a, e in entries.items():
    wr(a, b"\xE9" + rel(a + 5, e))
print("t=%.2fs patched (cave %#x): %s -> %s" % (time.time() - t0, cave, FROM, TO), flush=True)

# watch the player object: [[0x2DABC4]+0x94], type at +0x774 (1 Fry, 2 Leela, 3 Bender, 4 Rodent), vtable
def u32(a):
    d = rd(a, 4); return struct.unpack("<I", d)[0] if d and len(d) == 4 else None
last = None
while p.poll() is None:
    lvl = u32(0x2DABC4)
    pl = u32(lvl + 0x94) if lvl else None
    info = None
    if pl:
        name = u32(lvl + 0xC)
        lname = (rd(name, 16) or b"").split(b"\0")[0].decode("latin-1") if name else "?"
        info = (lname, hex(pl), hex(u32(pl) or 0), u32(pl + 0x774), struct.unpack('<f', rd(pl + 0x4F4, 4))[0])
    if info != last:
        print("t=%.1fs player %s" % (time.time() - t0, info), flush=True)
        last = info
    time.sleep(0.25)
print("exit", p.returncode)
