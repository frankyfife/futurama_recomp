"""Shared helpers for the code generators in tools/gen.

Every generator writes its output with LF line endings and UTF-8 encoding. In --check mode
nothing is written; the generator exits with status 1 if a checked-in file differs from what
it would generate (used by the CMake target fut_gen and by CI).
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ANALYSIS = ROOT / "analysis"
SRC = ROOT / "src"


def parse_args(description):
    ap = argparse.ArgumentParser(description=description)
    ap.add_argument("--check", action="store_true",
                    help="do not write; exit 1 if a generated file is out of date")
    return ap.parse_args()


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT)).replace("\\", "/")
    except ValueError:
        return str(path)


def emit(path: Path, text: str, check: bool) -> bool:
    """Write `text` to `path` (or compare in check mode). Returns True if the file is current."""
    old = path.read_bytes().decode("utf-8") if path.exists() else None
    if check:
        if old != text:
            print(f"OUT OF DATE: {rel(path)} (run: python {rel(Path(sys.argv[0]).resolve())})")
            return False
        print(f"up to date: {rel(path)}")
        return True
    if old == text:
        print(f"unchanged: {rel(path)}")
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))
    print(f"wrote: {rel(path)}")
    return True


def c_string(s: str) -> str:
    """C/C++ string literal for an ASCII-ish string."""
    out = []
    for ch in s:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif 0x20 <= o < 0x7F:
            out.append(ch)
        else:
            out.append("?")
    return '"' + "".join(out) + '"'


def comment_text(s: str) -> str:
    """Single-line comment payload (no newlines, ASCII only)."""
    s = " ".join(s.split())
    return "".join(ch if 0x20 <= ord(ch) < 0x7F else "?" for ch in s)


def header(generator: str, sources: list[str]) -> str:
    src = ", ".join(sources)
    return (f"// GENERATED FILE - DO NOT EDIT.\n"
            f"// Generator: tools/gen/{generator} (run with --check to verify).\n"
            f"// Sources:   {src}")
