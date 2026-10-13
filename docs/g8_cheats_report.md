# G8: Cheats – Unverwundbar, Ein Treffer, Feinde ignorieren mich – Bericht

Stand: 2026-10-13. Nutzerwunsch: „Ich brauche noch Cheats – erstens Unverwundbarkeit, zweitens One-Hit-Kill, drittens Feinde ignorieren mich.“ Analyse: [`analysis/loader/g8_cheats.md`](../analysis/loader/g8_cheats.md). Abweichungen vom Entwurf: [`loader_design.md`](loader_design.md) 8.25. Schnittstellen: `src\INTERFACES.md` 4.41.

## 1. Kurzfassung

Neuer Abschnitt `[cheats]`, im Launcher Reiter **„Spiel“**, Gruppe **„Cheats“** (alle Schalter Standard aus; alles aus = kein Hook, das Spiel ist unverändert):

| Schalter | Wirkung | Wie (Spielcode) |
|---|---|---|
| **Unverwundbar** (`invulnerable`) | Kein Schaden durch Gegner, Geschosse, Explosionen, Fallen, schädliche Böden und Skript-Schaden (z. B. der rollende Stein in 2-2). Es geht **kein Leben** verloren. Todeszonen (Abgründe, Lava, tödliches Wasser) töten weiter – die Figur beginnt dann ohne Lebensverlust am letzten Kontrollpunkt (wie der eingebaute Gott-Modus des Spiels) | Probe auf `Player::TakeDamage` 0x51AD0 (alle fünf Spielerklassen inkl. Chickenwalker, Kanone, Ratte) überspringt den Schaden, außer beim Aufruf der Todeszone (Rückkehr 0x38C49); Probe auf das Skript-Native FsDeductLives 0xC1240 überspringt den Lebensabzug |
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
* **Skriptsequenzen**, in denen das Levelskript die Figur absichtlich „tötet“ (z. B. der rollende Stein in 2-2 „Minentunnel“, 1-5/1-7 nach einer Falle), töten mit „Unverwundbar“ nicht; die Figur spielt einfach weiter (auch der Gott-Modus des Spiels blockt diese Beträge). Nicht durchgespielt.
* **Bosse** mit „Ein Treffer“ (vier Treffer) und mit „Feinde ignorieren mich“ nicht gespielt. Der Destructor nimmt Schaden nur in bestimmten Animationen (eigene Logik, unverändert).
* **Feinde ignorieren mich** wirkt über das KI-Gehirn; Gegner ohne diesen Angriffsweg (Geschütze, Pflanzen, Statuenköpfe) sind nicht geprüft und greifen womöglich weiter an (**Vermutung**).
* Zu Fuß in Fry- und Bender-Leveln nicht im Lauf geprüft (Abschnitt 4.2).
* Statistik: Ob „Ein Treffer“ die Gegner-Statistik oder Beute am Levelende verändert, ist nicht geprüft; das „Todesstoß“-Ereignis des Spiels läuft wie bei einem normalen letzten Treffer.
