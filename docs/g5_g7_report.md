# G5 und G7: Präzise Kamera am rechten Stick, Level-Skip-Cheat – Bericht

Stand: 2026-10-12. Analyse der Kamera: [`analysis/loader/g5_camera.md`](../analysis/loader/g5_camera.md). Abweichungen vom Entwurf: [`loader_design.md`](loader_design.md) 8.19. Schnittstellen: `src\INTERFACES.md` 4.35.

## 1. Kurzfassung

* **G5 – `[input] camera_response`** (Launcher, Reiter „Steuerung“, „Kamera am rechten Stick“):
  * **Original** (Standard): wie auf der Xbox. Das Spiel dreht seine Folgekamera über zwei hintereinander geschaltete Filter: Die volle Drehrate von 155,6°/s ist erst nach etwa 0,8 s erreicht (90 % nach 0,63 s), unter 0,9 Ausschlag dreht der Eingang nur mit einem Viertel, und nach dem Loslassen gleitet die Kamera 0,2–0,9 s und 5–18° nach. Es wird kein Hook installiert.
  * **Präzise**: Die Drehung folgt dem Stick sofort und proportional (200°/s bei vollem Ausschlag, 90 % nach 33 ms) und hält beim Loslassen an (nach 0,1 s, Nachlauf ≈ 1°). Der rechte Stick eines Controllers bekommt eine kleinere tote Zone (0,18 statt 0,4). Kollision, Höhe, Abstand, automatisches Nachführen und alle anderen Kameramodi bleiben die des Spiels. Die Maus profitiert ebenso (sie wirkt wie der rechte Stick).
* **G7 – `[cheats] level_skip` (bis G8: `[game] level_skip_cheat`)** (Reiter „Spiel“, Standard an): **A A B B A A B B A A** zügig im Level gedrückt beendet das Level regulär – derselbe Aufruf, den das Levelskript am Levelende macht (Native FsChangeLevel mit dem Folgelevel aus dem Skript): Statistikbildschirm mit Speichern / Weiter / Wiederholen / Beenden, „Weiter“ lädt das nächste Level. Geprüft in zehn Leveln einschließlich Chickenwalker (1-7 New New York), Kanonen-Level (2-6 Schrottplatz 3), Ratte (4-1) und 5-3.

## 2. G5: Kamera am rechten Stick

### 2.1 Was das Spiel tut

Die Folgekamera (Kameraobjekt `[[0x2DABC4]+0x8C]`, Modus 0, `FUN_00015990`, einmal pro Tick) liest den Stickwert des Spielers (`+0xB38`, nach der Kennlinie des Spiels mit Totzone 0,4) und rechnet in 0,01-s-Unterschritten:

* Eingang `u = Stick · (|Stick| < 0,9 ? 0,25 : 1) · 35`
* Stufe 1 `v = (u + v) · 0,91`, begrenzt auf ±70
* Stufe 2 `w = (v · 0,001 + w) · 0,97` – Zeitkonstante **0,33 s**
* Winkel des Ticks `Σ w · 0,01` → Drehmatrix (`MakeYRotation`, Aufruf 0x17B2B) → neue Kameraposition um die Figur → Kollision.

Belege mit Adressen und Konstanten: `g5_camera.md` 3.

### 2.2 Umsetzung „präzise“

| Teil | Datei | Inhalt |
|---|---|---|
| Probe `FUN_00015990` (Folgekamera) | `src\host\xapi\camera_response.cpp` | Stickwert d (+0xB38, nur bei steuerbarer Figur) → Ziel d · 200°/s, Glättung 20 ms → Winkel des Ticks; nach dem Original den Filterzustand des Spiels löschen (kein Nachgleiten, kein Sprung beim Übergang) |
| Probe `NiMatrix3::MakeYRotation` 0x103020 | ebenda | nur beim Aufruf der Folgekamera (Rückkehr 0x17B30) und nur während einer präzisen Drehung: Winkel ersetzen; danach rechnet das Spiel Position und Kollision selbst |
| Rechter Stick der Controller | `src\host\input\input_system.cpp`, `xinput_source.*` (`SetPadShaper`), `stick_curve.h` | radiale Totzone 0,18, linear bis Ausschlag 0,95, über die Umkehrung der Spielkurve kodiert; nicht bei Freecam |
| Mitschnitt | `camera_response.cpp`, `input_script.*` | Skriptbefehl `game camera_trace on|off`: eine Zeile je Tick mit dem Gierwinkel (ohne Hook, auch in „Original“) |
| Schema | `src\host\core\config_schema.cpp` | `[input] camera_response = original|precise` mit deutschen/englischen Texten |

Bei mittleren Ausschlägen entspricht die präzise Rate dem Endwert des Originals (halber Ausschlag: 100 gegen 98°/s), bei vollem Ausschlag ist sie ein Viertel höher (200 gegen 155,6°/s) – der Unterschied liegt vor allem darin, dass „präzise“ die Rate sofort erreicht.

### 2.3 Messung (Level 1-1, Sprungeingabe über das Eingabeskript)

Skript `tools\input_scripts\g5_camera.txt`, Auswertung `analysis\loader\scripts\g5_camera_trace.py`, Läufe `run\g5g7\original`, `precise`, `long_original`, `long_precise` (RelWithDebInfo, 60 Hz, `[debug] movies = skip`):

| Eingabe | Messgröße | Original | Präzise |
|---|---|---|---|
| RX = −1,0 für 1,75 s | 50 % / 90 % / 95 % der Endrate | 183 / 633 / 817 ms | 17 / 33 / 50 ms |
| | Endrate | 155,1–155,3°/s (nach ≈ 1,2 s) | 200,0°/s |
| RX = ±1,0 für 1 s | gedreht | 113–114° | 193–194° |
| | nach dem Loslassen: Stillstand / Nachlauf | 217–433 ms / 6–10° | 100 ms / 1,1° |
| RX = ±0,6 (Spielwert 0,433) für 1,5 s | Endrate | 82,5°/s | 82–87°/s |
| | 50 % / 90 % | 267 / 650 ms | 17 / 33 ms |
| | nach dem Loslassen: Stillstand / Nachlauf | 383–400 ms / 5,6–6,2° | 33–83 ms / 0,1–0,5° |

Die Werte schwanken um ±3 %, weil der Blickpunkt des Spiels eigens geglättet wird. In beiden Varianten hält die Kollision die Kamera an denselben Wänden des Hangars an (+72° bzw. −235° vom Start) – die Kollision ist unverändert.

Mit „Original“ meldet das Protokoll `hooks: 181 installed` (wie ohne G5), mit „Präzise“ 183 und am Ende `camera summary: precise response, … rotations replaced`.

## 3. G7: Level-Skip-Cheat

### 3.1 Ablauf

1. Der Port sieht die Controller-Eingaben jedes Ticks (Filterkette des Eingabe-Routers, nach dem Freecam-Filter). Eine Folge zählt nur, solange die Figur steuerbar ist: im Level, geladen, Eingaben an (`+0x630`), keine Zwischensequenz, keine Pause, kein Speicherdialog, kein Statistikbildschirm, Freecam aus.
2. Gezählt werden Drücke von A und B (jeweils mit Loslassen dazwischen). Die letzten zehn Drücke müssen A A B B A A B B A A sein, innerhalb von 4 s und mit höchstens 1 s Pause. Jede andere Taste oder A+B zugleich beginnt von vorn; die Sticks stören nicht.
3. Treffer: Protokollzeile `cheat: level skip (A A B B A A B B A A, port 1) in Level1-1 - level end via the script native FsChangeLevel("Level1-2") …`. Zum nächsten Tick ruft der Port das Levelskript-Native FsChangeLevel (`FUN_000B0520`) mit dem Folgelevel – derselbe Aufruf wie am Ende des Levelskripts: Levelzeit/Bestzeit, „alles gesammelt“, Zustand fürs nächste Level, Statistikbildschirm.
4. Die Drücke erreichen auch das Spiel (Springen/Angreifen), wie bei den eingebauten Cheatcodes.

Das Folgelevel steht in einer Tabelle, die aus den Levelskripten stammt (in jedem `scripts\levelN-M.ful` steht genau ein Levelname = Argument von Opcode 0xA1): 1-1→1-2→1-3→1-4→1-5→1-7→1-6→2-1→2-6→2-2→2-3→2-4→2-5→3-1→3-2→3-3→3-6→3-4→4-1→5-1→5-2→5-3→1-1 (Dateinamen; der Unit-Test prüft, dass die Kette alle 22 Level der G4-Tabelle in Spielreihenfolge durchläuft).

### 3.2 Prüfläufe (`tools\input_scripts\g7_level_skip.txt`, je 42–57 s, Rückgabe 0)

Skript: Neues Spiel → steuerbar → Folge → Statistik → RECHTS („Weiter“) → A → Rückfrage „Weiter? Sie können erst am Ende des nächsten Levels wieder speichern!“ → LINKS („Ja“) → A → nächstes Level lädt → steuerbar → Folge noch einmal.

| Lauf | Startlevel | 1. Skip | 2. Skip (im Folgelevel) |
|---|---|---|---|
| `skip11` | Level 1-1 (Standard) | 1-1 → Statistik („Nächster Level: Kanalisation“) | 1-2 (Kanalisation, Fry steuerbar) → FsChangeLevel("Level1-3") |
| `dbg21` | `start_level = Level2-1` | 2-1 → „Level2-6“ | 2-6 (Minentunnel) → „Level2-2“ |
| `skip16` | Level1-6 (Chickenwalker) | im Walker-Cockpit → „Level2-1“ (Statistik „New New York“, „Nächster Level: Wiesel-Cañon“) | 2-1 → „Level2-6“ |
| `skip25` | Level2-5 (Kanonen-Level) | am Levelanfang (zu Fuß) → „Level3-1“ | 3-1 → „Level3-2“ |
| `skip41` | Level4-1 (Ratte) | → „Level5-1“ (Statistik „Bogad-Sumpf“) | 5-1 → „Level5-2“ |
| `skip53` | Level5-3 (Railgun) | → „Level1-1“ (der Abspann des Levelskripts entfällt) | 1-1 → „Level1-2“ |

Beobachtet: Level 2-6 beginnt nach dem ersten steuerbaren Tick mit einer Szene ohne HUD (etwa 7 s, Figur nicht steuerbar). Drücke in dieser Zeit zählen nicht (Debug-Zeile `cheat: press … not counted for the level skip - character not controllable …`); das Skript wartet deshalb, bis die Figur steuerbar bleibt.

### 3.3 Waffen beim Level-Skip (Nachtrag 2026-10-13)

**Problem (Nutzer):** Nach dem Skip von 1-1 nach 1-2 hatte Fry keine Waffe. FsChangeLevel baut den Zustand für das Folgelevel aus dem aktuellen Inventar (`FUN_000C26E0`). Wer gleich am Anfang von 1-1 springt, hat noch nichts eingesammelt: `Ammo=0|0.000000` für alle 5 Plätze, keine `Pickup=`/`Equip=`-Zeile (Lauf `run\fix\skip11`, Zeile `save: debug action state: Level=Level1-1;Health=100.000000;Ammo=0|0.000000;…;Lives=5;…`).

**Lösung:** Nach dem FsChangeLevel des Skips ergänzt der Port den anstehenden Zustand (`DAT_002EA988`) um die Waffen der Tabellenzeile des **Ziellevels** (0x2D60BC, dieselbe Quelle wie Startlevel und Cheatcodes) und setzt ihn mit `FUN_000C1D30` neu:

* Was der Spieler hat, bleibt. Fehlende Waffen der Zeile kommen dazu, ohne Doppelte (Namen ohne Groß-/Kleinschreibung).
* `Equip=` der Tabelle gilt nur, wenn nichts ausgerüstet ist. Besitzt der Spieler die Waffe schon unausgerüstet, wird seine Zeile zu `Equip=`. Sonst wird die Waffe als `Pickup=` ergänzt.
* Kam eine Waffe dazu, bekommt jeder Munitionsplatz mindestens den `Ammo=`-Wert der Zeile (100). `FUN_000C5340` setzt fehlende Plätze auf 0; eine Pistole ohne Munition wäre keine Waffe. Mehr Munition als die Zeile bleibt.
* `play_as`: Ersetzt die Einstellung die Figur des Ziellevels (z. B. `bender` in 1-2), werden keine Waffen ergänzt (sie würden beim Laden ohnehin entfernt). Protokoll `cheat: level skip to Level1-2: no start weapons added - play_as bender replaces Fry`.
* Level ohne Waffen in der Tabelle (1-1, Bender-, Leela-, Zoidberg-Level) bleiben unverändert. Der Skriptbefehl `game level_end` (M7) ändert nichts.

Die reine Logik ist `xapi::select::MergeStartWeapons` (`src\host\xapi\game_select.cpp`), Tests in `tests\game_select_tests.cpp` („G7 fix: …“, 4 Fälle).

**Prüfung:** Lauf `run\fix\skip11` (16:9, Skript: Neues Spiel → Skip sofort in 1-1 → „Weiter“ → „Ja“ → 1-2):

```
cheat: level skip to Level1-2: start weapons of table row 1 added [Hammer,Pistol], equipped Pistol, ammo raised to the table value - state now: Health=100.000000;Ammo=0|100.000000;…;Power=0.000000;Pickup=Hammer;Equip=Pistol;Lives=5;…
game: start state applied in Level1-2: Level=Level1-2;Health=100.000000;Ammo=0|100.000000;…;Pickup=Hammer;Equip=Pistol;Lives=5;…
save: debug action state: Level=Level1-2;…;Pickup=Hammer;Equip=Pistol;…
```

Bild `run\fix\skip11\shots\frame_002524.png`: Fry hält in 1-2 die Pistole, Waffen-HUD oben rechts mit 100 Schuss.

## 4. Prüfung

* Bauten: RelWithDebInfo `build\g5g7`, Debug `build\g5g7-werr` und clang-cl Debug `build\g5g7-clang` (beide `FUT_WERROR=ON`) ohne Warnungen. `fut_tests` in allen drei Bauten: 483/483 bestanden (1 übersprungen).
* Neue Unit-Tests `tests\g5_g7_tests.cpp` (11 Fälle): Parser und Schema beider Schlüssel; `PreciseTurn` (Anstieg, proportional, Stillstand, Begrenzung); Modell des Originalfilters gegen die Messwerte (155,6 und 85,2°/s); Stick-Kennlinie über die Spielkurve; Prologe, Aufrufstellen (0x17B2B → 0x103020, 0x19CCB → 0x15990) und Filterkonstanten gegen die Retail-XBE; Skriptbefehl `game camera_trace`; Folgelevel-Tabelle; Sequenzerkennung (Loslassen, Fenster 4 s, Pause 1 s, andere Tasten, A+B, nicht scharf, 50 Hz); Filterkette und `Diverted` des Routers.
* `tools\m4_check.py --exe build\g5g7\bin\futurama.exe` mit der Konfiguration von `run\verify-m5\futurama.ini` (Kopie mit eigenen Benutzerdaten `run\g5g7\verify-m5`): **ALL PASS** (inkl. M4-7 Rumble, M4-9).
* Launcher (unverändert, baut die Reiter aus dem Schema): „Kamera am rechten Stick“ erscheint im Reiter „Steuerung“, „Level-Skip-Cheat (A A B B A A B B A A)“ im Reiter „Spiel“ (`run\g5g7\launcher\shots\de_tab_controls_full.png`, `de_main_game.png`, mit `--schema-file` aus dem neuen `futurama.exe`).
* Hinweis: Im gemeinsamen Arbeitsbaum arbeitete gleichzeitig der G6-Agent (Ego-Steuerung); seine Dateien `xapi\first_person.*` waren in diesen Bauten mit enthalten (eigene Hook-Adressen, keine Überschneidung mit G5).

## 5. Checkliste für den Nutzer

**G5 – Kamera** (mit echtem Controller und mit Maus; beides konnte der Agent nicht prüfen):

1. Launcher → Reiter **Steuerung** → **Kamera am rechten Stick** = „Präzise (direkt und schnell)“, Spiel starten, Level 1-1.
2. Rechten Stick leicht antippen: Die Kamera dreht langsam und hört sofort auf, wenn der Stick losgelassen wird. Ein leichter Ausschlag (etwa ein Viertel) dreht schon – im Original passiert dort noch nichts.
3. Voll auslenken: Die Kamera dreht sofort mit voller Geschwindigkeit (≈ 200°/s, etwa eine halbe Umdrehung in knapp einer Sekunde) und bleibt beim Loslassen stehen.
4. Gegen eine Wand drehen: Die Kamera bleibt wie im Original hängen bzw. rückt an die Figur heran; sie schaut nicht durch Wände.
5. Rechten Stick nach oben/unten: Höhe wie im Original (federt zurück).
6. Mit der Maus (Standardbelegung): Drehen fühlt sich direkt an, ohne Nachlauf. Bei Bedarf **Mausempfindlichkeit** anpassen.
7. Ruht der Stick, dreht sich die Kamera wie im Original langsam hinter die Figur.
8. Zurück auf „Original“: alles wie vorher. Rückmeldung erwünscht: Ist 200°/s zu schnell oder zu langsam? Ist die tote Zone (0,18) zu klein (Kamera driftet von selbst)? Dann `[input] pad_deadzone` erhöhen oder melden.

**G7 – Level-Skip-Cheat:**

1. Launcher → Reiter **Spiel** → **Level-Skip-Cheat** ist an (Standard).
2. In einem Level, sobald die Figur steuerbar ist: **A A B B A A B B A A** zügig drücken (innerhalb von 4 Sekunden, höchstens 1 Sekunde Pause, keine andere Taste dazwischen). Fry springt und schlägt dabei – das ist normal.
3. Es erscheint der Statistikbildschirm mit „Nächster Level: …“. **Speichern** speichert wie am echten Levelende; **Weiter** → „Ja“ lädt das nächste Level.
4. Gegenprobe: In Menüs, in der Pause und in Zwischensequenzen passiert nichts. Mit ausgeschaltetem Schalter passiert im Level nichts.
5. Bekannt: Was das Level an seinem Ende sonst noch zeigt (Schlussszene, in 5-3 der Abspann), entfällt; der Abspann lässt sich im Menü „Extras“ ansehen, sofern freigeschaltet (**Vermutung**).

## 6. Offen

* G5 ohne echten Controller und ohne echte Maus geprüft (Skript-Eingaben, Unit-Tests): Gefühl, Geschwindigkeit und tote Zone bestätigt der Nutzer.
* G5 betrifft nur die Folgekamera (Modus 0). Ego-Ansicht (Modus 3, G6), Chickenwalker-, Kanonen- und Rattensteuerung bleiben beim Drehen original (nur die kleinere tote Zone des Sticks wirkt dort mit).
* G7 in 2-5 nur am Levelanfang geprüft (zu Fuß), nicht während der Kanone selbst; in 1-6 im Walker geprüft.
* G7 überspringt die Schlussszene eines Levels (z. B. Abspann nach 5-3). Ob ein Level-Ende-Film eines Levels Story-Fortschritt trägt, der später fehlt, ist nicht geprüft (**Vermutung**: nein, der Spielstand speichert nur Level und Inventar, `g4_select.md` 2.3).
