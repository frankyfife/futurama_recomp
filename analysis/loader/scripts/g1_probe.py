"""G1 outlines (analysis/loader/g1_outlines.md 3): live probe of a running futurama.exe (guest identity-mapped).
Starts the exe with the given ini, waits, reads the NiOutline globals (DAT_002D7AB8 mode, DAT_002D7AB0 width,
DAT_002FA440 pool index, visibility flags DAT_002FA45C) and lists all NiOutline objects (vtable 0x2A9CC0) with
their outline geometries. Usage: python g1_probe.py <exe> <ini> <seconds>  (run dir: run/g1_outlines)."""
# Reads outline-related guest globals / NiOutline objects from a running futurama.exe (identity-mapped guest).
import ctypes, ctypes.wintypes as w, struct, subprocess, sys, time, json
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROCESS_VM_READ, PROCESS_QUERY_INFORMATION = 0x10, 0x400
class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", w.DWORD),
                ("PartitionId", w.WORD), ("RegionSize", ctypes.c_size_t), ("State", w.DWORD), ("Protect", w.DWORD), ("Type", w.DWORD)]
k32.VirtualQueryEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
exe, ini, delay = sys.argv[1], sys.argv[2], float(sys.argv[3])
classes = json.load(open(r"D:\FuturamaDecomp\analysis\retail_eu_ni_classes.json"))["classes"]
vt2name = {c["vtable"]: n for n, c in classes.items() if "vtable" in c}
p = subprocess.Popen([exe, "--config=" + ini], cwd=r"D:\FuturamaDecomp\run\g1_outlines", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, p.pid)
def rd(a, n):
    b = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(got)): return None
    return b.raw[:got.value]
def u32(a):
    d = rd(a, 4); return struct.unpack("<I", d)[0] if d else None
def f32(a):
    d = rd(a, 4); return struct.unpack("<f", d)[0] if d else None
t0 = time.time()
while time.time() - t0 < delay and p.poll() is None:
    time.sleep(0.5)
def globals_():
    return dict(mode=u32(0x2d7ab8), thick=f32(0x2d7ab0), bias=f32(0x2d7aac), poolIdx=u32(0x2fa440), edges=u32(0x2fa43c),
                eb40=hex(u32(0x30eb40) or 0), fa460=hex(u32(0x2fa460) or 0), maxv=u32(0x30f684), props=[hex(u32(a) or 0) for a in (0x3044bc,0x3044c8,0x3044e8,0x30e568,0x3044a0)])
print("t=%.1f" % (time.time() - t0), globals_())
# scan for NiOutline objects
found = []
a = 0x10000; mbi = MBI()
while a < 0x20000000 and k32.VirtualQueryEx(h, ctypes.c_void_p(a), ctypes.byref(mbi), ctypes.sizeof(mbi)):
    base, size = mbi.BaseAddress or 0, mbi.RegionSize
    if mbi.State == 0x1000 and mbi.Protect in (0x04, 0x02, 0x40, 0x20, 0x08, 0x80) and base >= 0x300000:
        for off in range(0, size, 1 << 20):
            chunk = rd(base + off, min(1 << 20, size - off))
            if not chunk: continue
            i = chunk.find(b"\xc0\x9c\x2a\x00")
            while i >= 0:
                if i % 4 == 0: found.append(base + off + i)
                i = chunk.find(b"\xc0\x9c\x2a\x00", i + 1)
    a = base + size
print("NiOutline objects:", len(found))
for o in found[:40]:
    data = u32(o + 0xe8); n = u32(o + 0xf8) or 0; lst = u32(o + 0xf0)
    info = dict(obj=hex(o), refc=u32(o + 4), intensity=f32(o + 0xe4), cam=hex(u32(o + 0xe0) or 0), nGeom=n, nChildren=u32(o + 0xcc))
    if data and data < 0x20000000:
        info.update(nEdges=u32(data + 0x1c), nTris=u32(data + 0x20), nVerts=u32(data + 0x24))
    kids = []
    for i in range(min(n, 8)):
        g = u32(lst + 4 * i)
        if not g: continue
        kids.append((hex(g), vt2name.get(u32(g), hex(u32(g) or 0)), "skin" if u32(g + 0xcc) else "rigid"))
    info["geoms"] = kids
    print(info)
for i in range(6):
    time.sleep(0.4)
    g = globals_(); flags = rd(u32(0x2fa45c) or 0, max(1, min(64, u32(0x2fa458) or 1))) if u32(0x2fa45c) else b""
    print("sample", g["poolIdx"], g["edges"], g["eb40"], "visflags", flags.hex() if flags else None)
p.wait(timeout=60)
print("exit", p.returncode)
