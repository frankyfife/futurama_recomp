# Meilenstein M3: Bericht

Stand: 2026-10-05 (Phase 5, nach Behebung der Review-Befunde) · Ziel-XBE: `orig\retail_eu\default.xbe` (Retail EU) · Verbindliches Design: [`docs/loader_design.md`](loader_design.md) (E1–E12, Abweichungen in Abschnitt 8.3) · Grafikvertrag: [`src/host/gfx/GFX_CONTRACT.md`](../src/host/gfx/GFX_CONTRACT.md) · Vorgänger: [`docs/m2_report.md`](m2_report.md)

## 1. Ergebnis in Kürze

**Das Spiel zeigt Bild.** `NiPCRenderer` ersetzt `NiXBoxRenderer` an der NiRenderer-Schnittstelle und zeichnet über `ni_extract` → `ff_pipeline` → RHI mit einem D3D11-Backend. Legal-Bild, Tech-Notice „NDL graphics technology“ (`NI_Splash_PAL.nif`, das „Splash“ des Designs), Ladebild und das animierte Frontend-Menü erscheinen vollständig. Grundlage ist ein Fenster mit Swapchain, VSync und dem 50-Hz-Takt des PAL-VBlanks. Ton, Filme und Eingabe fehlen noch (M4–M6).

```
NiXBoxRenderer::Create 0x14C170 → NiPCRenderer + GpuRenderer (Fenster, D3D11-Device, Fixed-Function-Pipeline)
  Frame: 17 ClearBuffer → 45 BeginScene → 47 SetCameraData → 48 RenderShape / 54 / 55 … → 46 EndScene
         → 16 DisplayFrame (Screenshot, Present-Pass in die Swapchain, Present mit VSync, 50-Hz-Takt)
  Texturen: 65 CreateSourceTextureRendererData = FUN_0015E010 (Formatwahl, texconv, Upload, Pixelpuffer frei)
```

Alle drei Exit-Kriterien aus Design-Abschnitt 6 (M3) sind erfüllt (Abschnitt 4). Grundlage sind `tools\m3_check.py` in drei Builds (MSVC Debug, MSVC RelWithDebInfo mit Debug-Layer, clang-cl Debug) und der Vergleich mit Aufnahmen des Originals in xemu. Legal-Bild und Tech-Notice sind texelgenau gleich. Im Frontend sind 98,0–98,8 % der Pixel, die in allen 81 xemu-Aufnahmen statisch sind, exakt gleich. Der D3D11-Debug-Layer meldet 0 Fehler und 0 Warnungen. 3000 Frames laufen mit 50,01 Hz und Exitcode 0; die Arbeit je Frame trägt 326–445 Hz. `m1_check` und `m2_check` bestehen weiter, auch mit `[render] backend = null`.

Von den elf Review-Befunden sind alle angenommen und behoben (Abschnitt 8). Zwei Teilaussagen sind abgelehnt, weil sie nicht zutreffen (Befunde 9 und 10). Die wichtigsten Korrekturen:
- Slot 65 gibt den Pixelpuffer frei wie das Original. Der Spielpool liegt damit am Ende eines Laufs 2,4 MiB tiefer.
- Zwei persistente Backbuffer bilden den Xbox-Flip nach. Frames ohne Draw zeigen so ein definiertes Bild.
- Transformierte Vertizes rasten auf 1/16 Pixel. Das hebt die Frontend-Übereinstimmung von 93 % auf 98 %.
- Die Dampfwolke im Frontend galt als ungeklärte Abweichung. Sie ist auch im Original sichtbar; die alten xemu-Aufnahmen hatten nur stets dieselbe Phase getroffen.

## 2. Was gebaut wurde

### 2.1 Struktur (neu oder erweitert in M3)

```
D:\FuturamaDecomp\
  src\host\
    gfx\rhi\            RHI-Schnittstelle (rhi.h) + D3D11-Backend (Device, Context, Ressourcen, Debug-Layer,
                        persistente Backbuffer, Present-Pass, eingebaute Shader)                  ~3 750 Zeilen
    gfx\ni_extract.*    NetImmerse-Objekte → DrawItem: Kamera, Property→State (FUN_00159CD0), Licht
                        (FUN_0015BA40/FUN_0015B6A0), Geometrie, CPU-Skinning (FUN_0014F710)     ~3 300 Zeilen
    gfx\ff_pipeline.*   D3D8-Fixed-Function auf der RHI, Ubershader (5 Permutationen, fxc zur Bauzeit) ~1 850 Zeilen
    gfx\texconv.*       NiPixelData → BGRA8/BC1–3 (Bitreplikation, Paletten, Formatwahl FUN_001572D0)  ~1 200 Zeilen
    gfx\nipc_*          NiPCRenderer (Gastseite) + GpuRenderer (Hostseite, nipc_gpu.*)             ~3 000 Zeilen
    platform\           Fenster (4:3-Ansicht, DPI, Alt+Enter, F12), Frame-Takt + Statistik, PNG-Screenshots
                                                                                                  ~1 200 Zeilen
  tests\                8 neue bzw. erweiterte Testdateien für M3 (rhi_d3d11, platform, extract, ff_pipeline,
                        ff_render, texconv, nipc_gpu, gfx_contract), insgesamt 191 Testfälle      ~5 800 Zeilen M3
  tools\m3_check.py     M3-Exit-Prüfung (importiert m2_check: alle M2-Kriterien laufen mit)
  tools\xemu_capture.py Referenzaufnahmen des Originals in xemu (neu: --start)
```

Entstanden ist M3 in fünf Phasen: Schnittstellen (Phase 1), vier parallele Agenten für Backend, Extraktion, texconv und Shader (Phase 2), Integration (Phase 3), Review (Phase 4) und Review-Fixes (Phase 5, dieser Bericht).

### 2.2 Komponenten

| Komponente | Umsetzung |
|---|---|
| Fenster (Stufe 12) | Rahmenfenster „Futurama“ und eine 4:3-Ansicht als Kindfenster mit Letter-/Pillarbox. Per-Monitor-DPI-aware (V2), Anfangsgröße 640×480 × `window_scale`, an den Arbeitsbereich angepasst. Versteckt bis zum ersten Present. Alt+Enter schaltet randloses Vollbild, F12/Druck lösen einen Screenshot aus. Das Fenster gehört dem Boot-Thread (Message-Loop, Stufe 14), Swapchain und Present laufen auf dem Gast-Hauptthread. |
| RHI + D3D11 | Typisierte Handles, gecachte Pipeline-/Sampler-Zustände, Transient-Ring für Vertex-/Indexdaten, Rechteck-Clears (Farbe über `ClearView`, Tiefe/Stencil über ein Dreieck), Read-backs. Debug-Layer mit Info-Queue-Auswertung, Zählern und Exit-Zeile. **Seit Phase 5:** zwei persistente 640×480-Backbuffer in Rotation (Xbox-Flip). Present zeichnet den aktuellen Backbuffer in eine Swapchain in Fenstergröße (`FLIP_DISCARD`, `ResizeBuffers` bei Größenänderung), Skalierung `linear`, `point` oder `integer`. |
| `ni_extract` | Kamera wie `SetCameraData` 0x1469E0 (x87-Reihenfolge, `_ftol`), Viewport, ClearBuffer-Rechteck. Property→State in der Reihenfolge von `FUN_00159CD0` mit nachgebildetem REN- und Device-Zustand (`StateShadow`): Alpha, Z-Buffer, Texturing (Basis-Map, Bildschirm-Pfade), VertexColor, Material, Specular, Fog, Stencil, Wireframe, Shade, Dither. Lichter wie `FUN_0015BA40`/`FUN_0015B6A0`. Geometrie für die Slots 48–51/54/55, CPU-Skinning über den Originalcode `FUN_0014F710`. |
| `ff_pipeline` | D3D8-Fixed-Function: Texturstufen-Ops, Vertex-Licht (direktional, Punkt, Spot, Ambient), Nebel, Alpha-Test/-Blend, Cull-Modi. Halbpixel-Korrektur für transformierte Geometrie (B19) und **seit Phase 5** Vertex-Raster auf 1/16 Pixel (B33). |
| `texconv` | Alle Formate der Disc: DXT1/3 → BC1/BC2, RGB565/RGBA4444 → BGRA8 mit Bitreplikation (passt texelgenau zu den xemu-Aufnahmen), Paletten über Slot 69, Formatwahl `FUN_001572D0` mit den FormatPrefs, Default-Textur 8×8 (`FUN_0014AB50`). |
| `NiPCRenderer` / `GpuRenderer` | Slots 15, 16, 17 (neu), 19, 27, 45–51, 54, 55, 60 (neu in Phase 5), 65. Draw-Ergebnis je Art gezählt (Drawn, Skipped, Failed, Empty, Unsupported, Error, NotInScene, TextureTarget). Trace einzelner Frames (`trace_frames`, seit Phase 5 mit Bildschirmrechteck je Draw) und Frame-Zusammenfassung. Slot 65 = `FUN_0015E010` mit Upload und, seit Phase 5, Freigabe des Pixelpuffers. |
| Takt und Statistik | `platform::PaceFrame` (M2-Algorithmus, wartbarer Hochauflösungs-Timer) nach Present mit VSync. Exit-Zeilen `present:` (Rate, langsamstes 1-s-Fenster, längster Frame, Takt-Neustarts). **Seit Phase 5** zusätzlich die Arbeit je Frame (`present: frame work …`). |

## 3. Bauen und Ausführen

### 3.1 Bauen

Wie M2; `build.cmd` immer mit vollem Pfad aufrufen. Die HLSL-Shader übersetzt `fxc` beim Bauen in Header, die Laufzeit braucht keinen Shader-Compiler.

```powershell
$env:FUT_CMAKE_ARGS = '-DFUT_WERROR=ON'
cmd /c "D:\FuturamaDecomp\build.cmd" build\m3 Debug
cmd /c "D:\FuturamaDecomp\build.cmd" build\m3-rel RelWithDebInfo
cmd /c "D:\FuturamaDecomp\build.cmd" build\m3-clang Debug all clang
python tools\gen\gen_all.py --check
```

### 3.2 Konfiguration

Beispiel `run\m3final\debug\futurama.ini` (M3-Prüflauf, Debug):

```ini
[paths]
iso      = Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso
userdata = D:\FuturamaDecomp\run\m3final\debug

[debug]
selftest = 1               ; m3_check prüft auch alle M2-Kriterien
msgbox   = 0
timeout  = 150
frames   = 3000
trace    = vfs

[settings]
language = german

[render]
backend           = d3d11  ; Standard seit M3; null = Null-Renderer aus M2
frame_rate        = 50     ; PAL-VBlank-Takt nach Present; 0 = ungebremst
vsync             = 1
screenshot_frames = 150, 400, 680, 1000, 2990
trace_frames      = 150, 400, 680      ; Draw-Trace der Intro-Szenen

[log]
level    = info
```

Schlüssel in `[render]` (neu in M3):
- `backend` = `d3d11` | `null`
- `vsync` = 0 | 1
- `debug_layer` = `auto` | `on` | `off`; `auto` schaltet den Layer nur in Debug-Builds ein, Release-Prüfläufe brauchen `on`
- `adapter` = `hardware` | `warp`
- `window_scale` = 1..8
- `fullscreen` = 0 | 1
- **`scaling` = `linear` | `point` | `integer`** (neu in Phase 5)
- `screenshot_dir`, Standard `<userdata>\screenshots`
- `screenshot_frames`, `trace_frames`: Listen von Frame-Nummern
- `validate`: Prüfgerüst für die Extraktion, nur mit `backend = null`

**Screenshots:** F12 oder Druck im Spielfenster speichern das nächste angezeigte Bild als `<screenshot_dir>\frame_NNNNNN.png` (640×480, unabhängig von Fenstergröße und Skalierung). Das Log meldet `screenshot: <Pfad> (640x480)`. `screenshot_frames` erzeugt die Screenshots automatisch. F12 ist mit angehängtem Debugger dessen Break-Taste. **Fenster:** Alt+Enter schaltet randloses Vollbild. Das Schließen beendet den Prozess geordnet nach dem nächsten Frame (Exitcode 0).

### 3.3 Ausführen und prüfen

```powershell
# Ein Lauf (PowerShell: GUI-Programm, deshalb Start-Process -Wait)
Start-Process -Wait -PassThru D:\FuturamaDecomp\build\m3\bin\futurama.exe -ArgumentList '--config=D:\FuturamaDecomp\run\m3final\debug\futurama.ini'
# M3-Exit-Prüfung (aus D:\FuturamaDecomp)
python tools\m3_check.py --exe build\m3\bin\futurama.exe --config run\m3final\debug\futurama.ini --runs 3 --fresh
python tools\m3_check.py --exe build\m3-rel\bin\futurama.exe --config run\m3final\rel\futurama.ini --runs 1 --fresh
python tools\m3_check.py --exe build\m3-clang\bin\futurama.exe --config run\m3final\clang\futurama.ini --runs 1 --fresh
# Regressionen und Unit-Tests
python tools\m1_check.py --exe build\m3\bin\futurama.exe --config run\m3final\m1\futurama.ini --runs 3 --xbe orig\retail_eu\default.xbe --fresh
python tools\m2_check.py --exe build\m3\bin\futurama.exe --config run\m3final\m2\futurama.ini --runs 1 --fresh
python tools\m2_check.py --exe build\m3\bin\futurama.exe --config run\m3final\m2-null\futurama.ini --runs 1 --fresh
build\m3\bin\fut_tests.exe
```

`m3_check.py` übergibt `--selftest` sowie `--frames=3000` und `--trace=vfs`, falls die INI sie nicht setzt. Die Konfiguration muss `[render] backend = d3d11` haben. Logs und Screenshot-Kopien jedes Laufs landen in `<userdata>\m3_check\`. Optionen:
- `--scene NAME=FRAME` (Standard `legal=150 tech_notice=400 loading=680 frontend=1000`)
- `--xemu-ref DIR` (Standard `run\xemu_ref`) und `--xemu-frontend DIR` (Standard `run\xemu_ref_steam`)
- `--min-hz` (Standard 49,5) und `--max-work-ms` (Standard 20)

Diagnose:
- `trace_frames` loggt jeden Draw: Art, Gastobjekt, Ergebnis, Bildschirmrechteck in D3D8-Pixeln, Vertizes, Texturstufe, Licht, Blend, Z, Cull, Material.
- Trace- und Screenshot-Frames enden mit `draw summary frame N: …`. Hat ein Frame nichts gezeichnet, nennt die Zeile den Frame, dessen Bild er zeigt.

## 4. Status der M3-Exit-Kriterien

Belege aus den Läufen nach dem letzten Fix (`run\m3final\debug\m3_check\run_01..03.log`, `run\m3final\rel\…`, `run\m3final\clang\…`):

| # | Kriterium | Status | Beleg |
|---|---|---|---|
| 1 | Splash (`NI_Splash_PAL.nif`), Legal-Bild und Frontend-Menü erscheinen vollständig; Vergleich mit xemu | **erfüllt** | Screenshots angesehen (Abschnitt 5). Draw-Bilanz pro Szene: Legal 6/6 gezeichnet, Tech-Notice 6/6, Ladebild 7/7, Frontend 342–345 `RenderShape`, alle gezeichnet. Laufsummen über 3000 Frames: ≈ 763 000 `RenderShape`, 44–46 `RenderScreenPolygon`, ≈ 4840 `RenderScreenTexture`, **100 % gezeichnet**, 0 übersprungen, gescheitert, „unsupported“, Fehler oder außerhalb einer Szene. xemu: Legal und Tech-Notice mittlere Abweichung **0,00** (texelgenau), Ladebild 0,00–0,20 (animierte Dose), Frontend 0,01–0,02 auf den 61 % statischen Pixeln, davon 98,0–98,8 % exakt gleich |
| 2 | Kein D3D-Debug-Layer-Fehler | **erfüllt** | Debug-Layer installiert und aktiv: `D3D11 debug layer: enabled - 0 errors, 0 warnings, 0 corruption (1 device)` in jedem Lauf (Debug, RelWithDebInfo mit `debug_layer = on`, clang-cl), `d3d11 device: … 0 debug errors, 0 debug warnings`. Seit Phase 5 pumpt die Exit-Zeile die Queue noch einmal auf dem Renderthread (Befund 10). Unit-Tests auf WARP mit Debug-Layer: 0 Meldungen, Teardown ohne lebende Objekte |
| 3 | `--frames=3000` ohne Absturz, Bildrate ≥ 50 | **erfüllt** | Exitcode 0, `FRAMES: limit of 3000 frames reached`, `present: 3000 frames in 59.96 s (average 50.01 Hz, slowest 1 s window 49.0–49.4 Hz, longest frame 32–40 ms)`, 0 Takt-Neustarts. Arbeit je Frame (Gast + Renderer ohne Present und Takt): Ø 3,07 ms Debug, 2,25 ms RelWithDebInfo, 2,91 ms clang; 99 % ≤ 3,4–4,6 ms, also tragfähig für 326–445 Hz. Die Prüfung bewertet das seit Phase 5 so (Befund 3): 99. Perzentil ≤ 20 ms und gepacter Durchschnitt ≥ 49,5 Hz |

Zusätzlich gelten alle M2-Kriterien und die allgemeine Regel (keine error-, warn-, `UNIMPLEMENTED`- oder `TRAP`-Zeile); `m3_check` prüft sie in jedem Lauf mit.

Prüfläufe nach den Review-Fixes (alle `ALL PASS`):

| Build | Prüfung | Konfiguration | Läufe | Ergebnis |
|---|---|---|---|---|
| MSVC Debug | `m3_check` | `run\m3final\debug` | 3, `--fresh` | ALL PASS; 3 × 3000 Frames, 50,01 Hz, Frame-Arbeit 99 % ≤ 4,6 ms, Debug-Layer 0/0/0 |
| MSVC RelWithDebInfo | `m3_check` | `run\m3final\rel` (`debug_layer = on`) | 1 | ALL PASS; Frame-Arbeit Ø 2,25 ms, langsamstes Fenster 49,40 Hz |
| clang-cl Debug | `m3_check` | `run\m3final\clang` | 1 | ALL PASS |
| MSVC Debug / RelWithDebInfo | `m1_check` | `run\m3final\m1` | 3 / 2, `--fresh` | ALL PASS (Fenster wird in Stufe 12 versteckt angelegt) |
| MSVC Debug | `m2_check` | `run\m3final\m2` (d3d11) und `m2-null` (`backend = null`) | je 1 | ALL PASS, gleiche Spielpool-Werte in beiden Backends |
| MSVC RelWithDebInfo | `m2_check` | `run\m3final\m2` | 1 | ALL PASS |
| alle drei | `fut_tests` | – | – | 191/191 Testfälle, 8814 Assertions (1 manueller Test übersprungen) |

Alle drei Builds sind mit `FUT_WERROR=ON` ohne Warnung (MSVC `/W4`, clang-cl `/W4`, fxc `/WX`). `gen_all.py --check`: alle generierten Dateien aktuell.

In Phase 5 schlug der erste Debug-Lauf einmal fehl: Legal-Frame 150 hatte 0 Draws. Das Bild war trotzdem vollständig (xemu-Abweichung 0,00), denn der persistente Backbuffer zeigte das Bild von zwei Presents zuvor. `m3_check` hat das zu Recht nicht als Draw-Bilanz akzeptiert. Seit dem Fix nennt die Frame-Zusammenfassung den Ursprungsframe; die Prüfung verlangt für ihn K > 0 Draws, alle gezeichnet. Ein Diagnoselauf (`run\m3final\diag\nodraw`) zeigt das Muster: Frame 115 hat 0 Draws und zeigt das Bild von Frame 113 (6 Draws), beide Screenshots sind pixelgleich.

## 5. Bilder, Vergleich mit dem Original, verbleibende Unterschiede

**Finale Screenshots** (MSVC Debug, letzter `m3_check`-Lauf, nicht im Repository):
- Legal-Bild: `D:\FuturamaDecomp\run\m3final\screenshots\frame_000150.png`
- Tech-Notice/Splash (`NI_Splash_PAL.nif`): `D:\FuturamaDecomp\run\m3final\screenshots\frame_000400.png`
- Ladebild: `D:\FuturamaDecomp\run\m3final\screenshots\frame_000680.png`
- Frontend-Menü: `D:\FuturamaDecomp\run\m3final\screenshots\frame_001000.png`, `…\frame_002990.png`
- Vergleichsbögen: `D:\FuturamaDecomp\run\m3final\compare\final_vs_xemu.png` (unser Bild, xemu, Differenz × 8), `steam_cycle.png` (Dampfwolke in xemu in allen Phasen), `row189.png` (Oberkante „Neues Spiel“ vor und nach dem Raster, xemu)
- Fensteraufnahmen der Skalierung (2631×1973, 225 %): `D:\FuturamaDecomp\run\m3final\diag\window_linear.png`, `window_point.png`, `window_integer.png`, Ausschnitte `window_scaling_crops.png`

**Referenz:**
- `run\xemu_ref` (Intervall 3 s) und `run\xemu_ref_steam` (0,62 s, neu in Phase 5) sind Fensteraufnahmen von 1280×960. Jeder 2×2-Block ist gleich: xemu rendert in 1× und vergrößert mit Nearest. Die frühere Annahme, xemu rendere in doppelter Auflösung, war falsch.
- Verglichen wird auf der 640×480-Box-Verkleinerung.
- Statisch sind 61 % der Frontend-Pixel: in allen 81 Aufnahmen unverändert und außerhalb der Dampfregion.

**Verbleibende sichtbare Unterschiede:**
- **Animationsphasen:** Logo-Buchstaben, Blitze, Zahnräder, Dampfwolke, Dose im Ladebild und der Puls der Menü-Hervorhebung stehen zu anderen Zeitpunkten als in den Aufnahmen. Das ist Timing, kein Renderer-Unterschied.
- **Restliche Rasterkanten:** Knapp 2 % der statischen Frontend-Pixel weichen ab, vor allem an Kanten dünner Geometrie (Rohre, Zahnräder). xemu ist selbst eine Emulation. Ohne Aufnahme von echter Hardware bleibt offen, welche Seite dem NV2A näher ist.
- **Filme** (fox/sci/uds/intro): kein Bild, der XMV-Stub beendet sie im ersten Update (M6).
- **Nicht nachgebildet:** Multi-Map- und Multi-Pass-Texturpipelines (nur die Basis-Map-Stufe), NiTextureEffect, Render to Texture, Punkte und Linien (52/53), Batch-Slots, `SetScreenSpaceOffset`. Diese P2-Funktionen braucht keine der Szenen; gemessen 0-mal.

Nicht mehr auf der Liste:
- Dampfwolke (Befund 7): auch im Original sichtbar.
- „Kantenglättung in xemu“: Die Aufnahmen sind 1×. Der Unterschied war die Rasterung, behoben mit B33.
- Oberkante „Neues Spiel“ (Befund 9).

## 6. Slot-Abdeckung

Aufrufe je 3000-Frame-Lauf (`NiPCRenderer slot calls`, `run\m3final\debug\m3_check\run_01..03.log`):

| Slot | Name | Aufrufe | Umsetzung |
|---:|---|---:|---|
| 16 | DisplayFrame | 3000 | Screenshot, Present-Pass, Present, Takt, `--frames`, Schließen des Fensters |
| 17 | ClearBuffer | 3014–3015 | Viewport = ganzes Ziel, Farbe/Tiefe/Stencil, NiRect (y nach oben) |
| 21/22 | Breite/Höhe | je 3 | Create-Größe oder Größe des Texturziels |
| 27 | SetBackgroundColor | 12 | `_ftol(c * 255) & 0xFF` wie 0x14B0C6 |
| 39 | PrecacheGeometry | 181 | FALSE (CPU-Geometrie bleibt, [REN] 5.3) |
| 45/46/47 | BeginScene/EndScene/SetCameraData | je 2976–2977 | Cache-Resets, Kamera, Viewport |
| 48 | RenderShape | 761 900–763 800 | Extraktion → Fixed-Function → D3D11, 6618 geskinnt |
| 54 | RenderScreenPolygon | 44–45 | Bildschirmkamera |
| 55 | RenderScreenTexture | 4832–4845 | vortransformierte Quads |
| 56/71 | – | ≈ 763 000 / 1 | No-ops wie in M2 |
| 61/64 | – | 19 / 5 | No-ops wie in M2 |
| 65 | CreateSourceTextureRendererData | 161 | 140 neue Daten, 136 hochgeladen (15 101 KiB BGRA8), 4 Default (Filmtexturen), 115 Pixelpuffer freigegeben |
| 66 | CreateRenderedTextureRendererData | 1 | Daten + Zieldeskriptor (Render to Texture selbst ist P2) |

Umgesetzt, aber in den M3-Szenen nicht aufgerufen:
- 15 SetRenderTarget: NULL bzw. Textur, Draws auf Texturziele sind gezählte No-ops
- 19 SetLeftRightSwap
- 26/28 Clear-Werte für Tiefe und Stencil
- 49/50/51 Triangles, TriStrip, TriStrips
- 60 PurgeEffect
- 69 Paletten: TRUE, P8 expandiert texconv

Die übrigen Slots bleiben No-ops mit dem `ret N` des Originals, zur Compilezeit und per Selbsttest `GfxRendererThunkPurge` geprüft. Hooks: 161 installiert, wie in M2.

## 7. Leistung

Messrechner: NVIDIA RTX 5090, Feature-Level 11_1, 4K-Monitor mit 225 %.
- **Arbeit je Frame** (vom Ende des Takts bis vor Present; enthält Spiellogik, alle Slot-Aufrufe und die D3D11-Befehlsabgabe):

  | Build | Ø | 99 % | längster Frame | tragfähig |
  |---|---:|---:|---:|---:|
  | Debug | 3,06–3,07 ms | 4,5–4,6 ms | 18,1–18,9 ms | 326–327 Hz |
  | RelWithDebInfo | 2,25 ms | 3,4 ms | 8,1 ms | 445 Hz |
  | clang-cl | 2,91 ms | 4,5 ms | 13,9 ms | 343 Hz |

  Die Messung des Reviewers ohne Takt und ohne VSync ergab im Frontend ≈ 228 Hz (Debug, Debug-Layer an).
- **Gepact (VSync + 50 Hz):** 50,01 Hz, langsamstes 1-s-Fenster 49,0–49,4 Hz, längster Frame 32–40 ms, 3–9 Frames über 1,5 Perioden (Ladehänger, vom Takt aufgefangen), 0 Takt-Neustarts.
- **Umfang je Lauf:** 768 000 Draws, 52,7 Mio. Dreiecke, 1,86 GiB Transient-Daten (3000 Frames). 8 Pipeline-Zustände, 4 Sampler, 4 von 5 Shader-Permutationen in Gebrauch. 111 lebende Texturen (4,9 MB). Arena-Spitze 6,4 KB.
- **Spielpool** (memory.cpp, 38 MiB): am Ende 5839 KiB belegt, Spitze 6855 KiB. Ohne die Freigabe aus Befund 1 waren es 8295 bzw. 8892 KiB (Gegenprobe `run\m3final\diag\nofree`).
- **Slot 65 auf Lade-Fibers:** 81 von 140 Aufrufen. Tiefste Stack-Nutzung 21 680 Byte; mindestens 502 444 von 524 288 Byte bleiben frei.

## 8. Review-Befunde

Jeder Befund wurde vor der Änderung am Disassembly, am Dekompilat oder mit einem eigenen Lauf nachvollzogen.

| # | Schwere | Befund | Entscheidung | Umsetzung und Nachweis |
|---|---|---|---|---|
| 1 | hoch | Slot 65 gibt den Pixelpuffer nicht frei wie `FUN_0015E010` | **angenommen** | Capstone 0x15E1A6..0x15E1DD bestätigt die Bedingung: Daten +0x3C (D3D-Textur) ≠ 0, Bild ≠ `renderer+0x5A0`, `[texture+0x3C]` ≠ 0, Breite und Höhe ≤ 0x200 → `FUN_001511B0` (`FUN_000A3880(+0x30)`, +0x30 = 0). `vf3` 0x118B3D liest +0x3C ab Stromversion 4.1.0.13 (String 0x2A6E28), der ctor setzt 0, +0x38 = 1. Umsetzung (B30): Freigabe in beiden Backends, Konstanten `ni::kNiSourceTextureStatic/DiscardPixelsHint`, `guest::kNiPixelDataFreePixels`. Der Resolver überspringt Neu-Uploads ohne Puffer (gezählt), texconv/validate liefern `NoPixelData`. Exit-Zeilen `NiPCRenderer pixel buffers …` und `game memory pool: …` (DAT_002E7D60/64, Spitze DAT_002E8D38). Lauf: 115 Puffer, 2558 KiB frei; 18 behalten (+0x3C = 0, vermutlich zur Laufzeit erzeugte Texturen, nicht einzeln geprüft), 3 behalten (> 512). Pool am Ende 5839 statt 8295 KiB. Die NIF-Zählung des Reviewers (8301 von 8312) wurde nicht nachgezählt. Nebenbei korrigiert: Das Original leakt das Dateibild nicht (0x15E1E2..0x15E243 gibt es frei) |
| 2 | niedrig | `FLIP_DISCARD`: Backbuffer-Inhalt nach Present undefiniert, das Spiel verlässt sich darauf | **angenommen, anders gelöst** | `FUN_00146260(0x14000002)` liefert `D3DSWAPEFFECT_DISCARD` (1) bei `BackBufferCount` 1. Auf der Xbox ist das ein Flip mit zwei Puffern, der neue Backbuffer enthält das vorletzte Bild. Gemessen: 2980 von 3000 Frames ohne Farb-Clear; Kontextwechsel löschen mit 2–3 Clears pro Frame. Statt `FLIP_SEQUENTIAL` (B31): zwei persistente Offscreen-Backbuffer in Rotation, Present-Pass in eine Swapchain in Fenstergröße. Der Inhalt ist auf jedem Treiber definiert, die Semantik entspricht genau der Xbox, Screenshots sind von der Swapchain unabhängig. Unit-Test „persistent back buffers …“ für 1, 2 und 3 Puffer |
| 3 | niedrig | M3-3 misst bei exakt 50 Hz „kein einziger Hänger“ statt Leistung | **angenommen** | `frame_pacer.cpp`: Ein Frame, der mehr als eine Periode zu spät ist, startet den Zeitplan neu; die verlorene Zeit kommt nicht zurück. Umsetzung (B36): `platform::NoteFrameWorkDone` und die Zeile `present: frame work …` (Histogramm in 0,1-ms-Schritten). M3-3 verlangt jetzt 99. Perzentil ≤ 20 ms und gepact ≥ 49,5 Hz; Takt-Neustarts werden gemeldet. Messwerte siehe Abschnitt 7 |
| 4 | niedrig | Slot 60 PurgeEffect ist ein No-op | **angenommen** | Capstone 0x146E40: RTTI-Kette gegen 0x30EBAC, dann `FUN_0015B9E0` (Liste, `LightEnable(+0xC4, 0)`, +0x108 frei). Umsetzung (B34): neuer Handler (ret 4), IsKindOf über vf1 und RTTI+8, `ni::PurgeLight` entfernt das Licht aus `StateShadow::lights`, `lastEffectState` bleibt. Unit-Test. Im Spiel 0 Aufrufe |
| 5 | niedrig | WM_CLOSE: Exit-Reporter auf dem UI-Thread lesen nicht-atomare Zustände; der Debug-Layer wird nicht mehr gepumpt | **angenommen** | Im Code bestätigt: `LogRendererStatistics` → `GpuRenderer::LogStatistics` und `Device::Stats` lesen Container des Renderthreads. Umsetzung (B35): WM_CLOSE setzt nur `platform::CloseRequested()`, der nächste `DisplayFrame` beendet auf dem Gast-Hauptthread; Rückfall nach 3 s. `LogStatistics` loggt auf fremden Threads nur einen Hinweis. Der Reporter „d3d11“ pumpt auf dem Renderthread. Live-Test `run\m3final\diag\live`: `window closed - ending after frame 1297` auf `guest-main`, alle Reporter auf `guest-main`, Exitcode 0 nach 0,08 s |
| 6 | niedrig | Falsche Belegadresse 0x146538 in drei Kommentaren | **angenommen** | Capstone bestätigt: Viewport 0x14659E..0x1465B9 (`call 0x234B20`), Flags 0x14654D..0x14659C, Clear 0x1465E0. Kommentare in `nipc_gpu.cpp`, `nipc_gpu.h` und `nipc_renderer.cpp` korrigiert; das Verhalten war richtig |
| 7 | mittel | Dampfwolke = Abtastfehler der xemu-Referenz, keine Renderabweichung | **angenommen** | Neue Aufnahmen `run\xemu_ref_steam` (0,62 s, 68 Bilder, Frontend ab 74 s). Die Wolke ist in xemu sichtbar. Regionshelligkeit (515,105)–(600,160): 22,2 / 24,8 / 28,5 / 35,8 / 39,1 – dieselben fünf Werte wie bei uns, Zyklus 1 s. `xemu_ref` (3 s) trifft immer 22,2. Doku korrigiert (B37, GFX_CONTRACT §9). `m3_check` bildet die statische Maske aus beiden Verzeichnissen und maskiert die Dampfregion ausdrücklich. `xemu_capture.py --start` |
| 8 | niedrig | Legal/Tech-Notice: Frames ohne Draw zeigen einen nach DXGI undefinierten Backbuffer | **angenommen** | Bestätigt: 23–63 Frames je Lauf ohne Draw, davon 5–45 auch ohne Farb-Clear; Frame 115 hat 0 Draws. Behoben mit B31 (wie Befund 2). Neu sind die Zähler in `d3d11 frames:` (ohne Draw / ohne Farb-Clear / ohne beides) und der Bildursprung in `draw summary` (`shows the picture of frame M (K draw calls, D drawn)`). `m3_check` wertet beides aus |
| 9 | niedrig | Oberkante „Neues Spiel“ eine Zeile zu tief | **angenommen, teilweise abgelehnt** | Abgelehnt ist die Vermutung, die Abweichung könne von xemus 2×-Rendering kommen: Die xemu-Aufnahmen sind 1× (alle 2×2-Blöcke gleich). Die verlangte 1×-Referenz liegt also vor und bestätigt die Abweichung. Trace mit Vertex-Positionen: Oberkante bei y = 188,997..189,008, genau auf den Pixelzentren. D3D11 mit 8 Bit Subpixel deckt Zeile 189 nur links ab, das Original deckt sie ab. Umsetzung (B33): Vertizes werden im Ubershader auf 1/16 Pixel gerastet (Rundung). A/B mit je 4 Frontend-Bildern: statische Pixel exakt 93,0–93,8 % → 98,0–98,8 %, Zeile 189 jetzt (89,6, 50,9, 105,2) gegen xemu (87,6, 47,9, 103,2). Abschneiden statt Runden: 89 %. Unit-Test „transformed vertices snap to 1/16 pixel“; er scheitert ohne Raster |
| 10 | niedrig | wie 5, dazu: LogDebugLayerSummary ohne letztes Pumpen | **angenommen, teilweise abgelehnt** | Umsetzung wie Befund 5. Abgelehnt ist die Teilaussage, auch die Reporter „present“ und „d3d11“ läsen nicht-atomare Zähler: „present“ liest unter `g_statsMutex`/`g_paceMutex`, „d3d11“ nur atomare Summen. Das fehlende letzte Pumpen traf zu und ist behoben |
| 11 | niedrig | Bilineare Compositor-Streckung (Faktor 4,1) wirkt weich, keine Option | **angenommen** | Umsetzung (B32): `[render] scaling` = `linear` \| `point` \| `integer` im Present-Pass, Standard `linear`. Fensteraufnahmen 2631×1973: `integer` = Faktor 4 mit Rand (Inhalt 2560×1920 ab (35, 26)). Dabei gefunden und behoben: Der Gast-Thread ist nicht DPI-aware und sah 1169×876 statt 2631×1973. Die Größe wird jetzt im DPI-Kontext des Fensters gelesen |

## 9. Abweichungen vom Design

Die vollständige Tabelle steht in [`loader_design.md` Abschnitt 8.3](loader_design.md#83-abweichungen-in-m3). Die Entscheidungen E1–E12 gelten unverändert.

| # | Kurzfassung |
|---|---|
| B19 | +0,5-Pixel-Verschiebung auch für transformierte Geometrie (D3D8/NV2A-Pixelzentren). |
| B20 | `[render] backend` Standard `d3d11`. |
| B21 | Eigene Klasse `GpuRenderer` für die Hostseite. |
| B22 | Slot 65 nach `FUN_0015E010` mit Formatwahl `FUN_001572D0` (in Phase 5 korrigiert: der letzte Schritt fehlte, siehe B30). |
| B23 | Textur-Resolver = Texturmanager `FUN_0015F930`/`FUN_0015E260`. |
| B24 | Render to Texture: gezählte No-ops (P2). |
| B25 | Slot 27 rechnet wie das Original (Überlauf statt Klemmen). |
| B26 | Stack-Messung für Slot 65 auf Lade-Fibers. |
| B27 | M3-2 verlangt einen aktiven Debug-Layer. |
| B28 | Draw-Trace nur für die Intro-Szenen in der Prüfkonfiguration. |
| B29 | `NiTextureFormatPrefs` +0x18/+0x1C getauscht. |
| B30 | **Neu:** Freigabe des Pixelpuffers nach dem Upload (Befund 1). |
| B31 | **Neu:** zwei persistente Backbuffer (Xbox-Flip), Present-Pass in eine Swapchain in Fenstergröße (Befunde 2, 8). |
| B32 | **Neu:** `[render] scaling` (Befund 11). |
| B33 | **Neu:** Vertex-Raster 1/16 Pixel (Befund 9). |
| B34 | **Neu:** Slot 60 PurgeEffect (Befund 4). |
| B35 | **Neu:** WM_CLOSE als Anforderung, Exit auf dem Renderthread (Befunde 5, 10). |
| B36 | **Neu:** M3-3 = Frame-Arbeit (99 % ≤ 20 ms) + gepacter Durchschnitt ≥ 49,5 Hz (Befund 3). |
| B37 | **Neu:** xemu-Referenz ist 1×; Dampfwolke im Original sichtbar; statische Maske aus zwei Aufnahmeserien (Befund 7). |

## 10. Bekannte Probleme und offene Punkte

- **Kein Hardware-Vergleich:** Referenz ist ausschließlich xemu. Das 1/16-Pixel-Raster ist am xemu-Bild gemessen, nicht am NV2A belegt; die restlichen knapp 2 % Kantenpixel sind nicht zugeordnet.
- **Nicht erreicht in M3:**
  - Slots 15 (Textur), 19, 26, 28, 49–51 und 60
  - Render to Texture, Multi-Map, NiTextureEffect, Punkte und Linien (P2)
  - Neu-Upload über die Revision (0 im Lauf)
  - Pfad der Texturdateien in Slot 65 (keine Textur hat einen Pfad)
- **Device-Verlust** (`DeviceLost` → GFX/error + `core::Fatal`) ist ungetestet, auf WARP nicht provozierbar.
- **`[render] validate`** läuft nur mit dem Null-Renderer.
- **Rückfall von WM_CLOSE:** Zeigt der Gast 3 s lang keinen Frame, endet der Prozess auf dem UI-Thread; die Renderer-Statistik ist dann nur ein Hinweis.
- **Speicher:** `texconv` behält 16-Bit-Texturen nicht als 16 Bit (`keep16Bit` aus). Der Grund ist die texelgenaue Parität mit xemu; das kostet 15 MB Quelle bzw. 4,9 MB lebend im Frontend.
- **Filme** (M6), **Ton** (M5), **Eingabe** (M4) fehlen; das Frontend pendelt ohne Eingabe nach ≈ 60 s mit Attract.
- **Lizenz/Assets:** wie M1/M2 – keine Spieldaten im Repository; Testläufe, Screenshots und xemu-Aufnahmen nur unter `run\`.

## 11. Nächste Schritte

**M4 – Eingabe** (Design-Abschnitt 6): XInput-HLE der 7 XPP-Einstiege nach [AVI] 4.3 (heute stille Stubs: keine Geräte). Daraus folgt:
1. `XGetDevices`/`XGetDeviceChanges` melden ein Gamepad im Port 0 (Hotplug-Simulation), `XInputOpen`/`XInputGetState`/`XInputSetState` lesen ein XInput-Pad oder die Tastatur über ein konfigurierbares virtuelles Pad (`[input]`). Rumble mit synchronem `dwStatus = 0`.
2. Die Tastatur kommt aus dem vorhandenen Fenster (WM_KEYDOWN/WM_KEYUP des Rahmenfensters, wie F12 heute); die Eingabe muss Thread-sicher zum Gast-Hauptthread (Polling in `XInputGetState`).
3. Prüfung `tools\m4_check.py` nach dem Muster von `m3_check`: geskriptete Eingaben (Menünavigation „Neues Spiel“ → Level 1-1), Screenshots der erreichten Szenen, Hotplug, Rumble-Log; dazu neue Szenen für den M3-Renderer (Ingame), die vermutlich P2-Funktionen brauchen (Multi-Map, Texture-Effects, Render to Texture, Punkte/Linien – in M3 gezählt, nicht gezeichnet).

**Danach:** M5 Ton (DSOUND auf XAudio2, ADPCM, 3D), M6 Filme (XMV → Filmtextur, die heute schwarz gezeichnet wird), M7 Spielstände, M8 spielbares Level, M9 inkrementelle Dekompilierung, M10 Vulkan/D3D12 mit höherer interner Auflösung und Widescreen (die RHI ist dafür ausgelegt; der Present-Pass mit `[render] scaling` ist der Ort für eine spätere Hochskalierung).

Aufräumen aus M3:
- Den statischen Geometrie-Cache (Schlüssel `NiGeometryData` + id + Revision) einführen, sobald Ingame-Szenen den Transient-Ring belasten; heute 1,86 GiB je 60 s.
- Den Trace-Aufwand eines voll getracten Frontend-Frames (≈ 30 ms Logging) senken.
- Die doppelte Purge-Messung aus M2 zusammenlegen.
