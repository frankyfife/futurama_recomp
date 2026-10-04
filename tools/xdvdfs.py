"""Minimal XDVDFS (Xbox DVD filesystem) reader: list and extract files."""
import os, struct, sys

SECTOR = 2048

class XDVDFS:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.base = 0
        for base in (0, 0x18300000):  # plain xiso or full redump image
            self.f.seek(base + 32 * SECTOR)
            if self.f.read(20) == b"MICROSOFT*XBOX*MEDIA":
                self.base = base
                break
        else:
            raise ValueError("no XDVDFS volume descriptor found")
        self.root_sector, self.root_size = struct.unpack("<II", self.f.read(8))

    def read(self, sector, size, offset=0):
        self.f.seek(self.base + sector * SECTOR + offset)
        return self.f.read(size)

    def walk(self, sector=None, size=None, prefix=""):
        if sector is None:
            sector, size = self.root_sector, self.root_size
        if size == 0:
            return
        data = self.read(sector, size)
        seen = set()
        stack = [0]
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            left, right, start, fsize, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
            if left == 0xFFFF and right == 0xFFFF:
                continue
            name = data[off + 14: off + 14 + nlen].decode("latin-1")
            path = prefix + "/" + name
            if attr & 0x10:
                yield path, start, fsize, True
                yield from self.walk(start, fsize, path)
            else:
                yield path, start, fsize, False
            if left:
                stack.append(left * 4)
            if right:
                stack.append(right * 4)

    def extract(self, outdir, only=None):
        for path, start, size, is_dir in self.walk():
            dst = os.path.join(outdir, path.lstrip("/"))
            if is_dir:
                os.makedirs(dst, exist_ok=True)
                continue
            if only and not any(path.lower().endswith(o.lower()) for o in only):
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as out:
                self.f.seek(self.base + start * SECTOR)
                remaining = size
                while remaining:
                    chunk = self.f.read(min(remaining, 1 << 20))
                    out.write(chunk)
                    remaining -= len(chunk)

if __name__ == "__main__":
    cmd, img = sys.argv[1], sys.argv[2]
    v = XDVDFS(img)
    if cmd == "ls":
        for path, start, size, is_dir in sorted(v.walk()):
            print(f"{'D' if is_dir else 'F'} {size:>11} {path}")
    elif cmd == "x":
        v.extract(sys.argv[3], sys.argv[4:] or None)
