# Meilenstein M5: Audio (DSOUND auf XAudio2, Xbox-ADPCM, Musik-Streaming, 3D-Klang, I3DL2-Hall) – Bericht

Stand: 2026-10-12 (Phase 5, nach Behebung der Review-Befunde) · Ziel-XBE: `orig\retail_eu\default.xbe` (Retail EU) · Verbindliches Design: [`docs/loader_design.md`](loader_design.md) (E7, E10, Abweichungen 8.12–8.15) · Analyse: [`analysis/loader/m5_audio.md`](../analysis/loader/m5_audio.md), [`analysis/loader/avi.md`](../analysis/loader/avi.md) (DSOUND) · Schnittstellen: [`src/INTERFACES.md`](../src/INTERFACES.md) 4.29, 4.30 · Nichts committet.

## 1. Ergebnis in Kürze

**Das Spiel hat Ton.** Die 40 DSOUND-Einstiege und die 5 XMediaObject-Stream-Methoden sind keine stummen Stubs mehr, sondern eine HLE über XAudio2 2.9.

- **Was spielt:** Frontend-Musik (gestreamt, mit Schleife), Menüklänge und die deutsche Menüstimme, in Level 1-1 die Planet-Express-Musik, Sprache, Schritte, Sprünge, Landungen, Angriffe und die Umgebungsgeräusche (soweit sie nahe genug an der Kamera sind) – mit 3D-Richtung, Entfernungsdämpfung nach DSOUND und I3DL2-Raumhall.
- **Gemessen, nicht gehört:** Alle Aussagen dieses Berichts stammen aus Mitschnitten (`[audio] capture_wav`) und deren Abgleich mit den unabhängig dekodierten Originaldateien der Disc. **Niemand hat zugehört.** Der Nutzer prüft nach Gehör (Abschnitt 8).
- **Pegel exakt nach DSOUND:** 2D-Klänge bei −6,0 dB (600 mB Headroom), Musik-Regler −6/−16/−64/−7 dB auf 0,08 dB genau, Tonhöhe 1,0000, Musiktempo = Xbox-Tonhöhenquantisierung (0,999959).
- **Robust:** Ohne Ausgabegerät, mit `[audio] enabled = 0`, bei Geräteverlust (Fernseher aus) und bei wiederkehrendem Gerät läuft das Spiel gleich weiter; die Musik setzt nach einem Wechsel auf ±0,08 ms an der richtigen Stelle fort. Gerätearbeit läuft seit Phase 5 in einem eigenen Host-Thread (kein Ruckeln mehr beim Wechsel).
- **Launcher:** neuer Reiter „Audio“ mit Geräteauswahl, Lautsprecheranordnung, Gesamtlautstärke, Hall, Vorne/hinten-Korrektur, Geräteprüfung und Testton.
- **Prüfungen:** `tools\m5_check.py` **ALL PASS** in RelWithDebInfo, Debug und clang-cl am echten 5.1-Endpunkt (Fernseher), dazu ohne Gerät, mit abgeschaltetem Ton und mit simuliertem Geräteverlust; `fut_tests` 433/433 in allen drei Bauten; 0 Warnungen; m1–m4 ohne M5-Regression (Anzeige-Kriterien bei 120 Hz scheitern auch mit dem M4-Bau, Abschnitt 7 und 10).
- **Review:** 17 Befunde nachgeprüft, alle angenommen (einer nur als Formulierung) und behoben. In der Endprüfung kamen zwei eigene Fehler der Phase-5-Korrekturen ans Licht und wurden behoben (Abschnitt 9: R2a, R9a).

## 2. Was gebaut wurde

| Teil | Dateien | Inhalt |
|---|---|---|
| DSOUND-HLE | `src\host\audio\dsound_hle.*`, `dsound_mix.*`, `dsound_probe.cpp` | Gerät, Puffer, Streams (XMediaObject), 2D-Pegel/Mixbins, Kategorien über die Probe `Game::SoundVoiceCreateDsObject` (0xCC150), Start beim Commit des Ticks |
| Engine | `audio_engine.*`, `audio_api.h` | XAudio2-Graph (Mastering, ein Submix je Kategorie, Hall-Submix), Software-Mixer ohne Gerät, Gerätewechsel in beide Richtungen mit Positionserhalt, Geräte-Thread mit `IMMNotificationClient`, Master-Stufe (Begrenzer, LFE-Tiefpass, Gesamtlautstärke) |
| Dekoder | `codec_adpcm.*`, `codec_format.*`, `codec_decoder.cpp` | Xbox-ADPCM (bitgleich mit ffmpeg `adpcm_ima_xbox`), PCM8/PCM16, Xbox-Tonhöhenmodell |
| 3D und Hall | `spatial_model.*`, `spatial_x3d.*`, `reverb_i3dl2.*`, `reverb_software.*` | DSOUND-Entfernungsmodell exakt (FUN_00249893), X3DAudio für die Richtung, fünf I3DL2-Presets aus der XBE-Tabelle, XAudio2-Hall (auch im Software-Mixer) |
| Mitschnitt | `capture_*.*` | `[audio] capture_wav` (Mischung vor Begrenzer und Gesamtlautstärke), Teilmischungen je Kategorie und Hall, Ereignisliste |
| Werkzeuge | `tools\audio_compare.py`, `tools\m5_check.py`, `tools\input_scripts\m5_frontend.txt` | Abgleich mit den Originaldateien (Kreuzkorrelation, Tonhöhe, Pegel je Kanal, Balance), M5-Prüfung A1–A12 plus alle M4-Kriterien |
| Launcher | `launcher\src\core\audio_output.*`, `launcher\src\ui\audio_panel.*` | Reiter „Audio“, Geräteliste (WASAPI), Testton |
| Tests | `tests\audio_*_tests.cpp` | nur synthetische Signale; zwei optionale Fälle lesen Spieldaten aus `run\` (`FUT_AUDIO_CODEC_REFS`, `FUT_AUDIO_DSSTDFX`) |

## 3. Aufbau des Tons

```
Spiel (Gast-Thread)                         Host
SoundManager (app+0xF0)                     dsound_hle.cpp (nur Gast-Thread schreibt Gastspeicher, E10)
 ├─ SoundVoice → DSOUND-Puffer  ──────────▶ Objekt je Puffer/Stream ─▶ Decoder (ADPCM/PCM8 → PCM16)
 ├─ Musik-Stream (10 Pakete à 0,3 s) ─────▶ Process: E_PENDING sofort; fertige Pakete schreibt DoWork
 ├─ SetVolume/Frequency/MixBins/3D ───────▶ Werte je Tick, angewandt bei CommitDeferredSettings (ein Engine-Batch)
 └─ DirectSoundDoWork (jeder Tick) ───────▶ Pump: Gerätewechsel übergeben (Arbeit im Thread „audio device worker“)

audio_engine.cpp
 Source-Voice je Objekt ─▶ Submix Effekte / Sprache / Musik / Film ─▶ Mastering (48 kHz, Kanäle des Geräts)
                       └─▶ Hall-Submix (1 Eingang, XAudio2-Reverb mit I3DL2-Preset) ─┘
 Master-Stufe im Tap-XAPO: LFE-Tiefpass 120 Hz → [Mitschnitt] → Begrenzer −0,1 dBFS → master_volume
 Ohne Gerät: Software-Mixer mit demselben Graphen (Sinc-Resampler, derselbe Hall), 10-ms-Blöcke in Echtzeit;
 ohne Zuhörer (kein Gerät, kein Mitschnitt) nur Positionen und Pufferenden.
```

- **Pegelmodell (m5_audio.md 3.6):** 2D-Stimme = `lVolume − 600 mB` + Mixbin-Lautstärke, im 1/64-dB-Register (höchstens 63,98 dB Dämpfung, −10000 mB klingt also mit −63,98 dB). 3D-Stimme = `lVolume` + DSOUND-Entfernungsterm (eigene 6-Punkt-Rolloff-Kurve des Spiels, Stumm-Flag hinter `MaxDistance`), dazu LFE-Bin und I3DL2-Send.
- **Kategorien:** Effekte 0, Sprache 1, Musik 2 (Spielregler unter „Optionen“, im Spielstand gespeichert), Film 3 (nicht markierte Objekte, ab M6 XMV).
- **Richtung:** X3DAudio statt Xbox-HRTF/Crosstalk-DSP. Auf 5.1 nur über die vier Ecken, nie über den Center (die 3D-Mixbins des Spiels haben keinen Center-Bin); kein Kanal über 0 dB. `[audio] positional = corrected` (Standard) dreht die beim Original vertauschte Vorne/Hinten-Richtung um.
- **Hall:** Das Spiel wählt eines von fünf I3DL2-Presets (Tabelle 0x2D6818, Level 1-1: „Padded Cell“, Nachhall 0,17 s); umgesetzt mit dem XAudio2-Reverb. Der DSP-Code aus `dsstdfx.bin` wird nur geprüft, nicht emuliert.

## 4. Was wo spielt (gemessen)

Ausgabe in der Endprüfung: XAudio2 auf dem LG-Fernseher (5.1 mit Seitenlautsprechern, Maske 0x60F, 48 kHz), `master_volume = 2 %`. Werte aus `run\m5-final\rel\m5_check\` (Pegel relativ zur Originaldatei, Kanäle FL / FR / FC / LFE / SL / SR).

**Frontend (147-s-Lauf, erster Start ohne Spielstand):**

| Klang | Wo | Pegel | Weitere Messung |
|---|---|---|---|
| Musik `01 - front-end.wav` (gestreamt) | nur FL/FR (0,0 % der Energie anderswo) | −6,01 / −6,03 dB unter 2 kHz (Vollband −6,13/−6,21: Höhendämpfung des XAudio2-Resamplers) | 64/64 Abschnitte à 2 s gefunden, NCC 0,971, Tempo 0,999960 (= Xbox-Tonhöhe), Schleife mit 103,0 ms Lücke (Dateikopf: 102,9 ms), 0 Aussetzer, 0 Glitches |
| Menüklänge `scroll`/`go_to`/`go_back`, Menüstimme | nur FL/FR, mittig | −6,03 … −6,24 dB (6/6 gefunden) | Tonhöhe 1,0000 |
| Mischung | | Spitze −5,5 dBFS vor Begrenzer und Gesamtlautstärke | 0 übersteuerte Samples |

**Level 1-1 (40-s-Lauf: Neues Spiel, laufen, Kamera drehen, springen, angreifen, Pause):**

| Klang | Wo | Pegel | Weitere Messung |
|---|---|---|---|
| Musik `01 - planet express.wav` | FL/FR | −6,08 / −6,08 dB | NCC 0,988, 0 Aussetzer; das Pausenmenü hält sie an, danach geht es an derselben Stelle weiter |
| Sprache (Fry), Menüklänge | FL/FR mittig | ≈ −6 dB (Sprache −6,2 dB; `select.wav` −6,9 dB bei mB 0 laut Ereignisliste – Einzelmessung beim Wechsel ins Level, nicht weiter untersucht) | |
| Schritte, Sprung, Landung (3D, ≈ 20 Einheiten vor der Kamera) | FL und FR je −0,0 … −2,2 dB, je nach Kameradrehung links oder rechts betont; Center −200 dB; Seiten −70 … −120 dB; LFE-Bin −0,0 dB (am Ausgang auf 120 Hz tiefpassgefiltert) | ≈ 0 dB | Tonhöhe 0,9997–0,9999 (Xbox-Quantisierung bei 22050 Hz) |
| Umgebungsschleifen (`SteamLoop*`, `Cable_loop`, `FanSpinning`, `ElecBox-Loop`, `Heart`, Eule …) | | −63,98 dB | liegen 45–470 Einheiten von der Kamera, hinter ihrer `MaxDistance` (15–50): DSOUND dämpft sie auf das Registerminimum. Korrekt, kein Fehler (8.14 I7); im 33-min-Dauerlauf werden `ElecBox-Loop` und die Eule hörbar, sobald Fry nahe ist |
| Hall (Stem „reverb“) | alle Kanäle außer FC/LFE | aktiv −53 dBFS, etwa 21 dB unter den Effekten | Preset „Padded Cell“ |
| Mischung | | Spitze −2,6 dBFS | 0 übersteuerte Samples, höchstens 29 gleichzeitige Stimmen |

**Stereo (Software-Mixer, ohne Gerät oder mit `enabled = 0`):** Musik −6,00/−6,00 dB, Menüklänge −5,99 … −6,01 dB, 3D-Klänge links/rechts mit 26–27 dB Spannweite, 14 % der lauten Fenster links, 12 % rechts lauter.

**Optionen-Regler** (Integrationsphase, `run\m5-integ\r3`): Musik 1,0 → −6,08 dB (erwartet −6,00), 0,50 → −16,06 (−16,00), 0,00 → −64,02 (−63,98, Registergrenze), 0,95 → −7,05 (−7,00).

**Dauerlauf** (Integrationsphase, 33 min Level 1-1): 10 300 Stimmen erzeugt, höchstens 33 gleichzeitig, 5315 Musikpakete ohne Unterlauf, 0 Glitches; vor dem Begrenzer 0,00045 % der Samples ≥ 0 dBFS (Spitze +3,8 dBFS bei vielen 3D-Klängen auf einmal) – genau dafür ist der Begrenzer da (in Phase 5 im Spiel nachgewiesen, Abschnitt 7).

## 5. Einstellungen

### 5.1 Für Spieler: Reiter „Audio“ im Launcher

Bildschirmfoto aus dem Launcher-Test: `build\m5-final-launcher-rel\screenshots\de_tab_audio_full.png` (Bauverzeichnis, nicht im Repository).

- **Ton** an/aus. Aus: Das Spiel läuft gleich weiter (stumm), Spielverhalten und Zeitpunkte bleiben identisch.
- **Ausgabegerät:** „Windows-Standardgerät (…)“ oder ein bestimmtes Gerät. Ist es nicht angeschlossen, nimmt das Spiel das Standardgerät und wechselt zurück, sobald es wieder da ist.
- **Lautsprecher:** „Wie das Gerät“, Stereo oder 5.1. Hinweis, wenn mehr Lautsprecher gewählt sind, als das Gerät Kanäle hat (Windows mischt dann herunter).
- **Gesamtlautstärke** (0–100 %), **Hall** an/aus, **Vorne/hinten**: „Korrigiert“ (Standard), „Wie auf der Xbox“ (Geräusche vor der Kamera klingen wie von hinten), „Aus“ (alles von vorn, Lautstärke nach Entfernung bleibt).
- **Ausgabe prüfen:** verwendetes Gerät mit Kanälen und Abtastrate, **Testton** (ein Ton je Lautsprecher in Kanalreihenfolge, dann alle; mit Gerät, Anordnung und Gesamtlautstärke des Reiters, ohne Spieldaten), **Geräte neu suchen** (z. B. nachdem der Fernseher eingeschaltet wurde).
- Musik-, Effekt- und Sprachlautstärke stellt man wie auf der Xbox im Spiel unter „Optionen“ ein.

### 5.2 Konfiguration (`futurama.ini`, Abschnitt `[audio]`)

| Schlüssel | Werte (Standard) | Bedeutung |
|---|---|---|
| `enabled` | 0/1 (1) | Tonausgabe |
| `device` | Endpunkt-ID (leer = Windows-Standardgerät) | `IMMDevice::GetId`-Zeichenkette, so wie der Launcher sie speichert |
| `master_volume` | 0–100 (100) | Gesamtlautstärke in % |
| `limiter` | 0/1 (1) | Übersteuerungsschutz (Spitzenbegrenzer bei −0,1 dBFS, darunter wirkungslos) |
| `speakers` | `auto` \| `stereo` \| `5.1` (`auto`) | Kanäle der Ausgabe |
| `reverb` | 0/1 (1) | I3DL2-Raumhall |
| `positional` | `corrected` \| `xbox` \| `off` (`corrected`) | Richtung der 3D-Geräusche |
| `trim_3d` | −20 … 20 dB (0), Entwickler | hebt/senkt alle 3D-Geräusche samt Hall gegenüber Musik/Sprache/Menü (Abgleich mit der Xbox) |
| `capture_wav`, `capture_format` (`float`\|`pcm16`), `capture_stems`, `event_log` | Entwickler | Mitschnitt der Mischung (vor Begrenzer und Gesamtlautstärke), Teilmischungen, Ereignisliste |
| `device_test` | `off` \| `absent` \| `loss`, Entwickler | Prüfen ohne Gerät bzw. mit simuliertem Geräteverlust alle 20 s |
| `dummy_streams` | veraltet | wird angenommen, ohne Wirkung |

Alle Schlüssel stehen im einen Konfigurationsschema (`futurama.exe --print-config-schema=<datei>`) mit deutschen und englischen Bezeichnungen.

## 6. Wie ohne Ohren geprüft wurde

1. **Mitschnitt:** `[audio] capture_wav` schreibt die Mischung (Float, 48 kHz, Kanäle der Ausgabe) direkt aus dem Tap-XAPO der Mastering-Voice, `capture_stems` zusätzlich Musik, Sprache, Effekte, Film und Hall, `event_log` jede DSOUND-Aktion mit Name, Pegel, Frequenz, 3D-Position und Entfernung. Ohne Gerät liefert der Software-Mixer denselben Graphen.
2. **Referenzen:** `tools\audio_compare.py` liest die Originaldateien aus den Archiven der eigenen ISO im Speicher und dekodiert sie unabhängig (PyAV/ffmpeg `adpcm_ima_xbox`, PCM). Nichts davon liegt im Repository; Mitschnitte und Auszüge nur unter `run\`.
3. **Abgleich:** Kreuzkorrelation (Zeitversatz, NCC), Tonhöhe/Tempo, Pegel je Kanal (kleinste Quadrate, seit Phase 5 mit nachgesampelter und auf Bruchteile eines Samples ausgerichteter Referenz), Balance, Lücken/Aussetzer, übersteuerte Samples.
4. **`tools\m5_check.py`** (Kriterien A1–A12, siehe Kopf des Skripts): Engine und Mitschnitt laufen (A1), DSOUND-Objekte plausibel (A2), keine hängenden Stimmen (A3), Pegel je Kategorie (A4), Originalklänge gefunden (A5), keine Übersteuerung (A6), Musik ohne Aussetzer außer der eigenen Schleifenlücke (A7), kein Hänger/Absturz (A8), 3D-Panning mit Klang auf beiden Seiten (A9), 2D nur vorne links/rechts (A10), 2D-Pegel −6 dB (A11), Begrenzer-Statistik passend zur Spitze (A12) – plus M4-1 … M4-9 im Level-Lauf.

## 7. Prüfungen

| Prüfung | Bauten | Ergebnis |
|---|---|---|
| Bauen mit `FUT_WERROR=ON` | Debug `build\m5-final`, RelWithDebInfo `build\m5-final-rel`, clang-cl Debug `build\m5-final-clang` | `build.cmd: OK`, 0 Warnungen |
| `fut_tests` | alle drei | **433/433** (1 manueller Test übersprungen); dazu die optionalen Spieldaten-Fälle (`FUT_AUDIO_CODEC_REFS`, `FUT_AUDIO_DSSTDFX`) 99/99 Prüfungen |
| Generierte Dateien | `python tools\gen\gen_all.py --check` | aktuell, XBE-Abgleich stimmt |
| Launcher mit `LAUNCHER_WERROR=ON` | Debug, Release | 0 Warnungen, ctest 4/4 |
| `m5_check` mit Gerät (5.1, Fernseher an) | RelWithDebInfo (mit Rumble- und Fehlerskript-Lauf), Debug, clang (`--no-rumble`) | **ALL PASS** in allen drei (`run\m5-final\m5_check_{rel,debug,clang}_final.txt`) |
| `m5_check` ohne Gerät (`device_test = absent`) | RelWithDebInfo, `--device-optional` | **ALL PASS** (Software-Mixer stereo) |
| `m5_check` mit `[audio] enabled = 0` | RelWithDebInfo | **ALL PASS**, gleiches DSOUND-Verhalten |
| `m5_check` mit `device_test = loss` (6 Wechsel im Frontend, 1 im Level) | RelWithDebInfo | **ALL PASS** – auch A7, das in der Integrationsphase einen Abschnitt als „verloren“ meldete; längste Frame-Arbeit im Frontend 7,8 ms, 0 Frames über einer Periode (vorher 30,7 ms, 5 Frames) |
| Begrenzer im Spiel (`master_volume = 100`, `trim_3d = 10`) | RelWithDebInfo | Spitze vor dem Begrenzer +5,16 dBFS, Begrenzer auf 178 286 Frames aktiv, tiefste Absenkung 5,30 dB (erwartet 5,26), A12 PASS (A6 schlägt hier absichtlich an) |
| Anstieg beim Einsatz (Effektregler 0,25, 62 Starts von `scroll.wav`) | RelWithDebInfo | alle ab dem ersten 5-ms-Fenster bei −21,01 dB (vor Phase 5 die ersten 20 ms +15 dB zu laut) |
| Anstieg beim Einsatz, Endstand am 5.1-Gerät | RelWithDebInfo | 61/61 Starts mit −750 mB: erstes 5-ms-Fenster innerhalb 0,1 dB des Rests (`run\m5-final\onset2_result.txt`) |
| `m4_check` mit `[audio] enabled = 0` ohne Mitschnitt (Software-Mixer nur mit Positionen, R15) | RelWithDebInfo | **ALL PASS**; 3935 Blöcke = 39,4 s nur Positionen |
| `m1_check` | Debug 3 Läufe, RelWithDebInfo 2 Läufe | **ALL PASS** |
| `m2_check` | Debug d3d11 und Null-Renderer | **ALL PASS** |
| `m3_check` **ohne** `--min-*` (Anzeige an, 120 Hz) | RelWithDebInfo | **ALL PASS**, auch M3b-7 (99,19 % der Bilder genau 2 Refreshes); in M4 und der M5-Integration mangels Anzeige nicht messbar |
| `m3_check` mit `--min-*` | Debug, RelWithDebInfo, clang | **ALL PASS** |
| `m3c_check` (7 Varianten) | RelWithDebInfo; Debug | Rel **ALL PASS**. Debug: im ersten Lauf C2 in „max“ (8× MSAA, Supersampling 5760×4320, Debug-Layer) knapp verfehlt (2 Frames über einer Periode, 59,67 Hz); zwei Wiederholungen von „max“ **PASS**, der M4-Bau zeigt dort dasselbe Bild (1 Frame über einer Periode) – Grenzfall der Debug-Leistung bei eingeschalteter Anzeige, kein M5-Effekt |
| `m3d_check` (9 Varianten), mit und ohne `--display-optional` | RelWithDebInfo | 63 PASS, 4 FAIL: **D1** (i120/d120/f120: nur 43 % der Bilder genau 1 Refresh, ≈ 2000 „zu früh“) und **L1** (Just-in-time 680 von 2970). Gegenprobe mit dem **M4-Bau ohne Ton** und mit `[audio] enabled = 0`: dieselben Werte (D1 43,4 % / 43,1 %, L1 683 von 2970) – Anzeige-Umgebung des Fernsehers bei 120 Hz, keine M5-Regression. Ohne `--display-optional` zusätzlich einmal M3b-7/M3-3 in c60 (94,6 % auf Periode); in den Gegenproben besteht c60 mit und ohne Ton (der M4-Bau verfehlte M3b-7 dort einmal mit 98,96 %) |
| `m4_check` | RelWithDebInfo `--runs 2`, Debug, clang `--no-rumble` | **ALL PASS** (M4-1 … M4-9; beide Rel-Läufe Tick für Tick gleich) |
| Paket | `python tools\package.py --game-build build\m5-final-rel --launcher-build build\m5-final-launcher-rel` | `dist\FuturamaPC\`, 24 Dateien, 30,4 MB; `LIESMICH.txt` mit Abschnitt „Ton“ |

## 8. Worauf der Nutzer beim Hören achten sollte

Bitte mit Launcher-Standardwerten starten (Gesamtlautstärke zuerst niedrig), einmal über den Fernseher/5.1 und einmal über Kopfhörer/Stereo.

**Frontend**
1. Läuft die Menümusik ohne Knacken, Aussetzer oder Leiern? Nach gut zwei Minuten beginnt sie von vorn – die kurze Pause (≈ 0,1 s) gehört zur Datei.
2. Sind Menüklänge (Blättern, Bestätigen, Zurück) und die Menüstimme gut hörbar, nicht zu laut gegenüber der Musik?
3. Optionen → Musik-/Effekt-/Sprachregler: ändert sich die Lautstärke sofort und hörbar? Hat der allererste Klang nach dem Verstellen des Effektreglers die neue Lautstärke (kein lauter Anfang)?

**Level 1-1 (Planet-Express-Hangar)**
4. **Verhältnis 2D/3D:** Musik und Sprache liegen bei −6 dB, Schritte und Sprünge nahe der Kamera bei etwa 0 dB. Klingen Schritte/Sprünge im Vergleich zur Musik zu laut? Falls ja: `[audio] trim_3d` (z. B. −3 oder −6) ausprobieren und den Wert melden.
5. **Richtung:** Beim Drehen der Kamera wandern die Schritte von links nach rechts. Kommen Geräusche vor Fry auch von vorne (5.1: aus den Frontlautsprechern, nicht von hinten)? Zum Vergleich `Vorne/hinten = Wie auf der Xbox`.
6. **Hall:** Ist der Raumhall hörbar, aber dezent? Zu stark oder gar nicht wahrnehmbar?
7. **Subwoofer (5.1):** Brummen oder doppelte Schritte aus dem Subwoofer? (Der LFE-Kanal ist seit dieser Phase auf 120 Hz begrenzt.)
8. **Umgebung:** Zischen (Dampf), Kabel, Lüfter, Herzschlag-Maschine, Eule: erst hörbar, wenn Fry nahe genug ist – entspricht das der Erinnerung an die Xbox?
9. Pausenmenü: Musik hält an und geht danach an derselben Stelle weiter?
10. Fernseher/Kopfhörer während des Spiels aus- und wieder einschalten bzw. ab- und anstecken: läuft das Spiel ohne Ruckeln weiter, kommt der Ton zurück? Windows-Standardgerät umschalten: wechselt der Ton mit?

## 9. Review-Befunde

Details mit Belegen: [`loader_design.md`](loader_design.md) 8.15.

| # | Schwere | Befund | Ergebnis | Umsetzung |
|---|---|---|---|---|
| 1 | mittel | Klangeinsatz 10–20 ms mit Standardwerten (Pegel, Tonhöhe, Panorama, Hall) | bestätigt (Play vor SetVolume, Commit später) | Stimme startet beim Commit des Ticks mit allen Werten; gemessen +15 dB → 0 dB Abweichung |
| 2 | niedrig | 3D-Stimmen auf 5.1 im Center | bestätigt | Panning nur über die vier Ecken (siehe auch R2a) |
| 2a | – | eigener Fehler der ersten Korrektur: auf dem Fernseher (Maske 0x60F) jede 3D-Stimme vorne links | in der Endprüfung gefunden | nur Standardmasken an X3DAudio (Quad/Stereo) und Abbildung auf die echten Kanäle; Test mit 0x3F und 0x60F; A9 verlangt beide Seiten |
| 3 | niedrig | seitliche 3D-Quelle +3 dB über 0 dB | bestätigt | kein Kanal über der Front-Referenz; K3 berichtigt |
| 4 | niedrig | ADPCM „bitgleich“ nur gegen ffmpeg | bestätigt (nur Formulierung) | Texte berichtigt; kein Code geändert |
| 5 | niedrig | Musikpegel in A11 um 0,2–0,5 dB verzerrt | bestätigt | Abschnitte nachgesampelt und fein ausgerichtet, Bewertung unter 2 kHz ±0,1 dB |
| 6 | mittel | Backend-Wechsel im Gast-Thread (Ruckler) | bestätigt | Geräte-Thread, Backoff, eine WARN je Lauf |
| 7 | mittel | kein `IMMNotificationClient` | bestätigt | Benachrichtigung, sofortiger Wechsel, Rückkehr des gewählten Geräts; Hardwaretest durch den Nutzer |
| 8 | mittel | `[audio] device` nie mit echter ID geprüft | teilweise: Format funktioniert | Test öffnet jeden Endpunkt über seine ID; INFO-Zeile nennt das geöffnete Gerät |
| 9 | niedrig | `FramesPlayed` nach `Discontinuity`, Flush-Basis | bestätigt | monotoner Zähler, Basis beim nächsten `Submit` (siehe R9a) |
| 9a | – | eigener Fehler: XAudio2-Test lieferte 0 statt 4800 | in der Endprüfung gefunden | Basis wächst am Ende des Durchlaufs; höchstens 10 ms zu wenig |
| 10 | niedrig | Launcher friert beim Neusuchen während des Testtons ein | bestätigt | Abbruch-Flag, Knopf gesperrt |
| 11 | niedrig | Texte des Audio-Reiters | bestätigt | (a)–(d) umgesetzt |
| 12 | niedrig | > 8 Kanäle ohne Begrenzer/Hall/Mitschnitt | plausibel, nicht reproduzierbar | 8-Kanal-Mastering-Voice |
| 13 | niedrig | Stems allozieren im Echtzeit-Thread | bestätigt | alle Stems beim Anhängen deklariert |
| 14 | niedrig | „capture taps: 0“ irreführend | bestätigt | Zeile nur bei Benutzung |
| 15 | niedrig | Software-Mixer ohne Zuhörer rechnet vollen Mix | bestätigt | nur Positionen und Pufferenden |
| 16 | niedrig | Begrenzer im Spiel nie aktiv | bestätigt | Kriterium A12, Spiellauf mit 100 % und `trim_3d = 10` |
| 17 | niedrig | Zusammenfassung ohne Facade-Lock | bestätigt | nur unter dem Lock (`try_lock`) |

Kein Befund wurde abgelehnt.

## 10. Bekannte Probleme und offene Punkte

- **Abhören steht aus** (Abschnitt 8): Verhältnis 2D/3D und Hallpegel gegenüber der Xbox sind unbelegt (APU-Headroom und Pegel der DSP-Effekte unbekannt); X3DAudio-Amplitudenpanning statt Xbox-HRTF/Crosstalk; Standard `positional = corrected`; Wirkung des LFE-Tiefpasses auf dem Fernseher.
- **Gerätewechsel mit echter Hardware:** Umschalten des Windows-Standardgeräts und Ab-/Anstecken eines Headsets (WH-1000XM5, Jabra Link 370) sind Systemeingriffe bzw. Handgriffe und wurden nicht ausgeführt; geprüft sind der simulierte Verlust, der echte „Fernseher aus“-Verlust (Integrationsphase) und die Rückkehr im Unit-Test.
- **XMV-Pfade** (Callback-Streams, `Pause(2)`/`SynchPlayback`, `FlushEx`, `Discontinuity`) nur mit synthetischen Tests; im Spiel erst mit M6.
- **A9 auf Ereignisebene:** Die gefundenen 3D-Ereignisse eines Laufs liegen oft alle auf einer Seite; A9 besteht dann über den Effekt-Stem. Eine Prüfung „erwartete Seite je Ereignis“ (Emitter- gegen Hörerrichtung, R5 der Analyse) fehlt.
- **Sprachvarianten:** nur Deutsch geprüft (Menüstimme, Fry).
- **Anzeige bei 120 Hz (nicht M5):** Mit eingeschaltetem Fernseher bestehen `m3_check` (auch M3b-7) und die 60-Hz-Varianten von `m3d_check`, aber D1 (i120/d120/f120: nur 43 % der Bilder genau ein Refresh) und L1 (Just-in-time-Start) scheitern – mit dem M4-Bau ohne Ton genauso. In M3d bestanden sie am selben Fernseher (99,6–99,7 %); vermutlich hat sich die Anzeige-Umgebung geändert (Bildmodus/VRR des Fernsehers, Treiber). Gehört zu den Takt-Kriterien der Roadmap, nicht zu M5.
- **Vollbandpegel unter XAudio2** 0,1–0,2 dB unter dem Modell (Höhendämpfung des XAudio2-Resamplers); unter 2 kHz exakt.
- **Ein unerklärter Abbruch** (Rückgabecode −1 ohne Absturzbericht) in einem Lauf der abgebrochenen ersten Phase-5-Sitzung; mit dem Endstand nicht wieder aufgetreten (8.15).
- **Aufräumen:** `D:\FuturamaDecomp\buildm5-dsound\` (229 MB, nicht ignoriert) stammt aus einem Anführungszeichen-Fehler eines Phase-1-Agenten und kann gelöscht werden.

## 11. Nächste Schritte

1. **Abhören durch den Nutzer** (Abschnitt 8); bei Bedarf `trim_3d`-Standard, Hallpegel oder `positional`-Standard anpassen.
2. **Takt bei 120 Hz nachprüfen:** `python tools\m3d_check.py --exe build\m5-final-rel\bin\futurama.exe --config run\m5-final\checks\base\futurama.ini --variants i120 --fresh` – D1 scheitert derzeit auch mit dem M4-Bau (Abschnitt 10); Fernseher-Einstellungen (Spielmodus, VRR) und Treiber prüfen.
3. **M3e – Renderer P2:** Render to Texture (Avatarbild der Lebensanzeige, Konturen `NiOutline`), Partikel (Slot 52), schwarze Schiffsunterseite, fehlende Texturen (`m4_report.md` 6.1).
4. **M6 – Video:** XMV mit FFmpeg (WMV2 + Xbox-ADPCM). Den Filmton spielt XMV über DSOUND: Callback-Streams, `Pause(2)` + `SynchPlayback`, `FlushEx`, `Discontinuity` sind umgesetzt und getestet; nicht markierte Objekte laufen über den Submix „Film“. M6 muss dafür nur den XMV-Stub durch den Originalpfad ersetzen.
5. M4b (Freecam), dann G1/G2 (Konturen, Texturen) laut [`ROADMAP.md`](ROADMAP.md).
