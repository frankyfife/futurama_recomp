# Roadmap: Futurama-PC-Port

Stand: 2026-10-06. Diese Seite ist die Agenda. Die technischen Details stehen in [`loader_design.md`](loader_design.md), Abschnitt 6 (Meilensteine), und in den Berichten `m1_report.md` bis `m3c_report.md`.

## Erledigt

| Meilenstein | Ergebnis |
|---|---|
| M1 | XBE läuft bis `main()` (Loader bei 0x10000, Kernel-HLE, FS-Patches) |
| M2 | Archive, Fibers, asynchrone I/O, Hauptschleife ohne Bild und Ton |
| M3 | Erstes Bild mit D3D11: Legal-Bild, Tech-Notice, Ladebildschirm und Frontend pixelgenau wie in xemu |
| M3b | 60-Hz-Spiellogik (NTSC-Zeitwerte mit PAL-Inhalten, `[game] tick_rate`) und an den Monitor gekoppeltes Frame-Pacing: 60,01 Hz, auf 120 Hz genau jedes zweite Vblank ([Bericht](m3b_60hz_report.md)) |
| M3c | Qt-Launcher `FuturamaLauncher.exe` (Assistent mit ISO-Prüfung, Reiter aus dem Schema von `futurama.exe`, Spielstart mit Ergebnisanzeige, Deutsch/Englisch, Paket `dist\FuturamaPC\` mit Qt-Lizenztexten), Renderauflösung bis zur Bildschirmhöhe, MSAA 2×–8× mit Alpha-to-Coverage, Supersampling (exakt bei 2×, Catmull-Rom bei Zwischenwerten), helligkeitsrichtige Kantenglättung, anisotrope Filterung, Monitorwahl, Voreinstellungen nach Grafikspeicher; „Klassisch“ bitgleich zu M3. Alle 23 Review-Befunde nachgeprüft, 21 behoben ([Bericht](m3c_report.md), Abweichungen: [`loader_design.md`](loader_design.md) 8.5–8.8) |

## In Arbeit

Zurzeit nichts.

## Als Nächstes

| Meilenstein | Inhalt |
|---|---|
| M3d | Bildraten bis 120 Hz mit Interpolation (Details unten), dazu die Present-Phase vor das Ziel-Vblank legen (M3c-Review-Befund 6: Latenz schwankt um 1–2 Refreshes) und die Swapchain im Vollbild über den ganzen Monitor (Independent Flip, VRR); der Launcher zeigt die Bildrate bis dahin nur an |
| M4 | Eingabe mit Reiter „Steuerung“ im Launcher (Details unten) |

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

Zur Auflösung gehört eine Renderer-Erweiterung: In 3D wird in Zielauflösung gerendert, statt 640×480 hochzuskalieren. 2D-Elemente werden mitskaliert, der Tiefenpuffer hat die Zielgröße. Breitbild (16:9 mit angepasstem Sichtfeld) ist ein eigener späterer Punkt.

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

### M3d – Bildraten bis 120 Hz (Interpolation)

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

### M4 – Eingabe

XInput, Tastatur und Maus mit frei belegbaren Tasten, Rumble. Steuerung kommt als Tab in den Launcher.

### M4b – Freecam

Eine frei bewegliche Kamera für Screenshots, Fehlersuche und Level-Inspektion.

- **Bedienung:** Ein- und Ausschalten per Taste (Vorschlag F6). Maus zum Umsehen, WASD/QE zum Bewegen, Umschalt/Strg für schneller/langsamer. Gamepad mit beiden Sticks und Schultertasten. Sichtfeld per Mausrad verstellbar.
- **Optionen:** Spiel-Logik anhalten (Ticks einfrieren, Rendern läuft weiter), HUD ausblenden, Kamera-Position kopieren bzw. laden, zurück zur Spielkamera.
- **Umsetzung:** Die Weltmatrix der aktiven `NiCamera` wird **vor** dem Culling überschrieben (Hook an der Kamera-Aktualisierung bzw. `NiCamera::Click` 0x100C50), nicht erst im Renderer. Nur so tauchen Objekte außerhalb des ursprünglichen Sichtkegels auf. Während die Freecam aktiv ist, landen keine Eingaben im Spiel.
- **Voraussetzung:** M4, weil Tastatur, Maus und Gamepad über dieselbe Eingabeschicht laufen.

### Danach

| Meilenstein | Inhalt |
|---|---|
| M5 | Audio: DSOUND auf XAudio2, Xbox-ADPCM, 3D-Sound, I3DL2-Hall |
| M6 | Video: XMV über FFmpeg (WMV2 + Xbox-ADPCM), Untertitel |
| M7 | Spielstände: XAPI-SaveGame auf Host-Ordner, Signaturschlüssel |
| M8 | Spielbares Level von Anfang bis Ende, Vergleich mit xemu |
| M9 | Schrittweise Dekompilierung (re3-Stil), Funktion für Funktion |
| – | Breitbild 16:9 mit angepasstem Sichtfeld und HUD-Positionen |
| M10 | Vulkan- bzw. D3D12-Backend hinter der vorhandenen RHI |
