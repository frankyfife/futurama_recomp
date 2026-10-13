# G3: Breitbild – Bericht

Stand: 2026-10-12. Abweichungen vom Entwurf: [`loader_design.md`](loader_design.md) 8.21. Schnittstellen: [`src/INTERFACES.md`](../src/INTERFACES.md) 4.37.

## Kurzfassung

* **Neu: `[render] aspect`** (Launcher, Reiter „Grafik“, „Bildformat (Breitbild)“): **Original** (4:3 mit Balken, Standard), **16:9**, **16:10**, **21:9** (64:27, die 2560×1080-Bildschirme) und **Wie der Bildschirm** (Format der Renderauflösung, sonst des Bildschirms in echten Pixeln).
* **3D mit Hor+:** Das senkrechte Sichtfeld bleibt wie im Original, links und rechts sieht man mehr. Bei 16:9 ist das Bild um ein Drittel breiter, bei 21:9 um 78 %.
* **HUD am Bildrand:** Im Level rücken Lebensanzeige mit Avatar, Munition und Nibbler-Zähler an den linken Rand, Geld/Sammelobjekte an den rechten. Sie behalten Größe und Form.
* **4:3-Mitte mit Balken:** Hauptmenü, Ladebilder, Titelkarten, Filme und Texte. Pausenmenü und Statistikbildschirm stehen in der Mitte, das Level dahinter füllt die ganze Breite. Abblendungen (Pause, Überblendungen) decken die ganze Breite ab.
* **Original:** Es ändert sich nichts. Kein Frustum-Eingriff, keine Platzierung, 4:3-Backbuffer, 181 Hooks wie vorher, Bild gleich der Vorher-Aufnahme.
* **Freecam:** funktioniert zusammen mit Breitbild (Pose und Zoom der Freecam, dazu das breitere Frustum).

## 1. Umsetzung

| Teil | Datei | Inhalt |
|---|---|---|
| Mathematik und Regeln | `src\host\gfx\widescreen_math.h` (neu) | Hor+-Faktor, Frustum-Verbreiterung, Kriterium „Szenenkamera“, Abbildung Gast-Pixel → Backbuffer-Pixel je Platzierung, Einteilung der Zeichenaufrufe (`Classify`), Ausnahmetabelle |
| Zustand | `src\host\gfx\widescreen.h/.cpp` (neu) | `[render] aspect`, Bildschirmformat, verbreiterte Kameras während ihres Click, Spielkontext (Frontend/Ingame), Protokoll |
| Kamera | `src\host\gfx\freecam.cpp` | Der vorhandene Probe-Hook auf `NiCamera::Click` 0x100C50 verbreitert auch außerhalb der Freecam das Frustum der Szenenkameras und berechnet Kulling-Ebenen und View-Daten neu (wie die Freecam) |
| Renderer | `src\host\gfx\nipc_gpu.h/.cpp`, `nipc_renderer.cpp` | Backbuffer im Anzeigeformat, Platzierung jedes Zeichenaufrufs und Löschrechtecks, schwarze Seitenbalken, Kamera-Zeile in der Draw-Spur |
| Fenster | `src\host\platform\window.h/.cpp`, `platform.cpp` | Fenster und Bildfläche im Anzeigeformat (Höhe 480 × `window_scale`, Breite nach Format) |
| Schema | `src\host\core\config_schema.cpp` | Zeile `[render] aspect` mit Auswahlliste. Der Launcher braucht keine Änderung, er baut die Zeile aus dem Schema |
| Tests | `tests\widescreen_tests.cpp` (neu) | 9 Fälle mit 100 Prüfungen: Hor+-Faktor und Frustum, Szenenkameras, Abbildung je Platzierung, Einteilung der gemessenen Zeichenaufrufe, Ausnahmetabelle, Parser, Rendergrößen, Fensterlayout |

### 1.1 Hor+

Für jede Kamera, die in den Backbuffer zeichnet und wie eine Szenenkamera aussieht, multipliziert der Click-Probe `left` und `right` des Frustums (+0x134) mit k = (Anzeigeformat) / (4/3). `top`, `bottom`, `near` und `far` bleiben. Danach berechnet er wie die Freecam das Ende von `NiCamera::vf39` neu: die sechs Ebenen 0xFEED0–0xFF290 und die View-Daten 0xFF3A0. Kulling und Bild passen so zum breiteren Bild. Nach dem originalen Click schreibt er die Werte des Spiels zurück und rechnet erneut. Spiellogik, 3D-Ton und alles, was das Spiel selbst projiziert, sehen also weiter das 4:3-Frustum.

**Szenenkamera:** perspektivisch, `left < 0 < right`, `bottom < 0 < top`, `far ≥ 50·near` und ein Port über die ganze Breite (die Höhe darf kleiner sein, für Kinobalken). Damit fallen aus der Verbreiterung heraus:

* die Hilfskameras (n 1, f 2),
* die HUD-Kameras der Ziffern (l 0, r 0,1: die Achse liegt am Rand des Frustums),
* die Avatar-Kamera (Port im linken Drittel),
* Render-to-Texture-Kameras.

**Hauptmenü:** Im Kontext „Frontend“ wird nichts verbreitert. Im ersten 16:9-Lauf zeigte Hor+ am linken Rand das Panel des nächsten Menüs, das das Spiel knapp außerhalb des 4:3-Bildes parkt. Das Hauptmenü bleibt deshalb 4:3 mit Balken.

**Freecam:** Ist die Freecam aktiv, setzt ihr Zweig Pose, Zoom und Verbreiterung in einem Schritt.

### 1.2 Platzierung

Der Gast zeichnet weiter in sein 640×480-Raster. Der Renderer bildet jeden Zeichenaufruf über den D3D-Viewport in den breiten Backbuffer ab. Die Höhe wird immer gleich abgebildet, die Breite je nach Platzierung:

| Platzierung | Gast 0..640 wird zu | für |
|---|---|---|
| Stretch | der ganzen Breite | Hor+-3D (dessen Frustum um denselben Faktor breiter ist), Abblendungen |
| Center | dem zentrierten 4:3-Bereich | Menüs, Texte, Ladebilder, Filme, 3D ohne Hor+ |
| Left | einem 4:3-Bereich am linken Rand | HUD im linken Drittel |
| Right | einem 4:3-Bereich am rechten Rand | HUD im rechten Drittel |

Vorgetransformierte Pixel (Slot 55) und Bildschirm-Polygone (Slot 54) liegen relativ zum Viewport und wandern mit. Die Seitenbalken neben dem zentrierten Bereich löscht der Renderer einmal pro Bild schwarz, vor dem ersten Zeichenaufruf; seit 2026-10-13 nur, wenn dieser (oder das erste Farblöschen) nicht über die Breite gestreckt ist (Abschnitt 6). Bilder ohne Zeichenaufruf behalten wie bisher das alte Bild.

### 1.3 Regeln (`Classify`)

1. **Hor+-Kamera:** ihre Geometrie und ihre Kontur-Bänder (`NiOutline`, G1-Technik B) → Stretch.
2. **Kamera mit Teil-Port** (Avatar oben links): alle ihre Zeichenaufrufe nach dem Port.
3. **Geometrie einer anderen Kamera über die ganze Breite** (nicht HUD-Kamera, z. B. das Hauptmenü) → Center. Die Szene bleibt ein unverzerrtes 4:3-Bild.
4. **Zeichenaufruf über das ganze Raster:** ohne Textur (Abblendung) → Stretch, mit Textur (Bild) → Center.
5. **Bildschirmtexturen (Slot 55)** → Center. Ladebilder bestehen aus 256-Pixel-Kacheln und dürfen nicht auseinanderfallen.
6. **Nur im Level** (Kontext „Ingame“): erst die Ausnahmetabelle, dann das Drittel, in dem die Mitte des Rechtecks liegt: links → Left, rechts → Right, Mitte → Center.
7. Sonst Center.

**Sonderfall Abdunklung:** Eine Fläche mit höchstens 8 Ecken einer Hor+-Kamera, die mit der Projektion des Spiels den ganzen Bildschirm bedeckt, zeichnet der Renderer mit dieser Projektion, gestreckt. Das betrifft die Abdunklung des Pausenmenüs: ein Quad mit 137 % Breite, das ab 21:9 sonst schmaler als das Bild wäre.

**Ausnahmetabelle:** `kAnchorExceptions` in `widescreen.cpp` ist leer. Keiner der gemessenen Bildschirme braucht eine Ausnahme. Neue Fälle (Element, Bereich im 640×480-Raster, Platzierung) kommen dort hinein.

## 2. Vorher: welche Zeichenaufrufe gehören wozu

Gemessen im Lauf `run\g3\trace43` (4:3, `[render] trace_frames`, Kamera-Zeilen neu in der Draw-Spur).

| Bild | Kamera | Zeichenaufrufe | Rechteck (640×480) |
|---|---|---|---|
| Level 1-1 | Szene (l −1 r 1 t 0,75, n 0,25) | 400 Shapes | – |
| | Szene noch aktiv | Bildschirm-Polygone: Ring der Lebensanzeige, Lebensbalken, ein Element oben rechts, Munition, Geld-Symbol, Nibbler-Symbol | (36,49)–(143,156), (77,88)–(141,152), (516,49)–(604,137), (56,161)–(104,209), (520,352)–(584,416), (56,352)–(120,416) |
| | 3 HUD-Kameras (l 0, r 0,1, t 0,1, b 0) | Ziffern „0“ (Geld), „5“, „0/1“ | (540,416)–(564,443), (104,170)–(128,197), (59,416)–(117,443) |
| | Hilfskamera (n 1, f 2) | 2 Bildschirm-Polygone am linken Rand | (0,93)–(128,189) |
| | Avatar (Port 0–0,33 × 0,68–1) | Kopf (8 Shapes), 2 Kontur-Bänder | (0,35)–(117,167) |
| Pause | wie Level, dazu Pausenkamera (l −0,77, Port voll) | Menü-Panel und Texte als Shapes, Abdunklung als Quad | Quad (−118,−112)–(758,577) |
| Hauptmenü | Frontend-Kamera (l −0,771 t 0,514) | 340 Shapes, 1 Bildschirm-Polygon ohne Textur | Polygon (0,0)–(640,480) |
| Ladebild | Hilfskamera | 6 Bildschirmtexturen à 256er-Kachel, Dose unten rechts | (0,0)–(1024,1024), (539,349)–(603,477) |
| Titelkarte | Hilfskamera, HUD-Kamera | Bildschirmtextur in der Mitte, Text als Geometrie | (191,72)–(447,328), (220,336)–(420,360) |

Im Level zeichnet das Spiel die Bildschirm-Polygone des HUD, solange die Daten der Szenenkamera gesetzt sind. Die Kamera allein unterscheidet sie also nicht vom Bild. Deshalb entscheidet für Slot 54 das Rechteck, für die Szene der Zeichenaufruf-Typ.

## 3. Prüfung

* **Bauten:** MSVC RelWithDebInfo `build\g3` (`FUT_WERROR=ON`) und clang-cl Debug `build\g3-clang` (`FUT_WERROR=ON`), ohne Warnungen.
* **Unit-Tests:** `fut_tests` 493/493 (1 übersprungen) in `build\g3`. Im clang-Bau laufen die 9 Widescreen-Fälle und die Schema-, Fenster- und AA-Fälle (60 Fälle) ebenfalls durch.
* **Läufe:** in `run\g3\<Lauf>\`, nicht im Repository. Jeweils `[debug] movies = skip`, Lautstärke 0, Rumble aus, Rückgabe 0, 25–35 s. Skripte `run\g3\scripts\g3_probe.txt` (Hauptmenü → Neues Spiel → 1-1 → Pause) und `g3_scene.txt` (Startlevel 2-6 mit Kamerafahrt, HUD, Freecam, Levelende).

| Lauf | Einstellung | Geprüft (Screenshots selbst angesehen) |
|---|---|---|
| `base43` | Original, 1280×960 | Vorher-Bilder; nach der Umsetzung erneut: 181 Hooks, keine Widescreen-Zeile, Level-Bild gleich (`shots\frame_001433.png`) |
| `trace43` | Original, Draw-Spur | Zuordnung in Abschnitt 2 |
| `wide169` | 16:9, 1280×720 | Level 1-1: 3D breiter, HUD an den Rändern (`frame_001430.png`). Pause: Abdunklung über die ganze Breite, Panel in der Mitte (`frame_001608.png`). Ladebild 4:3 mit Balken (`frame_001000.png`). Titelkarte mittig (`frame_001150.png`). Hauptmenü: Hor+ zeigte das geparkte Menü-Panel, deshalb danach 4:3 (W3) |
| `wide219` | 21:9 (64:27), 1280×540, Startlevel 2-6 | Hauptmenü 4:3 mit schwarzen Balken (`frame_000751.png`). Kamerafahrt am Levelanfang über die ganze Breite, „Drücken Sie A …“ mittig (`frame_001275.png`). Bender-HUD an den Rändern (`frame_001358.png`). Freecam mit Hor+ (`frame_001748.png`). Statistikbildschirm mittig, HUD am Rand (`frame_001981.png`) |
| `monitor120` | Wie der Bildschirm (3840×2160 → 16:9), Auflösung automatisch 1920×1080, 120 fps, MSAA 4× | Level 1-1 mit Interpolation (`frame_002364.png`) |

Exit-Zeile im 16:9-Lauf: `widescreen: 16:9, 517 widened camera clicks; placements stretch 173244, center 6973, left 8901, right 1161; 72 full-screen quads drawn unwidened`.

## 4. Checkliste für Spieler

1. Launcher, Reiter **Grafik** → **Bildformat (Breitbild)** auf das Format des Bildschirms stellen, am einfachsten „Wie der Bildschirm“. Die Renderauflösung „Automatisch“ passt sich an.
2. Spiel starten und prüfen:
   * [ ] **Hauptmenü:** 4:3 in der Mitte mit schwarzen Rändern. Passt das, oder soll das Menü auch breit sein? Dann sieht man an den Rändern die Panels der anderen Menüs.
   * [ ] **Level:** Mehr Bild links und rechts, nichts gestaucht oder gestreckt. Figuren wirken rund, nicht breit.
   * [ ] **HUD:** Lebensanzeige mit Kopf, Munition und Nibbler-Zähler am linken Rand, Geld/Sammelobjekte am rechten. Das gilt für Fry, Bender und Leela. Gibt es Elemente am falschen Platz oder auseinandergerissene Teile?
   * [ ] **Zielmarkierungen** (Fry mit Waffe, RT auf einen Gegner): Sitzt die Markierung auf dem Gegner, auch am Bildrand?
   * [ ] **Pause, Optionen im Spiel, Statistikbildschirm, Speicher- und Hinweisfenster:** mittig, lesbar, nichts an die Ränder gerutscht.
   * [ ] **Ladebilder, Titelkarten, Filme:** 4:3 mit schwarzen Rändern.
   * [ ] **Zwischensequenzen und Kamerafahrten:** breit, Texte mittig.
   * [ ] **Überblendungen** (Tod, Levelstart, Pause): über die ganze Breite.
   * [ ] **Freecam (L3+R3):** Bild breit, HUD ausblenden (Y) wirkt.
   * [ ] **Fenster:** Größe ändern, Alt+Eingabetaste (Vollbild).
3. Zurück zum Original: „Original 4:3 (mit Balken)“.

## 5. Bekannte Grenzen

* **Was das Spiel selbst projiziert**, etwa Zielmarkierungen oder Namen über Figuren, liegt im 4:3-Raster. In der Mitte stimmt die Platzierung „Center“ dafür genau. Liegt so ein Element im linken oder rechten Drittel, würde es an den Rand verschoben und säße neben dem Ziel. Nicht im Lauf geprüft (keine Gegner in den Prüfszenen). Abhilfe wäre ein Eintrag in der Ausnahmetabelle oder ein eigenes Kennzeichen.
* **Elemente, die über zwei Drittel reichen**, werden nach ihrer Mitte eingeordnet. Ein HUD-Teil, der so aus der Gruppe fällt, braucht einen Eintrag in der Ausnahmetabelle.
* **Levelränder:** Hor+ zeigt mehr vom Level. Die Level sind für 4:3 gebaut. Am Rand können unfertige Stellen oder Objekte auftauchen, die das Spiel selbst (außerhalb des NetImmerse-Kullings) erst später einblendet. In 1-1 und 2-6 nicht gesehen.
* **Nicht in Läufen geprüft:** Filme mit `movies` an (laufen als bildfüllende Textur und sollten 4:3 bleiben), Todesbildschirm, Leelas HUD, Waffen-HUD von Fry, Speicher-Dialoge, 16:10. Ebenso das Avatarbild über Render-to-Texture (M8); es wird als Bildschirm-Polygon oben links nach den Regeln an den linken Rand gesetzt.
* Ohne echten Controller geprüft (Eingabeskript).

## 6. Nachtrag 2026-10-13: Tunnelende in Level 1-2 (Nebel)

**Meldung:** In der Kanalisation (16:9, MSAA 8×) war „etwa Mitte rechts“ ein schwarzer Fleck zu sehen, das Tunnelende; der Nebel reiche nicht weit genug.

**Nebel wie im Original:** Der Port rechnet den Nebel wie REN (`FUN_00159700`): `NiFogProperty` Funktion 0 → Tabellennebel LINEAR auf der Sichttiefe z (keine Entfernung, kein Bereichsnebel), Start = Far − Tiefe · (Far − Near), Ende = Far. In 1-2 (Hauptkamera Near 0,25, Far 150, Tiefe 1,0) heißt das: linear von 0,25 bis 150, voll genau an der Far-Ebene, Farbe 0x217219. Die Hintergrundfarbe setzt das Spiel auf dieselbe Farbe und löscht damit **in jedem Bild den ganzen Backbuffer** (Farbe + Tiefe, ohne Rechteck). Was hinter der Far-Ebene liegt, wird nicht gezeichnet; dort bleibt die Nebelfarbe stehen. So verschwindet das ferne Tunnelende auf der Xbox nahtlos. Das Xbox-D3D schneidet an der Far-Ebene ebenfalls ab (`D3DRS_DEPTHCLIPCONTROL`, Index 82 → `NV097_SET_ZMIN_MAX_CONTROL` 0x1D78, nur in `D3DDevice_SetRenderState_ZEnable` 0x236C30 geschrieben; weder REN noch das Spiel setzen „clamp“; **Vermutung**: Standardwert ohne Klemmen).

**Ursache im Port (G3):** Die Seitenbalken neben dem 4:3-Bereich wurden in **jedem** Bild vor dem ersten Zeichenaufruf schwarz gelöscht, also auch im Level nach dem grünen Löschen des Spiels. Wo die Szene außerhalb der 4:3-Mitte nichts zeichnet (Löcher hinter der Far-Ebene), blieb Schwarz stehen. Beleg im Bild des Nutzers (2000×1125): Die linke Kante des Flecks liegt genau bei x = 1750 = rechter Rand des 4:3-Bereichs (250 + 1500); links davon ist das Loch nebelgrün (33, 113, 25), rechts schwarz (0, 0, 0). Bei 4:3 tritt das nicht auf.

**Behebung (`nipc_gpu.cpp`, `ClearBars`):** Ob die Balken schwarz werden, entscheidet jetzt der erste Zeichenaufruf bzw. das erste Farblöschen des Bildes nach seiner Platzierung. Ist er über die Breite gestreckt (Hor+-Szene, bildschirmfüllendes Löschen), gehören die Seiten zur Szene und werden nicht schwarz gelöscht; Löcher zeigen wie in der Mitte die Löschfarbe des Spiels. Ein reines Tiefen-/Stencil-Löschen entscheidet nichts. Zentrierte Bilder (Hauptmenü, Ladebilder, Titelkarten, Filme) bekommen wie bisher schwarze Balken. Neue Abschlusszeile `d3d11 widescreen bars: N frames with the sides left to the stretched scene …`. Unit-Test „nipc_gpu: G3 bars …“ (WARP).

**Prüfung (`run\fix\user_before` mit dem alten Bau, `run\fix\user_after` mit der Behebung, gleiches Skript: Start 1-2, schräg nach links vorn, Kamera schrittweise nach rechts, 16:9, MSAA 8×):** Bild `frame_001460.png` zeigt vorher hinter dem Gitter rechts den schwarzen Halbkreis mit gerader linker Kante bei x = 1120 von 1280 (= 4:3-Rand), genau wie beim Nutzer; nachher ist die Stelle nebelgrün (Pixel 1130/262: (0, 0, 0) → (33, 114, 25)), Vergleich `run\fix\user_compare.png`. Hauptmenü weiter mit schwarzen Balken (`run\fix\hole_after\shots\frame_000751.png`).
