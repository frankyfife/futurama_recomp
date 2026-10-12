"""G1 outlines (analysis/loader/g1_outlines.md 3.3): experiment - paints the QDOutline shell material (NiMaterialProperty
vtable 0x2A7A60 with ambient = diffuse = 0.5, emissive 0, glossiness 4, alpha 0.75) magenta and opaque in the running
guest (WriteProcessMemory, revision +0x54 incremented) so that a later input-script screenshot shows which pixels
the inverted hull covers. Usage: python g1_magenta.py <exe> <ini> <seconds>."""
# Experiment: paint the QDOutline shell material magenta (emissive 1,0,1) in the running guest.
import ctypes, ctypes.wintypes as w, struct, subprocess, sys, time
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", w.DWORD),
                ("PartitionId", w.WORD), ("RegionSize", ctypes.c_size_t), ("State", w.DWORD), ("Protect", w.DWORD), ("Type", w.DWORD)]
k32.VirtualQueryEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
exe, ini, delay = sys.argv[1], sys.argv[2], float(sys.argv[3])
p = subprocess.Popen([exe, "--config=" + ini], cwd=r"D:\FuturamaDecomp\run\g1_outlines", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
h = k32.OpenProcess(0x10 | 0x20 | 0x08 | 0x400, False, p.pid)
def rd(a, n):
    b = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    return b.raw[:got.value] if k32.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(got)) else None
def wr(a, data):
    got = ctypes.c_size_t(); return k32.WriteProcessMemory(h, ctypes.c_void_p(a), data, len(data), ctypes.byref(got))
time.sleep(delay)
hits = []
a = 0x300000; mbi = MBI(); pat = struct.pack("<I", 0x2a7a60)
while a < 0x20000000 and k32.VirtualQueryEx(h, ctypes.c_void_p(a), ctypes.byref(mbi), ctypes.sizeof(mbi)):
    base, size = mbi.BaseAddress or 0, mbi.RegionSize
    if mbi.State == 0x1000 and mbi.Protect in (0x04, 0x40):
        for off in range(0, size, 1 << 20):
            chunk = rd(base + off, min(1 << 20, size - off)) or b""
            i = chunk.find(pat)
            while i >= 0:
                if i % 4 == 0 and i + 0x5c <= len(chunk):
                    m = chunk[i:i + 0x5c]
                    amb = struct.unpack_from("<3f", m, 0x1c); dif = struct.unpack_from("<3f", m, 0x28); em = struct.unpack_from("<3f", m, 0x40)
                    gl, al = struct.unpack_from("<2f", m, 0x4c)
                    if al == 0.75 and amb == (0.5, 0.5, 0.5) and dif == (0.5, 0.5, 0.5) and em == (0, 0, 0) and gl == 4.0:
                        hits.append(base + off + i)
                i = chunk.find(pat, i + 1)
    a = base + size
print("shell materials:", [hex(x) for x in hits])
for m in hits:
    rev = struct.unpack("<I", rd(m + 0x54, 4))[0]
    wr(m + 0x40, struct.pack("<3f", 1.0, 0.0, 1.0)); wr(m + 0x50, struct.pack("<f", 1.0)); wr(m + 0x54, struct.pack("<I", rev + 1))
print("written at t=%.1f" % delay)
p.wait(timeout=90); print("exit", p.returncode)
