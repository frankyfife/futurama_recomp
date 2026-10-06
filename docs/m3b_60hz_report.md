# Meilenstein M3b: 60 fps – Bericht

Stand: 2026-10-06 (Phase 5, nach Behebung der Review-Befunde) · Ziel-XBE: `orig\retail_eu\default.xbe` (Retail EU) · Verbindliches Design: [`docs/loader_design.md`](loader_design.md) (1.9 Zeitbasis, 6 M3/M3d, 7 R9, Abweichungen 8.4 B38–B48) · Analysen: [`analysis/loader/timing60.md`](../analysis/loader/timing60.md) (Zeitbasis, tick-gebundene Stellen, Review-Korrekturen in Abschnitt 12), [`analysis/loader/pacing60.md`](../analysis/loader/pacing60.md) (Bildtakt) · Schnittstellen: [`src/INTERFACES.md`](../src/INTERFACES.md) 4.19/4.20 · Vorgänger: [`docs/m3_report.md`](m3_report.md)

## 1. Ergebnis in Kürze

**Der Port läuft mit 60 fps.** Die verbindliche Vorgabe des Nutzers lautet „mindestens 60 fps, kein 50-Hz-PAL-Timing“. Beides ist erfüllt, ohne die PAL-Inhalte anzutasten:

- **Spiellogik mit 60 Hz:** Ein Operanden-Patch im PAL-Zweig von `Game::Init` 0xA26A0 setzt die Zeitwerte des NTSC-Zweigs desselben Blocks (fps 60, dt = 1/60 s). CApp+0xE8 bleibt 0, damit bleiben PAL-Archive, Sprachwahl, `*-PAL.xmv`-Filme und PAL-Splash/-Legal. Das Spiel liest die Werte nach `Game::Init` in jedem Lauf zurück: `fps 60 (0x42700000), dt 0.0166666675 s (0x3C888889), …, CApp+0xE8 = 0, movie tag "PAL" - ok`.
- **Jeder Frame zeigt genau einen Simulationsschritt:** Eine Lockstep-Uhr speist die drei Zeitproben von `Game::Frame` (99,56–99,80 % der Frames genau ein Schritt; mit echter Zeit waren es 98,0 %).
- **Bildsynchroner Takt:** Auf dem 120-Hz-Messrechner bleibt jedes Bild genau 2 Refreshes stehen (`vblank k = 2`, Present(2) auf einer Swapchain mit Frame-Latency-Waitable). `m3_check` misst 60,01–60,03 Hz über 3000 Frames, 99,76–99,90 % der vblank-gekoppelten Frames genau 2 Refreshes, 0–1 Takt-Neustarts. Vorher (M3, 50-Hz-Takt) fiel die Rate hier auf 49,2–49,3 Hz, weil 50 Hz kein Teiler von 120 Hz ist.
- **Inhalt unverändert:** Legal-Bild und Tech-Notice bleiben texelgenau gleich zu xemu (0,00), Ladebild 0,29–0,30, Frontend 0,01 auf den statischen Pixeln. Echtzeit-Szenen dauern gleich lang wie bei 50 Hz (Legal 6,0 s, Tech-Notice 5,05 s, Frontend-Leerlauf bis Attract 60,2 s).

Bewusst übernommen wird das Verhalten der NTSC-Fassung für alles, was das Spiel pro Tick statt pro Sekunde zählt (Abschnitt 6): UI-Fades, Kameraglättung, aber laut Review auch die Starrkörper-Dämpfung, das Zielen des Chicken Walker und Skriptschleifen mit kurzen Waits. Der NTSC-Prototyp hat denselben Code und dieselben Skripte und lief mit 60 Hz.

Das Review hat 14 Befunde erhoben; alle sind nachgeprüft. Befund 4 ist in einem Teilpunkt abgelehnt, Befund 13b ist plausibel, aber hier nicht prüfbar, alle anderen sind bestätigt (Abschnitt 7). Code-Korrekturen betreffen den Takt (Wiederherstellen nach Minimieren, Kadenzwächter für Timer-Pläne – in Phase 5 zusätzlich gegen VRR gehärtet –, VRR-Aufwärmzeit, Statistik), die Konfiguration und `m3_check` (neues Kriterium M3b-7). Die Dokumentation der tick-gebundenen Stellen ist korrigiert.

## 2. Was sich geändert hat

| Bereich | Änderung | Dateien |
|---|---|---|
| Zeitbasis | HostMain-Stufe 8b `ApplyGameTiming`: Patch-Tabelle mit 5 Operanden- und 2 `.data`-Zeilen (`[game] tick_rate = 60`), 16 unveränderte Referenzzeilen; jede Zeile wird vor dem Schreiben gegen das Abbild **und** die XBE-Datei geprüft. Rücklesen nach `Game::Init`, Selbsttest `GameTimingPatched` | `xapi\game_timing.*`, `core\guest.h`, `core\host_main.cpp`, `core\selftest_m1.cpp` |
| Frame-Clock | `probe:Game::Timer` (FUN_000D2B90) liefert den drei Zeitproben von `Game::Frame` eine Lockstep-Zeit (+ticks(dt) je Frame, Korrektur erst ab ¾ Schritt); `probe:Game::FixedStep` zählt Schritte. Hooks 161 → 163 | `xapi\game_timing.*`, `xapi\game_probes.*` |
| Takt | Neuer Pacer: Planwahl aus `[render] pacing`, Bildwiederholrate (QueryDisplayConfig, rational), Tearing, VRR-Probe und Fensterzustand; Pläne `vblank k`, `display-rate`, `tearing-timer`, `timer`, `free`, `unthrottled`, `headless`; Statistik aus den DXGI-Frame-Statistiken; optionaler CSV-Trace | `platform\frame_pacer.*`, `pacer_policy.*`, `pacer_display.*`, `window.*` |
| RHI / D3D11 | Swapchain mit Frame-Latency-Waitable, Tearing nur auf Anforderung, übersprungene Presents, Present-Zähler und Frame-Statistik | `gfx\rhi\rhi.h`, `gfx\rhi\d3d11\*` |
| Renderer | `GpuRenderer::DisplayFrame` fragt `PlanPresent`, meldet `NoteSwapChainPresent`; `OnFrameDisplayed` ruft `EndFrame` (beide Backends) | `gfx\nipc_gpu.*`, `gfx\nipc_renderer.cpp`, `gfx\gfx_config.*` |
| Konfiguration | `[game] tick_rate`, `[game] frame_clock`, `[render] pacing`, `[render] pacing_trace`; `[render] frame_rate` folgt der Tickrate | `core\config.*` |
| Prüfskripte | `m2_check`/`m3_check` rechnen in Sekunden × Rate, neue Kriterien T1/T2 (M3b-5/6) und M3b-7; neues `tools\timing_compare.py` | `tools\*.py` |
| Tests | `tests\timing_tests.cpp`, `tests\pacer_tests.cpp`, `tests\gfx_tests.cpp`: 229 Testfälle, 22 470 Assertions (1 manueller Test übersprungen) | `tests\*` |
| Phase 5 (Review) | siehe Abschnitt 7 | `pacer_policy.*`, `frame_pacer.*`, `config.cpp`, `gfx_config.cpp`, `m3_check.py`, Dokumentation |

## 3. Konfiguration

Alle Schlüssel sind optional; ohne Angabe läuft der Port mit 60 Hz und automatischem Takt.

```ini
[game]
tick_rate   = 60        ; 60 (Standard): NTSC-Zeitwerte mit PAL-Inhalten | 50: Original-PAL (xemu-Vergleiche)
frame_clock = lockstep  ; lockstep (Standard): ein Schritt je Frame | realtime: Original-Zeitbasis von Game::Frame

[render]
pacing       = auto     ; auto (Standard) | vsync | timer | off
vsync        = 1        ; 0: auto nutzt den Timer mit Present(0, ALLOW_TEARING); von pacing = vsync ignoriert
;frame_rate  = 60       ; Standard = tick_rate; ein anderer Wert loggt eine Warnung; 0 = ungebremst (nur mit auto)
;pacing_trace = D:\...\pacing_trace.csv   ; eine CSV-Zeile pro Frame (Diagnose)
```

| `[render] pacing` | Verhalten | Einsatz |
|---|---|---|
| `auto` | Refresh = k · 60 Hz (60/120/180/240, auch 59,94/119,88): `vblank k` (Present(k) auf dem Waitable). Sonst mit Tearing-Unterstützung `tearing-timer` (60-Hz-Timer + Present(0, ALLOW_TEARING), VRR-Probe nach 2 s Aufwärmzeit); bei abgelehntem VRR `display-rate` (Present(1), ein Spielbild je Refresh). Im Hintergrund, minimiert, bei Display aus oder gesperrter Sitzung `free` (Timer, nie blockierendes Present) | Standard |
| `vsync` | immer vblank-gekoppelt (`vblank k` oder `display-rate`), kein Wechsel in `free` bei sichtbarem Fenster | Diagnose |
| `timer` | absoluter Hochauflösungs-Timer mit der Zielrate, Present(0) in freie Slots; auf k · Ziel mit Kadenzwächter | 50-Hz-Prüfungen (`run\m3b\pal50`, `run\m3final\*`) |
| `off` | ungebremst, Presents nur in freie Slots | Messungen |

Kommandozeile: `--set=game.tick_rate=50`, `--set=render.pacing=timer` usw. (Werte werden wie in der INI getrimmt und entquotet). Ungültige Werte für die Auswahl-Schlüssel enden mit Exitcode 1.

**Prüfkonfigurationen:** `run\m3b\{debug,rel,clang}` (60 Hz, `auto`), `run\m3b\pal50` (`tick_rate = 50`, `pacing = timer`), `run\m3b\{m1,m2,m2-null}`, `run\m2fix\ctest-en`; `run\m3final\*` (M3-Belege) setzen jetzt `tick_rate = 50` + `pacing = timer`.

**Prüfbefehle** (aus `D:\FuturamaDecomp`; Bauen wie m3_report 3.1 mit `-DFUT_WERROR=ON`):

```powershell
python tools\m3_check.py --exe build\m3b\bin\futurama.exe --config run\m3b\debug\futurama.ini --runs 3 --fresh
python tools\m3_check.py --exe build\m3b-rel\bin\futurama.exe --config run\m3b\rel\futurama.ini --runs 1 --fresh
python tools\m3_check.py --exe build\m3b-clang\bin\futurama.exe --config run\m3b\clang\futurama.ini --runs 1 --fresh
python tools\m3_check.py --exe build\m3b\bin\futurama.exe --config run\m3b\pal50\futurama.ini --runs 1 --fresh
python tools\m1_check.py --exe build\m3b\bin\futurama.exe --config run\m3b\m1\futurama.ini --runs 3 --xbe orig\retail_eu\default.xbe --fresh
python tools\m2_check.py --exe build\m3b\bin\futurama.exe --config run\m3b\m2\futurama.ini --runs 1 --fresh
python tools\m2_check.py --exe build\m3b\bin\futurama.exe --config run\m3b\m2-null\futurama.ini --runs 1 --fresh
python tools\timing_compare.py --run run\m3b\dur50 --run run\m3b\dur60
build\m3b\bin\fut_tests.exe
```

## 4. Belege

### 4.1 Zeitbasis (Rücklesen, jeder Lauf)

```
game timing: 60 Hz (NTSC timing values, PAL content) - [game] tick_rate = 60; 7 patches applied: 5 operands in the
  PAL branch of Game::Init 0x000A26A0, 2 .data initial values (fps 0x002D6744, dt 0x002D6748); 16 reference rows
  verified; CApp+0xE8 and the "PAL" movie tag unchanged
game: video standard CApp+0xE8 = 0 (PAL; 60 fps, fixed time step 0.0167 s, movie tag "PAL")
game timing: effective after Game::Init: tick 60 Hz, fps 60 (0x42700000), dt 0.0166666675 s (0x3C888889), fixed
  step 1, dt clamps 0x3C888889/0x3C888889, fps copies 0x42700000/0x42700000, [0x002E7B68] 0x3D064B8A,
  CApp+0xE8 = 0, movie tag "PAL" - ok (NTSC timing values, PAL content)
game ticks: 2994 fixed steps in 49.944 s (... steady 59.988 steps/s), steps per CApp::Frame over 2985 frames:
  0 x 1, 1 x 2977, 2 x 5, 3+ x 1 (max 6)
```

Geöffnete Filme: nur `fox/sci/uds/intro-PAL.xmv` (M3b-5). Mit `tick_rate = 50` patcht der Host nichts und liest fps 50 / dt 0,02 zurück.

### 4.2 Prüfläufe (Phase 5, Endstand)

Endstand nach allen Korrekturen (Build vom 2026-10-06; FUT_WERROR=ON, alle drei Builds ohne Warnung; `gen_all.py --check` aktuell). Alle Läufe `ALL PASS`:

| Prüfung | Build / Konfiguration | Ergebnis |
|---|---|---|
| `fut_tests` | MSVC Debug, RelWithDebInfo, clang-cl Debug | je 229/229 Testfälle, 22 470 Assertions (1 manueller Test übersprungen) |
| `m3_check`, 3 Läufe `--fresh`, 60 Hz | MSVC Debug, `run\m3b\debug` | 3000 Frames in 49,98 s: **Ø 60,01 / 60,03 / 60,01 Hz**, langsamstes 1-s-Fenster 58,92–58,94 Hz, 0/1/0 Takt-Neustarts (eine abgelaufene Vblank-Wartezeit); 99,77–99,80 % der Frame-Starts im Takt, 1–3 Starts über 1,5 Perioden; Frame-Arbeit Ø 2,74–2,78 ms, 99 % ≤ 4,4 ms (tragfähig 360–365 Hz); vblank k = 2 für 2969 von 3000 Frames, davon 99,76 % genau 2 Refreshes; gemessener Refresh 120,001–120,002 Hz; Present → sichtbar Median 13,0 ms; Vordergrund 99,9 %; 59,995–59,997 Schritte/s, 99,56–99,60 % der Frames genau ein Schritt; Debug-Layer 0/0/0; xemu Legal 0,00, Tech-Notice 0,00, Ladebild 0,29–0,30, Frontend 0,01 |
| `m3_check`, 1 Lauf | MSVC RelWithDebInfo (`debug_layer = on`), `run\m3b\rel` | Ø 60,01 Hz, langsamstes Fenster 59,31 Hz, Arbeit Ø 2,05 ms / 99 % ≤ 3,4 ms (487 Hz), 99,93 % im Takt, 99,90 % genau 2 Refreshes, 99,73 % genau ein Schritt |
| `m3_check`, 1 Lauf | clang-cl Debug, `run\m3b\clang` | Ø 60,01 Hz, langsamstes Fenster 59,09 Hz, Arbeit Ø 2,59 ms / 99 % ≤ 4,2 ms (386 Hz), 99,93 % im Takt, 99,90 % genau 2 Refreshes, 99,66 % genau ein Schritt |
| `m3_check`, 50 Hz | MSVC Debug, `run\m3b\pal50` (`tick_rate = 50`, `pacing = timer`) | Ø 50,01 Hz, 100 % im Takt, Rücklesen fps 50 / dt 0,02 ohne Patches, 50,000 Schritte/s; xemu Ladebild 0,00 |
| `m1_check` | Debug 3 Läufe, RelWithDebInfo 2 Läufe, `run\m3b\m1` | 29 Selbsttests inkl. `GameTimingPatched`; Logs nach Maskierung gleich |
| `m2_check` | Debug + RelWithDebInfo `run\m3b\m2` (d3d11), Debug `run\m3b\m2-null` (Null-Renderer), Debug `run\m2fix\ctest-en` (Englisch) | inkl. T1/T2; 59,997–59,999 Schritte/s, 99,60–99,80 % genau ein Schritt; Intro verlassen bei `CApp::Frame` #889–890 |
| Dauerlauf 30 000 Frames (Phase 3) | MSVC Debug, `run\m3b\long` | 500 s, 8 Attract-Zyklen zu je 64,07–64,08 s, Ø 59,99 Hz, 60,010 Schritte/s, Spielpool am Ende 5783 KiB |

### 4.3 Dauern 50 Hz gegen 60 Hz

Gemessen in Phase 3 mit `tools\timing_compare.py` (je 155 s, `run\m3b\dur50` mit `pacing = timer`, `run\m3b\dur60` mit `auto`; Phase 5 hat die Spiellogik nicht verändert). Alles, was das Spiel in QPC- oder dt-Sekunden zählt, ist bis auf einen Tick gleich:

| Observable (timing60.md 8) | 50 Hz | 60 Hz | Differenz | Erklärung |
|---|---|---|---|---|
| Legal-Bild | 6,020 s | 6,001 s | −19 ms | QPC 6,0 s – gleich |
| Tech-Notice `NI_Splash_PAL.nif` | 5,060 s | 5,050 s | −10 ms | QPC max. 5,0 s – gleich |
| Ladebild `Loadmenu.nif` | 4,400 s (220 Frames) | 3,667 s (220 Frames) | −734 ms | frame-gebunden: die Ladewarteschlange läuft einmal pro Frame, die Host-E/A ist sofort fertig (B46) |
| Intro → Frontend (ab Start) | 15,983 s | 15,170 s | −813 ms | davon 734 ms Ladebild |
| Frontend-Fade | 0,864 s | 0,737 s | −127 ms | tick-gebunden (50 Ticks), NTSC-Verhalten |
| Frontend-Leerlauf → Attract | 60,220 s | 60,199 s | −21 ms | dt-Sekunden – gleich |
| Attract-Zyklus | 64,860 s | 64,066 s | −794 ms | 60 s + frame-gebundenes Neuladen (≈ 222 Frames) |
| Dampfwolke im Frontend | 1,000 s (50 Frames) | 1,004 s (60 Frames) | +4 ms | NIF-Zeit – gleich |
| Filmnamen | `…-PAL.xmv` | dieselben | – | Tag „PAL“ unverändert |

Alle Werte beider Raten liegen in den Schranken der xemu-Aufnahmen des Originals (Legal 3–9 s, Tech-Notice 3–9 s, Ladebild 0–6 s, Frontend bis Attract 57–63 s, Dampfwolke 1,0 s). Der Dauerlauf mit 30 000 Frames (500 s, Phase 3) hatte 8 Attract-Zyklen zu je 64,07–64,08 s, Ø 59,99 Hz und 60,010 Schritte/s.

### 4.4 Takt auf dem 120-Hz-Messrechner

Der Messrechner (LG-TV 3840×2160 an RTX 5090, 120/1 Hz laut QueryDisplayConfig, 225 % DPI) hat seinen Präsentationszustand zwischen Phase 3 und Phase 5 geändert. Am 2026-10-05 war das Fenster komponiert (kein VRR), am 2026-10-06 folgte das Panel im Fenster den Presents (VRR, **Vermutung:** Independent Flip auf einer Overlay-Ebene; die Ursache ist unbekannt). Der Standardpfad `vblank k = 2` verhält sich in beiden Zuständen gleich.

**Phase 3, 2026-10-05 (komponiert, kein VRR)** – je 3000 Frames mit `m3_check`, MSVC Debug (`run\m3b\pacing\*`, Details loader_design 8.4):

| Modus | Plan | Ø Hz | Starts im Takt | Auf dem Bildschirm | Present → sichtbar (Median) |
|---|---|---|---|---|---|
| `auto` (Standard) | vblank k = 2 (2970) + free (30, Start) | 60,01 | 99,80 % | 99,97 % genau 2 Refreshes | 21,3 ms |
| `vsync` | vblank k = 2 | 59,99 | 99,93 % | 99,93 % genau 2 Refreshes | 21,4 ms |
| `timer` | Timer + Present(0) | 60,02 | 99,93 % | 2958 von 2978 genau 2; in Review-Läufen je nach zufälliger Phase 1 % bis 37 % mit 1 oder 3 Refreshes (Befund 6) | 7,3 ms |
| `timer` mit Kadenzwächter (Läufe `run\m3b5\win\timer_1/2`, 60 s) | Timer | 60,01–60,02 | 99,97–100 % | 99,6 % bzw. 99,8 % genau 2 nach 1 bzw. 0 Verschiebungen | 4,4–5,8 ms |
| `auto`, Tick 50 | tearing-timer → VRR abgelehnt → display-rate 120 Hz | 114,77 | – | 1 Refresh je Frame (120 Hz) | 13,1 ms |
| `timer`, Tick 50 (`run\m3b\pal50`) | Timer | 50,01 | 100 % | 1: 848, 2: 839, 3: 772, 4: 437 (50 auf 120 Hz ohne VRR) | 8,5 ms |

**Phase 5, 2026-10-06 (VRR im Fenster aktiv)** – Endstand, MSVC RelWithDebInfo, je 1800 Frames (`run\m3b5\p5`, `run\m3b5\p5b`) bzw. `m3_check` (Abschnitt 4.2):

| Lauf | Plan | Ø Hz | Starts im Takt | Auf dem Bildschirm | Present → sichtbar (Median) |
|---|---|---|---|---|---|
| `auto` (`m3_check`: Debug 3 Läufe, RelWithDebInfo, clang-cl) | vblank k = 2 + free (30–31, Start) | 60,01–60,03 | 99,77–99,93 % | 99,76 % (Debug) bzw. 99,90 % (RelWithDebInfo, clang) genau 2 Refreshes | 13,0–14,0 ms |
| `timer`, erste Fassung des Kadenzwächters | Timer | **59,62** | 97,22 % | 1 Refresh von ≈ 16,7 ms je Frame (VRR); der Wächter verschob 50-mal in 30 s | 14,5–16,6 ms |
| `timer`, gehärteter Kadenzwächter (3 Läufe) | Timer | 60,02–60,03 | 99,83–100 % | 98,3–99,0 % der bewerteten Dauern „vom Panel gefolgt“, 0–1 Verschiebungen | 11,0–16,6 ms |
| `auto`, Tick 50 | free (31) → tearing-timer (1769) | 50,03 | 100 % | „VRR confirmed: the display follows the presents (100 %)“ nach 2 s Aufwärmzeit | 13,2–14,0 ms |
| `timer`, Tick 50 | Timer | 50,03 | 100 % | 1 Refresh je Frame (VRR) | 13,6 ms |
| `auto`, Minimieren/Wiederherstellen (Fenster im Vordergrund, 3600 Frames; Build vor der Härtung des Kadenzwächters, der vblank-Pfad ist davon unberührt) | vblank → free (ohne Present) → vblank | 60,07 | 99,61 % | Wiederherstellen bei 44,0 s: `free -> vblank k=2` im selben Frame (vorher 30 Frames = 0,48 s eingefrorenes Bild); 780 übersprungene Presents = 780 minimierte Frames | 15,8 ms |

Konfigurationsprüfungen (je 120 Frames): `"--set=render.pacing= timer"` ergibt jetzt den Plan `timer` (vorher still `auto`); `frame_rate = 0` mit `pacing = timer` und `vsync = 0` mit `pacing = vsync` loggen je eine GFX-Warnung.

Unter VRR zählt der Refresh-Zähler die verlängerten Refreshes; der „gemessene Refresh“ (119,9 Hz) ist dann kein Maß der Anzeige. Bei festem Refresh liefert die Regression 120,001–120,002 Hz.

## 5. Bewertung der Vorgabe „mindestens 60 fps“

| Anzeige | Verhalten | Vorgabe |
|---|---|---|
| 120 Hz (Messrechner), 60 Hz, 180/240 Hz | `vblank k` (k = Refresh / 60): jedes Spielbild genau k Refreshes; gemessen Ø 60,01–60,03 Hz, 99,76–99,90 % der vblank-gekoppelten Frames genau 2 Refreshes | **erfüllt** |
| 59,94 / 119,88 Hz | `vblank k` mit 59,94 fps; die Lockstep-Uhr holt alle 16,6 s einen Doppelschritt auf (Simulation, nicht gemessen) | knapp darunter (−0,1 %), offener Punkt (Abschnitt 8) |
| 144/165 Hz mit VRR | `tearing-timer` mit exakt 60 Hz, sofern die VRR-Probe bestätigt | erfüllt (Pfad auf dem Messrechner mit Tick 50 im Fenster bestätigt: „VRR confirmed“, 50,03 Hz; mit 144/165 Hz nicht gemessen) |
| 144/165 Hz ohne VRR | `display-rate`: Present(1) mit der vollen Rate, ein Spielschritt je 60-Hz-Raster (Bilder wiederholen sich unregelmäßig) | Bildrate ≥ 60, aber ungleichmäßig; gleichmäßig erst mit Interpolation (M3d) |
| Fenster im Hintergrund, minimiert | `free`: 60-Hz-Timer, das Spiel läuft mit 60 Schritten/s weiter | – |

Die Rate fällt nur bei synchronem Laden im Gast (Kontextwechsel, Neuladen des Frontends) für einzelne Vblanks unter 60 Hz: langsamstes 1-s-Fenster 58,9–59,3 Hz, 1–3 Frame-Starts über 1,5 Perioden je 3000 Frames, im 500-s-Dauerlauf Ø 59,99 Hz. Das Spiel holt die Zeit über seinen festen Schritt nach; der Takt bleibt im 60-Hz-Raster (0 Takt-Neustarts). Die Frame-Arbeit (Debug Ø 2,7–2,8 ms, 99 % ≤ 4,4 ms; RelWithDebInfo Ø 2,05 ms, 99 % ≤ 3,4 ms) reicht für 360–487 Hz. Mehr als 60 **verschiedene** Bilder pro Sekunde gibt es ohne Interpolation nicht (M3d).

## 6. Tick-gebundene Stellen: NTSC-Parität

Alles, was das Spiel pro festem Schritt ohne dt fortschreibt, läuft bei 60 Hz um den Faktor 60/50 anders als im PAL-Original – genau so wie in der NTSC-Fassung, die denselben Code und dieselben Level-Skripte mit 60 Hz betrieb (3112 von 3116 Spielfunktionen identisch, alle betroffenen Funktionen nachgeprüft, `timing_review.out` Abschnitt 5). Liste (timing60.md 4.10):

| Stelle | 50 Hz (PAL) | 60 Hz (= NTSC) | Art |
|---|---|---|---|
| Frontend-Fade, Eingabesperre, Menü-Entprellung, „Neues Spiel“, Overlay-Einblendung, Intro-Ladebeginn | z. B. Fade 1,00 s | 0,83 s | UI |
| Kamerawackeln, Glättungen pro Tick, Kamerafeder (feste 0,01-s-Unterschritte, 2 je Tick) | – | 20 % schneller | Kamera |
| Starrkörper-Dämpfung ×0.98/×0.9 je Tick (`FUN_000AFA30`: verschiebbare Objekte, Wippen, Minen) | Halbwertszeit 0,686 s, Ausrollweg v0·0,98 s | 0,572 s, v0·0,817 s (−17 %) | Spiel |
| Chicken Walker: Zielen/Drehen mit fest kodiertem Schritt 0.02 (`FUN_000445A0`) | 1,0 rad/s | 1,2 rad/s (+20 %) | Spiel |
| Skriptschleifen mit kurzen Waits (jede Schleife endet mit `Wait(0)`) | z. B. 0,080 / 0,120 s je Durchlauf | 0,067 / 0,133 s (−17 % / +11 %), kumulativ | Level-Abläufe |
| Tastenwiederholung (Periode aus dem `.data`-fps) | 12 Ticks = 0,24 s | 15 Ticks = 0,25 s (NTSC-Original: 0,20 s) | UI |

**Entscheidung:** NTSC-Parität, kein PAL-Ausgleich (timing60.md 12.2). Die NTSC-Fassung ist ein ausgeliefertes, abgestimmtes Verhalten; ein Ausgleich einzelner Stellen ergäbe eine Mischung, die es nie gab, und ein vollständiger Ausgleich ist statisch nicht erreichbar. Die Patchstellen für einen späteren Ausgleich von Dämpfung und Walker sind dokumentiert. Wer das PAL-Verhalten braucht, setzt `[game] tick_rate = 50`. Messbar werden Dämpfung und Walker erst mit Eingabe und Ingame-Szenen (Observablen 8.17b und 8.19, M4/M8).

## 7. Review-Befunde

Zwei Review-Agenten (Zeitbasis, Takt) haben 14 Befunde erhoben. Jeder wurde vor einer Änderung nachgeprüft.

| # | Schwere | Befund | Prüfung | Ergebnis | Umsetzung |
|---|---|---|---|---|---|
| 1 | mittel | Starrkörper-Dämpfung ist tick-gebunden, nicht dt-skaliert | Disassembly `FUN_000AFA30`, Aufrufkette pro Tick, ctor-Aufrufer, Prototyp (`timing_review.out` 1, 5) | **bestätigt** | Doku korrigiert (timing60.md 0/4.5/4.10/8.17b/12, `game_timing.h`, B40); NTSC-Parität entschieden, kein Patch |
| 2 | mittel | Chicken Walker zielt/dreht mit fest kodiertem Schritt 0.02 | einziger Schreiber 0x43EEC, Leser 0x44BA5/0x44BB2, Klemme ±0.05 im Decompilat, Prototyp (`timing_review.out` 2, 5) | **bestätigt** | timing60.md 4.8 korrigiert, 4.10, 8.19, B40; NTSC-Parität |
| 3 | niedrig | Jede Skriptschleife endet mit `Wait(0)`; Abweichungen summieren sich | Bytecode aller 23 PAL-Skripte: 3823 von 3823 Rücksprüngen nach `push_imm 0; WAIT` (`timing_review.out` 4) | **bestätigt** | timing60.md 4.3 (Periodentabelle, korrigierte Klassifikation), 4.10, 8.16, B40 |
| 4 | niedrig | Kamerafeder in festen 0,01-s-Unterschritten; Overlay ±0.03 pro Update; Eingabesperre im Pausemenü 30 Ticks | `timing_review.out` 3 | Kamera und Overlay **bestätigt**; Pausemenü-Sperre **abgelehnt**: `dec [esi+0x84]` in `FUN_000A55D0`/`FUN_000A62B0` ist ein Elementzähler in der Entfernen-Schleife eines Arrays (+0x78/+0x7C/+0x80); +0x84 = 30 schreibt nur das Frontend | timing60.md 4.1, 4.5, 4.10 |
| 5 | mittel | Nach dem Wiederherstellen aus „minimiert“ 30 Frames ohne Present (eingefrorenes Bild) | Code `PlanHysteresis::Accept`, Review-Trace | **bestätigt** | Wechsel zu einem Plan mit Presents gilt sofort; Unit-Test; Live-Lauf (4.4) |
| 6 | mittel | Timer-Pfade ohne Phasenbezug zum vblank: 1 % bis 37 % der Frames 1 oder 3 statt 2 Refreshes | Review-Läufe, eigene Läufe mit `pacing = timer` | **bestätigt** | Kadenzwächter (`CadenceGuard`) für Refresh = k·Ziel; Streuung in loader_design 8.4 dokumentiert. Ohne ganzzahliges Verhältnis (50 auf 120 Hz) bleibt es offen |
| 7 | niedrig | M3-3 bewertet weder Vordergrund noch vblank-Kopplung | `m3_check.py` | **bestätigt** | neues Kriterium M3b-7 |
| 8 | niedrig | „frames longer than 1.5 periods“ zählt Screenshot-Zeit und Arbeitsjitter mit | Review-Trace (4 Screenshot-Frames, 4 Arbeitsspitzen, 2 echte Fehltakte) | **bestätigt** | Zählung aus den Frame-Start-Abständen; Present-basierte Zahl in Klammern |
| 9 | niedrig | Bildschirmstatistik übersieht Sprünge über d ≥ 2 Presents; gemessener Refresh verzerrt (119,66–119,98 Hz) | Review-Traces, eigene Nachrechnung der Traces | **bestätigt** | Sprünge mit Kadenzabweichung gezählt; Refresh per Regression mit Segmentwechseln: 120,001–120,002 Hz |
| 10 | niedrig | `--set`-Werte ungetrimmt: `" timer"` besteht die Prüfung, wirkt aber nicht | Code `config.cpp`, `gfx_config.cpp` | **bestätigt** | `--set` wie INI-Werte (Trim + Anführungszeichen); GFX/warn, falls `ParseMode` scheitert; Unit-Test; Live-Lauf (4.4) |
| 11 | niedrig | Doku/Logs passen nicht zu den Überschreibungen (`frame_rate = 0`, `vsync = 0` mit `pacing = vsync`, veraltete Kommentare, Planzeile „Present(0)“ beim Minimieren) | Code und Review-Logs | **bestätigt** | INTERFACES 4.20, GFX/warn für wirkungslose Kombinationen, `config.cpp`-Kommentar, GFX_CONTRACT-Tabelle, `DescribePlan` „no Present“ |
| 12 | niedrig | Latenz Present → sichtbar bei vblank k = 2 etwa 3 Refreshes | Review-Messungen | **bestätigt** (bekannter offener Punkt) | Zahlen in loader_design 8.4 festgehalten; just-in-time-Framestart mit der Eingabe in M4 bewerten |
| 13 | niedrig | VRR-Ablehnung: (a) Messung ab dem ersten Frame, Ablehnung endgültig; (b) Vollbild-Swapchain bedeckt den Monitor nicht | Code, Review-Probe | (a) **bestätigt**; (b) **plausibel** (Vermutung, hier nicht prüfbar) | (a) 2 s Aufwärmzeit, eine Wiederholung nach 10 s; (b) offener Punkt mit Lösungsweg |
| 14 | niedrig | 59,94 Hz ergibt 59,94 fps und einen Doppelschritt alle 16,6 s | Unit-Test-Tabelle, Simulation des Reviews | **bestätigt** | dokumentiert (loader_design 8.4, R9); Entscheidung `tick_lock = display` gegen VRR nach einer Messung |

Code-Korrekturen: Befunde 5–11 und 13a. Dokumentiert ohne Code-Änderung: 1–4 (bewusste NTSC-Parität), 12, 13b, 14.

**Zusätzlich in Phase 5 gefunden und behoben:** Am 2026-10-06 lief das Fenster des Messrechners mit VRR. Die erste Fassung des Kadenzwächters (Befund 6) zählte die verlängerten Refreshes eines VRR-Panels als Fehltakte (1 statt 2 Refreshes), verschob das Raster 50-mal in 30 s und senkte `pacing = timer` auf 59,62 Hz – unter die Vorgabe. Seitdem gilt eine Dauer, die zeitlich eine Zielperiode sichtbar war, als pünktlich; Verschiebungen wechseln das Vorzeichen (keine Drift der mittleren Rate); nach 4 erfolglosen Verschiebungen hält der Wächter an. Ergebnis: 60,02–60,03 Hz, 0–1 Verschiebungen (Unit-Test „cadence guard - VRR panel, alternating shifts, stop after kMaxShifts“).

## 8. Bekannte Probleme und offene Punkte

- **Nicht getestet auf dem Messrechner:** andere Bildwiederholraten (60, 59,94, 144, 165 Hz), zweiter Monitor, Hotplug, Display aus, gesperrte Sitzung, VRR und Tearing im randlosen Vollbild. VRR im Fenster ist seit 2026-10-06 positiv geprüft (4.4). Die Logik ist kopflos getestet (`tests\pacer_tests.cpp`).
- **59,94 Hz:** 59,94 fps mit einem Doppelschritt alle 16,6 s; `tick_lock = display` oder ein VRR-Versuch sind nicht umgesetzt.
- **Vollbild und VRR:** Im randlosen Vollbild bleibt die Swapchain auf der 4:3-Ansicht; für Independent Flip/VRR muss sie vermutlich den ganzen Monitor bedecken (Pillarbox im Present-Pass).
- **Latenz:** vblank k = 2 im komponierten Fenster 21–25 ms Present → sichtbar (Phase 3 und Review), mit VRR/Independent Flip (Phase 5) 13–14 ms. Ein just-in-time-Framestart fehlt (M4).
- **Timer-Pfad ohne ganzzahliges Verhältnis** (50 Hz auf 120 Hz): keine echte Phasenbindung an den vblank. Für 50-Hz-Prüfungen irrelevant (`m3_check` bewertet dort Frame-Nummern, nicht Refreshes).
- **Tick-gebundene Stellen** (Abschnitt 6) laufen wie NTSC; Messung ab M4/M8.
- **Ladephasen sind frame-gebunden** (Ladebild 220 Frames): bei 60 Hz kürzer als bei 50 Hz, kein Fehler, aber Szenendauern mit Laden hängen von der Bildrate ab.
- **Laden kostet einzelne Vblanks** (Frame-Arbeit bis 20 ms in Debug, langsamstes 1-s-Fenster 58,9–59,3 Hz).
- **Start:** die ersten 30–31 Frames laufen „free“ (Fenster bis zum ersten Present unsichtbar, Hysterese 30 Frames).
- **Ältere Konfigurationen** unter `run\` (M2/M3-Agentenläufe) setzen noch `[render] frame_rate = 50` und loggen eine Warnung; gepflegt sind `run\m3b\*`, `run\m3final\*`, `run\m2fix\ctest*`. Die Belegprotokolle unter `run\m3final\*` stammen vom M3-Build; ein neuer Lauf dort überschreibt sie.
- `tests\platform_tests.cpp` enthält noch den manuellen Test „present loop at 50 Hz“ auf dem alten `PaceFrame(50)`.
- **Nichts committet:** Alle M3b-Änderungen liegen im Arbeitsbaum, auch die neuen Dateien (`xapi\game_timing.*`, `platform\pacer_*.*`, `tests\pacer_tests.cpp`, `tests\timing_tests.cpp`, `tools\timing_compare.py`, `analysis\loader\timing60.md`, `pacing60.md`, `scripts\timing_*`, dieser Bericht).

## 9. Ausblick

Reihenfolge nach [`docs/ROADMAP.md`](ROADMAP.md):

- **M3c – Launcher und Anzeige-Optionen:** Die neuen Schlüssel gehören in die Grafik- und Spiel-Tabs: `[render] pacing` (auto/vsync/timer/off), `[render] vsync`, `[game] tick_rate` (60 Standard, 50 PAL-Original). `[render] frame_rate` sollte dort nicht als freie Zahl erscheinen (ein Wert ≠ Tickrate erzeugt 0- und 2-Schritt-Frames).
- **M3d – Bildraten bis 120 Hz mit Interpolation** (im Design Abschnitt 6 ergänzt): Die Logik bleibt bei 60 Hz; der Renderer interpoliert Weltmatrizen und Kamera mit `α = Rest / dt` aus CApp+0xC8 zwischen den letzten beiden Ticks. Voraussetzungen: Frame-Clock und Takt mit der Render-Rate, Transform-Historie in `ni_extract`, Prüfung des pro Frame laufenden Codes auf doppelte Frame-Zahl. Ein höherer Tick ist kein Ersatz, weil er alle tick-gebundenen Stellen über das getestete NTSC-Verhalten hinaus beschleunigen würde.
- **M4 – Eingabe:** Damit werden die tick-gebundenen UI-Observablen messbar (Eingabesperre, Tastenwiederholung, Entprellung; timing60.md 8.13–8.15) und die Latenz bewertbar. Dazu gehört die Entscheidung über einen just-in-time-Framestart (pacing60.md 4.9).
- **M8 – Spielbares Level:** Dämpfung (8.17b), Walker (8.19), Skriptschleifen (8.16) und die Spielzeit-Uhr (8.18) an Ingame-Szenen messen; nur wenn die NTSC-Parität dort stört, den dokumentierten PAL-Ausgleich erwägen (timing60.md 12.2).
- **Takt:** Messung an 59,94/144/165-Hz-Anzeigen (T9/T12) und mit VRR im Vollbild (T10), Vollbild-Swapchain über den ganzen Monitor, danach `tick_lock = display` oder VRR für 59,94 Hz entscheiden.
