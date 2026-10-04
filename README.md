# Futurama (Xbox, 2003) – Dekompilierung & PC-Port

Ziel: Das Spiel läuft nativ auf x86-PCs mit einem Vulkan- oder D3D12-Renderer.

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

## Port-Strategie (Vorschlag)

Die Xbox-CPU ist ein Pentium III, der Spielcode ist also bereits x86-32. Deshalb bietet sich der inkrementelle Weg an, mit dem z. B. re3 gearbeitet hat:

1. **Analyse.** Ghidra mit XbSymbolDatabase, damit die XDK-Funktionen benannt werden. NetImmerse-Klassen werden über die Klassennamen und Vtables rekonstruiert, außerdem werden Skript-VM und `.ful`/`.dbg`-Format entschlüsselt.
2. **Host-Loader (32 Bit, Windows).** Er mappt die XBE-Sektionen an ihre Originaladressen (Basis 0x10000) und stellt die xboxkrnl-Imports bereit (Dateien, Speicher, Threads, Events). Die erkannten XDK-Funktionen werden durch PC-Implementierungen ersetzt: D3D8 durch den eigenen Renderer, DSOUND durch XAudio2, XInput durch XInput/SDL, XMV durch einen Videodecoder. Damit läuft das Spiel früh auf dem PC.
3. **Inkrementelle Dekompilierung.** Spielfunktionen werden einzeln durch C++ ersetzt (Hooks auf die Originaladressen). Jeder Schritt lässt sich sofort im laufenden Spiel testen.
4. **Renderer.** `NiXBoxRenderer` wird durch einen eigenen NetImmerse-Renderer für Vulkan/D3D12 ersetzt. Das ist sauberer, als NV2A-Spezialitäten (Pushbuffer, Register-Combiner, swizzled Texturen) auf D3D8-Ebene nachzubilden.
5. **Ablösung.** Sobald aller Code in C++ vorliegt, fallen Loader und XBE weg. Dann ist auch ein 64-Bit-Build möglich. Die Assets werden aus dem eigenen Disc-Image gelesen.
