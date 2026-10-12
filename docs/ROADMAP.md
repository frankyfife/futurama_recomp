# Roadmap: Futurama-PC-Port

Stand: 2026-10-12. Diese Seite ist die Agenda. Die technischen Details stehen in [`loader_design.md`](loader_design.md), Abschnitt 6 (Meilensteine), und in den Berichten `m1_report.md` bis `m5_report.md`.

## Erledigt

| Meilenstein | Ergebnis |
|---|---|
| M1 | XBE läuft bis `main()` (Loader bei 0x10000, Kernel-HLE, FS-Patches) |
| M2 | Archive, Fibers, asynchrone I/O, Hauptschleife ohne Bild und Ton |
| M3 | Erstes Bild mit D3D11: Legal-Bild, Tech-Notice, Ladebildschirm und Frontend pixelgenau wie in xemu |
| M3b | 60-Hz-Spiellogik (NTSC-Zeitwerte mit PAL-Inhalten, `[game] tick_rate`) und an den Monitor gekoppeltes Frame-Pacing: 60,01 Hz, auf 120 Hz genau jedes zweite Vblank ([Bericht](m3b_60hz_report.md)) |
| M3c | Qt-Launcher `FuturamaLauncher.exe` (Assistent mit ISO-Prüfung, Reiter aus dem Schema von `futurama.exe`, Spielstart mit Ergebnisanzeige, Deutsch/Englisch, Paket `dist\FuturamaPC\` mit Qt-Lizenztexten), Renderauflösung bis zur Bildschirmhöhe, MSAA 2×–8× mit Alpha-to-Coverage, Supersampling (exakt bei 2×, Catmull-Rom bei Zwischenwerten), helligkeitsrichtige Kantenglättung, anisotrope Filterung, Monitorwahl, Voreinstellungen nach Grafikspeicher; „Klassisch“ bitgleich zu M3. Alle 23 Review-Befunde nachgeprüft, 21 behoben ([Bericht](m3c_report.md), Abweichungen: [`loader_design.md`](loader_design.md) 8.5–8.8) |
| M3d | Bildraten bis 120 Hz: Logik bleibt bei 60 Hz, der Renderer interpoliert Kamera, starre Objekte (entlang der Elternkette) und Skins zwischen den beiden letzten Ticks; Halbtakt-Uhr mit alpha ½/1, Takt bis 120 Hz (Vblank, VRR-Timer, Kadenz), Just-in-time-Start (Bildstart → sichtbar bei 60 fps 6,6 statt 16,6 ms), Vollbild über den ganzen Monitor. Launcher-Option „60“ / „120 (interpoliert)“ / „Monitor (max. 120)“. Am 120-Hz-Fernseher 119,9 Hz, 99,6–99,7 % der Bilder genau ein Refresh, 99,9 % der bewegten Draws interpoliert. 13 Review-Befunde nachgeprüft, 10 behoben ([Bericht](m3d_report.md), Abweichungen: [`loader_design.md`](loader_design.md) 8.9) |
| M4 | Eingabe: XPP-HLE mit Eingabe-Router (XInput 1.4, Tastatur/Maus über Raw Input, alle 24 Xbox-Tasten frei belegbar, Rumble mit Stärke, Hotplug), Reiter „Steuerung“ im Launcher (Belegungsdialog, Controller-Test), Eingabeskript `[debug] input_script` und `tools\m4_check.py`. **Das Spiel erreicht zum ersten Mal ein Level:** „Neues Spiel“ → Level 1-1 (Planet-Express-Hangar), Fry ist steuerbar; 10 Minuten Zufallseingaben ohne Absturz. 17 Review-Befunde nachgeprüft und behoben. Ohne echten Controller geprüft (simuliert und per Unit-Tests). Sichtbare Lücken im Level (Render to Texture, Partikel, schwarze Schiffsunterseite) gehen an P2 ([Bericht](m4_report.md), Abweichungen: [`loader_design.md`](loader_design.md) 8.11) |
| M5 | Audio: DSOUND-HLE auf XAudio2 2.9 (40 Einstiege + Stream-Methoden), Xbox-ADPCM (bitgleich mit ffmpeg), gestreamte Musik, 3D-Klang mit dem exakten DSOUND-Entfernungsmodell und X3DAudio-Richtung, I3DL2-Hall, Lautstärke-Kategorien; Software-Mixer ohne Gerät, Gerätewechsel in beide Richtungen ohne Ruckler, Reiter „Audio“ im Launcher (Gerät, Lautsprecher, Testton). Ohne Zuhören geprüft: `[audio] capture_wav` und `tools\m5_check.py` gegen die Originaldateien (2D-Pegel −6,0 dB, Tonhöhe 1,0000, Musik ohne Aussetzer, Panning, Begrenzer) – ALL PASS am 5.1-Fernseher in allen drei Bauten. 17 Review-Befunde nachgeprüft und behoben. **Abhören durch den Nutzer steht aus** ([Bericht](m5_report.md) mit Hör-Checkliste, Abweichungen: [`loader_design.md`](loader_design.md) 8.12–8.15) |
| M4b | Freecam ([Bericht](m4b_report.md)): ein/aus mit L3+R3 (einzelne Stick-Klicks erreichen das Spiel weiter, höchstens 4 Ticks verzögert) oder F6, im 3D-Hauptmenü, in Levels und Zwischensequenzen. Probe-Hook auf `NiCamera::Click`: Kulling und Bild nutzen die Freecam, danach bekommt das Spiel seine Kamera zurück (Spiellogik und 3D-Ton bleiben bei der Spielkamera). Gamepad und Tastatur/Maus, Tempo und Zoom im Flug verstellbar, HUD ausblendbar, Optionen im Launcher-Reiter „Steuerung“. Geprüft mit Eingabeskript `m4b_freecam.txt`, Unit-Tests und `m4_check` (ALL PASS) |
| G1 | Konturen einstellbar ([Analyse und Umsetzung](../analysis/loader/g1_outlines.md)): Die Konturen sind vorgefertigte, um 0,05 aufgeblasene Rückseiten-Schalen (`QDOutline`, 75 % schwarz) bzw. beim HUD-Kopf `NiOutline`-Bänder – der Port zeichnete sie schon. Neu: `[render] outline_color` (original/RRGGBB), `outline_opacity`, `outline_thickness` (0–2,5; 0 = aus) im Reiter „Grafik“; Standard unverändert (kein neuer Code aktiv, Trace gleich). Dicke über die Paarung Schale↔Körper (Bereiche ohne Linie bleiben ohne). Freecam dazu: Rückseiten von außen (B), schwarze Leere |

## In Arbeit

| Meilenstein | Inhalt |
|---|---|
| M6 | Video (XMV) – läuft: eigener XMV-Demuxer, Bild über den in Windows eingebauten WMV-Decoder (kein FFmpeg-Download), Ton (Xbox-ADPCM, Sprachspur) über die DSOUND-HLE, Takt nach pts. **Nutzerwunsch: alle Videos überspringbar** – auch die, die das Original beim ersten Ansehen sperrt (Intro-Filme 1-1-01/02; Zwischensequenzen erst nach 2,5 s): Option `[game] skip_videos` (Standard: immer überspringbar) |
| M7 | Spielstände – nach M6 (Nutzer 2026-10-12: Speichern ist noch ungetestet): Speichern, Neustart, Laden, Optionen/Extras, Signatur, Löschen |

## Als Nächstes

Reihenfolge laut Nutzer (2026-10-12, nach dem Hörtest von M5: „Ton klingt gut“): Freecam (erledigt) → Konturen → Videos.

| Meilenstein | Inhalt |
|---|---|
| M3e (Renderer P2) | Rest nach G1: Render to Texture (Avatarbild in der Lebensanzeige oben links: separat gerenderter Kopf, Konturen `NiOutline`), Partikel (Slot 52), schwarze Schiffsunterseite, fehlende Texturen ([`m4_report.md`](m4_report.md) 6.1) |
| G4 | **Level- und Figurenauswahl** (Nutzerwunsch 2026-10-12). Das Spiel hat keine eingebaute Auswahl (Strings durchsucht: kein Debug-/Cheat-Menü; die Extras-Freischaltung über `menu/extras/unlock.txt` betrifft nur Bilder/Musik/Videos). Plan: (1) **Levelauswahl:** „Neues Spiel“ startet fest `Level1-1` (String 0x2940C0, Nutzer 0x20090/0x20B60/0xA26A0); Liste aller Level aus den Archiven (`LevelN-M`, z. B. 1-1…5-3), Auswahl im Launcher (`[game] start_level`) und optional als eigener Eintrag im Hauptmenü bzw. Overlay; prüfen, welchen Spielstand-/Fortschrittszustand ein Level beim Einstieg erwartet (Waffen, Leben, Zwischensequenzen). (2) **Figurenauswahl:** klären, wo ein Level die Spielfigur festlegt (Fry/Bender/Leela; Leveldaten oder Code), dann Figur pro Level wählbar machen – nur wo Fähigkeiten/Skript es zulassen, sonst als „experimentell“ markiert. Prüfung: Kurzläufe in jedes Level mit Screenshot und `controllable`. |
| G3 | **Breitbild** (Nutzerwunsch 2026-10-12): Option 16:9 / 16:10 / 21:9 / Monitor im Launcher statt 4:3 mit Seitenbalken. 3D mit Hor+-Sichtfeld (vertikales Sichtfeld bleibt, horizontal mehr Bild; Frustum der Szenenkamera, Kulling-Ebenen wie bei der Freecam über `NiCamera::Click` neu berechnet). **HUD korrekt platziert:** HUD-Elemente an ihrem Bildrand verankern (Lebensanzeige/Avatar oben links, Zähler unten links/rechts) statt sie zu strecken oder mittig im 4:3-Bereich zu lassen; Menüs, Texte und Vollbild-Elemente (Ladebilder, Videos) bleiben 4:3-zentriert mit Balken. Vorher klären: welche Bildschirmpolygone/HUD-Kameras zu welchem Rand gehören (Position im 640×480-Raster), Ausnahmen per Tabelle. Prüfung mit Screenshots in Menü, Level 1-1 und Zwischensequenz gegen 4:3. |
| G2 | Texturen artstyle-gerecht aufwerten: Austauschsystem im Renderer und lokales Upscale-Werkzeug (nach M4b, für Vorher/Nachher-Vergleiche mit der Freecam) |
| – | Takt bei 120 Hz: In der M5-Endprüfung (Fernseher an) bestanden `m3_check` inkl. M3b-7 und die 60-Hz-Varianten von `m3d_check`, aber D1 (120 Hz: nur 43 % der Bilder genau ein Refresh) und L1 scheiterten – mit dem M4-Bau genauso, also Anzeige-Umgebung, nicht M5 ([`m5_report.md`](m5_report.md) 7, 10). Fernseher-Modus/VRR und Treiber prüfen, dann `m3d_check` ohne `--display-optional` wiederholen. Mit echtem Controller sind Controller, Vibration und Bild bestätigt (Nutzertest 2026-10-12). |

### M3c – Launcher (Qt) und Anzeige-Optionen (erledigt, Plan zur Referenz)

Umgesetzt bis auf die als offen markierten Punkte in `loader_design.md` 8.6–8.8 (Render-to-Texture mit MSAA und Linienbreite folgen mit P2, Latenz mit M3d). Bericht: [`m3c_report.md`](m3c_report.md).

Ein eigenes Programm **`FuturamaLauncher.exe`** (Qt 6.8.3 Widgets, 64 Bit, lokal unter `D:\Qt\6.8.3\msvc2022_64`). Es ist der normale Einstieg für Spieler: Einstellungen bearbeiten und das Spiel starten. Der bisherige `futurama.exe` bleibt der technische Starter bei 0x10000.

- **Erster Start:** Assistent zur Auswahl des ISO. Die Prüfung erkennt das XDVDFS-Abbild und die SHA-1 der `default.xbe`. Retail EU wird unterstützt, andere Versionen (Prototyp, US) bekommen einen Hinweis.
- **Grafik:**
  - Fenster oder randloses Vollbild, Monitorwahl
  - Fenstergröße
  - **interne Renderauflösung** (Vielfaches von 640×480 oder Monitorauflösung, 4:3 mit Seitenbalken)
  - Skalierung (linear, point, integer)
  - VSync und Pacing-Modus, Bildrate
- **Spiel:** Sprache (EN/DE/FR/ES/IT), Logik-Takt (60 Hz Standard, 50 Hz PAL-Original als Option).
- **Pfade:** Spielstände, Screenshots, Logs. Jeweils mit „Ordner öffnen“.
- **Erweitert:** Protokollstufe, D3D-Debug-Layer, Traces, Selbsttest, fehlende D3D-Funktionen.
- **Starten:** Startet `futurama.exe` mit der gespeicherten `futurama.ini`. Danach zeigt der Launcher Exitcode und die letzten Log-Zeilen, und bei Fehlern gibt es einen Hinweis.
- **Keine doppelte Pflege der Einstellungen:** `futurama.exe --print-config-schema` liefert alle Schlüssel mit Typ, Auswahlwerten, Standard und Beschreibung als JSON. Der Launcher baut seine Felder daraus.
- **Sprache der Oberfläche:** Deutsch und Englisch.
- **Später ergänzt:** Steuerung (ab M4), Audio (ab M5).

Zur Auflösung gehört eine Renderer-Erweiterung: In 3D wird in Zielauflösung gerendert, statt 640×480 hochzuskalieren. 2D-Elemente werden mitskaliert, der Tiefenpuffer hat die Zielgröße. Breitbild (16:9 mit angepasstem Sichtfeld und korrekt verankertem HUD) ist der eigene Punkt G3.

**Anti-Aliasing** (Vorgabe: kein FXAA) gehört zur selben Renderer-Arbeit wie die Auflösung:
- **MSAA** 2×/4×/8×, je nachdem was die Grafikkarte kann. Render-Target und Tiefenpuffer werden multisampled, danach folgt ein Resolve vor Skalierung und Present. Der Renderer zeichnet vorwärts mit Fixed-Function-Nachbau, deshalb ist MSAA die passende und günstige Lösung.
- **Alpha-to-Coverage** für Materialien mit Alpha-Test (Gitter, Pflanzen, Kanten in Texturen), weil MSAA allein diese Kanten nicht glättet.
- **Supersampling** optional über eine Render-Skala (1,5× oder 2× der Zielauflösung, danach heruntergerechnet), kombinierbar mit MSAA.
- **Anisotrope Texturfilterung** (2×–16×) als eigene Option.
- **Prüfung:**
  - Render-to-Texture (Slot 15) mit MSAA richtig auflösen.
  - Pixel-Einrastung der 2D-Elemente (1/16 Pixel aus M3) auf die Zielauflösung umrechnen.
  - Screenshots in Zielauflösung.
  - Vergleich mit xemu bei 1× ohne AA bleibt als Regressionstest.
- **Ausdrücklich nicht:** FXAA. Auch kein TAA, das würde ohne Bewegungsvektoren schmieren.
- **Launcher:** Auflösung, Render-Skala, MSAA-Stufe und anisotrope Filterung im Grafik-Tab.

### M3d – Bildraten bis 120 Hz (Interpolation) (erledigt, Plan zur Referenz)

Umgesetzt mit Stufe 1 (Kamera, starre Objekte, dazu die Elternkette) und Stufe 2 (Skinning); Partikel (Slot 52) und UV-Animationen bleiben im 60-Hz-Raster (Partikel zeichnet der Port erst mit P2). Der Preis ist ein halber statt eines ganzen Ticks (Halbtakt-Uhr „Phase B“: die Hälfte der Wirkung eines Ticks ist im eigenen Bild sichtbar). Offen und an M4/M4b/M8 gebunden: Ingame-Szenen (Kamera, Schnitte, bildgetriebene Objekte, Kosten bei etwa 2000 Draws), andere Bildwiederholraten als 120 Hz am echten Gerät. Bericht: [`m3d_report.md`](m3d_report.md).

Vorgabe: mindestens 60 fps, höchstens 120 Hz.

- **Die Spiellogik bleibt bei 60 Hz** (NTSC-Werte aus M3b). Einige Teile rechnen pro Tick statt pro Sekunde: Physik-Dämpfung, Zielen beim Chicken Walker mit festen 0,02 s, Skript-Schleifen. Bei 120 Hz Logik liefen sie zu schnell (siehe `analysis/loader/timing60.md` und die M3b-Review-Befunde).
- **Das Rendern läuft bis 120 fps mit Interpolation im Renderer.**
  - `Game::Frame` (0xA2C20) arbeitet ganze Ticks ab und rendert danach einmal. Den Rest der Zeit kennt man über `app+0xC8` (Zeitstempel = jetzt − Rest), deshalb gilt `alpha = Rest / dt`.
  - Bei 120 fps gibt es jedes zweite Frame keinen neuen Tick. Der Renderer interpoliert dann die Weltmatrizen der gezeichneten Objekte und die Kamera zwischen den beiden letzten Logik-Zuständen. Dazu führt er pro Gastobjekt eine Historie, die mit jedem neuen Tick weiterrückt.
  - Große Sprünge (Teleport, Kontextwechsel) werden erkannt und nicht interpoliert.
- **Ausbaustufen:**
  1. Kamera und starre Objekte.
  2. Skinning (Knochenmatrizen interpolieren).
  3. Partikel und UV-Animationen.
- **Preis:** etwa ein Logik-Tick (16,7 ms) zusätzliche Verzögerung, wie bei jeder Interpolation.
- **Vorab prüfen:** Code, der pro Frame statt pro Tick läuft, also außerhalb der Tick-Schleife. Bekannt sind die Lade-Pumpen `FUN_000d2370`/`FUN_000f2050`/`FUN_000f2680`, die Speicherpflege `FUN_000a46d0` (zählt Aufrufe), `g_frameDelta` [0x2E7B88] und `FUN_0002c9c0`/`FUN_000d7630`. Er muss sich bei doppelter Frame-Zahl richtig verhalten.
- **Einstellung** (Launcher): Bildrate „60“, „120 (interpoliert)“ oder „Monitor (max. 120)“. Bei einem 120-Hz-Monitor ergibt das jedes Vblank, bei 144/165 Hz eine Begrenzung auf 120 per VRR oder Timer, bei 60 Hz bleibt es bei 60.

### M4 – Eingabe (erledigt)

XInput, Tastatur und Maus mit frei belegbaren Tasten, Rumble, Reiter „Steuerung“ im Launcher, Eingabeskripte für automatische Läufe bis ins erste Level. Bericht: [`m4_report.md`](m4_report.md).

### M4b – Freecam (erledigt, Plan zur Referenz)

Eine frei bewegliche Kamera für Screenshots, Fehlersuche und Level-Inspektion.

- **Bedienung:** Ein- und Ausschalten per Taste (Vorschlag F6), am Gamepad mit **L3+R3** (beide Sticks gleichzeitig drücken; Nutzerwunsch 2026-10-12). Das Spiel liest die Stick-Klicks als eigene Kanäle (`analysis\loader\m4_input.md`): vorher prüfen, ob es L3 oder R3 einzeln belegt. Die Kombination wird dann geschluckt und erreicht das Spiel nicht, die Einzelklicks bleiben unberührt. Die Kombination ist im Launcher änderbar. Maus zum Umsehen, WASD/QE zum Bewegen, Umschalt/Strg für schneller/langsamer. Gamepad mit beiden Sticks und Schultertasten. Sichtfeld per Mausrad verstellbar.
- **Optionen:** Spiel-Logik anhalten (Ticks einfrieren, Rendern läuft weiter), HUD ausblenden, Kamera-Position kopieren bzw. laden, zurück zur Spielkamera.
- **Umsetzung:** Die Weltmatrix der aktiven `NiCamera` wird **vor** dem Culling überschrieben (Hook an der Kamera-Aktualisierung bzw. `NiCamera::Click` 0x100C50), nicht erst im Renderer. Nur so tauchen Objekte außerhalb des ursprünglichen Sichtkegels auf. Während die Freecam aktiv ist, landen keine Eingaben im Spiel.
- **Auch im 3D-Hauptmenü** (Nutzerwunsch): Die Freecam muss in jedem Kontext mit 3D-Szene funktionieren, also im Frontend, im Level und in Zwischensequenzen mit Engine-Grafik. Das Frontend hat eigene Kameras. Der Hook muss deshalb die jeweils aktive `NiCamera` erwischen und nicht nur die Spielerkamera im Level.
- **Voraussetzung:** M4 (erledigt), weil Tastatur, Maus und Gamepad über dieselbe Eingabeschicht laufen. Anknüpfungspunkte: `inp::InputRouter::Divert(port, InputConsumer*)` (das Spiel sieht einen verbundenen, neutralen Controller; gehaltene Tasten zählen nach dem Ende erst nach dem Loslassen), `inp::KeyboardMouseSource::ReadRaw` (Tasten und Mausweg ohne Pad-Nachbildung), Belegungen und Eingabeskripte für automatische Freecam-Läufe (`src\INTERFACES.md` 4.27, 4.28).

### Vorgemerkte Grafik-Features (Nutzerwunsch 2026-10-12)

**Konturen der Figuren einstellbar.** Die Figuren haben comictypische harte schwarze Außenkanten. Gewünscht: mindestens die Farbe einstellbar, möglichst auch die Stärke (Breite).
- **Voraussetzung:** M3e. Die Konturen hängen sehr wahrscheinlich an `NiOutline` und Render to Texture (Slot 15). Render to Texture ist derzeit ein No-op, deshalb fehlen die Konturen im Port vermutlich noch ganz ([`m4_report.md`](m4_report.md) 6.1).
- **Erst die Technik klären:** M3e muss zuerst analysieren, wie das Original die Kontur erzeugt. Mögliche Verfahren:
  - Silhouette in eine Textur rendern und danach versetzt bzw. als Kante wieder einblenden.
  - Eine vergrößerte Hülle mit umgekehrten Flächen (Inverted Hull).
  - Linien-Primitive.
- **Je nach Verfahren** sitzen Farbe und Breite an einer anderen Stelle: Konstante im Composite-Shader, Extrusionsweite der Hülle oder Breite des Kantenfilters.
- **Breite und Auflösung:** Die Breite muss mit der Renderauflösung skalieren (Supersampling, `loader_design.md` 8.6–8.8: Linienbreite bei Render to Texture mit MSAA). Sonst werden die Linien bei 4K hauchdünn.
- **Launcher:** Optionen „Konturfarbe“ (Farbwähler, Standard Schwarz wie im Original) und „Konturstärke“ (Faktor, Standard 1,0 = Original).
- **Prüfung:** Mit Standardwerten pixelgleich zu xemu.

**Texturen artstyle-gerecht aufwerten.** Gewünscht ist etwas mehr Detail, aber nicht zu knackig, passend zum Comic-Stil.
- **Austauschsystem im Renderer:**
  - Jede Textur hat beim Laden einen stabilen Hash über die Originaldaten.
  - Optionaler Ersatzordner im Nutzerverzeichnis (DDS/BC7 mit Mipmaps, Alpha bleibt erhalten).
  - Debug-Option zum Abspeichern der geladenen Texturen.
  - Ohne Ersatzordner verhält sich das Spiel exakt wie bisher.
- **Werkzeug `tools/texture_upscale.py`:** erzeugt das Paket lokal aus der eigenen ISO.
  - Hochskalierung 2× bzw. 4× mit einem Verfahren, das flächige Farben und klare Kanten erhält, ohne Überschärfung, Rauschen oder erfundene Fotodetails. Kandidaten: auf Cartoon/Anime trainierte Upscaler, klassische Filter als Rückfall.
  - Alpha-Kanten getrennt behandeln, damit keine Säume entstehen.
  - Paletten- und UI-Texturen ausnehmen bzw. getrennt einstellbar halten.
- **Rechtliches und Downloads:** Das Paket entsteht nur beim Nutzer und kommt nie ins Repository, weil es abgeleitete Spieldaten enthält. Modelldateien werden erst nach Rückfrage heruntergeladen.
- **Launcher:** Option „Texturpaket“ (Aus / Ordner wählen).
- **Prüfung:**
  - Vorher/Nachher-Bilder von festen Kamerapositionen (Freecam).
  - Kein Mehrverbrauch bei abgeschaltetem Paket.
  - Speicher- und Ladezeit-Budget mit Paket.

### Danach

| Meilenstein | Inhalt |
|---|---|
| M6 | Video: XMV über FFmpeg (WMV2 + Xbox-ADPCM), Untertitel |
| M7 | Spielstände: XAPI-SaveGame auf Host-Ordner, Signaturschlüssel |
| M8 | Spielbares Level von Anfang bis Ende, Vergleich mit xemu |
| M9 | Schrittweise Dekompilierung (re3-Stil), Funktion für Funktion |
| M10 | Vulkan- bzw. D3D12-Backend hinter der vorhandenen RHI |
