"""Keeps the game windows of a check run in the foreground (display measurements: m3_check M3b-7, m3d_check D1/L1).

Windows lets a process take the foreground only under conditions (the foreground process started it or allowed it,
no recent user input, ...). A check script started from a background console does not qualify, and when a game
window closes, the foreground goes back to whatever was active before (a browser, another session's window) - the
next game window of the check then stays in the background and the pacer runs "free" on its timer (M3b design).

This wrapper shows a small window ("Messung läuft"), takes the foreground once at start (possible while nobody uses
the computer), allows every process to take the foreground while it is in front (AllowSetForegroundWindow(ASFW_ANY),
renewed every 0.5 s) and runs the given command. The game windows then come to the front when they open, and when
one closes, the foreground returns to this window instead of the previous application. While it runs it also holds a
display power request (SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED), like a video
player): a display that is on stays on for the whole measurement instead of switching off after the idle timeout (a
display that is already off does not come back - M3d phase 5: the 120 Hz TV was off, every display check then ran
"free" without presents). The request ends with this process. It changes no system setting.
Do not use the computer during the run (user input ends the permission, as it should).

Usage:
    python tools/foreground_hold.py -- python tools/m3d_check.py --exe ... --config ...
Exit code: the command's.
"""
import ctypes
import subprocess
import sys
import threading

ASFW_ANY = 0xFFFFFFFF
ES_CONTINUOUS = 0x80000000
ES_SYSTEM_REQUIRED = 0x00000001
ES_DISPLAY_REQUIRED = 0x00000002


def main() -> int:
    argv = sys.argv[1:]
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        sys.exit(__doc__)
    try:
        import tkinter as tk
    except ImportError:
        print("foreground_hold: tkinter missing - running the command without the holder window")
        return subprocess.call(argv)
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    result = {"code": 1}
    # Display and system stay on while the measurement runs (released when this process ends).
    kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)

    def worker() -> None:
        try:
            result["code"] = subprocess.call(argv)
        finally:
            root.after(0, root.destroy)

    root = tk.Tk()
    root.title("Messung läuft")
    root.geometry("360x80+40+40")
    tk.Label(root, text="Messung läuft - bitte den Rechner nicht benutzen.\n" + " ".join(argv)[:80],
             justify="left").pack(padx=10, pady=10)

    def keep_allowing() -> None:
        user32.AllowSetForegroundWindow(ASFW_ANY)
        root.after(500, keep_allowing)

    def start() -> None:
        root.lift()
        root.focus_force()
        keep_allowing()
        threading.Thread(target=worker, daemon=True).start()

    root.after(200, start)
    root.mainloop()
    return result["code"]


if __name__ == "__main__":
    sys.exit(main())
