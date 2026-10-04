# Launcher-Probe: XBE-Adressraum in einem 32-Bit-Windows-Prozess

Stand 2026-10-04. Quellcode und Build-Skripte: `analysis/loader/toolchain_probe/`.

## Toolchain

- MSVC 14.44.35207 x86 (VS 2022 Build Tools), über `vcvars32.bat`. Windows SDK 10.0.26100, MASM `ml.exe`, `dumpbin`.
- `cmd /c <datei>` braucht den **vollen Pfad**, und im Skript muss `cd /d "%~dp0"` stehen. Sonst findet `cmd` die Datei nicht. Die Meldung zu `vswhere.exe` beim Aufruf von `vcvars32.bat` ist harmlos.
- Außerdem verfügbar: CMake, Ninja, clang-cl 23 (LLVM), Git.

## Ergebnisse

| Test | Ergebnis |
|---|---|
| `VirtualAlloc(0x10000, …)` in einem normalen Prozess | **schlägt fehl** (Fehler 487). Windows legt Umgebungsblock und Prozessparameter in diesen Bereich, bevor eigener Code läuft. |
| `VirtualAlloc(0x80000000, 64 MB)` mit `/LARGEADDRESSAWARE` | funktioniert (WOW64 auf x64 hat 4 GB Adressraum) |
| EXE mit `/BASE:0x10000 /FIXED /DYNAMICBASE:NO` | wird genau bei 0x10000 geladen |
| Linker-Optionen per `#pragma comment(linker, …)` | `/FIXED`, `/DYNAMICBASE:NO` und `/LARGEADDRESSAWARE` werden dort **ignoriert** (LNK4229). Sie müssen auf die Kommandozeile. |
| `VirtualProtect` der PE-Header-Seite 0x10000 auf RWX | funktioniert. Der XBE-Header passt hinein (`SizeOfHeaders` = 0x968). |
| Code im Platzhalter ausführen | funktioniert |

## Platzhalter genau ab 0x11000

Das XBE-Image belegt 0x10000–0x31A300 (Retail EU) bzw. 0x31A460 (Prototyp). Der Header muss bei 0x10000 liegen, `.text` beginnt bei 0x11000.

- Der MSVC-Linker legt Code-Sektionen **vor** alle anderen, unabhängig vom Namen.
- Ein C-Array in `.text$a` landet nicht vor dem Code. Der Compiler markiert es als uninitialisierte Daten bzw. COMDAT mit Ausrichtung 1, also mit anderen Attributen. Der Linker legt dann eine zweite `.text`-Sektion an (LNK4078).
- **Lösung:** Ein MASM-Modul (`placeholder.asm`) definiert den Platzhalter als Code-Segment `ALIAS(".text$a")` mit `READ EXECUTE 'CODE'`. Der Linker vereint es mit `.text` und ordnet es vor `.text$mn` ein.
  Ergebnis: `xbe_space` = **0x11000–0x31B000**, der Launcher-Code folgt dahinter. Die EXE ist etwa 3,2 MB groß, weil der Platzhalter mit 0xCC gefüllt im File steht.

## Folgerungen für den Loader

1. **Launcher-EXE:** gelinkt mit `/BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE`. Zuerst kommt der MASM-Platzhalter `.text$a`, er sollte großzügig bemessen sein (beide Builds enden vor 0x31B000). Danach folgt minimaler Code, der die Host-DLL lädt und in sie springt.
2. **Host-DLL:** enthält Kernel-HLE, XDK-Ersatz, Renderer usw. Sie liegt an beliebiger Adresse oberhalb des XBE-Bereichs.
3. Der XBE-Header überschreibt die PE-Header-Seite bei 0x10000. **Offen:** wie sich das auf die SEH-Prüfung von Windows auswirkt (`RtlIsValidHandler` liest die PE-Header des Moduls, in dem ein Handler liegt). Das muss Meilenstein 1 testen, zum Beispiel mit einer XBE-Funktion, die `__try`/`__except` nutzt.
4. Speicher ab 0x80000000 lässt sich für zusammenhängenden bzw. physischen Xbox-Speicher reservieren.
