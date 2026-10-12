# Meilenstein M4: Eingabe (XInput, Tastatur und Maus, Rumble, Eingabeskripte) – Bericht

Stand: 2026-10-12 (Phase 5, nach Behebung der Review-Befunde) · Ziel-XBE: `orig\retail_eu\default.xbe` (Retail EU) · Verbindliches Design: [`docs/loader_design.md`](loader_design.md) (E7, Abweichungen 8.11 E1–E20) · Analyse: [`analysis/loader/m4_input.md`](../analysis/loader/m4_input.md), [`analysis/loader/avi.md`](../analysis/loader/avi.md) (Eingabe) · Schnittstellen: [`src/INTERFACES.md`](../src/INTERFACES.md) 4.26–4.28 · Nichts committet.

## 1. Ergebnis in Kürze

**Das Spiel ist steuerbar, und zum ersten Mal erreicht ein Lauf ein Level.**

- **Eingabe:** Die 7 XPP-Einstiege sind keine Stubs mehr, sondern eine HLE über einem Eingabe-Router. Xbox-Controller (XInput 1.4), Tastatur und Maus (Raw Input) und ein Testskript steuern gemeinsam Spieler 1. Alle 24 Xbox-Tasten sind für Controller und Tastatur/Maus frei belegbar, Rumble kommt mit einstellbarer Stärke am zuletzt benutzten Controller an, Ein- und Ausstecken wird wie auf der Xbox gemeldet.
- **Launcher:** neuer Reiter „Steuerung“ mit je einer Belegungstabelle für Controller und Tastatur/Maus, Aufnahmedialog („Taste drücken“), Konflikt- und Fehleranzeige, Schiebereglern (Vibrationsstärke, tote Zone, Mausempfindlichkeit) und einem Controller-Test.
- **Automatisierte Läufe:** `[debug] input_script` spielt ein Textskript als virtuellen Controller ab. `tools\m4_check.py` startet damit „Neues Spiel“, lädt Level 1-1 (Planet-Express-Hangar), läuft, springt, greift an, zieht den Controller simuliert ab und setzt mit START fort – in Debug, RelWithDebInfo und clang-cl **ALL PASS**.
- **Erweiterbar für die Freecam (M4b):** Der Router kann einen Anschluss an einen anderen Verbraucher umleiten (`InputRouter::Divert`); das Spiel sieht dann einen verbundenen, neutralen Controller. Tastatur/Maus liefert zusätzlich Rohdaten (`ReadRaw`: Tasten, Mausweg).
- **Review:** 17 Befunde, alle nachgeprüft und angenommen, alle behoben (Befund 6 vorbeugend, ohne Messung im Level). Darunter ein Absturz mit 0xC0000409 bei fehlerhaftem Skript, Rumble am falschen von zwei Controllern, Mehrfach-Auslösung bei hochauflösenden Mausrädern und rechte Strg-/Alt-Taste, die im Launcher als linke belegt wurde. Abschnitt 8.
- **Nicht mit echter Hardware geprüft:** kein Controller angeschlossen; XInput, Hotplug, Rumble-Gefühl und echte Mausbewegung sind nur mit Test-Doubles, simulierten Quellen und Unit-Tests belegt (Abschnitt 9).

## 2. Was gebaut wurde

| Teil | Inhalt | Dateien |
|---|---|---|
| XPP-HLE | `XInitDevices`, `XGetDevices`, `XGetDeviceChanges`, `XInputOpen`, `XInputClose`, `XInputGetState`, `XInputSetState` nach dem Vertrag aus avi.md 4.1 und m4_input.md 2: 4 Anschlüsse mit Hotplug, Paketnummer, `dwStatus` von `XInputSetState` synchron, keine Memory Units, kein Headset | `src\host\input\xpp_hle.*` (ersetzt `xpp_stubs.*`) |
| Eingabe-Router | Quellen je Anschluss zusammenführen (Tasten ODER, Analogtasten Maximum, je Achse größte Auslenkung), Fokusregel, Unterdrückung gehaltener Tasten, Rumble-Ziel, Umleitung (`Divert`) für M4b, simuliertes Abziehen | `input\host_router.*`, `input\input_api.h` |
| XInput-Backend | `xinput1_4.dll` zur Laufzeit (Rückfall `xinput9_1_0.dll`), Hotplug-Abfrage gedrosselt, radiale tote Zone, Belegungen, Rumble | `input\xinput_source.*` |
| Tastatur/Maus | Raw Input am Spielfenster, Mausfang, Taps kürzer als ein Tick, Mausrad als Tastendruck, Mausbewegung über die Umkehrung der Stick-Kennlinie, Esc in Menüs als Zurück | `input\kbm_source.*`, `input\kbm_window.*`, `input\stick_curve.h` |
| Belegungen | 48 Schlüssel `pad_<taste>` / `kbm_<taste>`, Token-Sprache `pad:`, `key:`, `mouse:`, Prüfung beim Start (Rückgabecode 1 bei Fehlern) | `input\bindings.*`, `input\input_config.*`, `core\config_schema.cpp` |
| Eingabeskript | Textformat mit `press/hold/wait/loop/expect/screenshot/unplug/quit`, Bedingungen (Kontext, Hauptmenü, Level geladen, steuerbar, Pause, aktiver Controller, Rumble), Tick-Herzschlag | `input\input_script.*`, `tools\input_scripts\` |
| Prüfung | `tools\m4_check.py` (M4-1 bis M4-9) | `tools\` |
| Launcher | Reiter „Steuerung“, Aufnahmedialog, Controller-Test, Schieberegler | `launcher\src\core\bindings.*`, `core\pad_reader.*`, `ui\controls_panel.*`, `ui\binding_capture.*`, `ui\pad_poller.*`, `ui\key_editors.cpp` |
| Tests | 3 neue Testdateien der Eingabeschicht, Skript-Tests, Launcher-Tests (`tst_bindings`, UI-Tests des Reiters) | `tests\input_*_tests.cpp`, `launcher\tests\` |

## 3. Aufbau der Eingabe

```
 XInput-Pads ─┐      Tastatur/Maus (Raw Input) ─┐      Eingabeskript ─┐
 (XInputPadSource)   (KeyboardMouseSource)       │      (ScriptSource) │
              └──────────────┬──────────────────┴──────────────────────┘
                             ▼  Belegungen (bindings.cpp) je Quelle → PadState
                      HostRouter (Port 1–4)
          ┌──────────────────┼─────────────────────┐
  Divert(port, consumer)     │ SampleGamePort       │ SetGameRumble
  (M4b: Freecam)             ▼                      ▼
                  XPP-HLE: XInputGetState      XInputSetState → Motoren des
                  (einmal je Logiktick)        zuletzt benutzten Controllers
```

- **Quellen und Belegung.** Jede Quelle liefert pro Logiktick einen virtuellen Xbox-Controller (`inp::PadState`: 8 Tastenbits, 8 Analogtasten 0–255, 4 Stickachsen). Die Belegung bildet physische Eingaben auf die 24 Xbox-Tasten ab, nicht auf Spielaktionen: Die Aktionstabelle des Spiels ist fest, und die Menüs lesen die 16 Kanäle direkt (m4_input.md 5.1). Ein digitaler Eingang auf einer analogen Taste gibt den vollen Wert, ein analoger auf einer digitalen Taste zählt ab halber Auslenkung. Tastatur-Diagonalen (W+D) liegen wie bei einem Pad auf dem Einheitskreis.
- **Anschlüsse.** `[input] pad_mode = single` (Standard) legt alle Controller, Tastatur/Maus und das Skript auf Anschluss 1; `per_port` gibt jedem Controller einen eigenen Anschluss. Tastatur/Maus hält Anschluss 1 immer verbunden; deshalb löst das Abziehen eines echten Pads keine Pause aus, solange Tastatur und Maus da sind (8.11 E3). Für Tests gibt es `SimulateUnplug`.
- **Abtastung.** Das Spiel ruft `XInputGetState` einmal je Logiktick und offenem Anschluss; dort tastet der Router seine Quellen ab. Kurze Tastendrücke zwischen zwei Ticks gehen nicht verloren (ein Tick lang gemeldet).
- **Maus.** Mausbewegung wird bis zum Tick gesammelt, mit `mouse_sensitivity` skaliert und über die Umkehrung der Stick-Kennlinie des Spiels (tote Zone 0,4, Verstärkung 1,3) in einen Rohwert für den rechten Stick umgerechnet; so dreht die Kamera proportional zur Mausbewegung. Ein Klick ins Fenster fängt die Maus, Fokusverlust gibt sie frei und lässt alle Tasten los.
- **Rumble.** `XInputSetState` setzt `dwStatus` synchron (das Spiel sendet nur, solange der Status nicht „pending“ ist). Der Router skaliert mit `rumble_strength`, schaltet ohne Fokus, bei umgeleiteten und abgezogenen Anschlüssen ab und schickt die Werte nur an den zuletzt **bewusst** benutzten Controller (Taste, Trigger ab 30/255, Stick über 0,2).
- **Fokus und Hintergrund.** Physische Quellen zählen nur bei aktivem Spielfenster (`background_input` = 0). Nach Fokus-Rückkehr zählen gehaltene Tasten erst nach dem Loslassen.
- **Erweiterbarkeit (M4b).** `InputRouter::Divert(port, InputConsumer*)` leitet die Eingaben eines Anschlusses an einen Verbraucher um (die Freecam); das Spiel sieht einen verbundenen, neutralen Controller, und gehaltene Tasten gehen nach dem Ende der Umleitung erst nach dem Loslassen wieder ans Spiel. `KeyboardMouseSource::ReadRaw` liefert Tasten und Mausweg ohne Pad-Nachbildung (WASD + Maus für die Freecam).

## 4. Standardbelegung

| Xbox-Taste | Bedeutung im Spiel (Schema) | Controller | Tastatur/Maus |
|---|---|---|---|
| A | Bestätigen / Angriff 1 | A | Eingabe, linke Maustaste |
| B | Zurück / Springen | B | Leertaste |
| X | Angriff 2 | X | F, rechte Maustaste |
| Y | Benutzen | Y | E |
| Weiß / Schwarz | Waffe zurück / vor (Fry) | LB / RB | 1, Mausrad runter / 2, Mausrad hoch |
| Linker / rechter Trigger | Kamera, Zielwechsel / Ziel erfassen | LT / RT | Q / linke Umschalttaste |
| START / BACK | Pause, Bestätigen / Zurück in Menüs | Start / Back | Esc (in Menüs: Zurück, `menu_escape`) / Rücktaste |
| Steuerkreuz | Menüauswahl / Laufen | Steuerkreuz | Pfeiltasten |
| Linker Stick | Laufen / Menü | linker Stick | W A S D, Klick C |
| Rechter Stick | Kamera, Klick Ego-Ansicht | rechter Stick | Mausbewegung, Klick V oder mittlere Maustaste |

Reservierte Tasten des Hosts (gelten zusätzlich zur Belegung): F12 und Druck = Screenshot, Alt+Eingabe = Fenster/Vollbild. Der Launcher warnt, wenn sie belegt werden.

## 5. Benutzung

### 5.1 Für Spieler: Reiter „Steuerung“

- **Belegungstabellen** „Controller“ und „Tastatur und Maus“: Xbox-Taste, Bedeutung im Spiel, Belegung. Ein Klick auf die Belegung öffnet „Taste drücken“ und **ersetzt** die Liste (der Dialog nennt, was er ersetzt); „+“ fügt hinzu (höchstens 4 verschiedene Eingaben), „✕“ entfernt, „↺“ und „… zurücksetzen“ stellen den Standard her. Zurücksetzen mehrerer Zeilen fragt vorher nach.
- **Aufnahme:** Tastatur einschließlich Esc, Tab und Eingabe (Abbrechen nur per Schaltfläche oder nach 10 s), Maustasten im Feld des Dialogs, Mausrad, Mausbewegung über vier Schaltflächen. Rechte Strg-/Alt-Taste und AltGr werden aus der Windows-Nachricht erkannt, „Druck“ beim Loslassen. Controller: der erste Knopf, Trigger oder Stick nach dem Öffnen; ohne Controller sagt der Dialog das, der Countdown wartet, und jede Controller-Eingabe lässt sich aus einer Liste wählen.
- **Hinweise:** Doppelbelegungen (erlaubt, aber selten gewollt), Screenshot-/Vollbild-Tasten, A/START/linker Stick ohne Belegung (Warnungen); ungültige Belegungen sperren Speichern und „Spielen“.
- **Weitere Einstellungen** („Eingabegeräte und Vibration“): Controller-Anschlüsse, Vibration an/aus und Stärke, tote Zone der Sticks, Mausempfindlichkeit, Maus im Fenster fangen, „START-Taste der Tastatur geht in Menüs zurück“; im Reiter Erweitert „Controller auch im Hintergrund“. Schieberegler zeigen einen von Hand gesetzten Wert außerhalb des Rasters genau an und ändern ihn erst, wenn man den Regler bewegt.
- **Controller-Test:** verbundene Controller mit Knöpfen, Sticks und Triggern, welche Xbox-Tasten das Spiel daraus erhält (mit Belegung, toter Zone und den Schwellen des Spiels) und „Vibration testen“. Nicht verbundene Anschlüsse stehen in einer Zeile.

Screenshots (offscreen gegen das echte Schema von `build\m4-final-rel`): `run\m4-final\launcher\shots\de_tab_controls_full.png`, `de_tab_controls_kbm_full.png`, `de_controls_capture_kbm.png`, `de_controls_capture_pad.png`, `de_controls_pad_test_simulated.png`; bei 225 % dunkel in `run\m4-final\launcher\shots_dark_2.25\`.

### 5.2 Konfiguration (`futurama.ini`, Abschnitt `[input]`)

| Schlüssel | Standard | Bedeutung |
|---|---|---|
| `pad_<taste>`, `kbm_<taste>` | siehe Abschnitt 4 | Belegungsliste, z. B. `kbm_a = key:enter, mouse:left`; leer = nicht belegt; Format `src\INTERFACES.md` 4.26 |
| `pad_mode` | `single` | `single` alle auf Anschluss 1, `per_port` ein Anschluss je Controller |
| `rumble`, `rumble_strength` | `1`, `100` | Vibration an/aus, Stärke in Prozent der Werte des Spiels |
| `pad_deadzone` | `0` | radiale tote Zone in Prozent (0–50) vor der toten Zone des Spiels (0,4) |
| `mouse_sensitivity` | `1.0` | 0,1–10; 1,0 = 24 Zählimpulse in einem Tick ergeben volle Auslenkung |
| `mouse_capture`, `menu_escape`, `background_input` | `1`, `1`, `0` | Mausfang, Tastatur-START in Menüs als Zurück, Controller im Hintergrund |

### 5.3 Eingabeskripte (für Entwickler und automatische Läufe)

`[debug] input_script = <datei>` spielt ein Textskript als virtuellen Controller auf Anschluss 1 ab (Format: `tools\input_scripts\README.md`). Zeitbasis ist der Logiktick. Innerhalb einer Phase läuft ein Skript Tick für Tick gleich, absolute Ticks verschieben sich aber über Intro und Laden um einige Ticks (8.11 E14); Skripte warten deshalb auf Bedingungen (`wait context Frontend`, `wait main_menu`, `wait level_loaded`, `loop … until controllable`). Ein Syntaxfehler oder eine fehlende Datei beendet den Start mit Rückgabecode 1, ein Fehlschlag zur Laufzeit (`expect`, Zeitlimit, `fail`) mit 7.

```
python tools\m4_check.py --exe build\m4-final-rel\bin\futurama.exe --config run\m4-final\level\futurama.ini --runs 2
```

`tools\input_scripts\m4_newgame.txt` (Intro → „Neues Spiel“ → Level 1-1 → spielen → Abziehen → START) und `m4_rumble.txt` (Optionen → Vibration aus/an) sind die Beispiele.

## 6. Wie weit der automatische Lauf ingame kommt

**Level 1-1 (Planet-Express-Hangar), frei spielbar.** Ablauf im Endlauf (`run\m4-final\level\m4_check\run_01.log`, RelWithDebInfo):

| Zeit | Frame | Ereignis |
|---|---|---|
| 0,2 s | 1 | Skript gestartet, Intro läuft ohne Eingabe |
| 14,1 s | – | Hauptmenü aktiv (Eintrag 0 „Neues Spiel“) |
| 15,2 s | 890 | Kontext Intro → Frontend |
| 15,6 s | 915 | Skript drückt A auf „Neues Spiel“ |
| 16,1 s | 934 | Kontext Frontend → Ingame (19 Frames nach dem Druck); kein Speicher- oder Profil-Dialog (8.11 E11) |
| 24,1 s | 1404 | Level 1-1 geladen |
| 26,8 s | 1567 | Fry steuerbar (Level-Filme enden sofort, M6) |
| 26,8–36,9 s | bis 2170 | Laufen, Kamera drehen, Springen, beide Angriffe, rückwärts laufen |
| 36,9 s | 2172 | simuliertes Abziehen: Pause von 0x2ACDB mit „Bitte Controller wieder am Controller-Anschluss 1 anschließen und mit START weiter.“ |
| 38,0–38,4 s | 2236–2263 | Wieder angesteckt, START schließt die Pause |
| 39,5 s | 2325 | `quit 0` |

Aus der Integrationsphase (`run\m4-integ`): ein 10-Minuten-Lauf mit Zufallseingaben (37 665 Ticks, 169 Tastendrücke, 21 Screenshots, ohne Absturz, Warnung oder Gast-Ausnahme; Fry läuft durch den Hangar bis ins obere Stockwerk, ein Kamera-Hinweis „Drücken Sie A, um die Kamera zu überspringen“ löst aus) und ein Level-Lauf mit D3D11-Debug-Layer und MSAA 4× (0 Fehler, 0 Warnungen). **Weiter als Level 1-1 kam kein Lauf:** Ziele des Levels, Tod/Zwischensequenz-Kontexte und Speicherpunkte (M7) sind ohne Kenntnis des Levelverlaufs nicht skriptbar.

Screenshots (angesehen):

- Endlauf: `run\m4-final\level\m4_check\run_01_level1-1_start.png`, `…_walk.png`, `…_jump.png`, `…_play.png`, `…_unplugged.png`, `…_resumed.png`
- Integration: `run\m4-integ\roam\sheet.png` (10-Minuten-Lauf, 21 Bilder), `run\m4-integ\roam\screenshots\frame_017217.png` (oberes Stockwerk), `run\m4-integ\diag\sheet.png`, `run\m4-integ\dbglayer\m4_check\run_01_level1-1_play.png` (Debug-Layer, MSAA 4×), `run\m4-integ\explore\screenshots\frame_002116.png`

### 6.1 Lücken im Level (für die nächsten Meilensteine, nichts davon blockiert)

| Lücke | Beobachtung | Zuständig |
|---|---|---|
| Render to Texture | Etwa 10 800 von 340 000 Shape-Draws je Level-Lauf gehen in Texturziele und sind gezählte No-ops; etwa 8 Szenen je Frame. Vermutlich fehlen deshalb die schwarzen `NiOutline`-Konturen der Figuren | P2 (Renderer) |
| Unterseite des Planet-Express-Schiffs tiefschwarz | In mehreren Kamerawinkeln. Ursache offen: Multi-Map-/Mehrpass-Texturen (nur die Basis-Map wird gezeichnet), erfasste, aber nicht angewendete Texture-Effects oder Beleuchtung ohne Umgebungslicht | P2/M8, Vergleich mit xemu |
| Partikel | Slot 52 `RenderPoints`, etwa 12 000 Aufrufe je Lauf, nicht gezeichnet | P2 |
| Texturen | 5 Texturen ohne Bild → Standardtextur 8×8; Purge-Slots 58/59/61/64 generische No-ops | P2 |
| Ton, Filme | Kein Ton (M5); Level-Filme `1-1-0n` enden sofort (M6) | M5, M6 |
| Spielverlauf | Level-Ziele, Tod, Zwischensequenzen, Speicherpunkte ungetestet | M7, M8 |

## 7. Prüfungen

Alle Läufe unter `run\m4-final\` (Logs `run\m4-final\checks\checks_*.txt`, `run\m4-final\checks_m4_*.txt`). **Der Fernseher war während der ganzen Endprüfung aus und abgemeldet:** Windows meldet den GDI-Platzhalter `WinDisc` mit 60 Hz, das Spiel meldet „display off“ und überspringt das Present. Deshalb laufen die anzeigeabhängigen Kriterien nicht (siehe unten); alle anderen bestehen.

| Prüfung | Aufruf | Ergebnis |
|---|---|---|
| Bauen Spiel | MSVC Debug `build\m4-final`, RelWithDebInfo `build\m4-final-rel`, clang-cl Debug `build\m4-final-clang`, alle mit `FUT_WERROR=ON` | ohne Warnungen (`run\m4-final\build_*.log`) |
| Generierte Dateien | `python tools\gen\gen_all.py --check` | aktuell |
| Bauen Launcher | Debug `build\m4-final-launcher`, Release `build\m4-final-launcher-rel`, `LAUNCHER_WERROR=ON`, mit ctest | ohne Warnungen; ctest **4/4** in Debug und Release (Unit-Tests, UI-Tests einschließlich der neuen Fälle `controlsReviewFixes` und `gameControlsLikeHost`, Screenshot-Lauf, CLI) |
| Unit-Tests `fut_tests` | Debug, RelWithDebInfo, clang | **367/367** (1 manueller Test übersprungen); neu: Rumble mit zwei verrauschten Pads, hochauflösendes Mausrad, Tastatur-Diagonale, Belegungs-Grenzfälle |
| `m4_check` | RelWithDebInfo `--runs 2` (mit Rumble-Lauf), Debug (mit Rumble-Lauf), clang `--no-rumble` | **ALL PASS** in allen drei Builds: M4-1 bis M4-6, M4-8, M4-9 (neu: fehlerhaftes Skript → Rückgabecode 1), M4-7 Rumble Spitze 65535/65535, Abklingen auf 0 in 60 Frames; 0 fehlgeschlagene `XInputGetState`, 0 Warnungen, keine undokumentierten VFS-Fehlzugriffe; in RelWithDebInfo liefen beide Läufe Tick für Tick gleich |
| Fehlerhaftes Skript von Hand | `build\m4-final-rel`, `run\m4-final\badscript` | Syntaxfehler und fehlende Datei: Rückgabecode 1, Zeile `process exit: input script syntax error (exit code 1 …)` bzw. `… not found …` (vorher 0xC0000409) |
| `m1_check` | Debug 3 Läufe, RelWithDebInfo 2 Läufe | **ALL PASS** |
| `m2_check` | Debug d3d11 und Null-Renderer | **ALL PASS**; T2 99,73 % der Frames genau ein Schritt, 59,94 Schritte/s (in Phase 3 noch 96,58 %, siehe 8.11 E16) |
| `m3_check` klassisch | Debug, RelWithDebInfo, clang; `--min-foreground 0 --min-vblank-frames 0 --min-on-plan 0` | M3-1 bis M3-4, M3b-5, M3b-6 und alle M2-Kriterien **PASS** (xemu: Legal 0,00, Tech-Notice 0,00, Ladebild 0,30, Frontend 0,01; Arbeit je Frame p99 3,4 ms). **M3b-7 FAIL** in allen drei Builds nur mangels Anzeige („no vblank-coupled on-screen statistics“, 3000 Frames „display off“) – wie in M3d Phase 5 |
| `m3c_check` | Debug und RelWithDebInfo, 7 Varianten | **ALL PASS**; X1 Legal 0,73, Tech-Notice 0,18, Ladebild 0,60, Frontend 0,99; X2 Verhältnis 0,48 |
| `m3d_check --display-optional` | RelWithDebInfo, 9 Varianten | **ALL PASS**: 49 PASS, 6 SKIP (D1 in i120/d120/f120, F1, M3b-7 in c60, L1: Anzeige `WinDisc` 60 Hz unter 120 Hz), 0 FAIL; Zwei-Tick-Mittelpunkttest p99 0,080 px |
| Launcher-Screenshots | `FuturamaLauncher -platform offscreen --screenshots=…` gegen `build\m4-final-rel` (1,0 hell und 2,25 dunkel) | Reiter „Steuerung“, beide Aufnahmedialoge und der Controller-Test korrekt ausgelegt (angesehen; Abschnitt 5.1) |
| Paket | `python tools\package.py --game-build build\m4-final-rel --launcher-build build\m4-final-launcher-rel` | `dist\FuturamaPC\`, 24 Dateien, 30,1 MB; `LIESMICH.txt` mit Abschnitt „Steuerung“ |

**Mit eingeschaltetem 120-Hz-Fernseher wiederholen:** `python tools\m3d_check.py --exe build\m4-final-rel\bin\futurama.exe --config run\m4-final\checks\base\futurama.ini --fresh` (ohne `--display-optional`) und `m3_check` ohne die `--min-*`-Schalter.

## 8. Review-Befunde

17 Befunde (5 mittel, 12 niedrig), jeder vor der Änderung nachgeprüft. **Alle angenommen und behoben;** abgelehnt wurde keiner. Teilweise umgesetzt sind nur optionale Vorschläge innerhalb der Befunde (unten jeweils „nicht umgesetzt“).

| # | Stufe | Befund | Nachprüfung | Behebung |
|---|---|---|---|---|
| 1 | mittel | Syntaxfehler oder fehlende Datei in `[debug] input_script` beendet den Prozess mit 0xC0000409 statt Rückgabecode 1 | Bestätigt im Code: `XppDevices::GetDevices` hielt `mutex_`, `ConnectedPorts` lud das Skript, `ExitHostProcess` lief den Exit-Reporter `LogInputSummary`, der `GetStats` und damit denselben Mutex sperrte | Skript wird in `StartInput` geladen (`script::PreloadScript`), vor jeder XPP-/Router-Sperre; zusätzlich fragt `XppDevices` den Router vor dem Sperren ab. Neues Kriterium **M4-9** in `m4_check` (Syntaxfehler und fehlende Datei → Rückgabecode 1, Fehlerzeile, geordnetes Ende): PASS in allen drei Builds |
| 2 | mittel | Rumble-Ziel „zuletzt benutzt“ über exakte Neutralität: ruhendes Stick-Rauschen eines zweiten Pads macht es zum Ziel | Bestätigt (`!state.Neutral()`) | Nur bewusste Eingaben (Taste, Analogtaste ab 30/255, Stick über 0,2) wechseln das Ziel. Test mit zwei verrauschten Pads |
| 3 | niedrig | Hochauflösende Mausräder: jeder Teil-Datensatz zählt als ganze Raste (bis 3 Drücke je Raste) | Bestätigt | Mausrad-Werte werden je Richtung aufsummiert, eine Raste je 120 Einheiten, Richtungswechsel verwirft den Rest (`WheelDeltaEvent`). Test: 8 × 15 Einheiten = 1 Druck |
| 4 | niedrig | Belegungs-Grenzfälle: Duplikate zählen gegen die Grenze 4; `key : a`; `f+1`/`vk_+1` im Launcher erlaubt | Bestätigt | Host und Launcher zählen nur verschiedene Tokens; Leerzeichen um `:` auf beiden Seiten erlaubt; `f<n>`/`vk_<xx>` im Launcher nur Ziffern. Gleiche Grenzfälle in `tests\input_bindings_tests.cpp` und `launcher\tests\tst_bindings.cpp` |
| 5 | niedrig | Header verspricht Determinismus Tick für Tick | Bestätigt (E14) | Text in `input_script.h`, `tools\input_scripts\README.md`, INTERFACES 4.27 korrigiert |
| 6 | niedrig | Tastatur-Diagonale W+D ergibt 1,0/1,0 (Länge 1,41), ein Pad höchstens etwa 0,71/0,71 | Bestätigt für den Rohwert; ob das Spiel diagonal schneller läuft, wurde **nicht im Level gemessen** | Vorbeugend: digitale Stick-Richtungen der Tastatur liegen auf dem Einheitskreis (23 170 je Achse), wie ein Pad an seinem runden Anschlag; Pad-Werte bleiben unverändert. Nicht umgesetzt: eine „Gehen“-Taste |
| 7 | mittel | Rechte Strg-/Alt-Taste wird im Launcher als linke belegt | Bestätigt anhand des Code-Pfads (`nativeScanCode & 0x100`) und der injizierten Eingabe des Reviewers | Der Dialog liest das Erweitert-Bit (lParam Bit 24) aus `WM_KEYDOWN`/`WM_SYSKEYDOWN` über einen nativen Ereignisfilter, Rückfall `GetKeyState(VK_RCONTROL/VK_RMENU)`. UI-Test speist eine echte `MSG`-Struktur ein (rechte Strg, AltGr). Nicht mit physischer Tastatur geprüft |
| 8 | mittel | Ohne Controller lassen sich Pad-Belegungen nicht ändern, der Dialog sagt nicht warum | Bestätigt (Warnung nur ohne XInput-DLL) | Hinweis „Kein Controller verbunden …“, Countdown wartet, Liste aller `pad:`-Eingaben mit „Übernehmen“; Aufforderung umformuliert („Knopf oder Trigger … Stick“) |
| 9 | mittel | Schieberegler zeigt den eingerasteten statt des eingestellten Werts, Berühren ändert still | Bestätigt (`updateLabel` aus der Reglerposition) | Wert außerhalb des Rasters wird genau angezeigt (`1,25×`, `60 % (ungültig)`) und bleibt, bis der Regler bewegt wird; Warntext „… das Spiel verwendet ihn trotzdem“. Nicht umgesetzt: logarithmische Skala für die Mausempfindlichkeit |
| 10 | niedrig | „Für das Spiel“ im Controller-Test nutzt 0,5 statt der Schwellen des Spiels, ohne tote Zone und Anschlussmodus | Bestätigt | `GameControlsForPad` rechnet wie der Host (Analogtasten ab 30/255, digitale ab 0,5, Sticks nach `pad_deadzone` außerhalb 0,4); bei `per_port` mit Anschlussnummer |
| 11 | niedrig | Launcher- und Host-Parser weichen in beide Richtungen ab (`vk_+f`, `key: space`) | Bestätigt (gleiche Ursache wie 4) | wie 4 |
| 12 | niedrig | F12, Druck und Alt+Eingabe sind Host-Tasten; Druck lässt sich nicht aufnehmen | Bestätigt (`window.cpp`) | Warnung an der Zeile; Druck wird beim Loslassen aufgenommen |
| 13 | niedrig | „Esc in Menüs als Zurück“ nennt Esc, wirkt aber auf jede START-Taste | Bestätigt | Bezeichnung „START-Taste der Tastatur geht in Menüs zurück“, Hilfe nennt die Belegung; Bedeutung der Tastatur-START-Zeile „Pause (in Menüs: Zurück)“, solange die Option an ist. Nicht umgesetzt: die aktuelle Taste dynamisch in die Hilfe einsetzen |
| 14 | niedrig | Kein Sicherheitsnetz: Zurücksetzen ohne Rückfrage, Klick ersetzt die ganze Liste, keine Warnung bei fehlender Pause-Taste | Bestätigt | Rückfrage bei mehr als einer Zeile (Reiter und Geräte), Dialog nennt die ersetzte Belegung, Warnung bei A/START/linkem Stick ohne Belegung. Nicht umgesetzt: „Rückgängig“ |
| 15 | niedrig | Begriffe und Texte (Rumble/Vibration, „Belegung(en) sind“, Gruppe „Steuerung“ im Reiter „Steuerung“, Fenstertitel bei „+“, „simulated“, „A *: K“) | Bestätigt | Alle sechs Punkte geändert („Vibration testen“, „Eine Belegung ist ungültig – …“, Gruppe „Eingabegeräte und Vibration“, „Belegung hinzufügen“, „Simulierte Controller …“, barrierefreier Name ohne Stern plus Beschreibung „geändert“) |
| 16 | niedrig | Controller-Test: leere große Rahmen, Fußnote ohne Bezug, ohne Controller nutzlos | Bestätigt | Nicht verbundene Anschlüsse als eine Zeile, Fußnote nur mit Controller. Nicht umgesetzt: Testfeld für Tastatur/Maus |
| 17 | niedrig | 225-%-Screenshot des Aufnahmedialogs abgeschnitten (nur offscreen) | Bestätigt | `adjustSize()` vor der Aufnahme; neuer Screenshot `controls_capture_pad`; Controller-Test nach der ersten Abfrage neu ausgelegt |

## 9. Nicht mit echter Hardware geprüft

Auf diesem Rechner ist kein Controller angeschlossen.

- **XInput-Eingabe**, Hotplug echter Pads, LB/RB- und Trigger-Gefühl, Stick-Rauschen (Befund 2 nur mit simulierten Werten), **Rumble an einem echten Gerät**. Das echte `xinput1_4.dll` ist nur bis „0 von 4 Controllern verbunden“ belegt (Launcher und Spiel); alles andere läuft gegen `XInputApi`-Doubles, simulierte Quellen, den `SimulatedPadReader` des Launchers und Unit-Tests.
- **Maus:** echte Mausbewegung, Gefühl von `mouse_sensitivity`, Mausfang im Fenster und im Vollbild, hochauflösende Mausräder (Befund 3 nur per Unit-Test).
- **Tastatur:** im Spiel nur per `SendInput` in der Integrationsphase (Menüs, Neues Spiel, Laufen, Springen, Pause). Im Launcher die rechte Strg-/Alt-Taste nur mit einer nachgebauten Windows-Nachricht im UI-Test, nicht mit physischer Tastatur; AltGr-Wartezeit und Tastatur-Griff nur offscreen.
- **Tastatur-Diagonale (Befund 6):** ob das Spiel diagonal schneller lief, wurde nicht gemessen; die Änderung macht die Tastatur einem Pad gleich.

## 10. Bekannte Probleme und offene Punkte

- **Takt auf diesem Rechner:** Die Anzeige-Kriterien (`m3_check` M3b-7, `m3d_check` D1/F1/L1/M3b-7 in c60) konnten nicht gemessen werden, weil der Fernseher aus und abgemeldet war (`WinDisc`, 60 Hz). In der Integrationsphase versuchte der Pacer zeitweise den Vblank-Plan und verfehlte T2/M3-3/C2 (96,3–96,6 %, gleich wie der Build vor M4); in der Endprüfung meldete das Spiel durchgehend „display off“, ließ das Present aus, und alle Takt-Kriterien ohne Anzeige bestanden (99,73 %). Mit angemeldetem 120-Hz-Fernseher wiederholen (Abschnitt 7).
- **Level-Lücken** (Abschnitt 6.1): Render to Texture, schwarze Schiffsunterseite, Partikel, fehlende Texturen – Renderer P2/M8.
- **`menu_escape`:** Esc wirkt im Kontext `Frontend` als BACK. Menüs, die ausdrücklich START verlangen (Menü-Code 6), wurden nicht gefunden, sind aber nicht ausgeschlossen.
- **Fortschritt über Level 1-1 hinaus** (Level-Ziele, Tod-/Zwischensequenz-Kontexte, Speicherpunkte, M7) ist ungetestet.
- **Pacer bei `WinDisc`:** Statt erst den Vblank-Plan zu versuchen (16 Planwechsel je 3000 Frames), könnte er beim GDI-Platzhalter sofort den Timer nehmen (M3b/M8).

## 11. Nächste Schritte

1. **M4b Freecam** auf dem Eingabe-Router: ein `InputConsumer`, der per `Divert` Anschluss 1 übernimmt (das Spiel sieht einen neutralen Controller), Tastatur/Maus über `KeyboardMouseSource::ReadRaw` (WASD/QE, Mausblick, Mausrad für das Sichtfeld), Gamepad über den umgeleiteten `PadState`; Kamera-Matrix vor dem Culling (`NiCamera::Click` 0x100C50). Damit lassen sich die offenen M3d-Punkte ingame messen (Kamera-Interpolation, Schnittregel, Kosten bei etwa 2000 Draws).
2. **Mit eingeschaltetem 120-Hz-Fernseher** die Takt-Kriterien wiederholen (`m3d_check` ohne `--display-optional`, `m3_check` ohne `--min-*`).
3. **Mit einem echten Controller** einmal den Controller-Test im Launcher, Rumble, Hotplug und zwei Pads gleichzeitig (Befund 2) prüfen.
4. **M5 Audio:** DSOUND auf XAudio2 (Xbox-ADPCM, 3D-Sound, I3DL2-Hall); der Level-Lauf aus `m4_check` ist dafür der erste Testfall mit Ingame-Ton.
5. **Renderer P2** für die Level-Lücken (Render to Texture zuerst, wegen der Konturen).
