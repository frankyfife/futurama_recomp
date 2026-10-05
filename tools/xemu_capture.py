"""Capture reference screenshots of the original game running in xemu.

Starts xemu (using its own configuration, e.g. the configured DVD image), grabs the
emulator window at a fixed interval and writes PNGs plus an index into an output
directory under run\\ (git-ignored). The xemu configuration is never modified.

Usage:
  python tools/xemu_capture.py --exe F:\\emulatoren\\xemu-win-release\\xemu.exe \\
      --out run\\xemu_ref --duration 180 --interval 2 [--dvd <iso>]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

from PIL import Image, ImageGrab, ImageStat

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32")
user32.SetProcessDPIAware()

PW_CLIENTONLY = 0x1
PW_RENDERFULLCONTENT = 0x2
WM_CLOSE = 0x0010


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]


def find_main_window(pid):
    found = []
    proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            rect = wt.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(rect))
            if rect.right > 200 and rect.bottom > 200:
                found.append((rect.right * rect.bottom, hwnd))
        return True

    user32.EnumWindows(proc(cb), 0)
    return max(found)[1] if found else None


def print_window(hwnd):
    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    w, h = rect.right, rect.bottom
    hdc = user32.GetDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    old = gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT)
    bih = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bih), 0)
    gdi32.SelectObject(mdc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)
    return Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1)


def screen_grab(hwnd):
    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return ImageGrab.grab(bbox=(pt.x, pt.y, pt.x + rect.right, pt.y + rect.bottom), all_screens=True)


def is_blank(img):
    return max(ImageStat.Stat(img.convert("L")).extrema[0]) < 8


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--duration", type=float, default=180)
    ap.add_argument("--interval", type=float, default=2)
    ap.add_argument("--dvd", help="optional DVD image override (-dvd_path)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    cmd = [args.exe] + (["-dvd_path", args.dvd] if args.dvd else [])
    proc = subprocess.Popen(cmd, cwd=os.path.dirname(args.exe))
    start = time.monotonic()
    hwnd = None
    while hwnd is None and time.monotonic() - start < 30:
        time.sleep(0.5)
        hwnd = find_main_window(proc.pid)
    if hwnd is None:
        proc.kill()
        sys.exit("xemu window not found within 30 s")

    index = []
    try:
        next_t = 0.0
        while time.monotonic() - start < args.duration and proc.poll() is None:
            t = time.monotonic() - start
            if t < next_t:
                time.sleep(min(0.1, next_t - t))
                continue
            next_t += args.interval
            img = print_window(hwnd)
            method = "printwindow"
            if is_blank(img):
                img = screen_grab(hwnd)
                method = "screen"
            name = f"t{t:06.1f}s.png"
            img.save(os.path.join(args.out, name))
            index.append(f"{t:.1f}\t{name}\t{img.width}x{img.height}\t{method}\t{'blank' if is_blank(img) else ''}")
    finally:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        with open(os.path.join(args.out, "index.tsv"), "w") as f:
            f.write("seconds\tfile\tsize\tmethod\tnote\n" + "\n".join(index) + "\n")
    print(f"{len(index)} captures in {args.out} ({sum(1 for l in index if l.endswith('blank'))} blank)")


if __name__ == "__main__":
    main()
