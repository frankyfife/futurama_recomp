# M3d: Interpolation im Renderer – Entwurf

Stand: 2026-10-06. Auftrag: `analysis/loader/workflows/m3d_120hz.js`, Analyse B („Interpolation Design“). Rahmen: `docs/ROADMAP.md` Abschnitt „M3d – Bildraten bis 120 Hz (Interpolation)“, `docs/loader_design.md` 6 (M3d) und 8.4, `analysis/loader/pacing60.md` 7. Die Frame-Clock (halber Tick, Alpha, Takt) ist Gegenstand der parallelen Analyse A (`analysis/loader/m3d_frameclock.md`); dieser Bericht legt nur fest, was der Renderer von ihr braucht (3.1, 6.1).

Belege: Ghidra-Export `analysis/retail_eu_export` (nie Ghidra geöffnet), Capstone auf `orig/retail_eu/default.xbe`, eine Zensur aller NIF/UCF-Dateien der Disc und zwei Mitschnitte aller Draw-Aufrufe des laufenden Spiels mit einer instrumentierten Kopie des Hosts. Skripte: `analysis/loader/scripts/m3d_interp_*.py` (Abschnitt 10). Rohdaten und die instrumentierte Kopie liegen unter `run/analyse_m3d_interp/` (git-ignoriert). `src/` und `launcher/` sind unverändert.

„Tick“ = ein fester Logikschritt (`Game::Frame` 0xA2C20, dt = 1/60 s). „Frame“ = ein präsentiertes Bild (`DisplayFrame`, Slot 16). S[G] = Spielzustand nach Tick G.

---

## 0. Kurzfassung

1. **Was die Slots bekommen.** Alle 3D-Draws des Spiels laufen über `RenderShape` (Slot 48). Auf der Disc gibt es kein `NiTriStrips`, keine Partikelsysteme und keine `NiLines` (Zensur 2.2). Der `NiTransform*` von Slot 48–53 zeigt **immer** auf die Weltmatrix des zeichnenden Objekts (`obj+0x70`, `NiTriShape::vf33` 0x1166D0), der `NiBound*` auf dessen Welt-Bound (`obj+0x20`). Schlüssel der Historie: **(Objekt = Transform − 0x70, aktuelle Kamera, Vorkommen im Frame)**. Die Kamera kommt aus `SetCameraData` (Slot 47) als Zeiger in die `NiCamera` (`loc = cam+0xA0`).
2. **Messung im Frontend** (einzige 3D-Szene ohne Eingabe; der Attract-Modus ist ein XMV-Film, siehe 1.2): 323–355 Draws pro Frame, alle `NiTriShape`, eine Kamera (steht still), 3 geskinnte Draws mit je 5 Knochen. 37,8 % der Objekt-Ticks bewegen sich. In 0-Schritt-Frames ändert sich **kein** Transform (259 249 Paare geprüft): Das Frontend aktualisiert seinen Szenengraphen im `Render()` mit der Tick-Zeit (idempotent).
3. **Alpha-Konvention (Vorgabe an die Frame-Clock):** angezeigt wird `interp(S[G-1], S[G], α)`, α = Gewicht des neuesten Zustands, α = 1 heißt „kein Eingriff, bitgleich“. Mit der Halbtakt-Uhr bei 120 Hz soll **α = ½ in den Tick-Frames und α = 1 in den 0-Schritt-Frames** gelten (α = Rest/dt + ½). Dann ist jedes zweite Bild exakt der heutige Zustand, die zusätzliche Latenz beträgt **½ Tick (8,3 ms) statt 1 Tick**, und Culling-Fehler betreffen nur die Tick-Frames (3.1, 4).
4. **Mathe:** Translation linear, Rotation per Quaternion-Slerp aus den Zeilen der NiMatrix3, Skalierung linear. Weltrotationen sind orthonormal (Fehler ≤ 8,9·10⁻⁷) und nie gespiegelt (0 von 1,4 Mio. Draws, 0 von 105 233 lokalen Transforms der Disc). Bei gleichen Eingaben oder α = 1 wird **nicht gerechnet** (der Quaternion-Umweg ist nicht bitgleich, 2,4·10⁻⁷).
5. **Sprungregel** (gemessen an 537 echten Sprüngen, vor allem Keyframe-Schleifenumbrüche alle ≈ 7,9 s): Einrasten, wenn der Weg > 2 Bound-Radien **und** > 4× der Weg des vorigen Ticks ist, oder die Drehung > 45° **und** > 4× die vorige Drehung + 1°. Trefferquote 100 %, Fehlauslösungen 0,002–0,009 % der Bewegungs-Ticks (3.4).
6. **Restfehler** (Mittelpunkttest über zwei Ticks, ¼ davon für einen Tick): im ruhigen Frontend p99 0,17 px bei 640×480 (0,8 px bei 2880×2160). Größter Einzelfehler sind **Bahnkurven**: Weltraum-Lerp schneidet Kreisbögen ab; beim Einflug des Frontends (erste ≈ 80 Frames nach dem Kontextwechsel, 23°/Tick nahe der Kamera) bis 92 px bei 640 px Breite. Abhilfe ist eine optionale **hierarchische Interpolation** über die Elternkette (Stufe 1b, 3.5).
7. **Skinning** läuft heute im **Original** `FUN_0014F710` (über `core::CallGuestThiscall`, `ni_extract.cpp` Skin()). Es kann interpolierte Knochen ohne Gastschreibzugriff verwenden: eine **Ersatz-`NiSkinInstance` in der Arena** (Gastspeicher), deren Wurzel die Einheitsmatrix und deren Knochen die interpolierten relativen Posen `Wurzel⁻¹·Knochen` tragen. `FUN_0014F710` liest nachweislich nur `+0x08/+0x0C/+0x10` und je `+0x70` (3.8).
8. **Bildgetriebene Objekte** (Animation mit `g_frameDelta` 0x2E7B88 im Render: Vision-Overlay `FUN_00045F10`, `FUN_000C8E90` +0x11C, Frontend +0x1C8) ändern sich auch in 0-Schritt-Frames. Erkennung: Änderung innerhalb derselben Tick-Generation → „bildgetrieben“, ohne Interpolation zeichnen (3.9).
9. **Culling** passiert im Spiel mit dem Tick-Zustand (Kamera-Ebenen aus `NiCamera::vf39` 0xFFDA0). Folge bei α = ½: Objekte an der nachlaufenden Bildkante verschwinden einen Frame (8,3 ms) früh, Breite des Streifens bei schnellem Schwenk (180°/s) ≈ 17 px bei 640 px. Empfehlung: zunächst nichts tun, Schutzrand nur nach Messung mit Freecam (M4b) (4).
10. **Kosten:** Mikrobenchmark 4,4 ns Erfassung + 18,7 ns Interpolation je Draw (40 % bewegt) → 0,05 ms je Tick-Frame bei 2000 Draws. Speicher: zwei Tabellen à 4096 Einträge zu 128 Byte = 1 MiB. Die Frame-Arbeit lag im 120-fps-Lauf bei Ø 0,59 ms (Budget 8,33 ms).
11. **Korrekturen am Plan:** Der Attract-Modus ist kein Spielgeschehen, sondern der Film `Attract-PAL.xmv` (bis M6 ein Frame lang). Die Ausbaustufe „Partikel“ entfällt fast ganz (kein `NiParticleSystemController` auf der Disc; einziges `NiParticles` ist ein Einzelpunkt aus `FUN_0006DC60`, Slot 52 ist im Port noch nicht implementiert). UV-, Alpha-, Farb- und Sichtbarkeitsanimationen bleiben im Tick-Raster. Outlines (`NiOutline`, nur 26 Knoten in 2 Dateien) sind 2D-Polygone aus dem Tick-Zustand und lassen sich auf Renderer-Ebene nicht mitinterpolieren (3.10).

---

## 1. Ausgangslage und Messgrundlage

### 1.1 Ablauf eines Frames (Belege `analysis/loader/startup.md` 3.1/3.2, `renderer.md` 3.3)

`CApp::Frame` 0xD2630 → `Game::Frame` 0xA2C20: ganze Ticks (`Update`), danach genau ein `Render()` des aktuellen Kontexts. `Render()` ruft (direkt oder über Unterfunktionen) `NiCamera::Click` 0x100C50 auf: `SetCameraData` (Slot 47) → `BeginScene` (45) → Culling und `Display` (vf33) der Szene → Alpha-Sortierer → Screen-Polygone/-Texturen (54/55) → `EndScene` (46). `Render()` bekommt keinen Alpha-Wert.

Wo der Szenengraph aktualisiert wird, entscheidet, ob sich ein Objekt in einem 0-Schritt-Frame ändert:

| Kontext | Aktualisierung | Beleg |
|---|---|---|
| Frontend | `Update` 0x22B30 (Gap-Code) zählt die Kontextzeit `+0x78 += dt` (0x22E26) und ruft `NiAVObject::Update` 0x101840 (0x22D6C); `Render` 0x21B60 ruft `NiAVObject::Update(+0x78, true)` mehrfach mit **derselben Tick-Zeit** und danach `NiCamera::Click` | Dekompilat 0x21B60, Capstone 0x22B30–0x22E2C |
| Ingame | `Render` 0x2A320 → Session-Render `FUN_000C8E90`: nur `NiNode::vf38` 0x104F40 (= UpdateWorldBound, nur Bounds) auf der Wurzel, dann die Zeichenlisten | Dekompilat 0xC8E90, 0x104F40 |
| Render-zeitige Animation mit `g_frameDelta` | Frontend `+0x1C8`→vf1(g_frameDelta) (0x21B60), `FUN_000C8E90` `+0x11C`→vf1(min(g_frameDelta, 0,1)), `FUN_00045F10` (`+0x1C += g_frameDelta`, `NiAVObject::Update`, Z-Clear, `Click`; Klasse mit `Cockpit\Leela_vision.nif`/`Bender_vision.nif`, `FUN_000462B0`), Lichtdimmer `FUN_000C0AC0`, Blende `FUN_000EEF80` | `grep DAT_002e7b88` im Dekompilat (5 Lesestellen) |

### 1.2 Was ohne Eingabe erreichbar ist

Ohne Eingabe (M4) erreicht der Port Intro, Frontend und Attract. **Attract ist ein Film**: Der Kontext Attract (`0x2BA80`, vtable 0x2952DC) öffnet `Video\Attract-PAL.xmv`; der XMV-Stub beendet ihn sofort (`docs/loader_design.md` 8.2, Log `run/analyse_m3d_interp/lock60`: `Frontend -> Attract (CApp::Frame #4495)`, `Attract -> Frontend (#4496)`). Die „Gameplay-Demos“ der xemu-Aufnahmen (`run/xemu_ref/ref_attract.png`) sind Filmbilder. Die einzige bewegte 3D-Szene für Messungen ist daher das Frontend (`menu/frontend.nif`: 707 `NiTriShape`, 4 Skins, 93 Billboards) samt Einflug und Ladebild. Ingame-Szenen werden ab M4/M8 messbar; für sie stützt sich dieser Entwurf auf die Zensur der Disc (2.2) und die Analyse des Codes.

### 1.3 Mitschnitte

Instrumentierte Kopie: `git archive HEAD` nach `run/analyse_m3d_interp/tree`, Patch nur in `gfx/nipc_renderer.cpp` (`run/analyse_m3d_interp/capture_patch.diff`): Slot 47, 48–51, 54/55 und 16 schreiben je einen Datensatz (Kamera-Vektoren, Frustum, Port; je Draw Transform-Zeiger, Daten, Skin, Property-State, vtable und Eltern-Zeiger des Objekts, Welt-`NiTransform`, Welt-Bound, Welt-Transform des Elternknotens, bei Skins Wurzel und alle Knochen-Welttransforms; je Frame die Zahl der festen Schritte aus `xapi::GetGameTickStats()`). Build RelWithDebInfo, Läufe auf dem 120-Hz-Messrechner:

| Lauf | Konfiguration | Mitschnitt | Ergebnis |
|---|---|---|---|
| `lock60` | Standard (Lockstep, 60 Hz, `pacing = auto`), 5200 Frames | Frames 900–5199: Frontend, Kontextwechsel Frontend → Attract → Frontend, Ladebild, neues Frontend | 4300 Frames, 1 403 402 Draws, 5188 Ticks (1 × 2, 1 × 5, 2 × 6 Schritte = Aufholen nach Kontextwechseln) |
| `rt120` | `[game] frame_clock = realtime`, `[render] frame_rate = 120` (vblank k = 1) | Frames 2000–3499: Frontend | 1500 Frames, davon **749 mit 0 Schritten** und 750 mit 1 Schritt; 519 190 Draws |

---

## 2. Was die Draw-Slots bekommen

### 2.1 Slots, Argumente und Schlüssel

| Slot | Aufrufer | Argumente (Ziel des Zeigers) | Historien-Schlüssel | Behandlung |
|---|---|---|---|---|
| 47 `SetCameraData` | `NiCamera::Click` 0x100C50 (0x100C8B–0x100CC7) | `cam+0xA0` Weltposition, `+0x110` dir, `+0x11C` up, `+0x128` right (= Zeilen 0/1/2 der Weltrotation, `NiCamera::vf39` 0xFFDA0), `+0x134` `NiFrustum`, `+0x14C` Port | Kamera = `loc − 0xA0` (+ Vorkommen) | interpolieren (3.7) |
| 48 `RenderShape` | `NiTriShape::vf33` 0x1166D0 | `obj+0xC8` Daten, `obj+0xCC` `NiSkinInstance*`, **`obj+0x70` Welt-`NiTransform`**, `obj+0x20` Welt-`NiBound`; Renderer `+0xC/+0x10` = `obj+0xC0/+0xC4` | (Objekt, Kamera, Vorkommen) | starr (3.3), Skin (3.8) |
| 49/50/51 | `NiTriangles`/`NiTriStrip`/`NiTriStrips::vf33` | wie 48 (49/50 ohne Skin) | wie 48 | wie 48; auf der Disc gibt es diese Klassen nicht (2.2) |
| 52 `RenderPoints` | `NiParticles::vf33` 0x10F820 (auch `NiRotatingParticles`), `NiAutoNormalParticles::vf33` 0x10FC60 | `obj+0xC8`, `obj+0x70`, `obj+0x20` | wie 48 | starr; Slot im Port noch nicht gezeichnet (P2) |
| 53 `RenderLines` | `NiLines::vf33` 0x10E6F0 | `obj+0xC8`, `obj+0x70`, `obj+0x20` | wie 48 | starr; auf der Disc nicht vorhanden |
| 54 `RenderScreenPolygon` | `FUN_00106AF0` aus `Click` (Kameraliste `cam+0x164`, Einzelpolygon `cam+0xC0`) | `NiScreenPolygon*` (2D in [0,1], Tiefe) | – | **nicht** interpolieren (HUD, Menü, Film-Quad, Outlines 3.10) |
| 55 `RenderScreenTexture` | `FUN_0010AF70` aus `Click` (Liste `cam+0x194`) | `NiScreenTexture*` (vortransformierte Pixel) | – | nicht interpolieren (Intro-Bilder) |

Belege: Dekompilat der genannten `vf33`; `renderer.md` 3.2, Key Fact 5 (`NiTriShape::vf33` pusht `this+0x20`, `this+0x70`, `[this+0xCC]`, `[this+0xC8]`). Laut `renderer.md` 3.1/3.2 ruft nur NI-Code die Slots 48–53 auf, und zwar nur aus diesen `vf33`; der Spielcode ruft nur 15/16/17/21/22/27/39/71. Mitschnitt: Bei allen 1 403 402 + 519 190 Draws lag an `Transform − 0x70` die vtable `0x2A7CE0` (`NiTriShape`).

Weitere Wege, auf denen dieselben Objekte gezeichnet werden:

- **Alpha-Sortierer:** `NiTriShape::vf33` übergibt das Objekt zuerst an den Accumulator (`FUN_00140140` → `acc->vf13`, `NiAlphaAccumulator::vf13` 0x2ADE0). Angenommene Objekte zeichnet `NiAlphaAccumulator::vf12` 0x2A590 am Ende derselben `Click` sortiert über `FUN_0010E9C0` → `FUN_00140280`: Accumulator an `renderer+8` kurz abgehängt, dann erneut `obj->vf33` (0x140280, `call [vt+0x84]`) – also derselbe Slot mit demselben `obj+0x70`. Nur die Reihenfolge hängt von der (Tick-)Kamera ab.
- **Billboards** (`NiBillboardNode::vf33` 0x110950 → `FUN_00110360(cam)`): Die Rotation des Knotens wird **beim Zeichnen** aus der Kamera neu berechnet (Welt = Eltern `+0x14`·lokal, dann Drehung nach Modus `(flags & 0x1C00) >> 10`: 0 = zur Kamera, 1 = um die Hochachse zur Kameraposition, 2–4 Varianten), danach `NiNode::vf33` für die Kinder. Für den Renderer sind die Kinder gewöhnliche Objekte, deren Welttransform pro Tick (mit der Kamera) wechselt; Interpolation von Billboard-Rotation und Kamera passt zusammen (bei Modus 0 ist die Billboard-Drehung eine feste Funktion der Kameradrehung, Slerp beider bleibt konsistent). Eine Kamera, die dasselbe Billboard in einem Frame zweimal zeichnet (zwei Kameras), braucht getrennte Historien: deshalb die Kamera im Schlüssel.
- **Switch/LOD/Flipbook:** `NiSwitchNode::vf33` 0x1134E0 zeigt nur das aktive Kind und holt dessen `Update` nach, wenn es seit der letzten Aktualisierung des Knotens nicht dabei war (Zähler `+0xE8`, Stempel je Kind `+0xF0`). `NiLODNode::vf33` 0x114250 wählt das Kind beim Zeichnen nach dem Abstandsquadrat zur Kamera (`cam+0xA0`, Faktor `cam+0x244`), `NiFltAnimationNode::vf33` 0x113DC0 nach der Zeit (`FUN_00113D00(+0xE4)`). Ein umgeschaltetes Kind ist für den Renderer ein **neues** Objekt (erste Sichtung, Einrasten), das alte verschwindet. Bei Flipbooks ist das richtig (diskrete Bilder).
- **Culling** `FUN_00101A30`: Objekte mit `flags(+0xB0) & 1` (app-culled, z. B. `NiVisController`) und Objekte, deren Welt-Bound außerhalb einer Frustum-Ebene liegt (`FUN_001019B0`, Ebenen `cam+0x1B0`, Maske `cam+0x228`, Anzahl `cam+0x240`), werden nicht gezeichnet → Historienlücke.
- **Portale:** `NiRoomGroup::vf33` 0x11F640 sucht beim Zeichnen den Raum, der die Kameraposition enthält (`FUN_0011F1D0(…, cam+0xA0, letzter Raum +0xEC)`), und zeigt ihn über `FUN_00101A30` an; `NiRoom::vf33` 0x11DAB0 setzt das über die Portale fort – ebenfalls mit der Tick-Kamera.
- **Outlines:** `NiOutline::vf33` 0x13E840 siehe 3.10.

### 2.2 Inhalte der Disc (`m3d_interp_nifscan.py`, `m3d_interp_levelsize.py`)

820 NIF/UCF-Dateien (alle „NetImmerse File Format, Version 4.2.1.0“) aus `FutPAL00–04.img` und `SysPAL.img`:

| Klasse | Anzahl (Dateien) | Folgerung |
|---|---|---|
| `NiTriShape` | 36 081 (237) | alle Geometrie über Slot 48 |
| `NiTriStrips`, `NiTriShapeDynamicData`, `NiLines`, `NiParticles*`, `NiParticleSystemController`, alle Partikel-Modifier | 0 | Slots 49–53 und Partikelsysteme spielen keine Rolle |
| `NiSkinInstance`/`NiSkinData` | 983 (57), davon 975 in `chars`, je 4 in `levels` und `menu`; Knochen min/Median/p90/max 1/3/18/49, höchstens 1650 geskinnte Vertices je Skin; 402 `NiSkinPartition` | Skinning ist auf wenige Figuren beschränkt; viele Figuren sind starre Teile an animierten Knoten (z. B. `chars/skel_hth.ucf`: 324 Shapes, 0 Skins, 144 LOD-Knoten) |
| `NiLODNode` | 733 (663 in `chars`) | LOD-Wechsel = erste Sichtung |
| `NiSwitchNode` / `NiFltAnimationNode` | 245 / 190 | Umschalten = erste Sichtung |
| `NiBillboardNode` | 385 (62 Dateien; Modus 0: 231, Modus 1: 154) | Effekte (Explosionen), Level, Frontend (93) |
| `NiRoom` / `NiRoomGroup` | 790 / 24 | Portal-Culling in Levels |
| `NiOutline` | 26 in 2 Dateien: `cockpit/chickencockpit.nif`, `pause/level_breakdown_menu.nif` | Outlines sind selten (3.10) |
| `NiKeyframeController` | 81 099 | fast alle Bewegung sind Keyframes in Sekunden (timing60.md 4.4) |
| `NiVisController` / `NiUVController` / `NiAlphaController` / `NiMaterialColorController` / `NiFlipController` / `NiGeomMorpherController` | 3273 / 1559 / 1578 / 63 / 7 / 1 (`levels/testlevel/defs.nif`) | bleiben im Tick-Raster (3.10) |
| Lichter | 1400 Punkt, 152 Richtung, 60 Ambient, 0 Spot; 66 `NiTextureEffect` (alle in `chars`) | 3.10 |
| lokale Transforms aller `NiAVObject`-Blöcke | 105 233: **0 gespiegelt**, Orthonormalitätsfehler max 5,7·10⁻⁶, Skalierung ≠ 1 in 1879 Fällen (0,0056 … 231,8) | Slerp ohne Sonderfälle (3.3) |

Größte Szenen: `levels/level5-3/level5_fry.nif` 2153 `NiTriShape` (24 Räume), Figuren bis 324 Shapes (mit allen LOD-Stufen), Effekte bis 254, `menu/frontend.nif` 707. Partikel erzeugt nur der Spielcode selbst: `FUN_0006DC60` legt mit `FUN_0010F680(1, …)` ein `NiParticles` mit **einem** Vertex an (Glanzpunkt mit eigener Alpha-/Z-Property, Klasse mit vtable um 0x29C340, Texturen `WaterShine.tga`, `LavaGlow.tga` …). Dieselbe Klasse (`FUN_0006E9A0`) klont einen Pool von `NiAVObject`s (`FUN_000FE090`) – gepoolte Effektobjekte, die wiederverwendet werden (3.2, 3.4).

### 2.3 Laufzeit im Frontend (`m3d_interp_capture.py`, Ausgaben `run/analyse_m3d_interp/*/capture_stats.out`)

| Größe | `lock60` | `rt120` |
|---|---|---|
| Draws pro Frame (Frames mit Draws) | min 323, Median 346, p99 352, max 355 (4060 von 4300) | min 341, Median 346, max 350 |
| geskinnte Draws pro Frame | 3 (je 5 Knochen) | 3 (je 5 Knochen) |
| Kameras | 1 `SetCameraData` pro Frame (23 Frames ohne); Frontend `0x348E9C0`, Ladebild `0x325C9E0`, neues Frontend `0x34888B0` | 1 pro Frame, `0x346E9C0` |
| Kamerabewegung pro Tick | keine (0 von allen Paaren) | keine |
| Schlüssel mehrfach in einem Frame | 0 | 0 |
| Identitätswechsel (gleicher Zeiger, andere vtable/Daten/Skin/Property-State) | 0 | 0 |
| erste Sichtung pro Frame | Median 3, p99 10, max 323 (nur bei Kontextwechseln: Frame 905 und 4744) | Median 1, p99 9 |
| bewegte Objekt-Ticks | 524 435 von 1 388 307 (37,8 %) | 94 841 von 256 994 (36,9 %) |
| Weg pro Tick (bewegte) | p50 0,028, p99 0,196, max 5,68 Einheiten; in Bound-Radien p50 0,088, p99 2,52, max 180 | p50 0,028, p99 0,17 |
| Drehung pro Tick | p50 0,75°, p99 4,97°, max 179° | p99 4,67°, max 179° |
| Skalierungswechsel | 4904 (Faktor 0,86 … 1,33 pro Tick, Einflug/Ladebild) | 0 |
| Weltrotationen | Orthonormalitätsfehler p99,9 6,7·10⁻⁷, max 8,9·10⁻⁷; 0 gespiegelt | gleich |
| relative Knochenposen (`Wurzel⁻¹·Knochen`) pro Tick | 45 285 gleich, 15 570 bewegt | 8317 / 2933 |
| **0-Schritt-Frames: Transform geändert** | – | **0 von 259 249** |

Bewegte Objekte haben immer einen bewegten Elternknoten (`moving with static parent world 0`): Animation sitzt an den `NiNode`s, die Geometrie hängt mit fester lokaler Lage darunter (3.5).

---

## 3. Interpolation

### 3.1 Zeitmodell und Alpha

**Vertrag Renderer ↔ Frame-Clock.** Pro Frame liefert die Uhr (Analyse A) `tick` (abgeschlossene feste Schritte beim `Render()`, = Generation G) und `alpha` ∈ [0, 1]. Der Renderer zeigt `interp(S[G-1], S[G], alpha)`; `alpha = 1` bedeutet „Gastdaten unverändert“ (bitgleich zu heute). Die Uhr bildet alpha aus dem Rest `rest = jetzt − last` (`app+0xC8`, startup.md 3.2) und einer festen Anzeigelatenz L:

`alpha = clamp(1 − (L − rest) / dt, 0, 1)` mit L = größter Rest, den die Uhr erzeugen kann.

| Uhr | Rest | L | alpha | Zusatzlatenz |
|---|---|---|---|---|
| Lockstep 60 Hz (heute) | immer 0 | 0 | immer 1 | 0 |
| **Halbtakt 120 Hz** (Analyse A) | 0 im Tick-Frame, dt/2 im 0-Schritt-Frame | **dt/2** | **½ im Tick-Frame, 1 im 0-Schritt-Frame** | **½ Tick = 8,3 ms** |
| Echtzeit / unregelmäßige Raten | 0 … dt | dt | rest/dt | 1 Tick = 16,7 ms |

Folge für 120 Hz (Frames f0, f1, … mit Tick in jedem geraden Frame):

| Frame | Tick | alpha | gezeigt |
|---|---|---|---|
| f0 | G | ½ | Mitte(S[G-1], S[G]) |
| f1 | – | 1 | S[G] (exakt wie heute) |
| f2 | G+1 | ½ | Mitte(S[G], S[G+1]) |
| f3 | – | 1 | S[G+1] |

Die Zeit läuft gleichmäßig in halben Ticks. Die naheliegende Lesart „alpha = rest/dt = ½ im 0-Schritt-Frame und 1 im Tick-Frame“ zeigt dagegen S[G], Mitte(S[G-1], S[G]), S[G+1] … – **die Bewegung liefe jedes zweite Bild rückwärts**. Die Konvention „alpha = rest/dt“ (Gaffer) ist nur zusammen mit „0 im Tick-Frame“ richtig und kostet einen vollen Tick Latenz. Diese Festlegung gehört deshalb in die Schnittstelle (6.1) und in einen Unit-Test der Uhr: Die angezeigte Zeit `G − 1 + alpha` muss pro Frame um genau ½ steigen.

Die Simulation der parallelen Frame-Clock-Analyse (`scripts/m3d_clock_sim.py`, Stand dieses Berichts) untersucht beide Phasenlagen als „align A“ (alpha ≈ 0 / ½, entspricht L = dt) und „align B“ (alpha ≈ ½ / 1, entspricht L = dt/2); dieser Entwurf empfiehlt B.

Die ROADMAP nennt „etwa ein Logik-Tick (16,7 ms) zusätzliche Verzögerung“. Mit L = dt/2 sind es 8,3 ms, weil der Tick-Frame bereits den halben Weg zum neuen Zustand zeigt und der 0-Schritt-Frame den neuen Zustand vollständig. Eine Extrapolation (L < größter Rest) wird nicht empfohlen: Sie würde bei jedem Richtungswechsel und jeder Kollision überschießen.

Draws, die innerhalb der Schrittschleife entstehen (Ladebilder und Löschbilder aus `Update`: `FUN_0002A660` 8×, `FUN_000A62B0`, `FUN_000A26A0`, Film `FUN_000EE080`), bekommen alpha = 1. Frames mit mehr als einem Schritt (Aufholen nach Ladehängern) zeigen S[G] (alpha = 1, `snapAll`).

### 3.2 Schlüssel, Identität, Generation

- **Schlüssel** = (Objekt = `transform − 0x70`, Kamera = `loc − 0xA0` der letzten `SetCameraData`, Vorkommen). Das Vorkommen zählt, wie oft dieselbe (Objekt, Kamera) im laufenden Frame schon gezeichnet wurde (Frame-Stempel im Eintrag). Im Frontend nie > 0 (2.3); vorgesehen für Spielcode, der ein Objekt verschoben mehrfach zeichnet (Kameraliste `cam+0x17C`, Text).
- **Identität:** Zum Eintrag gehört die Signatur (vtable `obj+0`, Daten `obj+0xC8`, Skin `obj+0xCC`, Property-State `obj+0xC0`). Weicht sie ab, gilt das Objekt als neu.
- **Lebensdauer:** Probe auf den gemeinsamen Basis-Destruktor `NiAVObject::~NiAVObject` = `FUN_00102CE0` (13 direkte Aufrufer, alle abgeleiteten Destruktoren inkl. `NiNode` `FUN_001066B0`, `NiGeometry` `FUN_001447B0`, `NiCamera` `FUN_000FFB60`; Prolog `6A FF 68 93 A4 17 00` = 7 Byte, lageunabhängig wie bei `probe:Game::Init`). Sie trägt die Adresse in eine Menge „zerstört seit Tick G-1“ ein; ein Eintrag, dessen Objekt darin steht, hat keine Vorgeschichte. Damit ist Wiederverwendung eines Zeigers innerhalb eines Ticks sicher erkannt.
- **Gepoolte Objekte** (`FUN_0006E9A0`) werden nicht zerstört, sondern neu platziert. Das fängt die Sprungregel (3.4) ab; ein Respawn näher als 2 Bound-Radien wird für einen Frame (8,3 ms) verschmiert und ist unsichtbar.
- **Generation:** Jeder Schnappschuss trägt den Tick, in dem er erfasst wurde. Interpoliert wird nur, wenn der Vorgänger aus genau G−1 stammt. Lücken (Culling, Sichtbarkeit, Ladehänger, Kontextwechsel) führen automatisch zum Einrasten.
- **Kamera im Schlüssel:** Wechselt das Spiel die `NiCamera` (anderes Objekt, z. B. Schnitt in Zwischensequenzen), rasten alle Objekte dieser Szene ein – gewollt. Legt das Spiel die Kamera jeden Frame neu an (im Frontend nicht der Fall), interpoliert nichts mehr; die Statistik „erste Sichtung“ zeigt das sofort.

### 3.3 Starre Objekte: Mathe

`NiTransform` (0x40): `rotate[3][4]` (Zeilen = Bilder der Achsen, Zeilenvektoren, `v' = s·v·R + t`), `translate` +0x30, `scale` +0x3C (GFX_CONTRACT 3, `ni_guest_layouts.h`). Für α ∈ (0, 1):

1. **Schnellpfade (bitgleich):** α = 1 → Gastzeiger unverändert durchreichen. Prev und Cur bitweise gleich (62 % aller Objekt-Ticks im Frontend) → Cur durchreichen. Begründung: Der Umweg Matrix → Quaternion → Matrix verändert Rotationen um bis zu 2,4·10⁻⁷ (Benchmark 5.3); ohne Schnellpfad würden ruhende Objekte in jedem Tick-Frame minimal flimmern (Vertex-Snapping auf 1/16 Pixel, GFX_CONTRACT 9).
2. **Translation:** `t = t0 + (t1 − t0)·α`.
3. **Rotation:** Quaternion aus den Zeilen (Shepperd: Zweig nach größter Diagonale, Spur > 0), Vorzeichen auf kürzesten Weg, Slerp (Nlerp ab Kosinus > 0,9995), normieren, zurück in Zeilen; Spalte 3 = 0. Eingangsprüfung: Orthonormalitätsfehler ≤ 10⁻³ (gemessen max 8,9·10⁻⁷ in der Welt, 5,7·10⁻⁶ lokal auf der Disc) und gleiches Vorzeichen der Determinante (nie negativ gemessen). Sonst einrasten und zählen – ein Polar-Zerlegungspfad ist bei diesen Daten unnötig.
4. **Skalierung:** linear; einrasten bei Vorzeichenwechsel oder Faktor außerhalb [0,5; 2] pro Tick (gemessen 0,86 … 1,33).
5. **Weltskalierung und `NORMALIZENORMALS`** (FUN_00159200) folgen der interpolierten Skalierung, weil das Ergebnis als `NiTransform` an `ni_extract` geht (6.2).

Der Quaternion-Rundweg im Double-Rechenmodell der Prüfskripte ist auf 3,4·10⁻⁶° genau (`midpoint.out`), die Winkelfehler der Interpolation (3.6) liegen weit darüber; Single-Precision reicht.

### 3.4 Sprünge (Einrasten)

Gemessen an `rt120` (ruhiges Frontend) und `lock60` (zusätzlich Einflug, Ladebild, Kontextwechsel) mit `m3d_interp_snaprules.py`. Referenz „Sprung“ (nicht kausal, nur zur Bewertung): ein Tick, dessen Weg > 2 Radien und > 4× **beide** Nachbarschritte ist, oder dessen Drehung > 30° und > 4× beide Nachbardrehungen (+1°). Das sind fast ausschließlich **Keyframe-Schleifenumbrüche**: Gruppen von Objekten springen alle 474–476 Ticks (≈ 7,9 s) um 4,6 Einheiten (22–74 Radien) und drehen um 115° bzw. 179° (`m3d_interp_outliers.py`, `rt120/outliers.out`). Interpoliert sähe man dort einen Frame lang eine Zwischenpose, die es nie gab.

| Regel | `rt120`: Treffer / verfehlt / Fehlauslösung (101 Sprünge, 118 735 Bewegungs-Ticks) | `lock60` (436 / 648 210) |
|---|---|---|
| R1 Weg > 4 r | 81 / 20 / 87 (0,073 %) | 355 / 81 / 719 (0,111 %) |
| R2 Weg > 2 r und > 4·Weg_vorher + 0,01 r | 93 / 8 / 2 | 404 / 32 / 60 |
| R3 Drehung > 90° | 80 / 21 / 0 | 320 / 116 / 36 |
| R4 Drehung > 45° und > 4·Drehung_vorher + 1° | 80 / 21 / 0 | 320 / 116 / 24 |
| **R2 oder R4 (Vorschlag)** | **101 / 0 / 2 (0,002 %)** | **436 / 0 / 60 (0,009 %)** |
| R2 oder R3 | 101 / 0 / 2 | 436 / 0 / 72 |

Ein reiner Radius-Schwellwert (R1) rastet dauerhaft schnelle kleine Objekte ein (z. B. `0x033811D0`: 0,17 Einheiten/Tick ≈ 4,2 Radien über viele Ticks) – das ergäbe 60-Hz-Ruckeln genau bei den schnellsten Objekten. Ein reiner Winkel-Schwellwert (R3) rastet schnell drehende Teile ein (72°→144°/Tick, Ladebild/Einflug). Die Kombination mit dem Vergleich zum vorigen Tick trennt beides. Eine Fehlauslösung kostet ein Objekt einen Frame mit ½ Tick Zeitversatz.

Weitere Einrastgründe: erste Sichtung, Generationslücke, Signaturwechsel, zerstört (3.2), Determinanten-/Orthonormalitätsprüfung und Skalierung (3.3), `snapAll` der Uhr (Kontextwechsel, Aufholen, Uhr-Reset, erstes Bild nach Aktivierung) und „bildgetrieben“ (3.9). Für die Regel braucht jeder Eintrag zusätzlich Weg und Drehwinkel des vorigen Ticks (2 Floats).

**Erste Sichtung im Tick-Frame:** Ein neu sichtbares Objekt wird mit S[G] gezeichnet, also ½ Tick voraus, einen Frame lang. Im Frontend sind das 1–10 Objekte pro Frame (umgeschaltete Teilbäume: Flipbooks, Sichtbarkeit). Der Gedanke, solche Objekte über die Bewegung ihres Elternknotens zu interpolieren, trägt nicht: Nur 3 von 2600 (`rt120`) bzw. 28 von 14 124 (`lock60`) ersten Sichtungen hatten einen Elternknoten, der im Tick davor schon (über ein anderes Kind) bekannt war (`m3d_interp_firstsight.py`). Auch „ein Tick unsichtbar lassen“ (zeitlich konsistent, weil die Anzeige ½ Tick hinterherläuft) wird verworfen: Bei LOD- und Switch-Wechseln fehlte das Objekt dann einen Frame ganz.

### 3.5 Bahnkurven: Welt- oder hierarchische Interpolation

Lineare Interpolation der Weltposition verkürzt Kreisbahnen. Dreht sich ein starr angehängtes Objekt pro Tick um θ und legt dabei den Weg d zurück, liegt der lineare Mittelpunkt d·tan(θ/4)/2 innerhalb des Bogens. `m3d_interp_chord.py` schätzt das für jeden Bewegungs-Tick (Sprünge ausgenommen) in Pixeln bei 640 px Breite mit der Kamera des Mitschnitts:

| Lauf | p50 | p99 | p99,9 | max | > 1 px |
|---|---|---|---|---|---|
| `rt120` (ruhiges Frontend) | 0,005 | 0,16 | 0,41 | 1,94 | 56 von 94 738 |
| `lock60` (mit Einflug) | 0,005 | 0,17 | 4,3 | **104** | 1614 von 523 945 |

Die großen Werte stammen aus dem **Einflug ins Frontend** (Frames 905–990 und 4744–4830, also ≈ 1,4 s nach jedem Kontextwechsel): Objekte wie `0x348D260` bewegen sich 0,96 Einheiten und 23,3° pro Tick dicht vor der Kamera. Der Mittelpunkttest (3.6) bestätigt die echte Bogenbahn. Bei α = ½ läge das Objekt dort bis zu 92 px (640 px) bzw. ≈ 400 px (2880 px) innen – in jedem zweiten Frame, für etwa eine Sekunde. Bei 0,96 Einheiten/Tick dicht vor der Kamera ist das Objekt allerdings selbst so schnell, dass der Fehler kaum zu sehen sein dürfte; die Sichtprüfung entscheidet.

Ein Elternknoten allein hilft nicht: Bei 99,99 % der bewegten Draws ist die lokale Lage zum Elternknoten konstant und liegt im Ursprung des Knotens (`m3d_interp_hier.py`: 94 838 von 94 841, 524 390 von 524 435). Dann ist „Eltern interpolieren, lokal anhängen“ dasselbe wie Welt-Interpolation (Differenz ≤ 2·10⁻¹³ px). Die Bögen entstehen weiter oben in der Kette. Ein Screw-/Dual-Quaternion-Verfahren pro Objekt träfe den Bogen, krümmt aber geradlinige Wege von Objekten, die sich unterwegs drehen (Figur läuft geradeaus und dreht sich) um denselben Betrag.

**Stufe 1b (optional): hierarchische Interpolation.** Für jedes gezeichnete Objekt wird die Elternkette (`+0x14`, bis zur Wurzel, höchstens 16 Ebenen) erfasst; Knoten-Historie im selben Tabellentyp (Schlüssel Knoten + Kamera, weil Billboard-Welten kameraabhängig sind). Im Tick-Frame: `W_n(α) = W_p(α) · interp(L_n,prev, L_n,cur, α)` mit `L = W_p⁻¹·W_n`, Wurzel direkt interpoliert, Sprungregel je Knoten auf L. Das bildet die Keyframe-Animation (lokale Rotationen per Slerp, lokale Translationen linear) so nach, wie NetImmerse sie rechnet, und trifft Bögen starrer Hierarchien exakt. Kosten: jeder sichtbare Vorfahr einmal pro Frame (Frontend: höchstens 879 Knoten in `menu/frontend.nif`), ≈ 2 Matrixprodukte je Knoten, also < 0,1 ms. Empfehlung: Stufe 1 mit Welt-Interpolation bauen, den Einflug als Sichtprüfung festlegen und Stufe 1b nur bei sichtbarem Wackeln nachziehen (Schalter `[render] interpolation_hierarchy`, Entwickler-Schlüssel).

### 3.6 Fehlerabschätzung mit echten Zwischenständen

Ohne Zwischenzustand im Spiel prüft `m3d_interp_midpoint.py` die Interpolation über **zwei** Ticks gegen den echten Zustand in der Mitte: `interp(S[G-1], S[G+1], ½) − S[G]`. Für Bewegungen mit stetiger Beschleunigung ist der Mittelpunktfehler über ein Intervall h gleich h²|a|/8; die echte Ein-Tick-Interpolation hat also etwa ¼ davon. Sprünge nach 3.4 sind ausgenommen.

| Lauf | Tripel | Weg-Fehler p50 / p99 (Einheiten; Schritt p50 0,0196) | relativ zum Schritt p50 / p99 | Drehfehler p50 / p99 | Pixel (640) p50 / p99 | ⇒ ein Tick p99 bei 640 / 2880 px |
|---|---|---|---|---|---|---|
| `rt120` | 116 455 | 0,00012 / 0,0052 | 0,9 % / 23 % | 3·10⁻⁶° / 0,31° | 0,017 / 0,68 | **0,17 / 0,76 px** |
| `lock60` | 637 847 | 0,00013 / 0,013 | 1,0 % / 24 % | 3·10⁻⁶° / 0,22° | 0,017 / 1,24 | 0,31 / 1,4 px |

Die größten Abweichungen (bis 153 px im Zwei-Tick-Test) sind Objekte, die aus der Ruhe heraus in einem Tick 2 Einheiten springen (Schritte 0,0 → 0,0 → 2,18) – im Ein-Tick-Fall ist die Mitte dort die beste Schätzung – und die Einflug-Bögen aus 3.5. Ergebnis: Lineare Interpolation in der Welt trifft die echten Bahnen des Frontends im ruhigen Betrieb auf Bruchteile eines Pixels.

### 3.7 Kamera

- **Erfassung** in Slot 47: Kamera-Schlüssel, `loc`, die Zeilen `dir/up/right` (= Weltrotation der Kamera, `NiCamera::vf39` 0xFFDA0 kopiert `+0x70/+0x80/+0x90` nach `+0x110/+0x11C/+0x128`), Frustum (l, r, t, b, near, far), Port.
- **Interpolation:** `loc` linear; Rotation per Slerp der Matrix mit den Zeilen (dir, up, right) – orthonormal, weil aus der Weltrotation kopiert –, zurück in die drei Vektoren; Frustum-Tangenten l/r/t/b linear (Zoom); near/far und Port **vom aktuellen Tick** (ändern sich nur bei Schnitten; die Bildschirm-Projektion der Screen-Polygone hängt an near/far, `ni_extract.h` `screenProjection`).
- **Ausgabe:** `ni::CameraInput` nimmt Host-Zeiger; die interpolierten Werte liegen in einem Puffer des Interpolators, `ni::ExtractCamera` rechnet daraus View, Projektion und Viewport wie bisher (inkl. Halbpixel-Versatz B19).
- **Schnitt-Erkennung:** einrasten, wenn (Weg > 4·Weg_vorher + 0,05 **und** Weg > 0,5 Einheiten) oder (Drehung > 4·Drehung_vorher + 2° **und** > 20°) oder Frustum-Tangenten um mehr als Faktor 1,25 oder near/far/Port geändert; außerdem bei erster Sichtung, Generationslücke, `snapAll`. Maßstab: Frontend-Kamera 2,7–10,8 Einheiten vor den Objekten, near 0,1, far 1000, horizontale Tangente 0,7714 (75,3°). Die Ingame-Kamera ist ohne Eingabe nicht messbar; die Schwellen sind Konstanten in `SnapLimits` und werden mit M4 (Freecam M4b) und M8 geprüft. Die Kamerafeder `FUN_00015990` (feste 0,01-s-Unterschritte, timing60.md 4.5) bewegt die Spielkamera stetig; Schnitte gibt es in Skriptkameras und in der Intermission.
- Eine Kamera, die sich innerhalb einer Tick-Generation ändert, ist bildgetrieben (3.9) und wird nicht interpoliert.

### 3.8 Skinning

**Wo es heute läuft:** im Original. `ni_extract.cpp` `Skin()` ruft `kNiSkinInstanceDeform` = `FUN_0014F710` über `core::CallGuestThiscall` auf (Zeile 1707) und legt die Vertices in die Arena (Gastseiten unter 0x80000000), höchstens einmal je (Skin, Daten, Frame). Es gibt keinen Host-Nachbau.

**Was `FUN_0014F710` liest** (Capstone über alle 5015 Byte, `m3d_interp_common.dis`): `[ebx+0xC]` (Wurzel) → `+0x70`, `[ebx+8]` (`NiSkinData`) → `+0x10` (Wurzel→Skin-Transform), `+0x50` (Knochenzahl), `+0x54` (Knochendaten, 0x60 Byte je Knochen: Transform +0, Gewichtsliste +0x50, Anzahl +0x54), und `[[esp+0x90]+0x10]` = Knochenliste → je Knochen nur `+0x70` (0x14F86B–0x14F88B, 0x1501D0–0x1501F3). Rechnung: `X = Wurzel→Skin · inv(Wurzel.Welt)` (`FUN_0014D880` = Invert, `FUN_0010DFC0` = Komposition, 0x14F757–0x14F772), je Knochen `M_i = X · Knochen_i.Welt · Knochendaten_i`, dann Summe der gewichteten Vertices. Keine Schreibzugriffe auf die Instanz (den Frame-Stempel `+0x14` schreibt nur der Original-Aufrufer `FUN_00157C00`, den der Port nicht ausführt).

**Interpolierte Knochen ohne Gastschreibzugriff:**
1. Erfassung je geskinntem Draw: relative Posen `R_i = inv(Wurzel.Welt) · Knochen_i.Welt` (Host-Nachbau von Invert/Komposition, 3.3-Mathe), gespeichert im Knochen-Pool des Eintrags.
2. Im Tick-Frame: `R_i(α) = interp(R_i,prev, R_i,cur)` mit Schnellpfad und Sprungregel je Knochen.
3. In der Arena eine **Ersatz-`NiSkinInstance`** (0x18 Byte: `+0x08` = echte `NiSkinData`, `+0x0C` = Ersatz-Wurzel mit Einheits-Welttransform an `+0x70`, `+0x10` = Liste von Ersatz-Knoten mit `R_i(α)` an `+0x70`; je Ersatzobjekt 0xC0 Byte, genullt). Da `inv(Einheit) = Einheit`, rechnet `FUN_0014F710` `M_i = Wurzel→Skin · R_i(α) · Knochendaten_i` – dieselbe Formel mit interpolierten Posen.
4. `GeometryInput::skinInstance` zeigt auf die Ersatz-Instanz; der bestehende Cache in `Skin()` greift je (Ersatz-Zeiger, Daten, Frame). Für zwei Draws desselben Skins in einem Frame verwendet der Interpolator dieselbe Ersatz-Instanz.
5. Die Weltmatrix des Draws (Slot-Argument, `FUN_00149AE0`) wird wie bei starren Objekten interpoliert. Weil nur relative Posen interpoliert werden, bleibt eine Figur, die sich als Ganzes dreht, formtreu (Welt-Lerp jedes Knochens würde Gliedmaßen auf Sehnen nach innen ziehen).

Bei alpha = 1 bleibt alles beim heutigen Pfad (echte Instanz, bitgleich). Kosten: Die Deformation läuft schon heute in jedem Frame; bei 120 fps verdoppelt sie sich pro Sekunde, nicht pro Frame. Im Frontend 3 Skins × 5 Knochen; auf der Disc höchstens 49 Knochen und 1650 Vertices je Skin. Prüfung: Ein Selbsttest vergleicht bei alpha = 1 Ersatz- und Originalpfad (Abweichung durch andere Assoziativität ≤ 10⁻⁵ relativ).

### 3.9 Bildgetriebene Objekte

Objekte, die das Spiel im `Render()` mit `g_frameDelta` animiert (1.1), bewegen sich auch in 0-Schritt-Frames und laufen bei 120 fps von selbst mit 120 Hz. Würde man sie im Tick-Frame zwischen dem letzten Bild und dem aktuellen interpolieren, ergäbe sich ein Schrittmuster ½ : 1½ (Ruckeln). Regel: Ändert sich der Transform eines Schlüssels zwischen zwei Erfassungen **derselben** Generation, setzt der Eintrag das Merkmal „bildgetrieben“ (Haltezeit 120 Frames, verlängert bei jeder weiteren Änderung) und wird ohne Interpolation gezeichnet. Der Schnappschuss wird dabei auf den neuesten Stand gesetzt. Im Frontend trat das nie auf (0 von 259 249); Kandidaten im Spiel sind das Vision-Overlay (`FUN_00045F10`, eigene Kamera) und das Objekt `+0x11C` der Session. Bis zur Erkennung ist ein Frame betroffen.

### 3.10 Was im Tick-Raster bleibt

| Inhalt | Grund | Sichtbare Folge |
|---|---|---|
| UV-Animation (`NiUVController` 1559), Alpha (`NiAlphaController` 1578), Materialfarbe (63), Texturwechsel (`NiFlipController` 7), Morph (1 Testobjekt), Vertexfarben | Daten werden im Tick geändert; der Port setzt `NiGeometryData+0x2E` nie zurück (im Mitschnitt 25 Draws/Frame dauerhaft ≠ 0, nie wechselnd), Änderungen sind also nicht billig erkennbar | 60-Hz-Stufen bei sehr schnell laufenden Texturen (Lava, Wasser), sonst unsichtbar |
| Sichtbarkeit (`NiVisController` 3273), Flipbooks (`NiFltAnimationNode` 190), LOD (733), Switch (245) | diskret | – |
| Screen-Polygone/-Texturen (Slots 54/55): HUD (`hud/*.nif` enthalten nur Texturen), Menüs, Film-Quad, Intro-Bilder | 2D, vom Spiel pro Tick gesetzt | HUD-Animationen in 60 Hz |
| **Outlines** (`NiOutline`) | `NiOutline::vf33` 0x13E840 lässt die Kinder mit gesetztem `DAT_0030EB40` zeichnen (`NiTriShape::vf33` schreibt dabei Welt-Vertices des Tick-Zustands nach `[0x30EB40]`, 0x116755–0x116B25), `FUN_00153E90` (5134 Byte) sucht Silhouettenkanten zur Kameraposition `cam+0xA0`, projiziert sie (`FUN_00153D30`) und hängt Quads an ein Screen-Polygon (`FUN_0013D3B0`, 404 Vertices), das `Click` nach der Szene über Slot 54 zeichnet (`FUN_0013E360`). Aktiv: `DAT_002D7AB8` = 2 (`.data`), abgeschaltet nur um die Render-to-Texture-`Click` in `FUN_0003B960` (0x3BBD5–0x3BBF0) | Im Tick-Frame liegt die Outline auf S[G], das Mesh auf der Mitte: Versatz von ½ Tick-Bewegung. Betroffen sind nur 26 Knoten in `cockpit/chickencockpit.nif` und `pause/level_breakdown_menu.nif`. Das Cockpit wird, falls es wie das Vision-Overlay bildgetrieben ist, ohnehin nicht interpoliert (3.9, in M8 prüfen). Abhilfe als Stufe 4: Probe auf `FUN_00153E90` merkt sich je `NiOutline`-Knoten den Vertexbereich; Slot 54 verschiebt diese Vertices um die interpolierte Bewegung des Knotens (zurückprojizieren mit der Tick-Kamera, Delta anwenden, mit der interpolierten Kamera projizieren) |
| Lichter (Position `NiLight+0xA0`, Richtung `+0x110`) | `ni_extract` liest sie beim Zustandsaufbau aus dem Tick | Beleuchtung um ½ Tick versetzt, unsichtbar; Stufe 3: dieselbe Historie mit Schlüssel `NiLight` |
| `NiTextureEffect` (Projektion, Umgebung) | im Port noch nicht gezeichnet (P2) | bei P2 mit Stufe 3 nachziehen |
| Slot 52 (Einzelpunkt-`NiParticles` aus `FUN_0006DC60`) | Slot im Port noch nicht gezeichnet (P2); Transform `obj+0x70` wie bei Shapes | mit P2 starr interpolieren, kein Pro-Partikel-Pfad nötig |

**Partikel mit stabilen Indizes:** Auf der Disc gibt es keine Partikelsysteme (2.2). Sollte M8 doch welche zeigen (Klassen sind im Binary vorhanden), gilt: je Partikel linear interpolieren, wenn aktive Anzahl und Index-Zuordnung zwischen zwei Ticks gleich sind (Prüfung: Abstand jedes Partikels < 4× sein Vortick-Schritt), sonst das ganze System einrasten.

---

## 4. Culling

Das Spiel cullt in `Click` gegen die Ebenen der Tick-Kamera (`FUN_00101A30` → `FUN_001019B0`; Ebenen berechnet `NiCamera::vf39` 0xFFDA0 aus dem Frustum: near `+0x144`, far `+0x148`, links/rechts/oben/unten aus `+0x134…+0x140`, Ablage `cam+0x1B0`), wählt LOD-Stufen (`NiLODNode::vf33`), Räume (`NiRoomGroup`), Billboard-Ausrichtung und Alpha-Reihenfolge mit derselben Kamera. Der Renderer bekommt nur, was danach übrig bleibt.

Folgen bei alpha = ½ (nur Tick-Frames; im 0-Schritt-Frame stimmen Anzeige und Culling überein):

- **Nachlaufende Bildkante:** Ein Objekt, das in der Mitte zwischen den Ticks noch sichtbar wäre, im Tick-Zustand aber vollständig (ganze Bound-Kugel) außerhalb liegt, fehlt einen Frame (8,3 ms) zu früh. Breite des Streifens = halbe Tick-Drehung der Kamera; nahe der Bildkante gilt bei 640 px und Tangente 0,7714: dx/dφ = 320/0,7714·(1 + 0,7714²) ≈ 662 px/rad. Schwenk 180°/s = 3°/Tick → Streifen 1,5° ≈ **17 px bei 640 px** (77 px bei 2880 px). Gleiches gilt für Vorwärtsfahrt und für Objekte, die selbst aus dem Bild laufen. Betroffen sind kleine Objekte, deren Kugel ganz im Streifen liegt.
- **Vorlaufende Kante:** Ein Objekt, das erst im Tick-Zustand ins Bild kommt, wird gezeichnet; liegt es in der Mitte noch außerhalb, schneidet es die GPU weg. Hatte es im Tick davor keine Historie (gecullt), wird es mit S[G] gezeichnet: Ruhende Objekte stehen richtig, bewegte sind ½ Tick voraus.
- LOD-, Raum- und Billboard-Entscheidungen sowie die Alpha-Reihenfolge sind um ½ Tick „voraus“ – nur bei gleichzeitigem Schwellenwechsel in einem einzelnen Frame sichtbar.

**Empfehlung: zunächst keine Gegenmaßnahme.** Der Effekt betrifft jedes zweite Bild, maximal 8,3 ms, nur schnelle Schwenks. Als Option, falls die Freecam (M4b) oder M8 Flackern an der Bildkante zeigt: **Schutzrand** über eine Nachher-Probe auf `NiCamera::vf39` 0xFFDA0, die die vier Seitenebenen in `cam+0x1B0` mit um den Faktor (1 + g) geweiteten Tangenten neu berechnet (g ≈ halbe gemessene Tick-Drehung + 2 %, z. B. 5 %). Das ist ein Gastschreibzugriff nur auf Cull-Daten (Projektion und `SetCameraData` bleiben unberührt) und kostet einige zusätzliche, von der GPU verworfene Draws. Er beseitigt zugleich die Vorlauf-Unschärfe, weil Objekte knapp außerhalb dann Historie haben.

---

## 5. Kosten und Datenstrukturen

### 5.1 Mengen

| Szene | Draws pro Frame | Quelle |
|---|---|---|
| Frontend | 323–355 (Median 346), 3 geskinnt | Mitschnitte 2.3 |
| Ladebild | wenige Shapes, eigene Kamera (Frustum ±0,5, near 1, far 2) | `lock60`, Kamera `0x325C9E0` |
| Ingame (Schätzung) | Obergrenze ≈ 2000: größte Level-Datei 2153 Shapes (24 Räume, Portal-Culling), Figuren bis 324 Shapes inkl. aller LOD-Stufen, Effekt-Dateien bis 254 | Zensur 2.2; messbar ab M4/M8 |

### 5.2 Datenstrukturen

- **Zwei Tabellen mit offener Adressierung** (lineares Sondieren), „prev“ (Generation G−1, vollständig) und „cur“ (G, wird gefüllt). Bei Tick-Wechsel werden sie getauscht; statt zu löschen trägt jeder Eintrag den Tick als Stempel (Stempel 0 = leer, Ticks beginnen bei 1). Dadurch entfallen Löschen, Grabsteine und Alterung: Ein Objekt, das in G−1 nicht gezeichnet wurde, steht nicht in „prev“. Bei einer Lücke von ≥ 2 Ticks stammt „prev“ aus G−2 oder älter und wird am Stempel erkannt.
- **Hash:** multiplikativ (Fibonacci) auf den **oberen** Bits: `h = ((objekt ^ kamera·0x85EBCA77 ^ vorkommen) · 0x9E3779B1) >> (32 − Bits)`. Gastzeiger sind 16-Byte-ausgerichtet; eine erste Benchmark-Fassung mit den unteren Bits fiel auf 16 Startplätze zusammen und sondierte quadratisch (5.3).
- **Eintrag (128 Byte):** Objekt, Kamera, Vorkommen, Stempel, Frame-Stempel, Signatur (vtable, Daten, Skin, Property-State), `NiTransform` (64), Weg und Drehung des vorigen Ticks, Merkmale (bildgetrieben + Haltezeit, eingerastet), Offset/Anzahl im Knochen-Pool.
- **Kapazität** 4096 je Tabelle (Last ≤ 0,5 bei 2000 Draws), Verdopplung bei Bedarf → 2 × 512 KiB. **Knochen-Pool** je Tabelle (relative Posen, 64 Byte je Knochen): Frontend 15 Knochen; Ingame bei 20 Figuren × 30 Knochen ≈ 38 KiB. **Kamera-Tabelle** linear (≤ 8 Einträge). **Zerstört-Menge** (3.2) als kleine Hash-Menge, geleert beim zweiten Tausch.
- **Arena pro Frame** (bestehende `ExtractArena`, Rücksetzen in `DisplayFrame`): interpolierte `NiTransform`s (64 Byte je bewegtem Draw), Ersatz-Skins (0x18 + 0xC0·(1 + Knochen) + 4·Knochen, bei 49 Knochen ≈ 9,6 KiB).

### 5.3 CPU

Mikrobenchmark `run/analyse_m3d_interp/bench/interp_bench.cpp` (MSVC x86 /O2, Messrechner; 2000 Draws, 40 % bewegt, Tabellen wie 5.2): **Erfassung 4,4 ns je Draw** (Suche, Vergleich und Kopie von 64 Byte), **Interpolation 18,7 ns je Draw** (Mittel über alle; je bewegtem Draw ≈ 47 ns: 2 Quaternionen, Slerp, Zeilen, Lerp) → **0,046 ms je Tick-Frame bei 2000 Draws**. Rundweg gleicher Eingaben: |ΔR| = 2,4·10⁻⁷ (deshalb Schnellpfad, 3.3). Der Benchmark ist cache-warm; Gastspeicher und Arena kosten in der Praxis mehr, auch mit Faktor 5 bleibt es unter 0,3 ms. Zum Vergleich: Frame-Arbeit im 120-fps-Lauf Ø 0,59 ms, p99 1,4 ms (RelWithDebInfo, `rt120`), Budget 8,33 ms. Stufe 1b (hierarchisch) kostet zusätzlich ≈ 1 Komposition je sichtbarem Knoten (< 0,1 ms).

---

## 6. Schnittstellen

### 6.1 Frame-Clock → Renderer

```cpp
// Published by the frame clock (xapi/game_timing, analysis A); read by gfx at every SetCameraData and draw.
// Constant between two DisplayFrame calls of the guest main loop.
namespace xapi {
struct FrameInterp {
    std::uint64_t tick = 0;     // completed fixed steps when the guest renders (generation G)
    float alpha = 1.0f;         // weight of S[G]: display = interp(S[G-1], S[G], alpha); 1 = guest data as is
    std::uint32_t flags = 0;    // kFrameInterpSnapAll: context switch, catch-up (> 1 step), clock reset, first frame
                                // kFrameInterpInsideStep: draw issued from Update (loading/clear frames) -> alpha 1
    bool enabled = false;       // [render] interpolation on AND frame rate above the tick rate
};
FrameInterp CurrentFrameInterp() noexcept;  // stub until the clock lands: {fixed steps, 1.0f, 0, false}
}
```

Pflichten der Uhr (Test in `tests/frameclock_*.cpp`): `G − 1 + alpha` steigt pro Frame um ½ bei 120 Hz; alpha = 1 bei 60 Hz und bei `[render] interpolation = off`; `tick` zählt abgeschlossene Schritte (`probe:Game::FixedStep` zählt `FUN_0002C890`, einmal je Schritt vor `Update`; beim `Render()` sind alle Schritte des Frames abgeschlossen); `alpha = clamp(1 − (L − rest)/dt, 0, 1)` mit L = größter möglicher Rest (3.1). Werte, die nur durch die Tick-Quantisierung knapp unter 1 liegen (in der Uhr-Simulation 0,999 bei „align B“), meldet die Uhr als genau 1; zusätzlich behandelt der Renderer alpha > 1 − 10⁻³ als 1, damit die 0-Schritt-Frames bitgleich und rechenfrei bleiben.

### 6.2 Interpolator (neu: `gfx/interp_history.h/.cpp`, `gfx/interp_math.h`)

```cpp
namespace gfx::interp {

enum class SnapReason : std::uint8_t {
    None, Disabled, AlphaOne, Static, FirstSight, GenerationGap, Identity, Destroyed, Translation, Rotation,
    Scale, Mirror, NotOrthonormal, FrameDriven, SnapAll, CameraCut, kCount
};

struct SnapLimits {                    // 3.4 / 3.7 (constants, unit-tested; no config keys)
    float stepRadii = 2.0f, stepGrowth = 4.0f, stepEpsilonRadii = 0.01f;
    float angleDeg = 45.0f, angleGrowth = 4.0f, angleEpsilonDeg = 1.0f;
    float scaleRatio = 2.0f, orthoTolerance = 1e-3f;
    float cameraStep = 0.5f, cameraStepEpsilon = 0.05f, cameraAngleDeg = 20.0f, cameraAngleEpsilonDeg = 2.0f;
    float frustumRatio = 1.25f;
    std::uint32_t frameDrivenHoldFrames = 120;
};

// Pure math (interp_math.h, header-only, tests/interp_math_tests.cpp).
struct Quat { float w, x, y, z; };
Quat QuatFromRows(const float rotate[3][4]) noexcept;              // Shepperd; rows = axis images
void RowsFromQuat(const Quat& q, float rotate[3][4]) noexcept;     // column 3 = 0
Quat Slerp(Quat a, Quat b, float t) noexcept;                      // shortest path, nlerp above cos 0.9995
float AngleDeg(const float a[3][4], const float b[3][4]) noexcept;
// Rigid interpolation incl. fast paths (bitwise equal / t == 1) and the checks of 3.3; false = snap (reason).
bool InterpolateTransform(const ni::NiTransform& prev, const ni::NiTransform& cur, float t,
                          ni::NiTransform* out, SnapReason* reason) noexcept;
ni::NiTransform Inverse(const ni::NiTransform& t) noexcept;         // FUN_0014D880
ni::NiTransform Compose(const ni::NiTransform& a, const ni::NiTransform& b) noexcept;  // FUN_0010DFC0: a * b

struct FrameStats {   // per frame and accumulated; exit line "interpolation: ..."
    std::uint64_t frames = 0, interpolatedFrames = 0, draws = 0, interpolated = 0, cameras = 0,
                  camerasInterpolated = 0, skinnedPoses = 0, bones = 0;
    std::uint64_t snaps[static_cast<std::size_t>(SnapReason::kCount)] = {};
    std::uint32_t tablePeak = 0, bonePoolPeakBytes = 0;
};

class History {
public:
    // Slot 47: returns the camera to extract (input itself when not interpolated). Also latches FrameInterp and
    // swaps the tables when the tick advanced.
    ni::CameraInput Camera(std::uint32_t cameraObject, const ni::CameraInput& in) noexcept;
    // Slots 48-53 (in scene): records the draw and returns the input to extract - transform and skin instance
    // replaced by arena copies (guest-visible, valid until the arena reset) when interpolated.
    ni::GeometryInput Geometry(ni::DrawKind kind, const ni::GeometryInput& in, ni::ExtractArena& arena) noexcept;
    void OnObjectDestroyed(std::uint32_t object) noexcept;  // probe on NiAVObject::~NiAVObject FUN_00102CE0
    void EndFrame() noexcept;                                // DisplayFrame: frame stamp, statistics
    const FrameStats& Stats() const noexcept;
};
}  // namespace gfx::interp
```

**Einbau (minimal):**
- `GpuRenderer::SetCameraData` (`nipc_gpu.cpp` 934): `in = history.Camera(cameraObject, in)` vor `ni::ExtractCamera`; Kamera-Objekt aus Slot 47 (`location − 0xA0`) mitgeben und als aktuelle Kamera merken.
- `GpuRenderer::DrawGeometry` (`nipc_gpu.cpp` 952): `const ni::GeometryInput in2 = history.Geometry(kind, input, d.arena)` vor `ni::Extract*`. `ni_extract` bleibt unverändert: `FinishGeometry` liest `input.transform` als Gastzeiger (`ni_extract.cpp` 1722–1729), und Skin() ruft `FUN_0014F710` mit `input.skinInstance` auf. Beides darf in die Arena zeigen, die unter 0x80000000 liegt (`ni_extract.h`, Arena-Kommentar). Weltmatrix, Weltskalierung und `NORMALIZENORMALS` folgen automatisch.
- `GpuRenderer::DisplayFrame`: `history.EndFrame()` vor dem Arena-Reset.
- Neue Probe `probe:NiAVObject::Destroy` auf `FUN_00102CE0` (Thiscall, Prolog `6A FF 68 93 A4 17 00`, 7 Byte), nur aktiv mit Interpolation.
- Null-Renderer und `validate`: unverändert (kein Interpolator).

### 6.3 Konfiguration (eine Quelle: `core/config_schema.cpp`, de/en)

| Schlüssel | Werte (Standard) | Label de / en | Sichtbarkeit |
|---|---|---|---|
| `[render] frame_rate` | `60` \| `120` \| `display` (Analyse A) | „Bildrate“ – „60 fps“, „120 fps (interpoliert)“, „Bildschirm (max. 120)“ / „Frame rate“ – „60 fps“, „120 fps (interpolated)“, „Display (max. 120)“ | Launcher, Grafik |
| `[render] interpolation` | Bool (`on`) | „Zwischenbilder berechnen“ / „Compute in-between frames“; Hilfe: „Bei mehr als 60 Bildern pro Sekunde werden Kamera und Objekte zwischen zwei Logik-Schritten interpoliert (½ Schritt = 8 ms Verzögerung). Aus: jedes Bild zweimal zeigen.“ | Launcher, erweitert; wirkungslos bei 60 fps |
| `[render] interpolation_hierarchy` | Bool (`off`) | „Interpolation entlang der Szenenhierarchie“ / „Interpolate along the scene hierarchy“ | Entwickler (`kDev`), Stufe 1b |
| `[render] interpolation_debug` | `off` \| `tint` \| `trace` (`off`) | „Interpolation sichtbar machen“ / „Visualise interpolation“: `tint` färbt eingerastete Draws (Emissiv-Override im ff-Pipeline-Draw), `trace` schreibt je Frame eine CSV-Zeile mit Zählern und Gründen (Pfad wie `pacing_trace`) | Entwickler |

### 6.4 Logzeilen

- Start (GFX/info): `interpolation: on (tick 60 Hz, frame rate 120, display latency 0.5 tick) | off (<grund>)`.
- `trace_frames`: `interp frame N: tick G alpha A, X draws (I interpolated, S static, F first sight, J snapped: <grund> n ...), camera <interpolated|snapped: grund>`.
- Exit-Reporter „interpolation“: `interpolation: N frames (M with alpha < 1); draws D: interpolated I, static S, first sight F, snapped J (translation a, rotation b, scale c, identity d, destroyed e, generation gap f, frame-driven g, snap-all h, not orthonormal i, mirror j); cameras C (interpolated K, cuts L); skinned poses P (B bones); table peak T entries, bone pool Q KiB; cost average A µs, p99 P µs per frame`.

---

## 7. Ausbaustufen (überarbeitet)

| Stufe | Inhalt | Voraussetzung | Erfolgskriterium |
|---|---|---|---|
| 1 | Historie (6.2), Kamera, starre Objekte (Slots 48–51, 52 sobald P2), Sprungregel, Signatur + Zerstörungsprobe, bildgetrieben, Statistik, `[render] interpolation` | `FrameInterp` (Stub bis Analyse A landet) | Frontend bei 120 Hz: ≈ 37 % der Draws in Tick-Frames interpoliert, Schleifenumbrüche eingerastet (Referenz: 101 Sprünge in 750 Ticks, meist ≈ 30 Objekte gemeinsam alle 474–476 Ticks), 0 Draws im 0-Schritt-Frame verändert; bei 60 fps bitgleich (m3_check-xemu-Werte unverändert) |
| 1b (optional) | hierarchische Interpolation (3.5) | Sichtprüfung Einflug | Bögen des Einflugs ohne Wackeln |
| 2 | Skinning über Ersatz-Instanz (3.8) | Stufe 1 | Selbsttest Ersatz- gegen Originalpfad bei alpha = 1; die 3 Frontend-Skins interpoliert |
| 3 | Lichter, `NiTextureEffect` (mit P2), Einzelpunkt-Partikel (mit P2) | P2 | – |
| 4 (niedrig) | Outline-Korrektur (3.10) | M8 (Cockpit/Pause messbar) | – |
| Option | Schutzrand für Culling (4) | Freecam M4b | kein Kantenflackern bei schnellem Schwenk |

## 8. Tests und Prüfungen

**Unit-Tests** (`tests/interp_*.cpp`): Quaternion-Rundweg für Zufallsrotationen und alle Shepperd-Zweige (Spur ≤ 0, 180°); Slerp-Endpunkte, kürzester Weg (q gegen −q), Stetigkeit am Nlerp-Übergang, 179° bleibt auf der 179°-Seite; `InterpolateTransform` bitgleich bei gleichen Eingaben und bei t = 1; Sprungregel mit den gemessenen Fällen (4,6 Einheiten nach 0,095 bei r 0,19 → einrasten; 0,17/Tick bei r 0,04 nach 0,16 → interpolieren; 23,3°/Tick anhaltend → interpolieren; 72° → 144° → interpolieren; 2° → 115° → einrasten; Skalierung × 2,5 → einrasten; Determinante < 0 → einrasten); `Inverse`/`Compose` gegen von Hand gerechnete Werte nach `FUN_0014D880`/`FUN_0010DFC0`; Tabelle: ausgerichtete Zeiger verteilen sich (keine Sondierketten > 8 bei Last 0,5), Stempel 0 = leer, Tausch je Tick, Lücke ≥ 2 Ticks ohne Vorgänger, Vorkommen, Zerstört-Menge; Ersatz-Skin-Layout (`+0x08/+0x0C/+0x10`, Knochen `+0x70`); Kamera-Schnitt.

**Laufprüfung** (für `tools/m3d_check.py`): Frontend bei `frame_rate = 120`: Statistik wie Stufe 1; Mittelpunkttest online: Für einen Teil der Objekte die Weltposition im Tick-Frame protokollieren und mit den Nachbarzuständen vergleichen (interpoliert = Mitte innerhalb 10⁻⁵); Bildtripel aus dem ruhigen Frontend und aus dem Einflug ansehen; `frame_rate = 60`: m3_check (xemu 0,00/0,00/0,29/0,01) und m3c_check unverändert. Die Skripte dieses Berichts lassen sich mit einem neuen Mitschnitt wiederholen.

## 9. Risiken und offene Punkte

1. **Alpha-Konvention** (3.1): Eine Uhr, die alpha = ½ in den 0-Schritt-Frames und 1 in den Tick-Frames liefert, lässt die Bewegung jedes zweite Bild zurückspringen. Muss in der Schnittstelle und im Test der Uhr festgeschrieben sein.
2. **Ingame nicht gemessen:** Kamera-Schwellen, Anteil bildgetriebener Objekte, Skin-Anzahl, Draw-Zahlen, Kamerawechsel und Mehrfach-Draws gibt es erst mit M4/M8. Die Statistik (6.4) ist dafür ausgelegt; die Mitschnitt-Skripte sind wiederverwendbar.
3. **Bahnkurven** beim Frontend-Einflug (bis 92 px bei 640 px, ≈ 1,4 s nach jedem Kontextwechsel): Stufe 1b bereit halten.
4. **Outlines** im Cockpit/Pausenmenü bleiben im Tick-Zustand (3.10); ob das Cockpit bildgetrieben ist, zeigt erst M8.
5. **Zeigerwiederverwendung ohne Destruktor** (Pools): nur über die Sprungregel abgesichert.
6. **Culling-Streifen** bei schnellen Schwenks (4): unbewertet, bis die Freecam (M4b) schnelle Schwenks reproduzierbar macht.
7. **Datenanimationen** (UV, Alpha, Farbe) bleiben 60 Hz. Falls schnelle UV-Animationen in Levels sichtbar stufen, wäre eine Texturmatrix-Interpolation möglich, sofern `NiUVController` die Texturtransformation und nicht die UV-Daten ändert (nicht untersucht).
8. **Erkennung bildgetriebener Objekte** braucht 0-Schritt-Frames; mit einer anderen Uhr (z. B. Echtzeit bei 100 Hz) gibt es sie nur unregelmäßig.
9. Der Port setzt `NiGeometryData+0x2E` (Revision) nie zurück; das ist für die Interpolation ohne Belang, verhindert aber eine billige Erkennung geänderter Vertexdaten.

## 10. Skripte und Rohdaten

| Datei | Zweck |
|---|---|
| `scripts/m3d_interp_common.py` | XBE (Capstone), Dekompilat, `functions.tsv`/`callgraph.tsv`, NI-Klassen |
| `scripts/m3d_interp_isofiles.py` | Dateien in den `.img`-Archiven des ISO (nur lesend) |
| `scripts/m3d_interp_nifscan.py` | Zensur der NIF/UCF-Klassen, Transforms, Billboard-Modi, Skins (2.2) |
| `scripts/m3d_interp_levelsize.py` | Shapes/Skins/Knoten je Datei (5.1) |
| `scripts/m3d_interp_capture.py` | Auswertung eines Mitschnitts (2.3); Format im Kopfkommentar |
| `scripts/m3d_interp_outliers.py` | große Sprünge je Schlüssel (3.4) |
| `scripts/m3d_interp_snaprules.py` | Bewertung der Sprungregeln (3.4) |
| `scripts/m3d_interp_firstsight.py` | erste Sichtungen und Eltern-Fallback (3.4) |
| `scripts/m3d_interp_midpoint.py` | Mittelpunkttest über zwei Ticks (3.6) |
| `scripts/m3d_interp_hier.py` | Welt- gegen Ein-Ebenen-Interpolation (3.5) |
| `scripts/m3d_interp_chord.py` | Sehnenfehler-Schranke (3.5) |

Rohdaten unter `run/analyse_m3d_interp/` (git-ignoriert): `tree/` (Kopie von HEAD mit Mitschnitt-Patch, `capture_patch.diff`), `build/` (RelWithDebInfo), `lock60/` und `rt120/` (INI, Log, `capture.bin`, `*.out` aller Auswertungen), `nifscan.out`, `levelsize.out`, `bench/` (Benchmark und `bench.out`). Wiederholen: `set FUT_INTERP_CAPTURE=<datei>`, `FUT_INTERP_CAPTURE_FROM`, `FUT_INTERP_CAPTURE_FRAMES`, dann `run\analyse_m3d_interp\build\bin\futurama.exe --config=run\analyse_m3d_interp\lock60\futurama.ini` und `python analysis\loader\scripts\m3d_interp_capture.py <datei>`.
