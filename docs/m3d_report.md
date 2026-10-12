# Meilenstein M3d: Bildraten bis 120 Hz mit Interpolation – Bericht

Stand: 2026-10-11 (Phase 5, nach Behebung der Review-Befunde) · Ziel-XBE: `orig\retail_eu\default.xbe` (Retail EU) · Verbindliches Design: [`docs/loader_design.md`](loader_design.md) (Abweichungen 8.9 V1–V14 und 8.10 P1–P13) · Analysen: [`analysis/loader/m3d_frameclock.md`](../analysis/loader/m3d_frameclock.md) (Frame-Clock und Takt), [`analysis/loader/m3d_interpolation.md`](../analysis/loader/m3d_interpolation.md) (Interpolation im Renderer) · Schnittstellen: [`src/INTERFACES.md`](../src/INTERFACES.md) 4.24/4.25 · Renderer-Vertrag: [`src/host/gfx/GFX_CONTRACT.md`](../src/host/gfx/GFX_CONTRACT.md) Abschnitt 13 · Vorgänger: [`docs/m3c_report.md`](m3c_report.md)

## 1. Ergebnis in Kürze

**Auf einem 120-Hz-Bildschirm zeigt der Port jetzt 120 Bilder pro Sekunde, während die Spiellogik bei 60 Hz bleibt.**

- **Vorgabe erfüllt:** mindestens 60 fps, höchstens 120 Hz; die Logik tickt weiter mit 60 Hz (tick-gebundene Abläufe laufen nicht zu schnell); jedes zweite Bild liegt zwischen zwei Logikschritten und zeigt den interpolierten Zustand der beiden letzten Ticks. An 60-Hz-Bildschirmen (auch 59,94 Hz) bleibt es bei einem Bild pro Tick ohne Interpolation.
- **Launcher-Option** „Bildrate“ im Grafik-Reiter: „60“, „120 (interpoliert)“, „Monitor (max. 120)“, mit einem Hinweis, was der gewählte Bildschirm daraus macht („Auf diesem Bildschirm (120 Hz): 120 Bilder pro Sekunde mit Zwischenbildern.“).
- **Gemessen am 120-Hz-Fernseher (Phase 3):** 119,8–119,9 Hz, 99,6–99,7 % der Bilder genau ein Refresh, Logik 59,99–60,03 Schritte/s, alpha im Wechsel ½ und 1, 99,9 % der bewegten Draws interpoliert, Zwei-Tick-Mittelpunkttest p99 0,08 px. Die Endprüfung nach den Review-Fixes lief mit **ausgeschaltetem Fernseher** (Abschnitt 9): alle anzeigeunabhängigen Kriterien bestanden, die Anzeige-Kriterien sind aus Phase 3 übernommen.
- **Latenz:** Der Just-in-time-Start (`[render] low_latency`, Standard an) senkt bei 60 fps auf 120 Hz die Zeit vom Bildstart bis zur Anzeige von 16,6 auf 6,6 ms; bei 120 fps liegt Present → sichtbar bei 1,8–1,9 ms.
- **Review:** 13 Befunde, alle nachgeprüft und bestätigt. 10 sind im Code behoben (darunter falsche Sprung-Erkennung bei Umkehrpunkten und anlaufenden Baugruppen, 59,94-Hz-Monitore, Aufhol-Frames, der Latenz-Puffer und die Zeitprobe der Uhr), 3 sind dokumentiert (Kosten, Kamera an animierten Knoten, gespiegelte Matrizen). Abschnitt 8.

## 2. Was gebaut wurde

| Teil | Inhalt | Dateien |
|---|---|---|
| Halbtakt-Uhr (Frame-Clock) | Die M3b-Lockstep-Uhr rückt die virtuelle Zeit des Spiels je Bild um ein Quant vor: bei N Bildern pro Tick um ticks(dt)/N (bei 120 fps abwechselnd 83 333/83 334 QPC-Ticks), bei ungeraden Verhältnissen um die Anzeigezeit des Bildes. Der Rest der Gast-Schrittschleife wird einmal auf „Phase B“ ausgerichtet; dann ist alpha genau ½ im Bild, das den Tick ausführt, und genau 1 im Bild dazwischen. N = 1 ist die M3b-Uhr Bit für Bit | `xapi\game_timing.*` |
| `FrameInterp` | Tick G, Schritte des Bildes, `pairTicks`, alpha, Merker (Kontextwechsel, Uhr-Reset, Aufholen, „im Schritt gezeichnet“); veröffentlicht an der Render-Probe 0xA2FBB | `xapi\game_timing.*`, `gfx\interp_frame.h` |
| Takt bis 120 Hz | `[render] frame_rate = 60 \| 120 \| display`: Bildrate aus der Bildwiederholrate (bis 120 Hz jedes Vblank, k × 120 Hz jedes k-te, sonst VRR-Timer 120, Kadenz oder gleichmäßiger Teiler); Vollbild über den ganzen Monitor (Independent Flip, VRR) mit 4:3-Bild und Balken | `platform\pacer_policy.*`, `platform\frame_pacer.*`, `platform\window.*` |
| Just-in-time-Start | Das nächste Bild beginnt `Budget` vor dem Vblank, an dem es sichtbar wird (statt direkt nach dem vorigen): Eingabe und Spielzustand werden später abgetastet | `platform\pacer_policy.*` (`LatencyBudget`, `JustInTimeStart`), `platform\frame_pacer.cpp` |
| Korrekturen für Code pro Bild | Nur aktiv, solange es Zwischenbilder gibt: F1 Bild ohne Zeichnen zeigt das vorige Bild (präsentiert), F2 Death-Render, F3 Okkluder-Blende, F4 Flackern | `xapi\frame_fixes.*`, `rhi::Device::RepeatPreviousPicture` |
| Interpolation im Renderer | Historie je Gastobjekt über die letzten zwei Ticks; starre Objekte (Lerp/Slerp), entlang der Elternkette (Stufe 1b), Kamera, Skinning über eine Ersatz-`NiSkinInstance`; Sprung-Regeln, Zerstörungs-Probe, bildgetriebene Objekte | `gfx\interp_history.*`, `gfx\interp_math.h`, `gfx\nipc_gpu.cpp` |
| Launcher | Bildrate im Grafik-Reiter mit Hinweis je Bildschirm, „Geringe Verzögerung“ und „Zwischenbilder berechnen“ im Reiter Erweitert | `launcher\src\core\frame_rate.*`, `launcher\src\ui\settings_tab.cpp` |
| Prüfung | `tools\m3d_check.py` (9 Varianten, Kriterien G1–G3, D1–D9, F1, R1, R2, Z1, Z2, L1), `tools\foreground_hold.py` | `tools\` |
| Review-Fixes (Phase 5, dieser Bericht) | Abschnitt 8 | siehe `src\INTERFACES.md` Changelog |

## 3. Aufbau

### 3.1 Frame-Clock und alpha

`Game::Frame` (0xA2C20) arbeitet feste Schritte ab (`while sec(elapsed) >= dt`), speichert `last = jetzt − Rest` in `CApp+0xC8` und rendert danach einmal. Der Loader ersetzt die drei Zeitproben dieser Funktion durch die virtuelle Zeit V der Frame-Clock (seit M3b). In M3d rückt V je Bild um ein Quant vor, das der Pacer vorgibt (`platform::CurrentFrameAdvance`):

- **N Bilder pro Tick** (Bildrate = N × Tickrate innerhalb 100 ppm): Quant = ticks(dt)/N. Bei 120 fps entsteht das Schrittmuster 1, 0, 1, 0, …
- **Anzeigezeit** (75/100/119,88 Hz, Kadenz-Plan, 72 fps auf 144 Hz): Quant = Anzeigezeit des Bildes in QPC-Ticks mit Bruchteil-Übertrag.
- **Bildrate bis zur Tickrate** (50, 59,94, 60, 60,02 Hz): N = 1, die M3b-Uhr ohne Interpolation (seit dem Review, Befund 8).
- Korrekturen wie in M3b: liegt die Echtzeit mehr als ¾ Quant vor V, holt V in ganzen Quanten auf; liegt sie mehr als ¾ Quant dahinter, hält V ein Bild an.
- **Phase B:** Einmal (im zweiten Bild, nach der 6-dt-Kappung des Gastes und nach einem Regimewechsel) wird V so verschoben, dass der größte Rest der Gast-Schleife ticks(dt) − μ ist (μ = max(16, ticks(dt)/1024)). Dann gilt `alpha = clamp(1 − (L − Rest)/ticks(dt), 0, 1)` mit L = ticks(dt) − μ exakt ½ im Tick-Bild und 1 im Bild dazwischen: Die angezeigte Zeit `G − 1 + alpha` steigt je Bild um genau ½ Tick, ein Tick zeigt die Hälfte seiner Wirkung im eigenen Bild.
- **Eingang der Uhr** (seit dem Review, Befund 11): In den Zwischenbild-Regimes vergleicht die Uhr den Bildstart des Pacers statt der Gast-Probe, die erst nach den Streaming-Pumpen kommt; ein Planwechsel (Hintergrund ↔ Vblank) wird einmal aufgefangen.

Das Ergebnis ist `FrameInterp {tick, stepsThisFrame, pairTicks, alpha, flags}`. Der Renderer zeigt `interp(S[G−1], S[G], alpha)`; nach einem Aufholen um k Ticks (2–6) `interp(S[G−k], S[G], (k − 1 + alpha)/k)`.

### 3.2 Interpolation im Renderer

- **Schlüssel und Historie:** Jeder Draw der Slots 48–51 hat den Schlüssel (Objekt = `NiTransform* − 0x70`, Kamera, Vorkommen im Bild) und eine Signatur (Vtable, Daten, Skin, Property-State). Zwei Tabellen (Generation G und G−1) mit Generationsstempel; interpoliert wird nur von genau G−1 (bzw. vom Paar G−k).
- **Starre Objekte:** Translation linear, Rotation per Quaternion-Slerp, Skalierung linear; schnelle Wege für alpha 1 und unbewegte Objekte (Gastzeiger unverändert).
- **Stufe 1b, Elternkette** (`[render] interpolation_hierarchy`, Standard an): `W(alpha) = W_Eltern(alpha) · interp(L_alt, L_neu, alpha)` mit `L = W_Eltern⁻¹ · W`; der oberste Knoten mit Historie im Weltraum. Teile an drehenden Knoten folgen dem Bogen statt der Sehne (Mittelpunkttest am Einflug: p99 0,08 px statt 12,5 px).
- **Kamera:** Position linear, Zeilen dir/up/right per Slerp, Frustum-Tangenten linear; Schnittregel.
- **Skinning:** relative Knochenposen `Wurzel⁻¹ · Knochen` je Knochen interpoliert; das Original `FUN_0014F710` verformt mit einer Ersatz-`NiSkinInstance` im Arena-Speicher (Selbsttest 4/4 mit Differenz 0).
- **Einrasten statt Interpolieren:** erste Sichtung, Lücke in den Generationen, andere Signatur, zerstörtes Objekt (`probe:NiAVObject::Destroy`), Sprung-Regeln R2/R4 (Schritt > 2 Radien und > 4 × Bezug; Drehung > 45° und > 4 × Bezug + 1°), Skalierung außerhalb [½, 2], Spiegelung, nicht orthonormal, bildgetriebene Objekte (ändern sich innerhalb einer Generation; 120 Bilder lang wie gegeben), Kontextwechsel.
- **Seit dem Review (Befund 1):** Bezug der Sprung-Regel ist die größere der letzten beiden bekannten Bewegungen; ohne bekannte Vorbewegung rastet nur ein Schritt über 16 Radien bzw. eine Drehung über 90° ein; mit Elternkette entscheidet die Kette (nur ein Knoten, dessen eigene lokale Transformation springt, rastet ein – mit seinen Nachfahren gemeinsam).
- **Bleibt im 60-Hz-Raster (gewollt):** Bildschirm-Polygone und -Texturen (HUD, Menüs, Intro), UV-/Alpha-/Daumenkino-Animationen, Lichter, Partikel (Slot 52 zeichnet der Port noch nicht).

### 3.3 Takt bis 120 Hz

| Bildschirm | „60“ | „120 (interpoliert)“ | „Monitor (max. 120)“ |
|---|---|---|---|
| 50 / 59,94 / 60 Hz | ein Bild pro Refresh | dasselbe, ohne Interpolation (M3b-Uhr) | wie links |
| 75 / 100 Hz | M3b | 75 / 100 fps, interpoliert | wie links |
| 120 Hz | Vblank k = 2 (60 fps) | **Vblank k = 1, 120 fps** | wie links |
| 119,88 Hz | k = 2 | k = 1, 119,88 fps, Uhr in Anzeigezeit | wie links |
| 144 / 165 Hz mit VRR | Timer 60 | Tearing-Timer 120, das Panel folgt | wie links |
| 144 / 165 Hz ohne VRR | M3b | Kadenz 1/2 Refreshes, im Mittel 120 | gleichmäßig 72 / 82,5 fps |
| 240 Hz | k = 4 | k = 2, 120 fps | wie links |

Hintergrund, minimiert, Anzeige aus: der Timer („free“) mit der gleichmäßigen Rate, ohne Present.

## 4. Benutzung

### 4.1 Für Spieler (Launcher)

1. `FuturamaLauncher.exe` starten, Reiter **Grafik**.
2. **Bildrate:** „60“ (wie bisher), „120 (interpoliert)“ oder „Monitor (max. 120)“. Darunter steht, was der gewählte Bildschirm daraus macht, z. B. „Auf diesem Bildschirm (120 Hz): 120 Bilder pro Sekunde mit Zwischenbildern.“ oder bei 144 Hz „mit VRR 120 Bilder pro Sekunde mit Zwischenbildern, ohne VRR 72 Bilder pro Sekunde.“
3. Für 120 Hz im Vollbild: „Vollbild“ einschalten (randlos, bedeckt den ganzen Monitor, das 4:3-Bild sitzt mittig mit schwarzen Balken; die Fenstergröße ist dann ausgegraut).
4. Reiter **Erweitert:** „Geringe Verzögerung“ (Standard an) und „Zwischenbilder berechnen“ (Standard an; aus = jedes Logikbild zweimal).

Der Standard der Bildrate bleibt „60“ (V13); die Umstellung auf „Monitor“ ist eine Entscheidung des Nutzers nach der Abnahme.

### 4.2 Konfiguration (`futurama.ini`)

| Schlüssel | Werte | Bedeutung |
|---|---|---|
| `[render] frame_rate` | `60` (Standard = `[game] tick_rate`), `120`, `display`; Entwickler: `50`, `0..1000` | Bildrate (3.3) |
| `[render] low_latency` | `on` (Standard) / `off` | Just-in-time-Start |
| `[render] interpolation` | `on` (Standard) / `off` | Zwischenbilder berechnen; `off` zeigt bei 120 fps jedes Logikbild zweimal |
| `[render] interpolation_hierarchy` | `on` (Standard) / `off` (Entwickler) | Stufe 1b, Interpolation entlang der Elternkette |
| `[render] interpolation_debug` | `off`, `tint`, `trace` (Entwickler) | `tint` färbt eingerastete Objekte (rot Sprung/Lücke, gelb erste Sichtung, cyan bildgetrieben, magenta Kontextwechsel); `trace` schreibt `logs\interp_trace.csv` und für die Bilder aus `[render] trace_frames` `interp_draws.csv`/`interp_nodes.csv` |
| `[render] pacing_trace` | Pfad | Takt-Trace je Bild (Spalten `frame_ticks`, `repeat`, `refreshes`, `jit_ms`) |
| `[game] frame_clock` | `lockstep` (Standard) / `realtime` | Uhr des Spiels (M3b) |

Logzeilen am Ende: `game clock M3d: …` (Regime, alpha-Verteilung, Kontextwechsel/Aufholen/Kappung, Eingang der Uhr), `present: frame rate …` (Plan, wiederholte Bilder, Just-in-time mit späten Bildern und fehlgeschlagenen Entspannungen), `interpolation: …` (vier Zeilen: Draws und Gründe, Kameras und Skins, Generationen/Kosten, Hierarchie).

### 4.3 Für Entwickler

```
cmd /c "D:\FuturamaDecomp\build.cmd" build\m3d-fix Debug                          (FUT_CMAKE_ARGS=-DFUT_WERROR=ON beim ersten Mal)
cmd /c "D:\FuturamaDecomp\build.cmd" build\m3d-fix-rel RelWithDebInfo
cmd /c "D:\FuturamaDecomp\build.cmd" build\m3d-fix-clang Debug all clang
cmd /c "D:\FuturamaDecomp\build_launcher.cmd" build\m3d-fix-launcher-rel Release test   (LAUNCHER_CMAKE_ARGS=-DLAUNCHER_WERROR=ON)
python tools\package.py --game-build build\m3d-fix-rel --launcher-build build\m3d-fix-launcher-rel
python tools\foreground_hold.py -- python tools\m3d_check.py --exe build\m3d-fix-rel\bin\futurama.exe --config run\m3d_final\base\futurama.ini --fresh
python tools\m3d_check.py ... --display-optional        (ohne eingeschalteten 120-Hz-Bildschirm: Anzeige-Kriterien SKIP)
python tools\timing_compare.py --run <Lauf 60 fps> --run <Lauf 120 fps>
```

Die Anzeige-Kriterien (D1, F1, L1, M3b-7) brauchen einen eingeschalteten Bildschirm mit mindestens 120 Hz, das Spielfenster im Vordergrund (`foreground_hold.py`) und einen unbenutzten Rechner.

## 5. Messwerte

### 5.1 Anzeige am 120-Hz-Fernseher (Phase 3, `run\m3d_verify`, Fenster vorn)

Diese Werte stammen aus der Endprüfung von Phase 3 (LG-Fernseher 3840×2160 bei 120 Hz, RTX 5090). Die Review-Fixes ändern den Präsentationsweg nicht (F1, Pläne, Waitable, Present); sie ändern den Eingang der Uhr, die Entspannung des Latenz-Budgets und das Verhalten unter 60 Hz. In Phase 5 war der Fernseher aus (Abschnitt 9), daher sind diese Zahlen nicht neu gemessen.

| Messgröße | i120 („120“, Fenster) | d120 („Monitor“, Fenster) | f120 (randloses Vollbild) |
|---|---|---|---|
| Plan | vblank k = 1 (99,2 % der Vordergrund-Frames) | vblank k = 1 | vblank k = 1 (100 %) |
| Dargestellte Rate | 119,87 Hz | 119,80 Hz | 119,90 Hz |
| Bilder genau 1 Refresh | 99,61 % (3562 Bilder) | 99,70 % (2662) | 99,74 % (2696) |
| Present → sichtbar (Median) | 1,9 ms | 1,8 ms | 1,8 ms |
| D3D11-Debug-Layer | 0 Meldungen | 0 | 0 |

60 fps auf 120 Hz (c60/ll60off): vblank k = 2, 99,49–99,73 % genau 2 Refreshes mit, 99,87 % ohne Just-in-time-Start; Bildstart → sichtbar Median 6,6 ms mit gegen 16,6 ms ohne, Present → sichtbar 3,9 gegen 14,0 ms.

### 5.2 Logik, Uhr und Interpolation (Phase 5, `run\m3d_final\base\m3d_check`, RelWithDebInfo)

| Messgröße | Wert |
|---|---|
| Logik bei 120 fps | 60,00 Schritte/s (i120: 3585 Frames, 0 Schritte 49,87 %, 1 Schritt 50,01 %, 2+ 0,03 %) |
| alpha | ½ × 1795, 1 × 1790, sonst 1 (der erste Frame); 3 Ereignisse (Kontextwechsel, Uhr-Reset, 1 Aufholpaar) = 0,08 % |
| Angezeigte Zeit (t120, `interp_trace.csv`) | + ½ Tick in 99,94 % der Frames, nie rückwärts, kein Frame ohne Schritt zeigt einen früheren Zustand |
| Eingang der Uhr | Bildstart des Pacers in allen 3586 Zwischenbild-Samples |
| Bewegte Draws interpoliert | 99,91 % (i120: 130 613 von 130 732); Rest: 27 Verschiebungs- und 92 Drehsprünge (Schleifensprünge des Einflugs; die Drehsprünge zählen jetzt jeden Nachfahren eines gesprungenen Knotens), 0 Identität / Spiegel / nicht orthonormal / nicht erfasst / Überlauf |
| Urteile der Elternkette gegen die Blattregel | 91 Draws in i120 (vorher als Sprung behandelt oder umgekehrt) |
| Skins | Selbsttest 4/4, Differenz 0; 2271 Ersatz-Skins, 0 Knochensprünge |
| Transformationen (t120x) | Frames mit alpha 1 exakt S[G]; Welt-Draws exakt der Lerp; 21 426–21 900 Hierarchie-Draws gegen eine unabhängige Python-Referenz max. 8,7·10⁻⁷ relativ; alle zwischen ihren Nachbarn |
| Zwei-Tick-Mittelpunkttest | Hierarchie p99 0,079 px, max 17,5 px; nur Weltraum p99 12,4 px, max 108,6 px (640×480) |
| Bildtripel (t120 / t120off) | Verhältnis d01/(d01+d12) Median 0,636 mit, 1,000 ohne Interpolation; 100 % zwischen 0,15 und 0,85 |
| Frames ohne Schritt | alle 147 haben ihr bitgleiches Gegenstück im Lauf ohne Interpolation |
| 60 fps | M3b-Uhr, 0 aufgezeichnete Frames, 0 wiederholte Bilder; c60off (Interpolation aus, Zerstörungs-Probe entfernt) bitgleich zu c60 und ll60off |

### 5.3 Kosten

| Messgröße | Wert |
|---|---|
| CPU-Zeit der Interpolation je Tick-Frame (t120, 346 Draws, etwa 800 Knoten) | Median 194 µs, p99 369 µs |
| … je Frame ohne Schritt | Median 39 µs, p99 92 µs |
| Mittel über alle aufgezeichneten Frames (i120) | 114 µs, p99 294 µs, max 2,1 ms (erster Frame nach dem Kontextwechsel) |
| Ohne Elternkette (Review-Lauf `cost_flat`) | Tick-Frames Median 63 µs, p99 125 µs |
| Speicher (Review-Lauf `long120`, 40 000 Frames) | Tabellenspitze 352 Einträge, Knotenspitze 865 (Kapazität 4096, nie gewachsen); privater Speicher wie ohne Interpolation |
| GPU (Phase 3) | „Maximum“ bei 120 fps: GPU 1,24 / 2,8 ms (Median / p99), 99,94 % genau 1 Refresh |

Die Analyse hatte 0,05 ms + < 0,1 ms je Tick-Frame bei 2000 Draws geschätzt. Gemessen sind 0,55 µs je Draw, bei 2000 Draws also etwa 1 ms (12 % des 8,33-ms-Budgets). Für das Frontend ist das unkritisch; eine Kostenprüfung mit etwa 2000 Draws gehört zu M8 (Review-Befund 2).

### 5.4 Dauer der Abläufe (`tools\timing_compare.py`, Review-Läufe `dur60`/`dur120`)

Die Periode der Dampfwolke im Frontend misst jetzt per Autokorrelation 0,999 s bei 60 fps und 1,003 s bei 120 fps (vorher 1,103 s bei 120 fps, ein Messartefakt der Stufenübergänge). Das bildgebundene Ladebild endet bei 120 fps früher (12,3 statt 13,6 s, Analyse 1.3; in `m3d_check` als Szenenzeit berücksichtigt).

## 6. Screenshots und Bildfolgen

- **Bildfolgen (angesehen):** `run\m3d_final\base\m3d_check\t120\sheets\sequence.png` (Einflug, Frames 1680–1691 mit Interpolation) gegen `…\t120off\sheets\sequence.png` (ohne): Mit Interpolation ändert sich jedes Bild, die Buchstaben des Logos laufen mit Zwischenständen ein; ohne Interpolation sind je zwei Bilder gleich. Keine Doppelbilder, kein Springen; Blitze, die Oszilloskop-Welle und das Einblenden springen im 60-Hz-Raster (gewollt). `…\t120\sheets\triplets_top.png`: das mittlere Bild jedes Tripels liegt zwischen seinen Nachbarn, die Differenzbilder zeigen nur die bewegten Buchstaben, die Welle und Blitze.
- **Launcher:** `run\m3d_final\shots\windisplay\de_tab_graphics_full.png` – Bildrate „Monitor (max. 120)“ mit „Auf diesem Bildschirm (120 Hz): 120 Bilder pro Sekunde mit Zwischenbildern.“ (englisch `en_tab_graphics_full.png`). Spiel-Screenshots liegen nur unter `run\` (nicht im Repository).

## 7. Prüfungen

| Prüfung | Build / Lauf | Ergebnis |
|---|---|---|
| Bauen | Spiel MSVC Debug `build\m3d-fix`, RelWithDebInfo `build\m3d-fix-rel`, clang-cl Debug `build\m3d-fix-clang` mit `FUT_WERROR=ON`; Launcher Debug/Release `build\m3d-fix-launcher(-rel)` mit `LAUNCHER_WERROR=ON`; `gen_all.py --check` | ohne Warnungen; generierte Dateien aktuell |
| Unit-Tests | `fut_tests` in allen drei Builds; Launcher-ctest | **318/318** (1 übersprungener Handtest), 1 421 356 Assertions; 4/4 in Debug und Release. Neu: 7 Interpolations-Tests (S10, S1, S2, S6, S11, Paare, Seiten-Cache), Latenz-Rückfall, Planwechsel der Uhr, Bildraten ≤ Tickrate |
| `m3d_check` | RelWithDebInfo, 9 Varianten, `--display-optional` (Fernseher aus) | Alle anzeigeunabhängigen Kriterien **PASS** (G1–G3, D2–D9, R1, R2, Z1, Z2); D1, F1, L1, M3b-7 **SKIP** (Anzeige aus: kein Present). `run\m3d_final\checks_m3d_rel.txt`, `checks_m3d_rel_motion.txt` |
| `m1_check` / `m2_check` | Debug 3 + RelWithDebInfo 2 Läufe; d3d11 und Null-Renderer | **ALL PASS**; 59,999 Schritte/s, 99,73–99,80 % der Frames genau ein Schritt |
| `m3_check` (klassisch) | Debug, RelWithDebInfo (Debug-Layer an), clang-cl | M3-1 bis M3-4, M3b-6, M2 **PASS**; xemu Legal 0,00, Tech-Notice 0,00, Ladebild 0,30, Frontend 0,01; M3b-7 FAIL nur wegen der ausgeschalteten Anzeige (keine Bildschirm-Statistik) |
| `m3c_check` (7 Varianten) | Debug und RelWithDebInfo | **ALL PASS**; X1 Legal 0,73, Tech-Notice 0,18, Ladebild 0,60–0,61, Frontend 0,99; X2 0,44/0,46 |
| Paket | `python tools\package.py --game-build build\m3d-fix-rel --launcher-build build\m3d-fix-launcher-rel` | `dist\FuturamaPC\`, 24 Dateien, 29,8 MB |

## 8. Review-Befunde

Alle 13 Befunde sind nachgeprüft (Code gelesen, Szenarien des Reviews als Unit-Tests nachgestellt) und bestätigt. Details und Belege: `loader_design.md` 8.10 (P1–P13).

| # | Schwere | Befund | Entscheidung | Umsetzung |
|---|---|---|---|---|
| 1 | mittel | Sprung-Regel R2 je Blatt im Weltraum mit nur einem Tick Gedächtnis: falsche Sprünge am Umkehrpunkt, im zweiten Tick nach der ersten Sichtung und bei kleinen Teilen einer anlaufenden Baugruppe | **behoben** | Bezug = größere der letzten beiden Bewegungen; unbekannte Vorbewegung: nur > 16 Radien / > 90°; mit Elternkette entscheidet die Kette. Translation-Sprünge im Einflug 125 → 27; Tests S10, S1, S2 |
| 2 | niedrig | Kosten deutlich über der Schätzung (Hierarchie ≈ ⅔) | **dokumentiert** | Messwerte in GFX_CONTRACT 13 und 5.3; Kostenprüfung mit etwa 2000 Draws in M8; keine Optimierung in M3d |
| 3 | niedrig | Wiederverwendeter Zeiger im Zwischenframe = 120 Frames bildgetrieben | **behoben** | Wiederaufnahme prüft die Zerstörungsliste; `kAfterDestroy`; Test S6 |
| 4 | niedrig | Kamera ohne Zerstörungsprüfung; Schnitt unter den Schwellen wird gemischt | **behoben (Zerstörung)**, Schwellen in den Messplan M4b/M8 | `CameraEntry::destroyedCheckFrom`; Test S11 |
| 5 | niedrig | Kamera im Weltraum, Objekte entlang der Kette: Geometrie am drehenden Kameraknoten verschiebt sich | **dokumentiert** | Bekannte Einschränkung mit „Kameras an animierten Knoten“; Messung mit M4b/M8 |
| 6 | niedrig | Cache lesbarer Seiten wird nie ungültig | **behoben** | Release-Epoche `guest::MemoryReleaseEpoch()` aus `kernel\mm.cpp` |
| 7 | niedrig | Gespiegelte Transformationen rasten immer ein | **dokumentiert, keine Änderung** | Die Disc hat keine; Zähler „mirror“ bleibt Auslöser |
| 8 | mittel | „120“/„Monitor“ an 50/59,94-Hz-Bildschirmen: Anzeigezeit-Quanten mit 2-Schritt-Frames, Sprung plus Standbild | **behoben** | Bildrate ≤ Tickrate × 1,002 → M3b-Uhr ohne Interpolation; Launcher, Hilfetext, Analyse 2.7/3.2 korrigiert |
| 9 | mittel | Just-in-time-Budget entspannt sich alle 2 s und erzeugt periodisch späte Frames | **behoben** | Rückfall: Intervall verdoppelt nach einem Fehlschlag, nach 2 Fehlschlägen hält der Faktor bis zum Planwechsel; Exitzeile „K failed relax steps“ |
| 10 | niedrig | Nach Aufholen/Kappung ein Standbild, `pairTicks` nicht weitergegeben | **behoben** | Interpolation über das Paar der k Ticks; kein `snapAll` mehr für Aufholen und Kappung |
| 11 | niedrig | Uhr vergleicht die Probe nach den Streaming-Pumpen; Phasensprung bei free → vblank | **behoben** | Bildstart des Pacers als Eingang (Zwischenbild-Regimes), Planwechsel einmal aufgefangen |
| 12 | niedrig | Beschriftung weicht von der Vorgabe ab | **behoben** | „60“, „120 (interpoliert)“, „Monitor (max. 120)“ |
| 13 | niedrig | Dampfwolken-Periode bei 120 fps 1,103 s (Messartefakt) | **behoben** | Autokorrelation: 0,999 / 1,003 s |

Zusätzlich in Phase 5 gefunden und behoben: Bei ausgeschalteter Anzeige (ohne Present) zeigte ein Zwischenframe ohne Zeichnen im Screenshot den Puffer von vor zwei Frames (schwarz im Intro); F1 behält das vorige Bild jetzt auch ohne Present (`frame_pacer.cpp`). Und `m3d_check` R2 wertet das erste Bild eines Screenshot-Fensters nicht, wenn sein Gegenstück wegen der Lauf-zu-Lauf-Verschiebung vor dem Fenster des Vergleichslaufs liegt.

## 9. Bekannte Probleme und offene Punkte

- **Anzeige-Messung nach den Review-Fixes fehlt:** Der Fernseher war während der Endprüfung von Phase 5 aus (`window: display off`, Plan „free“ ohne Present; eine Energie-Anforderung in `foreground_hold.py` hält die Anzeige nur an, schaltet sie aber nicht ein). D1, F1, L1 und M3b-7 sind deshalb aus Phase 3 übernommen. Mit eingeschaltetem 120-Hz-Fernseher erneut laufen lassen: `python tools\foreground_hold.py -- python tools\m3d_check.py --exe build\m3d-fix-rel\bin\futurama.exe --config run\m3d_final\base\futurama.ini --fresh` und `m3_check` ohne die `--min-*`-Schalter. Betroffen könnten sein: der neue Eingang der Uhr (Bildstart statt Probe) auf dem Vblank-Plan und der Rückfall des Latenz-Budgets.
- **Ingame nicht erreichbar** (ohne Eingabe; der Attract-Modus ist ein Film): Kamera-Interpolation und Schnittregel, bildgetriebene Objekte, Skins mit vielen Knochen, Mehrfach-Draws, F2–F4 und die Kosten bei etwa 2000 Draws sind nur durch Unit-Tests belegt (M4/M8).
- **Kamera nur im Weltraum** (Befund 5): Kameras an animierten Knoten (Zwischensequenzen, Fahrzeuge) können Sehnenfehler zeigen; Geometrie am selben Knoten verschiebt sich um bis zu 10 px bei 20°/Tick.
- **Schnitte unter den Schwellen** (0,5 Einheiten, 20°) werden gemischt (Review S4); Messplan M4b/M8, eventuell eine Winkelschwelle relativ zum Sichtfeld.
- **Andere Bildwiederholraten** (144/165/180/240 Hz, 119,88 Hz), der Kadenz-Plan und VRR mit dem Tearing-Timer sind nur simuliert und durch Unit-Tests belegt.
- **Datenanimationen im 60-Hz-Raster** (UV, Alpha, Daumenkino, Blitze, Einblenden) laufen in Tick-Frames einen halben Tick vor der interpolierten Geometrie; im Frontend unauffällig.
- **Gespiegelte Matrizen** rasten ein (Befund 7); **Kosten** etwa 0,55 µs je Draw (Befund 2).
- **Doppel-Render-Test** (m3d_frameclock.md 4.4) und F5 (Schwarzbilder nach Intermission) fehlen (M4/M8).
- **Standard der Bildrate** bleibt „60“; die Umstellung auf „Monitor“ entscheidet der Nutzer.
- Kleinigkeit: Im Reiter Erweitert stehen „Geringe Verzögerung“ und „Zwischenbilder berechnen“ zwischen technischen Schlüsseln.

## 10. Nächste Schritte

1. **M4 – Eingabe:** XInput, Tastatur und Maus mit frei belegbaren Tasten, Rumble; neuer Reiter „Steuerung“ im Launcher (aus dem Schema wie die übrigen Reiter). Damit werden Ingame-Szenen erreichbar, und die offenen M3d-Punkte lassen sich messen: Kamera-Interpolation und Schnitte, bildgetriebene Objekte, Skins mit vielen Knochen, Kosten bei etwa 2000 Draws, der Just-in-time-Start mit Eingabe.
2. **M4b – Freecam:** frei bewegliche Kamera (F6, Maus/WASD, Gamepad), Überschreiben der Weltmatrix der aktiven `NiCamera` vor dem Culling (`NiCamera::Click` 0x100C50). Sie liefert die Messszenen für die Kamera-Interpolation, die Schnittregel und Kameras an animierten Knoten (Befunde 4 und 5).
3. Vor dem Umstellen des Standards auf „Monitor“: die Anzeige-Kriterien mit eingeschaltetem Fernseher wiederholen (Abschnitt 9) und, wenn möglich, an einem 144/165-Hz-Bildschirm messen.
