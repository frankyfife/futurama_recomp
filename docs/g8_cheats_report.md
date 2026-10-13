# G8: Cheats – Unverwundbar, Ein Treffer, Feinde ignorieren mich – Bericht

Stand: 2026-10-13. Nutzerwunsch: „Ich brauche noch Cheats – erstens Unverwundbarkeit, zweitens One-Hit-Kill, drittens Feinde ignorieren mich.“ Analyse: [`analysis/loader/g8_cheats.md`](../analysis/loader/g8_cheats.md). Abweichungen vom Entwurf: [`loader_design.md`](loader_design.md) 8.25. Schnittstellen: `src\INTERFACES.md` 4.41.

**Nachtrag 2026-10-13 (Fehlermeldung: Softlock mit den Tentakeln in 1-2):** behoben, siehe Abschnitt 7. Kurz: Mit „Feinde ignorieren mich“ bemerken die Kanalisations-Tentakel Fry nicht mehr, mit „Unverwundbar“ können sie ihn nicht mehr packen, und ein tödlicher Schaden, der länger als **10 s** ununterbrochen geblockt wird, läuft als Tod mit Neustart am Kontrollpunkt durch (kein Leben verloren).

## 1. Kurzfassung

Neuer Abschnitt `[cheats]`, im Launcher Reiter **„Spiel“**, Gruppe **„Cheats“** (alle Schalter Standard aus; alles aus = kein Hook, das Spiel ist unverändert):

| Schalter | Wirkung | Wie (Spielcode) |
|---|---|---|
| **Unverwundbar** (`invulnerable`) | Kein Schaden durch Gegner, Geschosse, Explosionen, Fallen, schädliche Böden und Skript-Schaden (z. B. der rollende Stein in 2-6 „Minentunnel“; Skript level2-6, „MINETUNNEL:: rolling stone killed player“). Es geht **kein Leben** verloren. Todeszonen (Abgründe, Lava, tödliches Wasser) töten weiter – die Figur beginnt dann ohne Lebensverlust am letzten Kontrollpunkt (wie der eingebaute Gott-Modus des Spiels) | Probe auf `Player::TakeDamage` 0x51AD0 (alle fünf Spielerklassen inkl. Chickenwalker, Kanone, Ratte) überspringt den Schaden, außer beim Aufruf der Todeszone (Rückkehr 0x38C49); Probe auf das Skript-Native FsDeductLives 0xC1240 überspringt den Lebensabzug |
| **Gegner sterben mit einem Treffer** (`one_hit_kill`) | Jeder Treffer der Spielfigur tötet einen Gegner. **Bosse** (Panzer 1000, Chickenwalker-Gegner 2000, Destructor 10000 Gesundheit) verlieren ein Viertel ihrer Gesundheit je Treffer (vier Treffer), damit ihre Kampfphasen der Reihe nach laufen. Figuren, die das Spiel selbst unverwundbar macht (Gesundheit 55555: Pflanzen, Kaktusmonster, Krokodil, Sonnengott, Igner-Boss, Statuenkopf; Tentakel 55000; per Skript geschützte Figuren), bleiben es | Proben am Eingang der Schadensfunktionen der Gegnerklassen (0x47240 NPC-Basis, 0x60080, 0x80300, 0x86520, 0x88530, 0x90570) und auf `Actor::TakeDamage` 0x354B0: Treffer eines Spielers → Betrag = Restgesundheit. So läuft auch das „Todesstoß“-Ereignis des Spiels (Beute/Statistik, Vermutung) wie bei einem normalen letzten Treffer |
| **Feinde ignorieren mich** (`enemies_ignore`) | Gegner bemerken die Figur nicht und greifen nicht an – wie in einer Zwischensequenz. Ein getroffener Gegner dreht sich zur Figur, greift aber nicht an. Bosse mit eigener Angriffslogik (Destructor, Sonnengott), schon fliegende Geschosse und Fallen treffen weiter. Gegner lassen sich weiter besiegen | Die KI des Spiels hat ein Flag „Feuer einstellen“ (0x2DBAA0), das das Gehirn der Spielfigur jeden Tick auf „Figur nicht steuerbar“ setzt. Probe auf das Gehirn-Update 0x2ECB0 setzt es danach auf 1: Wahrnehmung (0x2E2E0) findet kein Ziel, Angriff (0x2F720) unterbleibt |
| **Cheats im Spiel per Steuerkreuz umschalten** (`ingame_toggles`) | Drei Tastenfolgen auf dem Steuerkreuz schalten die Cheats im Level an und aus (Abschnitt 2) | Filter in der Eingabekette (nach Freecam und G7), alle Hooks installiert |

## 2. Tastenfolgen im Spiel (`ingame_toggles = 1`)

Im Level, solange die Figur steuerbar ist (nicht in Menüs, Pause, Zwischensequenzen, Statistikbildschirm, Freecam), **acht Drücke auf dem Steuerkreuz** zügig hintereinander (alle innerhalb von 5 s, höchstens 1 s Pause; Tastatur: Pfeiltasten):

| Cheat | Folge |
|---|---|
| Unverwundbar | ↑ ↑ ↓ ↓ ← → ← → |
| Gegner sterben mit einem Treffer | ← → ← → ↑ ↓ ↑ ↓ |
| Feinde ignorieren mich | ↓ ↑ ↓ ↑ → ← → ← |

* Jede Folge schaltet um (an ↔ aus). Bestätigung: Das Bild blitzt **grün** (an) bzw. **rot** (aus) auf – derselbe Effekt wie bei den eingebauten Cheatcodes des Spiels – und der Controller vibriert kurz (an 0,4 s, aus 0,15 s; nur mit `[input] rumble = 1`, Stärke nach `rumble_strength`). Im Protokoll: `cheat: invulnerable ON (Unverwundbar "an", UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT, port 1)`.
* Ein anderer Knopf zwischendurch (A, B, X, Y, Schultertasten, START …) oder zwei Richtungen gleichzeitig (diagonal) beginnen von vorn; der Stick stört nicht. Das Steuerkreuz bewegt die Figur dabei ein paar Schritte (im Spiel läuft man auch mit dem Steuerkreuz) – sonst passiert nichts.
* Die Folgen kollidieren weder mit dem Level-Skip (A A B B A A B B A A) noch mit den eingebauten Cheatcodes (LT gehalten + Knöpfe).
* Was im Launcher an ist, ist beim Start an; die Folgen schalten von dort aus um. Ohne `ingame_toggles` wirken die Folgen nicht.

## 3. Umsetzung

| Teil | Datei | Inhalt |
|---|---|---|
| Optionen, Hooks, Entscheidungen | `src\host\xapi\cheats.h/.cpp` | `SettingsFromConfig`, Bedingungen je Cheat (`FUT_PROBE_IF`: an beim Start oder `ingame_toggles`), reine Entscheidungen `BlockPlayerDamage` und `OneHitDamage`, 10 Proben, Tick (`OnTick`: Aufblitzen, Gesundheits-Mitschrift), Zusammenfassung beim Beenden |
| Tastenfolgen | `src\host\input\cheat_toggles.h/.cpp` | `DpadSequenceDetector` (rein), Filter `SampleFilter`, Rumble-Puls |
| Gemeinsame Bedingung mit G7 | `src\host\input\level_skip.h/.cpp` | `InputCheatArmed(port, reason)` (vorher G7-intern) |
| Rumble-Puls | `src\host\input\host_router.h/.cpp` | `HostRouter::PulseRumble`, `PulseActive` (Maximum mit dem Rumble des Spiels, skaliert und stumm wie dieses) |
| Tick | `src\host\xapi\save_probes.cpp` | `RunPendingGameActions` ruft `cheats::OnTick` |
| Schema | `src\host\core\config_schema.cpp` | Abschnitt `cheats`, 4 Zeilen mit deutschen/englischen Texten |
| Funktionsstarts | `tools\gen\extra_function_starts.tsv`, `src\host\gen\xbe_functions.gen.inc` | 5 Schadensfunktionen, die Ghidra nicht als Funktionen kannte |

Protokollzeilen (Kanal GAME): `cheats: …` beim Start, `cheat: invulnerable - <n> damage to <Figur> blocked (… attacker <Gegner> …)` (die ersten 40, dann jede 100.), `cheat: invulnerable - instant-death floor …`, `cheat: invulnerable - life kept …`, `cheat: one-hit kill - <Gegner> hit by <Figur>: a -> b damage …`, `cheat: player health A -> B, lives L -> M` (nur mit Cheat-Hooks; Verluste, Gewinne ab 5, Leben), am Ende `cheats summary: …`.

## 4. Prüfung

### 4.1 Bauten und Unit-Tests

* MSVC RelWithDebInfo `build\cheats` (`FUT_WERROR=ON`): ohne Warnungen. `fut_tests`: **533/533** (1 übersprungen; vorher 527).
* clang-cl Debug `build\cheats-clang` (`fut_tests`, `FUT_WERROR=ON`): ohne Warnungen, 533/533.
* Neu `tests\g8_cheats_tests.cpp` (6 Fälle): Schema und Hook-Bedingungen (Standard: keine Hooks); Unverwundbar-Entscheidung (Todeszone, Skript-Schaden 1e6, Boden); Ein-Treffer-Entscheidung (tödlich, schon tödlich, 55555, 55000, Bosse 10000/1000, Grenze goonbox 450); Folgen-Erkenner (drei Folgen, Halten ohne Loslassen, anderer Knopf, diagonal, Pause > 1 s, Fenster > 5 s, nicht scharf, Sticks); Rumble-Puls (Maximum, Ende, `rumble = 0`, Stärke); Prologe, vtable-Slots +0x7C, Aufruf 0xB1885 → 0xC1240, Schreibstelle 0x2ECE2 des Flags, Aufblitz-Aufruf 0x2C7A5 gegen die Retail-XBE.

### 4.2 Läufe (RelWithDebInfo, Lautstärke 0, Rumble 0, Filme übersprungen, je 30–80 s; `run\cheats\<name>\` des Arbeitsbaums)

| Lauf | Level, Schalter | Ergebnis (Protokoll) | Bild |
|---|---|---|---|
| `default` | 1-1, ohne `[cheats]` | `hooks: 184 installed` (wie ohne G8), `cheats: all off (no hooks)` | – |
| `base31` | 3-1 Leela, alles aus (nur `ingame_toggles`, Hooks reichen durch) | Skelette treffen: `player health 100.0 -> 90.0 … -> 0.0`, Leben 5 → 4 → 3 (Skelette und Lava) | – |
| `inv16` | 1-6 Fry im Chickenwalker, Unverwundbar + Ein Treffer | 153 Treffer der Stormtrooper geblockt (`10.0 damage to fry/PLAYER blocked (health stays 400.0; attacker stormtrooper/S3_ST_00 …)`), Gesundheit bleibt 400 | `inv16\shots\frame_003391.png`: Cockpit, Gesundheitsbalken voll, Trooper im Bild |
| `ohk31` | 3-1, Unverwundbar + Ein Treffer | Skelett-Treffer geblockt; zweimal in die Lava gelaufen: `instant-death floor … passes`, `player health 100.0 -> 0.0`, dann `life kept (FsDeductLives skipped, lives stay 5)` und `0.0 -> 100.0, lives 5 -> 5` | `ohk31\shots\frame_003186.png`: Leela in der Lava; `frame_004664.png`: danach am Kontrollpunkt, Leben 5 |
| `ohk31b` | 3-1, Unverwundbar + Ein Treffer | `one-hit kill - skel_hth/Sk_M1_W1 hit by Leela/PLAYER: 10.0 -> 40.0 damage (health 40.0 of 40.0 …)`, ebenso Sk_M2_W1: beide Skelette mit je einem Tritt besiegt | `ohk31b\shots\frame_002411.png` (zwei Skelette an Leela), `frame_002699.png` (beide weg) |
| `ign31` | 3-1, nur Feinde ignorieren mich | Kein einziger Treffer der Skelette (im Vergleich zu `base31`), Flag 3488-mal gesetzt; einmal Lava (Leben 5 → 4, Unverwundbar war aus) | `ign31\shots\frame_003186.png`: Skelette bleiben fern |
| `tog31` | 3-1, nur `ingame_toggles`, Skript `g8_cheat_toggles.txt` | Folgen schalten Unverwundbar an, Feinde ignorieren an, Ein Treffer an, Unverwundbar aus; die durch A unterbrochene Folge schaltet nichts; Puls `feedback pulse port 1 … for 24 ticks (rumble off)` bzw. 9 Ticks | `tog31\shots\frame_001357.png` grünes, `frame_001787.png` rotes Aufblitzen |

Nicht gelungen: In 1-2, 1-3, 1-4, 1-5, 1-7, 2-1 und 2-2 kam in 60–80 s nahe dem Levelstart kein Gegner an die Figur (die Skriptläufe laufen nur geradeaus) – Bender und Fry zu Fuß sind deshalb nicht im Lauf geprüft. Dieselben Funktionen (Spielerklassen-Slot +0x7C, NPC-Basis 0x47240) sind aber für alle Figuren gleich (Unit-Test der vtable-Slots), und Frys Geschosse übergeben den Schützen als Angreifer (`FUN_00077E50` → `FUN_000503E0`).

## 5. Checkliste für den Nutzer

1. Launcher → Reiter **Spiel** → Gruppe **Cheats**: **Unverwundbar** anhaken, Level starten (z. B. Startlevel 3-1). Von Skeletten schlagen lassen: Der Gesundheitsbalken bleibt voll, Leela zuckt nicht. In die Lava springen: Leela stirbt, beginnt aber am Kontrollpunkt, die Lebenszahl bleibt gleich.
2. **Gegner sterben mit einem Treffer**: Jeder Gegner fällt beim ersten Treffer (Fry: ein Schuss; Bender/Leela: ein Schlag/Tritt). Geschützte Figuren (Pflanzen, Krokodil, Sonnengott …) bleiben unverwundbar wie im Original. Bei einem Boss (Panzer in 2-6 „Schrottplatz 3“, Destructor in Kapitel 5): vier Treffer statt einem – bitte melden, ob der Kampf normal weiterläuft.
3. **Feinde ignorieren mich** (Unverwundbar dabei aus): Gegner laufen nicht auf die Figur zu und greifen nicht an; man kann sie trotzdem besiegen. Bosse kämpfen weiter. Falls ein Kampf stockt, weil ein Gegner erst angreifen muss, um verwundbar zu werden: im Spiel ausschalten (Punkt 4).
4. **Cheats im Spiel per Steuerkreuz umschalten** anhaken. Im Level zügig drücken: ↑ ↑ ↓ ↓ ← → ← → → grünes Aufblitzen + kurzes Vibrieren = Unverwundbar an; dieselbe Folge noch einmal → rotes Aufblitzen = aus. Ebenso ← → ← → ↑ ↓ ↑ ↓ (Ein Treffer) und ↓ ↑ ↓ ↑ → ← → ← (Feinde ignorieren). Mit Tastatur: Pfeiltasten.
5. Gegenprobe: Alles aus → Spiel wie bisher.

## 6. Grenzen und offene Punkte

* **Todeszonen** (Abgründe, Lava, tödliches Wasser) töten auch mit „Unverwundbar“ – wie im eingebauten Gott-Modus der Entwickler. Es geht aber kein Leben verloren. Überleben dort würde in Abgründen festsetzen.
* **Skriptsequenzen**, in denen das Levelskript die Figur absichtlich „tötet“ (z. B. der rollende Stein in 2-6 „Minentunnel“, 1-5/1-7 nach einer Falle), töten mit „Unverwundbar“ nicht; die Figur spielt einfach weiter (auch der Gott-Modus des Spiels blockt diese Beträge). Wiederholt ein Skript oder Gegner den tödlichen Schaden ohne Pause länger als 10 s, greift seit dem Nachtrag das Sicherheitsnetz (Abschnitt 7). Nicht durchgespielt.
* **Bosse** mit „Ein Treffer“ (vier Treffer) und mit „Feinde ignorieren mich“ nicht gespielt. Der Destructor nimmt Schaden nur in bestimmten Animationen (eigene Logik, unverändert).
* **Feinde ignorieren mich** wirkt über das KI-Gehirn. Nachtrag: Der einzige Gegner, der ohne Gehirn angreift, ist der Kanalisations-Tentakel – jetzt behoben (Abschnitt 7). Geschütze, Pflanzen und Statuenköpfe greifen laut Code nur über das Gehirn an (statische Analyse, `analysis/loader/g8_cheats.md` 8.3; im Lauf nicht geprüft).
* Zu Fuß in Fry- und Bender-Leveln nicht im Lauf geprüft (Abschnitt 4.2).
* Statistik: Ob „Ein Treffer“ die Gegner-Statistik oder Beute am Levelende verändert, ist nicht geprüft; das „Todesstoß“-Ereignis des Spiels läuft wie bei einem normalen letzten Treffer.

## 7. Nachtrag: Softlock mit den Kanalisations-Tentakeln (1-2)

### 7.1 Fehler und Ursache

Meldung des Nutzers (2026-10-13): In 1-2 mit `invulnerable`, `one_hit_kill` und `enemies_ignore` packen Tentakel Fry, ziehen ihn in ein Rohr, das Spiel hängt; „Feinde ignorieren mich“ wirkt bei ihnen nicht. Sein Protokoll (nur gelesen) zeigt ab 56,4 s jeden Tick `cheat: invulnerable - 100.0 damage to fry-1-2/PLAYER blocked (health stays 100.0; attacker Tentacle_dude/TSewA_1 vt 0x0029F790, call from 0x0008C6F7; …)` – 1583-mal in 26 s.

Ursache (Analyse `analysis/loader/g8_cheats.md` Abschnitt 8):

* Der Tentakel (`tentacle_dude`, vtable 0x29F790) hat **kein KI-Gehirn**. Sein Update `FUN_0008C4F0` sucht die Figur selbst (Abstand < 12, Abklingzeit ≤ 0) und liest das Haltefeuer-Flag 0x2DBAA0 nicht – daher wirkte „Feinde ignorieren mich“ nicht.
* Nach dem Animationsereignis `morph:pull_through_pipe` zieht das Update der Figur **in jedem Tick ihre volle Gesundheit** ab (Aufruf 0x8C6F9), bis sie tot ist; erst der Neustart am Kontrollpunkt löst den Griff. „Unverwundbar“ blockte jeden dieser Treffer – die Figur starb nie, der Griff endete nie.

### 7.2 Behebung

| Fall | Verhalten jetzt | Wie |
|---|---|---|
| „Feinde ignorieren mich“ an | Tentakel bemerken Fry nicht, greifen nicht an, bleiben im Rohr | `probe:Tentacle::Update` 0x8C4F0 hält die Abklingzeit der Erkennung (+0x8C4) auf mindestens 0,5 s |
| „Unverwundbar“ oder „Feinde ignorieren mich“ an | Ein Tentakel, der zuschlägt, **packt nicht**: der Griff wird zum Fehlgriff des Spiels, der Tentakel zieht sich zurück und schlägt nach etwa 1,7 s erneut zu. Fry kann jederzeit weitergehen | `probe:Tentacle::AnimEvent` 0x8CAD0: Ereignis `morph:grab` nimmt den eigenen „verfehlt“-Weg des Spiels (+0x8C0 = 1) |
| „Unverwundbar“, Griff läuft schon | Der Durchzieh-Tod läuft durch wie eine Todeszone: Fry stirbt, beginnt am Kontrollpunkt, **kein Leben weg** | Rücksprung 0x8C6FC wird nicht geblockt. Im Lauf nicht herstellbar: Während des Griffs ist die Figur „nicht steuerbar“, die Steuerkreuz-Folgen werden dann nicht angenommen; reine Absicherung |
| **Sicherheitsnetz** („Unverwundbar“) | Tödlicher Schaden desselben Angreifers (oder Skripts), der **länger als 10 s** ohne Pause über 1 s geblockt wird, läuft einmal durch: Tod, Kontrollpunkt, kein Leben weg. Protokoll `cheat: invulnerable - safety net: …` | `LethalBlockWatch`, Konstanten `kLethalBlockLimitSeconds = 10` (Nutzervorgabe), `kLethalBlockGapSeconds = 1` |
| Alle Cheats aus | unverändert (Tentakel packt, zieht durch, Fry stirbt, ein Leben weg) | keine neuen Hooks ohne Cheat bzw. `ingame_toggles` |

Andere Gegner: Geschütze, Pflanzen, Statuenköpfe, Turdmen, Augenstiele usw. greifen laut Code nur über das Gehirn an, „Feinde ignorieren mich“ wirkt bei ihnen schon (statische Analyse; Turdmen und Augenstiele im Lauf bestätigt). Übersicht aller Griff-, Quetsch- und Dauer-Schäden: `analysis/loader/g8_cheats.md` 8.4 (Turdman schüttelt mit 1,78 und lässt von selbst los; fleischfressende Pflanze packt Bender und lässt über `switch_to_normal` los; rollender Stein in 2-6 ist Skriptschaden 1e6; schädliche Böden 10 je 2 s; Todeszonen; Explosionen).

### 7.3 Prüfung

Bauten: MSVC RelWithDebInfo `build\tent` (`FUT_WERROR=ON`) und clang-cl Debug `build\tent-clang` (`fut_tests`) ohne Warnungen, `fut_tests` je **535/535** (1 übersprungen; vorher 533). Neue Fälle in `tests\g8_cheats_tests.cpp`: „sewer tentacle“ (Durchzieh-Tod läuft durch, Hook-Bedingung, `SkipTentacleGrab` inkl. Groß-/Kleinschreibung und anderer Ereignisse, `TentacleHoldCooldown` inkl. NaN) und „safety net“ (jeden Tick ab 5 s: erst nach 10 s durch, nicht vorher; 9,9 s blockt; danach neue Serie; Pause > 1 s und wechselnde Angreifer beginnen neu; Skriptschaden alle 0,5 s; Uhr rückwärts); dazu XBE-Prüfungen der Probe-Stellen (Prologe, vtable-Slots +0x5C/+0xE8/+0x7C, Aufruf 0x8C6F9, Erkennung 0x8C675/0x8C688, Fehlgriff 0x8CCF5, `ret 8`/`ret 4`).

Läufe (Level1-2, Ego-Perspektive, Lautstärke 0, Rumble 0, Filme übersprungen, je etwa 60 s; `run\tent\<name>\` des Arbeitsbaums, Skript `run\tent\tent_route.txt` mit Varianten). Der Weg zum Tentakel TSewA_1 (64, 1121) wurde mit einer neuen Positionsmitschrift (`[debug] trace = game`: `tentacle trace: …`) gesucht: +x, −z, −x, +z bis zum Giftbecken, über den Steg, am anderen Ufer +x bis direkt vor das Rohr. Der Weg läuft mit „Feinde ignorieren mich“ (sonst stoßen Turdmen Fry ins Giftwasser); wo nötig, schaltet das Skript es kurz vor dem Tentakel per Steuerkreuz aus.

| Lauf | Schalter am Tentakel | Ergebnis (Protokoll) | Bilder |
|---|---|---|---|
| `base` | keiner (alle Hooks durchgereicht) | Original: `event morph:grab (player grabbed by Tentacle_dude/TSewA_1 …)`, `pickup`, `shake`, zweimal `hit_head` 2,0, `pull_through_pipe`, dann 73 Ticks lang `player damage 100.0 … return 0x0008C6FC`, `health 66.0 -> 0.0`, Neustart `0.0 -> 100.0, lives 5 -> 4` | `base\shots\frame_002895.png` (Blick aus dem Tentakel), `frame_003139.png` (Levelanfang, Leben 4) |
| `inv` | nur Unverwundbar | Tentakel schlägt 6-mal zu (alle 1,7 s), jedes Mal `cheat: invulnerable - Tentacle_dude/TSewA_1 grab skipped (morph:grab -> miss …)`, Greifer bleibt 0, Gesundheit 100, kein Leben weg; Fry geht danach weiter (x 61 → 80) | `inv\shots\frame_002795.png` (Tentakel vor Fry, kein Griff) |
| `ign` | nur Feinde ignorieren mich | `enemies ignore me - Tentacle_dude/TSewA_1/2/3 does not look for the player`; Fry steht 10 s im Abstand 1,5 vor dem Rohr: Abklingzeit 0,48, kein Angriff, kein Ereignis, kein Schaden | `ign\shots\frame_002962.png` (Tentakel bleibt im Rohr) |
| `all` | Unverwundbar + Ein Treffer + Feinde ignorieren mich (Konfiguration des Nutzers) | kein Angriff (`0 tentacle grabs skipped`, `3 tentacle holds`), kein Softlock | `all\shots\frame_003206.png` |
| `midgrab` | Unverwundbar per Steuerkreuz während des Griffs | Folge nicht angenommen (`not counted … character not controllable`) – Ablauf wie `base` (Leben 5 → 4). Zeigt: Ein schon laufender Griff ist mit den Steuerkreuz-Folgen nicht erreichbar | – |

Nicht im Lauf geprüft: das Sicherheitsnetz (kein Fall im Spiel gefunden, der es mit den neuen Hooks noch braucht; nur Unit-Test) und der Durchzieh-Tod bei schon laufendem Griff (siehe `midgrab`).

### 7.4 Checkliste für den Nutzer

1. Level 1-2 mit **Unverwundbar** (ohne „Feinde ignorieren mich“): zum Giftbecken mit den Tentakeln in den Rohren gehen und davor stehen bleiben. Der Tentakel schlägt nach Fry, packt ihn aber nicht; Fry kann jederzeit weitergehen.
2. Mit **Feinde ignorieren mich**: Die Tentakel bleiben in ihren Rohren.
3. Gegenprobe ohne Cheats: Der Tentakel packt Fry, zieht ihn ins Rohr, Fry verliert ein Leben (wie im Original).
4. Falls irgendwo anders ein Gegner oder ein Skript Fry trotz „Unverwundbar“ festhält: Nach spätestens 10 s stirbt Fry und beginnt ohne Lebensverlust am Kontrollpunkt. Bitte melden, wo das passiert (Protokollzeile `cheat: invulnerable - safety net: …`).
