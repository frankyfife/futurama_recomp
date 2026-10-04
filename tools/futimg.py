"""Reader for Futurama (UDS) .img archives.

Header layout (little endian), parsed the way the game does (FUN_0013fc40 in retail EU):
  u32 header_size            (may include up to 3 trailing pad bytes)
  entries until header_size:
    name: NUL-terminated, padded to 4 bytes (incl. NUL). First char has bit 7 set -> directory.
    directory: u32 rel       children run from (field + 4) to (field + rel - 4);
                             the next sibling starts at (field + rel)
    file:      u32 size, u32 offset (absolute in .img)
"""
import os, struct, sys

def _walk(data, off, end, prefix):
    while off < end:
        z = data.index(b"\0", off)
        raw = bytearray(data[off:z])
        if not raw:
            break
        field = off + ((z - off + 4) & ~3)
        is_dir = bool(raw[0] & 0x80)
        raw[0] &= 0x7F
        path = prefix + raw.decode("latin-1")
        if is_dir:
            rel = struct.unpack_from("<I", data, field)[0]
            yield path, None, None
            yield from _walk(data, field + 4, field + rel - 4, path + "/")
            off = field + rel
        else:
            size, foff = struct.unpack_from("<II", data, field)
            yield path, size, foff
            off = field + 8

def entries(data):
    """Yield (path, size, offset) for files and (path, None, None) for directories."""
    hdr_size = struct.unpack_from("<I", data, 0)[0]
    yield from _walk(data, 4, hdr_size, "")

def open_header(f):
    hdr_size = struct.unpack("<I", f.read(4))[0]
    f.seek(0)
    return f.read(hdr_size)

if __name__ == "__main__":
    cmd, img = sys.argv[1], sys.argv[2]
    with open(img, "rb") as f:
        hdr = open_header(f)
        for path, size, off in entries(hdr):
            if cmd == "ls":
                if size is not None:
                    print(f"{size:>10} {off:#010x} {path}")
            elif cmd == "x" and size is not None:
                pats = sys.argv[4:]
                if pats and not any(p.lower() in path.lower() for p in pats):
                    continue
                dst = os.path.join(sys.argv[3], path)
                os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
                f.seek(off)
                with open(dst, "wb") as o:
                    o.write(f.read(size))
