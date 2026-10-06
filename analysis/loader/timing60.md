# Zeitbasis 60 Hz: Timing-Semantik der EU-Version (Retail EU)

Ziel-Binary: `orig\retail_eu\default.xbe` (alle Adressen virtuell, Basis 0x10000). Vergleich: NTSC-Prototyp
`orig\proto\default.xbe` (17. Juni 2003). Belege: Ghidra-Export (`analysis\retail_eu_export`), Disassembly mit
capstone, Skripte `analysis\loader\scripts\timing_*.py` (Liste in Abschnitt 11, Ausgaben jeweils als `.out`
daneben). Funktionsnamen wie `Frontend::Update` sind Arbeitsnamen. Unsichere Deutungen sind als **Vermutung**
markiert. Der Quellbaum `src\` wurde nur gelesen.

Anlass: Der Port soll mit **mindestens 60 fps** laufen, das 50-Hz-PAL-Timing ist unerwünscht. Auf dem
Messrechner (120 Hz, VSync) erreicht der 50-Hz-Takt nur 49,2–49,3 Hz, weil 50 Hz kein Teiler von 120 Hz ist
(m3_report 7). 60 Hz sind genau zwei Vblanks.

---

## 0. Kurzfassung

| Frage | Ergebnis | Beleg |
|---|---|---|
| Lässt sich die EU-Version auf die NTSC-Zeitwerte umstellen? | **Ja.** Fünf Operanden-Patches im PAL-Zweig von `Game::Init` 0xA26A0 ergeben exakt den NTSC-Zeitzustand. Das Tag bleibt „PAL“, CApp+0xE8 bleibt 0. Damit bleiben PAL-Archive, Sprachwahl, PAL-Filme und PAL-Splash/Legal erhalten. | 2, `timing_patch.out` |
| Teilt der Compiler Konstanten? | Ja. fps und dt werden per `fld` aus dem gemeinsamen `.rdata`-Pool geladen (50.0f @0x2955AC: 9 Nutzer; 0.02f @0x2942C0: 13 Nutzer). Gepatcht wird deshalb der **Operand** (disp32 → 0x2942CC = 60.0f bzw. 0x29E390 = 1/60), nie die Konstante. Der gemeinsame Schluss 0xA274E–0xA276A bleibt unverändert. | 2.2 |
| Ist das Spiel-Timing bei 60 Hz korrekt? | **Ja, im Sinne der NTSC-Fassung.** Bewegungsintegration, Timer, Animationen (NetImmerse), Sound-Fades und Skript-Wartezeiten rechnen in Sekunden (dt bzw. QPC). Tick-gebunden sind UI- und Kameradetails, aber nach dem Review (Abschnitt 12) auch drei spielrelevante Stellen: die Starrkörper-Dämpfung (verschiebbare Objekte, Wippen, Minen: rund 17 % kürzeres Ausrollen), das Zielen und Drehen des Chicken Walker (20 % schneller) und Skriptschleifen mit kurzen Waits (−17 % bis +11 % je Durchlauf). Alles läuft bei 60 Hz exakt wie in der NTSC-Fassung (gleicher Code, gleiche Skripte); diese Parität ist bewusst akzeptiert (12.2). | 4, 12 |
| Belegt der NTSC-Prototyp das? | Ja. 3112 von 3116 Spielfunktionen sind bis auf verschobene Adressen identisch, alle 19 timing-relevanten Funktionen samt `.rdata`-Konstanten ebenso. 21 von 23 Level-Skripten sind byte-identisch mit den PAL-Skripten. Derselbe Code mit denselben Daten lief im Prototyp mit 60 Hz. | 5 |
| Was ist [0x2E7B68]? | Wird nur geschrieben (PAL 1/25,4, NTSC 1/30,5) und nirgends gelesen, also wirkungslos. **Vermutung:** Rest einer Bildzeit für 25/30-fps-Video. | 3 |
| Fester Zeitschritt bei 60 Hz | Unkritisch: Kappung 6·dt = 0,1 s, max. 100 Schritte (praktisch ≤ 6), Schrittrate 60,00024 Hz bei QPF 10 MHz. Ohne Interpolation zeigt jeder Frame genau einen Simulationsstand. Der Host sollte im 60-Hz-Raster präsentieren (bei 120 Hz: Sync-Intervall 2). | 6 |
| Was muss der Host noch beachten? | Den `.data`-Anfangswert von fps (0x2D6744) ebenfalls auf 60.0f setzen, sonst wiederholen Menütasten alle 12 statt 15 Ticks (siehe 4.7). Die Prüfskripte `m2_check`/`m3_check` rechnen mit Frame-Nummern bei 50 Hz; Szenen-Frames und Schwellen anpassen (Abschnitt 7). | 4.7, 7 |

**Empfehlung:** Die NTSC-Zeitwerte per Patch-Tabelle setzen (Abschnitt 2.3, Standard 60 Hz, 50 Hz als
Option für xemu-Vergleiche). Zusätzlich den `.data`-Anfangswert fps = 60.0f setzen und im 60-Hz-Raster
präsentieren. Danach die Laufzeit-Observablen aus Abschnitt 8 einmal mit 50 Hz und einmal mit 60 Hz messen.

---

## 1. Ausgangslage

* `Game::Init` 0xA26A0 setzt die Zeitbasis abhängig von CApp+0xE8 (0 = PAL). Die EU-Disc liefert immer PAL
  (startup.md 3.4). `Game::Frame` 0xA2C20 simuliert in festen Schritten dt (startup.md 3.2).
* dt `[0x2D6748]` hat 280 Code-Referenzen, fps `[0x2D6744]` nur 3 (`timing_globals.out` Abschnitt 1).
* Der Renderpfad bekommt keinen Interpolationsfaktor. Ein Frame zeigt also immer den letzten Simulationsstand.
* Methode: (a) Leser und Schreiber aller Zeit-Globals; (b) alle dt-Nutzer; (c) Suche nach tick-gebundenen
  Mustern: fest kodierte 0.02/1/60/50/60, Zähler mit Grenzwert, Glättung pro Tick; (d) Skript-VM und
  Skriptdaten; (e) Vergleich mit dem NTSC-Prototyp; (f) Modell der Schrittschleife.
* Der Instruktionsindex (`timing_common.index()`) umfasst neben den Ghidra-Funktionen auch Code zwischen den
  Funktionen. Wichtige Methoden wie `Frontend::Update` 0x22B30, `Intro::Update` 0x2B5F0 und `Ingame::Update`
  0x2AAB0 hängen nur an VTables und sind keine Ghidra-Funktionen. Alle 280 Rohtreffer von dt liegen auf
  dekodierten Befehlen.

---

## 2. Die Zeitwerte in `Game::Init` 0xA26A0 (Aufgabe 1)

### 2.1 Beide Zweige (`timing_patch.out` Abschnitt 1)

```
000A26BD 8b86e8000000           mov eax,[esi+0xe8]            ; CApp+0xE8
000A26C3 bb01000000             mov ebx,1
000A26C8 3bc3                   cmp eax,ebx
000A26CA 7548                   jne 0xa2714                   ; != 1 -> PAL
; ---- NTSC
000A26CC d905cc422900           fld [0x2942cc]                ; 60.0f
000A26D2 a164182a00             mov eax,[0x2a1864]            ; "NTSC"
000A26D7 8a0d68182a00           mov cl,[0x2a1868]             ; 0
000A26DD d915607a2e00           fst [0x2e7a60]
000A26E3 d90590e32900           fld [0x29e390]                ; 1/60 (0x3C888889)
000A26E9 c7056c7b2e0000007042   mov [0x2e7b6c],0x42700000     ; 60.0f
000A26F3 c7055c7a2e008988883c   mov [0x2e7a5c],0x3c888889     ; 1/60
000A26FD c705687b2e008a4b063d   mov [0x2e7b68],0x3d064b8a     ; 1/30.5
000A2707 a3707b2e00             mov [0x2e7b70],eax            ; "NTSC"
000A270C 880d747b2e00           mov [0x2e7b74],cl             ; NUL
000A2712 eb3a                   jmp 0xa274e
; ---- PAL
000A2714 d905ac552900           fld [0x2955ac]                ; 50.0f
000A271A c7056c7b2e0000004842   mov [0x2e7b6c],0x42480000     ; 50.0f
000A2724 d915607a2e00           fst [0x2e7a60]
000A272A c7055c7a2e000ad7a33c   mov [0x2e7a5c],0x3ca3d70a     ; 0.02f
000A2734 d905c0422900           fld [0x2942c0]                ; 0.02f
000A273A c705687b2e008542213d   mov [0x2e7b68],0x3d214285     ; 1/25.4
000A2744 c705707b2e0050414c00   mov [0x2e7b70],0x4c4150       ; "PAL\0"
; ---- gemeinsamer Schluss (x87-Stapel: st0 = dt, st1 = fps)
000A274E d915587a2e00           fst [0x2e7a58]                ; dt
000A2754 881d4c672d00           mov [0x2d674c],bl             ; fester Zeitschritt = 1
000A275A d9c9                   fxch st(1)
000A275C 8bce                   mov ecx,esi
000A275E d91d44672d00           fstp [0x2d6744]               ; fps
000A2764 d91d48672d00           fstp [0x2d6748]               ; dt
```

Das Decompilat (`_DAT_002d6744 = 0x42480000` usw.) verdeckt, dass fps und dt im gemeinsamen Schluss aus dem
x87-Stapel gespeichert werden. Ein Patch der Immediates allein würde fps und dt **nicht** ändern.

### 2.2 Konstanten-Pool (`timing_patch.out` Abschnitt 2, `timing_consts.out`)

| Konstante | Wert | Code-Nutzer | Folge |
|---|---|---|---|
| 0x2955AC | 50.0f | 9 (u. a. `FUN_00030E30` Distanz 50², `FUN_000445A0`, `FUN_000C0AC0`, 0x67139) | nicht patchbar |
| 0x2942C0 | 0.02f | 13 (`FUN_00022550` Fade, 9× `FUN_00077E50`, 0x67153) | nicht patchbar |
| 0x2942CC | 60.0f | 3 (0xA26CC, Frontend-Attract-Schwelle 0x22BEC, DSOUND 0x24EA60) | Ziel des fps-Operanden, wird nur gelesen |
| 0x29E390 | 1/60 | 4 (0xA26E3, 3× `FUN_00084E60`) | Ziel des dt-Operanden, wird nur gelesen |

Der Compiler teilt die Konstanten also über Funktionsgrenzen hinweg, aber **nicht** zwischen den beiden
Zweigen: Jeder Zweig lädt aus eigenen Pool-Einträgen. Die drei Immediates (0x2E7B6C, 0x2E7A5C, 0x2E7B68)
stehen inline im jeweiligen Zweig.

### 2.3 Patch-Tabelle (Originalbefehl vollständig prüfen, nur Operandenbytes ändern)

| Adresse | Original (ganzer Befehl) | neu | geänderte Bytes | Wirkung |
|---|---|---|---|---|
| 0xA2714 | `d905ac552900` | `d905cc422900` | 0xA2716–0xA2717 | fps-Quelle 50.0f → 60.0f (→ 0x2E7A60, 0x2D6744) |
| 0xA271A | `c7056c7b2e0000004842` | `c7056c7b2e0000007042` | 0xA2722 | [0x2E7B6C] 50.0f → 60.0f |
| 0xA272A | `c7055c7a2e000ad7a33c` | `c7055c7a2e008988883c` | 0xA2730–0xA2732 | [0x2E7A5C] 0.02f → 1/60 |
| 0xA2734 | `d905c0422900` | `d90590e32900` | 0xA2736–0xA2737 | dt-Quelle 0.02f → 1/60 (→ 0x2E7A58, 0x2D6748) |
| 0xA273A | `c705687b2e008542213d` | `c705687b2e008a4b063d` | 0xA2740–0xA2742 | [0x2E7B68] 1/25,4 → 1/30,5 (wirkungslos, nur für exakten NTSC-Zustand) |
| 0xA2744 | `c705707b2e0050414c00` | **unverändert** | – | Tag bleibt „PAL“ |

Die Emulation des Blocks (`timing_patch.py`, Mini-Interpreter für genau diese Befehle) ergibt für PAL mit Patch
in allen Zeit-Globals dieselben Werte wie der NTSC-Zweig: 0x2D6744 = 60.0, 0x2D6748 = 0x3C888889,
0x2E7A58 = 0x2E7A5C = 1/60, 0x2E7A60 = 0x2E7B6C = 60.0, 0x2E7B68 = 0x3D064B8A, 0x2D674C = 1. Dabei bleiben
0x2E7B70 = "PAL" und 0x2E7B74 = 0 (BSS) erhalten. Ergebnis: `patched PAL == NTSC ... : True`. Alle fünf
Originalbefehle und beide `.data`-Werte (2.4) stimmen mit dem XBE überein (`orig ok`).

**Alternativen (nicht empfohlen):**
* `jne` 0xA26CA ausnoppen: Dann läuft der NTSC-Zweig, das Tag wird aber „NTSC“ und die Filmnamen lauten
  `*-NTSC.xmv` (3).
* Nachträglich schreiben (Probe-Hook auf `CApp::Init` 0xD26A0, Werte nach der Rückkehr setzen): Zwischen
  0xA2764 und dem Ende von `Game::Init` liest laut Callgraph zwar niemand dt. Die Kontext-ctors und
  `Intro::Enter` rufen aber virtuell auf, das bleibt unsicher. Der Operanden-Patch hat dieses Problem nicht.

### 2.4 `.data`-Anfangswerte (`timing_patch.out` Abschnitt 4)

| Adresse | Original | neu | Grund |
|---|---|---|---|
| 0x2D6744 (fps) | `00004842` (50.0f) | `00007042` (60.0f) | Der Pause-/Eingabe-ctor `FUN_000E4AA0` liest fps **vor** `Game::Init` (CApp::Init Schritt 6, startup.md 2.6) und berechnet daraus die Tastenwiederholung (4.7) |
| 0x2D6748 (dt) | `0ad7a33c` (0.02f) | `8988883c` (1/60) | Konsistenz. Vor `Game::Init` liest niemand dt: Callgraph ab 0xD26A0 mit 875 Funktionen, keine C++-Initialisierer (`timing_globals.out` Abschnitt 5) |

Der Prototyp hat dieselben `.data`-Werte (fps 50.0, dt 0.02 an 0x2D68C4/0x2D68C8, `timing_proto.out`). Auch in
NTSC liest der ctor also 50.0 (siehe 4.7).

---

## 3. Bedeutung und Leser der Zeit-Globals (Aufgabe 2)

Alle Befehle mit der Adresse als Operand, Rohtreffer in allen Sektionen und ein Scan nach Dword-Konstanten
0x2E7A54–0x2E7B84 (Zugriff über eine Strukturbasis) stehen in `timing_globals.out` Abschnitte 1–2. Im Bereich
gibt es keine Basisadresse und keine Datenzeiger, nur die einzelnen Globals.

| Global | PAL | NTSC | Schreiber | Leser | Bedeutung | 60-Hz-Wert richtig? |
|---|---|---|---|---|---|---|
| 0x2D6748 dt | 0.02f | 1/60 (0x3C888889 = 0,016666668) | 0xA2764; variabler Zweig 0xA2D34/0xA2D4C/0xA2D6C (tot) | 276 Lesestellen in 139 Funktionen/Segmenten | fester Zeitschritt in s | ja |
| 0x2D6744 fps | 50.0f | 60.0f | 0xA275E, 0xA2D7E (tot) | **nur** 0xE4AA0 (vor `Game::Init`, liest also den `.data`-Wert) | Schritte pro Sekunde | ja; der Laufzeitwert wird nie gelesen, wirksam ist nur der `.data`-Wert (2.4) |
| 0x2D674C | 1 | 1 | 0xA2754 (`.data` = 1) | 0xA2D23 | fester Zeitschritt an | unverändert |
| 0x2E7A58 | dt | dt | 0xA274E | 0xA2D59/0xA2D66 | untere Klemme des variablen Schritts | ja (Zweig tot) |
| 0x2E7A5C | 0.02f | 1/60 | 0xA26F3/0xA272A | 0xA2D3A/0xA2D47 | obere Klemme des variablen Schritts | ja (Zweig tot) |
| 0x2E7A60 | 50.0f | 60.0f | 0xA26DD/0xA2724 | – | fps-Kopie, nur geschrieben | belanglos |
| 0x2E7B6C | 50.0f | 60.0f | 0xA26E9/0xA271A | – | fps-Kopie, nur geschrieben | belanglos |
| 0x2E7B68 | 0x3D214285 = 1/25,4 | 0x3D064B8A = 1/30,5 | 0xA26FD/0xA273A | – | nur geschrieben. **Vermutung:** Bildzeit eines 25/30-fps-Videos mit Toleranz (≈ 0,984 · 2·dt) | belanglos; der NTSC-Wert passt zu 60 Hz |
| 0x2E7B70/74 | "PAL\0" | "NTSC", 0 | 0xA2707/0xA270C/0xA2744 | **nur** 0xA2424 `push 0x2E7B70` in `CApp::PlayMovie` 0xA2380 (VTable-Slot 6): `sprintf(buf, "%s-%s", name, tag)` (Format 0x2A1838) → `Video\<name>-PAL.xmv` | Filmsuffix | **muss "PAL" bleiben**: alle 47 Filme der EU-Disc heißen `*-PAL.xmv` (`timing_extract`-Listing, avi.md) |
| 0x2E7B88 | – | – | 0xA2FD8 | 7 Leser: `FUN_00021B60` (Frontend::Render), `FUN_00045F10`, `FUN_000C0AC0`, `FUN_000C8E90` (Ingame::Render), `FUN_000EEF80` | echte Zeit zwischen zwei Render-Aufrufen (QPC), für Animationen im Renderpfad | unabhängig vom Tick |

Die Sprachwahl, die Archive sowie Splash und Legal hängen an CApp+0xE8, nicht an diesen Globals
(startup.md 3.4). Sie bleiben PAL, weil der Patch CApp+0xE8 nicht berührt.

---

## 4. Suche nach tick-gebundener Zeit (Aufgabe 3)

### 4.1 dt-Nutzer

280 Befehle in 139 Funktionen/Segmenten (45 davon Gap-Code), Adressbereich 0x12F64–0xEEF6A. Nach Befehlsart:
170 `fld`, 40 `mov`, 40 `fsub`, 16 `fmul`, 7 `fdiv`, 3 `fadd`, 2 `fcomp`, je 1 `fst`/`fstp`
(`timing_globals.out` Abschnitt 3). Muster:
* **Zeitakkumulatoren:** Im einfachsten Muster (`fld dt; fadd [x]; fstp [x]` und Varianten) gibt es 39
  Stellen an 23 Feldoffsets und 15 Abwärtszähler `x −= dt`. Beispiele: +0x24 (7×), +0x3C, +0x78
  (Entity-/Kontextzeit), Frontend +0x78/+0x7C/+0x174/+0x178 (0x22E15–0x22E50), Sound-Fade +0x44
  (`FUN_000C9480`).
* **Integration:** `FUN_00094560` (11 Stellen): v += a·dt, x += v·dt (halbimplizites Euler-Verfahren, siehe
  Decompilat).
* **Parameter pro Tick aus dt:** `FUN_000BB510` (Effekt-Presets) übergibt `dt·6`, `dt·1.8`, `dt·2π`, `dt·4π`
  usw. Das sind Schrittweiten pro Tick, die beim Anlegen aus dt berechnet werden. Weil dt vor dem ersten
  Effekt feststeht, sind sie auch bei 60 Hz in Sekunden richtig.
* `FUN_00019C80` (Kamera-Update) gibt dt an die Kameramodi weiter (Fälle 0–9).
* **Korrektur (Review M3b, 12.1):** Ein dt-Leser rechnet nicht automatisch in Sekunden. `FUN_00015990`
  (Kameramodus, Aufruf 0x19CC2 mit dt) zerlegt dt in feste Unterschritte von 0,01 s
  (`do { …; t −= 0.01 } while (t > 0)`, 0x179B3–0x179C4). Bei dt = 0,02 und bei dt = 1/60 sind das je 2
  Unterschritte pro Tick, also 100 bzw. 120 pro Sekunde. Die Heuristik „dt-Nutzer = Sekunden“ gilt nur, wenn
  dt auch die Schrittweite ist.

dt wird nur in `Game::Init` und im toten variablen Zweig geschrieben.

### 4.2 Kontexte

| Kontext | Zeitquelle | Beleg | 60 Hz |
|---|---|---|---|
| Intro (`Intro::Update` 0x2B5F0, Gap) | **QPC-Sekunden:** +0x18 = sec(QPC − letzter Aufruf) (0x2B611–0x2B632), +0x14 += +0x18. Bilder über das Zeitbild-Objekt `FUN_000EFB30` (VTable 0x2A57C0, vf1 0xEEC80: +8 += t; vor min 0x34 bleibt das Bild, nach max 0x38 endet es, dazwischen beendet Taste 10/7) | Legal: `push 6.0; push 6.0` (0x2B669/0x2B71C) → genau 6 s. Splash `NI_Splash_PAL.nif`: min 1,5 s, max 5,0 s (0x2B822/0x2B829) | unverändert (Echtzeit) |
| Intro, Zustand 0 | **Tick:** `inc [0x2DAD8C]; cmp 20` (0x2B695) – erst ab dem 21. Update startet das Vorladen, das Legal-Bild läuft weiter | `timing_tickscan.out` | 0,42 s → 0,35 s bis zum Ladebeginn; ohne sichtbare Folge |
| Frontend (`Frontend::Update` 0x22B30, Gap, VTable 0x294208 Slot 2) | dt: Leerlauf +0x7C += dt, solange `FUN_0009BDE0` (keine Eingabe) gilt, sonst 0 (0x22E0C–0x22E23); ab 57 s (0x2942D4) Lautstärkeaufruf, ab **60 s** (0x2942CC, 0x22BE9) → `SetNextContext("Attract")` | – | unverändert |
| Frontend | **Tick:** Eingabesperre +0x84 = 30 (`FUN_00021A80` 0x21AEB) zählt pro Update herunter (0x22C4C), `FUN_00020000` ignoriert Menüeingaben solange +0x80 | – | 0,60 s → 0,50 s |
| Frontend | **Tick:** Bildschirm-Fade `FUN_00022550` (0x22C3F): Alpha der 4 Eckfarben −= 0.02 pro Update, bis ≤ 0.02 (0x2942C0); gestartet u. a. von `FUN_00024AA0` (0x24DB7) | – | 50 Schritte: 1,00 s → 0,83 s (Startwert 1,0 Vermutung) |
| Frontend | **Tick:** „Neues Spiel“: +0x48 = 5 (`FUN_00020000` Fall 0), `FUN_00020090` zählt herunter → Ingame/`Level1-1` | – | 0,10 s → 0,083 s |
| Attract (0x2BB50) | Film `Attract` (eigene XMV-Uhr), Ende bei Filmende oder Taste 10/7 | – | Filmlänge unverändert |
| Death (0x2C1C0, Gap) | QPC-Sekunden wie Intro (0x2C1D5–0x2C1EB), Bild über `0x2C0F0` mit 2,5/3,0 s (0x2C247; **Vermutung:** min/max wie 0x2B360) | – | unverändert |
| Ingame (`Ingame::Update` 0x2AAB0 → `FUN_000C8BA0` 0x2AB48) | dt (Session, Skripte 4.3, Kamera, Physik); Spielzeit-Uhr +0x178 += dt, bei 3600 s Übertrag in den Stundenzähler +0x17C | – | unverändert |

Kontextwechsel (`FUN_0002A660`, acht `ClearBuffer`+`DisplayFrame`) und das Vorladen über die
Async-Warteschlange (`FUN_000D2370`, einmal pro `Game::Frame`, also pro Frame) hängen an Frames bzw. I/O,
nicht an Ticks.

### 4.3 Skript-VM (.ful)

* **Interpreter** `FUN_000B97D0` (Opcode = oberstes Byte eines 32-Bit-Worts, Bits 8–23 = Quellzeile).
  Zeitrelevante Opcodes: 0x18 = Wert vom Stapel → Thread+0x20 (Wartezeit), dann Rücksprung (yield);
  0x02 = Thread-Timer +0x1C auf den Stapel; 0x03 = Stapel → +0x1C; 0x13 = Native `FUN_000B0C40(Klasse, Id)`
  (Klasse 0, Id 9/10/11/12 = fadd/fsub/fdiv/fmul).
* **Scheduler** `FUN_000BA060(t)`: Für jeden lebenden Thread gilt +0x20 −= t und +0x1C −= t. Ist +0x20 ≤ 0,
  läuft der Thread. t = dt aus `FUN_000C8BA0` (0xC8DC8 `mov edx,[0x2D6748]`, Aufruf 0xC8DD5). Mit t = 0
  rufen 0xC8E77 (während Zwischensequenzen) und `Ingame::Render` 0xC90F8 (fünf Frames nach einer
  Intermission) auf. **Wartezeiten und Timer sind also Sekunden.**
* **Skriptdaten** (`timing_script.out`): 6473 von 7224 Wait-Opcodes haben einen Direktwert. Bis auf einen
  (0x01E80006, ein Dekodierartefakt der linearen Analyse) sind alle plausible Float-Sekunden: 0 (5511×),
  1.0 (235×), 0.1 (134×), 2.0, 0.5, 0.05 (48×), …, 0.02 (24×), 0.04 (7×). Timer-Werte: 1.0, 0.5, 1.5, 2.0, 5.0 usw. Es gibt keine ganzzahligen Tickzahlen, und in den
  Debug-Symbolen (`.dbg`) keine Namen mit „Frame“ oder „Tick“. Die Symbole enthalten `fDeltaTime`,
  `fTimeToWait`, `waitTime`.
* **Quantisierung:** Opcode 0x18 setzt die Wartezeit neu und verwirft den Rest. Wait(t) dauert also
  q(t) = ⌈t/dt⌉ Ticks (mindestens 1), mit float32-Rundung. Die Abweichung zwischen 50 und 60 Hz ist für
  **ein einzelnes** Wait höchstens ein Tick (≤ 20 ms); in Schleifen summiert sie sich (nächster Punkt).
  Beispiele aus `timing_script.out` Abschnitt 4:

  | Wait(t) | 50 Hz | 60 Hz | Differenz |
  |---|---|---|---|
  | 0 | 1 Tick = 20 ms | 1 Tick = 16,7 ms | −3,3 ms |
  | 0.02 | 1 = 20 ms | 2 = 33,3 ms | +13,3 ms |
  | 0.05 | 3 = 60 ms | 3 = 50 ms | −10 ms |
  | 0.1 | 5 = 100 ms | 7 = 116,7 ms | +16,7 ms |
  | 0.5 | 25 = 500 ms | 30 = 500 ms | 0 |
  | 1.0 | 51 = 1,02 s | 61 = 1,017 s | −3,3 ms |
  | 5.0 | 251 = 5,02 s | 300 = 5,00 s | −20 ms |

* **Schleifen** (Rückwärtssprünge 0x0F/0x10/0x11, Abschnitt 3; **korrigiert nach dem Review**, 12.1 und
  `timing_review.out` Abschnitt 4): Der Skript-Compiler setzt vor **jeden** unbedingten Rücksprung ein
  `Wait(0)` (`push_imm 0; WAIT`, alle 3823 von 3823 Rücksprüngen 0x0F in den 23 PAL-Skripten, z. B.
  `level1-1.ful` 0x400C/0x4014). Ein Durchlauf mit `Wait(t)` kostet also q(t) + 1 Ticks, und die Abweichung
  zwischen 50 und 60 Hz summiert sich über die Durchläufe:

  | Wait pro Durchlauf | Schleifen | Ticks 50/60 Hz | Periode 50/60 Hz | Änderung |
  |---|---|---|---|---|
  | Wait(0.05) + Wait(0) | 43 (u. a. `ResetChaseCam` 22×, `RunSteam` 20×) | 4 / 4 | 0,080 / 0,067 s | −17 % |
  | Wait(0.1) + Wait(0) | 63 (u. a. `WaitMove` 22×, `RunSteam` 20×, `Add` 8×) | 6 / 8 | 0,120 / 0,133 s | +11 % |
  | Wait(0.1) + 2× Wait(0) | 6 | 7 / 9 | 0,140 / 0,150 s | +7 % |
  | Wait(0.2) + 3× Wait(0) | 2 | 14 / 16 | 0,280 / 0,267 s | −5 % |
  | Wait(1) + Wait(0) | 55 (u. a. `RotatingArmSound` 46×) | 52 / 62 | 1,040 / 1,033 s | −0,6 % |
  | Wait(1) + 3× Wait(0) | 23 | 54 / 64 | 1,080 / 1,067 s | −1,2 % |

  Ab etwa 0,5 s Wartezeit bleibt die Abweichung unter 4 %, darunter erreicht sie −17 % bis +11 % je Durchlauf.
  Die Dampframpen `RunSteam` aller Level-1/2-Skripte laufen z. B. 0,60 → 0,67 s bzw. 0,48 → 0,53 s.
  Die frühere Einstufung „4526 reine `Wait(0)`-Schleifen (Warten auf eine Bedingung)“ war falsch: `Wait(0)`
  steht in jeder Schleife. Von den 5380 Schleifen haben 4200 kein explizites Wait außer `Wait(0)` und kein
  Timer-Lesen; 2979 davon rufen pro Durchlauf eine Engine-Funktion auf oder zählen eine Variable weiter, laufen
  also einmal pro Tick (meist Abfragen wie `@@action@@aOpen`, `RunHint`, `WaitLeaveDoor`; dazu Zählschleifen mit
  `iadd 1`, die Ticks statt Sekunden zählen). Schrittschleifen mit variablem Schritt gibt es ebenfalls (73 Schleifen, deren
  einziger Schritt eine Variable oder ein Ausdruck ist), laut Review z. B. `level3-2` `aEgg1` 0x8410/0x84E0 (Skalierung über Native 111 =
  `FUN_00036F10`, 2 Ticks pro Schritt; nicht nachgeprüft). Die erste Suche
  hatte nur sofortige fadd/fsub-Operanden ausgewertet (59 Schleifen). Alle diese Stellen verhalten sich bei
  60 Hz wie in der NTSC-Fassung (die Skripte sind identisch).
* Die Gültigkeit hängt an den Skriptdaten: Die PAL-Skripte (FutPAL01) und die NTSC-Prototyp-Skripte
  (FutNTSC01) sind in 21 von 23 Levels **byte-identisch**, `.dbg` eingeschlossen. Nur level1-3 und level1-5
  unterscheiden sich (Bearbeitungsstand). Es gibt also keine regionsspezifische Umrechnung von Zeiten.

### 4.4 Animation (NetImmerse)

`NiAVObject::Update(fTime, bUpdateControllers)` = `FUN_00101840` (vf25 + Bound) hat 212 Aufrufstellen in 147
Funktionen/Segmenten (`timing_globals.out` Abschnitt 3). Als Zeit übergeben sie (Abschnitt 7): 82× die
Konstante 0 (statische Szenen), sonst Objektfelder, vor allem +0x24 (31×) und +0x78 (22×), dazu +0x68, +0x18,
+0x174 usw. Diese Felder werden per dt hochgezählt (4.1). Einige HUD-Anzeigen übergeben statt einer Zeit
einen Wert: `FUN_000279C0` +0x10B4/+0x10B8/+0x10BC kommen aus `gap_028960`, z. B. aus dem Global 0x2E7920, und
wählen damit den Keyframe. 39 Argumente ließen sich nicht automatisch zurückverfolgen. Eine Zeitquelle aus
Tickzählern ist nicht aufgetaucht. NI-Controller rechnen mit absoluten Sekunden. Animationen laufen also in
Sekunden. Beispiel: Die Dampfwolke im Frontend hat bei 50 Hz eine Periode von 1,0 s = 50 Frames
(m3_report Befund 7) und muss bei 60 Hz 1,0 s = 60 Frames haben.

### 4.5 Physik, Partikel, Kamera

* Physik: Die Integration ist dt-skaliert (4.1). Das halbimplizite Euler-Verfahren hat einen Fehler O(dt).
  Sprunghöhen und Flugbahnen ändern sich deshalb minimal, aber nicht systematisch. **Vermutung:** unter 1 %,
  ungemessen. **Korrektur (Review M3b, 12.1):** Die Dämpfung der Starrkörper ist dagegen tick-gebunden.
  `FUN_000AFA30` (VTable 0x2A1F28 Slot 5) multipliziert bei jedem Aufruf ohne dt den Impuls +0xC0/+0xC4/+0xC8
  mit 0.98 (bei |v|² < 1 mit 0.9, 0xAFB73–0xAFBD1) und den Drehimpuls +0xCC/+0xD0/+0xD4 mit 0.9
  (0xAFB07–0xAFB37). Der Zeitparameter geht nur an `FUN_000AF770` (Schwerkraft). Aufruf einmal pro Tick:
  `FUN_000C8BA0` → Physikwelt [0x2E99DC] vf1 0xAE7D0 (+0x24 += dt) und vf2 0xAE7E0 → Slot 5 jedes Körpers der
  Liste [0x2E9BA0] (0xAE80F) → Solver 0xAE0D0. Körper legen `FUN_00073FC0` (verschiebbares Objekt,
  „MoveableDef“), `FUN_00095290` (Wippe, „seesaw_“), `FUN_00064DD0` (Mine, „Mine_Explode“) und `FUN_0007F960`
  über den ctor `FUN_000AF920` an. Bei 60 Hz: Halbwertszeit des Impulses 0,686 → 0,572 s, Ausrollweg
  v0·0,98 s → v0·0,817 s, Endgeschwindigkeit unter konstanter Kraft −17 %; Drehung 0,132 → 0,110 s.
* Partikel/Effekte: Lebensdauer und Emissionszeit in Sekunden (Natives `SetLifeTime`, `SetEmitTime`,
  `PresetEmitTime`; `FUN_000BB510` dt-skaliert).
* Kamera, **tick-gebunden:** Kamerawackeln `FUN_000131E0`: Die Phase ist der Tickzähler +0x14C
  (`FUN_00019C80` erhöht ihn pro Update) mal Frequenz +0x1F4, die Amplitude nähert sich pro Tick um 5 % an
  (0.050000012). Glättungen pro Tick: `FUN_00018250` (`x·0.04 + y·0.75`), `FUN_00061A70` (Faktor 0,25 und
  ·1/60 fest), `FUN_000C0AC0` (Flackern `0.8·x + 0.2·(rand·10 + 50)`). Bei 60 Hz wird das um 20 % schneller.
  **Ergänzung (Review M3b, 12.1):** Die Kamerafeder `FUN_00015990` integriert in festen Unterschritten von
  0,01 s (x += v·0,01, Glättungen ×0.91/×0.97/×0.9 je Unterschritt), 2 pro Tick bei beiden Raten (4.1): 20 %
  schneller. Die Overlay-Einblendung `FUN_000A8490` ändert +0x28 um ±0.03 pro Update bis 0.75 (25 Ticks:
  0,50 → 0,42 s; **Vermutung:** Pause-/Hinweis-Overlay).
* `FUN_00077E50` setzt Wasserspritzer auf `pos − vel·0.02` (feste PAL-Ticklänge). Bei 60 Hz liegt der
  Punkt einen Bruchteil eines Ticks daneben, unsichtbar.

### 4.6 Sound und Film

* Sound: `CApp::vf10` 0xD25D0 läuft pro Tick und ruft das Sound-Update auf (`DirectSoundDoWork`, avi.md).
  Lautstärke-Fades rechnen mit dt (`FUN_000C9480`, `FUN_000CD270`). Bei 60 Hz kommen die Aufrufe öfter, die
  Zeiten bleiben gleich. In der HLE ist `DoWork` ohnehin ein No-op.
* Film: Der XMV-Player hat eine eigene QPC-Uhr (startup.md 3.3), der Host muss nach pts takten (avi.md 3.4).
  Er ist unabhängig vom Tick. Die PAL-Filme laufen mit 15 bzw. 25 fps.

### 4.7 Eingabe und Menüs

* **Tastenwiederholung** (tick-gebunden): `CApp::vf10` 0xD25D0 → `FUN_000E52F0` → `FUN_0009B340`
  (ecx = Wiederholstruktur Pause+0x1C, Argument = aktiver Controller [0x2E6FA4]) → `FUN_0009B210` (16 Tasten)
  und `FUN_0009B270`/`FUN_0009B120` (Sticks). Die Wiederholung feuert, wenn Zähler % Periode == 0. Die
  Periode (u16 +0x50) schreibt nur `FUN_0009B2F0` (0x9B333) aus `FUN_000E4AA0`:
  `_ftol([0x2D6744] · 0.25)` (0xE4AA0–0xE4AB9; `_ftol` 0x167080 schneidet ab). Weil der ctor vor
  `Game::Init` läuft, ist die Periode in PAL **und** NTSC = trunc(50 · 0,25) = **12 Ticks**.

  | Variante | Periode | Zeit |
  |---|---|---|
  | PAL original (50 Hz) | 12 | 0,24 s |
  | NTSC original / Patch ohne `.data` (60 Hz) | 12 | 0,20 s |
  | Patch mit `.data` fps = 60.0 (60 Hz) | 15 | 0,25 s |

  Gemeint war sichtlich eine Viertelsekunde (fps · 0.25). Der `.data`-Patch trifft das Ziel und liegt
  näher an PAL (0,24 s) als die NTSC-Fassung.
* **Menü-Entprellung** (tick-gebunden, **Vermutung:** Pause-/Optionsmenü 0xE1000–0xE4000): +0x78 = 5 nach
  jeder Auswahl (`FUN_000E2BF0`/`E2C50`/`E2CE0`/`E2DB0`), `FUN_000E29E0` zählt pro Update herunter. Das ergibt
  0,10 s → 0,083 s.

### 4.8 Fest kodierte Zeitkonstanten (`timing_consts.out`)

| Konstante | Nutzer | Deutung | 60 Hz |
|---|---|---|---|
| 0.02f @0x2942C0 | `FUN_00022550` | Frontend-Fade pro Tick (4.2) | schneller |
| | `FUN_00077E50` (9×) | Spritzer `pos − vel·0.02` | belanglos |
| | 0x67153 | `(50 − d)·0.02` = Distanzabfall | keine Zeit |
| 1/60 @0x29E390 | `FUN_00084E60` (3×) | Vorhersage `vel·k·1/60` | fest wie NTSC |
| 1/60 Immediate | `FUN_00061A70` 0x62815 | Vektor ·1/60 (Kamera/Spieler) | fest wie NTSC |
| 50.0f @0x2955AC | `FUN_00030E30`, `FUN_000445A0`, 0x67139, `FUN_000C0AC0` | Distanzen (2500 = 50²), Flackerwert | keine Zeit |
| 60.0f @0x2942CC | Frontend 0x22BEC | Attract nach 60 s Leerlauf (Sekunden) | unverändert |
| 60.0 @0x2A5180, f64 @0x2A56B0 | `FUN_000E7980`, `FUN_000ED8B0` | Minuten↔Sekunden (Zeitanzeige, Film-Zeitstempel „mm:ss“) | unverändert |
| 1/25 @0x293B98 | `FUN_00018250`, `FUN_00034DF0`, 0x6BE49 | Kamerafilter (s. o.); `FUN_00034DF0`: Bildnummer·0.04 (**Vermutung:** 25-fps-Animationsdaten → Sekunden) | wie 4.5 |
| 0.02 Immediates | `FUN_0003B960`, 0x6BFB6, 0x6C1CA | Farb-/Effektparameter (0.02/0.98) | keine Zeit |
| 0.02 Immediate | `FUN_00043D80` 0x43EEC → +0x64 | **korrigiert (Review M3b, 12.1):** fest kodierte Schrittweite des Chicken Walker. `FUN_000445A0` integriert damit pro Update den Stick: Rohr-Pitch +0x50 −= stickY·k·[+0x64] (0x44BA5, Klemme ±0.7), Gier +0x54 += stickX·[+0x64] (0x44BB2, Klemme ±π/2); dazu automatisches Nachdrehen um höchstens 0.05 pro Update | **tick-gebunden:** Zielen/Drehen 1,0 → 1,2 rad/s, Nachdrehen 2,5 → 3,0 rad/s (+20 %) |

Bemerkenswert: Der Code enthält sowohl fest kodierte 0.02 als auch fest kodierte 1/60. Entwickelt wurde
also nicht für eine einzige Rate. Für das Ergebnis ist das ohne Bedeutung, weil alle Stellen in beiden
Fassungen identisch sind.

### 4.9 Heuristischer Zähler-Scan (`timing_tickscan.out`)

Gesucht wurden `dec`/`sub 1`/`lea r,[r-1]` auf Speicherfeldern sowie `inc` mit nachfolgendem `cmp imm`.
Stapel, Referenzzähler [reg+4] und reine [reg] sind ausgefiltert. Ergebnis: 99 Abwärts- und 14
Aufwärtsstellen. Von Hand geprüft:
* **Zeitrelevant (tick-gebunden):** Frontend +0x84 (30), Intro [0x2DAD8C] (20), Menü +0x78 (5),
  „Neues Spiel“ +0x48 (5), Kamera +0x14C/+0x198 (`FUN_00019C80`), `FUN_0007F8C0` +0x4C (Objekt wird
  11 Ticks nach einer Bedingung freigegeben).
* **Pro Frame, nicht pro Tick:** `FUN_000A46D0` (Speicherpflege alle 41/201 Render-Aufrufe),
  [0x2EA99C] (5 gelöschte Frames nach Intermission in `Ingame::Render`).
* **Keine Zeit:** Containergrößen (+0x10/+0x14/+0x74/+0x78 in Listen), Leben [0x2D67DC] (5 → „Death“),
  Munition +0xC4, Zustandsstapel +0x750 (max 15), Listengrößen der Intermission (+0x74 = 900 ist eine
  Kapazität).

### 4.10 Zusammenfassung der tick-gebundenen Stellen

| Stelle | Ticks | 50 Hz | 60 Hz (= NTSC) | Spielrelevant? |
|---|---|---|---|---|
| Frontend-Fade `FUN_00022550` | 50 | 1,00 s | 0,83 s | nein (Optik) |
| Frontend-Eingabesperre +0x84 | 30 | 0,60 s | 0,50 s | nein |
| „Neues Spiel“-Verzögerung | 5 | 0,10 s | 0,083 s | nein |
| Menü-Entprellung +0x78 | 5 | 0,10 s | 0,083 s | nein |
| Tastenwiederholung | 12 (15 mit `.data`-Patch) | 0,24 s | 0,20 s (0,25 s) | Bediengefühl |
| Intro-Ladebeginn | 21 | 0,42 s | 0,35 s | nein |
| Kamerawackeln (Frequenz, Abklingen) | – | – | +20 % | Optik |
| Glättungen pro Tick (Kamera, Flackern) | – | – | Zeitkonstante −17 % | Optik |
| Kamerafeder `FUN_00015990` (Unterschritte 0,01 s) | 2 je Tick | 100/s | 120/s (+20 %) | Optik |
| Overlay-Einblendung `FUN_000A8490` | 25 | 0,50 s | 0,42 s | nein |
| Skript-Schleifen mit kurzen Waits (Wait(t) + Wait(0) je Durchlauf, 4.3) | q(t) + 1 | z. B. 0,080 / 0,120 s | 0,067 / 0,133 s (−17 % / +11 %), **kumulativ** | Optik und Abläufe in Levels (z. B. Dampframpen, `WaitMove`) |
| Skript-Schleifen nur mit `Wait(0)` (2979 mit Engine-Aufruf oder Zähler) | 1 je Durchlauf | 50/s | 60/s | Abfragen ohne Folge; Zählschleifen +20 % |
| Skript-Wait-Quantisierung (einzelnes Wait) | ±1 Tick | – | ≤ 20 ms je Wait | nein |
| Objektfreigabe `FUN_0007F8C0` | 11 | 0,22 s | 0,18 s | nein |
| **Starrkörper-Dämpfung `FUN_000AFA30`** (verschiebbare Objekte, Wippen, Minen) | ×0.98/×0.9 je Tick | Halbwertszeit 0,686 s | 0,572 s; Ausrollweg −17 % | **ja** (Physik-Rätsel) |
| **Chicken Walker: Zielen/Drehen** (`FUN_000445A0`, Schritt 0.02 fest) | 0,02 rad·Stick je Tick | 1,0 rad/s | 1,2 rad/s (+20 %) | **ja** (Fahrzeugsteuerung) |

Alles andere, was gefunden wurde, rechnet in Sekunden: Bewegungsintegration (`FUN_00094560`), Timer
(`FUN_000445A0` Waffentimer, `FUN_00061A70`), Skript-Wartezeiten einzelner Waits, Animationen, Sound, Filme,
Intro-, Legal- und Death-Bilder sowie die Attract-Schwelle. Alle tick-gebundenen Stellen der Tabelle sind im
NTSC-Prototyp identisch (5, `timing_review.out` Abschnitt 5); bei 60 Hz entsprechen sie dem NTSC-Verhalten.
Entscheidung dazu: 12.2.

---

## 5. Vergleich mit dem NTSC-Prototyp (`timing_proto.out`)

* Methode: Für jede Retail-Funktion wird ein Bytemuster gebildet, in dem absolute Adressen (imm/disp im
  Image) und rel32-Sprungziele Platzhalter sind. Gesucht wird in der `.text` des Prototyps. Bei Treffern
  werden zusätzlich die Werte aller referenzierten `.rdata`-Konstanten verglichen.
* **Spielcode gesamt** (.text < 0x100000, Funktionen ≥ 16 Byte): **3112 von 3116** identisch. Abweichend
  sind `CApp::Init` 0xD26A0 (Reihenfolge, sysPAL/Sprachwahl), `FUN_000CC030` (Sound-Voice), `FUN_000E0690`
  und `XapiInitProcess`. Keine davon liest dt oder fps.
* **Alle 19 timing-relevanten Funktionen** sind identisch, die `.rdata`-Konstanten gleich: `Game::Init`
  (Prototyp 0xA2620, gleicher PAL/NTSC-Block), `Game::Frame` (0xA2BA0), `Frontend::Update`, Fade,
  Eingabesperre, `Intro::Update`, Zeitbild, Pause-ctor (Periode aus fps·0.25), Tastenwiederholung,
  Menü-Entprellung, Skript-Scheduler und -Interpreter, Physik, Kamera/Spieler, Spritzer, Flackern,
  Speicherpflege.
* Der Prototyp hat NTSC-Archive (`FutNTSC0x.img`), setzt also CApp+0xE8 = 1 und lief mit **dt = 1/60**.
  Retail-Code und Skriptdaten (4.3) sind gleich. Das tick-gebundene Verhalten bei 60 Hz ist damit das
  Verhalten, mit dem die Entwickler den Prototyp betrieben haben. Für die US-Retail-Fassung gilt dasselbe
  (gleicher Code mit NTSC-Zweig in der EU-XBE). **Vermutung:** Die US-XBE wurde aus demselben Quellstand
  gebaut. Sie liegt nicht vor (EU-Zertifikat Region 0x4, Prototyp 0x80000007; `tools\xbeinfo.py`).

---

## 6. Fester Zeitschritt bei 60 Hz (Aufgabe 4, `timing_loop.out`)

Befehle wie in startup.md 3.2, hier mit Rundungsdetails:
* `FUN_000D2B90` liest QPC (einmal QPF; 1/QPF wird als **float32** [0x2EB450] gespeichert, 0xD2BB6–0xD2BC2).
  `sec(t)` = t·f32(1/QPF) (`FUN_000D2B80`), `ticks(s)` = trunc(QPF·s) (`FUN_000D2B10`/`D2B50` mit `_ftol`).
* Kappung: Ist sec(elapsed) > f32(6·dt), dann elapsed = ticks(6·dt) (0xA2CE8–0xA2D1F). Bei 60 Hz sind das
  0,1 s statt 0,12 s.
* Schleife: `edi = 100` (0xA2D84). Solange sec(elapsed) ≥ dt (0xA2D99, `test ah,1`) und edi > 0 gilt:
  `vf10`, Eingaben, `cur->Update()`, elapsed −= ticks(dt) (0xA2EFD–0xA2F22). Danach gilt last = now − elapsed,
  die Restschuld bleibt also erhalten (0xA2F55–0xA2F73).
* **Rate:** ticks(1/60) = trunc(10⁷ · 0,016666668) = 166 666 → 60,00024 Hz bei QPF 10 MHz (PAL: 199 999 →
  50,00025 Hz). Die Abweichung von 4·10⁻⁶ ist vernachlässigbar. Xbox-QPF: 12 222 222 bzw. 14 666 666 Ticks.
* **Kappung/100 Schritte:** Bei 60 Hz laufen höchstens 6 (bzw. 5) Schritte pro Frame. Ein Hänger über
  100 ms kostet Spielzeit, das war in PAL ab 120 ms genauso. Gemessene Ladehänger liegen bei 32–40 ms
  (m3_report 7), also darunter. Die Frame-Arbeit liegt bei 2,3–3,1 ms, tragfähig sind über 300 Hz.
* **Phase:** Ohne Interpolation zeigt jeder präsentierte Frame genau den Stand nach n Schritten. Das Modell
  (`timing_loop.py`, 20 000 Frames, Abtastjitter 0,3 ms):

  | Präsentation | Startphase 0 | Startphase ½ |
  |---|---|---|
  | 120-Hz-Display, CPU-Takt 50 Hz + VSync (heute) | 16,9 % Frames mit 0 und 16,9 % mit 2 Schritten | 1 Schritt/Frame, aber Bildabstände 2/3 Vblanks (Ruckeln) |
  | 120-Hz-Display, Sync-Intervall 2 (60 Hz) | 11,6 % 0- und 11,6 % 2-Schritt-Frames | **immer 1 Schritt/Frame** |
  | 60-Hz-Display, Sync-Intervall 1 | wie oben | immer 1 Schritt/Frame |
  | 120/144 Hz ohne Takt | 50 %/58 % Frames ohne neuen Stand (Doppelbilder) | gleich |

  Die Startphase ist zufällig (QPC-Stand beim ersten Frame relativ zum Vblank). Sie driftet nur um
  0,67 Ticks pro Frame (Abschneiden von ticks(dt)), also etwa einen halben Schritt in 35 Minuten. Liegt sie
  an der Schrittgrenze, zappelt die Schrittzahl mit dem Jitter zwischen 0 und 2. Das ist sichtbares
  Mikroruckeln, obwohl die Präsentation exakt 60 Hz hat. **Abhilfe (Host, Vermutung):** Den Takt des
  Spieltimers auf die Präsentation einrasten lassen. Beispiel: einmalig die Restschuld CApp+0xC8 auf eine
  halbe Schrittweite setzen, wenn der Host die Vblank-Phase kennt. Oder eine Lockstep-Uhr: `FUN_000D2B90`
  (Funktionsanfang, hookbar) liefert bei stabiler 60-Hz-Präsentation „letzter Wert + ticks(dt)“, solange die
  echte Zeit nicht mehr als einen halben Schritt abweicht. Die Echtzeit-Nutzer (`Intro`/`Death`/Zeitbild)
  bekommen so weiterhin korrekte Sekunden. Erst messen (Observable 8.3), dann entscheiden.
* `g_frameDelta` [0x2E7B88] (echte Render-Dauer) und `FUN_000A46D0` laufen pro Frame. Bei 60 statt 50 Frames
  laufen sie öfter, ohne Folgen.

---

## 7. Empfehlung

1. **Zeitwerte patchen** (Abschnitt 2.3): fünf Operanden-Patches im PAL-Zweig, jeder gegen den ganzen
   Originalbefehl geprüft. Der Patch an 0xA273A ist wirkungslos, sorgt aber für den exakten NTSC-Zustand.
   Dazu die `.data`-Anfangswerte 0x2D6744 = 60.0f (wirksam: Tastenwiederholung 15 Ticks = 0,25 s) und
   0x2D6748 = 1/60 (Konsistenz), ebenfalls mit Prüfung des Originalwerts. Das Tag 0xA2744 und CApp+0xE8
   bleiben unverändert.
   * Mechanik: Der Host hat nur Funktions-Detours (`hooks.h`) und den fs-disp32-Patcher (`fs_patcher.h`). Für
     Operanden- und Datenpatches empfiehlt sich eine kleine Tabelle {Adresse, Originalbytes, neue Bytes, Name}
     mit derselben Prüfung gegen `core::ReadOriginalXbeBytes` und benannten Adressen in `guest.h`, z. B.
     `kGameInitPalFpsLoad` 0xA2714, `kGameInitPalDtLoad` 0xA2734 (Belege wie oben).
   * Konfiguration, **Vermutung** zum Namen: `[timing] rate = 60` (Standard) | `50` (Original-PAL für
     xemu-Vergleiche und Regressionen).
   * Kontrolle: Die vorhandene Logzeile von `GameInitProbe` (game_probes.cpp) muss
     `PAL; 60 fps, fixed time step 0.0167 s, movie tag "PAL"` zeigen.
2. **Präsentation im 60-Hz-Raster:** Bei Refresh = k·60 (60/120/240 Hz) VSync mit Sync-Intervall k statt des
   CPU-Takts. Bei anderen Raten (144/165 Hz) den CPU-Takt auf 60 Hz mit VRR oder Tearing nutzen
   (**Vermutung**, ungetestet). Mehr als 60 sichtbar verschiedene Bilder pro Sekunde gibt es ohne
   Interpolation nicht. Ein höherer Tick (z. B. 120 Hz) wäre technisch dieselbe Patchstelle, verdoppelt aber
   alle Stellen aus 4.10 gegenüber dem getesteten NTSC-Verhalten. Davon wird abgeraten. Mehr als 60 fps
   gehören zur Interpolation (Design R9/M10).
3. **Prüfskripte anpassen** (sie rechnen in Frames bei 50 Hz):
   * `m3_check.py` `DEFAULT_SCENES` = legal 150, tech_notice 400, loading 680, frontend 1000 Frames, bei
     50 Hz also etwa 3,2, 8,2, 13,8 und 20,2 s (erster Frame bei ~0,2 s). Bei 60 Hz entsprächen dieselben
     Frames etwa 2,7, 6,9, 11,5 und 16,9 s. „loading“ läge dann nur ~0,3 s nach dem Splash-Ende (~11,2 s).
     „frontend“ läge ~1 s nach dem Frontend-Eintritt (15,85 s), also knapp hinter dem 0,83-s-Fade. Ladehänger
     oder eine kürzere Ladephase verschieben das zusätzlich. Besser: Szenen in Sekunden angeben und mit der
     Rate umrechnen (180/480/816/1200 Frames bei 60 Hz).
   * M3-3: `--min-hz 49.5` → 59.4, `--max-work-ms 20` → 16,7; Kommentar „Dampfwolke alle 50 Frames“ → 1,0 s.
   * `m2_check.py` `INTRO_MIN_FRAMES = 1000`: Das Intro endet bei 60 Hz nach ~930–950 Frames (15,5–15,9 s ·
     60), der Puffer ist knapp. 1200 nehmen.
   * Timeouts `frames/50 + 90` → `frames/rate + 90`.
4. Die xemu-Referenzen bleiben PAL (50 Hz). Bildvergleiche zu festen **Sekunden** bleiben gültig (4.2, 4.4).
   Ausnahmen sind die ersten ~1 s nach dem Frontend-Eintritt (Fade) und alles, was 4.10 betrifft.

---

## 8. Laufzeit-Observablen (50 Hz gegen 60 Hz)

Basis 50 Hz: Logs von `run\m2fix\long` (600 s, 50-Hz-Takt) und `run\m3final\clang`. Zeiten aus den
Logstempeln `[t]` der Zeilen `game: context A -> B (CApp::Frame #n)`.

| # | Observable | Mechanik | 50 Hz (gemessen/Code) | 60 Hz erwartet | Messung |
|---|---|---|---|---|---|
| 8.1 | Zeitbasis nach `Game::Init` | Patch | `PAL; 50 fps, 0.0200 s, "PAL"` | `PAL; 60 fps, 0.0167 s, "PAL"` | Log `GameInitProbe` |
| 8.2 | Simulationsrate | fester Schritt | 50,000 Updates/s | 60,000 ± 0,01 Updates/s | Probe zählt `cur->Update()` (VTable-Slot 2) pro Sekunde |
| 8.3 | Schritte pro präsentiertem Frame | Phase (6) | – | Histogramm: ≥ 99 % genau 1; Anteil 0/2 melden | Probe: `Update`-Aufrufe je `CApp::Frame` |
| 8.4 | Legal-Screen | QPC, min = max = 6,0 s | 6,0 s | 6,0 s ± 1 Frame | Screenshot-Serie/Draw-Trace: erster und letzter Frame mit `legal.nif` |
| 8.5 | Splash/Tech-Notice `NI_Splash_PAL.nif` | QPC, max 5,0 s ohne Eingabe | 5,0 s | 5,0 s ± 1 Frame | wie 8.4 |
| 8.6 | Intro → Frontend | Summe | 15,85 s (#778, m2fix/long), 16,05 s (#781, m3final/clang) | 15,1–16,1 s; die Ladephase (~4,6 s) ist Frame-/I/O-gebunden und darf kürzer werden | Log |
| 8.7 | Frontend-Fade nach dem Eintritt | Tick, 50 Schritte | 1,00 s | 0,83 s (wie NTSC) | Helligkeitsrampe in einer dichten Screenshot-Serie |
| 8.8 | Frontend-Leerlauf → Attract (1. Zyklus) | dt-Sekunden, 60,0 s | 60,22 s (#778 → #3781) | 60,2 s ± 0,05 s. **50,2 s wäre ein Fehler** (tick-gebunden) | Log |
| 8.9 | Attract → Frontend (M3: Film fehlt) | 8 Clear-Frames + Enter | 0,200 s | ≈ 0,17 s (Frame-gebunden) | Log |
| 8.10 | Attract-Zyklus (Frontend → Attract, Folgezyklen) | 60 s Leerlauf + Neuladen | 64,64–64,66 s | ≈ 60 s + Ladezeit (≤ 64,7 s) | Log |
| 8.11 | Dampfwolke im Frontend | NIF-Zeit | Periode 1,0 s = 50 Frames | 1,0 s = 60 Frames | Region (515,105)–(600,160), Helligkeitsfolge (m3_report Befund 7) |
| 8.12 | Filmnamen | Tag | `Video\…-PAL.xmv` | unverändert | VFS-Trace |
| 8.13 | Frontend-Eingabesperre | Tick 30 | 0,60 s | 0,50 s | ab M4: früheste wirksame Taste nach dem Eintritt |
| 8.14 | Menü-Tastenwiederholung | Tick, Periode aus `.data`-fps | 0,24 s | 0,25 s (mit `.data`-Patch) / 0,20 s | ab M4: Cursorschritte pro Sekunde bei gehaltener Taste |
| 8.15 | Menü-Entprellung / „Neues Spiel“ | Tick 5 | 0,10 s | 0,083 s | ab M4 |
| 8.16 | Skript-Ereignisse im Level | dt, q(t) + 1 Ticks je Schleifendurchlauf (4.3) | – | einzelne Waits: gleiche Sekunden (±20 ms); Schleifen mit kurzen Waits: −17 % bis +11 % je Durchlauf, kumulativ (z. B. `RunSteam` 0,60 → 0,67 s) | ab M8: Zeitstempel von Skript-Ereignissen (z. B. Dampfzyklus „steamwait“, Trigger) |
| 8.17 | Physik (Integration) | dt-Euler | – | gleiche Sprunghöhe/Weite (< 1 %, Vermutung) | ab M8: Positionsspur des Spielers bei gleicher Eingabe |
| 8.17b | Starrkörper-Dämpfung (Review M3b) | Tick, ×0.98/×0.9 | Ausrollweg v0·0,98 s | v0·0,817 s (−17 %, wie NTSC) | ab M8: Gleitweg eines angestoßenen Moveables, Ausschwingzeit einer Wippe |
| 8.18 | Spielzeit-Uhr | dt | – | 1 s Spielzeit = 1 s Echtzeit | ab M8: Session +0x178/+0x17C (`FUN_000C8BA0`) gegen Logstempel |
| 8.19 | Chicken Walker: Drehrate (Review M3b) | Tick, Schritt 0.02 fest | 1,0 rad/s bei vollem Stick | 1,2 rad/s (wie NTSC) | ab M4/M8: Gierwinkel +0x54 über der Zeit bei vollem Stick |

8.4 bis 8.11 lassen sich schon mit dem M3-Stand messen: ein Lauf mit `[timing] rate = 50` und einer mit
`= 60`, je ≥ 150 s (zwei Attract-Zyklen). Dazu dichte Screenshots in den Fenstern 0–17 s und um den
Frontend-Eintritt.

---

## 9. Offene Punkte und Grenzen

* Die Suche nach tick-gebundenem Code ist heuristisch (Muster und Konstanten). Pro-Tick-Glättungen mit
  beliebigen Faktoren lassen sich statisch nicht vollständig finden. Weil der Code mit dem NTSC-Prototyp
  übereinstimmt (5), ist das Restrisiko auf das NTSC-Verhalten begrenzt: Was dort tick-gebunden ist, läuft
  bei 60 Hz so wie in der NTSC-Fassung. Das Review (12) hat mit dieser Grenze drei spielrelevante Stellen
  nachgetragen (Starrkörper-Dämpfung, Walker, Skriptschleifen). Weitere Glättungen oder Dämpfungen pro Tick
  sind wahrscheinlich; ihre Wirkung entspricht aber ebenfalls der NTSC-Fassung.
* Die Skriptanalyse dekodiert linear. Daten zwischen Funktionen ergeben Rauschen (`timing_script.out`
  Abschnitt 2, „undecoded“). Die Native-IDs außer den Rechenoperationen sind nicht benannt.
* Die Bedeutung von [0x2E7B68] ist ungeklärt, aber ohne Wirkung.
* Das Phasenmodell (6) ist eine Simulation, keine Messung. Die Startphase und den Jitter misst 8.3.
* Startwert des Frontend-Fades (1,0) nicht belegt; `FUN_00024AA0` wurde nicht im Detail gelesen.

---

## 10. Korrekturen und Ergänzungen zu startup.md

* 3.4 „Idee für den Port: 60 Hz über dt/fps (0x2D6748/0x2D6744) erzwingen“: Die Globals direkt zu
  überschreiben reicht nur nach `Game::Init`. Sauberer ist der Operanden-Patch. fps wird zur Laufzeit nie
  gelesen. Wirksam ist allein der `.data`-Anfangswert, und zwar über den Pause-ctor (2.4, 4.7).
* 3.4 „Movie-Namen ('%s-%s' mit PAL/NTSC)“: bestätigt, einziger Leser 0xA2424 in `CApp::PlayMovie` 0xA2380.
* 3.2 „Spielzustände ändern sich im 50-Hz-Raster“: Das gilt für das Tick-Raster. Animationen, einzelne
  Skript-Waits und Timer rechnen in Sekunden (4); Dämpfungen, Glättungen, der Walker und Skriptschleifen mit
  kurzen Waits sind tick-gebunden (4.10, 12).

---

## 11. Skripte

| Skript | Zweck | Ausgabe |
|---|---|---|
| `timing_common.py` | XBE (Retail, Prototyp), Funktionsliste, Decompilat, capstone-Index aller Ghidra-Funktionen **und** Gap-Segmente (Cache `scripts\out\timing_index_v2.pkl`, git-ignoriert) | – |
| `timing_patch.py` | Timing-Block 0xA26BD–0xA276A, Konstanten-Nutzer, Emulation PAL/NTSC/PAL+Patch, Patch-Tabelle mit Originalprüfung | `timing_patch.out` |
| `timing_globals.py` | Leser/Schreiber aller Zeit-Globals, Basis-Scan, dt-Statistik, Frame-Delta-Nutzer, Leser vor `Game::Init`, Pause-ctor, Tag-Leser | `timing_globals.out` |
| `timing_consts.py` | fest kodierte 0.02/1/60/1/50/0.04/1/30/1/25/25/30/50/60 in `.rdata` und als Immediate | `timing_consts.out` |
| `timing_tickscan.py` | heuristische Suche nach Tick-Zählern | `timing_tickscan.out` |
| `timing_extract.py` | holt `scripts\*.ful/*.dbg` aus den `.img` der EU-ISO (nur lesend) nach `run\timing60\pal` | Konsolenliste |
| `timing_script.py` | PAL-gegen-NTSC-Skripte, Wait-/Timer-Operanden, Schleifenklassen, Wait-Quantisierung | `timing_script.out` |
| `timing_proto.py` | Retail gegen Prototyp (relozierte Bytemuster, `.rdata`-Werte), ganzer Spielcode, Prototyp-Zeitblock | `timing_proto.out` |
| `timing_loop.py` | Modell der Schrittschleife (Rundung wie im Binary) für 50/60 Hz und verschiedene Präsentationsarten | `timing_loop.out` |
| `timing_review.py` | Review M3b (12): Starrkörper-Dämpfung, Walker-Schritt, Kamera-Unterschritte, Overlay, abgelehnte Pausemenü-Sperre, Skriptschleifen (automatisches `Wait(0)`, Periode je Durchlauf, Klassifikation), Prototyp-Vergleich dieser Funktionen | `timing_review.out` |

Reihenfolge: `timing_extract.py` einmal, danach alle anderen in beliebiger Reihenfolge (`python <skript>.py`).

---

## 12. Korrekturen nach dem Review (M3b, Phase 5)

Das Review der M3b-Umsetzung hat vier Befunde zu diesem Dokument erhoben. Belege: `timing_review.py` /
`timing_review.out` (Abschnittsnummern unten), Skripte des Reviews unter `run\review_m3b_timing` (git-ignoriert).

### 12.1 Befunde und Prüfung

| # | Befund | Prüfung | Ergebnis | Korrigiert in |
|---|---|---|---|---|
| 1 | Starrkörper-Dämpfung ist tick-gebunden, nicht dt-skaliert | `timing_review.out` 1: neun `fmul` mit 0.9/0.98 ohne dt in `FUN_000AFA30`; einmal pro Tick über Physikwelt vf2 0xAE7E0; ctor-Aufrufer `FUN_00064DD0`/`00073FC0`/`0007F960`/`00095290`; Prototyp identisch (0xAF9D0) | **bestätigt** | 0, 4.5, 4.10, 8.17b; `game_timing.h`; loader_design B40 |
| 2 | Chicken Walker zielt und dreht mit fest kodiertem Schritt 0.02 | `timing_review.out` 2: einziger Schreiber 0x43EEC (`C7 46 64 0A D7 A3 3C`), Leser 0x44BA5/0x44BB2; `FUN_000445A0` liest dt an fünf Stellen (Timer), läuft also pro Tick; Prototyp identisch (0x43D30/0x44550) | **bestätigt** | 4.8, 4.10, 8.19 |
| 3 | Jede Skriptschleife endet mit `Wait(0)`; kurze Waits summieren sich | `timing_review.out` 4: 3823 von 3823 unbedingten Rücksprüngen direkt nach `push_imm 0; WAIT`; Periodentabelle je Durchlauf | **bestätigt** | 4.3, 4.10, 8.16 |
| 4 | Kamerafeder in festen 0,01-s-Unterschritten; Overlay ±0.03 pro Update; Eingabesperre im Pausemenü +0x84 = 30 Ticks | `timing_review.out` 3: Schleife 0x179B3–0x179C4 (2 Unterschritte je Tick bei 0.02f und 1/60f), Aufruf mit dt 0x19CC2; `FUN_000A8490` 0xA84AC/0xA84F1 | Kamera und Overlay **bestätigt**; Pausemenü-Sperre **abgelehnt**: `dec [esi+0x84]` in `FUN_000A55D0` (0xA5640) und `FUN_000A62B0` (0xA64E4) steht in der Schleife, die Elemente aus einem Array entfernt (+0x78 Array, +0x7C Kapazität, +0x80 Größe). +0x84 ist dort ein Elementzähler. Der einzige Schreiber von +0x84 = 30 ist das Frontend (0x21AEB, schon in 4.2) | 4.1, 4.5, 4.10 |

### 12.2 Entscheidung: NTSC-Parität

Für alle tick-gebundenen Stellen, auch die spielrelevanten aus 12.1, gilt bei 60 Hz bewusst das Verhalten der
NTSC-Fassung. Es wird **kein** PAL-Ausgleich gepatcht. Gründe:
* Die NTSC-Fassung ist ein reales, von den Entwicklern abgestimmtes und ausgeliefertes Verhalten: gleicher
  Code (5, `timing_review.out` 5), gleiche Skripte, 60 Hz. Ein Teilausgleich nur einzelner Stellen ergäbe
  dagegen eine Mischung, die es nie gab. Die Kamera, die UI und 2979 Skriptschleifen blieben ohnehin
  tick-gebunden.
* Ein vollständiger PAL-Ausgleich ist statisch nicht erreichbar (9: weitere Glättungen pro Tick sind
  wahrscheinlich).
* Der Nutzer will mindestens 60 fps und ausdrücklich kein 50-Hz-PAL-Timing. Für Vergleiche mit dem PAL-Original
  bleibt `[game] tick_rate = 50`.

Falls M8-Messungen (8.17b, 8.19) einen Ausgleich doch nahelegen, wären die Stellen bekannt:
* Dämpfung: die neun `fmul`-Operanden 0xAFB0D–0xAFBCB (disp32) auf eigene Konstanten 0.9^(50/60) = 0.91594
  bzw. 0.98^(50/60) = 0.98331 umbiegen. Die geteilten `.rdata`-Werte bleiben unangetastet (0x293B68 hat
  27 Nutzer, 0x293B74 hat 4). Dazu braucht der Host eine Konstante im Gastadressraum.
* Walker: Immediate an 0x43EEC (`C7 46 64` imm32) auf 1/60 setzen. Dafür muss `ValidateTimingPatchTable` die
  Kodierung `mov [reg+disp8], imm32` zulassen. Zusätzlich die ±0.05-Klemme des Nachdrehens auf 0.05·50/60.
* Skriptschleifen: nicht sinnvoll patchbar (Bytecode in den Archiven).

