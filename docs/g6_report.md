# G6: Ego-Steuerung – Bericht

Stand: 2026-10-12. Analyse: [`analysis/loader/g6_firstperson.md`](../analysis/loader/g6_firstperson.md). Abweichungen vom Entwurf: [`loader_design.md`](loader_design.md) 8.20.

## Kurzfassung

* **Neu: `[input] first_person = on`** (Launcher, Reiter „Steuerung“, „Egoperspektive (experimentell)“): Man spielt aus der Sicht der Figur. Die Kamera sitzt im Kopf, die eigene Figur ist unsichtbar. Rechter Stick oder Maus sehen sich um. Linker Stick bzw. WASD laufen in Blickrichtung, rückwärts und **seitwärts**. Springen, Angriffe und Zielerfassung funktionieren. Frys Waffen treffen die Bildmitte, mit dem Fadenkreuz des Spiels. Das HUD bleibt.
* **Grundlage ist die Ego-Ansicht des Spiels selbst** (rechten Stick drücken). Im Original ist sie nur ein Stand-Zielmodus: Man kann darin nicht laufen und nicht springen, und sie endet bei jedem Sprung oder Angriff. Der Port hebt diese Sperren auf (6 Probe-Hooks) und steuert den Blick direkter. Die Ansicht ist im Level von Anfang an aktiv und kommt nach Tod, Zwischensequenz oder Levelwechsel von selbst zurück.
* **Umschalten im Spiel:** rechten Stick drücken (Tastatur V, mittlere Maustaste), wie im Original – Ego ↔ Verfolgerkamera.
* **Standard `off`:** Es wird kein Hook installiert, das Spiel bleibt unverändert (181 Hooks wie ohne G6).
* Die Level mit Fahrzeug oder Sonderobjekt (1-7 New New York/Chickenwalker = Datei `Level1-6`, 2-6 Schrottplatz 3/Kanone = `Level2-5`, 4-1 Ratte, 5-3 Railgun) behalten die Kamera des Spiels. Ebenso Zwischensequenzen und Kamerafahrten.

## Umsetzung

| Teil | Datei | Inhalt |
|---|---|---|
| Logik | `src\host\xapi\first_person.h/.cpp` | 6 bedingte Probes (Abschnitt 6.1 der Analyse), Blickzustand, Kennlinie, Bewegung relativ zum Blick, HUD/Visier, near-Ebene, Wandabstand, Spur `first person trace` |
| Funktionsanfänge | `tools\gen\extra_function_starts.tsv` → `src\host\gen\xbe_functions.gen.inc` | zwei nur über die vtable erreichte Funktionen (Sprung 0x50D10, R3-Handler 0x588B0) als Hook-Ziele |
| Maus | `src\host\input\kbm_source.h/.cpp` | `SetMouseMotionQuery`: Solange die Ego-Ansicht läuft, geht die Mausbewegung nicht an den rechten Stick, sondern direkt an den Blick (0,08° je Zählschritt × „Mausempfindlichkeit“) |
| Schema | `src\host\core\config_schema.cpp` | `[input] first_person` (off/on), `first_person_look_speed` (Drehtempo am Stick, Standard 180°/s) |
| Tests | `tests\first_person_tests.cpp` | 6 Fälle: Parser/Schema, Drehmatrix = Matrix des Spiels, Stick-Kennlinie (auch mit G5 „Präzise“), Bewegung relativ zum Blick, Prologe/Aufrufstellen gegen die Retail-XBE, Maus-Weiche |
| Prüfskript | `tools\input_scripts\g6_first_person.txt`, `analysis\loader\scripts\g6_firstperson_trace.py` | Lauf und Auswertung „läuft die Figur in Blickrichtung?“ |

Der Launcher braucht keine Änderung: Er baut die Zeilen aus dem Schema (Screenshot `run\g6_fp\launcher\shots\de_tab_controls_full.png`).

## Prüfung

* Bauten: RelWithDebInfo `build\g6-fp`, Debug `build\g6-werr` und clang-cl Debug `build\g6-clang` (beide `FUT_WERROR=ON`), ohne Warnungen. `fut_tests` in allen drei Bauten bestanden.
* `tools\m4_check.py --config run\verify-m5\futurama.ini` (Option aus): ALL PASS, keine G6-Hooks.
* Läufe (`run\g6_fp\<Lauf>\`, `[debug] movies = skip`, je 30–45 s, Rückgabe 0). Details und Messwerte in Abschnitt 7 der Analyse.

| Lauf | Was | Ergebnis |
|---|---|---|
| `orig` | Ego-Ansicht des Originals (ohne G6) | stehen, linker Stick kippt den Blick, A/B ohne Wirkung |
| `fp11` | 1-1 Fry | vorwärts, seitwärts, drehen (180°/s), auf/ab, rückwärts, Sprung, R3 aus/an |
| `fp12`, `fp12shoot` | 1-2 Fry mit Pistole | Fadenkreuz in der Bildmitte; 4 Schüsse in 4 Richtungen 0–0,9° neben der Blickrichtung |
| `fp21` | 2-1 Bender | volles Bild statt Visier, HUD, laufen, Angriffe |
| `fp31` | 3-1 Leela | HUD, laufen, Angriff auf ein Skelett |
| `fp31lava` | 3-1 in die Lava | Tod in der dritten Person, nach dem Neustart wieder Ego |
| `fplevelend` | 1-1 → Levelende → 1-2 | Ego auch im nächsten Level |
| `fp11precise` | mit G5 „Präzise“ | beide Optionen zusammen ohne Konflikt |

* Gemessen (Spur `first person trace`): Ohne Hindernis läuft die Figur genau in die gewünschte Richtung (Kosinus 1,000), seitwärts so schnell wie vorwärts (25 Einheiten/s), rückwärts mit dem langsameren Rückwärtstempo des Spiels.

## Checkliste für Spieler

1. Launcher öffnen, Reiter **Steuerung** → **Egoperspektive (experimentell)** auf „An: Ego-Steuerung“. Optional **Ego: Drehtempo am Stick** (Standard 180°/s) und **Mausempfindlichkeit** anpassen.
2. Spiel starten, „Neues Spiel“ (oder ein Startlevel im Reiter „Spiel“). Sobald die Figur steuerbar ist, schaltet die Ansicht in die Egoperspektive.
3. Prüfen und Rückmeldung geben:
   * [ ] Umsehen mit dem **rechten Stick**: fühlt sich das Tempo gut an? Tote Zone in der Mitte passend?
   * [ ] Umsehen mit der **Maus** (Fenster anklicken, damit die Maus gefangen ist): Richtung und Empfindlichkeit?
   * [ ] **Auf/ab:** Am Controller gilt die Einstellung des Spiels: Ohne die Spieloption „Ziel umkehren“ schaut Stick nach oben **nach unten** (wie in der Ego-Ansicht des Originals). Die Maus schaut nach vorn geschoben nach oben. Die Spieloption kehrt beides um. Passt das so, oder wird ein eigener Schalter gewünscht?
   * [ ] **Laufen** vorwärts/rückwärts/seitwärts mit linkem Stick bzw. WASD, **Springen** (B/Leertaste), Sprünge über Abgründe.
   * [ ] **Angriffe:** Fry (Hammer, Waffen; Fadenkreuz in der Bildmitte, Waffenwechsel LB/RB), Bender und Leela (A/X). **Ziel erfassen** mit RT.
   * [ ] **Rechten Stick drücken** (Tastatur V, mittlere Maustaste): Wechsel zur Verfolgerkamera und zurück.
   * [ ] Zwischensequenzen, Tod, Levelende: Danach ist die Egoperspektive wieder da.
   * [ ] Kamera an Wänden und in engen Gängen: Sieht man irgendwo durch Wände?
4. Zurück zum Original: Option auf „Aus (wie im Original)“.

## Bekannte Grenzen

* **Experimentell.** Rätsel- und Sprungstellen sind für die Verfolgerkamera gebaut. Abgründe direkt vor den Füßen sieht man schlechter; dann kurz mit dem rechten Stick nach unten schauen oder umschalten.
* **Keine sichtbare Waffe oder Hand.** Die Waffe hängt an der ausgeblendeten Figur. Das Fadenkreuz des Spiels zeigt bei Fry, wohin er schießt.
* Benders Spezialangriff (A und X zusammen mit voller Ladung) sperrt das Spiel in seiner Ego-Ansicht. Er geht nur in der Verfolgerkamera.
* Wird die Figur von einem Gegner gepackt, stirbt sie oder beginnt eine Zwischensequenz, zeigt das Spiel seine eigene Kamera. Die Egoperspektive kehrt danach von selbst zurück. Zwischensequenzen mitten im Level wurden nicht im Lauf geprüft.
* Mit einem Fry-Zielfernrohr (Sichtfeld 45°) dreht der Blick mit halbem Tempo, wie im Original.
* 1-7 (Chickenwalker), 2-6 (Kanone), 4-1 (Ratte) und 5-3 (Railgun) behalten immer die Kamera des Spiels.
* Ohne echten Controller und ohne echte Maus geprüft (Eingabeskript wie ein Controller; Maus per Unit-Test).
