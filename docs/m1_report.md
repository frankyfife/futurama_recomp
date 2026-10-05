# Meilenstein M1: Bericht

Stand: 2026-10-05 (Phase 5, nach Behebung der Review-Befunde) · Ziel-XBE: `orig\retail_eu\default.xbe` (Retail EU, SHA-1 `58f76de5bc739bf016d74c1d7f23e348f65ea4b4`) · Verbindliches Design: [`docs/loader_design.md`](loader_design.md) (E1–E12, Abweichungen in Abschnitt 8)

## 1. Ergebnis in Kürze

**`main` (0xD29B0) wird erreicht.** Der Loader mappt das Original-XBE bei 0x10000, löst alle 111 Kernel-Thunks auf, patcht die 38 fs:-Stellen und startet das Spiel über den Original-Bootpfad:

```
entry 0xF84DD → PsCreateSystemThreadEx (HLE) → XapiThreadStartup 0xF99DA → 0xF8469
  → XapiInitProcess → _rtinit → _cinit → main 0xD29B0
```

Mit `--stop-at=main` loggt der Hook auf `main` und beendet den Prozess mit Exitcode 0. Alle sechs Exit-Kriterien aus Design-Abschnitt 6 sind erfüllt. Geprüft wurde mit `tools\m1_check.py` über je zehn Läufe in drei Builds: MSVC Debug (mit vorhandenem und mit leerem `userdata`), MSVC RelWithDebInfo (leeres `userdata`) und clang-cl Debug (leeres `userdata`). Die elf Review-Befunde wurden geprüft. Zehn sind voll angenommen und behoben. Befund 11 ist angenommen und behoben, zwei seiner Teilaussagen sind aber widerlegt (Abschnitt 7).

## 2. Was gebaut wurde

### 2.1 Struktur

```
D:\FuturamaDecomp\
  CMakeLists.txt, CMakePresets.json, build.cmd, .clang-format
  src\launcher\     futurama.exe: placeholder.asm (.text$a, 0x30A000 Byte ab 0x11000), launcher.c (ohne CRT),
                    futurama.manifest (longPathAware)                                          ~140 Zeilen
  src\shared\       Gast-/Launcher-Strukturen: launcher_info.h, xbe_format.h, xbox_types.h, nt_status.h, win32.h
  src\host\         futurama_host.dll (Export HostMain)
    core\           Config, Log, Crash-Handler, XBE-Loader, Polyglott-Header, Kernel-Thunks, fs-Patcher,
                    Hooks + Probes, Gast-Threads, Selbsttests, HostMain-Stufen                ~4 500 Zeilen
    kernel\         xboxkrnl-HLE (76 registrierte Slots), Handles, APCs, IRQL, Timer, SHA-1   ~5 700 Zeilen
    kernel\gen\     kernel_table.gen.{h,cpp}: Exporttabelle + 98 Logging-Stubs (generiert)
    vfs\            Objekt-Namensraum, Symlinks, XDVDFS-ISO-Gerät, HostDirDevice              ~2 100 Zeilen
    xapi\           XAPI-Ersatz (QPC/QPF, OutputDebugStringA, XMountUtilityDrive, XLaunchNewImageA),
                    sechs Beobachter-Probes des Bootpfads
    platform\       Fenster-Stufe (leer bis M3), Message-Loop, Watchdog
    asm\            guest_call.asm (Host→Gast-Aufrufe), asm_probe.asm
    gen\            xbe_functions.gen.inc (7 013 Funktionsstarts), fs_patch_table.gen.inc (38 Stellen)
  src\game\         Platzhalter für dekompilierten Code (fut_game)
  tests\            doctest-Unit-Tests: Core, Kernel, VFS/IO (48 Testfälle, 1 537 Assertions)
  tools\gen\        Generatoren (gen_kernel_table.py, gen_fs_patches.py, gen_functions.py, gen_all.py --check)
  tools\m1_check.py M1-Exit-Prüfung (N Läufe, sechs Kriterien, Log-Vergleich)
```

### 2.2 Komponenten und Ablauf

| Komponente | Umsetzung |
|---|---|
| Launcher (E1) | `futurama.exe` mit `/BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE /SAFESEH:NO`, MASM-Platzhalter `.text$a` exakt bei 0x11000. Er reserviert fünf TLS-Slots (S_TLS, S_KTHREAD, S_PRCB, S_IRQL, S_DPC) vor dem Laden der DLL und prüft die Lage des Platzhalters, dann ruft er `HostMain`. |
| HostMain-Stufen | 1 Config+Log → 2 Crash-Handler (VEH-Ringpuffer, Filter, Minidump, MMIO-Falle 0xFD000000–0xFEFFFFFF) → 3 Kernel-Init → 4 XBE laden (aus dem ISO, SHA-1-Prüfung) → 5 Mappen (RWX, BSS genullt, 10 Sektionen) → 6 PE/XBE-Polyglott (E2) → 7 Kernel-Thunks → 8 fs-Patches (E3) → 9 Hooks → 10 Selbsttests → 11 VFS → 12 Fenster (leer) → 13 Boot-Thread adoptieren und `entry` aufrufen → Message-Loop |
| Kernel (E4) | 111 Slots aus der generierten Tabelle. Konvention, Purge und Registerargumente jeder Implementierung werden per `static_assert` gegen `kernel_imports.tsv` geprüft. Nicht registrierte Slots bekommen Logging-Stubs mit korrektem Purge und einem Fehlerwert passend zur Rückgabeart (Abschnitt 5). |
| fs-Segment (E3) | 38 disp32-Retargets auf `TEB.TlsSlots[k]` (`fs:[0xE10+4k]`), fs:[0] (SEH) bleibt. |
| XAPI (E5) | Originalcode, ersetzt sind nur QPC/QPF, `OutputDebugStringA`, `XMountUtilityDrive`, `XLaunchNewImageA`. |
| Hooks (E11) | 5-Byte-Detours, gegen die Funktionstabelle validiert, einzeln abschaltbar (`[hooks] disable`). Dazu kommen sechs Beobachter-Probes mit Trampolin (Abweichung A1). |
| VFS (E8) | `\Device\CdRom0` = XDVDFS direkt aus dem ISO (59 Einträge), `\Device\Harddisk0\partition1` = `<userdata>\hdd\partition1`. Symlinks legt der Gast selbst an (`D:`, `T:`, `U:`, `Z:`). |
| Threads (E10) | Der Boot-Thread führt `entry` aus. Danach läuft genau ein Gast-Hauptthread (HLE `PsCreateSystemThreadEx`, 2-MiB-Stack). |
| Diagnose | Log `<userdata>\logs\futurama.log` (rotierend), VEH-Ringpuffer, EBP- und StackWalk64-Backtrace, Stack-Scan, Minidump, Watchdog (`[debug] timeout`). Stufenfehler und `core::Fatal` erscheinen auch auf stderr bzw. als MessageBox (A15). |

## 3. Bauen und Ausführen

### 3.1 Bauen

Toolchain: MSVC 14.44 x86 (VS 2022 Build Tools), CMake ≥ 3.28, Ninja, Windows SDK 10.0.26100, MASM. `build.cmd` immer mit vollem Pfad aufrufen: Das AutoRun von cmd wechselt sonst das Arbeitsverzeichnis.

```powershell
cmd /c "D:\FuturamaDecomp\build.cmd" build\x86-debug Debug
cmd /c "D:\FuturamaDecomp\build.cmd" build\x86-relwithdebinfo RelWithDebInfo
# zweiter Build für Warnungen (Design 5.2): clang-cl 32 Bit + ml.exe + link.exe
cmd /c "D:\FuturamaDecomp\build.cmd" build\x86-clang-debug Debug all clang
# zusätzliche CMake-Optionen über die Umgebung, z. B. Warnungen als Fehler und ctest-Tests:
$env:FUT_CMAKE_ARGS = '-DFUT_WERROR=ON -DFUT_TEST_CONFIG=D:/FuturamaDecomp/run/phase5/ctest/futurama.ini'
cmd /c "D:\FuturamaDecomp\build.cmd" build\x86-werror RelWithDebInfo
```

Syntax: `build.cmd [builddir] [Debug|RelWithDebInfo] [target|all] [msvc|clang]`. Ausgabe: `<builddir>\bin\futurama.exe`, `futurama_host.dll`, `fut_tests.exe`. Generierte Dateien prüft `python tools\gen\gen_all.py --check` (CMake-Ziel `fut_gen`).

### 3.2 Konfiguration

Pfade mit Leerzeichen, Klammern und Kommas gehören in die INI, nicht auf die Kommandozeile. Beispiel (`run\integ\futurama.ini`):

```ini
; M1-Prüflauf. default.xbe wird direkt aus dem ISO gelesen.
[paths]
iso      = Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso
userdata = D:\FuturamaDecomp\run\integ        ; Testläufe nie unter %LOCALAPPDATA%

[debug]
stop_at  = main            ; Hook auf main 0xD29B0: loggen und mit Code 0 beenden
selftest = 1               ; Selbsttests aus Design 1.4 Schritt 8
msgbox   = 0
timeout  = 60              ; Watchdog in Sekunden (Exitcode 5)

[kernel]
strict   = 0               ; 1: jeder UNIMPLEMENTED-Stub bricht ab

[log]
level    = info
```

Regeln seit Phase 5:
- Kommentare am Zeilenende beginnen mit `;` oder `#` nach Leerraum und außerhalb von Anführungszeichen. Das entspricht Pythons `configparser` und damit `m1_check.py`.
- Unbekannte Schlüssel in bekannten Sektionen und ungültige Bool- oder Zahlenwerte (z. B. `timeout = 60s`) sind Konfigurationsfehler mit Exitcode 1. Unbekannte Sektionen erzeugen nur eine Warnung.
- Alle Schlüssel sind in `src\host\core\config.h` dokumentiert.

Kommandozeile (überschreibt die INI): `--config= --userdata= --iso= --xbe= --stop-at=<main|0xADDR> --selftest --trace=<kanäle|all> --strict --log-level= --set=<sektion>.<schlüssel>=<wert>`.

### 3.3 Ausführen

```sh
# Bash: ein Lauf bis main
/d/FuturamaDecomp/build/x86-debug/bin/futurama.exe --config=D:/FuturamaDecomp/run/integ/futurama.ini; echo $?
# Log: D:\FuturamaDecomp\run\integ\logs\futurama.log
```

```powershell
# M1-Exit-Prüfung (PowerShell, aus D:\FuturamaDecomp); --fresh leert userdata vor jedem Lauf
python tools\m1_check.py --exe build\x86-debug\bin\futurama.exe --config run\integ\futurama.ini --runs 10 --xbe orig\retail_eu\default.xbe [--fresh]
python tools\m1_check.py --exe build\x86-relwithdebinfo\bin\futurama.exe --config run\integ-rel\futurama.ini --runs 10 --xbe orig\retail_eu\default.xbe --fresh
python tools\m1_check.py --exe build\x86-clang-debug\bin\futurama.exe --config run\phase5\clang\futurama.ini --runs 10 --xbe orig\retail_eu\default.xbe --fresh
# Unit-Tests und ctest (im Build-Wurzelverzeichnis)
build\x86-debug\bin\fut_tests.exe
cd build\x86-werror; ctest --output-on-failure      # fut_selftest, fut_m1_check, fut_tests
```

`m1_check.py` übergibt selbst `--selftest --stop-at=main` und legt jedes Log unter `<userdata>\m1_check\run_NN.log` ab. Ein fehlendes oder leeres Log ist in jedem Kriterium FAIL. Ebenso FAIL ist ein Log, dessen Kopfzeile `userdata` nicht dem Wert aus der INI entspricht. Der Lauf über `main` hinaus (`run\integ-main\futurama.ini`, ohne `stop_at`) endet erwartungsgemäß mit Exitcode 3 in der MMIO-Falle (Abschnitt 8).

Exitcodes: 0 ok bzw. `--stop-at` erreicht · 1 Konfigurations-/Startfehler · 2 HostMain-Stufe gescheitert · 3 `core::Fatal` · 4 Gast beendet sich · 5 Watchdog · 10–14 Launcher (TLS, Platzhalter, DLL, `HostMain`, Basis).

## 4. Status der M1-Exit-Kriterien

Belege aus den Läufen vom 2026-10-05 nach dem letzten Build (`run\integ\m1_check\run_01.log`, 127 Zeilen):

| # | Kriterium | Status | Beleg |
|---|---|---|---|
| 1 | Alle Selbsttests aus 1.4.8, insbesondere `IsBadReadPtr(0,4) == TRUE` über Gast-SEH | **erfüllt** | 18/18 PASS in jedem Lauf. Phase host (6): AsmObjectLinked, TlsSlotsReadBack, GuestCallThunks, KernelStatusTableMatchesNtdll, KernelSha1KnownAnswer, KernelSlotsRegistered. Phase mapped (6): PolyglotNtHeader (`RtlImageNtHeader` = 0x10970), CreateThreadZeroStack, MappingMatchesFile (0 abweichende Bytes), FsPatchesApplied, KernelThunksResolved, HooksInstalled. Phase adopted-thread (6): BootThreadSlots, GuestGetCurrentThreadId, GuestLastErrorRoundTrip, GuestIsBadReadPtrNull (= 1), GuestSehIsBadReadPtr (AV bei 0x1709EF → `_except_handler3` → TRUE), KernelIrqlRoundTrip. Die Phasen laufen jetzt in fester Reihenfolge host → mapped (Befund 7). |
| 2 | Log zeigt `XapiInitProcess`, kein `XapiBootToDash`/`HalReturnToFirmware` | **erfüllt** | `boot: XapiInitProcess (0x000F92C0) start` … `done (XapiProcessHeap 0x02CA0000)`, danach `_rtinit` und `_cinit`. `XapiBootToDash`, `HalReturnToFirmware` und `XLaunchNewImageA` kommen in keinem Lauf vor. |
| 3 | `partition1\TDATA\46490002\`, `UDATA\46490002\TitleMeta.xbx` (UTF-16) und `TitleImage.xbx` (0x2800, = `$$XTIMAGE`) | **erfüllt** | `run\integ\hdd\partition1\TDATA\46490002\` existiert. `TitleMeta.xbx` hat 42 Byte, UTF-16 mit BOM, `TitleName=Futurama\r\n`. `TitleImage.xbx` hat 10 240 Byte und ist byte-identisch mit `$$XTIMAGE` (VA 0x3178C0). Das prüft `m1_check.py` in jedem Lauf, auch mit leerem `userdata` und mit 277 Zeichen langem `userdata` (Befund 11). |
| 4 | `_cinit` läuft durch: 38-MiB-Pool sichtbar, `XGetDiskClusterSizeA("U:\")` = 0x4000 | **erfüllt** | `game memory pool (memory.cpp, object 0x002E7D60): 0x2600000 bytes (38 MiB)`, `XGetDiskClusterSizeA("U:\") = 0x4000`, `boot: _cinit done (eax 0x0, save cluster size [0x002EDB0C] = 0x4000)` |
| 5 | `main`-Hook greift, Exitcode 0 | **erfüllt** | `STOP-AT 0x000D29B0 FUN_000d29b0 reached (called from 0x000F84CA XapiMainThreadStub+0x61)`, Exitcode 0 in allen 50 Prüfläufen. Ebenso Exitcode 0 mit `--strict` (kein Stub auf dem Pfad) und mit `--set=hooks.disable=probe:*`. |
| 6 | Zehn Läufe ergeben bis auf Zeitstempel und Adressen identische Logs | **erfüllt** | `m1_check.py --runs 10`: Nach dem Maskieren sind alle Logs gleich (je 127 Zeilen). Unmaskiert unterscheiden sich nur 5 Zeilen (KTHREAD-, Stack- und Heap-Adressen). Die normalisierten Logs von Debug und RelWithDebInfo sind jetzt bis auf Kopfzeilen und Pfade identisch, vorher war die Selbsttest-Reihenfolge verschieden. |

Zusätzlich gilt die Regel aus Abschnitt 6: 0 Fallen, 0 `UNIMPLEMENTED` und keine `error`- oder `warn`-Zeile im Log (Prüfung „log without error/warn/UNIMPLEMENTED/TRAP: clean“ in jedem Lauf).

Prüfläufe (alle `M1 check: ALL PASS`):

| Build | Konfiguration | Läufe |
|---|---|---|
| MSVC Debug | `run\integ`, `userdata` bleibt erhalten | 2 × 10 |
| MSVC Debug | `run\integ`, `--fresh` | 10 |
| MSVC RelWithDebInfo | `run\integ-rel`, `--fresh` | 10 |
| clang-cl Debug (`FUT_WERROR=ON`) | `run\phase5\clang`, `--fresh` | 10 |
| MSVC RelWithDebInfo (`FUT_WERROR=ON`) über ctest | `run\phase5\ctest` | `fut_selftest` + 10 (`fut_m1_check`) |

Alle Builds sind ohne Warnung (MSVC `/W4`, clang-cl `/W4`, jeweils einmal mit `/WX`). `fut_tests`: 48/48 Testfälle in Debug, RelWithDebInfo und clang.

## 5. Kernel-Slots: implementiert und Stub

111 Slots: 98 FUNC, 11 DATA, 2 ADDR. **76 haben eine registrierte Implementierung** (63 FUNC + 13 DATA/ADDR). **35 FUNC-Slots sind generierte Stubs.** Auf dem M1-Pfad wird keiner davon aufgerufen (`--strict` erreicht `main`).

| Menge (Design Anhang A) | Implementiert | Slots |
|---|---:|---|
| M1 – Bootpfad (FUNC) | 43 / 43 | 0 KfLowerIrql, 1 KeRaiseIrqlToDpcLevel, 3 RtlInitAnsiString, 6 KeInitializeTimerEx, 7 KeInitializeDpc, 9/10/14 Rtl\*CriticalSection, 11/12/15 Nt\*VirtualMemory, 13 RtlCompareMemoryUlong, 16 RtlRaiseException, 17 ExQueryNonVolatileSetting, 22 NtSetInformationFile, 23 KeQuerySystemTime, 24 RtlNtStatusToDosError, 25 NtCreateFile, 26 NtClose, 27–29 XcSHA\*, 34 NtReadFile, 35 NtWriteFile, 36 NtQueryInformationFile, 37 NtQueryVolumeInformationFile, 41 NtDeviceIoControlFile, 43 NtOpenFile, 48 IoCreateSymbolicLink, 54 MmFreeContiguousMemory, 55 HalReturnToFirmware, 58/59 Mm\*AddressProtect, 60 KeSetTimer, 72 XcVerifyPKCS1Signature, 74 HalRegisterShutdownNotification, 76 PsTerminateSystemThread, 77 PsCreateSystemThreadEx, 84 RtlEqualString, 85/86 XeLoad/UnloadSection, 105 RtlUnwind, 106 KeBugCheck |
| DATA + ADDR | 13 / 13 | 8 IoInvalidDeviceRequest, 18 ExEventObjectType, 30 XboxHDKey, 31 XboxSignatureKey, 32 XboxAlternateSignatureKeys, 39 NtUserIoApcDispatcher, 47 HalDiskCachePartitionCount, 53 LaunchDataPage, 64 XboxHardwareInfo, 73 XePublicKeyData, 75 XboxKrnlVersion, 87 IdexChannelObject, 110 HalBootSMCVideoMode |
| M2 – Streaming/Fibers | 10 / 12 | vorgezogen (A9): 20 NtWaitForSingleObjectEx, 21 KeDelayExecutionThread, 33 NtWaitForSingleObject, 38 NtFlushBuffersFile, 45 NtQueryFullAttributesFile, 46 NtQueryDirectoryFile, 51/52/56 Mm\*ContiguousMemory\*, 109 DbgBreakPoint. **Stub: 107 MmCreateKernelStack, 108 MmDeleteKernelStack** (brechen immer ab) |
| M7 – Saves | 2 / 4 | 42 MmFreeSystemMemory, 44 MmAllocateSystemMemory. **Stub: 49 NtQuerySymbolicLinkObject, 50 NtOpenSymbolicLinkObject** |
| Defensiv | 4 / 4 | 4 KeSetEvent, 5 KeWaitForSingleObject, 40 NtFsControlFile, 78 HalInitiateShutdown |
| Nur ersetzte Bibliotheken | 4 / 35 | 57 MmQueryAllocationSize, 61 KeCancelTimer, 80 KfRaiseIrql, 98 DbgPrint. **Stub (31):** 2, 19, 62, 63, 65–71, 79, 81–83, 88–97, 99–104 |

Rückgabe der Stubs (seit Phase 5, Befund 1): Exporte mit NTSTATUS-Rückgabe liefern `STATUS_NOT_IMPLEMENTED`, alle anderen 0. Unter den 35 Stubs sind 7 NTSTATUS, 4 Zeiger, 6 BOOLEAN, 4 ULONG und 14 void. Über alle 98 FUNC-Slots: 33 NTSTATUS, 7 Zeiger, 10 BOOLEAN, 12 ULONG, 36 void (Tabelle `RETURN_KIND` in `tools\gen\gen_kernel_table.py`).

## 6. Abweichungen vom Design

Die vollständige Tabelle steht in [`loader_design.md` Abschnitt 8](loader_design.md#81-abweichungen-in-m1). Die Entscheidungen E1–E12 gelten unverändert.

| # | Kurzfassung |
|---|---|
| A1 | Sechs Beobachter-Probes mit Trampolin (explizite Prologbytes statt Längen-Disassembler), damit Kriterien 2 und 4 auf Stufe info sichtbar sind. |
| A2 | Zusätzlicher Selbsttest `GuestSehIsBadReadPtr` mit NOACCESS-Seite. `IsBadReadPtr(0,4)` allein greift nie auf Speicher zu. |
| A3 | Prozessende über `TerminateProcess` nach Log-Flush; Exitcodes 4 (Gast beendet sich) und 5 (Watchdog). |
| A4 | Die MMIO-Falle reserviert der Host, nicht der Launcher. |
| A5 | 0x198 Byte gesicherte PE-Header (4 Sektionen) bei 0x10970. |
| A6 | `Nt*VirtualMemory` werden an ntdll weitergereicht, Ergebnisse ≥ 0x80000000 abgelehnt. |
| A7 | Fehlendes Zwischenverzeichnis → `STATUS_OBJECT_PATH_NOT_FOUND`. |
| A8 | Ein Read, der am oder nach EOF beginnt, liefert `STATUS_END_OF_FILE`. |
| A9 | M2-Slots (APCs, alertable Waits, Contiguous Memory, Verzeichnis-Abfragen) vorgezogen, mit Unit-Tests. |
| A10 | `Z:` → `\Device\Harddisk0\Partition3` → `<userdata>\cache\Z`. |
| A11 | **Neu:** Stub-Rückgabe nach Rückgabeart; 107/108 brechen immer ab (Befund 1). |
| A12 | **Neu:** clang-cl-Build mit `link.exe` statt `lld-link`, `code_seg` im Launcher, `/Oy-` im Host (Befund 6). |
| A13 | **Neu:** Validierung der INI (bekannte Schlüssel, Typen), Kommentare am Zeilenende, numerischer Vergleich in `[hooks] disable` (Befunde 2, 4, 8). |
| A14 | **Neu:** Lange Pfade: Der VFS legt Verzeichnisse über `\\?\` an, das Manifest setzt `longPathAware`, ohne Langpfad-Unterstützung gilt ein Limit von 200 Zeichen für `userdata` (Befund 11). |
| A15 | **Neu:** Stufenfehler und `core::Fatal` werden außerhalb des Logs gemeldet (stderr bzw. MessageBox, Befund 5). |

## 7. Review-Befunde

Jeder Befund wurde vor der Änderung am Code bzw. am Binary nachvollzogen.

| # | Schwere | Befund | Entscheidung | Umsetzung und Nachweis |
|---|---|---|---|---|
| 1 | mittel | Generierte Stubs liefern immer 0xC0000002, auch für Zeiger/BOOLEAN | **angenommen** | Disassembly bestätigt: `CreateFiber` 0x1708DA `call [0x29370C]`, 0x1708E0–0x1708E4 `mov ecx,eax; cmp ecx,edi; jne` und danach Schreibzugriffe ab `ecx-0x10`. Umsetzung: Spalte `ret` (`gen::Ret`) aus der Tabelle `RETURN_KIND` im Generator; Stubs liefern 0xC0000002 nur bei NTSTATUS, sonst 0. `FATAL_STUBS` = {107, 108} bricht auch ohne `strict` ab. Die Log-Zeile nennt den gelieferten Wert. Design 5.4 ist präzisiert. Abweichung vom Vorschlag: Die Spalte steht im Generator und nicht in `kernel_imports.tsv`, weil die TSV ein geprüfter Analysebericht ist und unverändert bleibt. Unit-Test „generated stubs return the failure value …“. |
| 2 | niedrig | `[hooks] disable` erkennt `0x000F641B` nicht | **angenommen** | `IsHookDisabled` verglich nur gegen `0x%X`. Neu: numerischer Vergleich (`strtoull` Basis 0) und eine Warnung für jeden Eintrag ohne registrierten Hook. Lauf `run\phase5\hooks`: `0x000F641B` schaltet den QPC-Hook ab, `prob:*`, `0x12345` und `XAPILIB::NoSuchHook` erzeugen je eine HOOK WARN. Unit-Tests. |
| 3 | niedrig | Kommentar zu `XBOX_HARDWARE_INFO` +1 falsch | **angenommen** | Disassembly bestätigt: 0x23C846 `mov edx,[0x293660]`, 0x23C84C `test byte ptr [edx+1],2` (= `Flags & 0x200`), XPP 0x28C8FB `cmp byte ptr [eax+5],0xA1`. Die Kommentare in `ke_types.h` und `data_exports.cpp` sind korrigiert (Bit 9 bleibt 0). |
| 4 | mittel | INI kennt keine Zeilenend-Kommentare, `m1_check` schon | **angenommen** | Einheitliche Regel (`configparser`-Semantik): `;`/`#` nach Leerraum außerhalb von Anführungszeichen. Dokumentiert in `config.h`. `m1_check.py` vergleicht zusätzlich den `userdata`-Pfad der Log-Kopfzeile. Die Review-INI `run\review_repro\inline_comment` besteht jetzt alle Kriterien. Unit-Test „INI inline comments …“. |
| 5 | mittel | Stufenfehler im Release-Build ohne jede Ausgabe | **angenommen** | `RunHost` meldet die letzte Fehlerzeile (`core::LogLastError`) und den Log-Pfad über `ReportStartupError`: stderr, ohne stderr oder mit `msgbox = 1` als MessageBox. `core::Fatal` schreibt ohne Konsolen-Sink ebenfalls auf stderr. RelWithDebInfo, `run\phase5\missing_iso`: Exitcode 2, stderr `futurama: stage LoadXbe failed (exit code 2): cannot read default.xbe from the disc image …does_not_exist.iso: cannot open disc image (Win32 error 2)` plus Log-Pfad. |
| 6 | mittel | clang-cl-Build fehlt bzw. scheitert | **angenommen** | `ExceptionName` schaltet auf `core::ExceptionCode(EXCEPTION_*)` (DWORD), die Vergleiche sind ebenso umgestellt, `#pragma optimize` nur für MSVC, `/Oy-` für clang. Neu sind das Preset `x86-clang-debug` und `build.cmd … clang` (`i686-pc-windows-msvc`, `ml.exe`). Beim Lauf gefunden: (a) clang legt Code in `.text`, das vor `.text$a` sortiert, deshalb lag der Platzhalter bei 0x11500 (Launcher-Exitcode 11). Behoben mit `#pragma code_seg(".text$mn")` im Launcher. (b) Eine von `lld-link` 23 gelinkte Host-DLL hängt bzw. endet mit 0xC0000409 schon in `LoadLibrary`. Querprobe: clang-Launcher + MSVC-DLL läuft, MSVC-Launcher + clang-DLL scheitert. Der clang-Build linkt deshalb mit `link.exe`. Ergebnis: Build ohne Warnung mit `FUT_WERROR=ON`, 48/48 Unit-Tests, M1-Check 10/10. |
| 7 | niedrig | Reihenfolge der Selbsttest-Phasen unspezifiziert | **angenommen** | Die Phasen laufen nacheinander (host, dann mapped; ein host-Fehler überspringt mapped). Normalisierte Logs Debug und Rel unterscheiden sich nur noch in Kopfzeilen und Pfaden. |
| 8 | niedrig | Tippfehler in der Konfiguration bleiben still | **angenommen** | `kKnownKeys` mit Typen. Unbekannter Schlüssel und ungültiger Wert → Exitcode 1 mit Meldung, unbekannte Sektion → WARN, Hook-Muster ohne Treffer → WARN. `run\phase5\typo` (`stop-at`): `unknown config key 'stop-at' in [debug]`, Exitcode 1. `run\phase5\cfgerr` (`timeout = 60s`): `invalid number for [debug] timeout`, Exitcode 1. |
| 9 | niedrig | `m1_check` bewertet ein leeres Log als bestanden | **angenommen** | Ein fehlendes oder leeres Log ist in jedem Kriterium FAIL, Kriterium 6 verlangt ein nicht leeres Referenzlog mit STOP-AT, das Log wird vor jedem Lauf gelöscht. Gegenprobe `run\phase5\cfgerr`: alle Kriterien FAIL („log file missing or empty“, „run 1 has no log lines“), stderr-Meldung in der Ausgabe. |
| 10 | niedrig | `ctest` findet im Build-Wurzelverzeichnis keine Tests | **angenommen** | `enable_testing()` steht in der obersten `CMakeLists.txt`. Mit `FUT_TEST_CONFIG` kommen `fut_selftest` (`futurama.exe --config=… --selftest --stop-at=main`) und `fut_m1_check` (10 Läufe) hinzu. `build\x86-debug`: `ctest -N` → `fut_tests`. `build\x86-werror`: 3/3 Tests bestanden. |
| 11 | niedrig | Lange `userdata`-Pfade brechen `InitVfs` ab | **angenommen, teilweise widerlegt** | Bestätigt: `std::filesystem::create_directories` in `vfs_init.cpp` und `CreateHostDirDevice` scheitert ohne `\\?\`. **Widerlegt:** „HostDirDevice benutzt kein `\\?\`-Präfix“ und „Saves ab ~200 Zeichen gefährdet“. `HostDirDevice` öffnet alle Dateien schon über `\\?\`-Pfade (`NativeRoot`), Spielstände waren also nach einem erfolgreichen `InitVfs` nicht betroffen. Umsetzung: `vfs::CreateHostDirectories` (`\\?\`), Manifest `longPathAware` und ohne Langpfad-Unterstützung ein klarer Konfigurationsfehler ab 201 Zeichen (Log, Minidumps). Lauf `run\phase5\longpath`: `userdata` mit 277 Zeichen, Exitcode 0, `TitleImage.xbx` bei 322 Zeichen. Unit-Test mit über 330 Zeichen. |

Kein Befund wurde vollständig abgelehnt.

## 8. Bekannte Probleme und offene Punkte

- **Über `main` hinaus:** Ohne `--stop-at` erreicht `main` `NiXBoxRenderer::Create` 0x14C170 → `D3D8::Direct3D_CreateDevice` → `CMiniport_InitHardware`. Dieser liest NV2A-MMIO 0xFD001804, die MMIO-Falle greift, `core::Fatal` endet mit Exitcode 3. Crash-Log und Minidump funktionieren. Das ist der erwartete Einstieg in M2.
- **R2 ungetestet:** SEH auf Fiber-Stacks und SEHOP gehören zu M2. R1 ist nur für den adoptierten Boot-Thread und den Gast-Hauptthread gelöst.
- **Kernel-Trace** (`--trace=krnl`, Ringpuffer aus Design 5.4) ist noch nicht über `core::AddCrashReporter` an das Crash-Log angebunden.
- **IO-Semantik in M2 prüfen:** Ein Read ab EOF liefert `STATUS_END_OF_FILE` (A8). Der Event-Parameter von `NtReadFile`, `NtWriteFile` und `NtQueryDirectoryFile` wird nie signalisiert.
- **Host-Frames zwischen Gast-Frames:** Probe- und Host→Gast-Wrapper sind noexcept-C++-Frames. Ob eine Gast-SEH-Ausnahme sauber durch sie hindurch abgewickelt wird, ist ungetestet. Beispiel wäre ein `HEAP_GENERATE_EXCEPTIONS`-Fehler in der Pool-Allokation, den ein äußeres Gast-`__except` fängt.
- **Rückgabearten der Stubs** sind außer bei Slot 107 nicht am Aufrufer geprüft (`// ASSUMPTION` im Generator; öffentliche NT-/xboxkrnl-Prototypen).
- **lld-link:** Warum eine von `lld-link` 23 gelinkte Host-DLL in `LoadLibrary` scheitert, ist ungeklärt. Der clang-Build umgeht das mit `link.exe`.
- **Backtraces in RelWithDebInfo:** EBP- und StackWalk64-Backtrace enden an Frames ohne Framepointer (`/Oy`); der Stack-Scan funktioniert.
- **Formatierung:** Die in Phase 5 geänderten Dateien sowie `guest.h` sind mit `.clang-format` formatiert. Andere Dateien (u. a. `kernel_api.h`, `log.h`, `vfs.h`) haben noch ältere, rein kosmetische Abweichungen.
- **Langpfade:** Ohne `LongPathsEnabled = 1` gilt das Limit von 200 Zeichen für `userdata`. Der VFS selbst hängt nicht davon ab.

## 9. Nächste Schritte für M2

1. **`NiXBoxRenderer::Create`-Detour** 0x14C170 (cdecl, 8 Argumente) → `NiPCRenderer` als Null-Renderer. Er hat alle 75 Slots mit dem `ret N` aus `renderer_retn.py`, Slots 65/66 = TRUE, Slot 39 = FALSE und einen Dummy-Device-Zeiger an +0x60. Den Basis-ctor `FUN_00140550` vorher am Aufruf in `FUN_0014af00` auf thiscall prüfen (Design 2.2). Damit endet der Lauf über `main` nicht mehr in der MMIO-Falle.
2. **D3D-Shim** für die 8 direkt gerufenen Funktionen (Design 2.2) und **Fallen** für alle übrigen D3D-Einstiege mit `ext_call_sites > 0`. Generator `d3d_traps.gen.inc` aus `d3d_functions.tsv`, jeweils mit passendem `ret N`.
3. **`MmCreateKernelStack`/`MmDeleteKernelStack`** (107/108): 512-KiB-Stacks, Bookkeeping-Map und SEH-Terminal-Record. Danach die Slots aus `FATAL_STUBS` entfernen.
4. **`SwitchToFiber`-Ersatz** 0x17094F in MASM. Er schaltet zusätzlich `TEB.StackBase/StackLimit/DeallocationStack` um. Dazu kommt der Selbsttest „SEH auf Fiber-Stack“ (M2-Kriterium 6, R2).
5. **XDK-Stubs:** DSOUND (40 Einstiege + 5 Stream-Methoden, `*pp = NULL`, `GetStatus` = 0), XMV (7, Ergebnis 2) und XPP (7, keine Geräte). Generator `xdk_entries.gen.inc` aus `avi_api_surface.tsv`.
6. **`--frames=N`** über den Zähler im `DisplayFrame`-Slot (16) des `NiPCRenderer`, mit geordnetem Ende.
7. **VFS-Trace** (`--trace=vfs` → `vfs_trace.log`, M2-Kriterium 7) und den **Kernel-Trace-Ringpuffer** über `core::AddCrashReporter` anbinden.
8. **IO nachschärfen:** A8 (Read ab EOF) gegen den Disc-Fehler-Handler `DAT_0030E68C` prüfen und den Event-Parameter signalisieren.
9. **`tools\m2_check.py`** nach dem Muster von `m1_check.py`: `SysPAL.img` und `FutPAL00`–`04` gemountet, `FutPAL05` fehlt, PAL (`CApp+0xE8 = 0`), Sprache aus der Config, `BUILD.TXT` = „1.25“, Fiber-Wechsel > 0, APC-Zustellungen > 0. Als ctest-Ziel registrieren.
10. **Gast-SEH durch Host-Wrapper** mit einem gezielten Selbsttest prüfen, bevor Hooks im Spielcode zunehmen.
