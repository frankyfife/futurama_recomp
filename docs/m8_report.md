# M3e / M8: Renderer P2 – Bericht

Stand: 2026-10-12. Abweichungen vom Entwurf: [`loader_design.md`](loader_design.md) 8.23. Schnittstellen: `src\INTERFACES.md` 4.39. Ausgangslage: [`m4_report.md`](m4_report.md) 6.1 (Lücken im Level).

## Kurzfassung

* **Render to Texture funktioniert.** Slot 15 mit einer `NiRenderedTexture` zeichnet in ein eigenes Farbziel mit eigenem Tiefenpuffer, statt die Draws zu verwerfen (vorher etwa 8 000 gezählte No-ops je Level-Lauf). Das Ziel hat die **Renderauflösung** (Gastgröße × Faktor des Backbuffers: 128×128 → 256×256 bei 1280×960, 576×576 bei 2880×2160) und die **MSAA-Samples des Backbuffers**. Beim Verlassen des Ziels wird aufgelöst; die Textur wird erst danach gesampelt.
* **Was das Spiel in Texturen rendert:** Im Level ist es genau ein Ziel, **Frys Schattenbild** (`FUN_0003B960`): eine Kamera von oben, weißer Hintergrund, Fry ohne Konturen in Grau 0,8. Das **HUD-Porträt** oben links ist *kein* Render to Texture – es wird direkt in den Backbuffer gezeichnet und war schon vor M8 sichtbar (G1-Analyse, Abschnitt 6). Weitere Texturziele legt nur das Frontend an (`FUN_0001F360`, 256×256, Menü in Textur) und ein Objekt mit eigener Kamera (`FUN_0006B670`); beide liefen in den Testszenen nicht.
* **Projizierte Schatten:** Damit das Schattenbild sichtbar wird, baut der Port jetzt die Zusatzpässe der Texture-Pipeline des Originals nach (`NiTextureEffect` „projected shadow“, vf2 0x164360 ab 0x164A20): ein zweiter Pass über die Levelgeometrie, der das Bild mit dem Schatten multipliziert (Blend ZERO/SRCCOLOR), dazu die Clip-Ebene des Spiels (2×1-Textur, nur unterhalb der Figur). **Fry wirft jetzt einen Schatten** auf den Boden, im Sprung deutlich zu sehen.
* **Partikel (Slot 52 RenderPoints):** Werden gezeichnet, als kameraausgerichtete Quads genau wie REN sie auf der CPU baut (Ecken, UVs, Farben, Indexmuster, Zustand). Sie bleiben im 60-Hz-Raster der Logik (keine Interpolation, vom Auftrag erlaubt).
* **„5 fehlende Texturen“:** sind keine Lücke. Es sind die Platzhalter-Texturen des Movie-Players (`NiSourceTexture::Create(NULL)` in `FUN_000EE220`), eine je geöffnetem Film; der Player setzt sofort seine YUY2-Textur ein (M6). Das Original nimmt dafür ebenfalls die 8×8-Standardtextur.
* **Purge-Slots 58/59/61/64:** bleiben No-ops. Sie geben nur REN-eigene Rendererdaten an Gastobjekten frei, die der Port nie anlegt (Felder bleiben 0). Die neuen Renderziele werden mit den Renderdaten der Textur freigegeben.
* **Nicht Teil des Auftrags:** die schwarze Unterseite des Planet-Express-Schiffs (laut Nutzer original).

## Umsetzung

| Teil | Dateien | Inhalt |
|---|---|---|
| RHI | `gfx\rhi\rhi.h`, `rhi\d3d11\d3d11_device.cpp`, `d3d11_internal.h` | Add-only: `TextureDesc::sampleCount` (multisampled Render-Target-/Tiefentexturen), `Device::ResolveTexture` (D3D11 `ResolveSubresource`, Kopie bei 1 Sample) |
| Render to Texture | `gfx\nipc_gpu.h/.cpp`, `gfx\nipc_renderer.cpp` | Je Renderdaten einer `NiRenderedTexture` ein Ziel {Farbe (sampelbar), MSAA-Farbe, D24S8}; Viewport und Clear-Rechtecke wie beim Backbuffer skaliert; Auflösen beim Verlassen; Resolver liefert die Textur (`RenderedTexture`), Datendestruktor gibt frei (`ForgetRenderedTexture`); Größenregel `RenderTargetDimension` (höchstens 4096) |
| Schattenpässe | `gfx\ni_extract.h/.cpp`, `gfx\nipc_gpu.cpp` | `FUN_00163F10` + Handler 0x162C90 (Schattenstufe: CAMERASPACEPOSITION, COUNT2, Matrix = inverse Ansicht × Projektion des Effekts + Translation), `FUN_00162BB0` (Clip-Stufe), Passbildung von vf2 (je geclipptem Schatten ein Pass, freie Stufen mit ungeclippten Schatten), Blend aus der Passtabelle (+0x2F0), Nebel ab Pass 2 aus; nur für Geometrie ohne Blending und ohne Alpha-Test. `RenderStateDesc::extraPasses`, gezeichnet von `GpuRenderer` mit Geometrie, z, Stencil, Cull und Licht des ersten Passes |
| Partikel | `gfx\ni_extract.h/.cpp`, `gfx\ni_guest_layouts.h`, `gfx\nipc_gpu.cpp`, `gfx\nipc_renderer.cpp` | `NiParticlesData` (+0x34 Radius, +0x38 aktive Anzahl, +0x3C Größen), `ExtractPoints` nach 0x148AE0 (A/B = (rechts ± oben) · Radius / Skala, je Partikel × Größe; Farbe gerundet wie `fistp`; Index 0 1 2 0 2 3 aus `FUN_0015CBF0`), neuer `DrawKind::Points` |
| Diagnose | `gfx\nipc_gpu.cpp` | Bilder aus `[render] trace_frames` schreiben die Texturziele als `frame_NNNNNN_rtt_K.png`; Trace-Zeile mit Passzahl; Exit-Zeile `d3d11 render to texture (M8): …`, `ni_extract: … projected shadow passes, … texture effects not reproduced` |
| Tests | `tests\extract_m8_tests.cpp` (neu), `tests\nipc_gpu_tests.cpp` | Partikel-Ecken/Achsen/Farben, Schatten- und Clip-Matrix, Clear im Texturziel, MSAA-Auflösen bei Faktor 2, Größenregel |
| Skript | `tools\input_scripts\m8_level.txt` (neu) | „Neues Spiel“ → Level 1-1, Laufen, Drehen, Springen, Angriff mit Screenshots (etwa 30 s mit `[debug] movies = skip`) |

**Zusammenspiel mit G3 (Breitbild, nach dem Zusammenführen):** Die Platzierung von G3 gilt nur für den Backbuffer. Texturziele behalten ihre eigene Skalierung (Gast- → Gerätepixel) und bekommen weder Platzierung noch Balken. Die Schattenpässe eines Draws laufen mit derselben Platzierung (Viewport) und Kamera wie der Draw selbst; Partikel sind Szenengeometrie und werden mit Hor+ gestreckt wie die übrige Szene.

## Prüfung

* **Bauten:** MSVC RelWithDebInfo `build\m8` und clang-cl Debug `build\m8-clang` mit `FUT_WERROR=ON` ohne Warnungen.
* **Unit-Tests:** `fut_tests` (RelWithDebInfo) **491/491** bestanden, 1 übersprungen (Handtest), 1 727 731 Prüfungen.
* **Läufe** (Level 1-1, 1280×960, MSAA 4×, eigene Konfigurationen unter `run\m8\<name>\`, Ton aus, Filme übersprungen):
  * Vorher (`run\m8\base`, Stand vor M8): 7 969 Shape-Draws „texture target“ (verworfen), Slot 52 7 685 Aufrufe ohne Bild.
  * Nachher (`run\m8\final`): Texturziel `128x128 -> 256x256, 4 samples`; 7 969 Draws und 613 Clears im Texturziel, 613 Auflösungen; 128 928 Schattenpässe; Shape 264 027 Draws alle gezeichnet, „texture target“ 0; Partikel 5 053 gezeichnet, 2 632 leer (keine aktiven Partikel, wie im Original); 0 Konvertierungsfehler. GPU-Zeit je Bild Ø 1,09 ms (99 % ≤ 3,4 ms), 59,89 Hz, 98,8 % der Bilder auf dem Plan.
  * Screenshots (angesehen): `run\m8\final\shots\frame_001733.png` (Sprung: Frys Schatten auf dem Boden unter ihm), `frame_001511.png`, `frame_001809.png` (Schatten an den Füßen); Schattenbild selbst `run\m8\p2\shots\frame_001600_rtt_0.png` (Silhouette von oben, 256×256). Gegenprobe mit z-Test „immer“ für die Schattenpässe (nur Experiment, nicht im Code): der Schatten liegt genau unter der Figur.
* **Nicht geprüft:** Vergleich mit xemu (Stärke des Schattens, Partikel). In den Testszenen liegen die Partikel am oberen Bildrand bzw. verdeckt; ob sie im Original sichtbar sind, ist offen.

## Bekannte Grenzen

* Projizierte Lichter, Umgebungs- und Nebel-Maps werden weiter nicht gezeichnet (im Lauf 31 520 Draws mit solchen Effekten, gezählt). Die übrigen Maps des Mehrpass-Pfads (Dark, Detail, Glow, Decal, Bump) bleiben bei der Basis-Map wie seit M3.
* Nicht geclippte Schatten bekommen immer eigene Pässe (REN legt sie in bestimmten Fällen in den ersten Pass; gleiches Bild bei deckenden Maps). Das Spiel nutzt nur geclippte Schatten.
* Partikel und Schattenbild werden nicht interpoliert (60-Hz-Raster); bei 120 fps folgt die Kamera, die Partikel bewegen sich je Tick.
* Linien (Slot 53) werden vom Spiel nicht aufgerufen und sind weiter nicht umgesetzt.

## Nachtrag 2026-10-13: Avatarbild ohne schwarze Ausreißer

Der Nutzer hatte zweimal schwarze „Polygonausreißer“ im HUD-Porträt gemeldet (Kopf oben links in der Lebensanzeige). Das Porträt läuft nicht über Render to Texture. Es sind die `NiOutline`-Konturbänder (G1, Technik B), und die Fehler stecken im Gastcode des Originals:

* Der Bandbauer hängt viele Ketten an einen 404-Vertex-Strip. Dabei entstehen Brückendreiecke von Kettenende zu Kettenanfang.
* Beim Abschließen eines Strips beginnt die Nullfüllung bei Anzahl − 2. Dadurch fällt das letzte Paar der letzten Kette auf die linke untere Port-Ecke. Das ergibt einen dunklen Keil vom Ohr nach links unten, in jedem Bild.

Der Port zeichnet Band-Polygone jetzt als Liste ihrer Ketten und holt das genullte Kettenende über zwei Probes zurück (immer an). Die Konturen selbst bleiben unverändert. Analyse, Belege und Läufe stehen in `analysis\loader\g1_outlines.md` Abschnitt 12, Vertrag in `GFX_CONTRACT.md` 14, Vorher/nachher in `run\avatar\cmp_sheet.png`.
