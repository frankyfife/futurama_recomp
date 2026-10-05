# Meilenstein M2: Bericht

Stand: 2026-10-05 (Phase 4, nach Behebung der Review-Befunde) · Ziel-XBE: `orig\retail_eu\default.xbe` (Retail EU, SHA-1 `58f76de5bc739bf016d74c1d7f23e348f65ea4b4`) · Verbindliches Design: [`docs/loader_design.md`](loader_design.md) (E1–E12, Abweichungen in Abschnitt 8.2) · Vorgänger: [`docs/m1_report.md`](m1_report.md)

## 1. Ergebnis in Kürze

**Das Spiel läuft über `main` hinaus durch Dateisystem, Archive und Hauptschleife, ohne Bild und Ton.** Der Loader ersetzt `NiXBoxRenderer::Create` durch den Null-Renderer `NiPCRenderer`, stellt einen D3D-Shim und 79 D3D-Fallen bereit, ersetzt DSOUND, XMV und XPP durch stille Stubs und emuliert Fibers (`MmCreateKernelStack`, `SwitchToFiber` in MASM) sowie asynchrones I/O mit Gast-APCs. Der Ablauf eines Prüflaufs:

```
main 0xD29B0 → CApp::Init 0xD26A0 → NiXBoxRenderer::Create (→ NiPCRenderer) → sysPAL.img, FutPAL00–04.img
  → Game::Init 0xA26A0 (PAL, Sprache, Kontexte, BUILD.TXT) → Hauptschleife CApp::Frame 0xD2630 / Game::Frame 0xA2C20
  → Intro (Legal-Bild, Filme fox/sci/uds, Splash, Ladebild, Film intro) → Frontend → … → --frames=3000 → Exitcode 0
```

Alle sieben Exit-Kriterien aus Design-Abschnitt 6 (M2) sind erfüllt. `tools\m2_check.py` prüft sie nach den Review-Fixes über je zehn Läufe mit leerem `userdata` in drei Builds (MSVC Debug, MSVC RelWithDebInfo, clang-cl Debug), dazu Varianten (Englisch bzw. ohne Sprachschlüssel, Französisch, 100 Frames, 30 000 Frames) und ctest mit `FUT_WERROR=ON`. `tools\m1_check.py` besteht weiterhin in allen drei Builds. Von den elf Review-Befunden sind zehn angenommen und behoben, einer (Befund 11) ist angenommen und in der Dokumentation korrigiert; keiner ist abgelehnt (Abschnitt 8).

## 2. Was gebaut wurde

### 2.1 Struktur (neu oder erweitert in M2)

```
D:\FuturamaDecomp\
  src\host\
    gfx\            NiPCRenderer (Null-Renderer, 75 Slots), Textur-RendererData, D3D-Shim (8), D3D-Fallen (79),
                    Purge-Messung, Frame-Takt und --frames                                   ~2 500 Zeilen
    audio\ video\ input\   XDK-Stubs: DSOUND (40 Einstiege), XMV (7), XPP (7), Gast-Dummy-Objekte  ~2 300 Zeilen
    kernel\         + MmCreate/DeleteKernelStack (Fiber-Stacks mit SEH-Records), APC-Queue, alertable Waits,
                    Event-Objekte, Statistiken                                               ~8 700 Zeilen
    xapi\           + SwitchToFiber-Ersatz (C++-Teil), sechs Spiel-Beobachter-Probes, Disc-Fehler-Falle
    core\           + Exit-Reporter, Trace (VFS-Trace, Kernel-Trace-Ring), Selbsttests M2       ~5 600 Zeilen
    asm\            + fiber_switch.asm (fut_switch_to_fiber, fut_seh3_frame_call)
    gen\            + d3d_traps.gen.inc (79), renderer_slots.gen.inc (75), xdk_entries.gen.inc (54 + 7)
  tests\            10 Testdateien, 95 Testfälle (u. a. apc, fiber, gfx, xdk, vfs_trace, m2_integration)
  tools\gen\        + gen_d3d_traps.py, gen_renderer_slots.py, gen_xdk_entries.py (alle mit --check)
  tools\m2_check.py M2-Exit-Prüfung (N Läufe, sieben Kriterien, VFS-Trace-Auswertung)
```

### 2.2 Komponenten

| Komponente | Umsetzung |
|---|---|
| Null-Renderer (E6, 2.2) | Detour `NiXBoxRenderer::Create` 0x14C170 (cdecl, 8 Argumente). Das 0x6D0-Byte-Objekt kommt wie im Original aus dem Spielpool und wird vom Original-Basis-ctor `FUN_00140550` gebaut (Renderer-Liste 0x2D7B18). Eigene 75-Slot-VTable, Slots 2–10 zeigen auf die Originale, alle anderen sind Host-Funktionen mit dem Original-`ret N` (zur Compilezeit gegen die Tabelle und zur Laufzeit per Purge-Messung geprüft). Pflicht-Rückgaben: Slot 1 RTTI 0x30EE00, 65/66 TRUE mit layoutkompatibler RendererData (0x70 bzw. 0x84 Byte), 39 FALSE. Dummy-Device an +0x60. |
| D3D-Shim und -Fallen | 8 direkt gerufene D3D-Funktionen des Movie-Players (Refcounts, `CreateTexture2` mit Xbox-ähnlichem Header, `GetSurfaceLevel2`, `YuvEnable`, `SetScreenSpaceOffset`). 79 weitere D3D-Einstiege sind Fallen (`TRAP` + `core::Fatal`, oder `[render] d3d_traps = log`). |
| XDK-Stubs (E7) | DSOUND: Device ≠ NULL, Buffer/Streams NULL, `GetStatus` 0. XMV: Dummy-Decoder, `GetNextFrame` = 2 (jeder Film endet im ersten Update). XPP: keine Geräte. Aufrufzähler pro Einstieg und Aufrufstelle. |
| Fibers (1.7, E9, R2) | `MmCreateKernelStack` (512 KiB + Guard-Page, Fiber-Top-Record und SEHOP-fester Terminal-Record), `SwitchToFiber` 0x17094F in MASM: schaltet zusätzlich `TEB.StackBase/StackLimit/DeallocationStack` um. |
| Asynchrones I/O (1.7, 4.3) | `NtReadFile`/`NtWriteFile` mit `NtUserIoApcDispatcher`: Transfer sofort, `STATUS_PENDING`, APC in der Queue des Threads; Zustellung in alertable `KeDelayExecutionThread`/`NtWaitForSingleObjectEx` (UserMode) direkt an die Completion (`ret 0xC`). |
| Frames (5.4) | `--frames=N` zählt `DisplayFrame` (Slot 16) und endet geordnet über die Exit-Reporter. `[render] frame_rate = 50` taktet wie der PAL-VBlank. |
| Beobachter (B7, B14) | Sechs Probes auf Spielcode (Archive, Archivsuche, `Game::Init`, `CApp::Frame`, Kontextwechsel, `XGetLanguage`) und die Falle `trap:Game::DiscErrorScreen`. Sie machen die Kriterien 1–5 und 7 auf Log-Stufe info sichtbar. |
| Diagnose | `--trace=vfs` → `<userdata>\logs\vfs_trace.log` (gleiche Uhr und Thread-Namen wie das Log), `--trace=krnl` → Ring der letzten 1024 KRNL-Zeilen im Crash-Report, Exit-Reporter (Fiber-, APC-, Spiel-, Renderer- und XDK-Statistik bei jedem geordneten Ende). |

## 3. Bauen und Ausführen

### 3.1 Bauen

Wie M1 (Abschnitt 3.1 des M1-Berichts). `build.cmd` immer mit vollem Pfad aufrufen:

```powershell
cmd /c "D:\FuturamaDecomp\build.cmd" build\x86-debug Debug
cmd /c "D:\FuturamaDecomp\build.cmd" build\x86-relwithdebinfo RelWithDebInfo
cmd /c "D:\FuturamaDecomp\build.cmd" build\x86-clang-debug Debug all clang
# ctest mit Warnungen als Fehler, M1- und M2-Prüfung (Pfade mit / in FUT_CMAKE_ARGS)
$env:FUT_CMAKE_ARGS = '-DFUT_WERROR=ON -DFUT_TEST_CONFIG=D:/FuturamaDecomp/run/m2fix/ctest-m1/futurama.ini -DFUT_M2_TEST_CONFIG=D:/FuturamaDecomp/run/m2fix/ctest/futurama.ini -DFUT_M2_EXTRA_TEST_CONFIGS=D:/FuturamaDecomp/run/m2fix/ctest-en/futurama.ini'
cmd /c "D:\FuturamaDecomp\build.cmd" build\x86-werror RelWithDebInfo
```

`python tools\gen\gen_all.py --check` prüft die fünf generierten Dateien (mit XBE zusätzlich per capstone gegen das Original).

### 3.2 Konfiguration

Beispiel `run\m2fix\de\futurama.ini` (M2-Prüflauf, Debug):

```ini
[paths]
iso      = Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso
userdata = D:\FuturamaDecomp\run\m2fix\de

[debug]
selftest = 1               ; Kriterium 6: GuestSehOnFiberStack
msgbox   = 0
timeout  = 150             ; Watchdog; 3000 Frames bei 50 Hz dauern ~60 s
frames   = 3000            ; Kriterium 4: geordnetes Ende nach 3000 angezeigten Frames
trace    = vfs             ; Kriterium 7: <userdata>\logs\vfs_trace.log

[settings]
language = german          ; Kriterium 2: XGetLanguage = 3 (english|japanese|german|french|spanish|
                           ; italian|korean|tchinese|portuguese oder 1..9, sonst Exitcode 1)
[render]
frame_rate = 50            ; PAL-VBlank-Takt; 0 = ungebremst (für Kriterium 4 ungeeignet)

[log]
level    = info
```

Neue Schlüssel in M2: `[debug] frames`, `[debug] exit_hang_test` (Test des Watchdogs während des Prozessendes), `[kernel] fiber_stack`, `[render] frame_rate | d3d_traps | load_textures`, `[audio] dummy_streams`. Kommandozeile zusätzlich `--frames=N`. Seit Phase 4 sind `[settings] language`, `[saves] key_mode` und `[render] d3d_traps` Auswahl-Schlüssel: Ein ungültiger Wert ist ein Konfigurationsfehler mit Exitcode 1 (Befund 10).

### 3.3 Ausführen und prüfen

```sh
# Bash: ein Lauf mit 3000 Frames (~60 s); Log run\m2fix\de\logs\futurama.log, Trace ...\logs\vfs_trace.log
/d/FuturamaDecomp/build/x86-debug/bin/futurama.exe --config=D:/FuturamaDecomp/run/m2fix/de/futurama.ini --selftest; echo $?
```

```powershell
# M2-Exit-Prüfung (aus D:\FuturamaDecomp); --fresh leert hdd, cache und logs vor jedem Lauf
python tools\m2_check.py --exe build\x86-debug\bin\futurama.exe --config run\m2fix\de\futurama.ini --runs 10 --fresh
python tools\m2_check.py --exe build\x86-relwithdebinfo\bin\futurama.exe --config run\m2fix\rel\futurama.ini --runs 10 --fresh
python tools\m2_check.py --exe build\x86-clang-debug\bin\futurama.exe --config run\m2fix\clang\futurama.ini --runs 10 --fresh
# Varianten: Englisch ohne Sprachschlüssel, Französisch (1000 Frames), 100 Frames, 10 Minuten
python tools\m2_check.py --exe build\x86-debug\bin\futurama.exe --config run\m2fix\en\futurama.ini --runs 1 --fresh
# M1-Regression und Unit-Tests
python tools\m1_check.py --exe build\x86-debug\bin\futurama.exe --config run\m2fix\m1\futurama.ini --runs 10 --xbe orig\retail_eu\default.xbe --fresh
build\x86-debug\bin\fut_tests.exe
cd build\x86-werror; ctest --output-on-failure   # fut_tests, fut_selftest, fut_m1_check, fut_m2_check, fut_m2_check_ctest-en
```

`m2_check.py` übergibt `--selftest`, dazu `--frames=3000` und `--trace=vfs`, falls die INI sie nicht setzt. Die INI darf kein `stop_at` setzen. Logs jedes Laufs: `<userdata>\m2_check\run_NN.log` und `run_NN_vfs_trace.log`. Bei einem gescheiterten Lauf zeigt das Skript die eigene stderr-Zeile des Programms (z. B. einen Konfigurationsfehler) oder die erste error/warn-Zeile des Logs, nicht mehr die Konsolenkopie der letzten Log-Zeile (Befund 6).

Exitcodes wie M1: 0 ok (`--frames` erreicht, `--stop-at`), 1 Konfiguration, 2 Stufe, 3 `core::Fatal` (auch D3D- und Disc-Fehler-Falle), 4 Gast beendet sich, 5 Watchdog (seit Phase 4 auch, wenn das geordnete Ende selbst hängt).

## 4. Status der M2-Exit-Kriterien

Belege aus den Läufen vom 2026-10-05 nach dem letzten Build (`run\m2fix\de\m2_check\run_01.log`, 475 Zeilen, und `run_01_vfs_trace.log`, 140 Datenzeilen):

| # | Kriterium | Status | Beleg |
|---|---|---|---|
| 1 | `SysPAL.img`, `FutPAL00`–`04.img` gemountet, `FutPAL05.img` nicht gefunden, PAL (`CApp+0xE8 = 0`) | **erfüllt** | `archive: mounted sysPAL.img as #0 (7742704 bytes, header 0xCF7 bytes …)`, `FutPAL00.img` bis `FutPAL04.img` als #1–#5 (Reihenfolge = Suchreihenfolge), `archive: FutPAL05.img not mounted (FUN_0013FE70 -> 0 …)`; im Trace `NtCreateFile "\??\D:\FutPAL05.img" … missing STATUS_OBJECT_NAME_NOT_FOUND (documented: end of the FutPAL%02d.img mount loop …)`. `game: video standard CApp+0xE8 = 0 (PAL; 50 fps, fixed time step 0.0200 s, movie tag "PAL")` |
| 2 | Sprache aus der Konfiguration greift (`german` → `XGetLanguage` = 3) | **erfüllt** | `XGetLanguage() = 3 (german), called from 0x000D9995 FUN_000d9990+0x5`, `game: language "german"` (String-Manager `[0x2EC640]+0x18`), Sounds aus `sounds/localized/german/`. Gegenproben: `french` → 4 / `french`; `english` und ohne Schlüssel → 1 / `English` (Befund 3) |
| 3 | `BUILD.TXT` = „1.25“ gelesen | **erfüllt** | `game: BUILD.TXT = "1.25"` (0x2E7A68 nach `Game::Init`); Trace `archive lookup "BUILD.TXT" -> sysPAL.img+0x00001000 (4 bytes) : hit archive #0` |
| 4 | Intro-Kontext läuft, `--frames=3000` endet geordnet ohne Hänger | **erfüllt** | `game: contexts Intro, Frontend, Attract, Ingame, Death, Intermission; current "Intro"`, `game: context Intro -> Frontend (CApp::Frame #780)` nach 15,9 s, `FRAMES: limit of 3000 frames reached`, `process exit: frame limit reached (exit code 0 …)` nach 60 s; 2985 `CApp::Frame`-Aufrufe |
| 5 | Fiber-Wechsel > 0, APC-Zustellungen > 0, Disc-Fehler-Handler nie aufgerufen | **erfüllt** | `fiber summary: 4932 switches (4930 game, 2 self-test; …)`, `fiber: first SwitchToFiber (game) …` auf `guest-main` (Befund 5); `I/O APC stats (process exit): queued 207, delivered 207 (XBE completions 205), … error completions 0, failed APC reads 0 (disc error path), failed APC writes 0 (save layer)`; `game: disc error handler [0x0030E68C] = 0x002D707C (vtable 0x002A59BC, vf0 0x000F2FD0 = trap:Game::DiscErrorScreen)`, `disc error screen calls 0`, keine `TRAP`-Zeile |
| 6 | Selbsttest „SEH auf Fiber-Stack“ | **erfüllt** | `SELFTEST PASS GuestSehOnFiberStack` (AV durch `IsBadReadPtr` auf einer NOACCESS-Seite in einer Gast-Fiber, `_except_handler3` → TRUE, TEB-Stackgrenzen auf der Fiber korrekt); 28 von 28 Selbsttests bestehen |
| 7 | `vfs_trace.log` listet alle geöffneten Pfade, jede Öffnung Treffer oder dokumentiertes „fehlt“ | **erfüllt** | 140 Zeilen: 89 Archivtreffer, 20 Treffer des Spiels, 1 Treffer des Selbsttests (`[self-test]`), 8 dokumentierte Fehlstellen (`FutPAL05.img`, 4 × `Video\*-PAL.txt`, 3 × `sounds\menu\MusicClip.wav`), 11 nicht archivierte Assets mit folgender loser Öffnung, 6 × mounted, 1 × not mounted, 4 Symlinks; 0 UNDOCUMENTED, 0 error. Die Zahl der Öffnungen stimmt mit den VFS-Debug-Zeilen in `futurama.log` überein, die Pfadfolge ist in allen Läufen gleich |

Zusätzlich gilt die Regel aus Abschnitt 6: 0 Fallen, 0 `UNIMPLEMENTED` und keine `error`- oder `warn`-Zeile („log without error/warn/UNIMPLEMENTED/TRAP: clean“ in jedem Lauf).

Prüfläufe nach den Review-Fixes (alle `ALL PASS`):

PRUEFLAEUFE_TABELLE

## 5. Kernel-Slots, Hooks und Stubs

**Kernel:** 111 Slots (98 FUNC, 11 DATA, 2 ADDR). **78 haben eine registrierte Implementierung** (65 FUNC + 13 DATA/ADDR), 2 mehr als in M1: 107 `MmCreateKernelStack` und 108 `MmDeleteKernelStack`. **33 FUNC-Slots sind generierte Stubs**: 49/50 `Nt*SymbolicLinkObject` (M7) und 31 Slots der ersetzten Bibliotheken (2, 19, 62, 63, 65–71, 79, 81–83, 88–97, 99–104). In keinem M2-Lauf wird ein Stub aufgerufen (keine `UNIMPLEMENTED`-Zeile); `FATAL_STUBS` ist leer. Gegenüber M1 neu benutzt bzw. erweitert: asynchrones `NtReadFile`, alertable `KeDelayExecutionThread`/`NtWaitForSingleObjectEx`, `NtUserIoApcDispatcher`, `NtQueryDirectoryFile`, `NtQueryFullAttributesFile`, `Mm*KernelStack`.

**Hooks:** 161 installiert, 0 abgeschaltet (Log: `hooks: 161 installed, 0 disabled, 161 registered`; mit `--stop-at=main` kommt der Hook `main` dazu):

| Gruppe | Anzahl | Inhalt |
|---|---:|---|
| XAPI-Ersatz | 6 | QPC, QPF, `OutputDebugStringA`, `XMountUtilityDrive`, `XLaunchNewImageA`, `SwitchToFiber` (MASM) |
| Boot-Probes (M1) | 6 | `XapiInitProcess`, `_rtinit`, `_cinit`, `XGetDiskClusterSizeA`, `XapiBootToDash`, Pool-Init |
| Spiel-Probes (M2) | 6 | `ArchiveMount`, `ArchiveLookup`, `Game::Init`, `CApp::Frame`, `Game::ContextSwitch` (neu, Befund 4), `XGetLanguage` |
| Fallen Spiel | 1 | `trap:Game::DiscErrorScreen` |
| Renderer | 1 | `NiXBoxRenderer::Create` → `NiPCRenderer` |
| D3D-Shim | 8 | `D3DDevice_AddRef/Release`, `CreateTexture2`, `GetSurfaceLevel2`, `D3DResource_AddRef/Release`, `YuvEnable`, `SetScreenSpaceOffset` |
| D3D-Fallen | 79 | alle übrigen extern gerufenen D3D-Einstiege |
| XDK-Stubs | 54 | DSOUND 40, XMV 7, XPP 7 |

**Aufgerufen in einem 3000-Frame-Lauf (60 s):**
- Keine D3D-Falle, kein generierter Kernel-Stub, keine `TRAP`-Zeile; die Disc-Fehler-Falle wird nie erreicht.
- DSOUND: 21 632 Aufrufe an 24 von 40 Einstiegen (25 Aufrufstellen). XMV: 24 Aufrufe an 6 von 7 Einstiegen (vier Filme `fox`, `sci`, `uds`, `intro` enden je im ersten Update). XPP: 2997 Aufrufe an 3 von 7 Einstiegen (keine Geräte).
- D3D-Shim: `D3DDevice_AddRef/Release`, 4 × `CreateTexture2` (YUY2 640×480 für die Filme), 4 × `GetSurfaceLevel2`, `D3DResource_Release`; am Ende 0 lebende Ressourcen.
- `NiPCRenderer`-Slots (Aufrufe): 16 `DisplayFrame` 3000, 17 `ClearBuffer` 3015, 21/22 je 3, 27 `SetBackgroundColor` 12, 39 `PrecacheGeometry` 181, 45/46/47 `BeginScene`/`EndScene`/`SetCameraData` je 2977, 48 `RenderShape` ≈ 762 000, 54 `RenderScreenPolygon` 45, 55 `RenderScreenTexture` 4838, 56 ≈ 762 000, 61 19, 64 5, 65 161 (140 neue Source-Texturdaten, 21-mal lag schon RendererData an), 66 1, 71 1. Nicht erreicht: Slot 15 auf eine Textur, der Texturdatei-Pfad von Slot 65 (keine der 140 Texturen hat einen Pfad).

## 6. Was das Spiel nach `main` tut

| Zeit (s) | Ereignis (Log) |
|---|---|
| 0,043 | Gast-Hauptthread startet, `XapiInitProcess`, `_cinit` (wie M1) |
| 0,046 | `NiXBoxRenderer::Create(640, 480, …)` → `NiPCRenderer`; XPP-, DSOUND-Stub |
| 0,047 | `sysPAL.img`, `FutPAL00`–`04` gemountet, `FutPAL05.img` fehlt (Ende der Mount-Schleife), `XGetLanguage() = 3` |
| 0,071 | `Game::Init`: PAL, 50 fps, Sprache `german`, `BUILD.TXT` = 1.25, sechs Kontexte, aktueller Kontext `Intro` |
| 0,11 | erster Fiber-Wechsel des Spiels: Lade-Fiber der Dateiklasse (Routine 0x13F1A0, ein Fiber je asynchroner Datei, startup.md 4.2) |
| 0–15,9 | Intro: Legal-Bild (`images\german\legal.nif`), Filme `fox`, `sci`, `uds` (XMV-Stub: je ein Update), Splash (`images\english\NI_Splash_PAL.nif`), Ladebild, Film `intro` |
| 15,9 | `game: context Intro -> Frontend (CApp::Frame #780)`: Frontend-Menü mit Menüsounds, Musik (`Sounds/Music`), Schriften |
| 60,0 | `FRAMES: limit of 3000 frames reached`, Exit-Reporter, Exitcode 0 |

Im 10-Minuten-Lauf (30 000 Frames) wechselt Frontend nach etwa 60 s ohne Eingabe zu Attract. Der XMV-Stub beendet `Video\Attract-PAL.xmv` sofort, das Spiel kehrt zu Frontend zurück; das wiederholt sich etwa alle 65 s. LONG_RUN_DETAILS

**Geöffnete Dateien** (ein 3000-Frame-Lauf): Auf `D:` die sechs Archive, die Filme `Video\{fox,sci,uds,intro}-PAL.xmv` (lose Dateien) und die drei fehlenden Dateien aus Kriterium 7. Aus den Archiven 89 Assets: 46 aus `sysPAL.img` (Sprachtabellen `Language/*.ldb`, Schriften `fonts/*`, Menüsounds, `BUILD.TXT`), 3 aus `FutPAL00.img`, 20 aus `FutPAL02.img`, 15 aus `FutPAL03.img`, 5 aus `FutPAL04.img` (Bilder, Menü- und Extras-NIFs, lokalisierte Sounds `sounds/localized/german/…`, Musik). Auf `partition1` legt XAPI wie in M1 `TDATA`/`UDATA\46490002` mit `TitleMeta.xbx`/`TitleImage.xbx` an; das Spiel zählt die Spielstände über `U:\` auf (keine vorhanden). Gespeichert wird in M2 nicht.

## 7. Abweichungen vom Design

Die vollständige Tabelle steht in [`loader_design.md` Abschnitt 8.2](loader_design.md#82-abweichungen-in-m2). Die Entscheidungen E1–E12 gelten unverändert.

| # | Kurzfassung |
|---|---|
| B1 | `NiPCRenderer` aus dem Spielpool (wie das Original), 79 statt „alle“ D3D-Fallen (ohne Shims und Kernel-Thunk). |
| B2 | Shim-Header in XDK-Kodierung (`// ASSUMPTION`), 50-Hz-Takt in `DisplayFrame`, Sektion `[render]`. Slot 65 seit Phase 4 in der Reihenfolge von `FUN_0015E010` mit 8×8-Default-Textur (B18). |
| B3 | `DirectSoundCreate` liefert ein Dummy-Device ≠ NULL, XMV einen Dummy-Decoder (sonst übersprungenes Voice-Update bzw. hängender Film). |
| B4 | Fiber-Stacks mit Terminal-Record bei StackBase−0x10 und Fiber-Top-Record (SEHOP-fest). |
| B5 | APC-Zustellung nur bei UserMode + Alertable; seit Phase 4 getrennte Zähler für gescheiterte Reads (Disc-Fehler-Pfad) und Writes (Speicherschicht). |
| B6 | VFS-Trace mit Tabelle dokumentierter Fehlstellen; seit Phase 4 mit Log-Uhr, Thread-Namen, `[self-test]` und engeren `partition1`-Mustern. |
| B7 | Beobachter-Probes auf Spielcode (jetzt sechs) und Disc-Fehler-Falle. |
| B8 | Exit-Reporter in core statt direkter Aufrufe im `--frames`-Ende. |
| B9 | Archivsuchen stehen im VFS-Trace. |
| B10 | `trap:Game::DiscErrorScreen` beendet sofort statt der Endlosschleife des Originals. |
| B11 | Kriterium 4 verlangt ab 1000 Frames, dass Intro endet. |
| B12 | Erwartete Sprache aus `[settings] language`; Spielsprache ohne Groß-/Kleinschreibung verglichen. |
| B13 | `FATAL_STUBS` leer. |
| B14 | **Neu:** `probe:Game::ContextSwitch` 0xA48C0 sieht jeden Kontextwechsel von `Game::Frame` (Befund 4). |
| B15 | **Neu:** Watchdog beendet auch ein hängendes geordnetes Prozessende mit Exitcode 5 (Befund 7). |
| B16 | **Neu:** Aktivität der Selbsttests wird getrennt gezählt und markiert (Befunde 5, 8b). |
| B17 | **Neu:** Auswahl-Schlüssel der Konfiguration, ungültige Werte → Exitcode 1 (Befund 10). |
| B18 | **Neu:** Slot 65 nach `FUN_0015E010` (Befund 1). |

## 8. Review-Befunde

Jeder Befund wurde vor der Änderung am Dekompilat bzw. am Binary nachvollzogen.

| # | Schwere | Befund | Entscheidung | Umsetzung und Nachweis |
|---|---|---|---|---|
| 1 | niedrig | Slot 65 wählt Bildquelle und Fehlerfall anders als `FUN_0015E010` | **angenommen** | Dekompilat bestätigt: `+0x30` (Pfad) vor `+0x34`, Bitschleifen auf Breite/Höhe (Zweierpotenz), Rückfall auf `renderer+0x5A0/+0x598`. Die Default-Textur legt `FUN_0014AB50` als `NiPixelData` 8×8 mit einer Mip-Stufe an (`FUN_00111260(8, 8, [+0x598], 1)`). Umsetzung (`gfx\nipc_renderer.cpp`): Reihenfolge des Originals; ohne Bild, bei unlesbarer Datei oder Nicht-Zweierpotenz 8×8 mit A8R8G8B8 (`// ASSUMPTION` für das Format), GFX/debug statt warn, Zähler in der Statistikzeile. Nicht nachgebildet und dokumentiert: die D3D-Formatwahl `FUN_001572D0`. Lauf: 140 Source-Texturdaten, 0 Dateien gelesen (keine Textur hat einen Pfad), 4 Default-Texturen (Filmtexturen ohne Bild), 0 Nicht-Zweierpotenzen; Kriterien unverändert erfüllt |
| 2 | niedrig | Gescheitertes `WriteFileEx` wird als Disc-Fehler gemeldet und gezählt | **angenommen** | Rohscan: `[0x2935FC]` (`NtUserIoApcDispatcher`) nur bei 0xF7625 (`ReadFileEx`) und 0xF7676 (`WriteFileEx`); `DAT_0030E68C` gelesen nur bei 0x13F1E3 und 0x13F6B2 (Setter 0x13FAD5). Callgraph: `ReadFileEx` nur aus `FUN_0013F660`, `WriteFileEx` nur aus `FUN_000E0B00`, das bei FALSE nur die Datei schließt. Umsetzung (`kernel\io_apc.*`): `failedRequests` nur für `NtReadFile` (KRNL/error), neu `failedWriteRequests` für `NtWriteFile` (KRNL/warn „save layer“); Zeile `failed APC reads N (disc error path), failed APC writes N (save layer)`; `m2_check.py` wertet nur die Reads. Auch der Text der Fehler-Completion nennt beide Completions. Unit-Test „apc: failed APC requests are split …“ |
| 3 | mittel | Kriterium 2 scheitert falsch für `english`/ohne Schlüssel | **angenommen** | Dekompilat bestätigt: `FUN_000D9B20` ersetzt den Vorgabenamen „English“ nur, wenn `__stricmp` abweicht; `FUN_000D9990` liefert für alles außer 3–6 „english“. Umsetzung: Vergleich ohne Groß-/Kleinschreibung; die Sprachnamen in `m2_check.py` folgen jetzt dem Kernel (`tchinese` statt `chinese` für 8, das Skript lehnte den gültigen Kernel-Namen ab). Läufe `run\m2fix\en` (ohne Schlüssel) und `en-explicit` (`english`): `XGetLanguage() = 1`, Spielsprache `English`, PASS. ctest-Variante über `FUT_M2_EXTRA_TEST_CONFIGS` (`fut_m2_check_ctest-en`). B12 und INTERFACES ergänzt |
| 4 | niedrig | Kontextwechsel hin und zurück innerhalb eines Frames unsichtbar | **angenommen** | Dekompilat/capstone: `Game::Frame` wechselt in der Schleife der festen Schritte (bis 6 pro Frame); `CApp+0xBC` wird nur bei 0xA2E28 geschrieben. Umsetzung: Probe `probe:Game::ContextSwitch` auf `FUN_000A48C0` (Aufruf 0xA2E40 direkt nach `cur = next`), gefiltert auf den Rücksprung 0xA2E45; der Vergleich nach `CApp::Frame` bleibt als Rückfall (`[seen after the frame]`, in der Zusammenfassung gezählt). LONG_RUN_FINDING4 Die Aussage „zurück nach einem Frame“ des Integrationsberichts ist korrigiert: Attract endet im selben Frame |
| 5 | niedrig | Fiber-Teil von Kriterium 5 schon durch den Selbsttest erfüllt | **angenommen** | Bestätigt: M1-Log `fiber summary: 2 switches`, im M2-Log stand die einzige „first SwitchToFiber“-Zeile auf dem Boot-Thread (Selbsttest). Umsetzung: `core::InSelfTest()` (Selbsttest-Runner); `FiberStats::selfTestSwitches`; Zusammenfassung `(G game, S self-test; …)`; je eine Zeile `fiber: first SwitchToFiber (self-test)` / `(game)`. `m2_check.py` verlangt G > 0 und die Game-Zeile; `GuestSehOnFiberStack` prüft, dass seine zwei Wechsel als Selbsttest zählen. Lauf: 4930 Wechsel des Spiels, 2 des Selbsttests; erster Wechsel des Spiels bei 0,11 s auf `guest-main` |
| 6 | niedrig | Kriterium 4 scheitert bei kurzen Frame-Limits; stderr-Ausgabe erklärt nichts | **angenommen** | Umsetzung (`m2_check.py`): Unter 1000 Frames Warnung zu Beginn, Kriterium 4 verlangt dann nur „Intro läuft + geordnetes Ende“. Bei einem Fehlschlag zeigt das Skript eine stderr-Zeile, die keine Log-Zeile ist, sonst die erste error/warn-Zeile des Logs. Nachweis: `run\m2fix\frames100` (2 Läufe à 100 Frames) ALL PASS; eine INI mit `language = 10` zeigt `stderr: futurama: configuration error: invalid value for [settings] language: '10' …` |
| 7 | niedrig | Watchdog kann einen Hänger im geordneten Prozessende nicht beenden | **angenommen** | Im Code bestätigt: Der zweite `ExitHostProcess`-Aufruf (Watchdog) endete in `Sleep(INFINITE)`. Umsetzung (`core\host_main.*`, `platform\platform.cpp`): `core::ExitInProgress()`; läuft schon ein Ende, beendet `ExitHostProcess(kExitWatchdog)` sofort mit `TerminateProcess` (stderr-Zeile ohne Log, dessen Lock der hängende Thread halten kann); der Watchdog überspringt dann Thread-Dump und Minidump. Testschalter `[debug] exit_hang_test` (Exit-Reporter, der nie zurückkehrt). Lauf `run\m2fix\exit-hang` (100 Frames, Timeout 20 s): Exitcode 5 nach 20 s, stderr `futurama: watchdog: the process exit did not finish within [debug] timeout - terminating with exit code 5` |
| 8 | niedrig | (a) `partition1`-Muster dokumentiert jede Fehlstelle unter `U:`; (b) Selbsttest-Öffnung im Trace als Treffer gezählt | **angenommen** | (a) `vfs\vfs_known_missing.cpp`: nur Spielstand-Verzeichnisse mit genau 12 Hex-Zeichen (`FUN_000F6544`) und die fünf Spielstand-Dateien `SaveMeta.xbx`, `Futurama.dat`, `Options.dat`, `Unlock.dat`, `saveimage.xbx` ([FIL] 4.1/4.2). `TitleMeta.xbx`, Tippfehler oder Nicht-Hex-Namen sind UNDOCUMENTED. Unit-Test mit 13 Pfaden. (b) Öffnungen während eines Selbsttests tragen `[self-test]`; `m2_check.py` zählt sie getrennt (20 Treffer des Spiels + 1 Selbsttest) |
| 9 | niedrig | `vfs_trace.log` und `futurama.log` haben verschiedene Zeitbasen | **angenommen** | Bestätigt: Trace ab Prozesserzeugung (`GetProcessTimes`), Log ab QPC beim Log-Start, etwa 18 ms Versatz. Umsetzung: `core::LogSeconds()`/`core::LogThreadName()` (log.h), der Trace schreibt `[<Log-Sekunden>] <tid>:<Thread-Name>`; der Kopf erklärt das Format. Lauf: Log `[0.046712] … archive: mounted sysPAL.img`, Trace `[0.046726] 27476:guest-main archive mount "sysPAL.img"` (14 µs, die Trace-Zeile folgt der Log-Zeile). Unit-Test „trace: VFS trace lines carry the log clock and the log thread name“ |
| 10 | niedrig | Ungültiges `[settings] language` endet mit Exitcode 2 statt 1 | **angenommen** | Umsetzung (`core\config.*`): `KeyType::Choice` mit erlaubten Werten; `[settings] language` (Namen des Kernels oder 1..9), `[saves] key_mode` (fixed\|xbox) und `[render] d3d_traps` (abort\|log) werden beim Laden geprüft. `--set=settings.language=klingon` und `=10` → Exitcode 1 (`configuration error: invalid value for [settings] language …`). Unit-Tests für gültige/ungültige Werte und dafür, dass jede Auswahl auch vom Kernel angenommen wird |
| 11 | niedrig | Ungenaue Messwerte im Integrationsbericht | **angenommen** | Bestätigt: Slot 65 hatte 161 Aufrufe, aber 140 neue Source-Texturdaten; ein M2-Log hatte 161 `HOOK INFO`-Zeilen (160 Hooks + Summenzeile), ein M1-Log 162 (zusätzlich `stop-at`). Korrigiert in Design 8.2 und hier. Seit Befund 4 sind es 161 Hooks: 162 Zeilen im M2-Log, 163 im M1-Log. Die Kontext-Historie war wegen Befund 4 unvollständig (behoben) |

Kein Befund wurde abgelehnt.

## 9. Bekannte Probleme und offene Punkte

- **Disc-Fehler-Falle ungetestet:** `trap:Game::DiscErrorScreen` wurde nie ausgelöst. Ein Lesefehler lässt sich ohne Fehlerinjektion im VFS nicht provozieren; geprüft sind Installation, Prologbytes und die Registrierung des Handlers.
- **Im Spiel nicht erreicht:** Slot 15 auf eine Textur (Render-to-Texture), der Texturdatei-Pfad von Slot 65 (keine Textur hat einen Pfad), `[audio] dummy_streams`, APC-Fehlerpfade (nur Unit-Tests), der Speicherpfad über `WriteFileEx`/0xE0030 (M7), `[render] load_textures = 0`.
- **Slot 65 und die D3D-Formatwahl:** `FUN_001572D0` (Zielformat aus den Renderer-Präferenzen +0x528) ist nicht nachgebildet; ihr NULL-Fall (Default-Textur) fehlt. Die gespeicherte Pixelformat-Beschreibung ist die der Quelle. Das klärt M3 mit den echten D3D11-Formaten.
- **Attract-Modus:** Wegen des XMV-Stubs endet `Attract-PAL.xmv` sofort; das Spiel pendelt alle etwa 65 s zwischen Frontend und Attract. Erwartet bis M6.
- **`[render] frame_rate = 0`** taugt nicht für Kriterium 4 (zeitbasierte Ladeschleifen laufen leer); `m2_check.py` warnt.
- **Doppelte Purge-Messung:** `xdk::MeasureEntryPurge` und `gfx::MeasureCalleePurge` implementieren dieselbe Methode und sollten zusammengelegt werden.
- **APC-Queues beendeter Threads** werden nicht verworfen (das XBE hat nur einen Gast-Thread). `NtWaitForSingleObject` stellt nie APCs zu (`// ASSUMPTION`: KernelMode).
- **Kontext-Historie** in der Spielzusammenfassung ist auf 32 Einträge begrenzt; die `game: context`-Zeilen im Log sind vollständig.
- **Gegenprobe für Befund 7 nur mit dem Fix:** Der Testschalter `exit_hang_test` existiert erst mit dem Fix; dass der alte Code ewig hing, folgt aus dem Code (`Sleep(INFINITE)` im zweiten `ExitHostProcess`), wurde aber nicht gemessen.
- **Lizenz/Assets:** wie M1 – keine Spieldaten im Repository, Testläufe nur unter `run\`.

## 10. Nächste Schritte für M3 (erstes Bild mit D3D11)

Ziel laut Design-Abschnitt 6 (M3): Splash (`NI_Splash_PAL.nif`), Legal-Bild und Frontend-Menü erscheinen vollständig, ohne D3D-Debug-Layer-Fehler, `--frames=3000` mit ≥ 50 fps. Grundlage ist die Prioritätenliste aus [REN] 7.2 und die in M2 gemessene Slot-Nutzung (Abschnitt 5):

1. **Fenster und RHI** (Stufe 12 `CreateWindow`, bisher leer): Win32-Fenster 640×480 (skalierbar), D3D11-Device mit Debug-Layer im Debug-Build, Swapchain mit VSync. Der `HostRenderer` hängt an `NiPCRenderer` +0x6C0; die Message-Loop des Boot-Threads bedient das Fenster.
2. **P0 – Frame-Gerüst** in Aufrufreihenfolge eines Frames (`CApp::Frame` → `NiCamera::Click`): **17 `ClearBuffer`** (3015 Aufrufe/60 s; Farbe aus **27 `SetBackgroundColor`**, Tiefe/Stencil aus 26/28), **47 `SetCameraData`** (View/Projektion aus dem `NiFrustum`, Viewport aus dem Port wie 0x1469E0), **45/46 `BeginScene`/`EndScene`**, **16 `DisplayFrame`** = Present (der 50-Hz-Takt bleibt als Option), **21/22** Breite/Höhe, **15 `SetRenderTarget(NULL)`**. 56 und 71 bleiben No-ops.
3. **P1 – Bilder und Menü:** **65 `CreateSourceTextureRendererData`** wirklich hochladen: `NiPixelData` linear → RGBA8/RGB565/BCn (Formattabelle [REN] 5.2), P8 über die Palette (Slot 69) expandieren, dabei die Default-Textur (8×8) und die Formatwahl von `FUN_001572D0` festlegen. **54 `RenderScreenPolygon`** (45 Aufrufe: Legal-/Splash-Bild, Movie-Quad) und **55 `RenderScreenTexture`** (4838 Aufrufe: Text/HUD des Frontends; [REN] stuft ihn als P2 ein, gemessen ist er im Menü aber nötig). **48 `RenderShape`** (≈ 762 000 Aufrufe im Frontend) mit Vertex-/Index-Cache an `NiGeometryData+0x30` (Slot 39 bleibt FALSE), danach 49–51 (Triangles/Strip/Strips).
4. **Property → State** in der Reihenfolge Alpha, ZBuffer, Texturing (Basis-Map), VertexColor/Material, danach Specular/Fog/Stencil/Wireframe/Shade/Dither ([REN] 5.4); Fixed-Function-Licht im Shader ([REN] 5.5). Referenz ist der REN-Code (`FUN_00159cd0`, `FUN_0015ba40`).
5. **Movie-Pfad vorbereiten:** Die Shim-Textur (YUY2) bekommt eine echte D3D11-Textur; der Inhalt kommt erst mit M6. `YuvEnable` und `SetScreenSpaceOffset` gehen an den `HostRenderer`.
6. **Prüfung:** `tools\m3_check.py` nach dem Muster von `m2_check.py` (Screenshots an festen Frames, Vergleich mit xemu-Aufnahmen, Debug-Layer-Meldungen als Fehler, Bildrate), Screenshot-Taste.
7. **Aufräumen aus M2:** Purge-Messung zusammenlegen; Fehlerinjektion im VFS (`[debug] fail_reads`), um die Disc-Fehler-Falle und die APC-Fehlerpfade einmal im Spiel auszulösen.
