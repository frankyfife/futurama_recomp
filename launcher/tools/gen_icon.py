#!/usr/bin/env python3
"""Generates the launcher icon (original, abstract design) as PNG files and a multi-size .ico.

The motif is deliberately generic and contains no game artwork: a rounded square with a diagonal
teal-to-indigo gradient, a white "play" triangle and three speed lines ("launch"). Everything is
drawn here with signed-distance shapes and 4x4 supersampling, so the output is deterministic and
needs only the Python standard library.

    python launcher/tools/gen_icon.py           regenerate launcher/resources/icon/*
    python launcher/tools/gen_icon.py --check   verify that the checked-in files are up to date
                                                (compares decoded pixels, not zlib bytes)
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
import zlib
from pathlib import Path

ICON_DIR = Path(__file__).resolve().parent.parent / "resources" / "icon"
PNG_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
SUPERSAMPLE = 4

# Colours (RGB 0..255).
GRADIENT_START = (32, 178, 160)   # teal, top left
GRADIENT_END = (58, 70, 196)      # indigo, bottom right
FOREGROUND = (255, 255, 255)
SPEED_LINE_ALPHA = 0.78


def rounded_rect_inside(x: float, y: float, x0: float, y0: float, x1: float, y1: float, r: float) -> bool:
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r and x0 <= x <= x1 and y0 <= y <= y1


def triangle_inside(x: float, y: float, pts: tuple[tuple[float, float], ...]) -> bool:
    (ax, ay), (bx, by), (cx, cy) = pts

    def edge(px: float, py: float, qx: float, qy: float) -> float:
        return (qx - px) * (y - py) - (qy - py) * (x - px)

    e0 = edge(ax, ay, bx, by)
    e1 = edge(bx, by, cx, cy)
    e2 = edge(cx, cy, ax, ay)
    return (e0 >= 0 and e1 >= 0 and e2 >= 0) or (e0 <= 0 and e1 <= 0 and e2 <= 0)


def capsule_inside(x: float, y: float, x0: float, x1: float, cy: float, r: float) -> bool:
    px = min(max(x, x0), x1)
    return (x - px) ** 2 + (y - cy) ** 2 <= r * r


def render(size: int) -> bytes:
    """Returns straight-alpha RGBA bytes of the icon at `size` x `size` pixels."""
    s = float(size)
    margin = s * (0.0 if size <= 20 else 0.04)
    radius = s * 0.22
    # Triangle (pointing right), slightly right of centre so the speed lines fit on the left.
    tri = ((s * 0.42, s * 0.27), (s * 0.42, s * 0.73), (s * 0.80, s * 0.50))
    if size < 24:
        tri = ((s * 0.33, s * 0.24), (s * 0.33, s * 0.76), (s * 0.80, s * 0.50))
    # Speed lines as capsules (x0, x1, centre y, radius); left out below 24 px where they would blur.
    lines: list[tuple[float, float, float, float]] = []
    if size >= 24:
        thickness = max(s * 0.035, 0.9)
        lines = [
            (s * 0.20, s * 0.34, s * 0.37, thickness),
            (s * 0.13, s * 0.34, s * 0.50, thickness),
            (s * 0.20, s * 0.34, s * 0.63, thickness),
        ]

    out = bytearray()
    n = SUPERSAMPLE
    for py in range(size):
        for px in range(size):
            acc_r = acc_g = acc_b = acc_a = 0.0
            for sy in range(n):
                for sx in range(n):
                    x = px + (sx + 0.5) / n
                    y = py + (sy + 0.5) / n
                    if not rounded_rect_inside(x, y, margin, margin, s - margin, s - margin, radius):
                        continue
                    t = ((x - margin) + (y - margin)) / (2.0 * (s - 2 * margin))
                    t = min(max(t, 0.0), 1.0)
                    r = GRADIENT_START[0] + (GRADIENT_END[0] - GRADIENT_START[0]) * t
                    g = GRADIENT_START[1] + (GRADIENT_END[1] - GRADIENT_START[1]) * t
                    b = GRADIENT_START[2] + (GRADIENT_END[2] - GRADIENT_START[2]) * t
                    if triangle_inside(x, y, tri):
                        r, g, b = FOREGROUND
                    else:
                        for (x0, x1, cy, rad) in lines:
                            if capsule_inside(x, y, x0, x1, cy, rad):
                                a = SPEED_LINE_ALPHA
                                r = r + (FOREGROUND[0] - r) * a
                                g = g + (FOREGROUND[1] - g) * a
                                b = b + (FOREGROUND[2] - b) * a
                                break
                    acc_r += r
                    acc_g += g
                    acc_b += b
                    acc_a += 1.0
            if acc_a == 0.0:
                out += b"\x00\x00\x00\x00"
                continue
            alpha = acc_a / (n * n)
            out += bytes(
                (
                    int(round(acc_r / acc_a)),
                    int(round(acc_g / acc_a)),
                    int(round(acc_b / acc_a)),
                    int(round(alpha * 255.0)),
                )
            )
    return bytes(out)


def png_bytes(size: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = bytearray()
    stride = size * 4
    for row in range(size):
        raw += b"\x00" + rgba[row * stride : (row + 1) * stride]
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def png_pixels(data: bytes) -> tuple[int, bytes]:
    """Decodes a PNG written by png_bytes (filter 0 only) -> (size, RGBA)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    size = 0
    idat = b""
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        if kind == b"IHDR":
            size = struct.unpack(">I", body[:4])[0]
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = size * 4
    out = bytearray()
    for row in range(size):
        line = raw[row * (stride + 1) : (row + 1) * (stride + 1)]
        if line[0] != 0:
            raise ValueError("unexpected PNG filter")
        out += line[1:]
    return size, bytes(out)


def ico_bytes(images: list[tuple[int, bytes]]) -> bytes:
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    payload = b""
    for size, png in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(png), offset + len(payload))
        payload += png
    return header + entries + payload


def ico_pixels(data: bytes) -> list[tuple[int, bytes]]:
    _, _, count = struct.unpack("<HHH", data[:6])
    result = []
    for i in range(count):
        entry = data[6 + 16 * i : 6 + 16 * (i + 1)]
        _, _, _, _, _, _, length, offset = struct.unpack("<BBBBHHII", entry)
        result.append(png_pixels(data[offset : offset + length]))
    return result


def generate() -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    images = []
    for size in PNG_SIZES:
        png = png_bytes(size, render(size))
        files[f"icon_{size}.png"] = png
        images.append((size, png))
    files["launcher.ico"] = ico_bytes(images)
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="verify the checked-in files instead of writing them")
    args = parser.parse_args()
    files = generate()
    if args.check:
        stale = []
        for name, data in files.items():
            path = ICON_DIR / name
            if not path.exists():
                stale.append(f"{name} (missing)")
                continue
            current = path.read_bytes()
            try:
                if name.endswith(".png"):
                    same = png_pixels(current) == png_pixels(data)
                else:
                    same = ico_pixels(current) == ico_pixels(data)
            except (ValueError, struct.error, zlib.error):
                same = False
            if not same:
                stale.append(name)
        if stale:
            print("gen_icon: out of date: " + ", ".join(stale) + " - run python launcher/tools/gen_icon.py")
            return 1
        print(f"gen_icon: {len(files)} files up to date")
        return 0
    ICON_DIR.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (ICON_DIR / name).write_bytes(data)
    print(f"gen_icon: wrote {len(files)} files to {ICON_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
