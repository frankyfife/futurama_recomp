"""Read .img archive headers directly from the retail EU ISO (read-only) via tools/xdvdfs.py and parse them
the way the game does (FUN_0013fc40): directory entry = name(bit7 on first char), pad4, u32 rel where
the directory's children end at (address_of_u32 + rel - 4) and skipping the directory jumps to
(address_of_u32 + rel); file entry = name, pad4, u32 size, u32 offset.
Compares with tools/futimg.py's interpretation (absolute end offset).
usage: python files_imgheader.py [archive-name ...]   (default: all .img)"""
import struct, sys
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xdvdfs import XDVDFS
ISO = r"Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso"

def game_walk(h, start, end, prefix, out):
    off = start
    while off < end:
        z = h.index(b"\0", off)
        raw = bytearray(h[off:z])
        if not raw:  # header_size may include up to 3 trailing pad bytes (SysPAL: 0xcf7)
            break
        nxt = off + ((z - off + 4) & ~3)  # game: (strlen+4)&~3
        is_dir = bool(raw[0] & 0x80); raw[0] &= 0x7F
        name = raw.decode("latin-1")
        if is_dir:
            rel = struct.unpack_from("<I", h, nxt)[0]
            child_end = nxt + rel - 4
            game_walk(h, nxt + 4, child_end, prefix + name + "/", out)
            off = nxt + rel
        else:
            size, foff = struct.unpack_from("<II", h, nxt)
            out.append((prefix + name, size, foff))
            off = nxt + 8

def main():
    v = XDVDFS(ISO)
    files = {p.lstrip("/"): (s, n) for p, s, n, d in v.walk() if not d}
    names = sys.argv[1:] or sorted(k for k in files if k.lower().endswith(".img"))
    for nm in names:
        sec, size = files[nm]
        hs = struct.unpack("<I", v.read(sec, 4))[0]
        h = v.read(sec, hs)
        out = []
        game_walk(h, 4, hs, "", out)
        mis = sum(1 for _, s, o in out if o % 0x800)
        past = sum(1 for _, s, o in out if o + s > size)
        print(f"== {nm}: img size {size}, header {hs:#x}, {len(out)} files, misaligned {mis}, past-EOF {past}")
        with open(rf"D:\FuturamaDecomp\analysis\loader\scripts\files_{nm.split('.')[0]}_gamelist.txt", "w") as f:
            for p, s, o in out:
                f.write(f"{s:>10} {o:#010x} {p}\n")

if __name__ == "__main__":
    main()
