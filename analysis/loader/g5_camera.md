# G5: Die Spielkamera am rechten Stick (Folgekamera, Trägheit, Variante „präzise“)

Stand: 2026-10-12. Nutzerwunsch: Das Drehen und Schwenken der Kamera mit dem rechten Stick fühlt sich „etwas indirekt (per design)“ an; gewünscht ist ein Launcher-Schalter zwischen „Original“ und einer „präziseren, wesentlich reaktionsfreudigeren“ Variante. Dieses Dokument belegt, wie das Spiel die Kamera aus dem Stick dreht, und beschreibt die Umsetzung `[input] camera_response = original|precise` (`src\host\xapi\camera_response.*`). Bericht mit Nutzer-Checkliste: [`docs/g5_g7_report.md`](../../docs/g5_g7_report.md).

Grundlage: Disassembly der Retail-EU-XBE (capstone), Ghidra-Export (`analysis\retail_eu_export`) und Läufe mit Kamera-Mitschnitt (`game camera_trace on`, Abschnitt 5). Adressen sind VAs der Retail-EU-XBE. „Tick“ = ein Logikschritt (60 Hz). Unbelegte Deutungen sind als **Vermutung** markiert.

## Inhalt

1. Kurzfassung
2. Vom Stick zur Kamera
3. Die Drehung der Folgekamera (`FUN_00015990`)
4. Umsetzung „präzise“
5. Messung: Original gegen präzise
6. Grenzen, offene Punkte
7. Skripte und Läufe

---

## 1. Kurzfassung

* **Weg:** Rechter Stick → Kanal-Stick 1 des Controllers (Totzone 0,4, Kennlinie `(|v|−dz)·1,3/(1−dz)`, `m4_input.md` 3.2) → Spieler `+0xB38` (X) / `+0xB3C` (Y) in `FUN_00057940` (Aktion 8, nur bei `+0x630 ≠ 0`) → Kameraobjekt `[[0x2DABC4]+0x8C]`, Modus `+0x15C = 0` → Folgekamera `FUN_00015990` (einmal pro Tick aus `FUN_00019C80` mit dt).
* **Korrektur zu `m4_input.md` 5.1:** `+0xB38/+0xB3C` gehen nicht nur an das leere vtable+0x120, die Folgekamera liest sie **direkt** (0x15A5C/0x15A78 und 0x1760C–0x17685). Die Aktionen 4–7 (`+0xB28/+0xB2C`, „Kamera neigen/drehen“) liest nur der Ego-Modus 3 (`FUN_00018250`).
* **Warum es sich indirekt anfühlt** – zwei hintereinander geschaltete Filter in festen 0,01-s-Unterschritten (2 je Tick):
  * Eingang `u = RX · (|RX| < 0,9 ? 0,25 : 1) · 35` (Rate `+0x178` = 35 aus dem Reset `FUN_00011380`), unter 0,01 → 0.
  * Stufe 1 `v = (u + v) · 0,91`, begrenzt auf ±2·35 = ±70 (`+0x154`).
  * Stufe 2 `w = (v · 0,001 + w) · 0,97` (`[+0xE4]+0xC`) – **Zeitkonstante 0,33 s**.
  * Winkel je Tick `Σ w · 0,01` (höchstens ±0,1 rad) → `NiMatrix3::MakeYRotation(−Winkel)` 0x103020 (Aufruf 0x17B2B) → Kamera kreist um die Figur, danach Kollision.
  * Folge (gemessen): Volle Drehrate (155,6°/s) erst nach etwa 0,8 s (90 % nach 0,63 s), bei halbem Ausschlag 50 % erst nach 0,27 s, nach dem Loslassen gleitet die Kamera noch 0,2–0,55 s und 6–18° weiter. Unter 0,9 Ausschlag dreht der Eingang nur mit einem Viertel, Stufe 1 sättigt aber ab 0,79 – die Kennlinie ist geknickt.
* **Präzise (neu, nur mit `precise`):** Zwei Probe-Hooks ersetzen nur den Drehwinkel des Ticks: proportional zum Stickwert des Spiels, höchstens 200°/s, Glättung 20 ms (90 % nach 33 ms), nach dem Loslassen Stillstand nach etwa 0,1 s. Kollision, Höhe, Abstand, automatisches Nachführen bei ruhendem Stick und alle anderen Kameramodi bleiben unverändert. Dazu bekommt der rechte Stick eines Controllers auf der Host-Seite eine kleinere Totzone (radial 0,18 statt 0,4 je Achse). Mit `original` wird nichts installiert.

## 2. Vom Stick zur Kamera

### 2.1 Eingabe

| Schritt | Ort | Beleg |
|---|---|---|
| XInput-Zustand → Stick 1 (`v = raw/32768`) | Controller-Update 0x9C3D0 | `m4_input.md` 3.2 |
| Kennlinie (Totzone 0,4 bzw. 0,2, wenn die andere Achse > 0,4; × 1,3/(1−dz), max 1) | `FUN_0009B600` (X) / `FUN_0009B630` (Y) | `m4_input.md` 3.2, `stick_curve.h` |
| Aktion 8 (Satz 1 = rechter Stick) → `+0xB38 = StickX`, `+0xB3C = StickY` | `FUN_00057940` (`param_1[0x2ce]`, `[0x2cf]`), nur bei `+0x630 ≠ 0` (Spieler nimmt Eingaben an) | Dekompilat `_text_00050000.c` |
| Aktionen 4/6 und 5/7 → `+0xB28` (neigen, Summe RS-Y + LS-Y, ×Invertierung „Ziel umkehren“) / `+0xB2C` (drehen) | ebenda | wird nur im Kameramodus 3 gelesen (`FUN_00018250`, 0x18250) und bei Modus 3 halbiert (Ego-Ansicht) |

Beobachtung: Ist die Figur nicht steuerbar, schreibt `FUN_00057940` `+0xB38/+0xB3C` nicht und es bleibt der letzte Wert stehen (kein Rücksetzer gefunden; **Vermutung:** das fällt im Spiel nicht auf, weil Zwischensequenzen eigene Kameramodi benutzen).

### 2.2 Kameraobjekt und Modi

Kameraobjekt `[[0x2DABC4]+0x8C]` (Session `0x2DABC4`, Spieler `+0x94`). Reset `FUN_00011380` (aus `FUN_00018FB0` und dem Skript-Native-Verteiler `FUN_000B0C40`):

| Feld | Wert nach Reset | Bedeutung (Beleg `FUN_00015990`) |
|---|---|---|
| `+0x48/+0x4C/+0x50` | – | Kameraposition (Ergebnis des Ticks, 0x17FD0…) |
| `+0x54/+0x58/+0x5C` | – | geglätteter Blickpunkt (`+= Schritt`), Kopie nach `+0x4/+0x8/+0xC` |
| `+0xE4` | Zeiger | Zustandsblock: `+0xC` Drehgeschwindigkeit w (Stufe 2), `+0x18` Zeitgeber „hinter die Figur schwenken“ (2,0 s), `+0x48…+0x84` Zeitmesser der Zustände (u. a. `+0x5C/+0x7C`: rechter Stick benutzt/nicht benutzt) |
| `+0x150` | 0 | Höhenversatz aus RS-Y (± 10) |
| `+0x154` | 0 | Drehgeschwindigkeit v (Stufe 1) |
| `+0x15C` | 0 | Modus |
| `+0x168` | 14,2 | Abstand |
| `+0x16C` | 5,89 | Grundhöhe |
| `+0x178` | **35,0** | Drehrate (Eingang u und Begrenzung von v) |
| `+0x180` | 50,0 | Rate des Höhenversatzes (RS-Y) |

Der Mitschnitt in Level 1-1 bestätigt `+0x178 = 35` und `+0x180 = 50` (Abschnitt 5).

**Update** `FUN_00019C80` (aus der Session `FUN_000C8BA0` mit dt, und aus `FUN_00019DB0`): zählt `+0x14C`/`+0x198` hoch und verteilt nach `+0x15C`:

| Modus | Funktion | Setzer / Deutung |
|---|---|---|
| 0 | `FUN_00015990` | **Folgekamera** (Normalfall im Level; Aufruf 0x19CCB, einziger Aufrufer) |
| 1 | – | steht (`FUN_00019DB0` u. a.; **Vermutung:** feste Skriptkamera) |
| 2 | `FUN_00012460` | **Vermutung:** Kamerafahrt |
| 3 | `FUN_00018250` | Ego-Ansicht (`FUN_00018050`, aus `FUN_00054DA0` = RS-Klick, `m4_input.md` 5.1) |
| 4/sonst | `FUN_00015110` | **Vermutung:** Skriptkamera mit Ziel |
| 5–9 | `FUN_00019180`, `FUN_00018C00`, `FUN_00014700`, `FUN_00014B00`, `FUN_000125B0` | Sonderkameras (Setzer 0x18xxx–0x19xxx; Chickenwalker, Kanone, Ratte haben eigene Steuerungen, `timing60.md` 4.8) |

Danach in jedem Modus `FUN_00012E40`, `FUN_000198E0` (**Vermutung:** baut die `NiCamera` aus Position und Blickpunkt) und das Kamerawackeln `FUN_000131E0`.

## 3. Die Drehung der Folgekamera (`FUN_00015990`, 9908 Byte)

### 3.1 Stickwert → Eingang (0x17608–0x176AC)

```
if |B38| > |B3C|:                                  ; 0x1760C..0x17627 (nur die größere Achse wirkt)
    x = B38; if byte[0x2D9440]: x = -x            ; 0x17629 Invertierung
    if |x| < 0.9 [0x293B68]: x *= 0.25 [0x293AB0] ; 0x1763E..0x1764B
    u = x * [+0x178]; if |u| < 0.01: u = 0        ; 0x17651..0x1766A
    damp = 1.0                                     ; 0x17672
else:
    h = [+0x180] * B3C * 8 [0x293B64]; if |h| < 0.01: h = 0   ; Höhe, 0x1767F..0x176A4
    damp = 1 - 0.1*sqrt|cos(Blickrichtung, Richtung zur Figur)|  ; 0x175DD..0x175FB
```

* `0x2D9440` liest nur diese Stelle, es gibt keinen Schreiber (rohe Referenzsuche im XBE: eine Fundstelle). Das Byte liegt hinter den Rohdaten der Sektion (Null): Die Invertierung der Drehung ist im Retail-Spiel **tot**. Die Option „Ziel umkehren“ (0x2E790A) wirkt nur auf `+0xB28` (Modus 3).
* Liegt RS-Y vorn, dreht der Stick nicht, er hebt/senkt die Kamera (`+0x150`); diagonale Eingaben wirken nur auf eine Achse.

### 3.2 Unterschritte (0x17720–0x179C4)

Die Schleife läuft, solange `t = dt; t -= 0.01; while (t > 0)` (dt = 1/60: **2 Unterschritte je Tick**, `timing60.md` 4.1/12.1). Je Unterschritt:

| Größe | Rechnung | Adresse | Konstante |
|---|---|---|---|
| v (`+0x154`) | `(u + v) · 0,91`, dann auf ±2·`+0x178` begrenzt | 0x1784B–0x178A6 | 0,91 @0x293B60, −2 @0x293B5C |
| f | `|v| > |auto| ? v : auto` | 0x178B0–0x178D4 | – |
| w (`[+0xE4]+0xC`) | `(f · 0,001 + w) · damp · 0,97` | 0x178D8–0x178FA | 0,001 @0x293A24, 0,97 @0x293B58 |
| Höhenversatz `+0x150` | `h ≠ 0 ? +0x150 + h·0,01 : +0x150 · 0,9` | 0x17920–0x17955 | 0,9 @0x293B68 |
| Winkel | `+= w · 0,01` | 0x1795B–0x17986 | 0,01 @0x293A0C |

* „auto“ (`uStack_19c` im Dekompilat) ist das automatische Nachführen: Kreuzprodukte zwischen Kamerarichtung und der Richtung zur Figur bzw. ihrer Blickrichtung (Dekompilat Z. 704–724, **Vermutung:** die Kamera dreht sich langsam hinter die Figur), dazu ±120 beim Schwenk hinter die Figur (Zeitgeber `[+0xE4]+0x18 = 2,0`, Z. 396/632/770–777). Solange der Stick schneller dreht (`|v| > |auto|`), ist das Nachführen abgeschaltet.
* Danach wird der Winkel auf ±0,1 rad je Tick begrenzt (0x179F9–0x17A25), mit `fchs` übergeben (0x17AFE) und `MakeYRotation` 0x103020 (`fld [esp+4]; fcos; fsin …`, `ret 4`) baut die Drehmatrix (Aufruf **0x17B2B**, Rückkehr 0x17B30). `FUN_00011770` (0x17B59) dreht die waagerechte Richtung Figur→Kamera; die neue Position ist Figur + Abstand · Richtung.
* **Kollision** danach (0x17C42–0x17F48): Strahl alte → neue Kameraposition `FUN_00011000` (0x17C42), Nachschieben `FUN_000129C0` (0x17CDF), Strahl Figur → Kamera `FUN_00011070` (0x17D6E, 0x17F08) mit Einzug auf 90 % des Treffers, erneut `FUN_000129C0`. Am Ende Position nach `+0x48…` (0x17FD0), Höhenversatz auf ±10 (0x17F70–0x17FC7).

### 3.3 Folgen in Zahlen

Gleichgewicht bei konstantem Stickwert d (Spielkurve) und Rate r = 35, 120 Unterschritte/s:

* `u = 0,25·d·r` (d < 0,9), `v = min(u·0,91/0,09, 2r)`, `w = v·0,001·0,97/0,03`, Drehrate `= w·0,01·120` rad/s.
* d ≥ 0,79: v sättigt bei 70 → **155,6°/s** (2,716 rad/s). d = 0,5: 98,4°/s, d = 0,433 (Stick roh 0,6): 85,2°/s.
* Anstieg: Stufe 1 sättigt bei vollem Ausschlag nach 3 Unterschritten; Stufe 2 nähert sich mit 0,97 je Unterschritt (τ = 0,33 s): 50 % nach 0,2 s, 90 % nach 0,65 s, 95 % nach 0,83 s. Nach dem Loslassen klingen v (τ 0,1 s) und w (τ 0,33 s) ab – die Kamera gleitet nach.
* Die Kennlinie: Stickrohwert 0,4 → 0, bis etwa 0,77 roh (d = 0,79) linear bis 155,6°/s, darüber konstant.

Das Modell steckt als `xapi::camera::OriginalSteadyTurnRate` in den Unit-Tests und passt zur Messung (Abschnitt 5).

## 4. Umsetzung „präzise“

`[input] camera_response = precise` (Launcher-Reiter „Steuerung“, „Kamera am rechten Stick“). Mit `original` (Standard) bleibt alles wie bisher: Die beiden Probes haben eine Installationsbedingung (`FUT_PROBE_IF`, `HookDesc::condition`), werden dann nicht installiert und nicht gezählt (Lauf: „hooks: 181 installed“ wie ohne G5; mit `precise` 183).

| Teil | Ort | Wirkung |
|---|---|---|
| `probe:Camera::FollowUpdate` 0x15990 (Prolog `55 8B EC 83 E4 F0` = `push ebp; mov ebp,esp; and esp,-16`, 6 Byte) | `camera_response.cpp` | Vor dem Original: Stickwert d = Spieler `+0xB38` (nur bei `+0x630 ≠ 0`; Vorzeichen wie das Spiel bei `0x2D9440`), Ziel = d · 200°/s, Glättung 1. Ordnung τ = 20 ms (`PreciseTurn`), Winkel des Ticks = Rate · dt. Nach dem Original, solange die präzise Drehung aktiv ist: v (`+0x154`) und w (`[+0xE4]+0xC`) auf 0, damit nichts nachgleitet und beim Übergang zurück ans Spiel nichts springt |
| `probe:NiMatrix3::MakeYRotation` 0x103020 (Prolog `D9 44 24 04 33 C0`, 6 Byte) | ebenda | Nur für den Aufruf aus der Folgekamera (Rückkehradresse 0x17B30) und nur während einer präzisen Drehung: Winkel = −(präziser Winkel). Die anderen 45 Aufrufer (Figuren, Objekte) gehen unverändert durch |
| Rechter Stick der Controller | `input_system.cpp` `PreciseRightStick`, `xinput_source.*` `SetPadShaper`, `stick_curve.h` `PreciseStickOutput` | Radiale Totzone 0,18, linear bis 1 bei Ausschlag 0,95, Richtung bleibt; kodiert über die Umkehrung der Spielkurve (`GameStickInverse`), sodass das Spiel genau diese Auslenkung liest (auch für RS-Y = Höhe und den Ego-Modus). Nicht, solange der Port umgeleitet ist (Freecam liest den rohen Stick). Maus und Eingabeskript sind nicht betroffen |
| Maus | – | Die Maus liefert dem Spiel schon die gewünschte Auslenkung exakt (`m4_input.md` 8.4, Umkehrung der Spielkurve). Mit `precise` wird daraus direkt eine Drehrate (Auslenkung × 200°/s), ohne Viertel-Knick und ohne Nachlauf |

Warum so:

* Der Winkel wird **an der Stelle ersetzt, an der das Spiel ihn anwendet**. Die Kollision (Strahlen, Nachschieben, Einzug) läuft danach mit dem Code des Spiels: Die Kamera bleibt wie im Original an Wänden hängen (Abschnitt 5: im Hangar nach 72° bei +RX).
* Das Spiel rechnet seine Filterkette weiter (auch die Zeitmesser „Stick benutzt“ `[+0xE4]+0x5C/+0x7C`), nur ihr Ergebnis wird verworfen. Damit bleibt das Nachführen bei ruhendem Stick (Schwenk hinter die Figur, langsames Ausrichten) wie im Original; während der Stick dreht, ist es im Original durch `|v| > |auto|` ohnehin aus.
* 200°/s bei vollem Ausschlag: Bei mittleren Ausschlägen entspricht die präzise Rate dem Gleichgewicht des Originals (d = 0,5: 100 gegen 98,4°/s), bei vollem Ausschlag ist sie ein Viertel schneller (200 gegen 155,6°/s) – das Original erreicht seine Rate nur verzögert, „präzise“ sofort.
* Kein Patch an Konstanten: 0,91/0,97/0,25 liegen in `.rdata` und werden zum Teil auch von anderen Funktionen gelesen (z. B. 0,9 @0x293B68 für die Höhe).

Unverändert: Höhe/Neigung über RS-Y (Dominanz der größeren Achse, Abklingen ×0,9), Abstand, Kollision, Kamerawackeln, alle Kameramodi außer 0 (Ego-Ansicht, Zwischensequenzen, Chickenwalker, Kanone, Ratte). Mit diagonalem Stick wirkt in „präzise“ der X-Anteil auch dann, wenn Y überwiegt (im Original nicht) – gewollt.

## 5. Messung: Original gegen präzise

Lauf `g5_camera.txt` (Level 1-1, kurz nach dem Start, `[debug] movies = skip`), RelWithDebInfo-Bau, 60 Hz. Gierwinkel aus Kameraposition und Blickpunkt des Kameraobjekts je Tick (`game camera_trace on`), ausgewertet mit `g5_camera_trace.py` (Drehrate = Differenz je Tick · 60).

| Stick (roh → Spielwert) | | Original | präzise |
|---|---|---|---|
| RX −1,0 / +1,0, 1 s | Rate am Ende (zweite Hälfte) | −142 / +144°/s (stationär 155,6 laut Modell, im Langlauf gemessen: siehe unten) | −197 / +200°/s |
| | 50 % / 90 % der Rate nach | 167 ms / 483 ms¹ | 17 ms / 33 ms |
| | gedreht in 1 s | −113° / +114° | −194° / +193° |
| losgelassen | steht (< 1°/s) nach / Nachlauf | 217–433 ms / 6–10° | 100 ms / 1,1° |
| RX ±0,6 → 0,433, 1,5 s | stationär | ±82,5°/s (Modell 85,2) | ±82–87°/s (Ziel 86,6) |
| | 50 % / 90 % nach | 267 ms / 650 ms | 17 ms / 33 ms |
| losgelassen | steht nach / Nachlauf | 383–400 ms / 5,6–6,2° | 33–83 ms / 0,1–0,5° |

¹ bezogen auf die Rate am Ende der Sekunde; bezogen auf den stationären Wert 155,6°/s: 50 % nach 0,18 s, 90 % nach 0,63 s (Langlauf unten, wie das Modell).

Langlauf `run\g5g7\long_*` (RX = −1,0 für 1,75 s, bezogen auf 155,6 bzw. 200°/s):

| | 50 % | 90 % | 95 % | stationär gemessen |
|---|---|---|---|---|
| Original | 183 ms | 633 ms | 817 ms | 155,1–155,3°/s ab etwa 1,2 s (Modell 155,6) |
| präzise | 17 ms | 33 ms | 50 ms | 200,0°/s |

In beiden Läufen hält nach −235° eine Wand die Kamera an (Kollision des Spiels, in beiden Varianten an derselben Stelle).

Der Messwert schwankt um ±3 %, weil der Blickpunkt `+0x54` eigens geglättet wird (die präzise Rate zeigt Spitzen bis 207°/s bei 200 Ziel). Erster Lauf mit +RX: Nach 72° hält die Kollision die Kamera an einer Wand des Hangars fest, w steigt weiter auf 2,2 – in beiden Varianten gleich (Kollision unverändert).

## 6. Grenzen, offene Punkte

* **Nicht mit echtem Controller und echter Maus geprüft** – die Kennlinie des rechten Sticks (Totzone 0,18) und das Mausgefühl müssen Nutzer bestätigen (Checkliste im Bericht).
* Nur die Folgekamera (Modus 0) wird präzise. Ego-Ansicht (Modus 3, `FUN_00018250`: eigener Filter `x·0,04 + y·0,75`, `timing60.md` 4.5), Chickenwalker (`FUN_000445A0`, Zielen 1,2 rad/s), Kanone und Ratte bleiben original (nur die kleinere Totzone des Sticks wirkt dort mit).
* Die Höhe (RS-Y) federt weiterhin nach dem Loslassen zurück (×0,9 je Unterschritt, τ ≈ 0,1 s) – Design des Spiels, nicht geändert.
* Die präzise Drehung ist an `+0x630` (Figur steuerbar) gebunden; ein stehen gebliebener Stickwert bei nicht steuerbarer Figur dreht im Original weiter, in „präzise“ nicht.
* **Vermutung:** Die Rate `+0x178` ist in allen Leveln 35 (nur der Reset `FUN_00011380` schreibt sie, der Skript-Native-Verteiler `FUN_000B0C40` ruft den Reset). Der Mitschnitt zeigt 35 in Level 1-1; andere Level ungemessen. Die präzise Rate hängt nicht davon ab.

## 7. Skripte und Läufe

| Datei | Inhalt |
|---|---|
| `analysis\loader\scripts\g5_camera_trace.py` | Auswertung der `camera trace:`-Zeilen (Gierwinkel entfalten, Rate je Tick, Phasen gleichen Stickwerts: stationäre Rate, 50/90 %, Stillstand und Nachlauf; optional CSV) |
| `tools\input_scripts\g5_camera.txt` | Sprungantwort in Level 1-1 (−1, +1, −0,6, +0,6, dazwischen losgelassen) |
| `run\g5g7\original`, `run\g5g7\precise` | Läufe zur Tabelle in Abschnitt 5 (`trace.csv`), `run\g5g7\long_*` Langlauf |
| `tests\g5_g7_tests.cpp` | Parser, Schema, `PreciseTurn`, Modell des Originalfilters gegen die Messwerte, Stick-Kennlinie über die Spielkurve, Prologe/Aufrufstellen/Konstanten gegen die Retail-XBE |
