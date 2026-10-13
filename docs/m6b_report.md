# M6b: Filmpakete – die Filme mit KI hochskaliert (1280 × 960) – Bericht

Stand: 2026-10-13 · Nutzerwunsch: die Videos „nicht übertrieben“ mit KI hochskalieren, 2× (640 × 480 → 1280 × 960), Comic-Stil erhalten · Grundlage: [M6](m6_report.md) (XMV-HLE) und [G2](g2_report.md) (Texturpakete) · Abweichungen: [`loader_design.md`](loader_design.md) 8.24 · Schnittstelle: [`src/INTERFACES.md`](../src/INTERFACES.md) 4.40, [`GFX_CONTRACT.md`](../src/host/gfx/GFX_CONTRACT.md) 15 · Nichts committet.

## 1. Ergebnis in Kürze

- **Alle 47 Filme liegen als KI-Paket vor:** `run\moviepack_ai_x2\` (git-ignoriert), 45 MP4-Dateien (Intro = 1-1-00 und die italienische = englische Trailer-Fassung sind bytegleich und teilen sich eine Datei), **39 602 Bilder, 555 MB** (die XMV-Originale: 724 MB), gebaut in **17,2 Minuten** auf der RTX 5090. Jedes Bild hat genau das pts der XMV (mit FFprobe gegen die XMV und gegen den Testmodus des Ports geprüft), 0 Fehler.
- **Verfahren:** Real-ESRGAN `realesr-animevideov3` in seiner nativen 2×-Stufe, **75 % KI + 25 % Lanczos** gemischt (wie bei den Texturpaketen), H.264 (NVENC, konstante Qualität cq 18, ohne B-Bilder, Schlüsselbild alle 30 Bilder, BT.601).
- **Im Port:** `[render] movie_pack = <Ordner>` (Launcher, Reiter „Grafik“, „Filmpaket“). Das Bild kommt dann aus dem Paket, dekodiert vom **in Windows eingebauten H.264-Decoder (Media Foundation)** – kein FFmpeg, kein Download. Ton, Sprache, Takt und Überspringen bleiben die der XMV; jedes Paketbild gehört genau zu seinem XMV-Bild. Bei jedem Problem zeigt der Film sein Originalbild weiter. Standard leer: alles wie bisher.
- **Belegt:** Screenshots im Spiel sind exakt das Paketbild des fälligen Bildes (47–58 dB gegen das Paketbild, 13–34 dB gegen die Nachbarbilder), die Filme laufen gleich lang wie in M6, Ton-Bild-Drift 0,1–0,3 ms, höchstens 29 ms spät (wie M6), 0 verpasste Paketbilder, Überspringen mit START/A funktioniert. Eingebrannte Schrift (Abspann, Trailer, Logos, HUD im Attract-Film) bleibt sauber lesbar.
- **Nicht gesehen in Bewegung:** Aussagen stammen aus Screenshots, Ausschnitten und Protokollen; die Bewertung „gefällt / zu glatt“ trifft der Nutzer (Abschnitt 8).

## 2. Wie der Nutzer ein Paket baut

Voraussetzungen (selbst installiert, das Werkzeug lädt nichts herunter): Python 3, FFmpeg mit `h264_nvenc` (Nvidia-Grafikkarte), Real-ESRGAN ncnn Vulkan mit dem Modell `realesr-animevideov3`, die eigene Disc als Abbild.

```
python tools\movie_upscale.py --iso "<Disc-Abbild>.iso" --pack D:\FuturamaDecomp\run\moviepack_ai_x2 ^
       --ai-exe D:\pop_upscale\realesrgan\realesrgan-ncnn-vulkan.exe ^
       --ffmpeg-dir "D:\Program Files (x86)\ffmpeg\bin"
```

Danach im Launcher „Filmpaket“ auf den Ordner setzen (oder `[render] movie_pack = <Ordner>` in der INI).

Ablauf je Film: XMV aus dem Abbild lesen (`tools\xdvdfs.py`) → alle Bilder mit FFmpeg als PNG → Real-ESRGAN über den ganzen Ordner (`-g 0`) → Mischung mit Lanczos und H.264-Kodierung mit den pts der XMV (ffconcat mit Bilddauern, Zeitbasis 1 ms) → Prüfung von Bildzahl und jedem pts → `<Name>.mp4` und `index.tsv` schreiben → temporäre Bilder und die XMV löschen (Platzbedarf höchstens ≈ 10 GB für den längsten Film 2-1-01). Fortschritt in `<Paket>\movie_upscale.log`; schon vorhandene Filme werden übersprungen (`--overwrite` baut neu), `--only Name,…` baut einzelne.

Wichtige Optionen: `--ai-mix 0.75` (KI-Anteil), `--ai-mix-for Name=0.5` (Anteil für einen einzelnen Film), `--cq 18` (Qualität), `--gop 30`, `--codec hevc` (nicht empfohlen: HEVC braucht unter Windows die Store-Erweiterung), `--reference-json run\m6\decode_all.json` (pts zusätzlich gegen den Testmodus `--decode-movies` des Ports prüfen). `tools\package.py` legt das Werkzeug samt `xdvdfs.py` ins Spielerpaket; Pakete selbst enthalten Spielgrafik und werden nie weitergegeben (`.gitignore`: `movie_pack*/`, `moviepack*/`, `*.mp4`, `*.xmv`).

## 3. Wahl des Verfahrens

Verglichen an Bildern aus Intro, 1-1-01 (Zwischensequenz), 3-1-01 und fox (Ausschnitte 2× vergrößert, `run\moviepack_work\cmp\sheets\zoom_*.png`, Spalten Lanczos | KI rein | KI 75 + Lanczos 25):

| Kriterium | KI rein (animevideov3 ×2) | KI 75 % + Lanczos 25 % (gewählt) |
|---|---|---|
| Konturen (Bender, Kacheln in 1-1-01) | sehr sauber, WMV2-Blöcke verschwinden | sauber, minimal weicher |
| kleine gemalte Details (Fenster im 3-1-01-Hintergrund) | etwas verwischt („Ölfarbe“) | bleiben erkennbar |
| Blockrauschen | weg | zu einem Viertel sichtbar (leicht) |
| Flimmern in ruhigen Flächen (Änderung des KI-Anteils von Bild zu Bild) | 0,03–0,06 Stufen | 0,02–0,04 Stufen |

Die Mischung entspricht „nicht übertrieben“ und dem, was sich bei den Texturen bewährt hat (G2: 75/25). `realesrgan-x4plus` (beim Prince-of-Persia-Projekt „zu harsch“) wurde nicht mehr erwogen.

**Eingebrannte Schrift** (Wunsch des Nutzers, `run\moviepack_work\tx\sheets\text_*.png`, je Original-Pixel 2× | Lanczos | KI rein | Paket):

| Film | Schrift | Ergebnis | Gewählt |
|---|---|---|---|
| Credits-PAL (Abspann, 25 fps, rollend) | Namen, Rollen, kleine Zeilen („www.TheTestStation.com“, E-Mail-Adresse), Alien-Schrift | schärfer und besser lesbar als Lanczos, keine verformten Buchstaben; beim Rollen (4 px je Bild) kein zusätzliches Flimmern: Restfehler nach Ausgleich der Bewegung 2,1–3,6 wie beim Lanczos-Bild (2,1–3,5) | 75/25 |
| DVDTrailer (5 Sprachen) | Chrom-Abspann (im Original mit Zeilensprung-Kämmen), „JETZT AUF DVD & VHS“ | Endtafel deutlich sauberer; die Kämme der Quelle bleiben wie im Original, werden nicht verschmiert | 75/25 |
| fox, SCI, uds | 3D-Logos („20th Century Fox“, „FOX INTERACTIVE“, „G A M E S“, „UDS“) | Kanten sauberer, Formen unverändert | 75/25 |
| Attract-PAL | HUD-Ziffern („3/5“, Zeiten), „FUTURAMA“-Schriftzug | Ziffern klarer, keine erfundenen Formen | 75/25 |
| übrige 35 Filme | keine eingebrannte Schrift (Level-Titelkarten zeichnet das Spiel selbst) | – | 75/25 |

Ein Neusetzen der Schrift wie im Prince-of-Persia-Projekt (`retext.py`) war nicht nötig; für einen Film mit sanfterem Ergebnis gibt es `--ai-mix-for`.

Bekannt: Die KI zeichnet sehr kleine Details neu, etwa die nur wenige Pixel großen Pupillen im Intro (Bild 41/42 – im Paket schärfer und mit Lidkante, in der XMV verschwommen).

## 4. Wie der Port das Paket abspielt

```
Spiel: Movie::Open → CreateDecoderForPackets(Kopf 0x1000 B)   XMV-HLE: CRC-32 des Kopfes → index.tsv → <Name>.mp4
                                                              PackVideoReader: Media-Foundation-Thread, YUY2, ≤ 6 Bilder voraus
Spiel: Movie::Update → GetNextFrame(Fläche)                   wie M6: Pakete, Ton, Audiotakt, WMV2 → 640×480 in die Fläche
                                                              + Paketbild Nr. n (n = Index des gezeigten XMV-Bildes),
                                                                pts relativ zum ersten Bild ±1 ms → d3d::SetHostImage
Renderer: MovieTexture                                        Host-Bild 1280×960 → BGRA-Textur in seiner Größe;
                                                              Texel-UVs des Spiels × (1280/640)/1280 → gleiche Fläche
Spiel: Close (Ende, A/START)                                  Reader beenden, Host-Bild entfernen
```

- **Zuordnung:** über die CRC-32 der ersten 0x1000 Bytes der XMV (der Kopf, den das Spiel übergibt) – ein Paket von einer anderen Disc passt nie; der Dateiname wird nicht gebraucht.
- **Synchron:** Das Paket liefert nur Bilder. Welches Bild wann fällig ist, entscheidet weiter die XMV am Audiotakt (M6); das n-te gezeigte XMV-Bild bekommt das n-te Paketbild, nach Prüfung des pts. Verspätete XMV-Bilder überspringt der Reader mit.
- **Start:** Vor dem Start des Tons wartet die HLE auf das erste dekodierte Paketbild (≈ 50–100 ms Media-Foundation-Anlauf), damit die ersten Bilder nicht spät kommen (ohne diese Wartezeit: 44 ms spät im ersten Lauf, mit: höchstens 29 ms wie in M6).
- **Rückfall:** Fehlt die Datei, endet sie zu früh, stimmt ein pts nicht oder meldet der Decoder einen Fehler, zeigt der Film ab dort sein Originalbild (WARN im Protokoll, keine Lücke, Ton läuft weiter). Ein einzelnes nicht rechtzeitig dekodiertes Bild (250 ms) zeigt nur für dieses Bild das Original.
- **Kosten:** WMV2 läuft weiter mit (0,2–0,4 ms je Bild); H.264 1280 × 960 dekodiert Media Foundation im eigenen Thread; Hochladen 1280 × 960 BGRA 15/25-mal je Sekunde.

## 5. Was gemessen wurde

**Paketbau** (`run\moviepack_ai_x2\movie_upscale.log`, `run\moviepack_work\full_run.txt`): 45 Filme hochskaliert, 2 geteilt, 0 Fehler, 17,2 min; KI 42–74 Bilder/s, Kodierung etwa halb so lang wie die KI; jedes pts gleich FFprobe(XMV) und gleich `run\m6\decode_all.json` (Testmodus des Ports).

**Spielläufe** (Arbeitsbaum `run\video\<Lauf>\`, Deutsch, 1280 × 960, `[audio] master_volume = 0`, `[input] rumble = 0`, `[debug] movies = play`, Timeout 120 s):

| Lauf | Inhalt | Ergebnis |
|---|---|---|
| `boot_full` (46 s, `m6_boot.txt`) | fox (nach 3 s START), SCI, uds, Intro bis zum Ende | 452/452 Bilder aus dem Paket, 0 verpasst, 0 Rückfälle; SCI 12 278 ms, uds 5929 ms, Intro 7898 ms (M6: 12 279 / 5929 / 7908); max. 29 ms spät; `m6_check --runs-only`: Spur erkannt (NCC 0,96–1,00), Drift +0,1…+0,3 ms, **ALL PASS** |
| `level_pack` (37 s, `m6_level.txt`) | Neues Spiel → 1-1-01 beim ersten Ansehen 9,5 s, A überspringt | 143/143 Bilder aus dem Paket, deutsche Spur (NCC 0,99 gegen 0,62–0,73), Drift +0,3 ms, nach dem Überspringen steuerbar; **ALL PASS** |
| `boot_pack` (46 s) | wie `boot_full` mit einem Testpaket ohne fox | fox mit Originalbild („not in the movie pack“), die anderen aus dem Paket; ALL PASS |
| `broken` (46 s) | SCI-Datei abgeschnitten, uds/Intro-Datei fehlt | SCI: Paket bis Bild 92, dann „movie pack off from frame 93: the pack file has fewer frames than the XMV“ und Originalbild; uds/Intro: „file missing – original picture“; alle Filme laufen normal zu Ende, Exit 0 |

**Screenshots gegen Original und Paket** (gleiches Bild; der Screenshot wird auf 640 × 480 verkleinert und dem besten XMV-Bild zugeordnet, Vergleich mit Ausgleich des bekannten 1-Pixel-Versatzes):

| Screenshot | Film, Bild | gegen Paketbild k−1 / **k** / k+1 | gegen Original (2× bilinear) |
|---|---|---|---|
| `boot_full\shots\frame_000540.png` | fox, 44 | 27,9 / **46,1** / 28,0 dB | 35,6 dB |
| `frame_000782.png` | SCI, 89 | 19,7 / **46,8** / 19,0 dB | 43,6 dB |
| `frame_001461.png` | uds, 43 | 27,2 / **57,4** / 23,5 dB | 38,4 dB |
| `frame_002399.png` | Intro, 41/42 (stehendes Bild) | **52,8 / 50,4** / 37,7 dB | 34,9 dB |
| `level_pack\shots\frame_001912.png` | 1-1-01, 141 | 31,3 / **46,8** / 33,6 dB | 30,8 dB |

Seitenvergleiche (links XMV-Bild wie ohne Paket, rechts Screenshot mit Paket): `run\video\boot_full\cmp\cmp_*.png`, `run\video\level_pack\cmp\cmp_1-1-01-PAL_141.png`. Das Bild füllt genau dieselbe Fläche wie das Original; der Versatz von 1 Ausgabepixel nach rechts unten ist der bekannte Halbpixel-Rand aus M6 (8.16 V12), unverändert.

**Unit-Tests:** `fut_tests` 517/517 (RelWithDebInfo, `FUT_WERROR=ON`, `build\video`); neu `tests\movie_pack_tests.cpp` mit 8 Fällen: Index (Kommentare, CRLF, unsichere Dateinamen, ungerade/zu große Maße), Suche über CRC/Seitenverhältnis, pts-Regel, 100-ns→ms, Warteschlangenschritte, Texel-Skalierung, Host-Bild der Shim-Textur (auch Freigabe mit der Textur), Schemazeile, und – wenn `run\moviepack_ai_x2` existiert – der echte Media-Foundation-Leser (alle Bilder, pts steigend, Überspringen 0 → 5).

## 6. Einstellungen

| Schlüssel | Werte | Wirkung |
|---|---|---|
| `[render] movie_pack` | Ordner, Standard leer | Launcher-Reiter „Grafik“, „Filmpaket“. Leer: kein M6b-Code, Filme wie bisher. Gesetzt: Bild aus dem Paket für jeden Film, der darin steht |

`[debug] movies = skip` schaltet auch das Paket ab (kein Film wird gespielt).

## 7. Prüfweg ohne Augen

Screenshots während der Filme über die vorhandenen M6-Eingabeskripte, Zuordnung zum Bild über PSNR (XMV-Bild und Paketbild aus FFmpeg), Ton über die Teilmischungen von `[audio] capture_stems` (bei Lautstärke 0 weiter aufgezeichnet) mit `tools\m6_check.py --runs-only`, Protokollzeilen „S of T shown frames from the movie pack … pts checked“.

## 8. Worauf der Nutzer achten sollte (Checkliste)

1. Launcher → Grafik → „Filmpaket“ = `D:\FuturamaDecomp\run\moviepack_ai_x2`, Spiel starten: fox, SCI, uds und Intro sollten deutlich schärfer wirken, Konturen sauber, Farben wie vorher.
2. **Stil:** Wirkt es „nicht übertrieben“? Zu glatt oder zu „gemalt“ (vor allem Hintergründe in 3-1-01, 4-1-01)? Dann z. B. `--ai-mix 0.6` (mehr Original) oder einzelne Filme mit `--ai-mix-for` neu bauen.
3. **Schrift:** Abspann (Extras), DVD-Trailer, Logos: lesbar und ruhig beim Rollen?
4. **Bewegung:** Flimmern oder „wabernde“ Flächen bei Kameraschwenks?
5. **Ton und Überspringen:** lippensynchron, START/A bricht weiterhin jeden Film ab.
6. Im Vollbild mit höherer Auflösung (z. B. 2880 × 2160) zeigt sich der Gewinn am stärksten.

## 9. Bekannte Punkte und offene Fragen

- Die KI zeichnet winzige Details neu (Pupillen, Gesichter in der Ferne) – meist besser lesbar, aber nicht immer genau wie im Original.
- Die Quelle der DVD-Trailer hat Zeilensprung-Kämme; sie bleiben (kein Deinterlacing, um nichts zu erfinden).
- Der Halbpixel-Rand (oberste Zeile/linke Spalte) bleibt wie in M6.
- Nur MSVC RelWithDebInfo gebaut; Debug- und clang-cl-Bau nicht wiederholt.
- Nur mit H.264 geprüft; HEVC-Pakete setzen die HEVC-Erweiterung von Windows voraus (sonst Rückfall auf das Originalbild).
- Abspann, Trailer und Attract-Film nur im Paketbau und in Ausschnitten geprüft, nicht im Spiellauf.
