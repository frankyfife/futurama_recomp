# Meilenstein M4b: Freecam – Bericht

Stand 2026-10-12. Umgesetzt ohne Multi-Agenten-Workflow, Testläufe bewusst kurz (Nutzerwunsch).

## 1. Ergebnis

Eine frei bewegliche Kamera für Screenshots und zum Umsehen. Sie funktioniert im 3D-Hauptmenü, in Levels und in Zwischensequenzen mit Engine-Grafik.

- **Ein/aus:** am Gamepad **L3+R3** (beide Sticks drücken), an der Tastatur **F6**. Beides ist im Launcher änderbar (Reiter „Steuerung“, Gruppe „Freie Kamera“).
- **Während die Freecam aktiv ist**, sieht das Spiel an diesem Anschluss einen neutralen Controller: Fry bleibt stehen, es öffnet sich kein Pausenmenü. Spiellogik, Animationen und Ton laufen weiter.

| | Gamepad | Tastatur/Maus |
|---|---|---|
| Bewegen | linker Stick | W A S D |
| Hoch/runter | RT / LT | E / Q |
| Umsehen | rechter Stick | Maus |
| Schneller / langsamer (halten) | A / X | Umschalt / Strg |
| Tempo dauerhaft | Steuerkreuz rechts / links (×1,5) | + / − |
| Zoom | Steuerkreuz hoch / runter | Mausrad |
| HUD aus/ein | Y | H |
| Wände von außen aus/ein | B | B |
| Zurück zur Spielkamera | BACK | R |

## 2. Umsetzung

### 2.1 Kamera

- **Hook:** `gfx/freecam.cpp` hängt einen Probe-Hook an `NiCamera::Click` 0x100C50. Der Trampolin-Prolog ist `mov eax, fs:[0]`. Offset 0 ist keine FS-Patch-Stelle, deshalb ist der Befehl positionsunabhängig.
- **Welche Kamera:** Pro Frame ersetzt die Freecam die erste **Szenenkamera**. Das ist eine Perspektivkamera mit Vollbild-Port, symmetrischem Frustum und far ≥ 50·near, gemessen im Frontend und in Level 1-1. Diese Kameras bleiben unberührt:
  - Hilfskameras (n 1, f 2)
  - HUD-Kameras (l 0, r 0,1)
  - Avatar-Kamera (Port oben links)
  - Render-to-Texture-Kameras
- **Ablauf je Click:**
  1. Die Weltpose der Kamera (+0x70/+0x80/+0x90 Zeilen, +0xA0 Position) und das Frustum (Zoom) werden gesichert und ersetzt.
  2. Der abgeleitete Teil von `NiCamera::vf39` 0xFFDA0 wird neu berechnet: die Zeilen-Kopie +0x110, die sechs Kulling-Ebenen 0xFEED0–0xFF290 und die View-Daten 0xFF3A0. So nutzen Kulling **und** Bild die Freecam.
  3. Danach wird alles zurückgeschrieben und neu berechnet. Die Spiellogik und der 3D-Ton-Hörer behalten so die Spielkamera.
- **Takt:** Die Pose wird einmal pro Logiktick aktualisiert, im umgeleiteten `XInputGetState`. Bei 120 fps interpoliert M3d sie wie die Spielkamera.
- **Koordinatensystem:** Die Weltachse „oben“ und die Händigkeit werden beim Start aus der Spielkamera abgeleitet. Gemessen: +Y oben, Händigkeit +1. Es gibt kein Rollen, die Neigung ist auf ±89° begrenzt.
- **Tempo:** Standard 40 Einheiten/s im Level. Im Hauptmenü ist die Kulisse etwa 3 Einheiten tief, dort skaliert das Tempo mit (near/0,25)², also ×0,16. Das ist eine Heuristik.

### 2.2 Bildpuffer und Rückseiten

Das Spiel löscht den Backbuffer nie, weil seine Levels immer den ganzen Bildschirm füllen. Solange die Freecam aktiv ist, löscht `gfx::ClearBackBuffer` deshalb Farbe und Tiefe vor der ersten Backbuffer-Kamera jedes Frames. Sonst blieben beim Blick ins Leere alte Pixel stehen, auch ein ausgeblendetes HUD.

**Farbe der Leere:** Schwarz. Zuerst war es die Hintergrundfarbe des Spiels, ein Dunkelgrün. Im Nutzertest wirkte das wie eine grüne Überblendung, sobald die Freecam von außen auf die Halle schaute.

**Rückseiten:** Die Levels sind nur von innen modelliert. Von außen sieht man deshalb nur die Rückseiten der Wände, und die werden normalerweise weggelassen. Mit `[freecam] back_faces = 1` (Standard, im Flug mit B umschaltbar) entfällt bei aktiver Freecam das normale Rückseiten-Culling (CW) in `MakeRasterDesc`, sodass diese Wände gezeichnet werden. Das CCW-Culling der Kontur-Schalen bleibt, die Konturen ändern sich also nicht. Kurztest in Level 1-1: Wände von außen sichtbar, mit B ausgeschaltet wieder durchsichtig.

### 2.3 HUD ausblenden

`freecam::SuppressOverlayDraw` lässt bei ausgeblendetem HUD diese Zeichenaufrufe in den Backbuffer weg:
- Geometrie, deren Kameradaten nicht von der Szenenkamera stammen, also Slot 47 mit einer anderen Kamera
- alle Bildschirm-Polygone und -Texturen (Slots 54/55)

Render-to-Texture bleibt unberührt.

### 2.4 Eingabe

- **Erkennen von L3+R3:** `inp::ChordFilter` (`input/chord_filter.h`) läuft als Sample-Filter des Routers (`HostRouter::SetSampleFilter`) vor der Umleitung. Das Spiel benutzt R3 selbst für die Ego-Ansicht. Deshalb hält der Filter einen frisch gedrückten Stick-Klick bis zu 4 Ticks zurück:
  - Folgt der andere Klick in dieser Zeit, wird die Kombination ausgelöst, und keiner der beiden Klicks erreicht das Spiel.
  - Sonst bekommt das Spiel den Klick verspätet.
  - Ein kurzes Antippen kommt als Ein-Tick-Druck an.
- **Umleitung:** `InputRouter::Divert` leitet den Anschluss auf die Freecam um. `KeyboardMouseSource::SetPadEmulation(false)` schaltet die Pad-Nachbildung der Tastatur ab, die Freecam liest Tasten und Maus roh über `ReadRaw`.
- **Rückkehr:** Beim Zurückschalten zählen Tasten, die noch gedrückt sind, erst nach dem Loslassen. Das ist die Regel des Routers.

## 3. Prüfungen

- **Unit-Tests:** `tests/freecam_tests.cpp` mit 8 Fällen: Kombination, Verzögerung, Antippen, Pose, Händigkeit, Neigungsgrenze, Stick-Kennlinie. `fut_tests` besteht 441/441 in RelWithDebInfo und Debug mit `FUT_WERROR=ON`. clang-cl Debug mit `FUT_WERROR=ON` baut ohne Warnungen.
- **Eingabeskript `tools/input_scripts/m4b_freecam.txt`** (etwa 40 s, Screenshots unter `run/freecam/shots`, nicht im Repository):
  - **Hauptmenü:** ein, Flug hinter das Menü-Panel, Zoom, BACK zurück zur Spielkamera, aus.
  - **„Neues Spiel“ → Level 1-1:** ein, Übersicht über den Hangar, Drehung, HUD aus, aus.
  - `expect controllable` danach ist erfüllt, das Spiel läuft also normal weiter. Exit 0.
- **`tools/m4_check.py`:** ALL PASS (M4-1 bis M4-9), weil Router und Tastatur-Quelle geändert wurden.
- **Launcher:** Die sieben Schlüssel von `[freecam]` erscheinen im Reiter „Steuerung“ (Screenshot-Automatik).

## 4. Offen

- **Echte Geräte:** Nicht mit echtem Controller, Maus oder Tastatur geprüft. Das übernimmt der Nutzer.
- **Tempo-Heuristik im Hauptmenü:** Andere Frontend-Szenen sind nicht gemessen.
- **Spiel anhalten während der Freecam:** Ticks einfrieren bei weiterlaufendem Rendern ist nicht umgesetzt. Die Spiellogik läuft weiter.
- **Kamera speichern und laden:** nicht umgesetzt (Plan in der Roadmap).
