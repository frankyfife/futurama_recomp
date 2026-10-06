# M3d: Pro-Frame-Code, Frame-Clock und Takt bis 120 Hz (Analyse A)

Stand: 2026-10-06 · Ziel-XBE: `orig\retail_eu\default.xbe` (Basis 0x10000) · Status: **Analyse und Entwurf**, `src\` und
`launcher\` sind unverändert. Experimente unter `run\analyse_m3d_clock\` (git-ignoriert), Skripte
`analysis\loader\scripts\m3d_{perframe,clock_trace,clock_sim,pacing_trace,pacing_sim}.py` mit ihren `.out`-Dateien.
Bezug: [`docs/ROADMAP.md`](../../docs/ROADMAP.md) M3d · [`docs/m3b_60hz_report.md`](../../docs/m3b_60hz_report.md) ·
[`docs/m3c_report.md`](../../docs/m3c_report.md) (Befund 6) · [`docs/loader_design.md`](../../docs/loader_design.md) 1.9, 2.2,
8.4 (B31, B42, B43, B46) · [`startup.md`](startup.md) 3.1–3.4 · [`timing60.md`](timing60.md) · [`pacing60.md`](pacing60.md) ·
parallel entstanden: [`m3d_interpolation.md`](m3d_interpolation.md) (Analyse B, Renderer-Interpolation).

**Vorgabe des Nutzers:** mindestens 60 fps, höchstens 120 Hz. Die Spiellogik bleibt bei 60 Hz. Gerendert wird bis
120 fps mit Interpolation zwischen den beiden letzten Logikständen; an 60-Hz-Anzeigen bleibt es bei 60 fps ohne
Interpolation. Launcher-Option Bildrate: „60“, „120 (interpoliert)“, „Monitor (max. 120)“.

Unsichere Deutungen sind als **Vermutung** markiert. „Tick“ = fester Logikschritt (dt = 1/60 s), „Zwischenframe“ =
Frame ohne Tick, „Tick-Frame“ = Frame mit mindestens einem Tick.

---

## 0. Kurzfassung

| Thema | Ergebnis | Beleg |
|---|---|---|
| Pro Frame statt pro Tick | Vor der Schrittschleife laufen die Lade-/Streaming-Pumpen und die fps-Zählung, nach ihr Speicherpflege, `g_frameDelta`, `cur->Render()`, Profiler-Reset und das HUD-Textobjekt, in `CApp::Frame` außerdem Z-Clear, Film-Update, Pause-/Debug-Overlay und `DisplayFrame`. Eingabe (`CApp::vf10`) und Sound-Update laufen **pro Tick** in der Schleife. | 1.1, `m3d_perframe.out` 1 |
| Gemessen bei 120 fps (60-Hz-Logik) | Logik 59,997–60,006 Schritte/s; Legal 6,01 s, Tech-Notice 5,02 s, Frontend-Leerlauf bis Attract 60,15 s – unverändert. **Frame-gebunden und halb so lang:** Ladebild 3,667 → 1,862 s, Kontextwechsel-Schwarzbilder 0,167 → 0,083 s, Intro → Frontend 15,21 → 13,26 s. | 1.2, `run\analyse_m3d_clock\c60`, `c120ls` |
| **Fehler bei 120 fps (gemessen)** | Legal-Bild und Tech-Notice zeichnet das Spiel **im `Update`** (Zeitbild-`vf1` 0xEEC80 mit `NiCamera::Click`), nicht im `Render`. Zwischenframes bleiben leer (nur Z-Clear) und zeigen über den Xbox-Flip (B31) das Bild von vor zwei Presents. Bei strengem 1/0-Muster ist das immer derselbe alte Puffer: **die Tech-Notice flackert mit 60 Hz zwischen Bild und Schwarz** (Frames 900–915). 666–671 leere Frames je 85-s-Lauf. | 1.3 F1, `s120ls\pair_900_901.png` |
| Fehler bei 120 fps (statisch) | `Death::Render` 0x2BF10 addiert pro Bild die Update-Zeit → Todesbild 2× so schnell (2,5/3,0 s → 1,25/1,5 s). Verdeckungs-Ausblendung `FUN_000CD340` ändert Alpha pro Bild ohne dt → 2× so schnell. `FUN_00045EF0` ruft `rand()` pro Bild → Flackern doppelt, Zufallsfolge der Logik hängt von der Bildrate ab. 5 Schwarzbilder nach einer Intermission mit Skript-Läufen `FUN_000BA060(0)` pro Bild → halb so lang. | 1.3 F2–F5 |
| Unkritisch | Speicherpflege `FUN_000A46D0` (zählt Aufrufe: gemessen 1/Frame, Verschmelzen 2,92/s statt 1,45/s), `g_frameDelta` (alle 5 Leser im Render-Pfad, Halbtakt-Uhr liefert genau 8,333 ms), Zonen-Verwaltung `FUN_000C7030` (toter Zeitgeber), Profiler, HUD-Text, Film-Update (eigene XMV-Uhr), fps-Zähler (nur Debug-Overlay). | 1.3 |
| M3b-Lockstep bei 120 fps | Ergibt schon 1/0 Schritte, aber `g_frameDelta` springt 16,7/0 ms und der Rest bleibt konstant (alpha immer ≈ 0): **für Interpolation unbrauchbar.** | 2.1 |
| **Halbtakt-Uhr (gemessen)** | Prototyp im Experiment-Build: V wächst je Frame um ticks(dt)/2 = 83 333 (abwechselnd ⌊⌋/⌈⌉), Korrekturen in halben Ticks wie M3b. Ergebnis über 10 168 Frames: 0/1-Muster mit 7 Brüchen, **alpha genau zwei Werte im Abstand ½** (0,0386/0,5386), `g_frameDelta` 8,333 ms in 10 156 Frames, angezeigte Spielzeit +83 333 Ticks je Frame, nie rückwärts. Korrekturen nur bei echten Ereignissen. Mit Restausrichtung („Phase B“): alpha 0,499/0,999. | 2.3–2.4, `m3d_clock_trace.out` |
| alpha | Am Render-Zeitpunkt (Rücksprung 0xA2FBB) `rest = V − [CApp+0xC8]`, `step = trunc(QPF·dt)`; `alpha = clamp(1 − (L − rest)/step, 0, 1)` mit L = größter Rest des Musters. Mit Phase B ist L = step − μ und alpha ∈ {½, 1}. Eingabe → halbe Wirkung im selben Bild, volle Wirkung ein Bild (8,3 ms) später. | 2.5, 2.6 |
| Takt | 120 Hz: `vblank k=1` (gemessen 99,94–99,98 % genau 1 Refresh, auch mit „Maximum“). 240 Hz: k = 2. 144/165 Hz mit VRR: 120-Hz-Timer. Ohne VRR: „Monitor“ → gleichmäßig R/k (72 bzw. 82,5 fps), „120“ → Kadenz 1/2 Refreshes mit Uhr in Anzeigezeit. 60 Hz: 60 fps wie M3b. | 3 |
| Latenz (Befund 6) | Bildstart → sichtbar: **k = 2 (60 fps) 16,6 ms**, davon 15,7 ms Warten in der Warteschlange; **k = 1 (120 fps) 1,9 ms** (Median, das Waitable meldet hier 1,9 ms vor dem Vblank). Ein Just-in-time-Start bei k = 2 (Start ≈ 4 ms vor dem Ziel-Vblank) spart rund 12 ms. | 3.1, 3.5 |

**Empfehlung (Kurzform, Details in 4):** Halbtakt-Uhr als Verallgemeinerung der M3b-Lockstep-Uhr (Quant = Anzeigezeit
je Bild, M3b-Korrekturregel, Restausrichtung Phase B nur nach Kappung/Start), `FrameInterp` am Rücksprung 0xA2FBB,
vier kleine Korrekturen für Zwischenframes (F1 Leerbild wiederholen, F2 Todesbild, F3 Verdeckungs-Ausblendung,
F4 `rand` im Render), alle nur aktiv, wenn die Bildrate über der Tickrate liegt – bei 60 fps bleibt alles bitgleich.

---

## 1. Code, der pro Frame läuft

### 1.1 Aufbau eines Frames

Disassembly `CApp::Frame` 0xD2630 und `Game::Frame` 0xA2C20 (`m3d_perframe.py`, Abschnitt 1 der Ausgabe; Pseudocode
startup.md 3.1/3.2):

| Ort | Aufruf | Häufigkeit | Inhalt |
|---|---|---|---|
| `CApp::Frame` 0xD2644 | Renderer vf17 `ClearBuffer(NULL, [0x2D6740])` | Frame | Modus 4 = nur Z (`.data` 4; 5 nur aus `FUN_00012210` bei Kamera-Flag +0x1E0). **Die Farbe wird nicht gelöscht.** |
| 0xD264B | CApp vf8 0xA24A0 → `FUN_000EDE90` | Frame | Film: `XMVDecoder_GetNextFrame` (eigene QPC-Uhr, startup.md 3.3) und Zeichnen der aktuellen Filmtextur |
| 0xD2650 | `Game::Frame` | Frame | unten |
| 0xD2668 | `FUN_000E52D0` | Frame, wenn Pause+0xA | Pause-Overlay: `FUN_000E3310` zeichnet mit der Tick-Zeit +0x8C |
| 0xD266D | `FUN_000F2CF0` | Frame | Debug-Overlay („%dfps“, Koordinaten) |
| 0xD2678 | 0xA3210 | Frame | Stub `mov al,1; ret` |
| 0xD2685 | Renderer vf16 `DisplayFrame` | Frame | Host: Present, Takt, Statistik |
| `Game::Frame` 0xA2C54/59/60 | `FUN_000D2370`, `FUN_000F2050`, `FUN_000F2680` | Frame | Lade-Warteschlange, Streaming-Singleton, Streaming-Pumpe |
| 0xA2C6A | Timer `FUN_000D2B90` (Probe 0xA2C6F) | Frame | Akkumulator-Zeit |
| 0xA2CAB–0xA2CDC | – | Frame | fps-Zähler CApp+0xD0 (+1 je `Game::Frame`), Fensterstart +0xD8, Ergebnis +0xE0 |
| 0xA2DBD–0xA2DE0 | Stub, **CApp vf10 0xD25D0**, `FUN_0002C890`, `cur->Update()` | **Tick** | Eingabe (`XGetDeviceChanges`, Controller), Sound-Update mit `DirectSoundDoWork`, Pause, Tastenwiederholung (timing60.md 4.6/4.7) |
| 0xA2DF3–0xA2EE1 | `Exit`, `FUN_0002A660`, …, `Enter`, `Update` | Kontextwechsel (in der Schleife) | 8 × Clear + `DisplayFrame` (Schwarzbilder) |
| 0xA2F6A | – | Frame | `last` = jetzt − Rest → CApp+0xC8 |
| 0xA2F7F | `FUN_000A46D0` | Frame | Speicherpflege |
| 0xA2FA1/0xA2FB6 | Timer (Proben 0xA2FA6/0xA2FBB) | Frame | Render-Zeit → `g_frameDelta` [0x2E7B88] |
| 0xA3014 | `cur->Render()` (vf3) | Frame | Kontext zeichnen |
| 0xA3052 | `FUN_0002C9C0` | Frame | Profiler-Objekt [0x2DB79C]: drei Zähler +0x10CC..+0x10D4 = 0 |
| 0xA3071 | `FUN_000D7630` (wenn [0x2EBD0C]) | Frame | HUD-Textobjekt: Szenengraph aktualisieren, nur wenn als geändert markiert (+0x79), zeichnen, wenn Einträge (+0x120) |

Render-Methoden der Kontexte (VTable-Slot 3, aus den Konstruktoren gelesen): Intro 0x2B300, Frontend 0x21B60,
Attract 0xB0390 (leer: `ret`), Ingame 0x2A320 (→ `Session::Render` 0xC8E90), Death 0x2BF10, Intermission 0xD62E0.

`DisplayFrame`-Aufrufe **außerhalb** von `CApp::Frame` (nixboxrenderer_vtable.tsv Slot 16, nachgeprüft): Kontextwechsel
`FUN_0002A660` (8×), `Game::Init` 0xA26A0 (2×), Levelende `FUN_000A62B0` (3×), `Session::Render` beim Ende des
Übergangsobjekts +0x120 (2×), Filmende `FUN_000EE080` (1×). Alle zeichnen nur Löschbilder. Render-to-Texture
(`FUN_0003B960` → `FUN_000FEEC0`) präsentiert nicht (GFX_CONTRACT: Slot 16 nur für den Backbuffer).

### 1.2 Messung bei 2 Frames pro Tick

Läufe mit dem Build `build\m3c-final-rel` (RelWithDebInfo, 640×480, 85 s, Fenster) und dem Experiment-Build (2.3),
Auswertung `tools\timing_compare.py`, `m3d_clock_trace.py`, `m3d_pacing_trace.py`. 120 fps = `[render] frame_rate =
120` (Entwicklerschlüssel), Plan `vblank k=1`.

| Lauf | Uhr | fps | Schritte/s | Schritte je Frame 0/1/2/3+ | Frames ohne Draw und ohne Farb-Clear |
|---|---|---|---|---|---|
| `c60` | M3b-Lockstep | 60,01 | 60,010 (stetig) | 0 / 5059 / 5 / 3 | 4 |
| `c120ls` | M3b-Lockstep | 119,97 | 60,003 | 5077 / 5087 / 0 / 3 | **669** |
| `c120rt` | Echtzeit (QPC) | 119,96 | 60,006 | 5076 / 5085 / 1 / 3 | **671** |
| `e120h` | Halbtakt (Prototyp) | 119,96 | 59,997 | 5079 / 5085 / 1 / 3 | 666 + 5 (Intro) |
| `e120ha` | Halbtakt + Phase B | 119,96 | 59,999 | 5080 / 5084 / 1 / 3 | 666 + 5 (Intro) |

Dauern (`timing_compare.py --run c60 --run c120ls`):

| Observable (timing60.md 8) | 60 fps | 120 fps | Mechanik |
|---|---|---|---|
| Legal-Bild (Logik) | 6,007 s | 6,013 s | QPC 6,0 s – gleich |
| Logo-Filme (XMV-Stub) | 0,117 s (7 Frames) | 0,104 s (12 Frames) | frame-gebunden |
| Tech-Notice (Logik) | 5,050 s | 5,018 s | QPC max. 5,0 s – gleich |
| Ladebild `Loadmenu.nif` | 3,667 s (220 Frames) | **1,862 s** (223 Frames) | frame-gebunden (B46): ein Zustandsschritt der Lade-Warteschlange je Frame |
| Intro-Film-Stub + Wechsel | 0,183 s | 0,104 s | frame-gebunden |
| Intro → Frontend | 15,214 s (#891) | **13,260 s** (#1558) | Summe |
| Frontend-Leerlauf → Attract | 60,200 s | 60,150 s | dt-Sekunden – gleich (60 fps verliert 33 ms durch die 6-dt-Kappung nach dem Wechsel) |
| Attract → Frontend | 0,167 s | **0,083 s** | 8 Schwarzbilder + 2 – frame-gebunden |

### 1.3 Befunde je Teil

„Wirkung“ = was sich bei 2 Frames pro Tick gegenüber 60 fps ändert. F1–F5 sind Korrekturvorschläge (4.2).

| # | Teil | Was es pro Aufruf tut (Beleg) | Wirkung bei 120 fps | Bewertung |
|---|---|---|---|---|
| 1 | **Intro: Legal, Tech-Notice** (Zustände 0/1 und 5 der Sprungtabelle 0x2BA2C) | `Intro::Update` 0x2B5F0 ruft `FUN_0002B360` → Zeitbild-`vf1` 0xEEC80: `NiCamera::Click` (zeichnet) **und** `+8 += t`. `Intro::Render` zeichnet nur in Zustand 7 (Ladebild). | Zwischenframes ohne Draw; nur Z gelöscht → Bild von vor zwei Presents (B31). Beim festen 1/0-Muster bekommt einer der beiden Puffer nie ein neues Bild. **Gemessen** (`s120ls`): Frames 300–315 zeigen jedes zweite Mal „the picture of frame 191“ (zufällig auch Legal), Frames 900–915 jedes zweite Mal „the picture of frame 732 (0 draw calls)“ = **schwarz**: Tech-Notice flackert mit 60 Hz. Trace: 666 leere Zwischenframes im Intro, Frontend 0. | **Fehler** → F1 |
| 2 | Extras-Bildbetrachter (Menüseite; **Vermutung:** VTable ab 0x293EA4, Slot 1 = Lücke 0x1D880) | dieselbe Zeitbild-`vf1` mit dt (0x1D8DF), Slot 1 = Seiten-`Update` (Aufruf `[+0x20]->vf1` in `Frontend::Update` 0x22E09) | wie 1, falls das Bild nur im `Update` gezeichnet wird (Vermutung; ohne Eingabe nicht erreichbar, M4) | Risiko → F1 bzw. 4.2 |
| 3 | **Death::Render** 0x2BF10 | `[+0x3C]->vf1([+0x20])`; +0x20 = QPC-Sekunden seit dem letzten `Death::Update` (0x2C1C0–0x2C1EB), vf1 0xEEC80 addiert sie (`+8 += t`) und beendet nach 2,5/3,0 s (0x2C247) | jede Update-Zeit wird zweimal addiert: **Todesbild 1,25/1,5 s statt 2,5/3,0 s** (statisch; Death ist ohne Spiel nicht erreichbar) | **Fehler** → F2 |
| 4 | **Verdeckungs-Ausblendung** `FUN_000CD340` (aus `FUN_000CE2B0`, nur aus `Session::Render` 0xC9232) | Objekt zwischen Kamera und Spieler: Alpha −= 2·Tempo, sonst += 2·Tempo, geklemmt; Materialalpha und Revision +0x54 setzen. Kein dt. | Wände blenden doppelt so schnell aus und ein | Abweichung → F3 |
| 5 | **Zufalls-Flackern** `FUN_00045EF0` (aus `Session::Render` 0xC92F1) | `+0x24 = rand() & 1`, `+0xA8 = 1` | doppelte Flackerrate; `rand()` ist die globale LCG der CRT (0x1668CC, Zustand im Per-Thread-Block +0x14): die Zufallsfolge der Logik hängt von der Bildrate ab (Replay M9) | Abweichung → F4 |
| 6 | Nach einer Intermission ([0x2EA99C] = 5) | `Session::Render` zeichnet 5 Frames nur die Hintergrundfarbe und ruft `FUN_000BA060(0)` (Skript-Scheduler mit t = 0) | 5 Frames = 42 statt 83 ms; die 5 Zusatzläufe der Skripte drängen sich in 2,5 statt 5 Ticks | gering → F5 (optional) |
| 7 | Lade-Warteschlange `FUN_000D2370` | je Aufruf genau ein Zustandsschritt einer Anforderung (öffnen → pollen → fertig + Fortschritts-Callback) | Laden in der halben Zeit (gemessen 1,862 s) – auch mit `pacing = off` funktionierte das (B46) | gewollt/akzeptiert, dokumentieren |
| 8 | Streaming `FUN_000F2050`/`FUN_000F2680` | Singleton anlegen; je Aufruf höchstens eine neue Datei und `File::Poll` (Fiber) | doppelt so viele Polls | unkritisch |
| 9 | Speicherpflege `FUN_000A46D0` | Zähler [0x2E8E48]/[0x2E8E44]; alle 41 bzw. 201 Aufrufe Verschmelzen freier Blöcke (`FUN_000A3CE0`/`FUN_000A3B10`) | gemessen +1 je Frame; 2,92/s und 0,59/s statt 1,45/s und 0,29/s | unkritisch (nur früheres Aufräumen) |
| 10 | `g_frameDelta` [0x2E7B88] | Leser: `Frontend::Render` (+0x1C8 vf1), `FUN_00045F10` (+0x1C += Δ), `FUN_000C0AC0` (Licht-Einblendung, Objekt [0x2EA784] Slot 3, aus `Session::Render`), `Session::Render` (+0x11C vf1, ≤ 0,1), `FUN_000EEF80` (Blende Session+0x120 Slot 2, ≤ 0,05) – **alle im Render-Pfad** | Halbtakt-Uhr: gemessen 8,333 ms in 10 156 von 10 168 Frames → bildgetriebene Teile laufen glatt mit 120 Hz. M3b-Lockstep bei 120 fps: 16,7/0 ms abwechselnd | richtig mit Halbtakt-Uhr |
| 11 | Zonen-Verwaltung `FUN_000C7030` (`Session::Render` in Zuständen ≠ 2, `Update` in Zustand 2) | wählt den Raum der Kamera, entlädt andere; zählt Session+0x6C um dt herunter | +0x6C setzt nur der ctor `FUN_000C3820` (0) → Zähler tot; Rest zustandsbasiert, idempotent | unkritisch |
| 12 | `Frontend::Render` 0x21B60 | `NiAVObject::Update` mit der Tick-Zeit +0x78 (timing60.md 4.1), Kamera-`Click` | Zwischenframes sind ohne Interpolation bitgleich zum Vorgänger (gemessen: 8 Paare, 0 geänderte Pixel) | erwartet → Interpolation (Analyse B) |
| 13 | Pause-Overlay `FUN_000A8290` | `Update` mit Tick-Zeiten, Zeichnen, `+4 += 1` je Aufruf | Leser von +4 nicht gefunden | unkritisch |
| 14 | Profiler `FUN_0002C9C0`, Stubs 0xA3210/0xB0390/0xAED30 | Zähler löschen bzw. nichts | – | unkritisch |
| 15 | HUD-Text `FUN_000D7630` | Update nur bei Änderungsmarke, Zeichnen | idempotent | unkritisch |
| 16 | Film CApp vf8 | `GetNextFrame` + Zeichnen der aktuellen Textur; Untertitel aus der Filmzeit | idempotent, solange der Decoder nach Zeit (pts) liefert | Auflage für M6 |
| 17 | fps-Zähler CApp+0xD0..0xE0 | +1 je `Game::Frame` | zeigt 120; nur im Debug-Overlay | unkritisch |
| 18 | Eingabe, Sound | CApp vf10 in der Schleife (0xA2DCA) | weiter 60 Abtastungen/s | unverändert |
| 19 | Schwarzbilder außerhalb von `Game::Frame` (1.1) | Löschbild + `DisplayFrame`, je ein Vblank | Kontextwechsel 67 statt 133 ms; nach dem Wechsel holt die Uhr 4 statt 6 Ticks nach, die Kappung greift bei 120 fps nicht | akzeptiert |
| 20 | Entitäten-Render (`Char:Render` vf3 aus `FUN_000CE2B0`, `FUN_000975E0`-Liste, `FUN_000E5870`-Liste, +0x94 vf3) | virtuell, statisch nicht aufgelöst | unbekannt | Restrisiko → Doppel-Render-Test (4.4) |

Host-Seite, ebenfalls pro `DisplayFrame` gezählt: `[debug] frames`, `screenshot_frames`, `trace_frames`, die
Plan-Hysterese (30 Frames), `kForcePresentAfter` (30), `DisplayLimitDetector` (3 von 8), `CadenceGuard` (60/30),
`VrrDetector` (60/240 Frames, Aufwärmzeit in Sekunden). Bei 120 fps halbieren sich diese Zeitfenster; Prüfskripte
müssen Frames aus Sekunden × Bildrate rechnen (4.5).

---

## 2. Frame-Clock

### 2.1 Die M3b-Lockstep-Uhr

`probe:Game::Timer` (`xapi\game_timing.cpp`, `OnGameFrameTimerSample`) ersetzt nur die drei Zeitproben von
`Game::Frame` (Rücksprünge 0xA2C6F, 0xA2FA6, 0xA2FBB). An 0xA2C6F rechnet `LockstepClock::Advance(real, step)`:
`V += n·step`, n = 1 normal; liegt `real − V` mehr als ¾ Schritt jenseits, holt n auf bzw. hält (n = 0). Die beiden
Render-Proben bekommen `V` – daher ist `g_frameDelta` immer ein Vielfaches von dt. Gemessen bei 60 fps: Phase
`real − V` −10,7…+10,9 ms (`c60`), Rest nach einer Kappung 4 Ticks, beim Start 3778 Ticks (`e60`).

Bei `frame_rate = 120` hält diese Uhr jedes zweite Mal (Hold, weil `real − V` unter −¾ Schritt fällt): 5077 Halte,
5088 Einzelschritte (`c120ls`). Das ergibt zwar das 1/0-Muster, aber der Gast sieht V nur in ganzen Schritten:
der Rest ist in jedem Frame gleich (alpha konstant ≈ 0) und `g_frameDelta` ist abwechselnd 16,7 und 0 ms. Für
Interpolation braucht es eine Uhr in halben Schritten.

### 2.2 Gast-Arithmetik

`m3d_clock_sim.py` Abschnitt 1 (exakte Rechnung mit 53- und 64-Bit-x87): ein Schritt braucht `sec(elapsed) ≥ dt`,
abgezogen wird `ticks(dt) = trunc(QPF·dt)`.

| QPF | ticks(dt) | kleinster Rest für einen Schritt | Hälften q_a/q_b | Rest nach Kappung |
|---|---|---|---|---|
| 10 000 000 (hier) | 166 666 | 166 667 | 83 333 / 83 333 | 4 |
| 3 579 545 | 59 659 | 59 660 | 29 829 / 29 830 | 0 |
| 24 000 000 | 400 000 | 400 001 | 200 000 / 200 000 | 0 |
| 733 333 333 (Xbox) | 12 222 222 | 12 222 224 | 6 111 111 / 6 111 111 | 5 |

Folgen: Nach einem Tick ist der Rest ≥ 1 (bei 10 MHz). Der Rest liegt immer in [0, ticks(dt)], alpha = Rest/ticks(dt)
ist also ≤ 1. Die Kappung (`sec(elapsed) > 6·dt`, 0xA2CE8–0xA2D1F) setzt den Rest unabhängig von V auf wenige Ticks.

### 2.3 Entwurf der Halbtakt-Uhr

Größen: `step = ticks(dt)`; Bildrate F, Tickrate T = 60, N = F/T Bilder je Tick. Ist F eine ganzzahlige Vielfache
von T (120 = 2·60) und die Anzeige innerhalb 100 ppm davon (120,000/120,002 Hz gemessen), läuft die Uhr
**anzeigegekoppelt** mit Quanten `q_i` = abwechselnd ⌊step/N⌋ und den Rest (Summe über N Bilder = step). Sonst
(75, 90, 100, 119,88 Hz, Kadenz) ist das Quant die Anzeigezeit des Bildes in QPC-Ticks mit Bruchrest-Akkumulator
(2.7). Pseudocode für die Akkumulator-Probe (Rücksprung 0xA2C6F):

```
Advance(real, lastPrev):                      // lastPrev = [CApp+0xC8] vor diesem Frame
  if first: V = real; return V                // keine Ausrichtung im ersten Frame (sonst Rest < 0)
  V += nextQuantum()                          // Normalfall: ein Quant
  d = real - V; m = 3/4 * q
  if d >  m: V += quanta for the smallest n with d - n*q <= m      // Aufholen (M3b-Regel, in Quanten)
  if d < -m: V -= one quantum (Halt, V nie hinter dem Vorframe)
  if alignNeeded:                             // nur im 2. Frame, nach einer Kappung oder einem Uhr-Reset
     el = V - lastPrev
     delta = ((phi - el) mod q) in (-q/2, q/2];  phi = q - mu (Phase B), mu = max(16, step/1024)
     V += the candidate delta + k*q (k in {-1,0,1}) closest to real, not below the previous V
  return V
Render-Proben (0xA2FA6/0xA2FBB): V           // g_frameDelta = ein Quant = 8,333 ms
```

Warum das 1/0-Muster stabil ist: zwei Quanten ergeben genau `step`, ein Tick zieht `step` ab. Der Rest r (mod q)
ändert sich deshalb nur, wenn der Gast kappt; Aufholen um beliebig viele Quanten lässt ihn modulo q unverändert.
Im Tick-Frame bleibt r, im Zwischenframe r + q (≤ step, kein Schritt, weil die Schwelle step + 1 ist).

**Ausrichtung (Phase):** r ist sonst zufällig (`e120h`: r = 6427 seit dem Start, keine Kappung bei 120 fps). Phase B
(`r = q − μ`) macht r konstant und alpha = Rest/step selbst schon zum latenzoptimalen Wert (2.6); Phase A
(`r = μ`) ergibt alpha ≈ 0/½. Eine Verschiebung beträgt höchstens ±q/2 und kommt nur nach Ereignissen vor, die
ohnehin eine Unstetigkeit haben (gemessen: genau eine, im ersten Frame, −4107 Ticks).

**Eingangsgröße real:** die Probe des Gasts wie in M3b. Gemessen liegt sie Median 7 µs, p99 0,09 ms, p99,9 1,0 ms
nach dem Eintritt in `CApp::Frame` (`e120h`); vblank-genauer wäre die Zeit aus der Frame-Statistik, das ist nach den
Messungen aber nicht nötig (2.4).

### 2.4 Messung des Prototyps

Experiment-Build: eine **Kopie** von `src\` (`git archive HEAD`) unter `run\analyse_m3d_clock\src_exp`, gepatcht mit
`run\analyse_m3d_clock\patch_exp.py` (Halbtakt-Uhr in `OnGameFrameTimerSample`, Ausrichtung, CSV je `CApp::Frame`
mit V, real, CApp+0xC8 vor/nach, Rest, Schritten, `g_frameDelta`, Pflegezählern, [0x2EA99C], Kontext, Draws),
gebaut nach `run\analyse_m3d_clock\build_exp` (RelWithDebInfo), gesteuert über `FUT_M3D_CLOCK=half`,
`FUT_M3D_ALIGN=1`, `FUT_M3D_TRACE=<csv>`. Auswertung `m3d_clock_trace.py` (`m3d_clock_trace.out`):

| Größe | `e60` (M3b, 60 fps) | `e120h` (Halbtakt) | `e120ha` (Halbtakt + Phase B) |
|---|---|---|---|
| Frames | 5068 | 10 168 | 10 168 |
| Schritte je Frame 0/1/2/3+ | 2 / 5058 / 5 / 3 | 5079 / 5085 / 1 / 3 | 5080 / 5084 / 1 / 3 |
| Brüche des Musters (gleiche Schrittzahl in Folge) | – | 7 | 9 |
| alpha (Rest/step) | 0,000024 (4179) / 0,0227 (889) | **0,038562 (Tick-Frames) / 0,538562 (Zwischenframes)** | **0,499028 / 0,999028** |
| Rest mod q | 4, 3778 | 6427 in allen 10 168 Frames | 83 171 = q − 162 in allen außer Frame 1 |
| `g_frameDelta` | 16,667 ms (5058) | 8,333 ms (10 156) | 8,333 ms (10 155) |
| angezeigte Spielzeit je Frame | +166 666 (5058) | **+83 333 (10 156), nie rückwärts** | +83 333 (10 154), nie rückwärts |
| Phase `real − V` | −0,84…+9,04 ms | −4,29…+5,02 ms | −6,05…+6,00 ms |
| Korrekturen | 8 Aufholen, 1 Halt | 10 Aufholen (max. 10 Quanten), 1 Halt | 11 Aufholen, 1 Halt, 1 Ausrichtung |

Alle Korrekturen in `e120h`/`e120ha` fallen auf echte Ereignisse: Planwechsel `free → vblank` beim Sichtbarwerden des
Fensters (Frames 37–39), `DisplayFrame` ohne `Game::Frame` beim Filmende (725/728, 1495), Kontextwechsel (1560,
8770/8771), ein Frame, der 3,5 ms nach seinem Vorgänger startete (Halt, 8989). Sprünge der Phase um bis zu 4,7 ms
ohne Korrektur kommen vor (Frame 190: der Startzeitpunkt im Refresh wechselt), deshalb bleibt der Rand ¾ Quant.
`m3d_clock_sim.py` Abschnitt 2 bestätigt das Verhalten mit Ereignissen (Vblank verpasst, Kontextwechsel, Hänger):
kein Rückwärtsschritt der angezeigten Zeit in allen Varianten; ein „Zentrieren“ nach Korrekturen bringt nichts.

### 2.5 alpha am Render-Zeitpunkt

Veröffentlichung in der Probe am Rücksprung **0xA2FBB** (nach `last` 0xA2F6A, vor `cur->Render()` 0xA3014): alle
Draws von `Render` und den Overlays danach sehen den Wert dieses Frames.

```
rest  = V_frame - [CApp+0xC8]          // V_frame = Wert der Akkumulator-Probe dieses Frames
step  = trunc([0x2EB448] * dt[0x2D6748])
L     = Phase B: step - mu (konstant) | sonst: groesster Rest des laufenden Musters (nach Kappung neu)
alpha = clamp(1 - (L - rest) / step, 0, 1)     // Phase B: Tick-Frame 1/2, Zwischenframe 1
```

Angezeigt wird `S[G−1] + alpha·(S[G] − S[G−1])`; die angezeigte Zeit `G − 1 + alpha` steigt je Frame um genau ½
(Messung 2.4). Diese Konvention stimmt mit `m3d_interpolation.md` 3.1/6.1 überein (`FrameInterp`, alpha = Gewicht
von S[G]).

| Fall | Erkennung | alpha / Merker |
|---|---|---|
| Normal (1 Tick oder 0 Ticks) | Schritte des Frames ≤ 1 | wie oben |
| Aufholen (n ≥ 2 Ticks) | Zähler `probe:Game::FixedStep` seit der letzten Render-Probe ≥ 2 (gemessen bei Wechseln 4–5) | `snapAll` (alpha 1). Verfeinerung: Gewicht über n Ticks `w = 1 − (L − rest)/(n·step)`, das vermeidet ein stehendes Bild danach (Analyse B rastet über die Generationslücke ein) |
| Kappung (> 6 dt) | `V_frame − lastPrev > ticks(6·dt)` | `snapAll`, Ausrichtung im nächsten Frame |
| Kontextwechsel | `probe:Game::ContextSwitch` (0xA2E45) in diesem Frame | `snapAll` |
| Erster Frame, Uhr-Reset, Planwechsel mit anderem Quant | Uhrzustand | `snapAll` |
| Frames ohne `Game::Frame` (1.1) | keine Render-Probe | keine Interpolation, Historie nicht weiterschalten (sie zeichnen nur Löschbilder) |
| Draws im `Update` (Intro, Extras) | Phase „Schleife“ zwischen Akkumulator- und Render-Probe | alpha 1 (`kFrameInterpInsideStep`) |
| 100-Schritte-Zähler erschöpft | Rest ≥ step | praktisch unmöglich (Kappung ≤ 6 Schritte); clamp auf 1 |
| N = 1 (60 fps) | Bildrate = Tickrate | alpha = 1, keine Interpolation, Uhr = M3b unverändert |

Vorschlag für die Schnittstelle (ergänzt `m3d_interpolation.md` 6.1): `gfx::FrameInterp { std::uint64_t tick;
std::uint32_t stepsThisFrame; std::uint32_t pairTicks; float alpha; std::uint32_t flags; }` mit `pairTicks` = Ticks
zwischen den beiden Historienständen und den Merkern `SnapAll`, `InsideStep`, `ClockReset`.

### 2.6 Phase und Latenz

Mit L = größter Rest gilt unabhängig von r: Der Tick g wird in dem Frame ausgeführt (und die Eingabe dafür
abgetastet), in dem alpha = ½ ist – die Hälfte seiner Wirkung ist im selben Bild sichtbar, die volle ein Bild
(8,3 ms) später. Nimmt man stattdessen alpha = Rest/step mit beliebiger Phase (Konvention „0 im Tick-Frame“,
Phase A), sieht man im Tick-Frame noch S[G−1], die volle Wirkung erst nach 16,7 ms. Phase B macht beides gleich:
alpha = Rest/step ≈ 1 − (L − Rest)/step (Abweichung μ/step = 0,001). Verglichen mit 60 fps (Wirkung im selben Bild,
aber Bildstart → sichtbar 16,6 ms, 3.1) ist 120 fps mit Phase B insgesamt schneller: 1,9 ms bis zur halben,
10,2 ms bis zur vollen Wirkung.

### 2.7 Andere Verhältnisse

- **119,88 Hz:** Quant = 1/119,88 s statt step/2: die Spielzeit bleibt exakt Echtzeit, alpha wandert langsam (das
  1/0-Muster verschiebt sich alle ≈ 1000 Bilder um einen Frame), die angezeigte Zeit bleibt stetig – kein
  Doppelschritt wie bei M3b (alle 16,6 s), keine Verlangsamung wie `tick_lock = display`.
- **59,94 Hz** mit „Monitor“: dasselbe mit N < 1 – die Interpolation beseitigt den M3b-Doppelschritt. Bei genau 60 Hz
  bleibt N = 1 ohne Interpolation (Vorgabe).
- **75/90/100 Hz**, Kadenz bei 144/165 Hz ohne VRR: Quant = Anzeigezeit des Bildes (`s_n/R`), alpha beliebig in [0, 1].
- Phase B lohnt nur bei N = 2 anzeigegekoppelt; sonst alpha ohne Ausrichtung.

---

## 3. Takt bis 120 Hz

### 3.1 Messungen am 120-Hz-Messrechner (LG-TV, VRR im Fenster aktiv)

`m3d_pacing_trace.py`: Present-Id je Frame mit dem ersten Statistik-Eintrag verbunden, der sie als sichtbar meldet.

| Lauf | Plan | Bildstart → sichtbar (Median / p99) | Present → sichtbar | Arbeit Median / p99 | Startphase im Refresh | Auf dem Bildschirm |
|---|---|---|---|---|---|---|
| `c60`/`e60` (60 fps) | vblank k = 2 | **16,62 / 16,65 ms** | 15,7 ms | 0,85–0,88 / 1,4–2,1 ms | 0,005 (direkt nach dem Vblank des Vorgängers) | 99,88 % genau 2 Refreshes |
| `c120rt`, `e120h` (120 fps) | vblank k = 1 | **1,87 / 6,2 ms** | 1,0 ms | 0,84 / 1,35 ms | 0,776 (1,9 ms vor dem Vblank) | 99,98 % genau 1 Refresh |
| `q120max` (120 fps, „Maximum“: 5262×3946, MSAA 8×, linear) | vblank k = 1 | 2,00 / 6,2 ms | 1,2 ms | 0,78 / 1,26 ms; GPU 1,24 / 2,8 ms | 0,760 | 99,94 % genau 1 Refresh |
| `q60max` (60 fps, „Maximum“) | vblank k = 2 | 16,62 ms | 15,8 ms | 0,82 / 1,35 ms; GPU 1,15 / 2,6 ms | 0,005 | 99,83 % |

Bei k = 2 startet das Bild sofort nach dem Vblank, an dem der Vorgänger sichtbar wurde, und wartet dann zwei
Refreshes in der Warteschlange (Present(2)). Bei k = 1 meldet das Waitable erst etwa 1,9 ms vor dem nächsten Vblank
(**Vermutung:** Independent Flip/VRR gibt den Latenzplatz spät frei; im zusammengesetzten Fenster wäre es ein
voller Refresh, pacing60.md 2.4). 120 fps tragen hier auch „Maximum“ problemlos.

### 3.2 Bedeutung der Option

| Anzeige | „60“ | „120 (interpoliert)“ | „Monitor (max. 120)“ |
|---|---|---|---|
| 60 Hz | vblank k=1, 60 fps | 60 fps (mehr geht nicht), N = 1, ohne Interpolation | wie links |
| 59,94 Hz | M3b (59,94 fps, Doppelschritt alle 16,6 s) | 59,94 fps interpoliert (2.7) | wie links |
| 75 / 100 Hz | M3b `display-rate` (60 Ticks, Bilder wiederholen sich: Halten 13,3/26,7 bzw. 10/20 ms) | 75 / 100 fps (k = 1), interpoliert | wie links |
| 120 Hz | vblank k = 2 | vblank k = 1, 120 fps | wie links |
| 119,88 Hz | k = 2, 59,94 fps | k = 1, 119,88 fps interpoliert, Quant aus der Anzeigezeit | wie links |
| 144 / 165 Hz mit VRR | Timer 60 Hz (M3b) | **Timer 120 Hz** + `Present(0, ALLOW_TEARING)`, Panel folgt | wie links |
| 144 / 165 Hz ohne VRR | M3b `display-rate` (Halten 13,9/20,8 ms, Positionsfehler 5,6 ms) | Kadenz 1,1,1,1,2 bzw. 1,1,2,… (Present(1)), Uhr in Anzeigezeit: 120 fps, Halten 6,9/13,9 ms, kein Positionsfehler | **vblank k = 2: 72 bzw. 82,5 fps gleichmäßig** |
| 180 Hz | k = 3 | VRR: Timer 120; sonst Kadenz 2,1,… (Halten 5,6/11,1 ms) | k = 2: 90 fps |
| 240 Hz | k = 4 | k = 2, 120 fps | k = 2, 120 fps |
| Hintergrund, minimiert | `free` mit 60 Hz | `free` mit der Bildrate (einfach) oder 60 Hz (spart Strom; Uhr-Reset beim Wechsel) | wie links |

Bewertung des Ruckelns (`m3d_pacing_sim.py`, 2 s je Fall; „Halten“ = Dauer eines Bildes auf dem Schirm,
„Positionsfehler“ = Spitze-Spitze von Inhaltszeit minus Anzeigebeginn):

| Anzeige | Variante | fps | Halten min/max | Positionsfehler |
|---|---|---|---|---|
| 144 | Kadenz 120, festes Quant 1/120 s | 120 | 6,94 / 13,89 ms | **5,56 ms** |
| 144 | Kadenz 120, Quant = Anzeigezeit | 120 | 6,94 / 13,89 ms | 0 |
| 144 | vblank k = 2 | 72 | 13,89 / 13,89 ms | 0 |
| 144/165 | VRR-Timer 120 | 120 | 8,33 / 8,33 ms | 0 |
| 165 | Kadenz 120, festes Quant | 120 | 6,06 / 12,12 ms | 5,30 ms |
| 180 | Kadenz 120, festes Quant | 120 | 5,56 / 11,11 ms | 2,78 ms |
| 144 | M3b heute (60 Ticks, Present(1) je Refresh) | 60 | 13,89 / 20,83 ms | 5,56 ms |

Eine Kadenz mit festem Quant ruckelt sichtbar (5–6 ms Positionsfehler); mit Quant = Anzeigezeit stimmen die Positionen,
es bleibt ungleiches Halten alle 5 Bilder (28,8 Hz bei 144 Hz). Daher: „Monitor“ wählt bei festen 144/165 Hz die
gleichmäßige Rate R/k ≤ 120, „120“ die Kadenz in Anzeigezeit.

### 3.3 Planauswahl

`pacing::SelectPlan` bekommt statt der Tickrate die **Bildrate F** als `targetHz` (Formeln aus 3.2). F ≤ T → N = 1,
Uhr = M3b, alpha = 1. Neu ist nur der Kadenz-Plan (pacing60.md 4.3: `s_n = ⌊(n+1)·R/F + ½⌋ − ⌊n·R/F + ½⌋`, Present(1),
Bild = s_n Refreshes); die übrigen Pläne gibt es seit M3b. Je Plan das Quant der Uhr:

| Plan | Quant je Bild |
|---|---|
| vblank k, R ≈ k·F·(1 ± 100 ppm), F = N·T | step/N (abwechselnd ⌊⌋/⌈⌉), Phase B |
| vblank k sonst (75, 100, 119,88 …) | QPC-Ticks von k/R (rational aus QDC), Bruchrest-Akkumulator |
| Kadenz | QPC-Ticks von s_n/R |
| tearing-timer, timer, free | Periode des absoluten Zeitplans (QPF/F) bzw. step/N bei F = N·T |

### 3.4 Wechselwirkungen

- Bei 120 Hz ist die Arbeit je Bild unverändert (0,75–0,85 ms RelWithDebInfo), die CPU-Last pro Sekunde verdoppelt
  sich (≈ 9–10 % eines Kerns); GPU auch mit „Maximum“ p99 2,8 ms.
- Die Phase der Uhr hängt an der Startzeit des Bildes. Ändert ein Just-in-time-Start (3.5) den Startpunkt, verschiebt
  sich die Phase einmal; die Uhr korrigiert erst jenseits von ¾ Quant (6,25 ms bei 120 fps).
- VRR im randlosen Vollbild setzt vermutlich eine Swapchain über den ganzen Monitor voraus (M3b-Befund 13b,
  M3c 10): Ohne sie greift bei 144/165 Hz der Kadenz- bzw. R/k-Plan statt des VRR-Timers.

### 3.5 Latenz (M3c-Befund 6)

Gemessen ist die Latenz bei k = 2 stabil (16,62 ms, p99 16,65 ms), aber unnötig hoch: 15,7 ms davon wartet das fertige
Bild. **Just-in-time-Start:** Nach dem Signal des Waitables bis `V_ziel − B` schlafen (absoluter Hochauflösungs-Timer),
`V_ziel` = SyncQPCTime des letzten sichtbaren Bildes + Refreshes bis zum Ziel-Vblank × Periode (Frame-Statistik),
`B = max(4 ms, p99(Arbeit) + p99(GPU) + 1,5 ms)` über die letzten 2 s; nach einem verpassten Ziel B für 2 s
verdoppeln; aus bei ungültiger Statistik, im Hintergrund und in Ladephasen (Arbeit > Periode). Erwartung bei 60 fps:
Bildstart → sichtbar ≈ 4–5 ms statt 16,6 ms. Bei k = 1 liegt der Start hier schon 1,9 ms vor dem Vblank; der
Mechanismus greift dort nur, wenn die Messung eine größere Wartezeit zeigt (zusammengesetztes Fenster).
Die Frame-Clock bleibt davon unberührt (virtuelle Zeit), die Eingabe wird später abgetastet – das ist der Gewinn.

---

## 4. Empfehlungen

### 4.1 Frame-Clock

1. `LockstepClock` zu einer Uhr in Quanten verallgemeinern (2.3); bei N = 1 exakt das M3b-Verhalten (Unit-Test:
   identische Ausgaben für 60 fps).
2. Phase B: Rest ≡ q − μ (mod q), Ausrichtung nur im zweiten Frame, nach einer Kappung, nach einem Planwechsel mit
   neuem Quant; Kandidat δ + k·q, der `real − V` am kleinsten hält.
3. `FrameInterp` in der Probe 0xA2FBB setzen (2.5): alpha, Tick-Zähler aus `probe:Game::FixedStep`, Schritte des
   Frames, Merker `SnapAll`/`InsideStep`/`ClockReset`. Zwischen Akkumulator- und Render-Probe gilt `InsideStep`.
4. Unit-Tests gegen den Gast-Nachbau (`m3d_clock_sim.py` als Vorlage): angezeigte Zeit `G − 1 + alpha` steigt je
   Frame um ½; alpha ∈ {½, 1} mit Phase B; nach Kappung höchstens ein ausgerichteter Frame; QPF 10 MHz, 3,58 MHz,
   24 MHz, 733 MHz; Ereignisse wie in `m3d_clock_sim.py`.

### 4.2 Korrekturen für Zwischenframes (nur aktiv bei Bildrate > Tickrate)

| # | Stelle | Vorschlag | Aufwand |
|---|---|---|---|
| F1 | Leere Zwischenframes (Intro Legal/Tech-Notice; Extras-Betrachter möglich) | In `GpuRenderer::DisplayFrame`: 0 Ticks im Frame, kein Draw, kein Farb-Clear → **das zuletzt präsentierte Bild erneut präsentieren** (RHI: `PresentDesc::repeatPrevious`, D3D11: Present-Pass aus dem vorigen persistenten Puffer, Rotation unverändert). Prüfung: Tech-Notice-Frames 900–915 bei 120 fps alle mit Bild. Falls der Extras-Betrachter (M4) gemischte Frames zeigt (Hintergrund im `Render`, Bild im `Update`): die Draws der Schleifenphase des letzten Tick-Frames in Zwischenframes wiederholen. | klein |
| F2 | `Death::Render` 0x2BF10 (Prolog `56 8B F1 8B 4E 3C`, 6 Byte, keine Sprünge) | Probe: nach dem Original `+0x20 = 0.0f` – jede Update-Zeit wird genau einmal addiert, gezeichnet wird weiter jedes Bild | klein |
| F3 | `FUN_000CD340` (Prolog `83 EC 3C 56 8B F1`) | In Zwischenframes nicht aufrufen (Alpha und Material bleiben auf dem Stand des letzten Ticks) – Ausblendung im 60-Hz-Raster wie im Original | klein |
| F4 | `FUN_00045EF0` (Prolog enthält `call rand` bei +3, kein Detour mit Trampolin) | Ersatz-Hook: in Tick-Frames das Original-Verhalten nachbilden (`rand` über `CallGuest` 0x1668CC, `& 0x80000001` mit Vorzeichenkorrektur, +0x24, +0xA8 = 1), in Zwischenframes nichts tun | klein |
| F5 (optional) | 5 Schwarzbilder nach Intermission ([0x2EA99C]) | In Zwischenframes bei [0x2EA99C] > 0 `Ingame::Render` überspringen (F1 wiederholt das Löschbild) – Dauer und Skript-Zusatzläufe wie bei 60 fps | mittel; erst mit M8 messbar |
| – | Lade-Pumpen | so lassen (schnelleres Laden). Optional Entwicklerschalter „Pumpen nur in Tick-Frames“ für Zeitlinien-Vergleiche | – |

### 4.3 Takt und Konfiguration

1. `[render] frame_rate` wird eine Auswahl `60 | 120 | display` (de: „60 fps“, „120 fps (interpoliert)“, „Bildschirm
   (max. 120)“; en entsprechend), Zahlen bleiben als Entwicklerwerte gültig. Standard-Empfehlung: `display`, sobald
   die Interpolation abgenommen ist; bis dahin `60`.
2. Planauswahl nach 3.2/3.3 mit F statt T; Kadenz-Plan neu; Quant der Uhr aus dem Plan.
3. Just-in-time-Start für vblank-Pläne (3.5), messgesteuert.
4. Swapchain im Vollbild über den ganzen Monitor (Pillarbox im Present-Pass), damit VRR/Tearing im Vollbild wirken.

### 4.4 Restrisiko und Prüfung

**Doppel-Render-Test** (Entwicklerschalter): bei 60 fps `cur->Render()` und die Overlays zweimal je Frame aufrufen
(zweiter Durchlauf ohne Present). Jeder Code, der im Render-Pfad Zustand weiterschaltet (Zeitakkumulation, Zähler,
`rand`), fällt dann als Abweichung gegenüber dem Normallauf auf (Bilder, Spielzeit, `rand`-Zustand im
Per-Thread-Block +0x14). Das deckt die virtuellen Entitäten-Renderer (1.3 Nr. 20) ab, die statisch nicht auflösbar
sind; Einsatz mit M4/M8, sobald Ingame erreichbar ist.

### 4.5 Für die Prüfung (Phase 3)

- Frame-Zahlen in Konfigurationen und Prüfskripten aus Sekunden × Bildrate rechnen (`[debug] frames`,
  `screenshot_frames`, `trace_frames`); bei 120 fps endet das Ladebild nach 1,86 s, das Intro bei 13,26 s –
  `m2_check` (Intro verlassen 15,2–16,0 s) und die Szene „loading“ von `m3_check` (13,6 s) brauchen eigene Werte
  für 120 fps.
- M3b-6 („≥ 99 % der Frames genau ein Schritt“) bei 120 fps als „Schritte je Bildpaar = 1“ bzw. Schritte/s prüfen.
- Flackerprüfung: aufeinanderfolgende Screenshots in Legal (3 s) und Tech-Notice (8 s) bei 120 fps, keine Draw-Summary
  mit „0 draw calls … shows the picture of frame N“, deren Bild abweicht.

---

## 5. Offene Punkte und Risiken

- Ingame, Death, Intermission und Extras-Menü sind ohne Eingabe nicht erreichbar; F2–F5 und Nr. 2/20 sind statisch
  belegt, nicht gemessen.
- Der Mechanismus hinter der späten Freigabe des Waitables bei k = 1 (1,9 ms vor dem Vblank) ist nicht geklärt; im
  zusammengesetzten Fenster ohne VRR ist er anders (pacing60.md 2.4).
- 144/165/180/240-Hz-Anzeigen und 59,94/119,88 Hz sind nur simuliert (`m3d_pacing_sim.py`), nicht gemessen.
- `rand()` könnte auch in virtuellen Render-Methoden laufen (Partikel, Entitäten); Determinismus für das Replay (M9)
  erst mit dem Doppel-Render-Test belegbar.
- Die Phase der Uhr springt bei 120 fps um bis zu 4,7 ms, wenn sich der Startpunkt im Refresh ändert (Frame 190 in
  `e120ha`); der Rand ¾ Quant reichte in allen Läufen, eine vblank-verankerte Eingangszeit wäre robuster.
- Frame-gebundenes Laden wird bei 120 fps schneller; Szenendauern mit Laden hängen von der Bildrate ab (wie B46).

---

## 6. Skripte und Rohdaten

| Datei | Inhalt |
|---|---|
| `scripts\m3d_perframe.py` → `m3d_perframe.out` | Aufrufstellen in `CApp::Frame`/`Game::Frame`, Erreichbarkeit pro Wurzel (direkte Aufrufe, Tiefe 5) mit Markern dt, `g_frameDelta`, `rand`, Skript-Scheduler, `NiAVObject::Update`, Timer, Global-Schreiber, Feldzähler |
| `scripts\m3d_clock_trace.py` → `m3d_clock_trace.out` | Auswertung der Experiment-Traces `frames.csv` |
| `scripts\m3d_clock_sim.py` → `m3d_clock_sim.out` | Gast-Arithmetik (Schwelle, Hälften, Kappungsrest je QPF) und Uhr-Varianten mit Ereignissen |
| `scripts\m3d_pacing_trace.py` | Latenz und Vblank-Phase aus `pacing_trace.csv` |
| `scripts\m3d_pacing_sim.py` → `m3d_pacing_sim.out` | Halten und Positionsfehler je Anzeige und Variante |
| `run\analyse_m3d_clock\c60`, `c120ls`, `c120rt` | saubere Läufe 85 s (Logs, `pacing_trace.csv`, `vfs_trace.log`) |
| `run\analyse_m3d_clock\s60`, `s120ls` | Screenshot-Serien und Draw-Summaries (`pair_900_901.png`, `pair_300_301.png`) |
| `run\analyse_m3d_clock\e60`, `e120h`, `e120ha` | Experiment-Build mit `frames.csv` |
| `run\analyse_m3d_clock\q60max`, `q120max` | „Maximum“-Qualität bei 60/120 fps |
| `run\analyse_m3d_clock\src_exp`, `patch_exp.py`, `build_exp` | Kopie von `src\` mit Prototyp und Trace (nicht Teil des Repositorys) |
