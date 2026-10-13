# G4: Level- und Figurenauswahl – Bericht

Stand: 2026-10-12. Analyse: [`analysis/loader/g4_select.md`](../analysis/loader/g4_select.md). Abweichungen vom Entwurf: [`loader_design.md`](loader_design.md) 8.17.

## Kurzfassung

* **`[game] start_level`** (Launcher, Reiter „Spiel“, „Startlevel“): „Neues Spiel“ beginnt in einem der 22 Level, die das Spiel erreicht, in Spielreihenfolge (1-1 Planet Express … 5-3 Moms Hauptquartier). Startzustand (Gesundheit, Munition, Fry-Waffen) und Name kommen aus der Tabelle des Spiels (0x2D60BC), also aus derselben Quelle wie der eingebaute Debug-Cheat. Danach läuft die normale Levelfolge. Level 3-5 wird nirgends erreicht und deshalb nicht angeboten. „Spiel laden“ ist nicht betroffen.
* **`[game] play_as`** (Launcher, „Spielfigur (experimentell)“): `fry`, `bender` oder `leela` statt der Figur des Levels. Der Port benennt beim Laden den Def der Spielfigur um; Modell, Klasse, Fähigkeiten, Sounds, HUD-Porträt und Ladeleiste folgen daraus. Waffen (`Pickup=`/`Equip=`) werden aus dem anstehenden Startzustand entfernt. Level 1-7 New New York (Datei `Level1-6`, Chickenwalker), 2-6 Schrottplatz 3 (`Level2-5`, Kanone), 4-1 (Ratte) und 5-3 (Railgun) behalten ihre Figur. Das steht im Log.
* **Standard `original` für beide:** Es wird kein Hook installiert, das Spiel bleibt unverändert. Neu dafür sind bedingte Probes (`FUT_PROBE_IF`): Ist die Bedingung aus, entsteht weder eine Logzeile noch ein Eintrag in der Hook-Summe.

## Umsetzung

| Teil | Datei | Inhalt |
|---|---|---|
| Logik | `src\host\xapi\game_select.h/.cpp` | Leveltabelle (22 Zeilen, Spielreihenfolge = Tabellenzeile), Parser, Sperrliste, Namensabbildung, Waffenfilter, 4 bedingte Probes |
| Start-Level | Probe `FUN_000A2340` (Prolog `8B 44 24 04 85 C0`) | Nur beim Aufruf aus „Neues Spiel“ (Rücksprung 0x200B8): `FUN_000C1D30(Zustand, Länge)`, danach der Levelname an das Original. Zuvor wird geprüft, ob die Tabellenzeile mit `Level=<Name>` beginnt; sonst Warnung und Level 1-1 |
| Spielfigur | Probes PlayerDef-ctor 0x5E580, `FindDef` 0xC3E80, `FindCharDef` 0xC3F20 | Ein Spieler-Def-Name einer anderen Figur (fry, fry-1-x, fry-5-3, railgunfry, bender, leela) wird zu `fry`, `bender` oder `leela`; die eigene Variante (z. B. `fry-1-3`) bleibt |
| Hooks | `src\host\core\hooks.h/.cpp` | `HookDesc::condition`, `FUT_PROBE_IF` |
| Schema | `src\host\core\config_schema.cpp` | 2 Auswahllisten mit deutschen und englischen Texten, Hilfe mit den Grenzen |
| Tests | `tests\game_select_tests.cpp` | 5 Fälle: Parser, Schema-Liste = Leveltabelle, Tabelle und Prologe gegen die Retail-XBE, Sperrliste und Abbildung, Waffenfilter |

Der Launcher brauchte keine Änderung: Er baut die Reiter aus dem Schema (Screenshot `run\g4_impl\launcher_shots\de_tab_game_full.png`).

## Prüfung

* Bauten: RelWithDebInfo `build\g4-select`, Debug `build\g4-werr` und clang-cl Debug `build\g4-clang` (beide `FUT_WERROR=ON`) ohne Warnungen. `fut_tests` 467/467 bestanden (1 übersprungen). Launcher `build\g4-launcher` Release: ctest 4/4.
* `tools\m4_check.py --config run\verify-m5\futurama.ini`: ALL PASS.
* Läufe (`run\g4_impl\<name>\`, `[debug] movies = skip`, Skript `start.txt`: Neues Spiel → steuerbar → laufen, X, Screenshot; je etwa 27 s, Rückgabe 0):

| Lauf | Einstellung | Ergebnis (Log/Bild) |
|---|---|---|
| `default` | keine | 1-1 mit Fry; `hooks: 181 installed` |
| `explicit` | beide `original` | Hook-Zeilen gleich wie `default` (182 Zeilen ohne Host-Adressen) |
| `start21` | `start_level = Level2-1` | `"Neues Spiel" starts Level2-1 (Bender) …`, Bender steuerbar (`shots\frame_001503.png`) |
| `start31` | `start_level = Level3-1` | Leela im Innenhof I (`shots\frame_001268.png`) |
| `bender11` | `play_as = bender` | `Level1-1 player def "fry-1-1" -> "bender"`, Bender in 1-1 (`shots\frame_001574.png`); 3 zusätzliche Hooks |
| `leela21` | `play_as = leela`, `start_level = Level2-1` | `Level2-1 player def "Bender" -> "leela"`, Leela im Canyon (`shots\frame_001577.png`) |
| `fry16`, `bender16` | `play_as = fry` bzw. `bender`, `start_level = Level1-6` | `Level1-6 keeps its player def "fry" - blocked level …`, Fry im Chickenwalker (`shots\frame_001474.png`, `…001552.png`) |

## Checkliste für Spieler

1. Launcher öffnen, Reiter **Spiel**.
2. **Startlevel** wählen (z. B. „2-1 Canyon (Bender)“), Spiel starten, im Hauptmenü **Neues Spiel**. Zurück zum normalen Spiel: „Wie im Original (Level 1-1)“.
3. Optional **Spielfigur (experimentell)**: Fry, Bender oder Leela. Bekannte Grenzen:
   * Fry hat in Bender- und Leela-Leveln keine Waffen und kann dort nicht angreifen.
   * Hinweistexte, Stimmen und Filme bleiben die der ursprünglichen Figur.
   * Das Symbol der Sammelobjekte unten rechts im HUD kann fehlen.
   * Ob sich jedes Level mit jeder Figur durchspielen lässt, ist nicht geprüft (Rätsel, Sprunghöhen).
   * 1-7 New New York, 2-6 Schrottplatz 3, 4-1 Bogad-Sumpf und 5-3 behalten immer ihre Figur.
4. Spielstände speichern wie gewohnt. Die Figur wird nicht gespeichert, beim Laden gilt die aktuelle Einstellung.

## Eingebaute Cheatcodes

Diese Codes gibt es im Originalspiel; sie funktionieren im Port ohne Einstellung. Im Level **LT gedrückt halten**, Ziffern eingeben und mit **BACK** bestätigen. Ziffern: Y = 1, B = 2, A = 3, X = 4; mit gehaltenem RT: Y = 5, B = 6, A = 7, X = 8; WHITE = 9, BLACK = 0. Jede andere Taste löscht die Eingabe. Tastenfolgen erzeugt `analysis\loader\scripts\g4_tables.py`.

| Code | Wirkung |
|---|---|
| 3853858511 | Level 1-1 Planet Express |
| 3853858512 | Level 1-2 Kanalisation (mit Hammer, Pistole) |
| 3853858513 | Level 1-3 U-Bahn (Hammer, Pistole) |
| 3853858514 | Level 1-4 Old New York (Hammer, Pistole, Schrotflinte) |
| 3853858515 | Level 1-5 Rotlichtviertel (Hammer, Pistole, Schrotflinte) |
| 3853858516 | Level 1-6 Uptown (dazu Maschinengewehr) |
| 3853858517 | Level 1-7 New New York |
| 3853858521 | Level 2-1 Canyon |
| 3853858522 | Level 2-2 Minentunnel |
| 3853858523 | Level 2-3 Mine |
| 3853858524 | Level 2-4 Schrottplatz 1 |
| 3853858525 | Level 2-5 Schrottplatz 2 |
| 3853858526 | Level 2-6 Schrottplatz 3 |
| 3853858531 | Level 3-1 Innenhof I |
| 3853858532 | Level 3-2 Linker Flügel |
| 3853858533 | Level 3-3 Rechter Flügel |
| 3853858534 | Level 3-4 Innenhof II |
| 3853858535 | Level 3-5 Innerer Tempel |
| 3853858541 | Level 4-1 Bogad-Sumpf |
| 3853858551 | Level 5-1 Moms Hauptquartier (Bender) |
| 3853858552 | Level 5-2 Moms Hauptquartier (Leela) |
| 3853858553 | Level 5-3 Moms Hauptquartier (Fry) |
| 3853856701 | volle Gesundheit |
| 3853856702 | Munition 999 |
| 3853856703 | 5 Leben mehr |
| 3853856704 | Power/Aufladung voll |
| 3853856705 | Unverwundbarkeit an/aus |
| 3853856706 | Extras entsperren an/aus (Vermutung, nicht geprüft) |
| 3853856711 | alle Zwischensequenzen sofort überspringbar an/aus |

Beispiel 2-1: LT halten, `A, RT+X, RT+Y, A, RT+X, RT+Y, RT+X, RT+Y, B, Y`, BACK (im Port geprüft).

## Nachtrag 2026-10-13: Waffen beim Startlevel

Nutzerbericht: „Level skip oder level select gibt den Charakteren scheinbar ihre Waffen nicht mit (Fry in der Kanalisation)“.

* **Startlevel (`start_level = Level1-2`, `play_as = original`):** nicht reproduzierbar. Der Zustand der Tabellenzeile (`Pickup=Hammer`, `Equip=Pistol`, `Ammo=100.0`) kommt an: Fry hält die Pistole, das Waffen-HUD zeigt die Pistole mit 100 Schuss (`run\fix\start12a\shots\frame_001385.png`, Arbeitsbaum `fix-gameplay`). Die neue Protokollzeile nach dem Anwenden lautet `game: start state applied in Level1-2: Level=Level1-2;Health=100.000000;Ammo=0|100.000000;…;Pickup=Hammer;Equip=Pistol;Lives=5;…` (`run\fix\fog169\logs\futurama.log`), also hat Fry auch den Hammer.
* **Def-Namen:** Die `defs.nif` der Fry-Level enthalten `WeaponDef::Hammer` und `WeaponDef::Pistol` (1-1, 1-2), dazu `Shotgun` (ab 1-3) und `MachineGun` (1-5, 1-6, 1-7); die Namen sind längenpräfixiert. „HammerP“ in `g4_select.md` war ein Lesefehler (das Byte nach dem Namen). `FindDef` 0xC3E80 findet die Tabellennamen also.
* **play_as:** Mit `original` ist kein Hook installiert, nichts wird entfernt. Mit `fry` bleibt der Def eines Fry-Levels (eigene Variante `fry-1-2`), es wird ebenfalls nichts entfernt. Nur `bender`/`leela` entfernen die Waffen in Fry-Leveln, wie oben beschrieben. Die Fry-Level enthalten keine Bender- oder Leela-Defs als Nebenfiguren (geprüft für alle `defs.nif`).
* Das Bild des Nutzers (4 Leben, kein Waffen-HUD) passt zum **Level-Skip** aus 1-1: Dort baut das Spiel den Zustand aus dem Inventar, und am Anfang von 1-1 hat Fry nichts. Das behebt der Nachtrag in `g5_g7_report.md` 3.3.
* **Neu zur Diagnose:** Nach jedem Levelstart mit Startzustand steht einmal `game: start state applied in <Level>: …` im Protokoll (Beobachter am Tick-Takt, kein Hook). Der Skriptbefehl `game state` schreibt den aktuellen Zustand (`save: debug action state: …`; `FUN_000C26E0` mit Ausgabepuffer).

## Offen

* Ganze Level mit fremder Figur durchspielen (Rätsel, Tod, Neustart, Levelende) – nicht geprüft.
* Fry in Fremdleveln Waffen geben (Waffen-Defs anderer Level nachladen) – nicht umgesetzt.
