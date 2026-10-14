# G8: Cheats – Unverwundbar, Ein Treffer, Feinde ignorieren mich – Bericht

Stand: 2026-10-13. Nutzerwunsch: „Ich brauche noch Cheats – erstens Unverwundbarkeit, zweitens One-Hit-Kill, drittens Feinde ignorieren mich.“ Analyse: [`analysis/loader/g8_cheats.md`](../analysis/loader/g8_cheats.md). Abweichungen vom Entwurf: [`loader_design.md`](loader_design.md) 8.25. Schnittstellen: `src\INTERFACES.md` 4.41.

**Nachtrag 2026-10-14 (Nutzerwunsch: weitere Cheats):** Zur Freecam teleportieren, Mondsprung (vor allem für Zoidberg auf der Ratte in 4-1), Sprungweite um x Meter erhöhen und Unendlich Munition – Abschnitt 8.

**Nachtrag 2026-10-13 (Fehlermeldung: Softlock mit den Tentakeln in 1-2):** behoben, siehe Abschnitt 7. Kurz: Mit „Feinde ignorieren mich“ bemerken die Kanalisations-Tentakel Fry nicht mehr, mit „Unverwundbar“ können sie ihn nicht mehr packen, und ein tödlicher Schaden, der länger als **10 s** ununterbrochen geblockt wird, läuft als Tod mit Neustart am Kontrollpunkt durch (kein Leben verloren).

## 1. Kurzfassung

Neuer Abschnitt `[cheats]`, im Launcher Reiter **„Spiel“**, Gruppe **„Cheats“** (alle Schalter Standard aus; alles aus = kein Hook, das Spiel ist unverändert):

| Schalter | Wirkung | Wie (Spielcode) |
|---|---|---|
| **Unverwundbar** (`invulnerable`) | Kein Schaden durch Gegner, Geschosse, Explosionen, Fallen, schädliche Böden und Skript-Schaden (z. B. der rollende Stein in 2-6 „Minentunnel“; Skript level2-6, „MINETUNNEL:: rolling stone killed player“). Es geht **kein Leben** verloren. Todeszonen (Abgründe, Lava, tödliches Wasser) töten weiter – die Figur beginnt dann ohne Lebensverlust am letzten Kontrollpunkt (wie der eingebaute Gott-Modus des Spiels) | Probe auf `Player::TakeDamage` 0x51AD0 (alle fünf Spielerklassen inkl. Chickenwalker, Kanone, Ratte) überspringt den Schaden, außer beim Aufruf der Todeszone (Rückkehr 0x38C49); Probe auf das Skript-Native FsDeductLives 0xC1240 überspringt den Lebensabzug |
| **Gegner sterben mit einem Treffer** (`one_hit_kill`) | Jeder Treffer der Spielfigur tötet einen Gegner. **Bosse** (Panzer 1000, Chickenwalker-Gegner 2000, Destructor 10000 Gesundheit) verlieren ein Viertel ihrer Gesundheit je Treffer (vier Treffer), damit ihre Kampfphasen der Reihe nach laufen. Figuren, die das Spiel selbst unverwundbar macht (Gesundheit 55555: Pflanzen, Kaktusmonster, Krokodil, Sonnengott, Igner-Boss, Statuenkopf; Tentakel 55000; per Skript geschützte Figuren), bleiben es | Proben am Eingang der Schadensfunktionen der Gegnerklassen (0x47240 NPC-Basis, 0x60080, 0x80300, 0x86520, 0x88530, 0x90570) und auf `Actor::TakeDamage` 0x354B0: Treffer eines Spielers → Betrag = Restgesundheit. So läuft auch das „Todesstoß“-Ereignis des Spiels (Beute/Statistik, Vermutung) wie bei einem normalen letzten Treffer |
| **Feinde ignorieren mich** (`enemies_ignore`) | Gegner bemerken die Figur nicht und greifen nicht an – wie in einer Zwischensequenz. Ein getroffener Gegner dreht sich zur Figur, greift aber nicht an. Bosse mit eigener Angriffslogik (Destructor, Sonnengott), schon fliegende Geschosse und Fallen treffen weiter. Gegner lassen sich weiter besiegen | Die KI des Spiels hat ein Flag „Feuer einstellen“ (0x2DBAA0), das das Gehirn der Spielfigur jeden Tick auf „Figur nicht steuerbar“ setzt. Probe auf das Gehirn-Update 0x2ECB0 setzt es danach auf 1: Wahrnehmung (0x2E2E0) findet kein Ziel, Angriff (0x2F720) unterbleibt |
| **Zur Freecam teleportieren**, **Mondsprung**, **Sprungweite um x Meter erhöhen**, **Unendlich Munition** (2026-10-14) | siehe Abschnitt 8 | Abschnitt 8.3 |
| **Cheats im Spiel per Steuerkreuz umschalten** (`ingame_toggles`) | Sechs Tastenfolgen (seit 2026-10-14; vorher drei) auf dem Steuerkreuz schalten die Cheats im Level an und aus (Abschnitt 2) | Filter in der Eingabekette (nach Freecam und G7), alle Hooks installiert |

## 2. Tastenfolgen im Spiel (`ingame_toggles = 1`)

Im Level, solange die Figur steuerbar ist (nicht in Menüs, Pause, Zwischensequenzen, Statistikbildschirm, Freecam), **acht Drücke auf dem Steuerkreuz** zügig hintereinander (alle innerhalb von 5 s, höchstens 1 s Pause; Tastatur: Pfeiltasten):

| Cheat | Folge |
|---|---|
| Unverwundbar | ↑ ↑ ↓ ↓ ← → ← → |
| Gegner sterben mit einem Treffer | ← → ← → ↑ ↓ ↑ ↓ |
| Feinde ignorieren mich | ↓ ↑ ↓ ↑ → ← → ← |
| Zur Freecam teleportieren (2026-10-14) | → → ← ← ↑ ↑ ↓ ↓ |
| Mondsprung (2026-10-14) | ↑ → ↓ ← ↑ → ↓ ← (zwei Kreise im Uhrzeigersinn) |
| Unendlich Munition (2026-10-14) | ↓ ↓ ↑ ↑ ← ← → → |

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

## 8. Nachtrag 2026-10-14: Teleport, Mondsprung, Sprungweite, Unendlich Munition

Nutzerwunsch (2026-10-14): drei weitere Cheats in der Gruppe „Cheats“ – zur Freecam-Position teleportieren, Mondsprung, unendlich Munition; danach präzisiert: Der Mondsprung ist vor allem für **Zoidberg auf der Ratte in 4-1** gedacht („äußerst schwierig“), und neu **„Sprungweite um x Meter erhöhen“** (0,0 bis 2,0 m). Analyse: [`analysis/loader/g8_cheats.md`](../analysis/loader/g8_cheats.md) Abschnitt 9. Schnittstelle `src\INTERFACES.md` 4.41.

### 8.1 Kurzfassung

| Schalter (`[cheats]`) | Bedienung | Wirkung |
|---|---|---|
| **Zur Freecam teleportieren (Freecam: RB / T) (→ → ← ← ↑ ↑ ↓ ↓)** (`teleport_freecam`) | In der Freecam (L3+R3 bzw. F6) **RB** drücken, Tastatur **T** | Die Figur steht danach auf dem Boden unter der Kamera (Strahltest des Spiels senkrecht nach unten, bis 300 m). Freecam verlassen: Die Figur ist dort, die Spielkamera folgt. Ohne Boden darunter, in Fahrzeugen (Chickenwalker 1-7, Kanone 2-6, Ratte 4-1) und während die Figur festgehalten wird: nichts (rotes Aufblitzen). Sonst grünes Aufblitzen |
| **Mondsprung (LB + B halten) (↑ → ↓ ← ↑ → ↓ ←)** (`moon_jump`) | **B** (Springen) und **LB** gedrückt halten; Tastatur mit der Standardbelegung **Leertaste + 1** | Die Figur steigt mit 40 Einheiten/s (≈ 7 m/s), solange beide gehalten werden – **auch Zoidberg mit der Ratte in 4-1** (die Ratte läuft dabei weiter ihre Strecke entlang). Loslassen: normaler Fall mit der Schwerkraft des Spiels; die **Landung danach verursacht keinen Fallschaden** (sonst töten Stürze über 100 Einheiten ≈ 18 m). Erst B, dann LB drücken: Dann wechselt Fry dabei nicht die Waffe (LB = Waffe zurück, die einzige Belegung von LB im Spiel) |
| **Sprungweite um x Meter erhöhen** (`jump_distance`, 0,0–2,0 in Schritten von 0,1, Einheit m) | – | Jeder Sprung **aus dem Lauf** landet x m weiter, Höhe und Flugzeit bleiben. Gilt für Fry, Bender, Leela und die Ratte. Sprünge aus dem Stand bleiben unverändert. 0 = aus (kein Hook) |
| **Unendlich Munition (↓ ↓ ↑ ↑ ← ← → →)** (`infinite_ammo`) | – | Fry verbraucht keine Munition (Pistole, Schrotflinte, Maschinengewehr, Railgun): die Anzeige bleibt stehen; ein leeres Magazin der gehaltenen Waffe wird auf das Maximum des Spiels gefüllt (500/200/300/400). Chickenwalker (1-7): Laserenergie immer voll, keine Raketen weniger. Kanone (2-6): keine Schüsse weniger. Bender und Leela: Ihre **Aufladung** („Power“, bis 4,0, je Spezialangriff 1,0 – die einzige begrenzte Ressource der beiden) sinkt nicht |

Alles aus = keine neuen Hooks (Lauf `default`: `hooks: 184 installed`, wie vorher). Mit `ingame_toggles` sind alle Hooks installiert (198), und die drei neuen Folgen schalten die Schalter um; `jump_distance` ist eine Zahl und hat keine Folge. Levelnummern wie im Spiel (Ordner: 1-7 = `Level1-6`, 2-6 = `Level2-5`).

### 8.2 Maßstab: Spieleinheiten und Meter

Das Spiel kennt keine Meter. Anhaltspunkte (Lauf `explore1`, Physik-Mitschrift mit `[debug] trace = game`): Frys Kollisionskörper ist ein Ellipsoid mit den Halbachsen 1,6 / 4,8 / 1,6, Mittelpunkt 4,8 über den Füßen – also **9,6 Einheiten hoch**. Für einen Mann von etwa 1,75 m (Annahme) sind das **5,5 Einheiten je Meter**. Das passt zu den übrigen Werten: Laufen 25 Einheiten/s = 4,5 m/s, Schwerkraft 95 Einheiten/s² = 17 m/s² (spieltypisch etwa 1,8 g), tödlicher Sturz ab 100 Einheiten = 18 m, Frys Sprung aus dem Lauf 13 Einheiten = 2,4 m weit und 5,6 Einheiten = 1 m hoch. 2,0 m sind also 11 Einheiten (`kUnitsPerMetre = 5.5`, `MetresToUnits`).

### 8.3 Umsetzung

| Teil | Wie (Spielcode, Adressen der Retail-EU-XBE) |
|---|---|
| Physik | Jede Figur hat einen Physikkörper (Spieler +0x5C): Schwerkraft +0x0C (Spieler −95; die Ratte −130 steigend, −180 fallend; während Frys/Benders/Leelas Sprunganimation 0), Dämpfungs-Flag +0x18 (waagerecht × 0,5 je Schritt), Geschwindigkeit +0x28 (Y = oben), „am Boden“ +0x45. Die Physikwelt rechnet alle Körper einmal je Logiktick: `FUN_000AE7E0` (vtable 0x2A1ECC +0xC) → je Körper `FUN_000AF170` |
| Mondsprung | **`probe:Physics::Step`** 0xAE7E0 (`83 EC 64 53 56`): Solange LB + B gehalten sind, setzt die Probe vor dem Schritt die senkrechte Geschwindigkeit der gesteuerten Figur auf mindestens 40. Die Fallhöhe des Spiels (Spieler +0xB84; Fallschaden ab 40/100 Einheiten in `FUN_0005A700`) läuft bis zur Landung mit. Die Tasten erkennt ein Filter in der Eingabekette (`inp::cheattoggles::MoonJumpFilter`); er blendet ein LB, das bei gehaltenem B gedrückt wurde, für das Spiel aus |
| Sprungweite | Dieselbe Probe erkennt den Absprung: **Animationssprung** (Fry, Bender, Leela: Sprungwunsch +0xB89 im Schritt davor am Boden, jetzt Schwerkraft 0) oder **Physiksprung** (Ratte: am Boden, Sprungwunsch, +56 nach oben). Flugzeit T: Animationssprung = gemessene Länge der Sprunganimation dieser Figur (Fry 0,48 s, Leela 0,58 s, Bender 0,45 s; bei jedem Sprung aus dem Lauf nachgemessen), Ratte aus 56 / 130 / 180 (0,80 s). Zusatz e = x / T entlang der Laufrichtung: vor dem Schritt e / k dazu (k = Dämpfung des Spiels), danach e wieder weg – jeder Schritt fliegt e·dt weiter, die Geschwindigkeit des Spiels bleibt unangetastet. Mit dem Ende der Animation bzw. der Landung, an einer Wand (waagerecht fast 0) oder nach 1,5 T endet der Zusatz. Jeder Sprung steht mit seiner Weite im Protokoll (`cheat: jump N (...) - landed ... units = ... m from the take-off`) |
| Teleport | Freecam (`gfx\freecam.cpp`): RB/T merkt die Kameraposition vor. Beim nächsten Logiktick (`cheats::OnTick`): Strahltest des Spiels `FUN_000FB6F0` senkrecht nach unten (wie der Neustart der Ratte), dann die Platzierung des Spiels: `SetPosition` (vtable +0x1C = `FUN_000353B0`, Füße → Körpermitte), Kontakte des Kollisionsobjekts zurücksetzen (vf+4), Geschwindigkeit 0, Fallhöhe +0xB84 = neue Höhe – die Teile von `FUN_00057910`/`FUN_00057660` (Neustart am Kontrollpunkt), die die Figur setzen, ohne deren Gesundheits- und Waffen-Reset |
| Munition | **`probe:Player::UseAmmo`** 0x50560 (vtable +0x90 aller Spielerklassen, `8B 44 24 04 8B 54 24 08`): bei „Unendlich Munition“ übersprungen – der Zähler +0xAD0[Slot] und die Anzeige bleiben. Leeres Magazin: `FUN_00051D50` (SetAmmo des Spiels, mit Anzeige). Ohne Hook, je Tick: Chickenwalker ([Spieler+0x7B0]) Energie +0xE4 = 500, Raketen +0xC4 nicht unter den höchsten Stand; Kanone ([Spieler+0x7A8]) Schüsse +0x90 nach einem Schuss (Abklingzeit > 0,2 s) zurück; Aufladung +0xAF8 nicht unter den höchsten Stand |

Dateien: `src\host\xapi\cheats_player.h/.cpp` (neu: Konstanten, reine Entscheidungen, 2 Proben, Tick-Teil), `xapi\cheats.h/.cpp` (Schalter, Statistik, `QueueFlash`, `CountPlayerCheat`), `input\cheat_toggles.h/.cpp` (6 Folgen, `MoonJumpButtons`, `MoonJumpFilter`), `input\input_system.cpp` (Filter), `gfx\freecam.cpp` (RB/T), `core\config_schema.cpp` (4 Zeilen), `tools\gen\extra_function_starts.tsv` + `gen\xbe_functions.gen.inc` (2 Funktionsstarts), `tests\g8_player_cheats_tests.cpp` (neu, 7 Fälle), `tools\input_scripts\g8_player_cheats.txt`.

### 8.4 Prüfung

Bauten: MSVC RelWithDebInfo `build\cheats2` (`FUT_WERROR=ON`) und clang-cl Debug `build\cheats2-clang` (`fut_tests`, `FUT_WERROR=ON`) ohne Warnungen; `fut_tests` je **542/542** (1 übersprungen; vorher 535). Neue Fälle: Schema/Hook-Bedingungen (Standard keine Hooks, `jump_distance` geklemmt), Mondsprung-Geschwindigkeit und Maßstab, Flugzeit (symmetrisch, Ratte 0,80 s), Boost-Plan (Richtung, Sprung aus dem Stand), Schritt mit Dämpfung 0,5 und 1 (genau e·dt weiter, Geschwindigkeit danach unverändert, Wand), Sprung-Simulation (+11 Einheiten auf 0,1 %), Absprungerkennung (Animation, Physik, Rückstoß ohne Wunsch), Teleportziel, Zähler (Raketen/Aufladung, Kanone), Mondsprung-Tasten (B dann LB: LB ausgeblendet; LB dann B), die drei neuen Folgen, und gegen die Retail-XBE: beide Prologe, die vtable-Slots (Physikwelt, Körper, Spielerklassen +0x1C/+0x90/+0x104), Dämpfung 0,5, Schwerkraft −95/−130/−180, Sprungwunsch 0x50E2D, Fallhöhe 0x57935, Strahltest-Aufruf 0x620A3, Munitionsmaxima, Walker- und Kanonenfelder.

Läufe (RelWithDebInfo, Lautstärke 0, Rumble 0, Filme übersprungen, je 30–60 s, nacheinander; `run\cheats2\<name>\` des Arbeitsbaums):

| Lauf | Level, Schalter | Ergebnis | Bilder (angesehen) |
|---|---|---|---|
| `ratmoon` | 4-1 Ratte, Mondsprung | `moon jump - lifting (type 4 …)`, die Ratte steigt von −2,9 auf 16,6, fällt normal und landet auf der Strecke (`landed at 1498.5 2.9 1453.5 (highest 16.6), no fall damage`), Gesundheit bleibt | `frame_001296.png` (Ratte mit Zoidberg hoch über ihrem Schatten), `frame_001322.png` (im Fall), `frame_001364.png` (wieder auf dem Weg) |
| `rat0` | 4-1, Mondsprung 60 Ticks | steigt auf 50,5, fliegt 131 Einheiten weit, landet ohne Fallschaden – aber im Sumpf (die Ratte läuft in der Luft weiter; der Sumpf schadet wie im Original) | `frame_001469.png` (Schatten tief unten), `frame_001545.png` (Füße der Ratte am oberen Bildrand) |
| `explore1` | 1-1 Fry, Mondsprung 90 Ticks | von −2,1 auf 61, `landed … no fall damage` (ohne den Cheat wäre das ein tödlicher Sturz) | `frame_001796.png` (Frys Schatten, Fry über dem Bildrand) |
| `ammo` | 1-2 Fry, `ingame_toggles` | 10 Schüsse ohne Cheat: Pistole 100 → 90; ↓↓↑↑←←→→ → `cheat: infinite_ammo ON`; **50 Schüsse: bleibt 90** (`50 shots kept`); ↑→↓←↑→↓← → `moon_jump ON`, B dann LB: Fry steigt, **die Pistole bleibt ausgerüstet** (LB ausgeblendet) | `frame_001591.png` (Anzeige 90 nach 10 Schüssen), `frame_002685.png` (90 nach weiteren 50), `frame_002820.png` (Fry hoch über den Kisten) |
| `walker` | 1-7 Chickenwalker | 30 Laserschüsse: Energie jedes Mal von 476,7 auf 500 zurück; 8 Raketen: `rockets 4.0 -> 5.0`, Anzeige bleibt 5 | `frame_002227.png` (Raketen 5, Energiebalken voll) |
| `tp` | 1-1, Teleport + Mondsprung | Freecam hoch und zurück, RB: `from 327.6 -2.1 336.2 to 313.3 22.3 290.1 (… ground 17.0 below)` – Fry steht oben auf der Galerie. Zweiter Teleport aus 73 Einheiten Höhe über dem Dach: Der Strahl trifft den Tisch im Besprechungsraum 47,8 tiefer, Fry steht darauf | `frame_001684.png` (Fry auf der Galerie nach dem Verlassen der Freecam), `frame_001927.png` (Fry auf dem Tisch) |
| `fry0`/`fry1`/`fry2` | 1-1, Sprungweite 0 / 1,0 / 2,0 | gleicher Sprung aus dem Lauf: **13,0 / 17,8 / 23,1 Einheiten = 2,37 / 3,24 / 4,21 m** (+0,87 / +1,84 m); Rücksprung 14,7 / 20,4 / 26,1 = 2,67 / 3,71 / 4,74 m (+1,04 / +2,07 m); Sprung aus dem Stand je 0,03 m (unverändert) | `fry2\shots\frame_001546.png` (Fry im Sprung) |
| `rat0j`/`rat1`/`rat2` | 4-1, Sprungweite 0 / 1,0 / 2,0 | gleicher Sprung: **53,0 / 59,0 / 65,1 Einheiten = 9,63 / 10,73 / 11,84 m** (+1,09 / +2,20 m; Landung je etwas tiefer, Flug 0,88 s) | – |
| `bender0`, `leela0` | 2-1, 3-1, Sprungweite 0,1 | Animationsphase gemessen: Bender 0,45 s, Leela 0,58 s (jetzt Startwerte); Bender aus dem Lauf 2,30 m weit | – |
| `default` | 1-1, ohne `[cheats]` | `hooks: 184 installed`, `cheats: all off (no hooks)` | – |
| `toggles` | 1-1, nur `ingame_toggles` | 198 Hooks; LB + B gehalten ohne Umschalten: kein Steigen (`0 lifts`) | – |

Abweichung der Weiten vom Sollwert: bis etwa 15 % (Fry Hinsprung +0,87 statt +1,0 m: Die Laufrichtung dreht sich während der Animation etwas; die Ratte etwas mehr, weil sie tiefer landet und länger fliegt).

### 8.5 Checkliste für den Nutzer

1. Launcher → **Spiel** → **Cheats**: **Mondsprung** anhaken, Startlevel **4-1**. Mit der Ratte erst **B** (springen), dann zusätzlich **LB** halten: Zoidberg und die Ratte steigen; loslassen: Sie fallen normal und landen ohne Schaden. Hinweis: Die Ratte läuft in der Luft weiter – über dem Sumpf über den Holzflößen loslassen.
2. Mondsprung mit Fry (z. B. 1-1): B, dann LB halten – Fry steigt, die Waffe wechselt nicht. Tastatur: Leertaste, dann 1.
3. **Sprungweite** auf 1,0 bzw. 2,0 m stellen: Sprünge aus dem Lauf werden sichtbar länger (Fry von etwa 2,4 m auf 3,2 bzw. 4,2 m, die Ratte von etwa 9,6 auf 10,7 bzw. 11,8 m), Sprünge aus dem Stand bleiben gleich. Vorsicht bei Sprungpassagen mit kleinen Plattformen.
4. **Zur Freecam teleportieren** anhaken. Im Level L3+R3 (bzw. F6), mit der Freecam an eine andere Stelle fliegen, **RB** (Tastatur **T**) drücken – grünes Aufblitzen. Freecam verlassen: Die Figur steht auf dem Boden unter der Stelle, an der die Kamera war.
5. **Unendlich Munition** mit Fry in 1-2: beliebig schießen, die Zahl unter der Waffe bleibt stehen. Im Chickenwalker (1-7): Laser und Raketen ohne Ende. Mit Bender/Leela: Spezialangriffe verbrauchen keine Aufladung.
6. Mit **Cheats im Spiel per Steuerkreuz umschalten**: → → ← ← ↑ ↑ ↓ ↓ (Teleport), ↑ → ↓ ← ↑ → ↓ ← (Mondsprung), ↓ ↓ ↑ ↑ ← ← → → (Munition) schalten im Level um (grün an, rot aus).
7. Gegenprobe: alles aus → Spiel wie bisher.

### 8.6 Grenzen

* **Teleport**: Gesucht wird nur senkrecht unter der Kamera (bis 300 m); darunter nichts → kein Teleport. Schwebt die Kamera von außen über einem Dach, trifft der Strahl die erste Fläche, die er von oben trifft (im Lauf: der Tisch unter dem Dach des Besprechungsraums). Wer die Figur hinter Türen, Auslöser oder Skriptgrenzen setzt, kann Levelskripte überspringen und festsitzen. Nicht in Fahrzeugen (Chickenwalker, Kanone, Ratte: deren Code bestimmt die Position; die Ratte folgt ihrer Strecke) und nicht während eines Griffs.
* **Mondsprung**: Unter einer Decke oder einem Hindernis steigt die Figur nicht (im Lauf `tp` auf dem Tisch unter der Lampe: weder der normale Sprung noch der Mondsprung hoben ab). Im Chickenwalker und an der Kanone aus. Die Ratte läuft während des Mondsprungs ihre Strecke weiter ab (Sumpf und Abgründe schaden wie im Original). Die Fallschaden-Freiheit gilt bis zur nächsten Landung.
* **Sprungweite**: nur waagerecht, in Laufrichtung beim Absprung; Höhe und Flugzeit unverändert. Bei Sprungpassagen (schmale Plattformen, Lücken mit genauer Weite) kann man über das Ziel hinausspringen – ein Opt-in-Cheat, bewusst so. Die Länge der Sprunganimation wird je Figur bei jedem Sprung aus dem Lauf nachgemessen; der erste Sprung nutzt die Messwerte von 2026-10-14. Bender und Leela: je ein Sprung gemessen; Doppel- oder Spezialsprünge mit anderer Animation nicht geprüft.
* **Unendlich Munition**: Railgun (5-3) und Kanone (2-6) nicht im Lauf geprüft (die Railgun nutzt dieselbe UseAmmo-Funktion; die Kanone verwaltet ihre Schüsse mit dem Levelskript, das sie auch nachlädt – senkt das Skript den Zähler selbst, bleibt das so). Walker-Raketen und Aufladung werden einen Tick nach dem Verbrauch zurückgesetzt (die Anzeige kann einen Tick lang zucken). Bender/Leela-Aufladung nicht im Lauf geprüft (Levelstart mit Aufladung 0).
