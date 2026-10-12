# M4: Eingabepfad (XPP/XInput, Kanäle, Menüs, Rumble, Neues Spiel)

Stand: 2026-10-11. Statische Analyse der Retail-EU-XBE für M4 (ROADMAP: XInput, Tastatur/Maus frei belegbar, Rumble, Reiter „Steuerung“, geskriptete Eingabe). Grundlage: `avi.md` Abschnitt 4 (dort schon: 7 XPP-Einstiege, Kanaltabelle, Rumble-Grundzüge). Dieses Dokument prüft das nach, korrigiert zwei Punkte und ergänzt die Spielseite: Aktionstabelle, Menücodes, Rumble-Effekte, den Weg vom Hauptmenü bis Level 1-1 und Vorschläge für Belegung und Eingabeskript.

Es gab **keinen Lauf** des Spiels und **keinen echten Controller**. Alle Aussagen kommen aus Disassembly (capstone), Ghidra-Export und den Spieldaten auf dem ISO (nur gelesen). Laufzeitverhalten ist als **Erwartung** markiert, unbelegte Deutungen als **Vermutung**.

Adressen sind VAs der Retail-EU-XBE. „Tick“ = ein Logikschritt von `Game::Frame` 0xA2C20 (dt = 1/60 s mit M3b). `FUN_xxx` sind Ghidra-Namen; Code ohne Ghidra-Funktion heißt `nofunc_xxx` (Start geschätzt, siehe `m4_common.guess_start`).

## Inhalt

1. Kurzfassung
2. XPP-API-Vertrag pro Einstiegspunkt
3. Controller-Objekt, 16 logische Kanäle, aktiver Controller
4. Menüs und Kontexte
5. Spielsteuerung: Aktionstabelle und Wirkung pro Taste
6. Rumble
7. Neues Spiel bis Level 1-1 spielbar (mit Erkennung je Schritt)
8. Empfehlungen für die HLE und die Eingabeschicht (Router, Freecam)
9. Standardbelegung
10. Vorschlag Eingabeskript `[debug] input_script`
11. Offen, Vermutungen, nicht geprüft
12. Skripte und Ausgaben

---

## 1. Kurzfassung

* **XPP:** Das Spiel nutzt nur Gamepads, 4 Ports, Hotplug über `XGetDeviceChanges` pro Tick. `XInputGetState` wird einmal pro Tick und offenem Port gerufen, `XInputSetState` nur bei geänderten Motorwerten und nur, wenn `dwStatus != 0x3E5`. `hEvent` im Feedback schreibt das Spiel **nie**: Die HLE darf es nicht als Handle benutzen (Korrektur zum Stub-Kommentar „0 in the game“).
* **Kanäle:** 16 Kanäle (D-Pad 4, X/B/Y/A, LT/RT, START/BACK, Stick-Klicks, WHITE/BLACK) als float 0..1, Schwelle 30/256 (Klicks 1/256), dazu 2 Sticks mit Totzone 0,4 und Kennlinie `(|v|−dz)·1,3/(1−dz)`, wobei dz auf 0,2 halbiert wird, wenn die andere Achse über 0,4 liegt.
* **Aktiver Controller:** Ein einziger Spieler. Aktiv wird der erste Port mit einer Eingabe (Taste > 30/256 oder |Stick| > 0,4). Ingame bleibt er aktiv, bis er abgezogen wird (dann Pause und „Controller an Anschluss # anschließen“). Im Frontend und im Intro wird er nach 0,33 s Leerlauf wieder freigegeben. **Folge für den Port:** Tastatur/Maus und der Controller von Spieler 1 müssen auf **denselben** logischen Port.
* **Spielsteuerung:** Das Spiel hat eine eigene Aktionstabelle (33 Aktionen → Kanal/Stick, 10 Sätze). Aktiv ist fest Satz 1, nichts im Spiel schaltet ihn um. Freie Belegung gehört deshalb in den Host (physische Taste → Xbox-Taste), nicht in die Tabelle.
* **Belegung laut Code und Hinweistexten:** LS laufen, RS Kamera, A Angriff 1/Bestätigen/Überspringen, B Springen/Zurück, X Angriff 2, Y Benutzen, RT Ziel erfassen, LT Zielwechsel und Kamera (Vermutung), WHITE/BLACK Waffe zurück/vor (Fry), RS-Klick Ego-Ansicht, START Pause.
* **Rumble:** Pro Spielerfigur eine Warteschlange mit 10 Effekten (Stärke links/rechts, Dauer, Form konstant/Sinus/Anstieg/Abklingen), summiert und auf 1 begrenzt. Dazu direkte Aufrufe: Geschütz, Optionsmenü-Test (1,0 → 0 in 1 s), Wiederherstellen nach der Pause. Pause, Ladebild und Menüs schalten Rumble ab. Die Option „Vibration“ ist standardmäßig an.
* **Neues Spiel:** Im Hauptmenü ist „Neues Spiel“ vorausgewählt (Index 0). **A oder START** reicht. Danach laufen 2 Speicherprüfungen (Extras, freier Platz), die ohne Fehler keinen Dialog zeigen. 5 Ticks später wechselt der Kontext zu `Ingame` mit Level `Level1-1`. Es gibt weder Schwierigkeitsgrad noch Profilwahl. Filme 1-1-01/1-1-02 sind beim ersten Abspielen nicht überspringbar. Zwischensequenzen lassen sich erst nach 2,5 s mit A/START überspringen.

## 2. XPP-API-Vertrag pro Einstiegspunkt

Alle 7 Einstiege sind stdcall. Weitere Aufrufer gibt es nicht (`avi.md` 2.5/4.1, rohe E8-Suche in `m4_input_sites.out`). Struktur- und Feldnamen nach XDK.

| Einstieg | Signatur (ret) | Aufrufer (Stelle) | Argumente im Spiel | Rückgabe/Wirkung, die das Spiel auswertet |
|---|---|---|---|---|
| 0x28CFE1 → 0x28C3B5 `XInitDevices` | `(DWORD n, XDEVICE_PREALLOC_TYPE*)` (8) | Init `FUN_0009C2A0` (0x9C2D3), einmal aus `FUN_000D26A0` | `n=1`, `{&XDEVICE_TYPE_GAMEPAD=0x28BF10, 4}` (Stack-Lokale `local_14/local_10`) | keine |
| 0x28CFE6 `XGetDevices` | `(PXPP_DEVICE_TYPE)` → DWORD Portmaske (4) | Init 0x9C328 (Gamepad), 0x9C334 (MU) | Gamepad bzw. MU 0x28BFC0 | Gamepad: Bit i = Port i (nur Bits 0..3 geprüft, Schleife 0x9C340). MU: Ergebnis verworfen |
| 0x28D008 `XGetDeviceChanges` | `(type, DWORD* ins, DWORD* rem)` → BOOL (12) | Hotplug `FUN_0009C140` (0x9C15D), aus `CApp::vf10` 0xD25D0 (pro **Tick**) und aus der Disc-Fehler-Schleife 0xF3004 | Gamepad, Masken vorher 0 | FALSE → nichts. TRUE → pro Port i: **erst** Entfernung (Handle schließen, Werte zurücksetzen `FUN_0009BB50`, `+0x138 = 1` „getrennt“), **dann** Einstecken (altes Handle schließen, `XInputOpen`, zurücksetzen, `+0x138 = 0`). Beide Bits gleichzeitig = neu öffnen |
| 0x28CE5C `XInputOpen` | `(type, DWORD port, DWORD slot, XINPUT_POLLING_PARAMETERS*)` → HANDLE (16) | 0x9C1E9 (Hotplug), 0x9C383 (Init) | Gamepad, Port aus Tabelle 0x2D65A0 = {0,1,2,3} mit Index `ctrl+0x14C`, Slot 0, `NULL` | Handle nach `ctrl+0x150`. NULL heißt „kein Handle“: Der Port gilt trotzdem als verbunden (`+0x138 = 0`), liefert aber nie Eingaben |
| 0x28CEB2 `XInputClose` | `(HANDLE)` (4) | 0x9C197, 0x9C1C9, 0x9C363 | nur Handle ≠ 0 | keine |
| 0x28CEBE `XInputGetState` | `(HANDLE, XINPUT_STATE*)` → DWORD (8) | einzig 0x9C3E6 im Controller-Update 0x9C3D0 (vtable 0x2A14F0[0]) | Puffer auf dem Stack (`esp+8`) | ≠ 0 → nur zurücksetzen (`FUN_0009BB50`), der Port bleibt „verbunden“. 0 → auswerten (Abschnitt 3), danach `+0x138 = 0` (0x9C99F) |
| 0x28CF2A `XInputSetState` | `(HANDLE, XINPUT_FEEDBACK*)` → DWORD (8) | einzig 0x9C28C in `FUN_0009C220` (aus 0x9C996, nur bei Dirty-Flag `+0x148`) | `ctrl+0x154` | Rückgabe ignoriert. Gesendet wird nur bei `dwStatus != 0x3E5` (0x9C229) |

**Strukturen, wie das Spiel sie benutzt** (Offsets aus 0x9C3D0, Puffer ab `esp+8`; `m4_flow.out`):

| Struktur | Feld | Offset | Benutzt |
|---|---|---|---|
| `XINPUT_STATE` (22 Byte) | `dwPacketNumber` | +0 | nein |
| | `Gamepad.wButtons` | +4 | Bits 0x01 hoch, 0x02 runter, 0x04 links, 0x08 rechts, 0x10 START, 0x20 BACK, 0x40 LS-Klick, 0x80 RS-Klick (nur Low-Byte gelesen, `mov dl,[esp+0xC]`) |
| | `bAnalogButtons[8]` | +6..+13 | [0] A, [1] B, [2] X, [3] Y, [4] BLACK, [5] WHITE, [6] LT, [7] RT; je 0..255 |
| | `sThumbLX/LY/RX/RY` | +14..+21 | int16, Y positiv = oben |
| `XINPUT_FEEDBACK` | `Header.dwStatus` | +0 (`ctrl+0x154`) | Konstruktor setzt 0 (0x9C112), danach nur gelesen |
| | `Header.hEvent` | +4 (`ctrl+0x158`) | **nie geschrieben** (Suche nach `0x158]` in 0x9B000–0x9D000: kein Treffer). Das Objekt kommt aus dem Spielpool (`FUN_000A4900`, nicht genullt) |
| | `Header.Reserved` | +8..+0x41 | Das Original-XPP schreibt +0x40/+0x41 (0x28CF45/0x28CF52), das Spiel liest sie nicht |
| | `Rumble.wLeftMotorSpeed`/`wRightMotorSpeed` | +0x42/+0x44 (`ctrl+0x196/+0x198`) | `_ftol(float · 65535.0)` (0x2A14F8), bei abgeschaltetem Gate 0 |
| `XDEVICE_PREALLOC_TYPE` | `DeviceType`, `dwPreallocCount` | +0/+4 | `{0x28BF10, 4}` |

**Gerätetypen:** `XDEVICE_TYPE_GAMEPAD` 0x28BF10 und `XDEVICE_TYPE_MEMORY_UNIT` 0x28BFC0. Die Adressen werden nur verglichen, nie dereferenziert.

**Original-XPP zum Vergleich** (0x28CEBE, 0x28CF2A): `XInputGetState` hebt die IRQL an, kopiert Paketnummer und Report (Länge aus dem Typdeskriptor) **auch bei 0x48F** (Gerät entfernt) und liefert 0x57 bei ungültigem Handle. `XInputSetState` liefert 0x57 ohne Ausgabe-Fähigkeit und startet sonst einen asynchronen Transfer `FUN_0028D9BF`, der `dwStatus` später setzt.

**Vertrag für die HLE (4 Pads mit Hotplug):**

1. `XInitDevices`: Typen merken, sonst nichts.
2. `XGetDevices(Gamepad)`: Maske der logischen Ports mit Gerät. `XGetDevices(MU)`: 0. Danach gilt die gemeldete Maske als bekannt (keine zweite „Einsteck“-Meldung für dieselben Ports).
3. `XGetDeviceChanges`: Differenz seit dem letzten Aufruf (bzw. seit `XGetDevices`). Ab-/Anstecken zwischen zwei Aufrufen meldet beide Bits. Rückgabe TRUE genau dann, wenn eine Maske ≠ 0 ist.
4. `XInputOpen`: Für einen gemeldeten Port nie NULL liefern, Handle ≠ 0 und eindeutig pro Port.
5. `XInputGetState`: Sendet der Port gerade nichts (Freecam, Router), trotzdem 0 mit Neutralzustand liefern, nicht 0x48F, und auf keinen Fall eine Entfernung melden. Sonst pausiert Ingame (Abschnitt 4.4).
6. `XInputSetState`: Motoren an das Backend, `dwStatus` synchron auf `ERROR_SUCCESS`, `hEvent` ignorieren. Den Rückgabewert liest das Spiel nicht (`ERROR_SUCCESS` oder `ERROR_IO_PENDING` sind beide in Ordnung).
7. Zeitbasis: Die Abfragen laufen im Tick (`CApp::vf10` 0xD25D0 in der Schrittschleife von `Game::Frame`, `m3d_frameclock.md` Tabelle 0xA2DBD). Bis zu 6 Ticks pro Frame sind möglich. Eingaben werden deshalb pro `XInputGetState`-Aufruf abgetastet, nicht pro Bild (Abschnitt 8.3).

## 3. Controller-Objekt, 16 logische Kanäle, aktiver Controller

### 3.1 Objekt (0x19C Byte, `FUN_0009C2A0` legt 4 an, Array 0x2E6F84..0x2E6F93)

| Offset | Inhalt | Beleg |
|---|---|---|
| +0 | vtable 0x2A14F0 (abgeleitet; [0] Update 0x9C3D0, [1] Port lesen 0x9C3C0), Basis 0x2A14E4 | ctor 0x9BF50 / 0x9B7D0 |
| +4 + 16·k | Kanal k: `{float thr, float val, float prev, u8 consumed, u8 lock}` | 0x9B7D0, 0x9B7A0, 0x9B910, 0x9B930 |
| +0x104 + 0x18·s | Stick s: `{thrX, thrY, X, prevX, Y, prevY}` | 0x9B670, 0x9B380/0x9B3A0 |
| +0x134 | nächstes Objekt (Liste ab 0x2E6F94) | 0x9B7D0 |
| +0x138 | getrennt (1) / verbunden (0) | 0x9B780/0x9B790 |
| +0x13C | Leerlaufzeit in s, **nur aufaddiert** (bei > 7200 −3600), nie bei Eingabe auf 0 gesetzt | 0x9BEA5..0x9BECA; Schreibzugriffe nur dort und im ctor 0x9B88C |
| +0x140/+0x144/+0x148 | Rumble links/rechts (0..1), Dirty | 0x9B6B0 |
| +0x14C/+0x150/+0x154 | Port, Handle, `XINPUT_FEEDBACK` | 0x9BF50 |

### 3.2 Update pro Tick (0x9C3D0, Kanalbelegung)

| Kanal | Quelle | Wert | Schwelle |
|---|---|---|---|
| 0/1/2/3 | `wButtons` 0x04/0x08/0x01/0x02 (D-Pad links/rechts/hoch/runter) | 0 oder 1 | 30/256 |
| 4/5/6/7 | `bAnalogButtons` [2] X / [1] B / [3] Y / [0] A | n/256 | 30/256 |
| 8/9 | [6] LT / [7] RT | n/256 | 30/256 |
| 10/11 | START / BACK | 0/1 | 30/256 |
| 12/13 | LS-Klick / RS-Klick | 0/1 | 1/256 (Basis-ctor, nicht überschrieben) |
| 14/15 | [5] WHITE / [4] BLACK | n/256 | 30/256 |
| Stick 0/1 | LX,LY / RX,RY | v/32768 | Totzone 0,4 je Achse |

Schwellen: Der Basis-ctor 0x9B7D0 setzt 1/256 und 0,15, der abgeleitete ctor 0x9BF50 überschreibt 14 Kanäle mit 0x3DF00000 = 0,1171875 = **30/256** und alle Stickachsen mit **0,4** (0x3ECCCCCD). Ist ein Kanal belegt (Kanalwert ≥ Schwelle), ist ein Analogwert ≥ 30 gemeint. Für die Wahl des aktiven Controllers gilt dagegen streng > 30.

**Stick-Kennlinie** (`FUN_0009B3E0` X, `FUN_0009B480` Y): `dz = 0,4`. Liegt die **andere** Achse außerhalb ±0,4, gilt `dz = 0,2`. Ausgabe `o = sign(v)·min(1, (|v| − dz)·1,3/(1 − dz))` für |v| > dz, sonst 0. Voll ausgelenkt ist der Stick ab |v| = 0,862 (dz 0,4) bzw. 0,815 (dz 0,2). Die Werte wirken auf Laufen, Kamera und Menüs; die Menüs verlangen Ausgabe > 0,4, also Rohwert > 0,585.

**Abfragefunktionen** (thiscall auf dem Controller):

| Funktion | Bedeutung | Nutzer |
|---|---|---|
| `0x9B550` Held | val ≥ thr | `FUN_0002C890` (Debug/Kamera, Vermutung) |
| `0x9B580`/`0x9B5C0` Pressed/Released | Flanke gegen `prev` | `FUN_0002C890` |
| `0x9B8D0` HeldFree | Held und weder verbraucht noch gesperrt | Menüs, Spieler, Skript-Tasten |
| `0x9B990` PressedFree | Flanke, nicht verbraucht/gesperrt | Bestätigen/Zurück/Pause |
| `0x9B9D0` ValueFree | Wert, sonst 0 | Spieler (Analogwert von A/X/LT/RT) |
| `0x9B910` Consume | „verbraucht“, gelöscht sobald val < thr | Menüs nach Bestätigen (A/START) |
| `0x9B930`/`0x9B960` SetLock/DecLocks | Sperrzähler setzen/verringern | Skript-Tasten `FUN_000B7460` (sperrt 20), Session/Laden zählen herunter |
| `0x9B600`/`0x9B630` StickX/StickY | Kennlinie | Spieler, Menüs |
| `0x9BAC0` IsIdle | keine Taste frei gedrückt, |Stick| ≤ 0,25 | Leerlauf-Zeiten |

Folge für geskriptete Eingaben: Ein „Druck“ verlangt mindestens 1 Tick gedrückt und davor 1 Tick losgelassen. Nach einem verbrauchten Bestätigen muss A erst losgelassen werden.

**Tastenwiederholung** `MenuRepeat` (Objekt Pause+0x1C, `FUN_0009B340` pro Tick für den aktiven Controller; Periode 15 Ticks mit dem `.data`-Patch, `timing60.md` 4.7). Nur `FUN_0002A0D0` nutzt sie, aufgerufen von `FUN_000E4C90` (Menüs im Bereich 0xE1000–0xE4000). Das Hauptmenü nutzt sie nicht (4.2).

### 3.3 Aktiver Controller (0x2E6FA4)

* **Wahl** (0x9C3FD..0x9C63D): Solange 0x2E6FA4 = 0, wird der Controller aktiv, dessen Update als erstes eine Eingabe sieht (D-Pad/START/BACK/Klick-Bit, Analogtaste > 30/256 oder |Rohwert| > 0,4 auf einer Stickachse). Dabei wird auch 0x2E6FA0 = 1 gesetzt („es gab schon einmal einen aktiven Controller“). Die Liste läuft in Portreihenfolge 0..3.
* **Lesen:** `FUN_0009B690` liefert den aktiven Controller, sonst den von Port 0. Menüs (`FUN_00029FA0`) geben 0 zurück, wenn dieser Controller getrennt ist (`+0x138`).
* **Freigabe:**
  * Im Intro, wenn alle Pads zusammen > 0,33 s im Leerlauf sind (0x2B9F3..0x2BA06, 0x2E6F98).
  * Im Frontend-Render `FUN_00021B60` bei gesetztem Frontend-Flag +0x4C, wenn die aufaddierte Leerlaufzeit des aktiven Pads > 0,33 s ist. Weil der Zähler nie zurückgesetzt wird, gilt das praktisch in jedem Frame. Im Frontend kann deshalb jeder Port das Menü bedienen.
  * `FUN_000201B0` setzt nach den Speicherdialogen den gemerkten Controller zurück (0x20265; gemerkt bei 0x20760).
* **Ingame:** Keine Freigabe bei Leerlauf. Ist der aktive Controller getrennt (`+0x138 == 1`), öffnet die Session die Pause (0x2ACBF → `FUN_000A8220`). `FUN_0002A320` zeigt dann `ERRCTRLINGAME` mit „Port + 1“ (vtable[1] 0x9C3C0 liefert `+0x14C`). Nur `XGetDeviceChanges` mit Entfernung setzt `+0x138`. Fehlercodes von `XInputGetState` allein lösen das nicht aus.
* **Frontend ohne Controller:** `ERRCTRLFRONTEND` („Bitte den Controller anschließen.“) erscheint, wenn alle Pads getrennt sind **und** 0x2E6FA0 gesetzt ist (`FUN_00021B60`, `FUN_0009BA10`).

## 4. Menüs und Kontexte

Kontexte (`m2_report.md`): `Intro`, `Frontend`, `Attract`, `Ingame`, `Death`, `Intermission`. Jeder Wechsel steht im Log (`game: context A -> B`, Probe `Game::ContextSwitch`, B14).

### 4.1 Menü-Abfrage `FUN_00029FA0(code)` (aktiver Controller, sonst Port 0)

| Code | Bedeutung | Bedingung |
|---|---|---|
| 0 | hoch | HeldFree(D-Pad hoch) oder StickY(LS) > 0,4 |
| 1 | runter | HeldFree(D-Pad runter) oder StickY(LS) < −0,4 |
| 2 | links | HeldFree(D-Pad links) oder StickX(LS) < −0,4 |
| 3 | rechts | HeldFree(D-Pad rechts) oder StickX(LS) > 0,4 |
| 4 | bestätigen | PressedFree(A) **oder** PressedFree(START) |
| 5 | zurück | PressedFree(B) oder PressedFree(BACK) |
| 6 | nur START | PressedFree(START) |
| 7 | X gehalten | HeldFree(X) |

`FUN_0002A0D0` ist dieselbe Tabelle über `MenuRepeat` (Tastenwiederholung). Nutzer laut `m4_flow`/E8-Suche: Hauptmenü (0x205B0), Extras `FUN_00020B60`, Optionen `nofunc_00028960`/`nofunc_000290F0`, Pausemenü `FUN_000A74E0` (Codes 0–5; Zuordnung zur Pause ist Vermutung), Level-Ende „Breakdown“ `FUN_000A5940` (2/3/4) und Speicherdialoge (4).

Die Hinweistexte bestätigen das: `HINTS_001` „Press \x10 to start a new game“, `HINTS_EX_001` „Press \x11 to go back“, `HINTS_012` „Use \x0e \x0f to move selection“. Zur Glyphendeutung siehe 5.3.

### 4.2 Hauptmenü (vtable 0x2940D4, Update `[1]` = 0x20580)

* Einträge (Index `+8`, ctor 0x20810 setzt 0): **0 „Neues Spiel“** (`main_newgamenull`, `HINTS_001`), 1 Optionen (`HINTS_002`), 2 Spiel laden (`main_continuenull`, `HINTS_003`), 3 Extras (`HINTS_044`). Belege: `FUN_00020320`, `FUN_0001FF60`, `FUN_00020000`.
* Eingaben wirken nur bei sichtbarem Menü (`+4`, gesetzt von der Aktivierung `FUN_00020150` = vtable[8]). Nach jeder Cursorbewegung folgen 10 Ticks Sperre (`+0x14 = 0xA`, 0x206A6). Hoch/Runter laufen über „gehalten“: Ein Tipp von 1 Tick bewegt genau einmal.
* Bestätigen (Code 4, 0x206BA):
  * Abbruch, wenn ein Speicherdialog offen ist (`[[0x2EB310]+0xF4]+0xA`), `FUN_00029F80` ≠ 0 ist oder eine Kamerafahrt läuft (`[+0x34]+0x6C`).
  * Sonst wird A bzw. START verbraucht (0x20714..0x20729). Danach laufen die Prüfungen aus 4.3, zum Schluss `FUN_00020000(index)`.
  * Für Index 0: Sound `+0x3C` (`sounds/menu/select.wav`, geladen in `FUN_00020320`), Countdown `+0x48 = 5`. `FUN_00020090` zählt pro Update herunter und ruft bei 0 `FUN_000A30D0("Ingame")` (nächster Kontext) und `FUN_000A2340("Level1-1")` auf (Levelname in CApp+0xAC, 8 Zeichen).

### 4.3 Speicherdialoge beim ersten Bestätigen (Merker 0x2D9D44/48/4C)

| Reihenfolge | Bedingung | Dialogobjekt | Sichtbar nur bei | Text |
|---|---|---|---|---|
| a | `[0x2ED1B8] && ![0x2ED1B9]` | `FUN_000E4D60` → `FUN_000DE870` (Optionen laden) | Ladefehler | `MEMCARD_049` |
| b | immer beim ersten Bestätigen | `FUN_000E4E10` → `FUN_000DC630`/`0xDC6E0` (Extras laden) | Ladefehler | `MEMCARD_053` |
| c | nur „Neues Spiel“ | `FUN_000E5030` → `FUN_000DC210`/`0xDC3A0` (Platzprüfung) | zu wenig Platz (`MEMCARD_047`, **A = Xbox-Dashboard**, B = ohne Speichern weiter) oder Höchstzahl erreicht (`MEMCARD_050`) | `MEMHINT_006` |

`FUN_000201B0` führt die Kette nach jedem geschlossenen Dialog selbst fort. Mit leerem `partition1` und dem freien Platz der Host-Platte (`host_dir_device.cpp`, `GetDiskFreeSpaceEx`) ist **zu erwarten**, dass keiner der drei Dialoge sichtbar wird. Ein Skript darf hier **nie blind A drücken**: Bei `MEMCARD_047` startet A das Dashboard (`XLaunchNewImageA` → Prozessende, `loader_design.md` E5).

### 4.4 Ingame-Session (`nofunc_0002AAB0`, Update des Ingame-Kontexts)

Ablauf pro Tick:

1. **Level laden:** Ist ein Levelname gesetzt, lädt `FUN_0002A760` das Level und leert den Namen (0x2AB11..0x2AB24). Solange `FUN_000C3E20([0x2DABC4])` FALSE liefert (Aufruf 0x2AB2F), läuft nur das Laden (`FUN_000C8BA0`).
2. **Zwischensequenz** (`[[0x2EB7C8]+0x34]` ≠ 0, Manager `FUN_000D4DC0`):
   * Alle Sperren werden gelöst.
   * START muss einmal losgelassen worden sein (Merker 0x2DAD89). Dann löst PressedFree(START) oder PressedFree(A) `FUN_000D3120` aus.
   * Überspringbar ist die Sequenz, wenn sie nicht als fest markiert ist (+0x2D) **und** mehr als **2,5 s** seit ihrem Start vergangen sind. Bei 1-1-01.avi/1-1-02.avi (Tabelle 0x2D6D10, `FUN_000D5780`) gilt außerdem: erst ab dem **zweiten** Abspielen. 0x2EB7D4 ≠ 0 macht alles überspringbar.
   * Danach überspringt `FUN_000D5700` und gibt Rumble nach 1 s wieder frei.
3. **Pause:**
   * Ist der aktive Pad getrennt → Pause.
   * START muss erst losgelassen sein (Merker 0x2DAD88). Dann öffnet HeldFree(START) die Pause `FUN_000A8220`. Die Pause sichert die Motorwerte (`FUN_0009B760`) und schaltet Rumble ab.
4. **Auto-Pause:** Sind alle Pads zusammen **1800 s** im Leerlauf (0x2951D8), öffnet sich ebenfalls die Pause.

Im Frontend wechselt das Spiel nach etwa 60 s ohne Eingabe in den Attract-Modus (`m2_report.md`). Ein Skript sollte das Hauptmenü vorher bedienen.

### 4.5 Intro

`Intro::Update` (0x2B9B0): PressedFree(START) oder PressedFree(A) verbraucht die Taste und ruft `[0x2E7B84]`-vtable `+0x18` mit 0 auf. Dieselbe Methode nutzt `FUN_000D5700` beim Überspringen von Zwischensequenzen. **Vermutung:** Sie beendet den laufenden Film bzw. Intro-Schritt. Ohne Eingabe dauert Intro → Frontend bei 60 Hz 15,2 s (`loader_design.md` B46).

## 5. Spielsteuerung

### 5.1 Aktionstabelle (Spieler +0x7BC, `FUN_0005E220`)

Die Tabelle hat 10 Sätze zu je 0x21 Aktionen. Satz 0 ist aktiv: `FUN_0005DF80(n)` kopiert Satz n hinein und wird nur aus dem ctor mit n = 1 gerufen. Sätze 2/3 = 1, 5/6 = 4, 8/9 = 7. Satz 7 weicht von 1 und 4 nur bei Aktion 29 ab. `FUN_0005DF10(a)` liefert den Kanal bzw. Stick (0xFF = unbelegt). Der Spieler liest pro Tick den aktiven Controller nach +0x7B8 (`FUN_00057940`, 0x57951), aber nur bei `+0x630 ≠ 0`. Belege: `m4_controlmap.out`, `m4_actions.out`, Dekompilat von `FUN_00057940`.

| Aktion | Satz 1 | Leser (Ort) | Wirkung |
|---|---|---|---|
| 1 | LS Y | StickY 0x57A18 | Laufen vor/zurück (+0x4DC), plus D-Pad hoch−runter (Aktionen 10/11) |
| 2 | LS X | StickX 0x57AF9 | seitlich/drehen (+0x4D8), plus D-Pad rechts−links (13/12) |
| 3 | – | Ersatz RT−LT (14/15) | +0x4E0, danach auf 0 gesetzt: wirkungslos |
| 4 / 6 | RS Y / LS Y | StickY ×Invertierung (`FUN_0005E020`, Option „Ziel umkehren“ 0x2E790A) | Kamera neigen (+0xB28), Summe beider, begrenzt auf ±1 |
| 5 / 7 | RS X / LS X | StickX | Kamera drehen (+0xB2C), Summe beider |
| 8 | RS | StickX/StickY | +0xB38/+0xB3C an vtable+0x120 (bei allen Figuren leer 0xAED30) |
| 14 / 15 | LT / RT | `FUN_000589E0`, `FUN_00061A70` | RT gehalten = Ziel erfassen. Neuer Druck auf RT/LT = Ziel wechseln. Bewegung mit erfasstem Ziel |
| 20 | RS-Klick | HeldFree → Bit 0x20 | Ego-Ansicht umschalten (vtable+0x11C 0x588B6 → `FUN_00054DA0`) |
| 24 | B | HeldFree → Bit 0 → vtable+0x104 = 0x50D10 | **Springen** (Sequenzprüfung {0x18}). Mit RT: Rolle/Rückwärtssalto (`FUN_00061A70`, Geschwindigkeit ×1,4) |
| 26 | LT | Bit 1 → vtable+0x108 = 0x50E80 | Bei Druck im Kameramodus 0: Flag an der Kamera [Session+0x8C] (`FUN_000111A0`). **Vermutung:** Kamera hinter die Figur |
| 27 / 28 | WHITE / BLACK | Bit 2/3 → vtable+0x10C(0/1) | Fry: Waffe zurück/vor (0x85AE0, Sounds „weapons“/„select“, „ammo“). Bei anderen Figuren leer |
| 30 | START | Bit 4 → vtable+0x110 | leer (0xB0390). Die Pause steuert die Session (4.4) |
| 31 | A | ValueFree → vtable+0x114 (Wert 0..1) | **Angriff 1**: Leela Tritte (0x8A100, `Leela_RunKick` …), Fry Waffe/Hammer, Geschütz `FUN_00032FB0`, Chicken Walker `FUN_000445A0` |
| 32 | X | ValueFree → vtable+0x118 | **Angriff 2**: Leela Schläge (0x8A620, `Leela_JPL`/`SPL`), Bender Drehangriff (0x81140, `Bender_Spin`), Aufladen (Hinweise 055–061) |
| 25 | Y | kein direkter Leser | **Benutzen**, über Skript-Tasten (5.2) |
| 29 / 21 | BACK / LS-Klick | keine Leser | – |
| 9–13, 16–19, 22/23 | D-Pad bzw. unbelegt | nur als Ersatz, wenn Stickaktionen unbelegt wären (Ausnahme: 10–13 werden zu LS addiert) | – |

Figuren (vtable, A-/X-Handler): Basis 0x29A1E8 (0x4F910/0x4F9D0), Bender 0x29D8E0 (0x80F40/0x81140), Fry 0x29E1E8 (0x84B00/0x84BC0, WHITE/BLACK 0x85AE0), Leela 0x29F0C0 (0x8A100/0x8A620), unbekannt 0x2A0DB0 (Basis-Handler). Die Zuordnung stützt sich auf Strings in den Handlern (`Bender_Spin`, `Leela_*`, `weapons`). Bender und Fry selbst sind **Vermutung**.

Analogwerte: A, X, LT und RT gehen als Wert 0..1 in die Handler (Chicken-Walker-Laserstreuung, Geschütz). XInput-Tasten sind digital, das ergibt immer 1,0. Auf der Xbox wäre leichter Druck möglich gewesen. Das ist eine Abweichung ohne erkennbare Spielfolge (Vermutung).

### 5.2 Skript-Tasten

Level-Skripte (`scripts\<level>.ful`, Debug-Symbole in `.dbg`) kennen die globalen Tasten `g_tButtonSkip`, `g_tButtonUse`, `g_tButtonJump`, `g_tButtonAttack1`, `g_tButtonAttack2`, `g_tButtonStart` (`level1-1.dbg`). Der Wertetyp 4 (`FUN_000B7460`, vtable 0x2A2440) hält eine Aktionsnummer in +0xC, liest sie über die Aktionstabelle (HeldFree) und sperrt den Kanal danach 20 Einheiten lang für andere Leser (`SetLock(…, 0x14)`). Die Nummern stehen im Bytecode und sind nicht entschlüsselt. **Vermutung** nach Namen und Hinweisen: Use = 25 (Y), Jump = 24 (B), Attack1 = 31 (A), Attack2 = 32 (X), Start = 30, Skip = A.

### 5.3 Glyphen in den Hinweistexten (`language/hints_xbox.ldb`, `memhints_xbox.ldb`)

| Zeichen | Taste | Belege |
|---|---|---|
| \x10 | A | Menü bestätigen = A/START (4.1) ↔ `HINTS_001`. Leelas A-Handler = Tritte ↔ `HINTS_069/071` „\x10 … kick“. `MEMHINT_001` „Press \x10 to continue“ ↔ `ERRCDREADINGAME` „Press A to continue“ |
| \x11 | B | Springen = B ↔ `HINTS_076` „Press \x11 to jump“. Zurück = B/BACK ↔ `HINTS_EX_001` |
| \x12 | X | Leelas X-Handler = Schläge ↔ `HINTS_068`. Benders X = Drehangriff ↔ `HINTS_063` |
| \x13 | Y | Ausschlussverfahren. Benutzen/Hebel/Aufheben (`HINTS_024–029`, `035`, `047`) |
| \x0e \x0f / \x05 \x06 | hoch/runter bzw. links/rechts | Vermutung aus „move selection“/„adjust screen“ |

Die Schriftmetrik `fonts/latin1_xbox.fnt` hilft hier nicht weiter. Bis Zeichen 0x54 enthält sie keine gültigen Einträge (Datensätze ab 0x352: `{u16 h, u16 code, u16 x, u16 y, u16 w}`, davor Pixeldaten). Die Zuordnung oben beruht deshalb allein auf Code und Texten.

## 6. Rumble

**Weg:**

1. `SetRumble` `FUN_0009B6B0(pad, l, r, force)` speichert l/r im Bereich 0..1 und setzt Dirty, wenn sich ein Wert ändert.
2. Das ist wirkungslos, solange das Gate 0x2D659C = 0 ist und `force` = 0.
3. Am Ende des Updates sendet `FUN_0009C220`, wenn Dirty gesetzt ist. Bei geschlossenem Gate gehen die Motoren auf 0.
4. Ziel ist immer der **aktive** Controller (`FUN_0009B690` bzw. Spieler +0x7B8).

**Gate und Option:**

* `SetVibrationOption` `FUN_0009BA40(on, delay)` mit 13 Aufrufern.
* `on = 0` schließt das Gate sofort (Pause `FUN_000A8220`, Optionsmenü, Laden `FUN_000C8360`).
* `on = 1` öffnet es nach `delay` s. Gezählt wird in `FUN_0009BE10`, offen ist es dann nur, wenn die Option 0x2E7910 an ist.
* Die Option „Vibration“ ist standardmäßig 1 (`FUN_000A1AE0`, Restore Defaults) und wird über `FUN_000A1820` gesetzt.

**Effektwarteschlange der Spielerfigur** `FUN_0005EF70(l, r, dauer, form)` (thiscall auf Spieler+0xA04):

* 10 Plätze. Jeden Tick summiert `FUN_0005EFE0` alle Plätze, begrenzt auf 1, und ruft `SetRumble`.
* Formen: 0 konstant, 1 `sin(π·rest/dauer)`, 2 `1 − rest/dauer` (Anstieg), 3 `rest/dauer` (Abklingen).
* `FUN_0005EF60` schaltet stumm.

| Aufrufer (Stelle) | links/rechts | Dauer s | Form | Anlass (Strings, sonst Vermutung) |
|---|---|---|---|---|
| `FUN_00038880` (0x38B5C, 0x38D78) | 0,5/0,5 | 0,3 bzw. 0,5 | 0 | Wasser-, Lava-, Schlamm-, Ölspritzer (vorher je ein `FUN_00013130`, Vermutung: Kamerawackeln) |
| `FUN_0003C090` (0x3C5EA) | 0,5/0,5 | 0,2 | 0 | Treffer „SMACK!!!“ |
| `FUN_00042800` (0x4287A, 0x428EB) | 0/0,5 (beide Füße gleich) | 0,35 | 3 | Chicken-Walker-Schritte (`lfdown`/`rfdown`) |
| `FUN_000445A0` (0x4539F, 0x456B9) | 0,35/0,35; 1,0/0,5 bzw. 0,5/1,0 (je nach 0x2DCD18) | 0,15; 0,4 | 0 | Chicken-Walker-Laser bzw. Netz |
| `nofunc_00051520` (0x517A9, 0x51980, 0x519D4) | 0,5; 0,4; 0,6 (beidseitig) | 0,2; 0,3; 0,3 | 0 | vtable+0xEC Fry/0x2A0DB0 (Vermutung: Treffer/Landung) |
| `FUN_00051AD0`, `FUN_00052C20` | 0,5/0,5 | 0,3; 0,2 | 0 | – |
| `FUN_00051DA0` (0x51F64) | 0,4/0,4 | 0,15 | 3 | Spritzer |
| `FUN_0005A700` (0x5C1D5, 0x5C22A, 0x5C624, 0x5C943) | 0,5/0,5; 0,9/0,9 | 0,3; 0,2 | 0; 1 | Spieler-Update (Schaden/Landung, Vermutung) |
| Bender `FUN_00080690`, Fry `FUN_00082A20`/`FUN_000843D0`/`FUN_00084E60` | 0,5/0,5 bzw. berechnet | 0,3 | 0 | Raketen, Maschinengewehr, Waffen |
| `nofunc_000837D0` (0x83C47) | 0,9/0,9 | 0,8 | 0 | – |
| `FUN_00012E40` (0x12F85) | berechnet (beidseitig gleich) | berechnet | 0 | Vermutung: Skriptbefehl |

**Direkte `SetRumble`-Aufrufe** (16 Stellen, `m4_rumble.out`):

| Aufrufer | Werte |
|---|---|
| Geschütz `FUN_00032FB0` (`Gun_KA`) | `v = (x/0,3)²` beidseitig |
| Optionsmenü beim Einschalten der Vibration (0x297CF) | Test 1,0/1,0, fällt in 1 s linear auf 0 (0x28A59..0x28AE2), danach Gate zu |
| Ende der Pause `FUN_000A8190` | stellt die gesicherten Werte wieder her |
| übrige Stellen | 0/0 (stoppen). 0xB04B2 und 0xC22E6 mit `force = 1` (Stopp auch bei abgeschalteter Option) |

**XInput:** `wLeftMotorSpeed` = linker (tieffrequenter) Motor, `wRightMotorSpeed` = rechter, beide 0..65535 wie auf der Xbox. Die Werte lassen sich direkt durchreichen; eine Stärke-Einstellung im Launcher skaliert nur im Host.

## 7. Neues Spiel bis Level 1-1 spielbar

| # | Schritt | Eingabe | Erkennung |
|---|---|---|---|
| 1 | Start, Intro (Legal, Filme, Splash, Ladebild, Intro-Film) | keine (optional A/START je Schritt, 4.5) | `game: context Intro -> Frontend`, bei 60 Hz nach ≈ 15,2 s |
| 2 | Frontend lädt, Kamerafahrt, Hauptmenü wird aktiv | – | **Neu nötig:** Probe auf `FUN_00020150` (Aktivierung, `+4 = 1`, Hinweis `HINTS_001`). Ersatzweise feste Wartezeit (Erwartung: einige Sekunden; im Lauf messen) |
| 3 | „Neues Spiel“ ist vorausgewählt | A (oder START) tippen, ≥ 1 Tick | Merker 0x2D9D4C (und für Index 0 0x2D9D48) werden 1; Sound `sounds/menu/select.wav` (bis M5 ohne Ton, also kein Erkennungsmerkmal) |
| 4 | Speicherprüfungen Extras/Platz (4.3) | keine; Dialog sichtbar = Abbruch des Tests | `[[0x2EB310]+0xF4]+0xA` = 0, keine `MEMCARD_*`-Texte |
| 5 | Countdown 5 Ticks, `FUN_00020090` | – | `game: context Frontend -> Ingame` |
| 6 | Level laden | – | VFS-Trace (`--trace=vfs`): `scripts\Level1-1.Ful` (FutPAL01), `Levels\Level1-1\…` (`Level1-1.nif`, `defs.nif`, `ingame_dlg_<sprache>.nif`; FutPAL00), `SoundCache\Level1-1.nif`; Ende: `FUN_000C3E20([0x2DABC4])` = TRUE |
| 7 | Filme/Zwischensequenzen des Level-Skripts (`1-1-01.avi` … `1-1-03.avi` → `Video\1-1-0n-PAL.xmv`) | A nach > 2,5 s; die erste Wiedergabe von 1-1-01/02 ist nicht überspringbar | `[[0x2EB7C8]+0x34]`; Öffnen der `.xmv` im VFS-Trace. Bis M6 enden Filme sofort (XMV-Stub) |
| 8 | spielbar | Laufen (LS), Springen (B) usw. | Spieler `p = [[0x2DABC4]+0x94]` ≠ 0, Byte `[p+0x630]` ≠ 0, keine Zwischensequenz, Pause zu (`[[Kontext+0x14]]+0x24` = 0; Kontext = `this` des Session-Updates, Vermutung: CApp+0xBC), kein Speicherdialog |

Weitere Angaben:

* `Death` und `Intermission` kommen auf diesem Weg nicht vor. Ein Schwierigkeitsgrad und eine Profil- oder Speicherplatzwahl existieren nicht (`frontend.ldb` hat keine solchen Texte; der Ablauf in 4.2 hat keinen Zwischenschritt).
* `Language\L1A1-INT.ldb` lädt `FUN_000D9D90` (`files.md`, Tabelle Archive). Der genaue Zeitpunkt ist nicht geprüft.

## 8. Empfehlungen für die HLE und die Eingabeschicht

### 8.1 Schichten

```
Quellen (XInput 1.4 ×4, Tastatur, Maus, Skript)
   │  Abtastung pro Tick (in XInputGetState) / Raw-Input-Ereignisse
   ▼
Belegung (physisch → Xbox-Control, frei konfigurierbar, je Profil)
   ▼
Router  ──► Abnehmer „Freecam“ (M4b), „Debug“ …   (kann Quellen dem Spiel entziehen)
   ▼
4 logische Xbox-Ports (Zustand XINPUT_STATE, verbunden ja/nein, Motoren)
   ▼
XPP-HLE (7 Einstiege)  ◄── XInputSetState → Rumble-Ausgabe an die physische Quelle des Ports
```

* **Port-Modell:**
  * Port 0 = „Spieler“: der erste XInput-Controller, zusammengeführt mit Tastatur/Maus und Skript. Bei Tasten gilt ODER, bei Achsen die betragsgrößte Quelle.
  * Weitere XInput-Controller standardmäßig ebenfalls auf Port 0 (ein Spieler). Optional „je Controller ein Port“.
  * Begründung: Das Spiel hört Ingame nur auf den zuerst aktiven Port (3.3). Getrennte Ports für Tastatur und Controller würden dort einen der beiden stumm schalten.
* **Port 0 immer verbunden** (virtuelles Pad), solange Tastatur oder Skript aktiv sind. So greifen weder `ERRCTRLFRONTEND` noch die Reconnect-Pause, wenn kein Controller steckt. Hotplug echter Controller ändert dann nur die Quellen, nicht den Port. Hotplug-Meldungen nach außen nur, wenn wirklich ein Port entsteht oder verschwindet, etwa bei „je Controller ein Port“.
* **Router/Freecam:** Ein entzogener Port liefert weiter `ERROR_SUCCESS` mit Neutralzustand (Abschnitt 2, Punkt 5). Beim Zurückgeben gilt: Was beim Entziehen gehalten war, wird erst nach einem Loslassen wieder durchgereicht. Sonst sieht das Spiel eine falsche Flanke (`prev`).

### 8.2 XInput-Backend

* Die Strukturen passen 1:1 (2). Abbildung: `wButtons` Low-Byte = XInput-Bits; A/B/X/Y/LB(WHITE)/RB(BLACK) als 0/255; Trigger direkt; Sticks direkt.
* Rumble mit `XInputSetState` (`XINPUT_VIBRATION`). Beim Verlust des Fokus und beim Beenden auf 0 setzen.
* Nicht verbundene XInput-Slots nicht in jedem Tick abfragen, etwa nur jede Sekunde. **Vermutung:** Allgemein bekannt ist, dass `XInputGetState` auf leeren Slots teuer sein kann; im Projekt nicht gemessen.

### 8.3 Abtastung und Zeit

* Der Zustand wird beim `XInputGetState`-Aufruf für Port 0 einmal pro Tick eingefroren.
* Tastendrücke kürzer als ein Tick werden zwischengespeichert: Ein Ereignis „gedrückt + losgelassen“ zwischen zwei Abtastungen meldet 1 Tick gedrückt, im nächsten Tick losgelassen. Sonst gehen Tipps verloren, besonders bei mehreren Ticks pro Frame.
* Mausbewegung wird bis zur Abtastung aufsummiert.

### 8.4 Maus auf den Stick

Die Spielkennlinie hat eine große Totzone, die der Host ausgleicht. Gewünschte Auslenkung `o` (aus Mausgeschwindigkeit × Empfindlichkeit, begrenzt auf ±1) → Rohwert `sign(o)·(dz + |o|·(1 − dz)/1,3)·32767` mit `dz = 0,2`, wenn die andere Achse roh über 0,4 liegt, sonst 0,4 (3.2). Tastatur-Richtungen geben roh ±32767 aus, das ergibt Ausgabe 1,0. Optional „Gehen“-Taste mit halbem Rohwert.

### 8.5 Testbarkeit ohne Hardware

* Unit-Tests: Abbildung der Zustände, Kennlinie und Inverse, Hotplug-Folgen (`XGetDevices`/`XGetDeviceChanges`, beide Bits bei schnellem Umstecken), `dwStatus`-Vertrag und Latch kurzer Tipps.
* Laufzeit über ein injiziertes virtuelles Pad (Skript) und simuliertes Ab-/Anstecken mit Erwartung „Pause + `ERRCTRLINGAME`“.
* Das echte XInput-Backend lässt sich nur bis „kein Gerät“ (`ERROR_DEVICE_NOT_CONNECTED`) prüfen.

### 8.6 Konfiguration

* Belegungen im Schema als `[input]`-Schlüssel je Xbox-Control. Ziel ist die Xbox-Taste, nicht die Spielaktion (5.1: Die Spieltabelle ist fest, und die Menüs lesen die Kanäle direkt).
* Die Launcher-Beschriftung nennt beide, etwa „A – Bestätigen / Angriff 1 (A – Confirm / Attack 1)“.
* Weitere Schlüssel: Mausempfindlichkeit, Rumble an/aus und Stärke, Controller-Modus (ein Spieler / je Controller ein Port), `[debug] input_script`.
* „Y invertieren“ hat das Spiel selbst (Optionen „Ziel umkehren“). Der Host braucht dafür keinen eigenen Schalter.

## 9. Standardbelegung

### 9.1 Xbox-Controller (XInput) → Xbox → Spiel

| XInput | Xbox | Frontend/Menüs | Ingame |
|---|---|---|---|
| Linker Stick | linker Stick | Auswahl (> 0,4) | Laufen; trägt auch zur Kamera bei (5.1) |
| Rechter Stick | rechter Stick | – | Kamera |
| D-Pad | D-Pad | Auswahl | Laufen (zum Stick addiert) |
| A | A | Bestätigen | Angriff 1 (Tritt/Schuss/Hammer), Zwischensequenz überspringen |
| B | B | Zurück | Springen |
| X | X | – | Angriff 2 (Schlag/Drehangriff/Aufladen) |
| Y | Y | – | Benutzen (Skript-Tasten) |
| LB | WHITE | – | Waffe zurück (Fry) |
| RB | BLACK | – | Waffe vor (Fry) |
| LT (analog) | LT | – | Kamera/Zielwechsel (Vermutung) |
| RT (analog) | RT | – | Ziel erfassen (halten) |
| Start/Menu | START | Bestätigen | Pause; Zwischensequenz überspringen |
| Back/View | BACK | Zurück | – |
| LS-Klick | LS-Klick | – | – |
| RS-Klick | RS-Klick | – | Ego-Ansicht umschalten |

### 9.2 Tastatur/Maus → Xbox → Spiel

| Xbox | Tastatur | Maus | Begründung |
|---|---|---|---|
| LS ↑↓←→ | W S A D | – | Laufen |
| RS | – | Bewegung (Kennlinie 8.4) | Kamera |
| D-Pad | Pfeiltasten | – | Menüs; Ingame wie LS |
| A | Enter | linke Taste | Bestätigen / Angriff 1 / Überspringen |
| B | Leertaste | – | Springen; in Menüs „Zurück“ |
| X | F | rechte Taste | Angriff 2 / Aufladen (halten) |
| Y | E | – | Benutzen |
| RT | Umschalt links | – | Ziel erfassen (halten) |
| LT | Q | – | Kamera/Zielwechsel |
| WHITE / BLACK | 1 / 2 | Rad runter / Rad hoch (je 2 Ticks gedrückt) | Waffe zurück/vor |
| RS-Klick | V | mittlere Taste | Ego-Ansicht |
| LS-Klick | C | – | ohne Funktion, für Vollständigkeit |
| START | Esc | – | Pause (in Menüs: Bestätigen!) |
| BACK | Rücktaste | – | Zurück in Menüs |

Esc liegt bewusst auf START, weil nur START ingame pausiert (BACK hat ingame keinen Leser). Nachteil: In Menüs bestätigt START. Die Alternative ist ein Host-Schalter „Esc kontextabhängig“ (Kontext `Frontend` → BACK, sonst START, Kontext aus der Probe `Game::ContextSwitch`). Diese Entscheidung bleibt der Umsetzung überlassen.

## 10. Vorschlag Eingabeskript `[debug] input_script`

Das Format ist eine Textdatei, eine Anweisung pro Zeile, `#` leitet Kommentare ein. Zeiteinheit ist der **Tick**, gemessen an `XInputGetState`-Aufrufen für Port 0 (`t`); `s` = Sekunden Echtzeit, nur für Zeitlimits. Das Skript speist eine eigene Quelle auf Port 0 (8.1).

| Befehl | Wirkung |
|---|---|
| `press <taste> [n]t` | n Ticks halten (Standard 3), danach mindestens 1 Tick loslassen |
| `hold <taste\|achse=wert> … <n>t` / `release <taste>` | halten, Achsen roh in −1..1 (`LX=0 LY=1.0`) |
| `wait <n>t` | warten |
| `wait <bedingung> timeout <s>s` | warten bis wahr; Zeitüberschreitung beendet mit Fehler, Screenshot, Exitcode ≠ 0 |
| `loop timeout <s>s until <bedingung>` … `end` | wiederholen bis wahr |
| `expect <bedingung>` | sofort prüfen, sonst Fehler |
| `log "<text>"`, `screenshot <name>`, `quit <code>` | Protokoll, Bild nach `run\…\screenshots`, Ende |

**Bedingungen** (vom Host aus Proben bzw. Gastspeicher, 7):

| Bedingung | Quelle |
|---|---|
| `context <Name>` | Probe `Game::ContextSwitch` |
| `main_menu` | neue Probe `FUN_00020150` |
| `dialog` / `no_dialog` | `[[0x2EB310]+0xF4]+0xA` |
| `level_loaded` | Ergebnis von `FUN_000C3E20([0x2DABC4])`: `+0x58 ≠ 0`, dazu `+0x10 == 4` oder alle Einträge der Liste ab `+0x58` fertig (`FUN_000CE370`), dazu `+0x11C == +0x120 == 0`. Am einfachsten als Probe auf den Rückgabewert beim Aufruf 0x2AB2F statt eines Nachbaus |
| `cutscene` | `[[0x2EB7C8]+0x34]` |
| `paused` | 7, Schritt 8 |
| `controllable` | Spieler vorhanden, `+0x630`, `!cutscene`, `!paused`, `no_dialog` |
| `file "<pfad>"` | VFS hat die Datei geöffnet |

**Skript „Neues Spiel → Level 1-1“:**

```
# m4_newgame.txt - Frontend -> "Neues Spiel" -> Level1-1 spielbar (Port 0)
wait context Frontend timeout 40s        # Intro ohne Eingabe: ~15,2 s bei 60 Hz
wait main_menu timeout 20s               # Hauptmenü aktiv (Index 0 = "Neues Spiel")
wait 30t
expect no_dialog
press A 3t                               # Bestätigen (A oder START)
wait 10t
expect no_dialog                         # MEMCARD_047: A wäre "Dashboard" -> hier abbrechen statt raten
wait context Ingame timeout 20s          # Countdown 5 Ticks
wait file "scripts\Level1-1.Ful" timeout 30s
wait level_loaded timeout 180s
loop timeout 300s until controllable
  wait 160t                              # > 2,5 s: erst dann überspringbar (FUN_000D3120)
  press A 3t                             # Zwischensequenz überspringen (wirkungslos, wenn nicht erlaubt)
end
log "M4: Level1-1 spielbar"
screenshot level1-1_start
hold LY=1.0 120t                         # 2 s vorwärts
press B 3t                               # springen
wait 60t
screenshot level1-1_moved
quit 0
```

Zur Robustheit: Die Schleife drückt nur A. Gerät der Lauf unerwartet in die Pause, bestätigt A dort den ausgewählten Eintrag; Standard ist vermutlich „Spiel fortsetzen“, das ist nicht geprüft. `expect no_dialog` verhindert das Raten bei Speicherdialogen. Ab M6 (echte Filme) verlängern die nicht überspringbaren Filme 1-1-01/02 die Schleife um ihre Laufzeit. Das Zeitlimit von 300 s deckt das voraussichtlich ab, ist aber nicht gemessen.

## 11. Offen, Vermutungen, nicht geprüft

* **Nicht zur Laufzeit geprüft:** alle Abläufe aus 4 und 7. Dazu gehören die Zeit bis zum aktiven Hauptmenü, das Ausbleiben der Speicherdialoge, die Reihenfolge der Filme und Zwischensequenzen in Level 1-1 und ob der Spieler danach sofort steuerbar ist.
* **Keine Hardware:** XInput-Backend, Rumble-Gefühl und Hotplug echter Geräte. Zu prüfen sind sie, sobald ein Controller verfügbar ist.
* **Vermutungen:**
  * LT = Kamera hinter die Figur.
  * Nummern der Skript-Tasten (5.2).
  * Figurenzuordnung Bender/Fry, Klasse 0x2A0DB0 unbekannt.
  * Anlässe einzelner Rumble-Effekte.
  * Bedeutung von Frontend+0x4C.
  * Kontextobjekt+0x14 = Pausenobjekt.
* **Offen:** Warum auch der linke Stick über die Aktionen 6/7 auf die Kamera wirkt. Das ist die Datenlage in `FUN_00057940`; wie es sich anfühlt, zeigt erst ein Ingame-Test (M4).

**Korrekturen an bestehender Doku:**

* `avi.md` 4.2 nennt den Hotplug-Aufruf „pro Frame“. Richtig ist pro **Tick** (0xD25D0 in der Schrittschleife) und zusätzlich in der Disc-Fehler-Schleife (0xF3004).
* Der Stub-Kommentar in `src\host\input\xpp_stubs.cpp` (`XInputSetState`): „hEvent (0 in the game)“ ist nicht belegt. Das Spiel schreibt `hEvent` nie (2).
* Die Schwelle für die Wahl des aktiven Controllers ist streng > 30/256, die Kanalschwelle ≥ 30/256 (3.2). `avi.md` nennt nur „> 0.1171875“.

## 12. Skripte und Ausgaben

Alle unter `analysis\loader\scripts\`, Aufruf `python -I <skript>`. Die Skripte lesen nur. Spieldaten landen ausschließlich in `run\m4_input_analysis\` (git-ignoriert).

| Skript | Inhalt | Ausgabe |
|---|---|---|
| `m4_common.py` | XBE/capstone, Funktionen, rohe E8-Suche, Startschätzung für Code ohne Ghidra-Funktion, Dekompilat-Index | – |
| `m4_input_sites.py` | alle Aufrufe der Controller-Abfragen mit Kanalargument, nach Aufrufer und Kanal | `m4_input_sites.out` |
| `m4_controlmap.py` | nachgespielte Bytes von `FUN_0005E220`: 10 Sätze × 33 Aktionen | `m4_controlmap.out` |
| `m4_actions.py` | jede Abfrage der Aktionstabelle mit Aktion, Kanal und Leser | `m4_actions.out` |
| `m4_rumble.py` | `SetRumble`, `SetVibrationOption`, `AddRumbleEffect` mit Argumenten | `m4_rumble.out` |
| `m4_flow.py` | Disassembly der zitierten Stellen (Update, Hotplug, Hauptmenü, Session, Intro, Handler, Original-XPP) und Konstanten | `m4_flow.out` |
| `m4_ldb.py` | Texttabellen `language/*.ldb` (Format „0BDL“, UTF-16LE) vom ISO | `run\m4_input_analysis\*.ldb.txt` |
| `m4_extract.py` | einzelne Archivdateien (z. B. `scripts/level1-1.ful/.dbg`) und deren Strings | `run\m4_input_analysis\` |
