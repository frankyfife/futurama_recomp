# G8: Cheats – Schaden, Leben, Gegner-KI

Stand: 2026-10-13. Basis: Retail-EU-XBE (`orig\retail_eu\default.xbe`, Capstone), Ghidra-Export `analysis\retail_eu_export`, Levelskripte `scripts\levelN-M.ful/.dbg` (FutPAL01, nur gelesen) und `chars\*.ucf` (FutPAL00, Eigenschaft `maxhealth`), dazu Läufe des Ports unter `run\cheats\` des Arbeitsbaums (git-ignoriert). Umsetzung: `src\host\xapi\cheats.*`, `src\host\input\cheat_toggles.*`; Bericht [`docs\g8_cheats_report.md`](../../docs/g8_cheats_report.md).

Adressen sind VAs der Retail-EU-XBE. „Vermutung“ kennzeichnet Deutungen ohne direkten Beleg.

## 1. Kurzfassung

* Jede Figur (Spieler und Gegner) nimmt Schaden über ihren vtable-Slot **+0x7C** `TakeDamage(float amount, Actor* attacker)` (thiscall, `ret 8`). Am Ende steht immer die Basis **`Actor::TakeDamage` `FUN_000354B0`**: Gesundheit `+0x4F4 -= amount`, tot bei ≤ 0. Das Spiel hat dort schon einen Gott-Modus: Ist die Gesundheit genau **55555,0** (0x47590300), wird jeder Betrag außer 55555 ignoriert.
* **Spieler:** Alle fünf Spielerklassen teilen `FUN_00051AD0` (Treffer-Reaktion, Rumble `FUN_0005EF70`, dann `0x354B0`). Zwei globale Sperren stehen davor: `0x2DD5F4` (gesetzt, solange die Figur keine Eingaben annimmt, `FUN_00037010`) und `0x2DD5F5` (nie geschrieben – Entwickler-Schalter ohne Code).
* **Leben:** nur das Skript-Native **FsDeductLives** (Opcode 0x149 → `FUN_000C1240`: `dec [0x2D67DC]`, bei < 0 Kontext „Death“). Die Levelskripte erkennen den Tod selbst (FsCharIsDead = Opcode 0x9D → vf+0xA8 = Gesundheit ≤ 0), ziehen ein Leben ab und setzen die Figur an den Kontrollpunkt (gemeinsame Skriptroutine, Zeile 233 in allen 23 Skripten).
* **Todeszonen:** `FUN_00038880` (Material-Kollision je Tick) – Materialflag 4 = Schaden alle 2 s (Aufruf 0x38ACE), Flag 8 = sofortiger Tod: `vf+0x7C(health)` (Spielerzweig 0x38C46, Rückkehr **0x38C49**), Todesart +0x778 (Lava, Wasser …).
* **Gegner-KI:** Jeder Gegner hat ein „Gehirn“ (`+0x58`, Update **`FUN_0002ECB0`**). Das Gehirn der Spielfigur schreibt jeden Tick **`0x2DBAA0` = (Spieler +0x630 == 0)** – „Feuer einstellen“, solange die Figur nicht steuerbar ist (Zwischensequenzen). Die Wahrnehmung `FUN_0002E2E0` liefert dann kein Ziel (Lesestelle 0x2E300), der Angriffszustand `FUN_0002F720` greift keinen Spieler an (0x2FA84).

## 2. Spielerschaden

| Stelle | Inhalt |
|---|---|
| `FUN_00051AD0` (vtables 0x29A1E8, 0x29E1E8 Fry, 0x29D8E0 Bender, 0x29F0C0 Leela, 0x2A0DB0 Rodent, Slot +0x7C) | Prolog `55 8B EC 83 E4 F0`. Sperren `0x2DD5F4`/`0x2DD5F5`, Bender-/Leela-Abwehr (Animationszustände 0xD2/0xBC/0xBB/0xBD bzw. 0xB5), Chickenwalker (`+0x7B0`): `FUN_000436C0` (nur Effekte) und danach doch `0x354B0` auf dem Spieler. Danach Rumble (`> 10`), Tod (+0x2FD Zähler, +0x778 = 0), Blickrichtung zum Angreifer |
| `FUN_000354B0` | Prolog `64 A1 00 00 00 00` (SEH). Gott-Modus-Abfrage, `FUN_00033EE0(attacker)` (Angreifer lebt noch?), Abzug, Tod: Ereignisse an Skripte/Statistik, Flag `| 0x10` (`FUN_000C19B0`) |
| `FUN_000505D0` (vf+0x174) | Cheat 3853856705 „Gott-Modus“: Gesundheit 55555 ↔ 100 |
| Aufrufer von Schaden am Spieler | Gegnerangriffe (`0x46CC9` u. a.), Geschosse (`FUN_00077E50`: Schütze lebt → `Schütze->vf+0xCC(Ziel, Betrag)` = `FUN_000503E0`, sonst Angreifer 0), Material 0x38ACE/0x38C46, Skript FsDamageCharacter (Opcode 0x70 → `FUN_00036F90`, Angreifer 0) |

**Skriptschaden am Spieler** (Scan der `.ful`, Opcode 0x13 Klasse 0 Id 0x70 mit vorangehendem Push): 1-1 `Damage(PLAYER, 10)`, 1-4 `(PLAYER, 100)` und ein variabler Betrag, 1-5/1-7 `(PLAYER, 100)` nach drei Sekunden gesperrter Eingabe (geskripteter Tod), 2-1 und 2-3 variabel, 2-6 `(PLAYER, 1e6)` („MINETUNNEL:: rolling stone killed player“), 3-4 `(PLAYER, 30)`. Der Gott-Modus des Spiels blockt all diese Beträge (≠ 55555).

**Entscheidung „Unverwundbar“:** wie der eingebaute Gott-Modus, aber ohne die Gesundheit zu verändern (HUD und Spielstand bleiben normal):

* `probe:Player::TakeDamage` überspringt jeden Aufruf mit Betrag > 0 – außer von der Todeszone (Rückkehr 0x38C49), die wie im Gott-Modus tötet (dort ist der Betrag = aktuelle Gesundheit, also 55555 im Gott-Modus).
* `probe:Script::FsDeductLives` überspringt den Abzug. Das Skript setzt die Figur danach an den Kontrollpunkt (Gesundheit 100).
* Begründung für die Todeszonen: Abgründe haben einen tödlichen Boden; überlebte die Figur dort, säße sie fest. Die Entwickler haben dasselbe gewählt.
* `0x2DD5F5 = 1` (Entwickler-Schalter) wäre ohne Hook gegangen, hätte aber auch die Todeszonen abgeschaltet und nicht protokolliert.

## 3. Gegnerschaden und Klassen

vtables mit dem Actor-Layout (Slot +0x1C = 0x353B0, +0x20 = 0x35400; Slot +0x7C = TakeDamage), Konstruktoren über die Gegner-Fabrik (Name → ctor, Aufrufe in 0x4A400–0x4B400) und `maxhealth` aus `chars\<name>.ucf`:

| +0x7C | vtables (Klasse) | Aufbau |
|---|---|---|
| `0x354B0` | 0x295D38 (Actor-Basis) | – |
| `0x47240` | 0x297468 NPC-Basis und die meisten Gegner (skel_hth, high_priest, igner_boss, gunturret, toxicwarrior, …); Thunks 0x81AC0, 0x86D50, 0x91E90 (`jmp 0x47240`), 0x8F8B0, 0x8EE30, 0x93CB0 (rufen 0x47240) | Vorprüfung „tödlicher Treffer“ (`amount ≥ health` und Def +0x183 → `FUN_000C2EE0`, Ereignis an `[Level+0x44]`, Vermutung: Beute/Statistik), dann 0x354B0, dann Treffer- bzw. Todeseffekte |
| `0x60080` | 0x29B7E8 kamikaze | Vorprüfung `FUN_000C2EE0` |
| `0x80300` | 0x29D7B0 aquafish | Vorprüfung `FUN_000C2E90` |
| `0x86520` | 0x29E3D0/0x29E510/0x29E640/0x29E790 goonbox, goonskinny, goonsneak | Vorprüfung `FUN_000C2D50` + Effekt |
| `0x88530` | 0x29ECF8/0x29EE38 junkbot | Vorprüfung `FUN_000C2EE0` |
| `0x90570` | 0x2A00B0 stormtrooper_mk2, 0x2A01F0 stormtrooper | Vorprüfung `FUN_000C2D50` |
| `0x8DEB0`, `0x8E710`, `0x92A30` | skel_ranged, warthog | rufen 0x354B0 ohne Vorprüfung |
| `0x82500`, `0x89060`, `0x8B410` | destructor, Liquid_LavaMan, crocodile | eigene Logik (Animationsfenster, Weitergabe), 0x82500 und 0x89060 rufen später 0x354B0 (0x82622, 0x891A5) |

`maxhealth` (Auszug): nibbler 1, weasel 20, aquafish 25, skel_hth/skel_ranged 40, eyestalk/warthog 50, toxicwarrior/kamikaze/patrol_guard/spinbot/lavaman 100, junkbot 125, stormtrooper_mk2 150, stormtrooper/rockmonster/turdman 200, goonsneak 250, goonskinny/high_priest 300, goonbox 450, **tank 1000, chickenwalker 2000, destructor 10000**, tentacle_dude 55000, **55555** (vom Spiel unverwundbar): carnivorous_plant, cactusmonster, crocodile, igner_boss, owl, statuehead, sungod, flowerplant, giantbird, alien_pod.

**Skripte und Gesundheit:** FsGetHealthCharacter (Opcode 0x6E) fragen die Skripte nur für PLAYER, `St1Laundry1` (1-7, „wurde getroffen?“), drei Skelette in 3-2 (Gesundheit sichern/wiederherstellen) und in Kapitel 5 `DESTRUCTOR::RunBenderFloor` (wartet auf eine Änderung, dann auf ≤ 0) ab. Kampfphasen über Gesundheitsschwellen gibt es nicht; die Skripte warten auf den Tod („WAITING FOR DEATH: …“) oder auf einen Treffer. FsSetHealthCharacter(…, 55555) („SetBigHealth“, Zeile 216 aller Skripte) macht Figuren für Skriptphasen unverwundbar.

**Entscheidung „Ein Treffer“** (`xapi::cheats::OneHitDamage`): nur Treffer, deren Angreifer eine Spielerklasse ist (Geschosse übergeben den Schützen, `FUN_00077E50` → `FUN_000503E0`), an Nicht-Spielern: Betrag = aktuelle Gesundheit. Ausgenommen: Gesundheit oder Maximum 55555 und Maximum ≥ 50000 (unverändert, wie vom Spiel/Skript gewollt). Bosse (Maximum ≥ 1000) bekommen ein Viertel ihres Maximums je Treffer (vier Treffer). Die Probe sitzt am **Eingang** der Klassen mit Vorprüfung, damit deren „tödlicher Treffer“-Ereignis wie bei einem normalen Todesstoß läuft, und auf `0x354B0` für alle übrigen.

## 4. KI: „Feinde ignorieren mich“

* `FUN_0002ECB0` (Gehirn-Update, einmal pro Gehirn und Tick aus 0x2FF43): Gehirn +0x878 = Besitzer, +0x87C = Besitzer, falls Gegner, +0x880 = Besitzer, falls Spieler (`FUN_0002CEB0`). Für das Spielergehirn: `mov [0x2DBAA0], al` bei 0x2ECE2 mit `al = (Spieler+0x630 == 0)`.
* `FUN_0002E2E0` (Wahrnehmung): bei gesetztem `0x2DBAA0` sofort „kein Ziel“; sonst die Spielerliste `[Level+0x98]` (Glied +0xC4C), Sichtprüfung `FUN_0002D030`, dann aufmerksam (`FUN_0002D980`) oder Angriff (`FUN_0002CC80` = Zustand 3, Ziel +0x1D4).
* `FUN_0002F720` (Angriffszustand): ist das Ziel ein Spieler und `0x2DBAA0` gesetzt → Rückkehr vor dem Angriff.
* `FUN_0002DEC0` (Schaden am Gegner → Aggro): setzt bei einem Spieler-Angreifer den Angriffszustand – der Gegner dreht sich zur Figur, greift aber wegen des Flags nicht an.
* Bosse mit eigener Logik im Gehirn-Update (Destructor `+0x774 == 0xE`, Sonnengott `0xC`) zielen über `[Level+0x94]` direkt und lesen das Flag nicht.

**Entscheidung:** `probe:AI::BrainUpdate` setzt nach jedem Gehirn-Update `0x2DBAA0 = 1`. Damit verhalten sich die Gegner wie während einer Zwischensequenz. Ausschalten im Spiel: Das nächste Update des Spielergehirns schreibt wieder den echten Wert.

## 5. Probes (Prologe mit Capstone geprüft, ganze, positionsunabhängige Befehle, kein Sprung hinein – Rohscan E8/E9/EB/Jcc über `.text`)

| Probe | Adresse | Prolog | Bedingung |
|---|---|---|---|
| `probe:Player::TakeDamage` | 0x51AD0 | `55 8B EC 83 E4 F0` | invulnerable oder ingame_toggles |
| `probe:Script::FsDeductLives` | 0xC1240 | `FF 0D DC 67 2D 00` | dto. |
| `probe:Actor::TakeDamage` | 0x354B0 | `64 A1 00 00 00 00` | one_hit_kill oder ingame_toggles |
| `probe:Npc::TakeDamage` | 0x47240 | `D9 44 24 04 83 EC 0C` | dto. |
| `probe:Kamikaze::TakeDamage` | 0x60080 | `D9 44 24 04 83 EC 0C` | dto. (Funktionsstart ergänzt) |
| `probe:Aquafish::TakeDamage` | 0x80300 | `83 EC 0C 56 8B F1` | dto. (ergänzt) |
| `probe:Goon::TakeDamage` | 0x86520 | `83 EC 18 56 8B F1` | dto. (ergänzt) |
| `probe:Junkbot::TakeDamage` | 0x88530 | `D9 44 24 04 83 EC 0C` | dto. (ergänzt) |
| `probe:Stormtrooper::TakeDamage` | 0x90570 | `83 EC 0C 56 8B F1` | dto. (ergänzt) |
| `probe:AI::BrainUpdate` | 0x2ECB0 | `55 8B EC 83 E4 F0` | enemies_ignore oder ingame_toggles |

Die fünf von Ghidra nicht erkannten Funktionen stehen in `tools\gen\extra_function_starts.tsv` (Größe bis zum letzten `ret 8`, nach dem alle Sprünge enden).

## 6. Rückmeldung beim Umschalten

Der eingebaute Cheat-Handler `FUN_0002C530` bestätigt mit `FUN_00013130` (thiscall auf die Kamera `[[0x2DABC4]+0x8C]`, Argumente r, g, b, Alpha 0,5, 0,1 s ein, 0,2 s halten, 0,2 s aus; `ret 0x1C`) – ein farbiges Aufblitzen. Der Port nutzt denselben Aufruf (grün = an, rot = aus) beim nächsten Tick. Ein eigener Hinweistext wurde verworfen: Die Hinweise des Spiels sind lokalisierte Text-IDs der Levelskripte (`HINTS_024`), ein freier Text bräuchte eigene Textobjekte.

## 7. Offen / Vermutungen

* Ereignis `FUN_000C2EE0`/`C2D50`/`C2E90` an `[Level+0x44]` vf+0x20(Position, Typ): Vermutung Beute bzw. Statistik.
* Bosskämpfe (Panzer 2-5, Destructor 5-x, Chickenwalker-Gegner) mit „Ein Treffer“ und „Feinde ignorieren mich“ nicht gespielt.
* Gegner ohne Gehirn-Angriff (Geschütze, Pflanzen, Statuenköpfe) – ob sie `0x2DBAA0` beachten, ist nicht geprüft.
