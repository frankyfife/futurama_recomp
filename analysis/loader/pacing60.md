# Präsentation und Takt: 60-Hz-Logik mit displaygekoppeltem Pacing

Stand: 2026-10-05 · Ziel-XBE: `orig\retail_eu\default.xbe` (Basis 0x10000) · Status: **Analyse und Entwurf**, `src\` ist
unverändert. Experimente und Rohdaten liegen unter `run\analyse_pacing\` (git-ignoriert, Liste im Anhang).
Bezug: [`docs/m3_report.md`](../../docs/m3_report.md) 3.x, 7, 10 · [`docs/loader_design.md`](../../docs/loader_design.md)
1.9, 3.1, 6 (M3), 7 (R9), 8.3 · [`src/host/gfx/GFX_CONTRACT.md`](../../src/host/gfx/GFX_CONTRACT.md) 8, 9 ·
[`startup.md`](startup.md) 3.1–3.4 · [`timing60.md`](timing60.md) (60-Hz-Tick: Patch-Tabelle, tick-gebundene Stellen; parallel
entstanden).

**Verbindliche Vorgabe des Nutzers:** Der Port läuft mit **mindestens 60 fps**. Der 50-Hz-PAL-Takt ist unerwünscht.
CApp+0xE8 bleibt 0, damit PAL-Archive, Sprachwahl, PAL-Filme und PAL-Splash/Legal-Bilder erhalten bleiben. Nur die
Zeitwerte ändern sich. Dieses Dokument setzt den 60-Hz-Tick (dt = 1/60, Patch der Basis-Init 0xA26A0, Abschnitt 4.10)
voraus. Es entwirft, wie dieser Tick an die Anzeige gekoppelt wird. Unsichere Deutungen sind als **Vermutung**
markiert.

---

## 0. Kurzfassung

| Thema | Ergebnis | Beleg |
|---|---|---|
| Warum M3-3 hier scheitert | Die Ursache ist **nicht** in erster Linie „50 teilt 120 nicht“. Der Entwicklungsrechner hängt an einem LG-TV mit **VRR**, das auch im Fenstermodus aktiv ist (**Vermutung**: NVIDIA G-SYNC Compatible). Das Panel folgt der Bildrate der **Vordergrundanwendung**. Steht ein anderes Programm vorn (hier `claude.exe`) und zeichnet es selten, fällt das Panel auf den VRR-Boden von **40,4 Hz**. Das Spiel präsentiert mit VSync und wird darauf gedrosselt: Das langsamste 1-s-Fenster liegt dann bei 40,40 Hz, und die Takt-Neustarts folgen. Jeder VRR-Ein-/Ausstieg kostet zusätzlich 14–58 ms. | 2.2, 2.3 |
| 50 Hz auf 120 Hz | Bei fester Bildwiederholrate steht jedes Bild abwechselnd 2 bzw. 3 Refreshes (16,7/25 ms, Judder). Die Rate stimmt trotzdem (50,03 Hz in der Sonde, 0 Neustarts). | 2.4 |
| 60 Hz heute | `[render] frame_rate = 60` ergibt mit dem bestehenden Code 60,01 Hz und 0 Neustarts (2 Läufe). In diesen Läufen trat zufällig keine Phase am VRR-Boden auf. Der heutige Pacer ist aber nicht an die Anzeige gekoppelt. | 2.2 |
| Zeitbasis des Spiels | `Game::Frame` 0xA2C20 tastet QPC ab und akkumuliert die Zeit. Nach jedem Kontextwechsel und jedem Hänger ≥ 100 ms (Kappung auf 6·dt) bleibt ein Rest von **4 Ticks**, also genau die Schrittgrenze. Bei vblank-genauen 60 Hz und 0,05–0,5 ms Jitter des Abtastzeitpunkts haben danach **2,3–37 % der Frames 0 oder 2 Schritte**: Ein Bild wird doppelt gezeigt, beim nächsten fehlt ein Tick. Bei 59,94/119,88 Hz kommt alle 16,7 s ein solcher Ausreißer-Schub hinzu. | 1.4, 2.6 |
| Abhilfe Zeitbasis | Eine **Frame-Clock**: An den drei Aufrufstellen des Spieltimers in `Game::Frame` (0x0A2C6A, 0x0A2FA1, 0x0A2FB6) liefert der Host virtuelle Zeit. Sie wächst pro angezeigtem Spielframe um genau `ticks(dt)`, bei verpassten Slots um ein Vielfaches davon. Ergebnis in allen simulierten Fällen: 0 Fehlschritte. | 4.4 |
| Strategie | Ist die Refreshrate ein ganzzahliges Vielfaches von 60 (60/120/180/240 und die /1,001-Familie): Flip-Modell mit **SyncInterval k = R/60** und Waitable-Swapchain mit MaxFrameLatency 1. Sonst mit bestätigtem VRR: **Timer mit 60 Hz + `Present(0, ALLOW_TEARING)`**. Sonst **Kadenz** mit SyncInterval ⌊R/60⌋/⌈R/60⌉ im Bresenham-Muster. Im Hintergrund oder wenn die Anzeige bremst: **frei laufender 60-Hz-Timer ohne blockierendes Present**. | 4.3 |
| Bildwiederholrate | Primärquelle ist `QueryDisplayConfig` → `targetVideoSignalInfo.vSyncFreq` (rational, hier 120/1). Danach prüft der Pacer den Wert mit DXGI-Frame-Statistiken. `DwmGetCompositionTimingInfo` ist auf diesem Rechner veraltet (`qpcVBlank` 6,3 s alt, `cRefresh` steht) und damit unbrauchbar. | 3 |
| Regel | Der Takt darf **nie allein** am Blockieren von `Present` hängen. Ein minimiertes Fenster präsentiert weiter mit `S_OK` im vblank-Takt. Wie sich Sperre und ausgeschaltete Anzeige verhalten, ist hier nicht messbar (Test T12). | 2.5, 4.7 |

---

## 1. Ist-Zustand

### 1.1 Ablauf eines Frames (Stand M3, Commit cf908e8)

```
CApp::Frame 0xD2630: ClearBuffer → Movie vf8 → Game::Frame 0xA2C20 (Schritte + Render) → Overlays → renderer vf16
  SlotDisplayFrame                          nipc_renderer.cpp:587
    GpuRenderer::DisplayFrame               nipc_gpu.cpp:647
      platform::NoteFrameWorkDone()         :651   Ende der Frame-Arbeit (Statistikzeile 3)
      Screenshot (F12 / screenshot_frames)
      Present({syncInterval = vsync ? 1 : 0})  :676
        D3D11Device::Present                d3d11_device.cpp:1417
          UpdateSwapChainSize (ResizeBuffers bei neuer Fenstergröße), DrawPresentPass (640×480 → Fenster)
          IDXGISwapChain::Present(sync, sync == 0 && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0)   :1427
          persistenten Backbuffer weiterdrehen (Xbox-Flip, B31)
      platform::NotePresentedFrame()        :696   QPC-Zeitstempel → Statistik „present:“
  OnFrameDisplayed                          nipc_renderer.cpp:1338
    platform::PaceFrame([render] frame_rate)  frame_pacer.cpp:133 → PaceFrameLocked :103
    Framezähler, --frames, CloseRequested
```

### 1.2 Eigenschaften

| Baustein | Heute | Folge |
|---|---|---|
| Swapchain | `FLIP_DISCARD`, 2 Puffer, BGRA8, Fenstergröße, `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`, wenn unterstützt (d3d11_device.cpp:553); **kein** Waitable-Objekt | Mit `Present(1)` blockiert erst der dritte Frame in der Warteschlange |
| Latenz | `IDXGIDevice1::SetMaximumFrameLatency(2)` für das ganze Device (d3d11_device.cpp:453) | Bis zu 2 Frames Vorlauf; der Pacer wartet erst nach `Present` |
| VSync | `Present(1)` (`[render] vsync = 1`) | Zweiter, unabhängiger Takt neben dem Timer |
| Pacer | M2-Algorithmus: relativer Hochauflösungs-Waitable-Timer bis `g_nextFrame`, dann `g_nextFrame += QPF/hz` (ganzzahlig: 50 Hz = 200 000 Ticks exakt, 60 Hz = 166 666 Ticks = 60,0002 Hz). Liegt der Frame mehr als eine Periode hinter dem Plan, beginnt ein neuer Zeitplan („schedule reset“), und die verlorene Zeit ist weg. | Keine Phasenkopplung an den vblank. Ob ein Bild 2 oder 3 Refreshes steht, hängt von der zufälligen Phase Timer ↔ vblank ab |
| Statistik | QPC-Zeitpunkt nach `Present` (Rate, langsamstes 1-s-Fenster, längster Frame), Neustarts, Frame-Arbeit | Keine Aussage darüber, was wann auf dem Schirm stand |
| Anzeigewissen | keines; 50 Hz aus der Konfiguration | Refreshrate, VRR, Vordergrund, Anzeige an/aus unbekannt |

### 1.3 Schwächen

1. **Zwei unabhängige Uhren.** Der Timer läuft mit 50/60 Hz auf QPC-Basis, die Anzeige mit ihrem vblank. Bei 50 Hz auf
   120 Hz entsteht der 2/3-Judder. Bei 60 Hz auf 120 Hz driftet die Phase langsam: 166 666 Ticks Timerperiode gegenüber
   2 × 83 333,3 Ticks vblank-Paar, also 0,6 Ticks pro Frame. Etwa alle 2,3 min läuft sie einmal durch eine vblank-Grenze.
   Dort kippen einzelne Bilder zwischen 1 und 3 Refreshes (abgeleitet, nicht gemessen).
2. **Blockierendes `Present` und Timer gleichzeitig.** Wird die Anzeige langsamer als geplant (VRR-Boden, siehe 2.3),
   blockiert `Present`. Der Timer meldet dann Neustarts, und die Rate fällt unter den Sollwert.
3. **Kein Wissen über die Anzeige.** Der Takt kommt aus der Konfiguration. Monitorwechsel, Frequenzwechsel, VRR und
   Vordergrund bleiben unbemerkt.
4. **Zeitbasis des Gasts** (1.4). Das Spiel tastet QPC zu einem schwankenden Zeitpunkt ab. Nach Ladehängern steht der
   Akkumulator auf der Schrittgrenze.
5. Mit `[render] frame_rate = 0` hängt der Takt allein an `Present(1)`.

### 1.4 Zeitbasis des Gasts (Game::Frame 0xA2C20)

Spieltimer `FUN_000D2B90` (cdecl, `void(int64* out)`, `ret` ohne Operand, Aufrufer räumen mit `add esp,4` ab): beim
ersten Aufruf QPF nach 0x2EB448 und `1.0f/(float)QPF` nach 0x2EB450, dann `XAPILIB::QueryPerformanceCounter` →
Win32-QPC (Design 1.9). Aufrufstellen in `Game::Frame` (capstone, `run\analyse_pacing\src\timer_sites.py`):

| Stelle | Bytes | Zweck |
|---|---|---|
| 0x0A2C6A | `e8 21 ff 02 00` | „jetzt“ für Akkumulator und fps-Zähler (CApp+0xC8 `last`, +0xD0 Zähler, +0xD8 Start, +0xE0 fps) |
| 0x0A2FA1 | `e8 ea fb 02 00` | einmalige Initialisierung von 0x2E7D50 (vorheriger Render-Zeitpunkt) |
| 0x0A2FB6 | `e8 d5 fb 02 00` | Render-Zweig: `g_frameDelta` [0x2E7B88] = jetzt − 0x2E7D50 |

Rechenweg (Disassembly 0xA2CE8…0xA2F73, 0xD2B10, 0xD2B50, 0xD2B80, `_ftol2` 0x167080):
- `sec(t) = (x87) t · float(1/QPF)`.
- `ticks(s) = trunc(QPF · s)`.
- `elapsed = now − last`, gekappt auf `ticks(6·dt)`.
- Die Schleife macht einen Schritt, solange `sec(elapsed) ≥ dt` gilt, und zieht dabei jeweils `ticks(dt)` ab. Danach
  folgt `last = now − elapsed`.

Bei QPF = 10 MHz (hier gemessen) gilt `ticks(dt) = 166 666` für dt = 1/60 (0x3C888889). Ein Schritt braucht
`elapsed ≥ 166 667`. Nach einem gekappten Frame bleibt `ticks(6·dt) − 6·166 666 = 1 000 000 − 999 996 = `**4 Ticks**
Rest. Gekappt wird bei jedem Hänger ≥ 100 ms zwischen zwei Zeitabfragen. Dazu gehört **jeder Kontextwechsel**: Er
läuft innerhalb der Schrittschleife, die die Zeit nicht neu misst, mit 8 Leerframes aus `FUN_0002A660` (≥ 133 ms bei
60 Hz) plus `Enter()`. Jeder dieser Fälle setzt die Phase also exakt an die Grenze (2.6). Danach entscheidet allein
der Jitter des Abtastzeitpunkts, ob ein Frame 0, 1 oder 2 Schritte bekommt. Dieser Zeitpunkt liegt **nach**
`ClearBuffer`, dem Movie-Update und den Streaming-Pumpen `FUN_000D2370`/`FUN_000F2050`/`FUN_000F2680`
(Fiber-Wechsel), und deren Dauer schwankt. [`timing60.md`](timing60.md) 6 modelliert denselben Effekt mit zufälliger
Startphase (11,6 % 0- und 11,6 % 2-Schritt-Frames bei Phase 0 und 0,3 ms Jitter).

Die Render-Seite nutzt die echte Frame-Dauer `g_frameDelta` [0x2E7B88] an sechs Stellen in fünf Funktionen:
`FUN_00021B60`, `FUN_00045F10` (addiert sie auf eine Zeit und ruft damit `FUN_00101840` auf, **Vermutung**:
NiAVObject::Update; aufgerufen aus `FUN_000C8E90`), `FUN_000C0AC0`, `FUN_000C8E90` (auf 0,1 gekappt) und `FUN_000EEF80` (Überblendung, auf
0,05 gekappt). Diese Teile animieren also schon heute mit der Render-Rate.

---

## 2. Messungen auf dem Entwicklungsrechner

### 2.1 Umgebung (`display_probe.exe`, 19:39)

| Größe | Wert |
|---|---|
| System | Windows 11 Pro 10.0.26200, QPF 10 000 000 |
| Sitzung / Anzeige | entsperrt (`WTSSessionInfoEx` SessionFlags 1, Eingabedesktop „Default“), `GUID_CONSOLE_DISPLAY_STATE` = 1 (an) |
| Ausgabe | ein aktiver Pfad: `\\.\DISPLAY1` → „LG TV SSCR2“, HDMI (outputTechnology 5), 3840×2160, 10 bpc, MaxLuminance 1499, DPI 216 |
| Modus | QDC `targetInfo.refreshRate` = 120/1, `targetVideoSignalInfo.vSyncFreq` = 120/1, Gesamtraster 4400×2250, Pixeltakt 1188 MHz; `EnumDisplaySettings` 120 |
| gemessen | `IDXGIOutput::WaitForVBlank` ×240: Mittel 8,33328 ms = **120,0008 Hz** (8,27…8,40 ms) |
| DWM | `rateRefresh` 10 000 000/83 333 = 120,0005 Hz, aber `cRefresh` +1 in 2 s, `qpcVBlank` 6,3 s alt: **veraltet**, solange DWM nicht zusammensetzt |
| Modi bei 4K | 24, 25, 30, 48, 50, 60, 75, 100, 120 und 23,976, 29,97, 59,94 (60000/1001), 119,88 (119880/1000) |
| DXGI | `DXGI_FEATURE_PRESENT_ALLOW_TEARING` = 1; `CheckHardwareCompositionSupport` = 0x3 (Vollbild + Fenster, MPO) |
| Adapter | RTX 5090 (Ausgabe), dazu AMD Radeon (iGPU) und RTX 3050 ohne aktive Ausgabe |
| Registry (nur gelesen) | `DirectXUserGlobalSettings` = `SwapEffectUpgradeEnable=1;…`, kein `VRROptimizeEnable` |
| VRR | Mit öffentlichen APIs nicht abfragbar. Gemessen (2.3): aktiv für das Vordergrundfenster, Boden 24,74 ms = 40,4 Hz |

### 2.2 Spiel (build\m3 Debug, d3d11 + Debug-Layer, 3000 Frames, Fenster 2631×1973)

Konfigurationen `run\analyse_pacing\game*\futurama.ini` (M3-Debug-Konfiguration ohne Screenshots und Traces).

| Lauf | `frame_rate` | Rate | langsamstes 1-s-Fenster | längster Frame | Neustarts | > 1,5 Perioden | Frame-Arbeit Ø / 99 % / max |
|---|---:|---:|---:|---:|---:|---:|---|
| m3_check run_01/02 (19:29/19:30, vorher) | 50 | 49,34 / 49,22 Hz | 43,28 / 41,96 Hz | 46,7 ms | 35 / 41 | 10 / 17 | 3,15–3,21 / 4,8–5,1 / 17,9–20,9 ms |
| game50 | 50 | 49,68 Hz | 41,36 Hz | 46,5 ms | 17 | 9 | 3,13 / 4,9 / 17,0 ms |
| game50b (+ vblank-Monitor) | 50 | 49,44 Hz | **40,40 Hz** | 46,6 ms | 29 | 10 | 3,12 / 5,4 / 17,4 ms |
| game60 | 60 | 60,01 Hz | 58,98 Hz | 33,7 ms | 0 | 5 | 3,06 / 5,8 / 16,8 ms |
| game60b (+ vblank-Monitor) | 60 | 60,01 Hz | 58,60 Hz | 39,7 ms | 0 | 6 | 2,98 / 5,7 / 23,2 ms |
| game0 | 0 (nur VSync) | 119,64 Hz | 114,16 Hz | 33,5 ms | – | – | 1,94 / 5,3 / 19,4 ms |

Die Frame-Arbeit überschreitet in den 50-Hz-Läufen höchstens in einem Frame eine Periode (m3_check run_02: 20,9 ms).
Die verlorene Zeit entsteht also in `Present` und im Warten. `game0` zeigt: Das Spiel trägt 120 Hz. Bei PAL-dt hat
dort mehr als die Hälfte der Frames keinen Schritt, daher die kleinere Ø-Arbeit.

### 2.3 VRR folgt dem Vordergrund (`vblank_monitor.exe` parallel zum Spiel)

Der Monitor ruft in einer Schleife `IDXGIOutput::WaitForVBlank` auf und protokolliert das Vordergrundfenster (nur
Klasse und Prozessname). Mit aktivem VRR folgt der vblank-Abstand der Bildrate der Anwendung, die das Panel steuert.

| Zeit (game50b) | Panel | Median-Abstand | Vordergrund |
|---|---|---:|---|
| 0,53–2,42 s | VRR | 20,01 ms (49,98 Hz) | Spielfenster |
| 2,43–36,48 s | fest | 8,33 ms (120,00 Hz) | chrome.exe |
| 36,50–39,45 s | VRR | 20,02 ms (49,95 Hz) | Spielfenster |
| 39,46–44,68 s | fest | 8,33 ms | claude.exe |
| **44,71–51,52 s** | **VRR-Boden** | **24,74 ms (40,4 Hz)** | **claude.exe** |
| 51,52–63,01 s | fest | 8,33 ms | claude.exe |

game60b: VRR mit 16,48 ms (60,7 Hz) nur in 0,51–1,18 s, solange das Spiel vorn war, danach fest 120 Hz (chrome.exe
vorn). Eine Phase am VRR-Boden gab es nicht, deshalb 0 Neustarts. Das langsamste Fenster von game50b (40,40 Hz) fällt
genau auf den VRR-Boden. Die M3-3-Fehlschläge auf diesem Rechner sind damit ein Messartefakt der Umgebung:
VRR + anderes Vordergrundprogramm + VSync-gebundenes `Present`. Bei 50 Hz kommen VRR-Übergänge und der 2/3-Judder
hinzu. Wie viele der 29 Neustarts auf welche Ursache entfallen, lässt sich ohne Frame-Trace im Spiel nicht aufteilen.

### 2.4 Präsentationsvarianten (`present_probe.exe`)

Die Sonde hat dasselbe Thread-Modell wie das Spiel: ein UI-Thread besitzt Rahmen- und Kindfenster und pumpt
Nachrichten; ein Render-Thread hält die Swapchain auf dem Kindfenster und präsentiert. Je Frame laufen 3 ms
CPU-Arbeit, ein Clear und ein `ClearView`. Gemessen wird mit QPC und DXGI-Frame-Statistiken (`GetLastPresentCount`,
`GetFrameStatistics`: PresentCount → SyncRefreshCount/SyncQPCTime des Refreshes, in dem das Bild sichtbar wurde).
„Dauer“ = Refreshes bis zum nächsten sichtbaren Bild.

Kindfenster 2631×1973, aktiviert (wie `ShowWindowAsync(SW_SHOWNORMAL)` im Spiel). Das Fenster blieb 0,7–2,3 s im
Vordergrund, dann ging der Fokus an ein anderes Programm zurück. Die „1“-Dauern stammen aus dieser VRR-Phase:

| Modus | Takt | Rate | Dauer in Refreshes | Present → sichtbar (Median) | Bemerkung |
|---|---|---:|---|---:|---|
| `legacy:50` = heute | `Present(1)`, Latenz 2, M2-Timer 50 Hz | 50,04 Hz | 1: 116, 2: 528, 3: 350 | 4,1 ms | Judder 16,7/25 ms; ein `Present` blockierte 14 ms beim VRR-Übergang |
| `legacy:60` | dito, 60 Hz | 60,08 Hz | 1: 81, **2: 513**, 4: 1, 7: 1 | 2,6 ms | Phase Timer ↔ vblank zufällig günstig |
| **`sync:2`** | Waitable, Latenz 1, `Present(2)` | 60,06 Hz | **2: 592** (+ 4 Einzelwerte) | 13,6 ms | auf das 120-Hz-Raster ausgerichtet (Phase 0,005) |
| `dup:2` | Waitable, `Present(1)` je vblank, neuer Spielframe jeder 2. | 59,71 Hz (3 Hänger von 33 ms) | 1: 1194 (120 Presents/s) | 6,2 ms | doppelte Presents |
| `timer:60` | Waitable, absoluter Timer 60 Hz, `Present(0, ALLOW_TEARING)` | 60,00 Hz | 1: 44, 2: 549 | – | zusammengesetztes Fenster: Tearing wirkungslos, das Waitable bremst auf die DWM-Zusammensetzung (13,5 ms) |
| `timersync:60` | Waitable, Timer 60 Hz, `Present(1)` | 59,77 Hz | 1: 35, 2: 559 | 5,0 ms | 1 Neustart beim Übergang |

Randloses Vollbild 3840×2160 im Vordergrund (Independent Flip, VRR aktiv):

| Modus | Rate | Dauer / sichtbare Abstände | Present → sichtbar |
|---|---:|---|---:|
| `sync:2` | 60,02 Hz | 2: 298 von 299, 16,67 ms | 13,6 ms |
| `timer:60` | 60,00 Hz | 1: 268; Abstände 15,0–18,5 ms (VRR folgt dem Timer-Jitter); in 1,4–1,9 s Gruppen gleicher Sync-Zeit mit Lücken von 33–83 ms (Übergang) | – |
| `dup:2` | 60,52 Hz | 1: 594, 8,33 ms | 6,7 ms |
| `legacy:50` | 50,14 Hz | 1: 297, 19–21 ms (VRR fährt das Panel mit 50 Hz) | – |

Oberstes Fenster, nicht aktiviert (feste 120 Hz): `legacy:50` 50,03 Hz, Dauern 2: 900, 3: 598, exakt im Raster
(Phase 0,0016). Das ist der reine 50-auf-120-Judder ohne Ratenverlust.

Timer-Genauigkeit (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, absoluter Plan): Weckfehler Median 0,26 ms, 99 % 0,51 ms,
max. 0,93 ms. Debug-Layer an (`PROBE_DEBUG_LAYER`): keine Änderung (`legacy:50` 50,03 Hz, 0 Neustarts).

### 2.5 Minimiert und verdeckt

| Fall | Ergebnis |
|---|---|
| Minimiert (`sync:2`, `legacy:60`, `timer:60`, je 100 Frames) | `Present` liefert `S_OK`, **nicht** `DXGI_STATUS_OCCLUDED`. Das Waitable (`sync:2`, `timer:60`) signalisiert weiter im vblank-Takt (Wartezeit 13,5–13,6 ms), die Rate bleibt in allen drei Modi 60 Hz. Die Frame-Statistik ordnet nur noch 62–76 % der Presents einem Refresh zu und friert danach ein. |
| Vollbild verdeckt (Vordergrund verloren, `sync:2 full`) | `Present`/Waitable weiter mit 60 Hz. Die Frame-Statistik bleibt stehen (2 Presents in 300 Frames). |
| Session gesperrt, Anzeige aus | **Nicht gemessen.** Beides hätte den Rechner des Nutzers gesperrt bzw. die Anzeige ausgeschaltet. Erwartungen und Test siehe 4.7 und T12. |

### 2.6 Akkumulator-Simulation (`sim_accumulator.py`)

Die Simulation rechnet die Gast-Arithmetik aus 1.4 exakt nach: float32 dt, x87 mit 53 Bit, `_ftol2` mit Abschneiden.
Gezählt werden Frames mit ≠ 1 Schritt in 5 min (18 000 Frames). Der Abtastzeitpunkt hat Gauß-Jitter σ, gemittelt über
10 Läufe.

| Anzeige | σ | QPC, Rest zufällig | QPC, Rest nach Kontextwechsel/Hänger (4 Ticks) | Frame-Clock |
|---|---:|---:|---:|---:|
| 60,000 Hz (k = 1) | 0,05 / 0,25 / 0,5 ms | 0 / 0,6 / 522 | 477 / 2671 / 4483 | 0 |
| 120,000 Hz (k = 2) | 0,05 / 0,25 / 0,5 ms | 231 / 712 / 1349 | 417 / 2019 / 3953 | 0 |
| 120,0005 Hz (dieser PC) | 0,05 / 0,25 / 0,5 ms | 0 / 516 / 1102 | 5316 / 6758 / 5794 | 0 |
| 59,94 Hz | 0,05 / 0,25 / 0,5 ms | 61 / 301 / 627 | 66 / 311 / 623 | 0 |
| 119,88 Hz (k = 2) | 0,05 / 0,25 / 0,5 ms | 63 / 307 / 626 | 68 / 307 / 624 | 0 |

Die Zeilen 60,000 Hz und 120,000 Hz haben dieselbe Periode. Ihre Unterschiede sind Streuung der Zufallsläufe; beim
zufälligen Rest entscheidet vor allem, ob er in der Nähe der Grenze landet.

Echte Frame-Starts der Sonde, Rest nach einem Ladehänger:
- `timer:60`: {0: 157, 1: 284, 2: 157}, also ein Viertel der Frames ohne Schritt.
- `sync:2`, `sync:2 full`: {0: 1, 1: 597} und {0: 1, 1: 297}. Das Waitable kehrt sehr genau nach dem vblank zurück.

Im Spiel liegt der Abtastzeitpunkt hinter schwankender Arbeit (1.4). Erst die Frame-Clock macht das Ergebnis
unabhängig vom Jitter.

### 2.7 Folgerungen

1. 60 Hz auf 120 Hz ist mit `SyncInterval 2` (Waitable, Latenz 1) exakt: jedes Bild 2 Refreshes, kein Neustart. Das
   gilt im Fenster und im Vollbild, mit und ohne VRR.
2. Ein Pacer, der an `Present(1)` hängt, ist auf VRR-Systemen von anderen Programmen abhängig. Im Hintergrund muss der
   Takt vom Display entkoppelt werden (4.7). Prüfläufe müssen den Vordergrund protokollieren und sicherstellen (6).
3. `ALLOW_TEARING` wirkt nur ohne DWM-Zusammensetzung (Independent Flip/MPO, typisch randloses Vollbild). Im
   zusammengesetzten Fenster gibt es weder Tearing noch VRR-Vorteil.
4. Bei festen 60 Hz entscheidet der Abtast-Jitter über doppelte und ausgelassene Schritte. Ohne Frame-Clock ist auch
   ein perfekt getakteter Present-Pfad nicht ruckelfrei.

---

## 3. Bildwiederholrate ermitteln

### 3.1 Quellen

| Quelle | Liefert | Hier | Bewertung |
|---|---|---|---|
| `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` → Pfad mit `DisplayConfigGetDeviceInfo(GET_SOURCE_NAME).viewGdiDeviceName` = Gerätename des Monitors → `modes[targetModeInfoIdx].targetMode.targetVideoSignalInfo.vSyncFreq` | rationale Signalfrequenz der aktuellen Ausgabe, z. B. 60000/1001 | 120/1 | **Primärquelle.** Mit `QDC_VIRTUAL_MODE_AWARE` den Index `targetModeInfoIdx` verwenden, sonst `modeInfoIdx`. `vSyncFreqDivider` (Miracast) mitloggen |
| `DISPLAYCONFIG_PATH_TARGET_INFO::refreshRate` | rationale Pfadfrequenz | 120/1 | Gegenprobe; mit `QDC_VIRTUAL_REFRESH_RATE_AWARE` ggf. die virtuelle Desktoprate (Windows 11) |
| `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS).dmDisplayFrequency` | ganzzahlig (59 für 59,94) | 120 | nur Rückfall, gebrochene Raten nicht unterscheidbar |
| `IDXGIOutput::GetDisplayModeList` | verfügbare Modi | 13 Raten | zeigt, welche Raten es gibt, **nicht** die aktuelle |
| `IDXGIOutput::FindClosestMatchingMode` (Refresh 0) | „nächster“ Modus | 120/1 | **nicht** der aktuelle Modus |
| `DwmGetCompositionTimingInfo(NULL)` | `rateRefresh`, `qpcRefreshPeriod`, `qpcVBlank` | 120,0005 Hz, aber veraltet | **ungeeignet**: friert ein, solange DWM nicht zusammensetzt (Leerlauf, Independent Flip), und schwankt unter VRR (in einem Lauf 10⁷/85 461 = 117 Hz) |
| `IDXGIOutput::WaitForVBlank` | tatsächlicher vblank (auch unter VRR) | 120,0008 Hz | blockiert einen Thread; nur Diagnose und Prüfwerkzeug |
| DXGI-Frame-Statistik der eigenen Swapchain | SyncRefreshCount/SyncQPCTime je sichtbarem Present | 2 Refreshes je Frame bei `sync:2` | **Messquelle im Betrieb**: misst Periode, Phase, VRR; erste Abfrage liefert `DXGI_ERROR_FRAME_STATISTICS_DISJOINT` (0x887A000B, gesehen); friert bei Verdeckung ein |

### 3.2 Verfahren (Render-Thread, an Frame-Grenzen)

```
OutputTiming QueryOutputTiming(HWND frame):
  mon  = MonitorFromWindow(frame, MONITOR_DEFAULTTONEAREST); GetMonitorInfoW(mon) → szDevice ("\\.\DISPLAY1")
  QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE, …)
  für jeden Pfad: GET_SOURCE_NAME == szDevice → nominal = targetVideoSignalInfo.vSyncFreq (Num/Den)
                                                 path   = targetInfo.refreshRate
  sonst EnumDisplaySettingsW(szDevice, ENUM_CURRENT_SETTINGS) → dmDisplayFrequency/1
  tearing = IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)
  output  = IDXGISwapChain::GetContainingOutput() → IDXGIOutput6::GetDesc1 (DeviceName muss szDevice sein;
            DXGI_ERROR_NOT_FOUND bei einem Monitor an einem anderen Adapter → keine Frame-Statistik, Timer-Modi)
  Log: "display: \\.\DISPLAY1 \"LG TV SSCR2\" 120/1 Hz (QDC signal), path 120/1, tearing 1, output on adapter 0"

PeriodVerifier (laufend, aus der Frame-Statistik, nur im Vordergrund und bei fester Rate):
  über ≥ 120 Refreshes: P_mess = ΔSyncQPCTime / ΔSyncRefreshCount
  |P_mess − 1/nominal| / P_nominal > 0,3 % über 2 Fenster → nominal = 1/P_mess (gemeldet als "measured"),
  Plan neu wählen; Abstände, die kein Vielfaches von P sind → VRR-Verdacht (4.6)
```

### 3.3 Gebrochene Raten

Die /1,001-Familie (59,94, 119,88, 239,76 Hz) kommt aus QDC als `60000/1001` bzw. `119880/1000`. Dafür gilt
`k = round(R/60)`, und ein ganzzahliges Verhältnis wird angenommen, wenn |R/k − 60|/60 ≤ 0,2 %. Die Frequenz des
Spielticks bleibt 60,000 Hz (dt der NTSC-Konstanten). Zwei Varianten sind wählbar (`[render] tick_lock`):
- **`display`** (Standard): Die Frame-Clock zählt genau einen Tick pro k Refreshes. Das Spiel läuft dann gegenüber der
  Wanduhr 0,1 % langsamer (3,6 s pro Stunde) und dafür ohne jeden Ruck. Toneffekte hängen an Spielereignissen und sind
  nicht betroffen. Gestreamte Sprache in Zwischensequenzen kann bis zu 1 ms pro Sekunde gegenüber dem Lippensync
  driften (**Vermutung**: unkritisch bis etwa 60 s Dialog). XMV-Filme nutzen eigenes QPC und sind nicht betroffen.
- **`realtime`**: Die Frame-Clock folgt der Wanduhr. Bei 59,94 Hz bekommt dann alle 1000 Frames (16,7 s) ein Frame zwei
  Schritte. So verhielt sich auch die NTSC-Xbox an einem 59,94-Hz-Fernseher.

Die Vorgabe „≥ 60 fps“ ist an einer 59,94-Hz-Anzeige ohne Tearing physikalisch nicht erreichbar. Mit VRR kann der
VRR-Pfad genau 60,000 Hz liefern. Der Pacer probiert ihn bei `tick_lock = realtime` und Tearing-Unterstützung aus und
bleibt nach bestätigtem VRR (4.6) dabei. Dann gibt es keine Doppelschritte.

### 3.4 Ereignisse

Der UI-Thread (platform/window.cpp) setzt Flags bzw. erhöht eine Epoche. Der Render-Thread prüft sie an jeder
Frame-Grenze:

| Ereignis | Quelle | Reaktion |
|---|---|---|
| Fenster auf anderem Monitor | `WM_WINDOWPOSCHANGED` → `MonitorFromWindow(frame)` ≠ zuletzt; `WM_DPICHANGED` | Epoche +1 → `QueryOutputTiming`, Plan neu, VRR-Erkennung zurücksetzen |
| Modus/Frequenz geändert, Monitor an/ab | `WM_DISPLAYCHANGE` (auch bei reinem Frequenzwechsel: in T9 prüfen); `IDXGIFactory1::IsCurrent()` = FALSE → neue Factory für Abfragen | dito; die Swapchain bleibt |
| Vordergrund | `WM_ACTIVATEAPP`/`WM_ACTIVATE`; Kontrolle `GetForegroundWindow() == frame` | Vordergrund ↔ Hintergrund (4.7) |
| minimiert | `WM_SIZE` mit `SIZE_MINIMIZED` / `IsIconic` | Hintergrundmodus, Presents auslassen |
| Anzeige aus/an/gedimmt | `RegisterPowerSettingNotification(frame, GUID_CONSOLE_DISPLAY_STATE)` → `WM_POWERBROADCAST`/`PBT_POWERSETTINGCHANGE` (0 aus, 1 an, 2 gedimmt; der aktuelle Wert kommt sofort nach der Registrierung, hier gesehen) | aus → frei laufender Timer, Presents aus |
| Sitzung gesperrt | `WTSRegisterSessionNotification(frame, NOTIFY_FOR_THIS_SESSION)` → `WM_WTSSESSION_CHANGE` (`WTS_SESSION_LOCK`/`UNLOCK`) | dito |
| Anzeige bremst | Pacer-intern: Waitable-Wartezeit > 1,5 geplante Perioden in 3 von 8 Frames, oder sichtbare Abstände > Plan | Hintergrundmodus, bis die Messung 1 s lang wieder stimmt |

Ein Frequenzwechsel ohne Nachricht fällt spätestens dem `PeriodVerifier` auf.

---

## 4. Entwurf

### 4.1 Grundsätze

1. **Eine Uhr pro Modus.** Entweder taktet der vblank (Waitable + SyncInterval), oder ein absoluter
   Hochauflösungs-Timer taktet (VRR, Hintergrund). Nie beide gegeneinander.
2. **Nie allein am Blockieren von `Present` hängen.** Jeder Wartevorgang hat ein Timeout von 2 Perioden, danach greift
   der Timer.
3. **Gastframe und Swapchain-Present trennen.** Der Gast-`DisplayFrame` dreht immer den persistenten Backbuffer weiter
   (Xbox-Flip, B31). Ob, wie oft und mit welchem SyncInterval die Swapchain präsentiert, entscheidet der Pacer
   (`skip`, `duplicate`).
4. **Die Spielzeit gehört dem Pacer.** `Game::Frame` sieht die Frame-Clock (4.4), nicht das rohe QPC.
5. **Messen, was auf dem Schirm war.** Frame-Statistik ist die Wahrheit für Kadenz, VRR und verpasste Refreshes. QPC ist
   die Wahrheit für die Spiellogik-Rate.

### 4.2 Swapchain und RHI

- Erzeugung: `FLIP_DISCARD`, 2 Puffer (3 nur für `dup` bei k > 4), Flags `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`
  und `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` (wenn unterstützt). Dann `IDXGISwapChain2::SetMaximumFrameLatency(1)` und
  `GetFrameLatencyWaitableObject()`; der Handle wird beim Zerstören mit `CloseHandle` geschlossen.
  `IDXGIDevice1::SetMaximumFrameLatency(2)` entfällt, weil es für Waitable-Swapchains nicht gilt. Vor dem ersten Frame
  einmal auf den Handle warten.
- `ResizeBuffers` übergibt dieselben Flags (`swapFlags_` tut das heute schon). Beide Flags lassen sich nicht nachträglich
  ändern.
- `DXGI_PRESENT_ALLOW_TEARING` nur mit SyncInterval 0. Sonst `DXGI_ERROR_INVALID_CALL`.
- Erweiterung von `rhi.h` (Vorschlag):

```cpp
struct PresentDesc {
    std::uint32_t syncInterval = 1;   // 0..4
    bool allowTearing = false;        // only with syncInterval 0 and Info().tearingSupported
    std::uint32_t duplicates = 0;     // extra Present(1) of the same picture (dup mode, k > 4)
    bool skipSwapChain = false;       // rotate the persistent back buffers only (background / display off)
};
struct PresentTiming {                // after Present
    std::uint32_t presentId = 0;      // IDXGISwapChain::GetLastPresentCount
    bool statsValid = false;          // GetFrameStatistics succeeded (not DISJOINT)
    std::uint32_t statPresentCount = 0, statPresentRefreshCount = 0, statSyncRefreshCount = 0;
    std::int64_t statSyncQpc = 0;
    bool occluded = false;            // DXGI_STATUS_OCCLUDED
};
// Device:
virtual bool WaitForPresentSlot(std::uint32_t timeoutMs) noexcept = 0;  // frame-latency waitable; true = signaled
virtual bool PresentSlotReady() noexcept = 0;                          // WaitForSingleObject(h, 0)
virtual PresentTiming LastPresentTiming() const noexcept = 0;
virtual bool ContainingOutputName(wchar_t (&name)[32]) noexcept = 0;   // GetContainingOutput → DeviceName
```

`DeviceInfo` erhält `tearingSupported`. Der Headless-Pfad liefert `WaitForPresentSlot` = sofort `true` und
`statsValid` = `false`.

### 4.3 Modi und Auswahl

T = Tickrate aus dem Gast (`[0x2D6744]` nach der Basis-Init, 60,0). R = Refreshrate (3.2). Das Verfahren ist allgemein
in T, also auch für einen PAL-Tick (T = 50) verwendbar.

| Modus | Bedingung | Takt | Present | Spielzeit (4.4) |
|---|---|---|---|---|
| **VblankLocked(k)** | Vordergrund; R/T = k ganzzahlig (±0,2 %), 1 ≤ k ≤ 4 | Waitable (Latenz 1) | `Present(k, 0)` | +1 Tick je Frame, + verpasste Slots |
| VblankDup(k) | wie oben, k > 4 (≥ 300 Hz) | Waitable | `Present(⌈k/2⌉)` + Duplikat(e) mit dem Rest bzw. `Present(1)` × k | dito |
| **Vrr** | Vordergrund; R/T nicht ganzzahlig (oder R < T); Tearing unterstützt; VRR bestätigt (4.6) | absoluter Timer T Hz; vor dem Present `PresentSlotReady`, sonst bis 1 Periode warten | `Present(0, ALLOW_TEARING)` | Timer-Slots |
| **Cadence** | Vordergrund; R/T nicht ganzzahlig; kein bestätigtes VRR | Waitable | Frame n: `s_n = ⌊(n+1)·R/T + ½⌋ − ⌊n·R/T + ½⌋` (144 Hz: 2,3,2,3,2 …; 75 Hz: 1,2,1,1 …); `s_n` > 4 → Duplikat; `s_n` = 0 (R < T) → `skipSwapChain` | +1 Tick je Frame |
| **Free** | Hintergrund, minimiert, verdeckt, Anzeige aus, gesperrt, Anzeige bremst (3.4) | absoluter Timer T Hz | `PresentSlotReady()` ? `Present(0, tearing)` : nur Backbuffer drehen; minimiert/aus/gesperrt: nie präsentieren | Echtzeit (Timer-Slots) |
| Legacy(hz) | `[render] pacing = legacy` (Vergleich, Null-Backend) | heutiger M2-Pacer | `Present(vsync)` | QPC |

Auswahl (an jeder Frame-Grenze, Hysterese: Wechsel frühestens nach 30 Frames, Free sofort):

```
plan(state, timing):
  if headless/null backend                         → Legacy(T) bzw. Timer ohne Present
  if !state.foreground || state.minimized || !state.displayOn || state.locked || displayLimited
                                                    → Free
  r = timing.nominalHz; k = round(r / T)
  if k ≥ 1 && |r/k − T| ≤ 0.002·T:
      if tickLock == realtime && tearingSupported && vrrState != Rejected && |r/k − T| > 0.0001·T
                                                    → Vrr (Probing/Confirmed)   // 59,94 mit VRR → exakt 60
      return k ≤ 4 ? VblankLocked(k) : VblankDup(k)
  if tearingSupported && vrrState != Rejected       → Vrr (vrrState Probing → Confirmed/Rejected, 4.6)
  return Cadence(r / T)
```

### 4.4 Frame-Clock (Spielzeit)

**Ziel:** Jeder angezeigte Spielframe bekommt genau so viele Ticks, wie Anzeige-Slots seit dem letzten vergangen sind.
Normal ist das genau 1. Der Akkumulator von `Game::Frame` bleibt unverändert und rechnet weiter selbst.

- Größen: `P_tick = ticks(dt)` = 166 666 (wie der Gast rechnet, `trunc(QPF · dt)`). `V` = virtuelle Zeit in
  QPC-Einheiten, monoton.
- **Pro Gastframe** (in `EndFrame`, nach dem Warten, vor dem nächsten `Game::Frame`):
  - VblankLocked/Cadence: Tick-Slot i beginnt beim Refresh `a_i = ⌊i·R/T + ½⌋` seit dem Verankern (VblankLocked:
    `i·k`). Der aktuelle Refresh-Index ist `round((QPC_jetzt − QPC_Anker) / P_refresh)`; falls gültig, gleichwertig
    über `SyncRefreshCount`. `m` ist die Zahl der Slots, die seit dem letzten Frame-Start begonnen haben, mindestens 1.
    `V += m · P_tick`. Liegt ein Frame um mehr als einen Slot hinter dem Plan, wird neu verankert und als Neustart
    gezählt.
  - Vrr/Free: `m` = Anzahl der Timer-Slots, die seit dem letzten Frame verstrichen sind (Absolutplan, 1 im Normalfall).
    `V += m · P_tick`.
  - Gastframes ohne `Game::Frame` (8 leere Frames bei Kontextwechseln aus `FUN_0002A660`, Ladebild, Menü) zählen
    genauso. Der nächste `Game::Frame` sieht die Summe und kappt selbst auf 6·dt.
- **Wanduhr-Bindung:** `tick_lock = realtime` oder `|V − (QPC + Offset)| > 6 · P_tick` im Vrr- und Free-Modus →
  `Offset` neu setzen, ohne dass `V` springt. Im Modus `display` driftet `V` mit der Anzeige (3.3).
- **Wirkung** (2.6): Nach einem Kontextwechsel oder Hänger ist der Rest 4 Ticks. Danach ist jeder Frame `4 + 166 666 ≥ 166 667`, also
  genau 1 Schritt, dauerhaft. Ist der Rest 0, gibt es einmal 0 Schritte, danach steht der Rest bei 166 666 und bleibt
  stabil.
- **Einbau:** Die drei Aufrufe von `FUN_000D2B90` in `Game::Frame` (0x0A2C6A, 0x0A2FA1, 0x0A2FB6) gehen auf einen
  Host-Thunk `void __cdecl GameFrameClock(int64_t* out)`. Er ruft einmal das Original (QPF-Initialisierung von
  0x2EB448/0x2EB450) und schreibt dann `*out = V`. Damit wird auch `g_frameDelta` [0x2E7B88] exakt m·dt. Varianten:
  - (a) **Neue Patch-Art „Call-Site-Retarget“** wie fs-Retarget (Design 3.1): `E8 rel32` an verifizierter Adresse,
    Originalbytes `e8 21 ff 02 00` / `e8 ea fb 02 00` / `e8 d5 fb 02 00` werden geprüft. Das wäre eine Abweichung vom
    Design (neue Bxx-Zeile).
  - (b) Mit der vorhandenen Maschinerie: Funktions-Detour auf `FUN_000D2B90` (87 Byte), der Ersatz unterscheidet nach
    `_ReturnAddress()` ∈ {0x0A2C6F, 0x0A2FA6, 0x0A2FBB}. Alle anderen Aufrufer (0x24DD0, 0x2B440, 0x2BF50, 0xA26A0,
    0xD3120 2,5-s-Zeitlimit, 0xD48C0, 0xDCCD0) bekommen echtes QPC.
  - (c) Später (M9): `Game::Frame` als Host-Code. Dann ist die Zeitquelle ein normaler Aufruf.

  [`timing60.md`](timing60.md) 6 schlägt dieselbe Richtung vor („Lockstep-Uhr“ über einen Detour von `FUN_000D2B90`).
  Der Unterschied hier: Die Uhr zählt Anzeige-Slots statt „letzter Wert + ticks(dt)“, damit verpasste vblanks und
  Hintergrundphasen richtig gezählt werden.

  Alle anderen Aufrufer auf virtuelle Zeit umzustellen wäre riskant. `V` steht innerhalb eines Frames still, und eine
  Warteschleife auf den Timer innerhalb eines Frames hinge dann. Unter den acht Aufrufern gibt es so eine Schleife
  nicht, sie vergleichen über Frames hinweg. Sicherer ist trotzdem (a) oder (b).

### 4.5 Ablauf pro Frame (Pseudocode)

```
GpuRenderer::DisplayFrame(frame):
  NoteFrameWorkDone()
  screenshot …
  plan = pacer.PlanPresent()                    // Modus, s_n, Tearing, skip, Duplikate
  device.Present({plan.sync, plan.tearing, plan.dups, plan.skip})   // dreht den persistenten Backbuffer immer weiter
  pacer.AfterPresent(device.LastPresentTiming(), QPC)                // Statistik, VRR-Erkennung, Verifier
OnFrameDisplayed():
  pacer.EndFrame():
    events = platform::ConsumeDisplayEvents(); if (events) Replan()
    switch mode:
      VblankLocked/Cadence/Dup: ok = device.WaitForPresentSlot(2 · Planperiode)
                                if !ok: displayLimited-Zähler++ (→ Free nach 3/8)
                                [optional latency=low: Sleep bis vblank_next − (Arbeit_p95 + 1,5 ms), 4.9]
      Vrr:                      timer.WaitAbsolute(slot_n); if !device.PresentSlotReady(): WaitForPresentSlot(Periode)
      Free:                     timer.WaitAbsolute(slot_n)
    clock.Advance(m)            // 4.4
  Framezähler, --frames, CloseRequested (wie heute)
```

### 4.6 VRR-Erkennung

Public APIs sagen nicht, ob VRR aktiv ist. `ALLOW_TEARING` heißt nur, dass das Betriebssystem es zulässt. Der Pacer
misst deshalb im Zustand `Probing` (Vrr-Modus, 60 Frames, nur im Vordergrund):

| Signatur in der Frame-Statistik | Deutung | Zustand |
|---|---|---|
| ≥ 90 % der sichtbaren Abstände in 1000/T ± 1,5 ms **und** nicht innerhalb ±0,3 ms eines Vielfachen von 1000/R; SyncRefreshCount +1 je Frame | Panel folgt den Presents (hier gemessen: `legacy:50 full` 1 Refresh/Frame bei 19–21 ms; `timer:60 full` 15–18,5 ms) | `Confirmed` |
| Abstände auf Vielfachen von 1000/R (2/3-Muster) | feste Rate, zusammengesetzt oder kein VRR | `Rejected` → Cadence |
| mehrere Presents mit gleicher SyncQPCTime | Tearing ohne VRR (Independent Flip) oder Übergang (gesehen: 1,4–1,9 s in `timer:60 full`) | 1× wiederholen, dann `Rejected` |
| keine gültige Statistik | verdeckt, anderer Adapter | `Rejected` |

`Rejected` gilt bis zur nächsten Epoche (3.4). Ein Wechsel ins randlose Vollbild (Alt+Enter) erhöht die Epoche, weil
VRR und Tearing dort erst wirken.

### 4.7 Hintergrund, verdeckt, minimiert, Anzeige aus, Sperre

- **Gemessen:** Minimierte Fenster präsentieren weiter (`S_OK`, Waitable im vblank-Takt). Verdeckte Fenster ebenso,
  ihre Frame-Statistik friert aber ein. Unter VRR steuert das Vordergrundprogramm das Panel, bis hinunter auf 40,4 Hz
  (2.3).
- **Entwurf:** Free-Modus. Die Logik läuft auf dem absoluten 60-Hz-Timer in Echtzeit weiter, wie die Xbox, die bei
  ausgeschaltetem Fernseher weiterlief. Präsentiert wird nur, wenn ein Slot frei ist (`PresentSlotReady`), mit
  `Present(0)`. Minimiert, Anzeige aus und gesperrt: gar nicht, nur der Backbuffer dreht weiter. Das Waitable wird nur
  mit Timeout 0 abgefragt. Dadurch gibt es weder Leerlauf-Spinnen noch Drosselung auf fremde Raten. Die
  Frame-Statistik ist in diesem Modus ungültig und wird nicht ausgewertet.
- **Nicht gemessen** (Test T12):
  - Ob bei gesperrter Sitzung (Winlogon-Desktop) oder ausgeschalteter Anzeige `Present` blockiert, sofort zurückkehrt
    oder `DXGI_STATUS_OCCLUDED` liefert.
  - Ob `WaitForVBlank` dann weiterläuft.
  - Was ein HDMI-Hotplug beim Ausschalten des TVs auslöst. **Vermutung:** Die Ausgabe verschwindet,
    `WM_DISPLAYCHANGE` kommt, und `GetContainingOutput` scheitert.

  Der Entwurf ist davon unabhängig: Free schaltet über die Ereignisse aus 3.4 ein. Fehlt ein Ereignis, greift der
  Detektor „Anzeige bremst“ bzw. das Timeout von 2 Perioden.
- **Wirkung auf heutige Messungen:**
  - Eine Phase am VRR-Boden senkt Rate und langsamstes Fenster und erzeugt Neustarts (2.2).
  - Bei Verdeckung friert die Frame-Statistik ein, die QPC-Statistik nicht.
  - Unter dem heutigen Pacer würde eine Anzeige, die `Present` nicht mehr blockiert, die Rate **nicht** erhöhen, denn
    der Timer begrenzt. `frame_rate = 0` würde dann spinnen.
  - Prüfläufe ohne Vordergrund oder mit aus­geschalteter Anzeige sind für Kadenz-Kriterien ungültig (6).

### 4.8 Konfiguration (`[render]`, Vorschlag)

| Schlüssel | Standard | Bedeutung |
|---|---|---|
| `pacing` | `auto` | `auto` (4.3) \| `vblank` \| `cadence` \| `vrr` \| `timer` (= Free auch im Vordergrund) \| `legacy` (heutiger Pacer) |
| `tick_lock` | `display` | `display` \| `realtime` (3.3, 4.4) |
| `frame_rate` | 60 | nur `legacy`/`timer`; 0 = ungebremst (nur Benchmarks) |
| `vsync` | 1 | 0 erzwingt `timer` mit `Present(0, tearing)` |
| `max_frame_latency` | 1 | 1–3 (Waitable) |
| `latency` | `normal` | `normal` \| `low` (Just-in-time-Start, 4.9) |
| `pacing_trace` | leer | CSV-Pfad, ein Datensatz je Frame (5) |

### 4.9 Abwägungen

| Variante | Kadenz | Latenz (Present → sichtbar, gemessen) | Kosten | Risiko |
|---|---|---|---|---|
| `SyncInterval k` (VblankLocked) | exakt k Refreshes | 13,6 ms bei k = 2 (Arbeit beginnt direkt nach dem vblank); mit `latency = low` etwa Arbeit + 1,5 ms + Scanout | 1 Present je Frame | k ≤ 4; bei einem verpassten vblank steht das Bild 1 Refresh länger (gezählt) |
| `Present(1)` je vblank + Duplikate (dup) | exakt, auch unter VRR (Panel fährt 120 Hz) | 6,2–6,7 ms | k Presents und k Weckvorgänge je Frame (Present-Pass 4K etwa 0,1 ms GPU) | Spielframe muss in einen Refresh passen, sonst Halbbild-Verschiebung; mehr Thread-Wechsel |
| Timer + `Present(0, ALLOW_TEARING)` (Vrr) | mit VRR: Panel = 60 Hz, Jitter ±0,5 ms (Timer); ohne VRR: Tearing (Vollbild) bzw. Raster-Judder (Fenster) | gering | Timer | VRR-Übergänge (33–83 ms Lücken gesehen); nur mit Erkennung (4.6) |
| `Present(1)` bei R mit Kadenzmuster (Cadence) | 2/3-Judder (144 Hz: 13,9/20,8 ms), deterministisch | wie VblankLocked | 1 Present je Frame | Judder ohne VRR oder Interpolation unvermeidbar |
| Timer + `Present(1)` (heute, `legacy`) | phasenabhängig, Neustarts bei bremsender Anzeige | 2,6–5 ms | – | 2.2/2.3 |

Für die Anzeige hier (120 Hz, VRR) ist **VblankLocked(2)** die beste Wahl. Sie war in der Messung exakt, im Fenster und
im Vollbild, mit und ohne VRR. Den Latenznachteil gegenüber dup (13,6 zu 6,5 ms) gleicht `latency = low` aus, sobald
Eingabe da ist (M4). Bis dahin bleibt `normal` der Standard.

### 4.10 Abhängigkeit vom 60-Hz-Tick (Schnittstelle, nicht Teil dieses Entwurfs)

Maßgeblich ist die Patch-Tabelle in [`timing60.md`](timing60.md) 2.3, einschließlich der `.data`-Anfangswerte 0x2D6744/0x2D6748.
Die Stellen stimmen mit der eigenen Disassembly überein. Basis-Init 0xA26A0, PAL-Zweig (CApp+0xE8 = 0, bleibt so):

| Stelle | Original | 60-Hz-Wert |
|---|---|---|
| 0x0A2714 `fld [0x2955AC]` (50,0 → 0x2E7A60 per `fst` 0x0A2724, fps 0x2D6744 per `fstp` 0x0A275E) | Operand 0x2955AC (Bytes +2) | 0x2942CC (60,0, NTSC-Konstante) |
| 0x0A271A `mov [0x2E7B6C], 0x42480000` | 50,0f (Bytes +6) | 0x42700000 |
| 0x0A272A `mov [0x2E7A5C], 0x3CA3D70A` | 0,02 (Bytes +6) | 0x3C888889 |
| 0x0A2734 `fld [0x2942C0]` (0,02 → 0x2E7A58 per `fst` 0x0A274E, dt 0x2D6748 per `fstp` 0x0A2764) | Operand 0x2942C0 (Bytes +2) | 0x29E390 (1/60) |
| 0x0A273A `mov [0x2E7B68], 0x3D214285` | 1/25,4 | 0x3D064B8A (1/30,5) |
| 0x0A2744 `mov [0x2E7B70], "PAL"` | „PAL“ | bleibt |

Bei [0x2E7B68] ist der Zusammenhang mit dt unklar (1/25,4 bzw. 1/30,5). Im Dekompilat ist keine Leseweise gefunden. Eine
Tickrate, die weder 50 noch 60 ist, wird deshalb **nicht** empfohlen; der Pacer bekommt T aus [0x2D6744]. Ob alle 280
dt-Verwender und die Skripte (`*.ful`) bei 60 Hz richtig rechnen, klärt die Tick-Analyse.

---

## 5. Messgrößen, Logzeilen, Trace

Exit-Reporter „present“, neue Fassung. Zeile 1 bleibt formatgleich, damit `m3_check` sie weiter lesen kann:

```
present: 3000 frames in 50.00 s (average 60.00 Hz, slowest 1 s window 59.90 Hz, longest frame 17.1 ms)
present: pacing vblank k=2 (display \\.\DISPLAY1 120/1 Hz QDC, measured 120.0005 Hz; tick 60 Hz, lock display),
         2 mode changes (vblank 2985, free 15 frames), 0 schedule resets, 0 waits timed out
present: steps per game frame: 0:1 1:2993 2:0 3+:6 (loading), frame clock slots missed 6
present: on screen: 2984 of 2985 presents seen, duration in refreshes 2:2983 3:1, late 1, phase |p| median 0.005,
         present-to-visible median 13.6 ms p99 13.9 ms, VRR not probed
present: environment: foreground 99.5 %, minimized 0.0 s, occluded 0.0 s, display off 0.0 s, locked 0.0 s,
         display-limited 0.0 s
present: frame work 2999 frames: average 3.06 ms, 99th percentile 4.6 ms, longest 16.8 ms (0 frames longer than one
         pacing period) -> sustainable 327 Hz
```

| Größe | Quelle |
|---|---|
| Spielframe-Rate, langsamstes Fenster, längster Frame | QPC nach `Present` (wie heute) |
| Frame-Zeit-Histogramm | QPC-Abstände, 0,25-ms-Eimer; dazu sichtbare Abstände (SyncQPCTime), 1-ms-Eimer |
| Schritte je Spielframe | Aus CApp+0xC8 (`last`, int64) beim nächsten Thunk-Aufruf: `(last_neu − last_alt) / P_tick`; gekappte Frames ≥ 6 |
| vblank-Ausrichtung | Phase von SyncQPCTime gegen das Raster; Dauer in Refreshes gegen Plan `s_n`; Present → sichtbar |
| Neustarts | Frame-Clock m > 1 (verpasste Slots) und Neuverankerung des Timers |
| Umgebung | Vordergrundanteil, minimiert, verdeckt (`statsValid` = false bei sichtbarem Fenster), Anzeige aus, gesperrt, „Anzeige bremst“ |

`pacing_trace`: `frame, mode, qpc_start, qpc_work_done, qpc_present_call, qpc_present_ret, wait_ms, sync, tearing,
skip, present_id, stat_present_count, stat_sync_refresh, stat_sync_qpc, clock_m, steps, foreground`. Das Format folgt
`present_probe` (`run\analyse_pacing\src`); die Auswertungsskripte `stats_anomalies.py` und `sim_accumulator.py csv`
lassen sich weiterverwenden.

---

## 6. Testplan

**Gültigkeit:** Kadenz- und Ratenkriterien gelten nur für Läufe mit Vordergrund ≥ 95 %, eingeschalteter Anzeige und
entsperrter Sitzung (Zeile `present: environment`). Andernfalls meldet die Prüfung **UNGÜLTIG** statt FAIL. Auf
VRR-Rechnern läuft `vblank_monitor.exe` parallel; ein Abschnitt am VRR-Boden oder ein fremdes Vordergrundprogramm
macht den Lauf ungültig.

| # | Art | Aufbau | Erwartung |
|---|---|---|---|
| T1 | Unit | Refreshraten aus QDC-Rationalen (120/1, 60000/1001, 119880/1000, 143856/1000, 165/1, 75/1, 50/1) → k bzw. Kadenz | richtige Klasse; Toleranz 0,2 % |
| T2 | Unit | Kadenzfolge `s_n` für 144/165/75/100/90/240/360/50 Hz | Summe über T Frames = R; nur ⌊R/T⌋ und ⌈R/T⌉; > 4 → Duplikat; 0 → skip |
| T3 | Unit | Frame-Clock gegen die Gast-Arithmetik (C++-Nachbau von `sim_accumulator.py`): Rest 0, 4, zufällig; Jitter 0–0,5 ms; Raten aus 2.6 | genau 1 Schritt je Frame nach höchstens 1 Einschwingframe; verpasste Slots → m Schritte; Kappung 6 |
| T4 | Unit | Policy-Tabelle 4.3 (Zustand × Rate × Tearing × VRR-Zustand) | erwarteter Modus, Hysterese, Free sofort |
| T5 | Unit | VRR-Detektor mit synthetischer Statistik (fest 120, VRR 60, VRR-Boden 40,4, Tearing-Gruppen, eingefroren) | Confirmed/Rejected wie 4.6 |
| T6 | Spiel, dieser Rechner | 3000 Frames, Fenster aktiviert, keine Bedienung, `pacing = auto` | `vblank k=2`; ≥ 59,95 Hz; Schritte = 1 in ≥ 99,5 % der Frames außer Ladeframes; Dauer 2 Refreshes in ≥ 99 %; 0 Neustarts außerhalb von Ladehängern; Frame-Arbeit 99 % ≤ 8 ms |
| T7 | Spiel | wie T6, nach 10 s ein anderes Fenster nach vorn, nach 20 s zurück | Free innerhalb von 0,5 s; Spielframes 60 Hz ±0,5 % trotz VRR-Boden; danach wieder `vblank` |
| T8 | Spiel | wie T6, Alt+Enter (randloses Vollbild), auch `pacing = vrr` erzwungen | `vblank k=2` exakt; erzwungenes `vrr`: Confirmed, sichtbare Abstände 16,7 ± 1,5 ms |
| T9 | Spiel, Nutzer stellt die Desktop-Rate um (LG-TV kann 119,88/100/75/60/59,94/50) | je Rate ein Lauf; einmal Umschalten während des Laufs | 119,88 → k = 2 (lock display), 100/75 → Cadence {1,2} bzw. Vrr, 60/59,94 → k = 1, 50 → Vrr bzw. Cadence mit skip; Wechsel bemerkt (Epoche) und neuer Plan in ≤ 1 s, kein Absturz |
| T10 | Spiel | zweiter Monitor mit anderer Rate (wenn vorhanden): Fenster verschieben | Epoche, `QueryOutputTiming` des neuen Monitors, neuer Plan |
| T11 | Spiel | minimieren/wiederherstellen, verdecken | Free; 60 Hz Logik; CPU des Render-Threads < 10 % beim Minimieren; danach wieder `vblank` |
| T12 | Spiel, Nutzer handelt | (a) Win+L 30 s, entsperren; (b) TV ausschalten (HDMI-Hotplug) und an; (c) Energieoption „Bildschirm aus nach 1 min“ (vom Nutzer gesetzt) | Logik läuft mit 60 Hz weiter oder Free; kein Spinnen, kein Device-Verlust; danach Neuplanung ≤ 1 s. Das gemessene Verhalten von `Present`, Waitable, Statistik und `WaitForVBlank` wird hier nachgetragen |
| T13 | Langlauf | 30 min Frontend/Attract | Drift V gegen QPC wie erwartet (120,0005 Hz: < 0,001 %; 119,88: 0,1 %); keine Häufung von Neustarts |
| T14 | Latenz (M4) | `latency = low` gegen `normal` | Present → sichtbar ≤ Arbeit + 3 ms bei < 0,5 % verpassten vblanks |
| R | Regression | `m1_check`, `m2_check` (d3d11 und null), `m3_check` mit den Anpassungen aus [`timing60.md`](timing60.md) 7.3 (Szenen in Sekunden, M3-3-Grenzen für 60 Hz), `fut_tests` | ALL PASS; M3-3 zusätzlich mit Gültigkeitsprüfung (oben) |

---

## 7. Ausblick: mehr als 60 Hz mit Render-Interpolation

Mehr als 60 **verschiedene** Bilder pro Sekunde gibt es nur mit Interpolation, denn `Render()` bekommt keinen
Alpha-Wert (startup.md 3.2). Das Gerüst ist dafür schon günstig: `Game::Frame` rendert auch ohne Schritt, wenn
`elapsed < dt`. Gibt die Frame-Clock Zeit mit Render-Rate aus (z. B. 120 Hz, `V += P_tick/2`), hat jeder zweite Frame
0 Schritte, und die Schleife muss dafür nicht umgebaut werden.

**Was dafür gebraucht wird:**

1. **Alpha:** `α = sec(V − CApp+0xC8) / dt` nach `Game::Frame`. Das lässt sich ohne Dekompilierung bestimmen. Sauberer
   ist `Game::Frame` 0xA2C20 (1131 Byte) und `CApp::Frame` 0xD2630 als Host-Code (M9). Dann gibt es eine explizite
   Schrittgrenze und α als Parameter.
2. **Transform-Historie in `ni_extract`:** je NiAVObject-Zeiger die Welt-Transformation (Rotation, Translation,
   Skalierung, Offsets aus `ni_guest_layouts.h`) des letzten und vorletzten Schritts. Die Schrittgrenze erkennt ein
   Zähler an 0x0A2F0C (`call 0xD2B50`, einmal je Schritt) oder der Wechsel von CApp+0xC8. Beim Zeichnen wird mit
   `lerp`/`slerp` (Quaternion aus NiMatrix3) bei α interpoliert, also mit einem Tick Verzögerung. Das betrifft
   Geometrie (Slots 48–51), **Kamera** (`SetCameraData`, größter sichtbarer Gewinn) und Knochen für das CPU-Skinning
   (`FUN_0014F710`). Ohne Interpolation bleiben neue Objekte, große Sprünge (Teleport, Schnitt) und wiederverwendete
   Zeiger (Generation prüfen).
3. **Idempotentes Rendern prüfen:** `Render()` (vf3, Kontext-VTable +0xC) aller Kontexte. Die Konstruktoren sind
   Intro 0x2B440, Frontend 0x22650, Attract 0x2BA80, Ingame 0x2A1F0, Death 0x2BF50 und Intermission 0xD37C0
   (startup.md 2.6). Dazu kommen `FUN_0002C9C0`, `FUN_000D7630`, Pause-Overlay
   `FUN_000E52D0` und Debug-Overlay `FUN_000F2CF0`. Diese Funktionen dürfen keine Zustände pro Aufruf
   weiterschalten. Die `g_frameDelta`-Verwender (1.4) animieren schon mit der Render-Rate und passen.
4. **Zeitbasis der Controller:** Ob NiTimeController-Zeiten, Partikel, Morphs, UV-Animation und Sichtbarkeitsschalter
   im Update-Schritt (Spielzeit) oder im Render (`FUN_00045F10` → `FUN_00101840` mit akkumulierter `g_frameDelta`)
   fortgeschrieben werden, entscheidet, was ruckelt. Dafür müssen die Update-Aufrufe `FUN_00101840` (119 Aufrufer)
   im Kontext-`Update()` dekompiliert werden. Partikel und Vertex-Morphs bleiben vermutlich im 60-Hz-Raster.
5. **Kosten:** Frame-Arbeit etwa 3 ms (Debug) je Render. 120 Hz entsprechen etwa 36 % eines Kerns, 240 Hz etwa 72 %.
   Die Pacing-Modi aus 4.3 gelten unverändert mit Render-Rate statt T (Frame-Clock in Bruchteilen von `P_tick`).
6. **Nicht empfohlen:** eine höhere Tickrate (dt = 1/120 usw.). Die Abhängigkeiten von dt sind unbekannt
   ([0x2E7B68], Frame-Zähler, Skripte), und getestet wurden vom Entwickler nur 50 und 60 Hz.

---

## 8. Offene Punkte und Risiken

- Sperre, Anzeige aus und HDMI-Hotplug sind ungemessen (T12). Der Entwurf verlässt sich dafür auf Ereignisse und
  Timeouts.
- Semantik von `SyncInterval n` im Flip-Modell bei wechselndem n (Kadenz): Steht das **aktuelle** Bild n Refreshes,
  oder wartet es n vblanks nach dem vorigen? Visuell ergibt beides dasselbe Muster, eine Verschiebung um einen Frame.
  T2 und T9 prüfen das.
- VRR-Erkennung beruht auf einer Messheuristik. Die Treiber (NVIDIA G-SYNC im Fenstermodus, Windows-VRR, AMD) wechseln
  den Zustand je nach Vordergrund und Zusammensetzung (2.3, 2.4). Übergänge kosten 14–83 ms.
- Andere Adapter: Liegt das Fenster auf einem Monitor am iGPU- oder RTX-3050-Adapter, liefert `GetContainingOutput`
  nichts, und die Statistik fehlt vermutlich. Dann wird nur nach Timer und QDC-Nennrate geplant.
- `tick_lock = display` verlangsamt das Spiel auf /1,001-Anzeigen um 0,1 %. Wie sich das auf Lippensync in langen
  Zwischensequenzen auswirkt, ist ungeprüft.
- Die Aufteilung der 29 Neustarts von game50b auf VRR-Boden und Übergänge ist ohne Frame-Trace im Spiel offen.
  `pacing_trace` schließt diese Lücke.
- Die Frame-Clock braucht eine neue Patch-Art oder eine Rücksprungadress-Weiche (4.4). Dafür ist eine Design-Abweichung
  zu dokumentieren.

---

## 9. Nachtrag: Umsetzung, Review und Abschluss (M3b Phase 5, 2026-10-06)

Umgesetzt ist der Entwurf aus 4 (`platform\frame_pacer.*`, `pacer_policy.*`, `pacer_display.*`; Abweichungen B43/B47
in [`docs/loader_design.md`](../../docs/loader_design.md) 8.4, Bericht [`docs/m3b_60hz_report.md`](../../docs/m3b_60hz_report.md)).
Nicht umgesetzt: Kadenz-Modus, doppelte Presents für k > 4, Neuplanung aus der gemessenen Rate, `tick_lock = display`,
just-in-time-Framestart (4.9), Phasenbindung des Timers an den vblank bei nicht ganzzahligem Verhältnis.

Korrekturen aus dem Review (Befunde 5–11, 13a): sofortiger Wechsel, wenn nach „minimiert“ wieder präsentiert wird;
Kadenzwächter für `timer`/`free` bei Refresh = k·Ziel; VRR-Probe mit 2 s Aufwärmzeit und einer Wiederholung nach
10 s; langsame Frames aus den Frame-Starts; Sprünge über d ≥ 2 Presents und Regression des Refreshs in der
Bildschirmstatistik; `--set`-Werte getrimmt; Warnungen bei wirkungslosen Kombinationen; `m3_check` M3b-7.

**Zustand des Messrechners hat sich geändert.** Am 2026-10-05 (Abschnitte 2.x, Phase 3) war das Fenster komponiert:
VRR abgelehnt („visible intervals follow the refresh grid“), 50 Hz mit `timer` unregelmäßig 1–4 Refreshes pro Frame,
Present → sichtbar 21 ms bei vblank k = 2. Am 2026-10-06 lief dasselbe Fenster mit VRR (**Vermutung:** Independent
Flip auf einer Overlay-Ebene; die Ursache der Änderung ist unbekannt):
- `tick_rate = 50`, `pacing = auto`: `tearing-timer`, „VRR confirmed: the display follows the presents (100 % of the
  visible intervals at the target period)“, 50,03 Hz.
- `pacing = timer` bei 60 Hz: jedes Present bleibt **einen** verlängerten Refresh von ≈ 16,7 ms sichtbar
  (`refreshes per frame 1:1770 2:17`). Die erste Fassung des Kadenzwächters wertete das als Fehltakt, verschob das Raster
  50-mal in 30 s und senkte die Rate auf 59,62 Hz. Seitdem gilt eine Dauer, die zeitlich eine Zielperiode sichtbar war,
  als pünktlich; Verschiebungen wechseln das Vorzeichen; nach 4 erfolglosen Verschiebungen hält der Wächter an.
- `vblank k = 2` (Standard) unverändert genau 2 Refreshes; Present → sichtbar 13–14 ms statt 21 ms.
- Unter VRR ist der „gemessene Refresh“ (Regression des Refresh-Zählers) kein Maß der Anzeige: Der Zähler zählt die
  verlängerten Refreshes (119,9 Hz statt 120,001 Hz bei festem Refresh).

Damit ist T10 (VRR) im Fenster positiv geprüft; im randlosen Vollbild (Swapchain auf der 4:3-Kindansicht) und auf
anderen Anzeigen bleibt er offen.

---

## Anhang: Werkzeuge und Rohdaten (`run\analyse_pacing\`, git-ignoriert)

| Datei | Inhalt |
|---|---|
| `src\display_probe.cpp` → `out\display_probe_1.txt` | Sitzung, Anzeigestrom, GDI, QueryDisplayConfig (auch `VIRTUAL_*_AWARE`), DWM-Timing, DXGI-Ausgaben/Modi/Tearing/MPO, `WaitForVBlank`-Messung, Registry (nur lesen) |
| `src\present_probe.cpp` → `out\<modus>_<fenster>.txt/.csv` | Präsentationsvarianten (2.4); Umgebungsvariablen `PROBE_ACTIVATE`, `PROBE_DEBUG_LAYER`, `PROBE_MINIMIZE=a,b`; Fenster `WxH`, `child:WxH`, `full` |
| `src\vblank_monitor.cpp` → `out\vblank_game50b.*`, `out\vblank_game60b.*` | vblank-Abstände und Vordergrund (Klasse, Prozessname) parallel zum Spiel |
| `src\stats_anomalies.py`, `src\vblank_segments.py`, `src\phases.py` | Auswertung der CSVs |
| `src\sim_accumulator.py` → `out\sim_synth.txt`, `out\sim_csv.txt` | Akkumulator-Simulation (2.6) |
| `src\timer_sites.py` | capstone: Timer-Aufrufstellen in `Game::Frame`, Zeit-Stores der Basis-Init |
| `src\build_probe.cmd` | baut eine Sonde mit MSVC x86 (`vcvars32`) |
| `game50`, `game50b`, `game60`, `game60b`, `game0` | INI + Log (`logs\futurama.log`) der Spielläufe aus 2.2 |
