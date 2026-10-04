"""Tiny XBE image helper: VA <-> file offset, reading, section lookup."""
import struct

class Xbe:
    def __init__(self, path):
        self.b = open(path, "rb").read()
        b = self.b
        assert b[:4] == b"XBEH"
        self.base = struct.unpack_from("<I", b, 0x104)[0]
        nsect, sect = struct.unpack_from("<II", b, 0x11C)
        self.sections = []
        for i in range(nsect):
            o = sect - self.base + i * 0x38
            fl, va, vsz, raw, rsz, na = struct.unpack_from("<6I", b, o)
            name = b[na - self.base:b.index(b"\0", na - self.base)].decode()
            self.sections.append((name, va, vsz, raw, rsz))

    def off2va(self, off):
        for n, va, vsz, raw, rsz in self.sections:
            if raw <= off < raw + rsz:
                return va + off - raw

    def va2off(self, v):
        for n, va, vsz, raw, rsz in self.sections:
            if va <= v < va + rsz:
                return raw + v - va

    def section(self, v):
        for n, va, vsz, raw, rsz in self.sections:
            if va <= v < va + vsz:
                return n

    def read(self, v, size):
        o = self.va2off(v)
        return self.b[o:o + size] if o is not None else None

    def u32(self, v):
        return struct.unpack("<I", self.read(v, 4))[0]

    def cstr(self, v):
        o = self.va2off(v)
        if o is None:
            return None
        return self.b[o:self.b.index(b"\0", o)].decode("latin-1")
