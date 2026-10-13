# Meilenstein M6: Video (XMV-HLE, WMV2 über Windows, Ton der Spielsprache, alle Filme überspringbar) – Bericht

Stand: 2026-10-12 · Ziel-XBE: `orig\retail_eu\default.xbe` (Retail EU) · Verbindliches Design: [`docs/loader_design.md`](loader_design.md) (M6, Abweichungen 8.16) · Analyse: [`analysis/loader/avi.md`](../analysis/loader/avi.md) Abschnitt 3 (XMV), [`analysis/loader/m4_input.md`](../analysis/loader/m4_input.md) 4.4/4.5 (Überspringen) · Schnittstellen: [`src/INTERFACES.md`](../src/INTERFACES.md) 4.32, [`src/host/gfx/GFX_CONTRACT.md`](../src/host/gfx/GFX_CONTRACT.md) 15 · Nichts committet (die Hauptsitzung hat einen Zwischenstand als 80b7d52 gesichert).

## 1. Ergebnis in Kürze

**Die Filme laufen.** Die sieben XMV-Einstiege sind keine Stubs mehr, die jeden Film sofort beenden, sondern eine HLE.

- **Was läuft:** Logos (fox, SCI, uds), Intro, die Zwischensequenzen der Level (5 Tonspuren, die Sprache wählt das Spiel), Attract-Modus, Abspann und Trailer – alle 47 Dateien der Disc dekodieren fehlerfrei.
- **Ohne FFmpeg, ohne Download:** eigener XMV-Demuxer, WMV2 über den in Windows eingebauten „WMVideo Decoder DMO“ (`wmvdecod.dll`), Ton (Xbox-ADPCM) über den M5-Dekoder.
- **Wie im Original:** Das Bild geht als YUY2 in die Filmtextur, die das Spiel selbst anlegt (`D3DDevice_CreateTexture2(…, 0x24, …)`); das Spiel zeichnet sein Vollbild-Viereck mit `YuvEnable` wie auf der Xbox. Der Ton ist ein DSOUND-Stream, auf dem das Spiel selbst `SetVolume` und `Release` aufruft.
- **Takt:** strikt nach pts am Audiotakt. Die 15-fps-Dateien (mit ihren unregelmäßigen Lücken bis 4,7 s) und die 25-fps-Dateien (Attract, Abspann, SCI) laufen in richtiger Geschwindigkeit; jedes Bild erscheint höchstens 29 ms nach seinem Soll (ein bis zwei Logik-Ticks), Tondrift 0,1–0,3 ms über die gemessenen Abschnitte.
- **Sprache:** In Level 1-1 hört man im Film 1-1-01 die deutsche Spur (Einstellung Deutsch), gemessen über den Mitschnitt: Korrelation 0,99 mit Spur 1 gegenüber 0,62–0,73 mit den anderen Sprachen.
- **Überspringen (Nutzerwunsch):** Jeder Film lässt sich mit A oder START abbrechen, auch 1-1-01/1-1-02 beim ersten Ansehen und Zwischensequenzen in den ersten 2,5 s: `[game] skip_videos = always` (Standard). `original` stellt das Verhalten der Xbox wieder her.
- **Prüfungen:** Testmodus `--decode-movies` 47/47 Dateien, 39 602 Bilder, 0 Fehler; `tools\m6_check.py` **ALL PASS** (Ton aller 195 Spuren bitgleich mit FFmpeg, alle pts gleich, Luma ≥ 61 dB); Spielläufe `boot`, `level`, `attract` mit Screenshots und Ton-Auswertung **ALL PASS**; `fut_tests` 457/457 in RelWithDebInfo, Debug und clang-cl (0 Warnungen).
- **Nicht gehört, nicht gesehen in Bewegung:** Alle Aussagen stammen aus Protokollen, Screenshots und Mitschnitten. Der Nutzer prüft nach Augenmaß und Gehör (Abschnitt 8).

## 2. Was gebaut wurde

| Teil | Dateien | Inhalt |
|---|---|---|
| Demuxer | `src\host\video\xmv_demux.*` | Dateikopf, Paketkette, WMV2-Bilder (Wörter little-endian → big-endian), Extradaten-Umsetzung, ADPCM je Spur |
| Decoder | `video\wmv_decoder.*` | WMVideo Decoder DMO über `DllGetClassObject` (keine COM-Registrierung, kein `CoInitialize` im Gast-Thread), Ausgabe I420 |
| Kern | `video\movie_core.*`, `movie_clock.h`, `yuv_pack.*` | Paketquelle → Demuxer → Warteschlangen; Filmuhr (Audiotakt, Wanduhr als Rückfall); I420 → YUY2 |
| XMV-HLE | `video\xmv_hle.*` (vorher `xmv_stubs.*`) | die 7 Einstiege, Pakete über die Callbacks des Spiels, DSOUND-Stream, Bild in die Shim-Fläche |
| Überspringen | `video\movie_skip.*`, `core\guest.h` | `[game] skip_videos`, Probe-Hook auf die Sperre FUN_000D3120 |
| Renderer | `gfx\d3d_shim.*`, `gfx\texconv.*`, `gfx\nipc_gpu.*`, `gfx\nipc_renderer.cpp` | Generation je Shim-Textur, YUY2 → BGRA (BT.601) beim Hochladen, Texel-Adressierung der Filmstufe; die schwarze 1×1-Ersatztextur gilt nicht mehr für Filme |
| DSOUND | `audio\dsound_hle.*` | `StreamFramesPlayed` (Audiotakt), keine Unterlauf-Warnung nach dem Datenende |
| Testmodus | `video\xmv_test_mode.*`, `core\cli_tools.*` | `futurama.exe --decode-movies=<Abbild> --result=<json> [--movie-dump=<dir>]` |
| Werkzeuge | `tools\m6_check.py`, `tools\input_scripts\m6_boot.txt`, `m6_level.txt`, `m6_attract.txt` | Vergleich mit FFmpeg (PyAV), Auswertung der Spielläufe (Spur, Sprache, Drift, Länge) |
| Tests | `tests\video_tests.cpp`, `tests\xdk_tests.cpp` | Demuxer mit synthetischen Paketen, YUY2, Uhr, Option, Shim-Generation, CRC; optional echte Proben |

Angepasst, damit bestehende Prüfungen mit echten Filmen weiterlaufen: die Eingabeskripte `m4_newgame`, `m4_rumble`, `m4b_freecam`, `m5_frontend` drücken bis zum Frontend START (Logos und Intro dauern jetzt ≈ 50 s); `m2/m3/m3c/m3d_check.py` starten mit `--set=debug.movies=skip` (Filme enden sofort wie vor M6, damit ihre festen Bildnummern stimmen).

## 3. Aufbau

```
Spiel: Movie::Open 0xEE220                      XMV-HLE (video\xmv_hle.cpp, Gast-Thread)
  "Video\<name>-PAL.xmv" lesen, 0x1000 B Kopf ─▶ CreateDecoderForPackets: Kopf prüfen, Handle (nichts gelesen)
  GetVideoDescriptor ──────────────────────────▶ 640 × 480, Spurenzahl
  EnableAudioStream(Spur = Sprache bei 5) ─────▶ DSOUND-HLE-Stream (0x69, 32 Pakete), Pause(2)
  GetAudioStream → SetVolume, Release ─────────▶ derselbe Stream (AddRef)
  CreateTexture2(640, 480, …, 0x24 YUY2) ──────▶ D3D-Shim-Textur
Spiel: Movie::Update 0xEDE90 (jeder Tick)
  GetNextFrame(Fläche) ─────────────────────────▶ DoWork; Pakete über 0xEDBA0/0xEDE60 nachlesen; Ton abgeben;
                                                  Uhr = gespielte Frames / Rate; fällige Bilder dekodieren (WMV DMO),
                                                  das neueste als YUY2 in die Fläche → Ergebnis 1 / 0 / 2
  YuvEnable(1); Viereck zeichnen; YuvEnable(0) ─▶ Renderer: Filmtextur → BGRA (BT.601), UV / (640, 480), Clamp
Spiel: Close (Ende oder Überspringen) ──────────▶ CloseDecoder: FlushEx, Release
```

- **Erstes Bild:** Vorlauf von 0,6 s Ton, dann starten alle Streams gemeinsam (`SynchPlayback`, wie das Original) und Bild 0 wird gezeigt. Danach hält die HLE etwa 1,2 s Ton vor.
- **Späte Bilder:** Ist die Uhr schon über mehrere pts hinaus (z. B. nach einem Ruckler des Spiels), werden alle fälligen Bilder dekodiert (P-Bilder bauen aufeinander auf), gezeigt wird das neueste. In allen Läufen: 0 verworfene Bilder.
- **Ende:** wenn alle Bilder gezeigt sind und die Uhr das Ende von Bild und Ton erreicht hat. Endet der Ton vor dem Bild, läuft die Uhr mit der Wanduhr weiter.

## 4. Was gemessen wurde

**Testmodus (alle 47 Dateien, 34 s, `run\m6\decode_all.json`) gegen FFmpeg über PyAV 18 (libavcodec 62) (`run\m6\m6_check.txt`):**

| Kriterium | Ergebnis |
|---|---|
| V1 dekodiert ohne Fehler | 47/47 Dateien, 39 602 Bilder, 0 Fehler; 0,19–0,44 ms je Bild |
| V2 Bildzahl und jedes pts gleich FFmpeg | alle 47 gleich |
| V3 Luma bitgleich (Information) | 2,97 % der Bilder – Microsofts Decoder rundet anders als FFmpegs wmv2; ±1 in einzelnen Pixeln, das sich bis zum nächsten Schlüsselbild fortsetzt |
| V4 PSNR der 235 gesicherten Bilder | Luma min 61,0 dB, Mittel 76,1 dB (58 identisch); Chroma min 35,6 dB, Mittel 65,1 dB (62 identisch) |
| V5 Anzeigeweg (YUY2 + BT.601) gegen FFmpegs rgb24 | min 33,5 dB, Mittel 40,8 dB (FFmpeg skaliert die Farbe anders hoch) |
| A1 Ton | 195/195 Spuren bitgleich (Länge und CRC-32 des PCM) |
| T1 Bildraten | Attract, Credits, SCI 40 ms (25 fps), alle anderen 66/67 ms plus einzelne Lücken (bis 4666 ms in 1-1-02) |

**Spielläufe (`run\m6\<lauf>\`, Deutsch, 1280×960, Mitschnitt mit Teilmischungen; `m6_check.py --runs-only --run …`):**

| Lauf | Film | Ergebnis |
|---|---|---|
| `boot` (46 s) | fox | nach 3 s mit START übersprungen, Tonspur erkannt (0,98) |
| | SCI (25 fps) | bis zum Ende: 243/243 Bilder in 12 279 ms (Datei 12 276 ms), höchstens 29 ms spät, Drift +0,3 ms |
| | uds | bis zum Ende: 71/71 Bilder in 5929 ms, Drift +0,1 ms |
| | Intro (= 1-1-00, bytegleich) | bis zum Ende: 93/93 Bilder in 7908 ms, Spur 1 (Deutsch; die fünf Spuren sind hier gleich), Drift +0,1 ms |
| `level` (37 s) | 1-1-01 | erstes Ansehen, nach 9,5 s mit A übersprungen (Original: „no“ → `always`: „skip“), danach steuerbar; Sprachfenster bei 4 s: Spur 1 (Deutsch) 0,99, andere 0,62–0,73 |
| `level_original` | 1-1-01 | mit `skip_videos = original`: A wird abgelehnt („keep playing“), das Skript scheitert erwartungsgemäß an `expect controllable` |
| `attract` (82 s) | Attract-PAL (25 fps) | nach 60 s Leerlauf, 70 Bilder in 4969 ms (die Datei beginnt mit einer Lücke von 2240 ms), A kehrt ins Frontend zurück |
| `m4script` | m4_newgame.txt | mit echten Filmen: Exit 0 nach 38 s, Level 1-1 spielbar |

Statistik am Ende von `boot`: `d3d11 movie textures (YUY2): 453 frame uploads, 1752 texel-addressed stage draws, 0 draws without YuvEnable, 0 failures`; `XMV HLE: 4 movies opened, 3 played to the end, 1 closed early (skipped), 0 failed; 453 frames shown, 0 dropped, max 29 ms late`.

**Screenshots** (verkleinert): `run\m6\crop_boot_fox.png`, `crop_boot_sci.png`, `crop_boot_uds.png`, `crop_boot_intro.png`, Übersicht `run\m6\boot\boot_sheet.png`; Level `run\m6\level\level_sheet.png` (Titelkarte, 1-1-01, nach dem Überspringen im Hangar); Attract `run\m6\attract\attract_small.png`. Alle zeigen richtige Bilder in richtigen Farben und richtiger Lage.

## 5. Einstellungen

| Schlüssel | Werte | Wirkung |
|---|---|---|
| `[game] skip_videos` | `always` (Standard), `original` | Launcher-Reiter „Spiellogik“: „Filme überspringen“. `always`: A/START bricht jeden Film sofort ab. `original`: wie auf der Xbox (1-1-01/1-1-02 erst ab dem zweiten Ansehen, Zwischensequenzen nach 2,5 s; Logos/Intro/Attract waren schon immer abbrechbar) |
| `[debug] movies` | `play` (Standard), `skip` | Entwickler/Prüfungen: `skip` beendet jeden Film sofort ohne Bild und Ton (Verhalten vor M6) |

Die Sprache der Filme folgt `[settings] language` (Englisch, Deutsch, Französisch, Spanisch, Italienisch); die Lautstärke regelt das Spiel selbst (Kategorie „Effekte“ der Spieloptionen, wie im Original).

## 6. Wie ohne Augen und Ohren geprüft wurde

- **Testmodus statt Spiellauf:** dekodiert alle 47 Dateien in 34 s und schreibt je Bild pts und CRC-32 der Luma-Ebene, je Tonspur Länge und CRC-32 des PCM; `m6_check.py` dekodiert dieselben Dateien mit FFmpeg direkt aus dem Abbild und vergleicht.
- **Spielläufe kurz gehalten** (≤ 1 min, nur `attract` 82 s wegen der 60 s Leerlauf), mit Eingabeskripten, Screenshots im Film und `[audio] capture_wav` + `capture_stems`: Die Teilmischung „movie“ wird je Film (Zeiten aus dem Protokoll) mit jeder Tonspur der Datei kreuzkorreliert: welche Spur klingt, mit welchem Versatz am Anfang und am Ende (Drift = Geschwindigkeit).
- **Machbarkeit zuerst:** Vor dem Bau ein 2-KB-Testprogramm (WMV2-Typ, Extradaten, 45/45 Bilder, Luma des ersten Bilds bitgleich mit FFmpeg).

## 7. Prüfungen

- Bauten: `build\m6` (MSVC RelWithDebInfo), `build\m6-debug` (MSVC Debug, `FUT_WERROR=ON`), `build\m6-clang` (clang-cl Debug, `FUT_WERROR=ON`): 0 Warnungen.
- `fut_tests`: 457/457 in allen drei Bauten (1 übersprungener Fall wie vorher). Neu: 10 Fälle in `tests\video_tests.cpp`, der XMV-Fall in `tests\xdk_tests.cpp` neu geschrieben. Der Fall „XMV decode of the disc samples“ dekodiert `analysis\loader\samples\*.xmv` (git-ignoriert), wenn vorhanden.
- `python tools\m6_check.py --reuse --run run\m6\boot --run run\m6\level --run run\m6\attract`: ALL PASS.
- Nicht wiederholt: die langen Prüfungen `m3_check`, `m3d_check`, `m4_check`, `m5_check` (der Nutzer wünscht kurze Läufe). Sie laufen jetzt mit `--set=debug.movies=skip` bzw. mit START bis zum Frontend; geprüft ist nur `m4_newgame.txt` direkt (Exit 0).

## 8. Worauf der Nutzer achten sollte (Checkliste)

1. **Start:** Nach dem Legal-Bild laufen 20th Century Fox (mit Fanfare), SCI und UDS, dann das Intro mit Bender. Bild flüssig, Ton lippensynchron, keine Knackser am Anfang und Ende eines Films.
2. **Überspringen:** START oder A bricht jedes Logo und das Intro sofort ab. Im Spiel: „Neues Spiel“ → nach der Titelkarte „Planet Express“ läuft 1-1-01 (≈ 3 min) – A/START bricht ihn ab, obwohl das Original das beim ersten Mal nicht erlaubt. Mit „Filme überspringen“ = „Wie im Original“ (Launcher, Reiter Spiellogik) muss der Film beim ersten Mal durchlaufen.
3. **Sprache:** In 1-1-01 sprechen die Figuren in der eingestellten Spielsprache (Launcher: Spielsprache). Bitte einmal mit einer anderen Sprache wiederholen.
4. **Lautstärke:** Der Filmton folgt dem Effekte-Regler der Spieloptionen (wie im Original); mit dem Gesamtregler des Launchers zusammen angenehm?
5. **Geschwindigkeit:** Der Attract-Modus (Hauptmenü 60 s ohne Eingabe) und der Abspann (Extras) laufen mit 25 Bildern/s – Bewegungen weder zu schnell noch zu langsam, Ton synchron bis zum Ende.
6. **Bildränder:** Am oberen und linken Rand ist bei hoher Auflösung eine Pixelzeile dunkel (bekannt, Abschnitt 9). Andere Ränder, Streifen oder Farbstiche bitte melden.
7. **Vollbild/Fenster und 120 Hz:** Filme im Vollbild und bei 120 Hz ruckelfrei? (Die Filme haben 15 bzw. 25 Bilder/s; jedes Bild bleibt 4–8 Refreshes stehen.)

## 9. Bekannte Probleme und offene Punkte

- **Nicht bitgleich mit FFmpeg:** Der Windows-Decoder ist Microsofts eigene WMV8-Umsetzung (die Referenz, mit der die Filme erzeugt wurden); FFmpegs wmv2 weicht davon ab. Luma ≥ 61 dB, Farbe ≥ 35,6 dB – nicht sichtbar. Welcher Decoder näher an der Xbox liegt, ist nicht belegt (kein xemu-Vergleich von Filmszenen).
- **Halber Pixelrand:** Bei 2-facher interner Auflösung sind oberste Zeile und linke Spalte nicht vom Film bedeckt (Halbpixel-Regel aller vortransformierten Draws). Nicht mit xemu verglichen.
- **`YuvEnable` aus:** Würde eine YUY2-Textur ohne `YuvEnable` gezeichnet, nimmt der Renderer die Rohkomponenten (ANNAHME zum NV2A); im Spiel kommt das nie vor (0 Fälle).
- **1-1-02, Abspann und Trailer im Spiel:** nur im Testmodus geprüft (1-1-02 kommt später im Level, der Abspann aus dem Extras-Menü). Untertitel gibt es auf der PAL-Disc nicht ([AVI] 3.2).
- **Lange Prüfungen** (`m3d_check`, `m4_check`, `m5_check`) nicht neu gelaufen; die Anpassungen (START bis zum Frontend, `debug.movies = skip`) sind nur einzeln belegt.
- **`wmvdecod.dll`** gehört zu jedem Windows 10/11 (Media Foundation). Fehlt sie (Windows „N“ ohne Media Feature Pack), endet jeder Film sofort mit Ergebnis 3 und einer Fehlerzeile; das Spiel läuft weiter.

## 10. Nächste Schritte

- **M7 Spielstände** (laut Roadmap als Nächstes): Speichern, Neustart, Laden, Optionen/Extras, Signatur, Löschen.
- Filme mit xemu vergleichen, sobald Referenzaufnahmen von Filmszenen vorliegen (Halbpixelrand, Farbumrechnung).

**Nachtrag 2026-10-13 (M6b):** Mit `[render] movie_pack` zeigen die Filme ein KI-hochskaliertes Bild (1280 × 960) aus einem Paket, das `tools\movie_upscale.py` aus der eigenen Disc baut; Ton, Takt und Überspringen bleiben wie hier beschrieben. Bericht: [`m6b_report.md`](m6b_report.md).
