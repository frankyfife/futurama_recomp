# Futurama (Xbox, 2003) – Dekompilierung & PC-Port

Ziel: Das Spiel läuft nativ auf x86-PCs mit einem Vulkan- oder D3D12-Renderer.

## Schnellstart für Spieler

Stand M3c: Legal-Bild, Intro-Bildschirme und das Frontend-Menü laufen mit 60 fps und Kantenglättung; Ton, Filme und Eingabe fehlen noch.

1. Paket bauen oder entpacken (`dist\FuturamaPC\`, siehe unten) und **`FuturamaLauncher.exe`** starten. Voraussetzung: Windows 10 oder 11 (64 Bit), Grafikkarte mit Direct3D 11 und ein Abbild (ISO) **Ihrer eigenen** europäischen Futurama-Disc für die Xbox. Das Paket enthält keine Spieldaten.
2. Beim ersten Start führt ein Assistent durch die Einrichtung: Oberflächensprache, Spielabbild (wird sofort geprüft; nur die europäische Verkaufsversion startet), Sprache des Spiels, Bildschirm und Fenster oder randloses Vollbild, Bildqualität.
3. Bildqualität: **Klassisch** (640×480 wie auf der Xbox), **Standard** (Bildschirmhöhe, 4× MSAA – empfohlen) oder **Maximum** (dazu 2× Supersampling, 8× MSAA und helligkeitsrichtige Kantenglättung; passt sich an den Grafikspeicher an). Alles lässt sich später im Reiter „Grafik“ einzeln ändern.
4. **„Spielen“** – der Launcher verschwindet und zeigt nach dem Spiel das Ergebnis (bei Fehlern mit Ursache und Protokollende). Im Spiel: Alt+Eingabetaste = Vollbild, F12 = Screenshot.

Einstellungen, Protokolle, Screenshots und Spielstände liegen in `%LOCALAPPDATA%\FuturamaPC\`; mit einer leeren Datei `portable.txt` neben `futurama.exe` im Spielordner. Ausführlich: [`docs/m3c_report.md`](docs/m3c_report.md) Abschnitt 3 und [`launcher/README.md`](launcher/README.md).

## Ausgangsmaterial

Die Disc-Images bleiben in `Y:\xbox games\Futurama (Jun 17, 2003 prototype)\`.

| | Prototyp (`Xbox - Futurama.bin`) | Retail EU (`Futurama (Europe) …iso`) |
|---|---|---|
| XBE-Build | 2003-06-17 12:41 UTC | 2003-07-01 16:47 UTC |
| Signatur | Debug-Keys (Devkit) | Retail |
| Title-ID | VU-021 (Vivendi Universal) | FI-002 |
| Region / Video | NTSC (`FutNTSC0x.img`, `*-NTSC.xmv`) | PAL, 5 Sprachen (`FutPAL0x.img`, `SysPAL.img`) |
| `.text` | 0x223440 | 0x2232C0 |
| Build-Pfad | `C:\Code\Futurama\Futurama\Build\XBoxMaster\Futurama.exe` | gleich |

- XDK **5233** (QFE 1): XAPILIB, D3D8, DSOUND, XMV, LIBCMT, XGRAPHC, statisch gelinkt.
- Engine: **NetImmerse 4.2.1.0** (NiMain, NiAnimation, NiPortal, NiSystem, NiXBox) mit UDS-Erweiterungen (`NiUDSNode`, `NiUDSObject`, …).
- **Keine** PDB, kein MSVC-RTTI, nur Release-Bibliotheken. Der Prototyp liefert keine zusätzlichen Symbole.
- Die Levelskripte `scripts/levelX-Y.ful` haben jeweils eine **`.dbg`-Symboldatei** (Variablen- und Funktionsnamen), und zwar auf beiden Discs.
- Archivformat `.img`: siehe `tools/futimg.py`.

## Verzeichnisse

```
tools/          Python-Tools (XDVDFS, .img, XBE-Header, Strings, Ghidra-Starter)
orig/proto/     default.xbe + Dateilisten + extrahierte Skripte/Textdateien (Prototyp)
orig/retail_eu/ default.xbe + Dateilisten (Retail EU)
ghidra/         Ghidra-Projekt "Futurama" (/proto, /retail_eu)
src/            Loader: Launcher, Host-DLL (core, kernel, vfs, xapi, gfx, audio, video, input), gemeinsame Gast-Strukturen
tests/          Unit-Tests des Loaders (doctest)
docs/           Loader-Design und Meilenstein-Berichte
run/            Benutzerdaten und Logs der Testläufe (eine INI pro Lauf)
```

Spieldaten und XBEs gehören nicht in ein öffentliches Repository.

## Tools

```
python tools/xdvdfs.py ls  <image>                 # Dateien eines Xbox-Images auflisten
python tools/xdvdfs.py x   <image> <out> [suffix…] # extrahieren
python tools/futimg.py ls  <file.img>              # .img-Archiv auflisten
python tools/futimg.py x   <file.img> <out> [teil…]
python tools/xbeinfo.py    <default.xbe>           # Header, Libs, Sektionen
python tools/ghidra_headless.py <AnalyzeHeadless-Argumente…>
```

`ghidra_headless.py` ruft Ghidra direkt über Java auf, weil `analyzeHeadless.bat` an Klammern im Pfad scheitert. Ghidra lehnt außerdem Kommas in Projektpfaden ab. Unterordner werden im Projektargument angegeben (`Futurama/proto`), eine Option `-folderPath` gibt es nicht.

## Ghidra-Datenbank neu bauen

```
python tools/build_ghidra.py retail_eu
```

Der Lauf ist reproduzierbar (Import mit `-overwrite`) und dauert ein paar Minuten:
1. Import und Auto-Analyse mit ghidra-xbe. XbSymbolDatabase benennt dabei die XDK-Funktionen.
2. `CreateFunctionsFromTable.java` legt Funktionen für alle CRT-Initialisierer an (Tabellen aus `XAPILIB::_cinit`).
3. `tools/niclasses.py` rekonstruiert die NetImmerse-Klassen, `ApplySymbols.java` trägt sie ein (Namespaces, `__thiscall`, Klassen-Structs, Vtables).

Manuelle Arbeit in der Ghidra-Oberfläche geht bei einem Neubau verloren. Dauerhafte Namen gehören deshalb in eine Symbol-TSV.

## Stand der Analyse (Retail EU)

- 30.403 Funktionen in `.text`, davon gut 24.300 Initialisierer-Stubs.
- **NetImmerse-Klassen:** 130 Klassen mit Name, Größe und Elternklasse aus NiRTTI (`{name, size, parent}`, Konstruktor bei 0x141E60). Dazu 128 Vtables, 53 Konstruktoren (61 sind in `CreateObject` eingebettet) und 934 benannte virtuelle Methoden (`Klasse::vfNN`).
- `NiStream::RegisterLoader` (0x1087D0) wird in jeder Übersetzungseinheit für alle Klassen aufgerufen. Daraus ergeben sich **407 Quelldateien**, die die NetImmerse-Header einbinden.
- Renderer-Schnittstelle: `NiRenderer` (72 virtuelle Methoden) und `NiXBoxRenderer` (75 Methoden, 1744 Byte).
- Spielklassen mit NiRTTI: `PhysicController`, `AimController`, `LightSource`/`LightTarget`, `LMSource`/`LMTarget`, `MasterPropertyAccumulator`.
- Eintrittspunkt `entry` (0xF84DD) startet einen Thread bei 0xF8469, `XapiInitProcess` liegt bei 0xF92C0.

## Loader (PC-Port, Meilensteine M3, M3b und M3c erreicht)

Der Loader führt das Original-XBE nativ auf Windows aus. `futurama.exe` ist ein Launcher mit fester Basis 0x10000. `futurama_host.dll` stellt den xboxkrnl-Kernel als HLE bereit, mappt `default.xbe` direkt aus dem eigenen ISO und startet es. Stand M3: Das Spiel zeigt Bild – Legal-Bild, Tech-Notice (`NI_Splash_PAL.nif`), Ladebild und das animierte Frontend-Menü, noch ohne Ton, Filme und Eingabe. `NiPCRenderer` ersetzt `NiXBoxRenderer` an der NiRenderer-Schnittstelle und zeichnet über eine D3D11-RHI mit nachgebildeter D3D8-Fixed-Function (Property→State, Licht, Texturstufen, CPU-Skinning). Die Szenen stimmen mit xemu-Aufnahmen des Originals überein (Legal/Tech-Notice texelgenau, statische Frontend-Pixel zu 98 % exakt), 0 Debug-Layer-Meldungen. **Seit M3b läuft der Port mit 60 fps:** Die Spiellogik tickt mit den NTSC-Zeitwerten (60 Hz) bei unveränderten PAL-Inhalten (`[game] tick_rate = 60`, Standard; `50` = Original-PAL für Vergleiche), und der Bildtakt koppelt sich an die Anzeige (`[render] pacing = auto`: bei 120 Hz genau 2 Vblanks pro Bild). Darunter liegen aus M1/M2 die Archive, Fibers, asynchrones I/O mit Gast-APCs und stille Stubs für DSOUND/XMV/XPP. **Seit M3c** gibt es den Qt-Launcher `FuturamaLauncher.exe` (Einstellungen, ISO-Prüfung, Spielstart; Deutsch/Englisch) und eine höhere Bildqualität: Renderauflösung bis zur vollen Bildschirmhöhe (4:3 mit Seitenbalken), MSAA 2×/4×/8× mit Alpha-to-Coverage, Supersampling (Render-Skala 1,0–2,0), helligkeitsrichtige Kantenglättung, anisotrope Filterung und Monitorwahl. „Klassisch“ (640×480 ohne Kantenglättung) bleibt bitgleich zu M3. Nächste Meilensteine: M3d (bis 120 Bilder pro Sekunde), M4 (Eingabe).

- Design: [`docs/loader_design.md`](docs/loader_design.md) (Entscheidungen E1–E12, Abweichungen in Abschnitt 8)
- 60 fps (Zeitbasis, Takt, Konfiguration, Belege): [`docs/m3b_60hz_report.md`](docs/m3b_60hz_report.md)
- Launcher und Bildqualität (M3c): Bericht [`docs/m3c_report.md`](docs/m3c_report.md), [`launcher/README.md`](launcher/README.md), Abweichungen in [`docs/loader_design.md`](docs/loader_design.md) 8.5–8.8, Renderer-Vertrag [`src/host/gfx/GFX_CONTRACT.md`](src/host/gfx/GFX_CONTRACT.md) Abschnitt 12
- Stand, Bauen, Konfiguration und Prüfung: [`docs/m3_report.md`](docs/m3_report.md) (Abschnitt 3), frühere Meilensteine: [`docs/m2_report.md`](docs/m2_report.md), [`docs/m1_report.md`](docs/m1_report.md)
- Schnittstellen für Entwickler: [`src/INTERFACES.md`](src/INTERFACES.md), Grafikvertrag: [`src/host/gfx/GFX_CONTRACT.md`](src/host/gfx/GFX_CONTRACT.md)

Kurzfassung (MSVC x86, CMake, Ninja; `build.cmd` immer mit vollem Pfad aufrufen):

```
cmd /c "D:\FuturamaDecomp\build.cmd" build\x86-debug Debug
cmd /c "D:\FuturamaDecomp\build_launcher.cmd" build\launcher-rel Release test     (Qt-Launcher, x64)
python tools\package.py --game-build build\x86-relwithdebinfo --launcher-build build\launcher-rel   (dist\FuturamaPC\)
build\x86-debug\bin\futurama.exe --config=<eigene futurama.ini>                (ISO-Pfad und userdata in der INI; F12 = Screenshot, Alt+Eingabetaste = Vollbild)
python tools\m3_check.py --exe build\x86-debug\bin\futurama.exe --config <ini> --runs 2 --fresh
python tools\m2_check.py --exe build\x86-debug\bin\futurama.exe --config <ini> --runs 3 --fresh
python tools\m1_check.py --exe build\x86-debug\bin\futurama.exe --config <ini mit stop_at = main> --runs 10 --xbe orig\retail_eu\default.xbe
python tools\m3c_check.py --exe build\x86-debug\bin\futurama.exe --config <Basis-ini> --fresh
```

Ohne `--config` liest `futurama.exe` seit M3c `%LOCALAPPDATA%\FuturamaPC\futurama.ini` (Benutzerdaten im selben Ordner); liegt `portable.txt` neben `futurama.exe`, stehen Konfiguration und Benutzerdaten im Programmordner. Werkzeug-Modi für den Launcher: `--print-config-schema=`, `--check-iso= --result=`, `--query-gpu=`, `--print-paths=` (`src/INTERFACES.md` 4.21). Für den xemu-Vergleich (`m3_check`) muss die INI die klassischen Werte setzen: `[render] resolution = 640x480`, `render_scale = 1.0`, `msaa = off`, `anisotropy = off`.

## Port-Strategie (Vorschlag)

Die Xbox-CPU ist ein Pentium III, der Spielcode ist also bereits x86-32. Deshalb bietet sich der inkrementelle Weg an, mit dem z. B. re3 gearbeitet hat:

1. **Analyse.** Ghidra mit XbSymbolDatabase, damit die XDK-Funktionen benannt werden. NetImmerse-Klassen werden über die Klassennamen und Vtables rekonstruiert, außerdem werden Skript-VM und `.ful`/`.dbg`-Format entschlüsselt.
2. **Host-Loader (32 Bit, Windows).** Er mappt die XBE-Sektionen an ihre Originaladressen (Basis 0x10000) und stellt die xboxkrnl-Imports bereit (Dateien, Speicher, Threads, Events). Die erkannten XDK-Funktionen werden durch PC-Implementierungen ersetzt: D3D8 durch den eigenen Renderer, DSOUND durch XAudio2, XInput durch XInput/SDL, XMV durch einen Videodecoder. Damit läuft das Spiel früh auf dem PC.
3. **Inkrementelle Dekompilierung.** Spielfunktionen werden einzeln durch C++ ersetzt (Hooks auf die Originaladressen). Jeder Schritt lässt sich sofort im laufenden Spiel testen.
4. **Renderer.** `NiXBoxRenderer` wird durch einen eigenen NetImmerse-Renderer für Vulkan/D3D12 ersetzt. Das ist sauberer, als NV2A-Spezialitäten (Pushbuffer, Register-Combiner, swizzled Texturen) auf D3D8-Ebene nachzubilden.
5. **Ablösung.** Sobald aller Code in C++ vorliegt, fallen Loader und XBE weg. Dann ist auch ein 64-Bit-Build möglich. Die Assets werden aus dem eigenen Disc-Image gelesen.
