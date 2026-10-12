#!/usr/bin/env python3
"""Assembles the player package dist\\FuturamaPC\\ (no game data).

Contents:
  futurama.exe, futurama_host.dll        from the game build (build.cmd, x86)
  FuturamaLauncher.exe + Qt runtime      from the launcher build (build_launcher.cmd, x64, windeployqt
                                         output incl. the app-local MSVC runtime of Release builds)
  LIESMICH.txt                           short German readme for players
  LICENSES\\Qt\\                           the GNU LGPL 3.0 and GPL 3.0 texts and a note on Qt and its source
                                         (LGPLv3 section 4; review M3c finding 14) - from launcher\\licenses\\
  portable.txt                           only with --portable (config and user data next to futurama.exe)

Usage:
  python tools\\package.py --game-build build\\x86-relwithdebinfo --launcher-build build\\launcher-rel
                           [--out dist\\FuturamaPC] [--portable] [--with-pdb] [--zip] [--allow-debug]

Safety checks (the package is meant to be passed on): no file with a game-data extension (ISO, XBE,
archives, media), no file above 200 MB, futurama.exe must be the real loader (>= 3 MB because of the
0x30A000-byte XBE placeholder - the launcher test double is tiny), and Debug builds are refused unless
--allow-debug (their runtime is not redistributable). The output directory is only replaced when it is
below dist\\ or carries the marker file written by this script.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MARKER = ".futuramapc-package"
MIN_LOADER_SIZE = 3 * 1024 * 1024
MAX_FILE_SIZE = 200 * 1024 * 1024
# Extensions that would mean game data (disc images, executables of the game, archives, media).
FORBIDDEN_EXTENSIONS = {
    ".iso", ".xiso", ".xbe", ".img", ".xmv", ".xwb", ".xsb", ".wma", ".bik", ".nif", ".kf", ".kfm",
    ".dds", ".tga", ".xpr", ".wav", ".mp4", ".log", ".dmp", ".ini",
}
# What is taken from the launcher build's bin directory (an allow-list: build directories also hold tests,
# test output and files of older windeployqt runs). FuturamaLauncher.exe needs Qt6Core/Gui/Widgets, the
# windows (+ offscreen for --screenshots) platform plugin and the modern Windows style.
LAUNCHER_FILES = ("FuturamaLauncher.exe",)
LAUNCHER_DLL_PATTERNS = ("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "vcruntime140*.dll", "msvcp140*.dll",
                         "concrt140.dll", "vccorlib140.dll")
LAUNCHER_PLUGIN_DIRS = ("platforms", "styles")
# Review M3c finding 14: Qt 6 is used under the LGPL 3.0, which requires the texts of the GNU LGPL and GPL with the
# library. The official FSF texts live in launcher\licenses\ (copied verbatim).
LICENSE_SOURCE = REPO / "launcher" / "licenses"
LICENSE_FILES = ("LGPL-3.0.txt", "GPL-3.0.txt")
QT_VERSION = "6.8.3"


def qt_notice() -> str:
    return (
        "Qt " + QT_VERSION + " - Lizenzhinweis / license notice\n"
        "==========================================\n"
        "\n"
        "FuturamaLauncher.exe verwendet die Qt-Bibliotheken Qt6Core.dll, Qt6Gui.dll und Qt6Widgets.dll sowie die\n"
        "Qt-Plugins in platforms\\ und styles\\ unter der GNU Lesser General Public License, Version 3\n"
        "(LGPL-3.0.txt; sie ergänzt die GNU General Public License, Version 3, GPL-3.0.txt).\n"
        "Die Bibliotheken sind dynamisch eingebunden und unverändert; sie dürfen durch eigene, kompatible\n"
        "Builds ersetzt werden.\n"
        "\n"
        "FuturamaLauncher.exe uses the Qt libraries Qt6Core.dll, Qt6Gui.dll and Qt6Widgets.dll and the Qt plugins in\n"
        "platforms\\ and styles\\ under the GNU Lesser General Public License, version 3 (LGPL-3.0.txt, which\n"
        "supplements the GNU General Public License, version 3, GPL-3.0.txt). The libraries are linked dynamically\n"
        "and unmodified; they may be replaced by compatible builds of your own.\n"
        "\n"
        "Qt: Copyright (C) The Qt Company Ltd. and other contributors.\n"
        "Quelltext / source code of Qt " + QT_VERSION + ":\n"
        "  https://download.qt.io/archive/qt/6.8/" + QT_VERSION + "/single/\n"
        "Lizenzen / licensing: https://www.qt.io/licensing\n"
    )


def bin_dir(build: Path) -> Path:
    return build / "bin" if (build / "bin").is_dir() else build


def git_description() -> str:
    try:
        rev = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"], capture_output=True,
                             text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True, check=True).stdout.strip()
        return rev + (" (mit lokalen Änderungen)" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unbekannt"


def readme_text(portable: bool) -> str:
    today = datetime.date.today().isoformat()
    location = (
        "  Portabler Modus (portable.txt liegt neben futurama.exe): Einstellungen, Protokolle, Screenshots und\n"
        "  Spielstände liegen im Spielordner.\n"
        if portable
        else "  Gespeichert in %LOCALAPPDATA%\\FuturamaPC\\futurama.ini; Protokolle, Screenshots und Spielstände\n"
        "  liegen ebenfalls in %LOCALAPPDATA%\\FuturamaPC\\.\n"
        "  Portabler Modus: eine leere Datei portable.txt neben futurama.exe legen - dann liegt alles im\n"
        "  Spielordner.\n"
    )
    return (
        "FuturamaPC - PC-Port der Xbox-Version von Futurama\n"
        "===================================================\n"
        "\n"
        f"Stand: {today}, Quellstand {git_description()}\n"
        "\n"
        "Was ist das?\n"
        "  Ein Programm, das die Xbox-Version von Futurama (europäische Verkaufsversion) auf dem PC\n"
        "  ausführt. Dieses Paket enthält KEINE Spieldaten. Sie brauchen ein Abbild (ISO) Ihrer eigenen\n"
        "  Disc.\n"
        "\n"
        "Starten\n"
        "  1. FuturamaLauncher.exe starten.\n"
        "  2. Beim ersten Start führt ein Assistent durch die Einrichtung: ISO wählen (es wird geprüft),\n"
        "     Sprache des Spiels, Anzeige (Monitor, Fenster oder randloses Vollbild) und Bildqualität\n"
        "     (Klassisch, Standard, Maximum).\n"
        "  3. „Spielen“ drücken. Der Launcher verschwindet und erscheint wieder, wenn das Spiel endet.\n"
        "\n"
        "Einstellungen\n"
        + location
        + "  Alle Werte lassen sich im Launcher ändern (Reiter Spiel, Grafik, Audio, Steuerung, Pfade, Erweitert).\n"
        "\n"
        "Steuerung\n"
        "  Xbox-Controller (XInput) sowie Tastatur und Maus steuern gemeinsam Spieler 1. Standard: WASD\n"
        "  laufen, Maus Kamera, Eingabe/linke Maustaste A, Leertaste B, F/rechte Maustaste X, E Y, Esc Pause\n"
        "  (in Menüs: zurück). Ein Klick ins Spielfenster fängt die Maus, Alt+Tab gibt sie frei. Alle Tasten\n"
        "  sind im Launcher-Reiter „Steuerung“ frei belegbar; dort gibt es auch einen Controller-Test.\n"
        "  F12 oder Druck: Screenshot. Alt+Eingabe: Fenster/Vollbild.\n"
        "\n"
        "Ton\n"
        "  Ausgabe über das Windows-Standardgerät oder ein im Reiter „Audio“ gewähltes Gerät (Stereo oder\n"
        "  5.1), mit Gesamtlautstärke, Raumhall und einem Testton. Musik-, Effekt- und Sprachlautstärke stellt\n"
        "  man wie auf der Xbox im Spiel unter „Optionen“ ein. Ohne Ausgabegerät (z. B. Fernseher aus) läuft\n"
        "  das Spiel stumm weiter und schaltet den Ton ein, sobald ein Gerät da ist. Filme fehlen noch.\n"
        "\n"
        "Probleme?\n"
        "  Nach dem Spiel zeigt der Launcher den Rückgabecode mit Erklärung und die letzten 30 Zeilen des\n"
        "  Protokolls (Knöpfe „Protokoll öffnen“ und „Ordner öffnen“). Das vollständige Protokoll steht in\n"
        "  <Benutzerdaten>\\logs\\futurama.log.\n"
        "\n"
        "Voraussetzungen\n"
        "  Windows 10 oder 11 (64 Bit), Grafikkarte mit Direct3D 11.\n"
        "\n"
        "Inhalt\n"
        "  FuturamaLauncher.exe            Einstellungen und Start\n"
        "  futurama.exe, futurama_host.dll Spielstarter\n"
        "  Qt6*.dll, platforms\\, styles\\  Qt 6 (LGPL-3.0); die Qt-Bibliotheken sind dynamisch eingebunden\n"
        "                                  und dürfen ersetzt werden\n"
        "  LICENSES\\Qt\\                    Lizenztexte (GNU LGPL 3.0 und GPL 3.0) und Hinweis zum Qt-Quelltext\n"
        "  vcruntime140*.dll, msvcp140*.dll Microsoft Visual C++ Laufzeit\n"
    )


def check_tree(root: Path) -> list[str]:
    problems = []
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(root)
        if path.suffix.lower() in FORBIDDEN_EXTENSIONS:
            problems.append(f"{rel}: file type not allowed in the package")
        if path.stat().st_size > MAX_FILE_SIZE:
            problems.append(f"{rel}: larger than {MAX_FILE_SIZE // (1024 * 1024)} MB")
    return problems


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game-build", default="build/x86-relwithdebinfo", help="game build dir (or its bin dir)")
    parser.add_argument("--launcher-build", default="build/launcher-rel", help="launcher build dir (or its bin dir)")
    parser.add_argument("--out", default="dist/FuturamaPC", help="output directory")
    parser.add_argument("--portable", action="store_true", help="add portable.txt")
    parser.add_argument("--with-pdb", action="store_true", help="also copy the PDB files")
    parser.add_argument("--zip", action="store_true", help="also write <out>.zip")
    parser.add_argument("--allow-debug", action="store_true", help="accept a Debug launcher build")
    args = parser.parse_args()

    game_bin = bin_dir((REPO / args.game_build).resolve())
    launcher_bin = bin_dir((REPO / args.launcher_build).resolve())
    out = (REPO / args.out).resolve()

    errors = []
    loader = game_bin / "futurama.exe"
    host = game_bin / "futurama_host.dll"
    launcher = launcher_bin / "FuturamaLauncher.exe"
    for required in (loader, host, launcher):
        if not required.is_file():
            errors.append(f"missing: {required}")
    if loader.is_file() and loader.stat().st_size < MIN_LOADER_SIZE:
        errors.append(f"{loader} is {loader.stat().st_size} bytes - not the real loader (launcher test double?)")
    if (launcher_bin / "Qt6Cored.dll").is_file() and not args.allow_debug:
        errors.append(f"{launcher_bin} is a Debug launcher build (debug runtime is not redistributable); "
                      "build Release with build_launcher.cmd <dir> Release or pass --allow-debug")
    if not (launcher_bin / "platforms").is_dir():
        errors.append(f"{launcher_bin}\\platforms missing - run build_launcher.cmd (windeployqt) first")
    if errors:
        for e in errors:
            print("package: " + e)
        return 1
    if not (launcher_bin / "vcruntime140.dll").is_file():
        print("package: warning: no app-local vcruntime140.dll (players need the VC++ 2015-2022 x64 runtime)")

    dist_root = (REPO / "dist").resolve()
    if out.exists():
        inside_dist = dist_root in out.parents
        if not inside_dist and not (out / MARKER).is_file():
            print(f"package: {out} exists and was not made by this script - refusing to replace it")
            return 1
        shutil.rmtree(out)
    out.mkdir(parents=True)
    (out / MARKER).write_text("written by tools/package.py\n", encoding="utf-8")

    # Launcher + Qt runtime (allow-list).
    for name in LAUNCHER_FILES:
        shutil.copy2(launcher_bin / name, out / name)
        if args.with_pdb and (launcher_bin / name).with_suffix(".pdb").is_file():
            shutil.copy2((launcher_bin / name).with_suffix(".pdb"), out / Path(name).with_suffix(".pdb").name)
    for pattern in LAUNCHER_DLL_PATTERNS:
        for dll in sorted(launcher_bin.glob(pattern)):
            shutil.copy2(dll, out / dll.name)
    for plugin_dir in LAUNCHER_PLUGIN_DIRS:
        shutil.copytree(launcher_bin / plugin_dir, out / plugin_dir, ignore=shutil.ignore_patterns("*.pdb"))
    missing_qt = [n for n in ("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll") if not (out / n).is_file()]
    if missing_qt:
        print("package: missing Qt libraries in the launcher build: " + ", ".join(missing_qt))
        return 1
    # Game loader.
    shutil.copy2(loader, out / loader.name)
    shutil.copy2(host, out / host.name)
    if args.with_pdb:
        for pdb in ("futurama.pdb", "futurama_host.pdb"):
            if (game_bin / pdb).is_file():
                shutil.copy2(game_bin / pdb, out / pdb)
    (out / "LIESMICH.txt").write_text(readme_text(args.portable), encoding="utf-8-sig", newline="\r\n")
    licenses = out / "LICENSES" / "Qt"
    licenses.mkdir(parents=True)
    for name in LICENSE_FILES:
        source = LICENSE_SOURCE / name
        if not source.is_file():
            print(f"package: missing license text {source}")
            return 1
        text = source.read_text(encoding="utf-8").replace("\r\n", "\n")
        (licenses / name).write_text(text, encoding="utf-8", newline="\r\n")
    (licenses / "README.txt").write_text(qt_notice(), encoding="utf-8-sig", newline="\r\n")
    if args.portable:
        (out / "portable.txt").write_text("", encoding="utf-8")

    problems = check_tree(out)
    if problems:
        for p in problems:
            print("package: " + p)
        print("package: FAILED - the package must not contain game data")
        return 1

    files = sorted(p for p in out.rglob("*") if p.is_file())
    total = sum(p.stat().st_size for p in files)
    print(f"package: {out} - {len(files)} files, {total / (1024 * 1024):.1f} MB")
    for name in ("FuturamaLauncher.exe", "futurama.exe", "futurama_host.dll"):
        print(f"  {name:22} {sha256(out / name)}")
    if args.zip:
        archive = out.with_suffix(".zip")
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for p in files:
                if p.name == MARKER:
                    continue
                z.write(p, Path(out.name) / p.relative_to(out))
        print(f"package: {archive} ({archive.stat().st_size / (1024 * 1024):.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
