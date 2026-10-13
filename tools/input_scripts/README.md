# Eingabeskripte (`[debug] input_script`)

Ein Eingabeskript spielt Controller-Eingaben aus einer Textdatei ab, damit automatische Läufe Menüs bedienen und ein Level starten können (M4, `src\host\input\input_script.h`, Analyse `analysis\loader\m4_input.md` Abschnitt 10). Es wirkt wie ein eigener Controller an **Anschluss 1** (logischer Port 0) und wird mit Tastatur, Maus und echtem Controller zusammengeführt (Tasten ODER, größte Stick-Auslenkung).

Einschalten in der INI oder auf der Kommandozeile:

```ini
[debug]
input_script = D:\FuturamaDecomp\tools\input_scripts\m4_newgame.txt
```

`--set=debug.input_script=<datei>` geht ebenso. Relative Pfade gelten relativ zur INI.

## Zeit

* Grundeinheit ist der **Tick** (`t`): ein Logikschritt des Spiels, gemessen an den `XInputGetState`-Aufrufen für den Port. Innerhalb einer geskripteten Phase läuft ein Skript mit der Lockstep-Uhr (`[game] frame_clock = lockstep`, Standard) Tick für Tick gleich ab. Absolute Ticks verschieben sich aber zwischen Läufen um einige Ticks (gemessen bis 18), weil Intro und Laden echter I/O- und QPC-Zeit folgen. Skripte synchronisieren sich deshalb auf Bedingungen (`wait context …`, `wait main_menu`, `wait level_loaded`, `wait controllable`) statt auf absolute Zeiten (`docs/loader_design.md` 8.11 E14).
* `s` und `ms` sind **Spielsekunden**: Ticks geteilt durch `[game] tick_rate` (60 Hz: `2s` = 120 Ticks).
* `f` zählt angezeigte Bilder (`CApp::Frame`). Bei 120 fps sind das zwei Bilder pro Tick.

## Befehle

Eine Anweisung pro Zeile, `#` beginnt einen Kommentar (außerhalb von Anführungszeichen). Groß-/Kleinschreibung spielt keine Rolle.

| Befehl | Wirkung |
|---|---|
| `press A [3t]` | n Ticks halten (Standard 3), danach mindestens einen Tick loslassen. Mehrere Tasten: `press A B 2t` |
| `hold LY=1.0 120t` | halten und danach loslassen, ohne zusätzlichen losgelassenen Tick |
| `hold RT` / `set RX=-0.5` | halten, bis `release` folgt. Der Wert 0 lässt los |
| `release LY` / `release all` | loslassen |
| `wait 30t` / `wait 2s` / `wait 10f` | warten |
| `at 600t` | warten, bis seit dem Skriptstart so viel Zeit vergangen ist |
| `wait <bedingung> [timeout 20s]` | warten, bis die Bedingung gilt. Ohne Angabe gilt ein Zeitlimit von 60 s. Wird es überschritten, schlägt das Skript fehl |
| `expect <bedingung>` | sofort prüfen; ist die Bedingung falsch, schlägt das Skript fehl |
| `loop [timeout 300s] until <bedingung>` … `end` | Block wiederholen; die Bedingung wird vor jedem Durchlauf geprüft |
| `repeat 5` … `end` | Block n-mal ausführen |
| `log "text"` | Zeile `input script: line N: log: text` ins Protokoll |
| `screenshot name` | Bildschirmfoto des nächsten Bildes (`[render] screenshot_dir`), wartet 2 Bilder |
| `unplug` / `replug` | Abziehen bzw. Wiedereinstecken des Skript-Ports simulieren (Ingame: Pause mit „Controller anschließen“) |
| `game level_end Level1-2` | (M7) beendet das laufende Level wie das Levelskript an seinem Ende (Native FsChangeLevel `FUN_000B0520`): Statistikbildschirm „Breakdown“ mit *Speichern / Weiter / Wiederholen / Beenden*; der Name ist das nächste Level (1–8 Zeichen). Läuft beim nächsten Tick; ohne laufendes Level schlägt das Skript fehl. So erreicht ein kurzer Lauf den Speicherpunkt, ohne das Level durchzuspielen |
| `game camera_trace on` / `off` | (G5) schreibt pro Tick eine Zeile `camera trace: <n> mode M yaw Y pos … v … w … stick S ctrl C` mit dem Gierwinkel der Spielkamera (Kamera `[[0x2DABC4]+0x8C]`, ohne Hook); Auswertung `analysis\loader\scripts\g5_camera_trace.py` |
| `game state` | schreibt den Zustand der Spielfigur wie ein Spielstand ihn enthielte (`save: debug action state: Level=…;Health=…;Ammo=…;Pickup=…;Equip=…;Lives=…`, `FUN_000C26E0`) beim nächsten Tick; steht noch ein Startzustand an, nur dessen Größe. Ohne laufendes Level schlägt das Skript fehl |
| `quit [code]` | Prozess mit diesem Rückgabecode beenden (Standard 0) |
| `fail "text"` | Skript absichtlich fehlschlagen lassen |

**Tasten:** `A B X Y BLACK WHITE LT RT` (analog, ohne Wert voll gedrückt, sonst `LT=0.5`), `START BACK`, D-Pad `UP DOWN LEFT RIGHT` (auch `DPAD_UP` …), Stick-Klicks `LS RS`. **Achsen:** `LX LY RX RY` mit Werten −1..1, +Y ist oben. Der Rohwert ist `round(v · 32767)`; das Spiel hat eine eigene Totzone von 0,4 (m4_input.md 3.2).

## Bedingungen

`not` davor kehrt die Bedingung um.

| Bedingung | Bedeutung (Quelle) |
|---|---|
| `context Frontend` | aktueller Spielkontext (`Intro`, `Frontend`, `Attract`, `Ingame`, `Death`, `Intermission`; CApp+0xBC) |
| `log "text"` | eine Protokollzeile mit diesem Text, die noch keine frühere `log`-Bedingung des Skripts verbraucht hat. Zeilen des Skripts selbst zählen nicht |
| `main_menu` | Hauptmenü sichtbar und bereit für Bestätigen: Kontext Frontend, aktiviert (Probe `FUN_00020150`, +4), keine Eingabesperre (+0x14), keine Kamerafahrt (`[+0x34]+0x6C`), kein Countdown (+0x48) |
| `dialog` / `no_dialog` | Speicherdialog offen (`[[0x2EB310]+0xF4]+0xA`) |
| `level_loaded` | Level fertig geladen (Probe auf `FUN_000C3E20` beim Aufruf 0x2AB2F, Kontext Ingame) |
| `cutscene` / `no_cutscene` | Zwischensequenz aktiv (`[[0x2EB7C8]+0x34]`) |
| `paused` | Pausemenü offen (Probe `FUN_000A8220`, Byte +0x24) |
| `controllable` | Ingame, Level geladen, Spielerfigur nimmt Eingaben an (`[[0x2DABC4]+0x94]+0x630`), keine Zwischensequenz, keine Pause, kein Dialog |
| `active_pad` | der aktive Controller des Spiels (0x2E6FA4) ist der Port des Skripts |
| `rumble` | die letzte Rumble-Anforderung des Spiels (`XInputSetState`) für den Port des Skripts hat einen Motor über 0 |
| `true` / `false` | immer wahr bzw. falsch |

## Fehler

Syntaxfehler beenden den Start mit Rückgabecode 1 und einer Fehlerzeile mit Zeilennummer. Läuft ein Zeitlimit ab oder schlägt `expect`/`fail` fehl, schreibt das Skript `input script: FAILED at line N: …`, nimmt ein Bildschirmfoto und beendet den Prozess zwei Bilder später mit **Rückgabecode 7**.

## Protokoll

Jede ausgeführte Anweisung steht als `input script: line N: … (tick T, frame F)` im Protokoll, beim Beenden folgt `input script summary: …` mit Ticks, Tastendrücken, Bildschirmfotos und Rumble-Werten. `tools\m4_check.py` wertet diese Zeilen aus.

## Dateien

* `m4_newgame.txt`: Intro → Hauptmenü → „Neues Spiel“ → Level 1-1, einige Sekunden spielen, simuliertes Abziehen. Grundlage für `tools\m4_check.py`.
* `m4_rumble.txt`: Hauptmenü → „Optionen“ → Vibration aus und wieder ein; das Spiel spielt dabei seinen Test-Rumble (1,0 → 0 in 1 s). Grundlage für das Kriterium M4-7 von `tools\m4_check.py`.
* `m5_frontend.txt`: Hauptmenü etwa 130 s mit Menünavigation und einmal „Optionen“; die Frontend-Musik läuft dabei einmal ganz durch und loopt. Grundlage des Laufs `frontend` von `tools\m5_check.py` (M5, mit `[audio] capture_wav`).
* `m6_boot.txt`: die Logo-Filme und das Intro mit der XMV-HLE: Screenshot in jedem Film, START überspringt den ersten (fox), SCI (25 fps), uds und das Intro laufen bis zum Ende, Ende im Frontend (etwa 50 s). Für `tools\m6_check.py --run` mit `[audio] capture_wav` und `capture_stems`.
* `m6_level.txt`: „Neues Spiel“ → Film 1-1-01 beim ersten Ansehen 9 s lang (deutsche Sprache ab 4 s), Screenshot, A überspringt (mit `[game] skip_videos = always`), danach steuerbar (etwa 40 s). Mit `skip_videos = original` scheitert das Skript am Ende erwartungsgemäß (Protokoll: „keep playing“).
* `m6_attract.txt`: Frontend ohne Eingabe → Attract-Modus (Attract-PAL.xmv, 25 fps) nach etwa 60 s, Screenshot, A kehrt zurück (etwa 80 s).
* **Seit M6 laufen die Filme in Echtzeit** (Logos und Intro zusammen etwa 50 s). Die Skripte drücken deshalb bis zum Frontend START (`loop timeout 120s until context Frontend` … `press START 3t` … `wait 30t`), jeder Druck überspringt einen Intro-Schritt (`Intro::Update` 0x2B9B0). Prüfungen mit festen Bildnummern ohne Skript (m2/m3/m3c/m3d) laufen mit `--set=debug.movies=skip` (Filme enden sofort wie vor M6).
* `m7_save.txt`, `m7_load.txt`, `m7_damaged.txt`, `m7_slots.txt`, `m7_nospace.txt` (M7): Spielstände – Optionen speichern, Neues Spiel, Levelende mit `game level_end` und Speichern; Neustart, Optionen/Extras geladen, Spielstand laden und erneut speichern; beschädigte Sicherung löschen; 25 Sicherungen und Überschreiben; zu wenig Platz (`[saves] free_space_limit`). Jeweils 20–55 s, nur zusammen mit `tools\m7_check.py` sinnvoll (es bereitet die Dateien vor und prüft sie). Die Skripte warten auf die Protokollzeilen der Spielstand-Proben (`wait log "save: dialog MEMCARD_007"` usw., `src\INTERFACES.md` 4.33) und drücken bei Ja/Nein-Fragen erst LINKS („Ja“; vorausgewählt ist „Nein“, beim Platzdialog `MEMCARD_047` aber „Steuerung“).
* `g5_camera.txt` (G5): Sprungantwort der Kamera in Level 1-1 mit `game camera_trace on` – rechter Stick −1,0 / +1,0 je 1 s, −0,6 / +0,6 je 1,5 s, dazwischen losgelassen. Einmal mit `[input] camera_response = original`, einmal mit `precise` laufen lassen und mit `analysis\loader\scripts\g5_camera_trace.py` auswerten (etwa 40 s).
* `g7_level_skip.txt` (G7): im ersten Level (Level 1-1 oder `[game] start_level`) A A B B A A B B A A → Statistikbildschirm → „Weiter“ → „Ja“ → das nächste Level lädt → dort die Folge noch einmal. Die Protokollzeilen `cheat: level skip (…) in <Level> … FsChangeLevel("<Folgelevel>")` nennen beide Level (50–60 s).
* `g6_first_person.txt` (G6): Egoperspektive (`[input] first_person = on`) im Level von `[game] start_level` – vorwärts, seitwärts, drehen, auf-/abschauen, rückwärts-diagonal, springen, beide Angriffe, Ziel erfassen (RT), R3 zur Verfolgerkamera und zurück (etwa 35 s). Mit `[debug] trace = game` schreibt der Kamera-Hook je Tick eine Zeile `first person trace: …`; `analysis\loader\scripts\g6_firstperson_trace.py` prüft daraus, ob die Figur entlang der Blickrichtung läuft.
* `g2_textures.txt` (G2): Hauptmenü, Level 1-1 aus drei Blickrichtungen, ein Stück gelaufen und zwei Freecam-Ansichten ohne HUD – je ein Screenshot für Vorher/Nachher-Vergleiche mit `[render] texture_pack` bzw. für den Textur-Abzug mit `[render] texture_pack_dump = 1` (etwa 35 s).
* `g8_cheat_toggles.txt` (G8): mit `[cheats] ingame_toggles = 1` im Level von `[game] start_level` die Steuerkreuz-Folgen für Unverwundbar an, Feinde ignorieren an, Ein Treffer an und Unverwundbar aus (wartet jeweils auf `cheat: <key> ON|OFF`), danach eine durch A unterbrochene Folge, die nichts schalten darf; Bildschirmfotos des grünen und roten Aufblitzens (etwa 35 s).
* Eigene Skripte für Testläufe gehören nach `run\<name>\` (nicht ins Repository).
