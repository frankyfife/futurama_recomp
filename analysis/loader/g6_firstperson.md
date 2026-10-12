# G6: Ego-Steuerung – Machbarkeit und Umsetzung

Stand: 2026-10-12. Basis: Retail-EU-XBE (`orig\retail_eu\default.xbe`, Capstone), Ghidra-Export `analysis\retail_eu_export` und kurze Läufe des Ports (`build\g6-fp\bin\futurama.exe`, je 30–45 s, `[debug] movies = skip`, Läufe unter `run\g6_fp`, git-ignoriert). Bericht für Spieler: [`docs\g6_report.md`](../../docs/g6_report.md). Abweichungen vom Entwurf: [`docs\loader_design.md`](../../docs/loader_design.md) 8.20.

Adressen sind VAs der Retail-EU-XBE. `FUN_xxx` sind Ghidra-Namen. Funktionen, die nur über eine vtable erreicht werden, heißen nach ihrem Slot (z. B. `Player::vf+0x11C` 0x588B0). „Vermutung“ kennzeichnet Deutungen, die nicht direkt aus Binary oder Lauf belegt sind. Spieler-Typ `+0x774`: 1 = Fry, 2 = Leela, 3 = Bender (g4_select.md 4.2).

Wunsch des Nutzers: eine spielbare Steuerung aus der Egoperspektive, die das Original nicht hat. **Antwort: machbar, umgesetzt (experimentell).** Das Spiel hat schon eine Ego-Ansicht (R3), aber nur als Stand-Zielansicht. Sechs bedingte Probes machen daraus eine spielbare Ego-Steuerung.

## Inhalt

1. Kurzfassung
2. Was R3 im Original tut
3. Folgekamera, Kamera-Objekt und Bewegung (dritte Person)
4. Zielen, Angriffe, Zielerfassung, Springen
5. Was eine spielbare Egoperspektive braucht: Anforderungen und Ansatzpunkte
6. Umsetzung
7. Prüfung
8. Bewertung, Aufwand, Risiken
9. Offen / Vermutungen
10. Skripte und Läufe

---

## 1. Kurzfassung

* **Die Ego-Ansicht des Spiels** (R3, `Player::vf+0x11C` 0x588B0 → `FUN_00054DA0(1)`) schaltet den Kamera-Controller `[[0x2DABC4]+0x8C]` in **Modus 3** (`FUN_00018050`). Die Kamera sitzt dann auf Augenhöhe (Position der Figur + `[[player+0x10]+0xA0]`, Fry: 8 Einheiten). Die **eigene Figur wird ausgeblendet** (Bit APP_CULLED von `[player+0x564]+0xB0`). Der rechte Stick dreht und neigt die Ansicht (`FUN_00018250`, geglättet, Neigung ±1,5 rad). Der **Körper dreht sich jeden Tick zur Blickrichtung** (Ende von `FUN_00057940`). **Frys Waffen schießen entlang der Blickrichtung** (`FUN_00084E60`: Strahl vom Auge entlang der Kamerazeile 0, bis 1000 Einheiten), und es gibt ein **Fadenkreuz** (`CrossHairNode`, `FUN_00058F10`).
* **Aber:** Die Ansicht ist ein Stand-Zielmodus. Laufen (`Player::vf+0x100` = `FUN_00054AE0`) und Springen (`Player::vf+0x104` 0x50D10) tun nichts, solange `player+0xB88` gesetzt ist. Der linke Stick dreht die Ansicht mit (Aktionen 6/7). `FUN_00057940` verlässt die Ansicht, sobald die Figur in der Luft ist, angreift, getroffen wird oder ein Ziel erfasst (Bender/Leela). Bei Leela und Bender verschwindet das HUD, Bender sieht durch ein grünes Visier.
* **Bewegung in der dritten Person:** Der linke Stick gibt eine Wunschrichtung **relativ zur Kamera** (Zeile 0 = vorn, Zeile 2 = rechts der Kameradrehung, in die Ebene gelegt). Die Figur dreht sich mit begrenzter Rate dorthin und läuft **nur in ihre eigene Blickrichtung**, sobald sie weniger als 0,45 rad abweicht (`FUN_000536D0`). Seitwärtslaufen gibt es nicht (außer mit erfasstem Ziel).
* **Machbarkeit:** gut. Die Ego-Ansicht des Spiels liefert Kopfkamera, Ausblenden der Figur, Körperdrehung, Zielen der Waffen und Fadenkreuz. Es fehlen nur Laufen/Springen in der Ansicht, Seitwärtslaufen, das Halten der Ansicht in Luft und Angriff und eine direktere Blicksteuerung. Das sind genau sechs Funktionsanfänge (Abschnitt 5). Ein Host-Kamerabild wie bei der Freecam scheidet aus: Bewegung, Zielen und Körperdrehung des Spiels hängen an der Spielkamera.
* **Umgesetzt:** `[input] first_person = off|on` (Launcher, Reiter „Steuerung“, „Egoperspektive (experimentell)“) und `first_person_look_speed`. Mit `on` wird die Ego-Ansicht des Spiels spielbar und ist im Level von Anfang an aktiv. R3 (Tastatur V, mittlere Maustaste) schaltet wie im Original zwischen Ego- und Verfolgerkamera. Mit `off` wird **kein** Hook installiert (181 Hooks wie ohne G6).
* **Geprüft** (Abschnitt 7): 1-1 und 1-2 (Fry, mit Pistole), 2-1 (Bender), 3-1 (Leela), Tod in der Lava mit Wiedereintritt, Levelwechsel 1-1 → 1-2, Kombination mit G5 `camera_response = precise`. Gemessen: Bewegung entlang der Blickrichtung (Kosinus 1,000 ohne Wand), seitwärts 25 Einheiten/s wie vorwärts, Drehen 180°/s, Frys Schüsse 0–0,9° neben der Bildmitte.

## 2. Was R3 im Original tut

### 2.1 Umschalten: `Player::vf+0x11C` 0x588B0 (R3-Handler)

`FUN_00057940` (Eingabe des Spielers, jeden Tick, nur bei `+0x630 ≠ 0` und ohne Fahrzeug/Geschütz) setzt Bit 0x20 von `+0xBA0`, solange Aktion 20 (RS-Klick) gehalten ist, und ruft `vf+0x11C(+0xB28, +0xB2C)` (0x588B0, in allen fünf Spieler-vtables gleich; `m4_input.md` 5.1).

| Schritt | Adresse | Inhalt |
|---|---|---|
| Bedingung | 0x588B6–0x58931 | Bit 0x20 gesetzt, **nicht** `vf+0x12C` (Animationszustände 0x1B/0x21/0x1E/0x7C/0x76…, 0x50010, Vermutung: Angriffe), nicht `vf+0x140` (0x501A0, Zustände 0x1C/0x22/…), nicht tot (`vf+0xA8` 0x33D80: `+0x4F4 ≤ 0`), nicht `vf+0x138` (`+0xB8C/+0xB91/+0xB92`), nicht `vf+0x13C` (`+0xB8D/+0xB8F/+0xB90`), **am Boden** (`vf+0x70` 0x4FFE0), `+0xB8E = 0`, `+0x7A0 = 0`, Sperre 0x2DD7BC frei |
| Umschalten | 0x5893F / 0x5894F | `+0xB88 = 0` → `FUN_00054DA0(1)` (Rückkehr 0x58946), sonst `FUN_00054DA0(0)` (Rückkehr 0x58956); Sperre 0x2DD7BC = 1 bis zum Loslassen |
| Jeden Tick in Modus 3 | 0x58966–0x589D2 | Kameraposition = Position (`vf+0x34`) + `(0, [[+0x10]+0xA0], 0)` über `FUN_00011120` |

### 2.2 Ein und aus: `FUN_00054DA0(player, char on)`

* **Ein** (0x54DB5–0x54DF3): `FUN_00018050(camera, player)`. Hat die Figur ein Visier-Objekt `+0x7B4` (Bender), wird es mit `FUN_00045ED0(1)` eingeschaltet. Für Typ ≠ 1 (alle außer Fry) blendet `FUN_000E59D0([session+0x118], 0)` das HUD aus (Liste der HUD-Elemente, je `vf+0x14(show)`). Danach `+0xB88 = 1`.
* **Aus** (0x54E03–0x54F74): Die Drehung der Figur wird aus der Gierung der Kamera gebaut (`[cam+0x8C]+0x70`, Zeile 0 in die Ebene gelegt) und mit `vf+0x24` gesetzt. Dann setzt `FUN_00018FB0` die Kamera zurück in Modus 0, `FUN_000110E0(90°)` das Sichtfeld, Visier aus, HUD an und `+0xB88 = 0`.
* **Aufrufer** (8, Rohsuche): R3-Handler (2), automatisches Verlassen in `FUN_00057940` (0x587F3), Spieler-Update `FUN_0005A700` (0x5AE8D bei Kameramodus 7/8; Zustände 0xB/0xC/0xD 0x5AF8C/0x5AFFF/0x5B044, Vermutung: Absturz, Teleport, Tod) und `FUN_00091A60` (0x91C6B, Gegner packt die Figur, Vermutung).

### 2.3 Kamera-Modus 3: `FUN_00018050` (Eintritt) und `FUN_00018250` (Update)

* `FUN_00018050`: merkt den alten Modus (`+0x160`, Rückkehr über `FUN_00014150`). Setzt Position = Figur + Augenhöhe und Drehung = nur die Gierung der Figur. Setzt das APP_CULLED-Bit von `[player+0x564]+0xB0` (alter Wert in `+0x1C0`), dann `+0x15C = 3`. Alle Setzer anderer Modi (`FUN_00015040` Modus 1, `FUN_00014240` 6, `FUN_00014690` 7, `FUN_00018FB0` 0) stellen das Bit aus `+0x1C0` wieder her, wenn vorher Modus 3 aktiv war. Zwischensequenzen lassen die Figur also nicht unsichtbar.
* `FUN_00018250` (fastcall ecx = Kamera, `ret 4` mit dt; einziger Aufrufer der Verteiler `FUN_00019C80` 0x19CDB):
  1. Euler-Winkel der aktuellen Drehung `+0x70` mit `FUN_00103720`: Gierung `atan2(m02, m00)`, Neigung `−asin(m01)`.
  2. Eingaben `+0xB28` (Neigen: RS-Y + LS-Y, mal −1 bei der Option „Ziel umkehren“ 0x2E790A, `FUN_0005E020`) und `+0xB2C` (Drehen: RS-X + LS-X). Die kleinere Achse fällt weg, wenn sie unter 30 % der größeren liegt.
  3. Zwei Glättungen je Tick: `g = 0,75·g + 0,04·u` (begrenzt auf ±1,5·|u|, globale 0x2D9660/0x2D965C), `c = 0,5·c + 0,125·g` (`+0x190/+0x194`). Gleichgewicht 0,04 rad/Tick = **137,5°/s** bei 60 Hz.
  4. Gierung += `c_x`, Neigung = Neigung + `c_y`, begrenzt auf **±1,5 rad**. Neue Drehung `Rz(Neigung) × Ry(Gierung)` (`FUN_00103060`, `FUN_00103020`, Produkt `FUN_00013B30`). Zeilen: vorn `(cos p cos y, −sin p, cos p sin y)`, oben, rechts `(−sin y, 0, cos y)`.
  5. Position wie 2.1. Dirty-Bytes `+0xA1`, `+0xA3`.
* **Vorzeichen:** Positive Neigung blickt nach unten. Ohne „Ziel umkehren“ (Standard 0, `FUN_000A1AE0`) schaut **Stick nach oben also nach unten** (Fliegersteuerung). Lauf `orig`: LS vorwärts kippt die Ansicht zum Boden (`frame_001633`).

### 2.4 Jeden Tick in `FUN_00057940` (Eingabe des Spielers)

* 0x580D2: `+0xB88 = (Kameramodus == 3)`. Wechselt ein Skript die Kamera, verliert die Figur die Ansicht beim nächsten steuerbaren Tick ohne Aufruf von `FUN_00054DA0`.
* Nach `vf+0xEC/+0x100/+0x150/+0x104…+0x11C/+0x120`: Bei `+0xB88` wird die neue Drehung der Figur (`param_3`) aus der Gierung der NiCamera-Drehung `[[cam]+0x30]+0x30` gebaut (Zeile 0 in die Ebene gelegt). Der **Körper folgt also der Blickrichtung**.
* Danach das **automatische Verlassen** (0x587F3): wenn `vf+0x12C`, `vf+0x140`, tot, `vf+0x138`, `vf+0x13C`, **nicht am Boden**, `+0x7A0 ≠ 0` oder bei Typ 2/3 eines der Angriffs- oder Zielflags `+0x506/+0x507/+0x50A/+0x50B/+0x50C/+0xB98/+0xB97/+0xB96/+0xB95`.
* Sichtfeld je Tick: `FUN_000110E0(90°)`; für Fry mit Waffentyp 3 (Vermutung: Zielfernrohr) 45°. `FUN_000110E0` setzt `+0xAC` (Grad), `+0xA4 = tan(fov/2)`, `+0xA8 = 0,75·tan` und Dirty `+0xA0`. Bei Fry mit Waffentyp 3 halbiert das Spiel außerdem die Blickeingaben.

### 2.5 Was in der Ansicht gesperrt ist

| Funktion | Sperre | Folge |
|---|---|---|
| Laufen `FUN_00054AE0` (`vf+0x100`) | 0x54AF8: `+0xB88 ≠ 0` → Ende | kein Laufen |
| Springen 0x50D10 (`vf+0x104`) | 0x50DA1: am Boden **und** `+0xB88 = 0` nötig (sonst nur in Sonderzuständen `+0xB8C/+0xB8D`) | kein Springen |
| Benders Spezialangriff `FUN_00080520` (`vf+0xEC`, A+X mit Ladung `+0xAF8`) | `vf+0x148` (= `+0xB88`, 0x50AD0) | nicht in der Ansicht |
| LS | Aktionen 6/7 addieren LS-Y/LS-X zu Neigen/Drehen | LS dreht die Ansicht |

Lauf `orig` (`run\g6_fp\orig`, Skript `orig_r3.txt`, ohne G6): R3 → Kopfkamera ohne Figur (`frame_001517`); RX dreht (`…001549`); LY kippt nach unten statt zu laufen (`…001633`); A und B ändern nichts (`…001659`, `…001685`); R3 → Verfolgerkamera, Fry blickt in die letzte Blickrichtung (`…001751`).

## 3. Folgekamera, Kamera-Objekt und Bewegung (dritte Person)

### 3.1 Kamera-Controller `[[0x2DABC4]+0x8C]` (ctor `FUN_00019DB0`)

| Offset | Inhalt | Beleg |
|---|---|---|
| +0x30 | NiCamera (lokale Drehung +0x30, Translation +0x60) | Anwenden `FUN_00012210` |
| +0x44 | verfolgte Figur | `FUN_00018050`, `FUN_00018FB0` |
| +0x48 / +0xA1 | Position / Dirty | `FUN_00011120` |
| +0x54 / +0xA2 | Blickziel / Dirty (`FUN_000FFEB0` LookAt) | `FUN_00011150` |
| +0x70 / +0xA3 | Drehung, 3 Zeilen à 4 floats (vorn, oben, rechts) / Dirty | `FUN_00011180`, `FUN_00018250` |
| +0xA0, +0xA4, +0xA8, +0xAC, +0xB4 | Frustum-Dirty, tan(fov/2), 0,75·tan, Sichtfeld in Grad, far | `FUN_000110E0`, `FUN_00012210` |
| +0x15C / +0x160 | Modus / vorheriger Modus | `FUN_00019C80` |
| +0x1C0 | gemerktes APP_CULLED-Bit der Figur | `FUN_00018050` |

Modi (`FUN_00019C80`, `g5_camera.md` 3): 0 Folgekamera `FUN_00015990` (Federung in 0,01-s-Unterschritten, Kollision über die Strahlen `FUN_00011000`/`FUN_00011070`), 1 fest, 2 `FUN_00012460`, 3 Ego, 4 `FUN_00015110`, 5–9 Sonderkameras (Skript, Fahrzeuge).

**Anwenden** (`FUN_00012210`, aus `Session::Render` `FUN_000C8E90` 0xC917C, also nach allen Ticks eines Bildes): Bei `+0xA0` setzt es das Frustum `{−tan, tan, 0,75·tan, −0,75·tan, near 0,25, far +0xB4}` über `FUN_00011960`. Das hält `near ≥ max(0,1, far/10000)` (Konstanten 0x2D7734/0x2D7738). Bei `+0xA1` wird die Position nach NiCamera+0x60 kopiert, bei `+0xA3` die Drehung 1:1 nach NiCamera+0x30 (keine Transposition). Im Lauf ist `+0xA0` beim Kamera-Update gesetzt. Die Eingabe der Figur läuft also im Tick vor dem Kamera-Update (Spur `fov 11`).

### 3.2 Laufen: `FUN_00054AE0` (`vf+0x100`) und `FUN_000536D0`

* `FUN_00054AE0` (thiscall(player, float* v, float* rot) `ret 8`): Wenderate `dt·16` (Fry mit `vf+0x124`: `dt·2,67`). Am Boden oder in Sonderzuständen ruft es `FUN_000536D0(out, rate, rot)` (bei `+0xA00` die Variante `FUN_000534F0`). Danach normalisiert es die Bewegung und begrenzt sie auf 1. Das Tempo kommt aus den Schwellen `+0x9F8/+0x9FC` und den Def-Tempi `[+0x10]+0x70/0x78`. Ist der Winkel zur Blickrichtung der Figur größer als 120° (Skalarprodukt < −0,5), gilt das Rückwärtstempo `[+0x10]+0x74`.
* `FUN_000536D0` (einziger Aufrufer 0x54C00): **Wunschrichtung** = `LS-Y·Zeile0 + LS-X·Zeile2` der NiCamera-Drehung, beide in die Ebene gelegt (`+0x4D8/+0x4DC`, Ergebnis nach `+0xB1C`). Die Figur dreht höchstens `1,5·rate` rad je Tick zur Wunschrichtung (`FUN_00103020`, Ausgabe `rot`). Bewegt wird nur, wenn die Abweichung < **0,45 rad** (0x29A450) ist oder ein Ziel erfasst ist (`+0xB00`, dann Rate 120). Bewegt wird dann entlang der **eigenen** Vorwärtsachse (Zeile 0 der neuen Drehung) mal `|Wunsch|`. Dazu kommt ein Seitwärtsanteil `+0x4E0·Zeile2`, den `FUN_00057940` aber auf 0 setzt.
* Folge: Die Steuerung ist kamerarelativ, aber ein „Panzer“-Modell. In einer Ego-Ansicht mit fester Körperdrehung bewegt seitwärts gar nicht (Abweichung 90°).

### 3.3 Kollision der Kamera

Die Folgekamera schützt sich mit Strahlen gegen die Levelgeometrie (`FUN_00011000`: Maske 0x60 über `FUN_000F9CF0`, Strahl `FUN_000FB6F0(origin, dir, &dist, 1, 7)`; Distanz rein/raus). Die Ego-Kamera sitzt in der Figur. Ihre Kollisionsform hält Wände in Augenhöhe mindestens **1,9 Einheiten** entfernt (Spur `fp11`, Strahlen 1,90 beim Seitwärtslaufen an der Wand). Bei near 0,25 entsteht daher kein Durchblick. Der Port setzt near trotzdem auf 0,1 und hält zusätzlich 0,4 Einheiten Abstand (6.4).

## 4. Zielen, Angriffe, Zielerfassung, Springen

* **Fry, Schusswaffen** (`FUN_00084E60`, Bit `+0xB88`):
  * Ohne Ansicht: Richtung = Vorwärtsachse der Figur. Mit erfasstem Ziel (`+0xB00`) zielt Fry auf das Ziel `+0xB04` mit Vorhalt, Neigung begrenzt auf ±0,6.
  * **In der Ansicht:** Ursprung = Auge (Position + `[+0x10]+0xA0`), Richtung = Zeile 0 von `[cam]+0x70`. Ein Strahl `FUN_000FB6F0(…, 1000, 1, 1)` ergibt den Zielpunkt `+0x5D8`; ohne Treffer liegt er 20 Einheiten voraus. Ein erfasstes Ziel hat Vorrang, wenn es näher liegt. Danach folgt eine zufällige Streuung `+0xD60`. **Die Waffen zielen also nativ auf die Bildmitte.**
* **Fadenkreuz** (`FUN_00058F10`): Der Knoten `CrossHairNode` steht in der Ansicht 12 Einheiten voraus entlang der Blickrichtung. Sichtbar ist er nur mit Waffe und mit Ansicht oder erfasstem Ziel. Im Lauf `fp12` ist es der blaue Ring in der Bildmitte.
* **Leela/Bender, Nahkampf** (`vf+0x114/+0x118`: Leela 0x8A100/0x8A620, Bender 0x80F40/0x81140): Die Handler lesen `+0xB88` nicht. Angriffe gehen in die Körperrichtung (Animation, Vermutung: Trefferzonen an den Knochen). In der Ansicht ist das die Blickrichtung. Lauf `fp31`: Leelas Angriff geht auf das Skelett vor ihr (`frame_001629`).
* **Zielerfassung** (RT halten, LT wechseln; `FUN_000589E0` = `vf+0x150`, Zielsuche `FUN_00053D60`/`FUN_00054440`): Mehrere Stellen fragen `vf+0x148` (= `+0xB88`) und setzen in der Ansicht die Drehung zum Ziel nicht. Die Körperdrehung kommt in der Ansicht ohnehin von der Kamera (2.4). Fry schießt trotzdem auf das erfasste Ziel.
* **Springen** (B, `vf+0x104` 0x50D10): Sequenzprüfung `{0x18}`, Sprungimpuls über `[+0x5C]vf+0x50`, Merker `+0xB89`. Mit RT folgt Rolle oder Salto (`FUN_00061A70`).

## 5. Was eine spielbare Egoperspektive braucht: Anforderungen und Ansatzpunkte

| Anforderung | Original | Lösung / Ansatzpunkt |
|---|---|---|
| Kamera am Kopf, eigene Figur unsichtbar | Modus 3 (`FUN_00018050`), Augenhöhe aus dem Def, APP_CULLED | übernommen |
| Blick mit RS/Maus (Gierung + Neigung) | `FUN_00018250`, träge Glättung, 30-%-Achsensperre, LS dreht mit | **Probe `FUN_00018250`**: eigene Gierung/Neigung, eigene Kennlinie, Maus direkt |
| Körper folgt der Gierung | `FUN_00057940` (2.4) | übernommen |
| Laufen/seitwärts relativ zum Blick | gesperrt (2.5), „Panzer“-Modell (3.2) | **Probe `FUN_00054AE0`** (Sperrbit kurz löschen), **Probe `FUN_000536D0`** (Wunschrichtung direkt als Bewegung) |
| Springen | gesperrt | **Probe 0x50D10** (Sperrbit kurz löschen) |
| Ansicht bleibt in Luft, Angriff, Treffer | automatisches Verlassen 0x587F3 | **Probe `FUN_00054DA0`**: diesen einen Aufruf (Rückkehr 0x587F8) überspringen, außer bei Tod oder `+0x7A0` |
| Ansicht von Anfang an, nach Tod/Skriptkamera zurück | nur R3 | **Probe 0x588B0** (R3-Handler, jeden Tick): Eintritt mit den Prüfungen des Spiels |
| Schüsse/Angriffe in Blickrichtung | Fry nativ (4), Nahkampf über die Körperrichtung | übernommen |
| HUD bleibt | Leela/Bender: HUD aus, Bender: Visier | nach dem Eintritt HUD an (`FUN_000E59D0(…, 1)`), Visier aus (`FUN_00045ED0(0)`) |
| Kein Durchblick durch Wände | Kollisionsform ≥ 1,9 Einheiten, near 0,25 | near 0,1 (Minimum der Engine), 0,4 Einheiten Abstand per Kamerastrahl |
| Zwischensequenzen, Sonderlevel | Skriptkameras setzen eigene Modi | Eintritt nur aus Modus 0, ohne Zwischensequenz, ohne Pause; 1-6/2-5/4-1/5-3 gesperrt |
| Waffe/Arme sichtbar | Waffe hängt an der ausgeblendeten Figur | nicht umgesetzt (9) |

**Verworfen:** eine reine Host-Kamera wie die Freecam (`NiCamera::Click`-Probe). Spiellogik, Zielen und Bewegung hingen dann weiter an der Folgekamera hinter der Figur. Außerdem bliebe die Figur sichtbar, und Frys Schüsse gingen nicht zur Bildmitte. Die Kamera des Spiels selbst zu steuern ist kleiner und verträgt sich mit allem, was das Spiel um Modus 3 schon tut.

**Aufwand (Schätzung vorher):** 2–3 Tage für Hooks, Prüfläufe und Dokumentation, dazu Feinschliff nach Rückmeldung. Tatsächlich: ein Arbeitstag inklusive Analyse.

## 6. Umsetzung

### 6.1 Dateien und Hooks

`src\host\xapi\first_person.h/.cpp`, Tests `tests\first_person_tests.cpp`. Sechs `FUT_PROBE_IF` (Bedingung `[input] first_person = on`), Prologe mit Capstone geprüft. Es gibt keinen Sprung in die übernommenen Bytes (Rohsuche über alle Sprungziele):

| Probe | Adresse | Prolog | Wirkung |
|---|---|---|---|
| `probe:Player::SetFirstPerson` | 0x54DA0 | `55 8B EC 83 E4 F0` | Wunsch mitführen (R3 an: Rückkehr 0x58946, R3 aus: 0x58956), automatisches Verlassen (0x587F8) überspringen, außer tot oder `+0x7A0`; nach jedem Eintritt HUD an und Visier aus |
| `probe:Player::vfViewToggle` | 0x588B0 (neuer Eintrag in `tools\gen\extra_function_starts.tsv`) | `83 EC 24 56 8B F1` | Vor dem Original: automatischer Eintritt, wenn gewünscht, Modus 0, kein R3 gedrückt, steuerbar, keine Zwischensequenz, kein Sonderlevel, 15 Ticks nach einem Verlassen durch das Spiel und die Prüfungen von 2.1 |
| `probe:Player::Move` | 0x54AE0 | `55 8B EC 83 E4 F0` | Original mit `+0xB88 = 0` |
| `probe:Player::MoveDirection` | 0x536D0 | `55 8B EC 83 E4 F0` | nur innerhalb von `Move`: Bewegung = `LS-Y·vorn + LS-X·rechts` (Zeilen 0/2 von `cam+0x70`, in die Ebene gelegt), Drehung bleibt, `+0xB1C` gesetzt |
| `probe:Player::vfJump` | 0x50D10 (neuer Eintrag) | `83 EC 10 56 8B F1` | Original mit `+0xB88 = 0` |
| `probe:Camera::FirstPersonUpdate` | 0x18250 | `55 8B EC 83 E4 F0` | ersetzt das Update von Modus 3 (6.2–6.4); in Sonderleveln das Original |

Mit `off` wird keiner installiert. G5 hakt 0x15990 und 0x103020 ein, G7 nutzt die Filterkette des Routers. Es gibt keine gemeinsame Adresse.

### 6.2 Blick

* Gierung und Neigung als eigener Zustand. Sie starten bei jedem Eintritt aus der Kameradrehung, die `FUN_00018050` aus der Körpergierung setzt (Neigung 0). Geschrieben wird die Drehung `Rz(p)·Ry(y)` des Spiels (Unit-Test: gleich dem Produkt aus `FUN_00103060`/`FUN_00103020`/`FUN_00013B30`). Neigung ±1,5 rad wie im Original.
* **Rechter Stick:** Rohwerte des aktiven Controller-Objekts (`[player+0x7B8]` +0x124/+0x12C = Rohwert/32768). Darauf eine radiale tote Zone 0,15, linear bis 0,95, dann Kennlinie `|o|^1,6`. Tempo `first_person_look_speed` (Standard 180°/s). Bei G5 `camera_response = precise` kommen die Rohwerte des Controllers schon für die Spielkurve kodiert an; dann dekodiert die Spielkurve (`inp::GameStickOutput`), die Kennlinie bleibt dieselbe. Damit gilt in beiden Fällen die Ego-Kennlinie, ohne dass G5 und G6 gegeneinander arbeiten.
* **Maus:** rohe Zählschritte der Tastatur/Maus-Quelle (`ReadRaw`), 0,08°/Zählschritt mal `[input] mouse_sensitivity`. Solange die Ansicht läuft (Hook in den letzten 150 ms), übergibt `KeyboardMouseSource` die Mausbewegung nicht an den Stick (neue Abfrage `SetMouseMotionQuery`). Nach Pause oder Laden wird aufgelaufene Bewegung verworfen. Bei aktiver Freecam liest der Hook die Maus nicht.
* **Vorzeichen:** Stick wie im Spiel (`+` Gierung = rechts; Neigung mal −1 bei „Ziel umkehren“). Ohne die Option schaut Stick hoch also nach unten wie in der Ansicht des Originals. Maus: nach vorn = nach oben; die Option kehrt das um.
* **Zoom:** Bei einem Sichtfeld unter 60° (Fry, Waffentyp 3: 45°) halbiert sich das Drehtempo, wie im Original.

### 6.3 Bewegung, Sprung, Bleiben in der Ansicht

* `Move` läuft mit gelöschtem Sperrbit. Tempi, Rückwärtstempo, Luftsteuerung, Animation und Kollision bleiben Code des Spiels. Nur die Richtung kommt aus `MoveDirection` (Blick statt „Panzer“). Die Körperdrehung setzt das Spiel danach wie im Original auf die Blickgierung.
* Das automatische Verlassen in `FUN_00057940` wird übersprungen: in der Luft, bei Angriff, Treffer und Zielflags. Andere Ausgänge laufen normal: Tod (`vf+0xA8`), `+0x7A0`, Spieler-Update, gepackt werden, Skriptkameras, Zwischensequenzen. Danach tritt die Ansicht von selbst wieder ein (6.1).

### 6.4 HUD, Visier, near-Ebene, Wandabstand

* Nach jedem Eintritt: `FUN_000E59D0([session+0x118], 1)` für Typ ≠ 1 (HUD wieder an) und `FUN_00045ED0([player+0x7B4], 0)` (Benders Visier aus). Beim Verlassen schaltet `FUN_00054DA0(0)` beides wie im Original.
* Ist `+0xA0` im Hook gesetzt, setzt der Hook das Frustum selbst (`FUN_00011960`, near 0,1 = Minimum der Engine) und löscht `+0xA0`. Beim Verlassen setzt `FUN_000110E0` das Dirty-Byte wieder, dann gilt wieder near 0,25.
* Wandabstand: vier Kamerastrahlen (`FUN_00011000`, Länge 1, vorn/hinten/links/rechts in der Ebene). Liegt ein Treffer näher als 0,4, rückt die Kamera um die Differenz weg. Bei normaler Geometrie greift das nie (3.3), es schützt nur vor Sonderfällen.

### 6.5 Konfiguration, Launcher, Diagnose

* `[input] first_person = off|on` (Choice, Standard `off`, Reiter „Steuerung“, „Egoperspektive (experimentell)“) und `first_person_look_speed` (30–720°/s, Standard 180). Der Launcher baut die Zeilen aus dem Schema; der Screenshot liegt unter `run\g6_fp\launcher\shots\de_tab_controls_full.png`.
* Protokoll: `first person: on (automatic|R3|game …)`, `off (R3|game, return …)`, Summe `first person summary: …` (Exit-Reporter). Mit `[debug] trace = game` schreibt der Hook je Tick `first person trace: pos … yaw … pitch … fwd … right … move … stick … look … walls … push … fov … aim …`. Auswertung mit `scripts\g6_firstperson_trace.py`.
* Umschalten im Spiel: R3, also RS-Klick, Tastatur V oder mittlere Maustaste (Standardbelegung). Die Freecam-Kombination L3+R3 bleibt frei: Ein einzelnes R3 erreicht das Spiel bis zu 4 Ticks verzögert. Die G7-Folge besteht nur aus A und B.

## 7. Prüfung

Bau `build\g6-fp` (RelWithDebInfo). Läufe mit `[debug] movies = skip`, `[debug] trace = game`, `[input] first_person = on`, Skripte unter `run\g6_fp\` (`fp_play.txt` = `tools\input_scripts\g6_first_person.txt`). Alle Läufe endeten mit Rückgabe 0, außer dem ersten Versuch des Levelwechsels (Skriptfehler, siehe unten).

| Lauf | Level/Figur | Ergebnis | Bilder (`run\g6_fp\<Lauf>\shots\`) |
|---|---|---|---|
| `orig` | 1-1 Fry, ohne G6 | Ansicht des Originals: stehen, LS kippt, A/B ohne Wirkung | `frame_001517`, `…001633`, `…001751` |
| `fp11` | 1-1 Fry | Ansicht ab dem ersten steuerbaren Tick; vorwärts, seitwärts (an der Wand gleitend), drehen, auf/ab, rückwärts-diagonal, Sprung (y +6 Einheiten), R3 aus und wieder an | `frame_001545` (vorwärts), `…001607` (seitwärts), `…001656` (hoch), `…001956` (dritte Person), `…002022` (wieder Ego) |
| `fp12` | 1-2 Fry mit Pistole | Fadenkreuz in der Bildmitte, Schüsse (Munition 100 → 98) | `frame_001674`, `…001776` |
| `fp12shoot` | 1-2 Fry | 4 Schüsse in 4 Blickrichtungen: Zielpunkt `+0x5D8` 0,70° / 0,00° / 0,00° / 0,86° neben der Blickrichtung (12–55 Einheiten) | `frame_001393` … `…001568` |
| `fp21` | 2-1 Bender | volles Bild und HUD (ohne Visier), Laufen, Drehen, Angriffe, Zielerfassung, R3 | `frame_001503`, `…001565`, `…001976` (dritte Person) |
| `fp31` | 3-1 Leela | HUD bleibt, Laufen, Angriff auf ein Skelett vor ihr (Effekt im Bild), R3 | `frame_001331`, `…001629`, `…001741` (dritte Person) |
| `fp31lava` | 3-1 Leela | in die Lava: Tod → Verlassen durch das Spiel (Rückkehr 0x587F8, tot) → Neustart am Kontrollpunkt (Leben 5 → 4) → Ansicht von selbst zurück, mit HUD | `frame_001420` (Tod, dritte Person), `…002386` |
| `fplevelend` | 1-1 → 1-2 | `game level_end Level1-2` → Statistik → „Weiter“ → „Ja“ → 1-2 lädt, Ansicht von selbst | `frame_001695`, `…002452` |
| `fp11precise` | 1-1 mit G5 `precise` | beide aktiv (189 Hooks = 181 + 2 + 6), Folgekamera nur in der dritten Person (67 Updates) | `frame_001544` |

**Bewegung entlang der Blickrichtung** (`scripts\g6_firstperson_trace.py`, Kosinus zwischen Weg und Wunschrichtung je Tick):

| Lauf | vorwärts | seitwärts rechts | rückwärts-links | Tempo vor/seit/rück |
|---|---|---|---|---|
| fp11 (1-1) | 1,000 | 0,671 (Wand) | 1,000 | 25 / 13,3 (Wand) / 15 |
| fp12 (1-2) | 1,000 | 0,530 (Röhrenwand) | 1,000 | 25 / – / 15 |
| fp21 (2-1) | 0,817 (Hindernis) | 1,000 | 1,000 | 25 / 25 / 9 |
| fp31 (3-1) | 1,000 | 1,000 | 1,000 | 25 / 25 / 10 |

Ohne Hindernis ist der Kosinus 1,000. Kleinere Werte entstehen, wo die Kollision an einer Wand entlang gleiten lässt. Rückwärts gilt das Rückwärtstempo des Spiels. Drehen: 3°/Tick = 180°/s (Spur `fp11`).

**Weitere Prüfungen:**
* `[input] first_person = off` (Standard): `tools\m4_check.py --config run\verify-m5\futurama.ini` ALL PASS. Hook-Summe 181, keine `probe:Player::*`/`probe:Camera::FirstPersonUpdate`-Zeile und keine Zeile `first person`.
* Unit-Tests: 6 Fälle G6 (Parser/Schema, Drehung = Produkt des Spiels und Rückrechnung, Kennlinie inkl. G5-Dekodierung, Bewegung, Prologe und Aufrufstellen gegen die XBE, Maus/Stick-Weiche, Wandabstand). `fut_tests` bestanden in RelWithDebInfo, Debug (`FUT_WERROR=ON`) und clang-cl Debug.
* Der erste Versuch des Levelwechsels scheiterte am Skript (das Statistikmenü ist waagrecht, danach folgt die Frage „Weiter?“ mit „Nein“ vorausgewählt). Mit RIGHT, A, LEFT, A lief er durch.

## 8. Bewertung, Aufwand, Risiken

* **Spielbar** in normalen Leveln mit allen drei Figuren: laufen, seitwärts, springen, angreifen, schießen, Ziel erfassen, sterben und neu starten, Levelwechsel.
* **Risiken:**
  * Rätsel- und Sprungstellen sind auf die Verfolgerkamera ausgelegt. Abgründe vor den Füßen sieht man in der Ego-Ansicht schlechter. R3 schaltet jederzeit um.
  * Zwischensequenzen im Level wurden nicht im Lauf geprüft. Die Logik ist belegt (2.3: Skriptkameras setzen eigene Modi und stellen die Sichtbarkeit der Figur her; Eintritt nur ohne Zwischensequenz).
  * Gegner, die die Figur packen (`FUN_00091A60`), beenden die Ansicht; sie kommt danach zurück.
* **Aufwand bis hier:** ein Tag. Mögliche Erweiterungen in Abschnitt 9.

## 9. Offen / Vermutungen

* **Waffe/Arme sichtbar:** nicht umgesetzt. Die Waffe hängt (Vermutung) unter dem ausgeblendeten Wurzelknoten `[player+0x564]`. Sie allein zu zeigen hieße, statt der Wurzel nur die Körper-Meshes auszublenden und die Waffe an die Kamera zu hängen (eigenes Modell, eigene Animation). Das ist ein eigenes Arbeitspaket. Das Fadenkreuz des Spiels zeigt die Zielrichtung.
* **Benders Spezialangriff** (A+X mit Ladung) ist in der Ansicht gesperrt (`FUN_00080520` fragt `vf+0x148`). Ebenso bleiben `vf+0x148`-Abfragen der Zielerfassung original (4).
* **Zwischensequenzen und Skriptkameras im Level** sind nur aus dem Code bewertet, nicht im Lauf (8).
* Die Bedeutung von `vf+0x12C/+0x140` (Animationszustände) und `+0x7A0` ist Vermutung (Angriff/Treffer bzw. Fahrzeug/Tragen).
* Echte Maus und echter Controller wurden nicht benutzt. Die Mausweiche ist per Unit-Test geprüft, das Eingabeskript wirkt wie ein Controller.
* Level 4-1 (Ratte), 1-6 (Chickenwalker), 2-5 (Kanone) und 5-3 (Railgun) behalten die Kamera des Spiels. Die Ego-Ansicht des Originals ist dort, wo das Spiel R3 zulässt, unverändert.

## 10. Skripte und Läufe

| Datei | Zweck |
|---|---|
| `tools\input_scripts\g6_first_person.txt` | Prüfskript (Abschnitt 7) |
| `analysis\loader\scripts\g6_firstperson_trace.py` | Auswertung der Spur: Bewegung gegen Blickrichtung |
| `run\g6_fp\orig_r3.txt`, `fp_play.txt`, `fp_lava.txt`, `fp_levelend.txt`, `fp_shoot.txt` | Läufe (git-ignoriert) |
| `run\g6_fp\<Lauf>\futurama.ini`, `logs\futurama.log`, `shots\` | Konfiguration, Protokoll, Bilder je Lauf |

Disassembly-Helfer der Analyse (Capstone über `analysis\loader\scripts\m4_common.py`) lagen nur im Arbeitsverzeichnis. Die Fundstellen oben sind mit Adresse angegeben und mit dem Ghidra-Export nachprüfbar.
