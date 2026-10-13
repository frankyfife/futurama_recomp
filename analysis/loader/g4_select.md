# G4: Figuren- und Levelauswahl – Machbarkeit

Stand: 2026-10-12. Basis: Retail-EU-XBE (`orig\retail_eu\default.xbe`, Capstone), Ghidra-Export `analysis\retail_eu_export`, Spieldaten auf dem ISO (nur gelesen; Auszüge zur Analyse unter `run\g4_select\data`, git-ignoriert) und sechs kurze Läufe des Ports (`build\g4-select\bin\futurama.exe`, Arbeitsbaum vom 2026-10-12 ohne Änderungen, je 35–45 s, `run\g4_select`, git-ignoriert). Unter `src\` und `launcher\` wurde nichts geändert. Die Experimente patchen nur den Speicher des laufenden Prozesses von außen (`scripts\g4_char_probe.py`).

Adressen sind VAs der Retail-EU-XBE. `FUN_xxx` sind Ghidra-Namen. „Vermutung“ kennzeichnet Deutungen, die nicht direkt aus Binary, Daten oder Lauf belegt sind.

Frage des Nutzers: „Haben wir einen Character Select implementiert? Wenn nein, bitte Machbarkeit prüfen.“ **Antwort: Nein.** Weder der Port noch das Spiel hat eine Figurenauswahl. Das Spiel hat aber eine **versteckte Debug-Levelauswahl über Cheatcodes** (22 Level). Sie funktioniert im Port unverändert (Abschnitt 2.4).

## Inhalt

1. Kurzfassung
2. Levelstruktur, Levelfolge, Startzustand, Spielstand, eingebaute Levelauswahl
3. Wie die Spielfigur entsteht (Daten, Klassen, Fähigkeiten)
4. Figuren-Override: Ansatz, Experiment, was bricht, Bewertung je Level
5. Levelauswahl: Umsetzung, Startzustand, Spielstände, Zwischensequenzen
6. Empfehlung, Aufwand, Risiken
7. Offen / Vermutungen
8. Skripte und Läufe

---

## 1. Kurzfassung

* **Levels:** 23 Level-Ordner, 22 davon in der Spielfolge: 1-1 → 1-2 → 1-3 → 1-4 → 1-5 → **1-7 → 1-6** → 2-1 → **2-6** → 2-2 → 2-3 → 2-4 → 2-5 → 3-1 → 3-2 → 3-3 → **3-6** → 3-4 → 4-1 → 5-1 → 5-2 → 5-3 → (Abspann) → Level1-1. Fry spielt Kapitel 1 und 5-3, Bender Kapitel 2 und 5-1, Leela Kapitel 3 und 5-2. In 4-1 spielt Zoidberg auf einer Ratte (`Rodent`). `Level3-5` hat keinen Vorgänger und keinen Cheatcode (Vermutung: herausgeschnitten). Das nächste Level bestimmt das **Levelskript** (`scripts\levelN-M.ful`, Skript-Opcode 0xA1 `EndLevel(name)` → `FUN_000B0520`).
* **Startzustand eines Levels** ist nur ein kurzer Text (`DAT_002EA988`): `Level=…`, `Health=…`, `Ammo=slot|wert` ×5, `Power=…`, `Pickup=…`/`Equip=…` (Waffen), `Lives=…`, `AgeSeconds/AgeHours`. Er wird am Levelende gebaut (`FUN_000C26E0`), beim Laden angewendet (`FUN_000C5340`) und ist zugleich der Inhalt eines Spielstands (`FUN_000DFE50`). Story-Flags über Levelgrenzen gibt es nicht. Jedes Levelskript kennt nur sein eigenes Folgelevel. **Die Figur steht nicht im Zustand.**
* **Figur:** Sie kommt aus den **Leveldaten**, nicht aus einer Code-Tabelle. Jedes `levels\levelN-M\defs.nif` enthält einen Knoten `CharacterDef::<name>` (`fry-1-1`, `Fry`, `Bender`, `Leela`, `Rodent` …). Eine Stage-NIF platziert die Instanz `<name>|<idx>::PLAYER`. Der Def-Lader `FUN_000C5B40` macht aus Spielernamen ein `PlayerDef` (`FUN_0005E580`, Modell `Chars\<name>.ucf`). Dessen Spawn `FUN_0005E620` wählt **anhand des Namens** eine von vier C++-Unterklassen einer gemeinsamen Basis `Player` (`FUN_0005D660`): Fry `0x29E1E8`, Bender `0x29D8E0`, Leela `0x29F0C0`, Rodent `0x2A0DB0`. Dazu kommen Soundsatz, HUD-Elemente (`weapons`/`charge`/`timer`), Animations-Root und einige **level- und figurgebundene Zusatzobjekte** (Fry + 1-6: Chickenwalker; Bender + 2-5: Kanone; Rodent: Ratten-Steuerung).
* **Figuren-Override ist machbar, getestet:** Drei Funktionen bilden den Def-Namen um (PlayerDef-ctor `0x5E580`, `FindDef` `0xC3E80`, `FindCharDef` `0xC3F20`). Damit startet **Bender in Level 1-1** (vtable 0x29D8E0, Typ 3, Bender-Porträt, Laufen/Springen/Angriff gehen), ebenso **Leela in 1-1** (0x29F0C0, Typ 2) und **Fry in 2-1** (0x29E1E8, Typ 1). Die Modelle aller Figuren liegen im globalen `FutPAL00.img` und laden in jedem Level. Fry hat in Nicht-Fry-Leveln aber **keine Waffen** (dort gibt es keine `WeaponDef`) und kann **nicht angreifen** (Lauf). Kosmetisch fehlt das Sammelobjekt-Symbol unten rechts. Hinweistexte und Sprachausgabe bleiben die der Originalfigur.
* **Bewertung:** Bender ↔ Leela in Kapitel 1–3 und 5-1/5-2: **läuft / läuft vermutlich**. Fry in Bender-/Leela-Leveln: **eingeschränkt** (ohne Angriff). Blockiert sind 1-6 (Chickenwalker nur für Fry), 2-5 (Kanone nur für Bender), 4-1 (Ratte) und 5-3 (Railgun, Vermutung). Durchspielbarkeit (Rätsel, Sprunghöhen) ist für kein Level geprüft.
* **Levelauswahl ist klein und sicher:** Das Spiel hat eine eigene Tabelle mit 22 Startzuständen (`0x2D60BC`, inklusive passender Waffen für Fry). Ein Start in einem beliebigen Level heißt: Levelname an `FUN_000A2340` geben (statt `"Level1-1"` bei 0x200AE) und den Zustand mit `FUN_000C1D30` setzen. Genau das tut der Cheat-Handler `FUN_0002C530`. Im Port getestet: Cheatcode für 2-1 in Level 1-1 eingegeben → Level 2-1 lädt, Bender ist steuerbar.
* **Cheat-Flag 0x2EB7D4:** Es wird nur von `FUN_000D3120` (Zwischensequenz überspringbar) gelesen und nur vom Cheat `3853856711` umgeschaltet. Es gibt 29 Codes: 22 Levelsprünge, volle Gesundheit, Munition 999, +5 Leben, Power, Gott-Modus, Flag `0x2D9880` (Vermutung: Extras entsperrt) und das Zwischensequenz-Flag (2.4).
* **Empfehlung:** `[game] start_level` (klein, ½–1 Tag) und `[game] play_as` (mittel, 2–4 Tage plus Testmatrix) nur als Launcher-Optionen, `play_as` klar **experimentell** mit Sperrliste. Kein eigenes Menü im Spiel (groß). Stattdessen die eingebauten Cheatcodes dokumentieren.

---

## 2. Levelstruktur, Levelfolge, Startzustand, Spielstand, eingebaute Levelauswahl

### 2.1 Alle Level

Quellen: `CharacterDef::` in `defs.nif` und die `::PLAYER`-Instanz in den Stage-NIFs (`FutPAL00.img`), Folgelevel und Filme aus den Strings in `scripts\levelN-M.ful` (`FutPAL01.img`), Anzeigename und Startzustand aus der Cheat-Tabelle (`scripts\g4_tables.py`; die Namen bei 0x2D60B8 werden von keinem Code gelesen).

| Datei | Anzeige (Tabelle) | Spielfigur: Def → Modell (PLAYER-Instanz) | Folgelevel | Filme im Skript | Startzustand laut Tabelle 0x2D60BC |
|---|---|---|---|---|---|
| Level1-1 | 1-1 Planet Express | `fry-1-1` → fry-1-1.ucf (stage2 `fry-1-1\|002d`) | Level1-2 | 1-1-01, 1-1-02, 1-1-03 | Health 100, Ammo 100 |
| Level1-2 | 1-2 Sewers | `fry-1-2` (sewera) | Level1-3 | – | + Pickup Hammer, Equip Pistol |
| Level1-3 | 1-3 Subway | `fry-1-3` (subwaya) | Level1-4 | – | + Hammer, Pistol |
| Level1-4 | 1-4 Old New York | `fry-1-4` (onya) | Level1-5 | – | + Hammer, Pistol, Equip Shotgun |
| Level1-5 | 1-5 Red Light | `fry-1-5` (stage1) | **Level1-7** | 1-2-01 | + Hammer, Pistol, Shotgun |
| Level1-7 | 1-6 Uptown | `Fry` → fry.ucf (stage3 `Fry\|0014`) | **Level1-6** | 1-2-02, 1-2-03 | + Hammer, Pistol, Shotgun, Equip MachineGun |
| Level1-6 | 1-7 New New York | **keine Instanz** → Fallback `fry` (2.2/3.1), Fry steuert einen Chickenwalker | Level2-1 | 1-2-04, 1-2-05 | nur Health/Ammo |
| Level2-1 | 2-1 Canyon | `Bender` (cana) | **Level2-6** | 2-1-01, 2-1-02, 2-1-03 | Health, Ammo |
| Level2-6 | 2-2 Mine Tunnel | `Bender` (mine_tunnel) | Level2-2 | – | dto. |
| Level2-2 | 2-3 Mine | `Bender` (stage_mine) | Level2-3 | 2-2-01 | dto. |
| Level2-3 | 2-4 Junkyard 1 | `Bender` (shipyarda) | Level2-4 | – | dto. |
| Level2-4 | 2-5 Junkyard 2 | `Bender` (shipyarde) | Level2-5 | – | dto. |
| Level2-5 | 2-6 Junkyard 3 | `Bender` (shipyardj) + Kanone (3.3) | Level3-1 | 2-3-01..03, 3-1-01 | dto. |
| Level3-1 | 3-1 Courtyard I | `Leela` (courtyard_1) | Level3-2 | – | dto. |
| Level3-2 | 3-2 Left Wing | `Leela` (slwa) | Level3-3 | 3-2-01, 3-2-03, 3-4-01 | dto. |
| Level3-3 | 3-3 Right Wing | `Leela` (rightwinga) | **Level3-6** | 3-2-02, 3-4-02..05 | dto. |
| Level3-6 | 3-4 Courtyard II | `Leela` (courtyard_3) | Level3-4 | – | dto. |
| Level3-4 | 3-5 Inner Temple | `Leela` (inner_temple_1) | Level4-1 | 3-5-01, 3-6-01, 3-6-02, 4-1-01 | dto. |
| Level3-5 | – (kein Code) | `Leela` (courtyard_2) | Level3-3 | – | – (kein Vorgänger; Vermutung: geschnitten) |
| Level4-1 | 4-1 Bogad Swamp | `Rodent` → rodent.ucf (swamp0a) | Level5-1 | 4-1-02, 4-2-01 | dto. |
| Level5-1 | 5-1 Moms HQ – Bender | `Bender` (level5_bender) | Level5-2 | 5-1-01, 5-2-01 | dto. |
| Level5-2 | 5-2 Moms HQ – Leela | `Leela` (level5_leela) | Level5-3 | – | dto. |
| Level5-3 | 5-3 Moms HQ – Fry | `fry-5-3` (level5_fry), `WeaponDef::Railgun` | Level1-1 (nach credits) | 5-2-02, 5-3-01, credits | dto. |

Weitere Spielerfiguren gibt es nicht. Der Def-Lader kennt außerdem `Fry-1-6` und `Railgunfry`, aber kein Level benutzt sie. `chars\fry-1-6.ucf` (323 KB) und `chars\railgunfry.ucf` liegen trotzdem im Archiv. Alle Spieler-Modelle stehen in `FutPAL00.img` (immer gemountet): bender.ucf 1,79 MB, leela.ucf 1,55 MB, fry.ucf 3,01 MB, fry-1-1 1,31 MB, fry-1-5 2,06 MB, fry-5-3 1,85 MB, rodent.ucf 1,03 MB.

Waffen-Defs (`WeaponDef::`) stehen **nur** in den Fry-Leveln (1-1/1-2: Hammer, Pistol - Nachtrag 2026-10-13: zuerst als „HammerP“ gelesen, das P ist das Byte nach dem längenpräfixierten Namen; 1-3/1-4: + Shotgun; 1-5/1-6/1-7: + MachineGun; 5-3: Railgun). Bender- und Leela-Level haben nur Powerups (Bier/Öl bzw. Vitamine).

### 2.2 Wie das nächste Level gewählt wird

* **Skript-VM** `FUN_000B0C40` (aufgerufen aus `FUN_000B97D0`): Opcode **0xA1** = `FUN_000B0520(name)`. Die Funktion trägt die Levelzeit ein (`FUN_000F3AD0`/`FUN_000F3B00`, Bestzeit), setzt bei `[lvl+0x18C] == [lvl+0x190] > 0` `FUN_000F3AB0(level, 1)` (Vermutung: alle Sammelobjekte gefunden), baut über `FUN_000C26E0` den Zustand für das Folgelevel, setzt mit `FUN_000A2340(name)` den neuen Levelnamen (CApp+0xAC, 8 Zeichen, `_strncpy(…, 8)`) und öffnet den Level-Ende-Bildschirm (`DAT_002E8E4C` = `FUN_000A4DE0`, Menü `FUN_000A5940`: weiter oder `Frontend`).
* Opcode **0xA0** = `FUN_000C2290`: das aktuelle Level neu laden (`[0x2DABC4]+0xC`) mit `DAT_002EA999 = 1`. Der Destruktor `FUN_000C8360` stellt dann den Eintrittszustand aus der Sicherung `DAT_002EA990` wieder her (Checkpoint/Neustart).
* Opcode **0x6C** = `FUN_00036D20(name)`: wechselt die gesteuerte Figur zwischen vorhandenen Spielerobjekten (siehe 3.4).
* Die Level-Session (`nofunc_0002AAB0`, `m4_input.md` 4.4) lädt, sobald CApp+0xAC gesetzt ist: `FUN_0002A9F0` → `FUN_0002A760(CApp+0xAC)` → `FUN_000C22B0` (versteht `"Level=…\n…"` oder einen nackten Namen), danach `FUN_000A2340("")`.

### 2.3 Startzustand und Spielstand

**Zustandstext** (`DAT_002EA988`/Länge `DAT_002EA98C`; Sicherung für Neustart `DAT_002EA990`/`DAT_002EA994`):

* **Schreiben** `FUN_000C26E0` (Level-Objekt, optional Ausgabepuffer): `Level=%s\n` (CApp+0xAC, sonst eigener Name), `Health=%f\n` (Spieler +0x4F4), `Ammo=%d|%f\n` für 5 Slots (vf+0x94), `Power=%f\n` (vf+0x164; Wert +0xAF8, max. 4,0), je Inventar-Waffe `Pickup=%s\n` bzw. `Equip=%s\n` (ausgerüstet = Spieler +0x528), `Lives=%d\n` (`DAT_002D67DC`), `AgeSeconds=%f\n`, `AgeHours=%d\n` (Level +0x178/+0x17C).
* **Anwenden** `FUN_000C5340` (Level-Update, sobald der Spieler `[lvl+0x94]` existiert und `DAT_002EA988 ≠ 0`): zuerst `Lives = 5`. Dann pro Zeile: `Lives`, `Health` (setzt den Wert aus dem Def, `[[pl+0x10]+0x60]`), `Ammo` (ohne `|` gilt der Wert für alle 5 Slots), `Power` (vf+0x168), `Pickup`/`Equip` (Def über `FindDef` 0xC3E80 suchen, Waffe erzeugen und ins Inventar legen), `AgeSeconds`, `AgeHours`. Danach wird `DAT_002EA988` freigegeben.
* **Neues Spiel** setzt nur den Namen (`FUN_00020090` → `FUN_000A2340("Level1-1")`). Ohne Zustandstext gelten die Standardwerte: 5 Leben, Gesundheit aus dem Def, keine Waffen.
* **Spielstand** (`FUN_000DFE50`): Ein Slot-Objekt (0x2C Byte, `FUN_000E14D0`) erhält **genau diesen Text** (+0x10 Zeiger, +0x14 Länge) und zweimal den 8-Zeichen-Levelnamen ab Textoffset 6 (+0x18/+0x1C). „Spiel laden“ (`FUN_0001A940`, aus `FUN_0001FD10` und dem Pausemenü `FUN_000A74E0`) zieht mit `FUN_000C1D80` den Wert von `Level=` heraus, setzt ihn mit `FUN_000A2340` und den ganzen Text mit `FUN_000C1D30`. Ein Spielstand ist also „Level + Inventar am Levelanfang“. Fortschritt **innerhalb** eines Levels und Story-Flags werden nicht gespeichert. Die Bestzeiten und „alles gesammelt“ (`FUN_000F3AB0/3B00`) gehen in die Extras-Daten (Vermutung: `extras`-Speicherdatei, M7).
* In `FUN_000DFE50` steht ein Vergleich mit `"level0-1"` (Vermutung: Entwickler-Testlevel, nicht im Archiv).

### 2.4 Eingebaute Debug-Levelauswahl (Cheatcodes)

Der Code ist in der Retail-Version aktiv; es gibt dafür nur kein Menü.

* **Objekt** `[0x2E7B7C]` (ctor `FUN_0002C440`): Puffer 40 Ziffern, +0x2C Länge, +0x30 offener Kanal, +0x34 aktiv. `Session::Render` `FUN_0002A320` setzt +0x34 jedes Bild auf 1 und auf 0, solange Pause (`[+0x14]+0x24`) oder Level-Ende-Bildschirm (`DAT_002E8E4C`) offen sind. Vermutung: Im Frontend bleibt es aus, weil nur die Ingame-Session es setzt.
* **Eingabe** `FUN_0002C890` (pro Bild aus `Game::Frame`, 0xA2DD3): nur mit aktivem Controller und **gehaltenem LT** (Kanal 8). Eine Taste wird beim Drücken vorgemerkt und beim Loslassen als Ziffer übernommen (`FUN_0002C460`): **Y=1, B=2, A=3, X=4**, mit gehaltenem **RT**: Y=5, B=6, A=7, X=8, **WHITE=9, BLACK=0**, **BACK = Bestätigen** (`FUN_0002C840` → `FUN_0002C7D0`). Alle anderen Tasten löschen den Puffer.
* **Tabelle** 0x2D6038 (29 Codes, `scripts\g4_tables.py`), Ausführung `FUN_0002C530(i)`:
  * `38538585` + `11`…`53` (i = 0..21): Levelsprung. Die Funktion bricht ab, wenn CApp vf+0x1C TRUE liefert (Vermutung: Film/Ladevorgang). Sonst setzt sie den Namen aus dem Zustandstext (`FUN_000C1D80` → `FUN_000A2340`), den Text selbst (`FUN_000C1D30`) und wechselt aus `frontend` nach `Ingame` (im Retail-Spiel tot, siehe oben). Die Codes folgen der **Spielreihenfolge**, nicht den Dateinamen: `…16` = Level1-7, `…17` = Level1-6, `…22` = Level2-6 usw. Level 3-5 hat keinen Code.
  * `3853856701` Health = 100 · `…702` Munition 999 in 5 Slots (`FUN_00051D50`) · `…703` Leben +5 (`FUN_000C1220/1230`, `DAT_002D67DC`) · `…704` Power +10000 (vf+0x15C `0x50420`, gekappt) · `…705` Gott-Modus an/aus (vf+0x174 `0x505D0`: Health 55555,0 ↔ 100) und `DAT_002D9880` umschalten · `…706` `DAT_002D9880` umschalten (einziger Leser `FUN_0001B630`, meldet dann „nicht gesperrt“; Vermutung: Extras entsperren) · `…711` **0x2EB7D4** umschalten. Danach ruft der Handler `FUN_00013130` mit einer Farbe auf (Vermutung: kurzer Bildschirmblitz als Bestätigung).
* **0x2EB7D4:** Rohe Referenzsuche im XBE: nur 0x2C760/0x2C76B (Cheat `…711`, schreibt) und 0xD3121 (`FUN_000D3120`, liest). Andere Leser gibt es nicht; die Beschreibung in `src\host\core\guest.h` (`kGameCheatSkipCutscenesVar`) stimmt.
* **Im Port geprüft** (Lauf „cheat“, 8): In Level 1-1 eingegeben – LT halten, `A RT+X RT+Y A RT+X RT+Y RT+X RT+Y B Y`, BACK (`3853858521`) – lädt das Spiel sofort Level 2-1 (`game: level loading` bei 26,6 s, `level loaded` bei 34,8 s). Bender ist steuerbar (`run\g4_select\cheat\shots\frame_002331.png`). Für Spieler am PC heißt das: Mit einem Controller oder der Standardbelegung des Ports (LT/RT/BACK auf Tastatur, M4) ist eine Levelauswahl schon heute möglich.

---

## 3. Wie die Spielfigur entsteht

### 3.1 Daten

* `defs.nif` jedes Levels: NiNode `CharacterDef::<name>` (dazu `WeaponDef::`, `PickupDef::`, `PowerupDef::` usw.). Aus dem Namen baut der Basis-ctor `FUN_000409C0` den Modellpfad **`Chars\<name>.ucf`** (Def +0x108). Das Laden startet vf[1] `FUN_0003FE00` asynchron über `FUN_000D20D0`.
* Stage-NIF, Knoten `Instances`: Jeder `NiUDSNode` heißt `<Def>|<idx %04x>::<Instanzname>`. `FUN_000CEDC0` zerlegt den Namen und sucht den Def mit **`FindDef` `FUN_000C3E80`** (Liste `[lvl+0x34]`, `__stricmp` auf Def +0x10). Fehlt der Def, wird die Instanz still übersprungen. Die Spielerinstanz heißt immer `PLAYER`.
* **Fallback** in `Stage::Load` `FUN_000D1380` (Ende): Gibt es einen Knoten `Start` und noch keinen Spieler (`[lvl+0x94] == 0`), sucht der Code mit **`FindCharDef` `FUN_000C3F20`** (Liste `[lvl+0x3C]`, Glied +0xFC) nach `"fry"` (0x29A664). Fehlt er, legt er `new PlayerDef("fry")` an und ruft `Spawn(level, pos, rot, "PLAYER")`. So entsteht Fry in Level1-6, das keine Spielerinstanz hat.
* Die Levelskripte sprechen die Figur fast nur als `PLAYER` an (String-Vorkommen in der `.ful`: 1-1 228, 2-1 414). Figurnamen kommen nur in Objektnamen vor (Spinde `Fry_Locker`, `nLockBender`), in Dialog-IDs (`FRY_0064`…), in Sounds und in 5-3 als `DESTRUCTOR::RunBender()/RunLeela()/RunFry1()`. Die Dialog-NIFs `ingame_dlg_<sprache>.nif` gehören zum Level (z. B. 1-x: `Fry.ldb`, `Chars\FRY\…`).

### 3.2 Def-Fabrik und Klassen

`FUN_000C5B40` (Lader für `defs.nif`, Zweig `CharacterDef::`): `Fry`, `Fry-1-1`…`Fry-1-6`, `Fry-5-3`, `Railgunfry`, `Leela`, `Bender`, `Rodent` → **`PlayerDef`** (`FUN_0005E580`, 0x144 Byte, vtables 0x29A67C/0x29A674/0x29A670). `kamikaze` → `FUN_0005F5A0`. Alle anderen Namen → Gegner-Def `FUN_0004B5B0`.

`PlayerDef::Spawn` = vf[5] (+0x14) `FUN_0005E620`. Es wählt **nach dem Def-Namen** (`this+0x10`):

| Def-Name | Klasse (ctor) | Größe | vtable | Typ `+0x774` | Sounds (Auswahl) | HUD `FUN_000E5AD0` | Animations-Root | Zusatzobjekte im Basis-ctor `FUN_0005D660` |
|---|---|---|---|---|---|---|---|---|
| `fry`, `railgunfry`, `fry-1-1`…`fry-1-6`, `fry-5-3` | Fry `FUN_000831F0` | 0xD70 | 0x29E1E8 | 1 | Hammer-/Treffer-Sounds, `Sounds/Level1/Fry/*`, Pickups (Ammo, Waffe, Geld, Slurm) | weapons 1, charge 0 | `Fry_ROOTBONE` | nur in `level1-6`: Chickenwalker-Steuerung `FUN_00043D80` (Raketen/Laser, `Sounds/Level1/Walker/*`) → +0x7B0 |
| `leela` | Leela `FUN_00089670` | 0xD60 | 0x29F0C0 | 2 | `Sounds/Level3/LEELA/*` (Tritte, Schläge, Rolle, Rückwärtssalto) | weapons 0, charge 1 | `Leela_BONEROOT` | `FUN_00045F60(1)` → +0x7B4 (mit `NiCamera`; Vermutung: Aufladeangriff) |
| `bender` | Bender `FUN_00080440` | 0xD60 | 0x29D8E0 | 3 | `Sounds/Level2/Bender/*` (Flamme, Drehangriff, Hintern-Stampfer) | weapons 0, charge 1 | `B_ROOTBONE` | `FUN_00045F60(0)` → +0x7B4; nur in `level2-5`: **Kanone** `FUN_000334D0` (`Cannon_Laser`, `Cannon_ShieldDeactivator`) → +0x7A8 |
| `rodent` | Rodent `FUN_00094380` | 0xD60 | 0x2A0DB0 | 4 | `Sounds/Level4/ZoidbergOnRat/*` | weapons 0, charge 0, **timer 1** | `rootbone_zoidberg` | Ratten-Steuerung `FUN_000611B0` → +0x7AC, `FUN_00062E50` |
| sonst | Basis `FUN_0005D660` | 0xD60 | 0x29A1E8 | 0 | – | weapons 1 | – | – |

* **Ein gemeinsamer Basistyp, vier Unterklassen.** Alle vtables teilen die Basis-Slots (z. B. +0x94 Munition, +0x15C/+0x164/+0x168 Power, +0x174 Gott-Modus, +0xB4). Die Unterklassen überschreiben Angriffe und Bewegung (nicht im Einzelnen untersucht).
* Der Basis-ctor hängt jeden Spieler in die Liste `[lvl+0x98]` (Glied +0xC4C). Der **erste** wird `[lvl+0x94]` = gesteuerte Figur (+0x630 = steuerbar, `m4_input.md`).
* Das HUD-Porträt folgt der Figur von selbst (Lauf: Bender-, Leela- und Fry-Kopf passend). Das Sammelobjekt-Symbol unten rechts (FryCash/Edelsteine/Goldbarren) fehlt bei fremder Figur (alle drei Läufe in 4.2; Vermutung: Symbol je Figur, nur das des Levels ist geladen).

### 3.3 Figurwechsel im Level

Skript-Opcode 0x6C → `FUN_00036D20(name)`: sucht über `FUN_000C26A0` ein Spielerobjekt mit Namen `name` (+0x34) und setzt `[lvl+0x94]` darauf. Die alte Figur wird nach ihrem Typ als `FRY`/`LEELA`/`BENDER` registriert, die neue als `PLAYER`, +0x630 wird umgesetzt. Das ist ein echter Figurwechsel innerhalb eines Levels. Er geht aber nur zwischen Figuren, die die Leveldaten anlegen (Vermutung: Kapitel 5; nicht weiter geprüft).

---

## 4. Figuren-Override

### 4.1 Ansatz

Den Def-Namen der Spielerfigur beim Laden umbenennen. Alles Weitere (Modell, Klasse, Sounds, HUD) folgt von selbst aus dem Namen:

1. **PlayerDef-ctor `FUN_0005E580(this, name)`** (thiscall; Prolog `8B 44 24 04 56` = `mov eax,[esp+4]; push esi`, 5 ganze Befehle, kein Sprungziel innen): `name ∈ Spielernamen` → Zielname. Dadurch heißt der Def z. B. `bender` und lädt `Chars\bender.ucf`.
2. **`FindDef` `FUN_000C3E80`** und **`FindCharDef` `FUN_000C3F20`** (thiscall; Prolog `53 8B 5C 24 08`): Lookups des alten Namens auf den Zielnamen umbiegen. Dann findet die Instanz `fry-1-1|002d::PLAYER` den Bender-Def, und der Fallback `"fry"` in 1-6 ebenso.

Der Zustand und die Skripte bleiben unberührt (sie kennen nur `PLAYER`). Das Original-Def wird gar nicht erst angelegt, das Modell der Originalfigur also nicht geladen. Der Speicher ändert sich nur um die Differenz der `.ucf`-Größen (≤ 1,7 MB, Fry → fry.ucf) plus den Soundsatz der neuen Figur.

### 4.2 Experiment (Speicherpatch von außen, nicht im Repo)

`scripts\g4_char_probe.py <exe> <ini> <von> <nach>` startet den Port und wartet, bis die drei Prologe im (identisch abgebildeten) Gastspeicher stehen (nach 0,05 s). Dann legt es mit `VirtualAllocEx` drei Code-Caves an (Vergleich mit `__stricmp` 0x168441, ggf. Name ersetzen, gestohlene 5 Byte, Rücksprung) und schreibt die drei `jmp`s. Danach liest es `[[0x2DABC4]+0x94]` (vtable, Typ +0x774, Health +0x4F4) mit.

| Lauf | Patch | Ergebnis (Protokoll des Probes) | Bild |
|---|---|---|---|
| Bender in 1-1 | `fry-1-1 → bender`, Skript „Neues Spiel“ | t=17,8 s `('Level1-1', vtable 0x29d8e0, Typ 3, Health 100.0)`. Laufen, Springen, Angriffsanimationen auf X/A (Bild `frame_001753.png`), Bender-Porträt, Ende `expect controllable` ok, Rückgabe 0 | `run\g4_select\char_bender_shots\frame_001493.png`, `…001965.png` |
| Leela in 1-1 | `fry-1-1 → leela` | `('Level1-1', 0x29f0c0, 2, 100.0)`, steuerbar, Leela-Porträt, Rückgabe 0 | `run\g4_select\char_leela_shots\frame_001668.png` |
| Fry in 2-1 | `bender → fry` + Cheatcode für 2-1 | 1-1 normal Fry (`fry-1-1` unberührt), nach dem Code `('Level2-1', 0x29e1e8, 1, 100.0)`. Fry lädt (fry.ucf), läuft und springt, **A/X/LT+A ohne Angriff** (keine Waffe), Bender-Hinweis „X für Wirbelangriff“ bleibt | `run\g4_select\char_fry21_shots\frame_002345.png`, `char_fry21_attack_shots\frame_002522.png` |

In keinem Lauf gab es eine Fehler- oder Absturzmeldung im Protokoll (`game summary` normal, Rückgabe 0). Nicht geprüft: Tod und Neustart der fremden Figur, Levelende, längeres Spielen.

### 4.3 Was bricht oder brechen kann

* **Fähigkeiten/Leveldesign:** Fry kämpft nur mit Waffen. Außerhalb von Kapitel 1 und 5-3 gibt es keine `WeaponDef`, also hat Fry keinen Angriff (Lauf). Waffen-Defs aus anderen Leveln nachzuladen hieße fremde `defs.nif` teilweise zu laden (groß, nicht untersucht). Bender und Leela sind Nahkämpfer; ob sie die Rätsel der Fry-Level lösen (Schalter beschießen, Waffen-Pickups, Sprunghöhen) und umgekehrt, ist **offen**.
* **Level-gebundene Zusatzobjekte** (3.2): Level1-6 braucht den Chickenwalker (nur Typ 1), Level2-5 die Kanone (nur Typ 3, Bosskampf gegen den Panzer `tank`). Level4-1 ist ein Ritt auf der Ratte (Typ 4 + `timer`). Mit anderer Figur fehlen diese Objekte → **blockiert** (Vermutung, aber aus dem Code klar).
* **5-3:** `WeaponDef::Railgun` und `fry-5-3` (Railgun-Animationen), dazu Skriptsequenzen `DESTRUCTOR::RunFry1/2`. Vermutung: nur mit Fry lösbar.
* **Zwischensequenzen/Filme:** Die XMV-Filme zeigen die Originalfigur. In-Engine-Sequenzen arbeiten mit `PLAYER` (Vermutung: dann mit der neuen Figur, Animationsnamen könnten fehlen).
* **Sprache/Hinweise:** Dialoge (`ingame_dlg_*.nif`, `Fry.ldb`) und Hinweistexte gehören zum Level und bleiben die der Originalfigur (Lauf 2-1).
* **HUD:** Das Sammelobjekt-Symbol fehlt (kosmetisch). Porträt und die HUD-Schalter `weapons`/`charge` folgen der Figur (Code 3.2, Lauf).
* **Zustand:** In Fry-Leveln trägt der Zustandstext `Pickup=/Equip=`-Waffen. `FUN_000C5340` würde sie auch Bender oder Leela ins Inventar legen (nicht geprüft, Risiko: Waffenanimationen fehlen). Abhilfe: Bei aktivem Override diese Zeilen aus dem Text entfernen, ehe er angewendet wird.
* **Tod/Neustart:** Todeseffekte wie `effects\deaths\bender_*.nif` liegen in `FutPAL01` (immer gemountet). Ob die Figurenklasse beim Tod levelspezifische Effekte erwartet, ist nicht geprüft (Risiko Absturz, Vermutung). Neustart über 0xA0 lädt das Level neu; der namensbasierte Override greift dabei wieder.
* **Fortschritt:** Levelende mit fremder Figur trägt Bestzeit und „alles gesammelt“ normal ein (Extras). Die Figur selbst wird nirgends gespeichert.

### 4.4 Bewertung je Level und Figur

„läuft“ = im Lauf gestartet und gespielt; „vermutlich“ = Code und Daten sprechen dafür, nicht gestartet; „eingeschränkt“ = startet, aber zentrale Fähigkeit fehlt; „blockiert“ = level- oder figurgebundenes Objekt fehlt.

| Level | Original | als Fry | als Bender | als Leela |
|---|---|---|---|---|
| 1-1 | Fry | – | **läuft** (Start, Laufen, Angriff; Durchspielen ungeprüft) | **läuft** (dto.) |
| 1-2, 1-3, 1-4, 1-5, 1-7 | Fry | – | vermutlich (Waffenrätsel offen) | vermutlich (dto.) |
| 1-6 | Fry + Walker | – | blockiert (kein Chickenwalker) | blockiert |
| 2-1 | Bender | **eingeschränkt** (Lauf: startet, kein Angriff) | – | vermutlich |
| 2-2, 2-3, 2-4, 2-6 | Bender | eingeschränkt (keine Waffen-Defs) | – | vermutlich |
| 2-5 | Bender + Kanone | blockiert | – | blockiert |
| 3-1 … 3-6 (inkl. 3-5) | Leela | eingeschränkt | vermutlich | – |
| 4-1 | Rodent | blockiert | blockiert | blockiert |
| 5-1 | Bender | eingeschränkt | – | vermutlich |
| 5-2 | Leela | eingeschränkt | vermutlich | – |
| 5-3 | Fry + Railgun | – | blockiert (Vermutung) | blockiert (Vermutung) |
| Rodent woanders | – | – | – | blockiert (Ratten-Steuerung braucht die 4-1-Strecke, Vermutung) |

---

## 5. Levelauswahl

### 5.1 Was ein Levelstart braucht

1. Den Kontext `Ingame` (`FUN_000A30D0("Ingame")`, 0x2940CC) – beim „Neuen Spiel“ ohnehin.
2. Den Levelnamen (8 Zeichen) über `FUN_000A2340` (thiscall auf `[0x2E7B84]`).
3. Optional den Zustandstext über `FUN_000C1D30(text, len)`. Ohne ihn starten alle Level mit den Standardwerten (5 Leben, Gesundheit aus dem Def, keine Waffen). Für 1-2 … 1-7 braucht Fry die Waffen aus der Tabelle 0x2D60BC. Diese Tabelle ist die Vorgabe der Entwickler und kann direkt aus dem XBE gelesen werden.

Mehr braucht es nicht: Es gibt keine levelübergreifenden Story-Flags, und jedes Levelskript startet seine eigenen Filme und Sequenzen (Kontext `Intermission` = Zwischensequenz-Manager `FUN_000D4DC0` → `FUN_000D37C0`). Ein Direktstart in 2-1 spielt also 2-1-01 usw. wie beim normalen Durchspielen. Die M6-Regeln gelten unverändert: `[game] skip_videos`; die Sperre beim ersten Ansehen betrifft nur 1-1-01/1-1-02, Tabelle 0x2D6D10.

### 5.2 Einstiegspunkte

* **`FUN_00020090`** (Countdown nach „Neues Spiel“), Stelle **0x200AE `push 0x2940C0`** (`"Level1-1"`). Der String wird außerdem bei 0x20CF1 (`FUN_00020B60`, Flag +0x30; Vermutung: Rückweg aus Extras/Attract) und im CApp-ctor 0xA28C6 (Vorgabe von CApp+0xAC) benutzt. Deshalb **nicht** den String patchen, sondern den Aufruf oder das `push`.
* Variante A (empfohlen): Probe-Hook auf `FUN_000A2340`. Wenn die Rücksprungadresse 0x200B8 ist, wird der Name ersetzt und vorher `FUN_000C1D30(state, strlen+1)` aufgerufen. Das ist derselbe Ablauf wie in `FUN_0002C530`. Alternativ ein Probe auf `FUN_00020090` (Prolog `56 8B F1 FF 4E 48`).
* Variante B (Laufzeit, für einen späteren Menüeintrag oder eine Konsole): direkt `FUN_0002C530(index)` rufen, solange Ingame aktiv ist. Das ist die geprüfte Originalfunktion (2.4).
* Gültige Namen: die 23 Ordnernamen. `Level3-5` startet nur so (experimentell, nie geprüft).

### 5.3 Spielstände (M7), Extras, Fortschritt

* Mit `start_level` läuft danach die normale Folge. Am Levelende entsteht der normale Zustand für das Folgelevel; ein Spielstand ist dann ganz normal `Level=<Folgelevel>…`. M7 muss dafür nichts Besonderes tun.
* Das Hauptmenü fragt beim Start die Speicherplatzprüfung ab (`m4_input.md` 4.3). Das bleibt so.
* Bestzeiten und „alles gesammelt“ landen in den Extras-Daten (2.2). Ein Direktstart verfälscht sie nicht, schaltet aber Extras frei, ohne die Level davor gespielt zu haben. Akzeptabel, aber dokumentieren.
* Ein geladener Spielstand überschreibt `start_level` nicht. Die Option wirkt nur auf „Neues Spiel“.

---

## 6. Empfehlung, Aufwand, Risiken

**A. `[game] start_level`** (Launcher: Auswahlliste „Startlevel“ mit den 22 Anzeigenamen plus „Level 3-5 (ungenutzt)“; Standard `Level1-1`):

* Umsetzung: Probe-Hook `FUN_000A2340` (bzw. `FUN_00020090`), Name und Zustand aus der Tabelle 0x2D60BC im XBE lesen (kein eigener Datenstand). Neuer Eintrag in `config_schema.cpp`, `guest.h`-Konstanten (0x200B8, 0x2D60BC, 0xC1D30, 0xA2340), Logzeile `game: start level …`, ein Eingabeskript `m7_*`/`g4_start_level.txt` mit `wait level_loaded` + `controllable` und Prüfung von `[lvl+0xC]`.
* Aufwand: **klein** (½–1 Tag inklusive Launcher). Risiko gering, weil das der Originalpfad der Cheat-Funktion ist (im Port geprüft). Experimentell markieren nur für `Level3-5`.
* Dazu die Cheatcodes in der Doku nennen (Tabelle aus `scripts\g4_tables.py`). Sie funktionieren ohne Codeänderung.

**B. `[game] play_as = original|fry|bender|leela`** (Launcher-Gruppe „Experimentell“):

* Umsetzung: drei Probe-Hooks (`0x5E580`, `0xC3E80`, `0xC3F20`). Zuordnung `{fry, railgunfry, fry-1-1…fry-1-6, fry-5-3, bender, leela} → fry|bender|leela`; Ziel-Def für Fry ist `fry` (volles Modell). Sperrliste nach Levelname `[[0x2DABC4]+0xC]` zum ctor-Zeitpunkt: level1-6, level2-5, level4-1, level5-3 (dort Original behalten und im Log melden). Bei aktivem Override `Pickup=/Equip=`-Zeilen aus `DAT_002EA988` entfernen (Hook vor `FUN_000C5340` oder in `FUN_000C1D30`). Logzeile `game: player def X -> Y`. Die Lookups lassen sich auch als reine Ersatzfunktionen in C++ schreiben (einfache Listen-Schleifen).
* Aufwand: **mittel**. Hooks und Option etwa 1 Tag. Dazu eine Testmatrix (22 Level × 2 Fremdfiguren, je ein kurzer Skriptlauf mit Start, Angriff, Tod und Neustart) von 1–2 Tagen. Durchspielbarkeit zu prüfen ist offen und eher groß.
* Risiken: Softlocks in Rätseln/Sprungpassagen, Abstürze beim Tod oder in Sequenzen mit Figur-Animationen (ungeprüft), Fry ohne Angriff außerhalb seiner Level, kosmetische HUD-Lücke, falsche Stimme und Hinweise. Alles klar als **experimentell** kennzeichnen. Der Standard `original` muss bitgleich bleiben (Hooks nur installieren, wenn die Option gesetzt ist).
* Optionaler Ausbau (groß, nicht empfohlen): Fry in Fremdleveln Waffen geben (WeaponDefs samt Meshes aus einem Fry-Level nachladen).

**C. Eigenes Menü im Spiel:** **groß**. Das Frontend ist NIF- und textbasiert (Menüs 0x1F000–0x21000, Sprachtabellen), ein neuer Eintrag braucht eigene Menüobjekte und Texte. Nicht empfohlen. Der Launcher reicht, die Cheatcodes decken den Ingame-Fall ab.

---

## 7. Offen / Vermutungen

* Durchspielbarkeit jeder Kombination aus 4.4 (nur Start und erste Sekunden geprüft).
* Tod, Neustart (0xA0) und Levelende mit fremder Figur; Verhalten von Waffen-Pickups bei Bender/Leela.
* `FUN_00045F60(0/1)` (Bender/Leela, mit `NiCamera`): genaue Rolle (Vermutung: Aufladeangriff/Kamera).
* Woher das Sammelobjekt-Symbol des HUD kommt (Vermutung: je Figur geladen).
* CApp vf+0x1C als Sperre der Cheat-Levelsprünge (Vermutung: Film/Ladevorgang aktiv).
* `DAT_002D9880` = Extras entsperrt (Vermutung aus `FUN_0001B630`).
* `Level3-5` und `fry-1-6.ucf`/`railgunfry.ucf`: ungenutzte Inhalte (Vermutung).
* Wann das Spiel den Spielstand schreibt (Level-Ende-Dialog?), gehört zu M7.

## 8. Skripte und Läufe

* `analysis\loader\scripts\g4_tables.py`: Cheatcodes 0x2D6038, Tastenfolgen, Startzustände 0x2D60BC, Anzeigenamen 0x2D60B8.
* `analysis\loader\scripts\g4_char_probe.py`: Figuren-Override per Speicherpatch (Code-Caves für 0x5E580/0xC3E80/0xC3F20) und Mitschrift des Spielerobjekts.
* Läufe (`build\g4-select\bin\futurama.exe` RelWithDebInfo, `[debug] movies = skip`, Eingabeskripte und INIs unter `run\g4_select\`, git-ignoriert):
  * `cheat` – `cheat_level.txt`: Neues Spiel → 1-1 → Cheatcode 2-1 → Bender in 2-1 (`cheat\shots\frame_002331.png`, `frame_002453.png`).
  * `char` + Probe `fry-1-1 bender` – `char_play.txt` → `char_bender_shots\`.
  * `char` + Probe `fry-1-1 leela` → `char_leela_shots\`.
  * `char` + Probe `bender fry` – `cheat_level.txt` bzw. `cheat_attack.txt` → `char_fry21_shots\`, `char_fry21_attack_shots\`.
* Datenauszüge (nur zur Analyse, git-ignoriert): `run\g4_select\data\FutPAL01\scripts\*.ful|*.dbg`. Die `defs.nif`/Stage-NIFs wurden nach der Auswertung wieder gelöscht und lassen sich mit `tools\xdvdfs.py` + `tools\futimg.py` (`levels/`) neu ziehen.

## 9. Umsetzung (2026-10-12)

Umgesetzt wie in Abschnitt 6 empfohlen: `[game] start_level` und `[game] play_as` (`src\host\xapi\game_select.*`, bedingte Probes `FUT_PROBE_IF`). Bericht, Prüfläufe und Spieler-Checkliste: [`docs\g4_report.md`](../../docs/g4_report.md).
