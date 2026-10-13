# G2: Texturpakete – Bericht

Stand: 2026-10-13 (Abschnitt „Nachbesserung: Alpha, Schrift, waifu2x“ neu; davor „KI-Paket für das ganze Spiel“). Abweichungen vom Entwurf: [`loader_design.md`](loader_design.md) 8.22. Schnittstelle: `src\INTERFACES.md` 4.38.

## Kurzfassung

* **Neu: `[render] texture_pack = <Ordner>`** (Launcher, Reiter „Grafik“, „Texturpaket“): Der Port ersetzt Texturen des Spiels durch Dateien aus diesem Ordner. Jede Textur wird beim Laden über ihre **Originaldaten** erkannt (Inhalts-Hash, 16 Hex-Ziffern). Liegt `<Hash>.dds` oder `<Hash>.png` im Ordner, zeichnet das Spiel ab dann diese Datei – in beliebiger Größe, mit Mipmaps, Alpha bleibt erhalten.
* **Neu: `[render] texture_pack_dump = 1`** („Texturen abspeichern“): Jede Textur, die das Spiel lädt, wird einmal als PNG in `<Benutzerdaten>\texture_dump\<Hash>.png` geschrieben, dazu eine Zeile in `index.tsv` (Größe, Originalformat, Mipstufen, Alpha-Art). Das ist die Vorlage für ein eigenes Paket.
* **Nachbesserung (Rückmeldung zum KI-Paket):** Die dunklen Rechtecke hinter dem gewählten Menüpunkt waren ein Renderer-Fehler (auch ohne Paket) und sind behoben. Alpha-Texturen (Lampen, Leuchten, Lichtkegel, Glanzschein) behalten jetzt ihr Original-Alpha und die Originalfarbe unter durchsichtigen Texeln; die Schrift ist ein sauberes 2× mit der Original-Strichstärke statt nachgezeichnet. **Empfohlen ist jetzt `--method waifu2x`** (cunet, Rauschstufe 1). Pakete: `run\texpack_w2x_x2` (empfohlen), `run\texpack_ai_x2` (Real-ESRGAN, mit den Korrekturen neu gebaut), das alte als `run\texpack_ai_x2_old`.
* **Neu: `tools\texture_upscale.py`** macht aus dem Abzug ein Paket – lokal, ohne Downloads (numpy + Pillow). Standard ist ein **comicgerechtes 2×-Verfahren**: Lanczos-Vergrößerung ohne Überschwinger, Kanten zur Nachbarfarbe gestrafft, flache Farbflächen bleiben flach, Alpha-Kanten sauber, Ausgabe als DDS (BC1/BC3) mit Mipmaps. Empfohlen ist **`--method ai`** mit einem selbst installierten Real-ESRGAN ncnn Vulkan (Modell `realesr-animevideov3`, 75 % KI + 25 % Comic-Verfahren, Detailrückführung). **Schrift** geht nie durch die KI: Schriftatlas, Schilder und der Text der Ladebildschirme werden nachgezeichnet (Abstandsfeld der Kontur).
* **KI-Paket für das ganze Spiel:** Abzug aller 22 Level und des Frontends: **2768 Texturen** (23 MB). Daraus in 49 s ein 2×-Paket mit 2738 Dateien (**116 MB**). Im Spiel geprüft (alle 22 Ladebildschirme, Hauptmenü, 1-1, 1-2, 2-1, 3-1): Der Comic-Stil bleibt, Linien und Schrift werden sauberer, keine Fehler, kein Speicherproblem.
* **Standard aus:** Ohne Paket und ohne Abzug läuft kein G2-Code (kein Hash, kein Thread, keine Logzeile).
* **Ergebnis (ehrlich):** In Level 1-1 bei 1280×960 sind die Unterschiede **klein**. Mit 2× bzw. 4× werden Kanten sauberer (Fensterrahmen des Schiffs, Warnstreifen, Risse im Boden) und das DXT-Blockrauschen auf großen Flächen (Tanks, Boden) verschwindet weitgehend. Neue Details erfindet das Verfahren absichtlich nicht; feine Körnung wird eher etwas ruhiger. Farben bleiben gleich (mittlere Abweichung 0,5–0,8 von 255 Stufen). Am meisten bringt es bei hohen Auflösungen und nah an der Kamera (Freecam).

## Umsetzung

| Teil | Datei | Inhalt |
|---|---|---|
| Paket, Hash, Formate | `src\host\gfx\texture_pack.h/.cpp` (neu) | XXH64, Hash-Schema „FTX1“, Dateinamen, DDS-Leser, Mipmaps für PNG, Index + Lade-Thread + Zwischenspeicher (96 MiB), Abzug-Thread, Statistik |
| Renderer | `src\host\gfx\nipc_renderer.cpp` | nach dem Upload einer Quelltextur: Hash, Abzug, Anfrage beim Paket; im Texturauflöser: Ersatz einsetzen; Destruktor; Statistik beim Beenden |
| Sampler | `src\host\gfx\ff_pipeline.cpp` | `texpack::AdjustSampler`: Mipfilter NONE bei einer größeren Ersatztextur → Stufen 0..log2(Faktor) |
| Tiefe bei Alpha 0 | `src\host\gfx\ff_pipeline.h/.cpp` | `DiscardsTransparentFragments` (Nachbesserung): durchsichtige Fragmente geblendeter Draws schreiben keine Tiefe mehr (Menü-Rechtecke) |
| RHI | `src\host\gfx\rhi\rhi.h/.cpp`, `d3d11\d3d11_convert.cpp` | neues Format `BC7_UNorm` |
| PNG mit Alpha | `src\host\platform\screenshot.h/.cpp` | `WritePngFileRgba` (32-Bit-PNG) für den Abzug |
| Einstellungen | `src\host\gfx\gfx_config.h/.cpp`, `src\host\core\config_schema.cpp` | `[render] texture_pack` (Ordner), `texture_pack_dump` (an/aus), beide im Reiter „Grafik“ |
| Werkzeug | `tools\texture_upscale.py` (neu), `tools\package.py` | Paket aus dem Abzug (Verfahren `cartoon`, `lanczos`, `external`, `ai`, `trace`; Schrift- und Textlisten der EU-Disc `TEXT_HASHES`, `TEXT_RECTS`); das Spielerpaket enthält das Werkzeug unter `tools\` und einen Abschnitt in `LIESMICH.txt` |
| Tests | `tests\texture_pack_tests.cpp` (neu) | 9 Fälle: XXH64-Referenzwerte, FTX1-Hash (auch aus Gastspeicher, Mipstufen ignoriert), Dateinamen, DDS (BC1/BC3/BC7/DX10/BGRA, Ablehnungen), Mipmaps, Sampler-Erweiterung, Paket-Index und Lade-Thread, Schema |
| Eingabeskript | `tools\input_scripts\g2_textures.txt` (neu) | Hauptmenü, Level 1-1 aus vier Blickwinkeln, zwei Freecam-Ansichten ohne HUD |
| Ausschluss | `.gitignore` | `texture_dump/`, `texture_pack*/`, `*.dds` |

### Hash-Schema „FTX1“

XXH64 (Seed 0) über:

1. einen 40-Byte-Kopf aus zehn u32 (little-endian): Kennung `0x31585446` („FTX1“), NiPixelFormat-Format (0 RGB, 1 RGBA, 2 PAL, 3 PALA, 4–6 DXT1/3/5, …), Bits pro Pixel, Breite und Höhe der Stufe 0, Rot-, Grün-, Blau- und Alphamaske, Bytezahl der Stufe 0;
2. die gespeicherten Bytes der Stufe 0 genau wie auf der Disc (DXT-Blöcke bzw. 16-Bit-Texel);
3. nur bei PAL/PALA: die 256 Paletteneinträge (r, g, b, a) und das Alpha-Flag der Palette.

Mipstufen gehen nicht ein. Der Hash hängt also nicht davon ab, was der Port daraus macht (BC bleibt BC, 16 Bit wird BGRA8). Gleiche Bilder in verschiedenen NIF-Dateien teilen sich eine Ersatzdatei. Der Dateiname ist der Hash in Kleinbuchstaben; ein Zusatz nach einem Nicht-Hex-Zeichen ist erlaubt (`1b29f9843b56fd49_512x256.png`), Unterordner werden mit durchsucht, `.dds` gewinnt vor `.png`.

### Was ersetzt wird – und was nicht

Ersetzt werden nur **statische Quelltexturen** (`NiSourceTexture +0x38` gesetzt), deren Bild der Port hochgeladen hat. Ausgenommen sind Filmtexturen (YUY2; ihre UVs zählen in Texeln), Render-Targets, die 8×8-Standardtextur und Texturen, die das Spiel zur Laufzeit neu beschreibt. Das Spiel sieht weiter Originalgröße, -format und -stufenzahl; nur die Host-Textur wechselt. Weil die UVs normiert sind, darf die Ersatzdatei jede Größe haben (auch keine Zweierpotenz; bei BC muss Stufe 0 durch 4 teilbar sein).

### Laden

Der Ordner wird beim Start nur nach Dateinamen indiziert. Bei einem Treffer lädt der Port zuerst das Original hoch und dekodiert die Datei in einem eigenen Thread; beim nächsten Zeichnen tauscht er die Textur aus. Dekodierte Bilder bleiben bis 96 MiB im Speicher (Neustart eines Levels). Gemessen beim Weg Intro → Hauptmenü → Level 1-1 (714 Dateien): 2× 101–116 ms Dekodierzeit insgesamt, höchstens 6 ms je Datei; 4× 235 ms, höchstens 9 ms. Kein Stocken.

## Werkzeug `tools\texture_upscale.py`

```
python tools\texture_upscale.py <Abzug-Ordner> <Paket-Ordner> [--scale 2|4]
       [--method cartoon|lanczos|external|ai|trace] [--format dds|png] [--strength 0.5] [--min-size 16]
       [--max-size 2048] [--only HASH,...] [--exclude Datei] [--limit N] [--jobs N] [--overwrite]
       [--ai-exe EXE] [--ai-model NAME] [--ai-mix 0.75] [--ai-detail N] [--trace-list builtin|none|Datei]
       [--w2x-exe EXE] [--w2x-model cunet] [--w2x-noise 1] [--w2x-mix 1.0]
       [--text-method conservative|trace|original|ai] [--text-sharpen 0.8] [--text-overlay builtin|auto|none]
```
(`--method waifu2x` und `--text-method` seit v3, siehe „Nachbesserung“; `--text-overlay` ist seit v3 standardmäßig `none`.)

Verfahren `cartoon` (Standard), Schritt für Schritt:

1. Farben unter vollständig transparenten Texeln werden aus den Nachbarn aufgefüllt (keine dunklen Säume an Alpha-Kanten).
2. Leichte kantenerhaltende Glättung (3×3 bilateral) in Originalgröße entfernt DXT-Blockrauschen in Flächen.
3. Lanczos-3-Vergrößerung, begrenzt auf den Wertebereich der vier umgebenden Originaltexel (keine hellen Höfe neben schwarzen Konturen).
4. Kantenstraffung: Jeder Texel rückt zur Hälfte (`--strength 0.5`) in Richtung der dunkleren oder helleren Farbe seiner Umgebung, je nachdem welcher er näher ist – nur an echten Kanten (`--edge`), nie über die vorhandenen Farben hinaus, die Mitte eines Übergangs weniger. Ergebnis: glatte, saubere Konturen statt Treppen, aber noch ein weicher Übergang („nicht zu knackig“).
5. Alpha (eigener Weg, nie durch eine KI): reines Ein/Aus-Alpha (Gitter, Blätter) wird nach Lanczos bei 50 % geschwellt, also glatte, saubere Umrisse ohne Halbtransparenz. Weiches Alpha (seit v3): Lanczos, auf die umgebenden Originaltexel begrenzt, plus vier Rückprojektionen – nie geschwellt, auf Originalgröße gemittelt gleich dem Original.
   Farbe unter (fast) durchsichtigen Texeln (seit v3): die Originalfarbe (Lanczos), nicht die aufgefüllte aus Schritt 1 – bei additiv gezeichneten Texturen ist sie sichtbar.
6. Ausgabe DDS: BC1 (deckend oder Ein/Aus-Alpha mit schwarzen durchsichtigen Texeln) bzw. BC3 (sonst) mit allen Mipstufen (seit v3 einfacher 2×2-Mittelwert wie bei den Originalen, nicht mehr alphagewichtet) – dieselbe Speicherklasse wie die DXT-Originale (2×: etwa 4-mal, 4×: etwa 16-mal so viel). Texturen kleiner als 16 Texel, einfarbige und solche, die die Höchstgröße schon erreichen, bleiben original.

`--method lanczos` ist die weiche Vergleichsvariante ohne Schritte 2 und 4. `--format png` schreibt RGBA-PNG (der Port erzeugt die Mipmaps, braucht aber 32 Bit je Texel).

**Beliebiger externer Upscaler:** `--method external --external-exe <exe> --external-args "..."` ruft ein Programm je Textur auf (`<exe> -i <ein.png> -o <aus.png> <Argumente>`) und mischt 70 % davon mit dem `cartoon`-Ergebnis (`--external-mix`). Für Real-ESRGAN ist `--method ai` (nächster Abschnitt) schneller und abgestimmt.

## KI-Paket für das ganze Spiel

Wunsch: das ganze Spiel „dumpen und mit geeigneter KI hochskalieren, ohne den Stil zu brechen“, „nicht übertrieben skalieren“ – also 2×, nicht 4×.

### Modellwahl

Verglichen an Spieltexturen (Wände, Boden, Kisten, Figuren) mit den Modellen einer vorhandenen Real-ESRGAN-ncnn-Installation:

| Modell | Ergebnis |
|---|---|
| **`realesr-animevideov3`** (2×/3×/4× nativ) | behält den Comic-Look: Konturen sauber und gleichmäßig, flache Flächen bleiben flach, Farben bleiben. Glättet feine gemalte Körnung etwas weg – deshalb Mischung und Detailrückführung (unten) |
| `realesrgan-x4plus-anime` | macht Wände flach wie Plastik, Linien zu hart („zu knackig“) |
| `realesrgan-x4plus` | Modell für Fotos: hart, leicht überschärft, verändert den Malstil |
| `4x_NMKD-Siax_200k`, `4xNomos8kSC`, `4xLSDIRplusC` | erfinden Risse, Kratzer und Strukturen, die es im Original nicht gibt |

### Voreinstellung `--method ai`

```
python tools\texture_upscale.py <Abzug> <Paket> --scale 2 --method ai --ai-exe <Ordner>\realesrgan-ncnn-vulkan.exe
```

Statt `--ai-exe` geht die Umgebungsvariable `FUT_REALESRGAN`. Das Modell wird in `models\` neben der exe gesucht (`--ai-models` ändert das), `--ai-model` wählt ein anderes. Das Werkzeug lädt nichts herunter.

1. Farbe unter transparenten Texeln auffüllen (wie `cartoon` Schritt 1), alle Texturen in einen Ordner schreiben und den Upscaler **einmal** für alle aufrufen (2720 Texturen in 7 s auf der Grafikkarte).
2. Mischung **75 % KI + 25 % `cartoon`** (`--ai-mix 0.75`): Die KI liefert die sauberen Linien, der Anteil des Comic-Verfahrens hält die gemalte Körnung der Wände.
3. **Detailrückführung** (`--ai-detail 1`, neu): eine Rückprojektion. Das Ergebnis wird auf Originalgröße gemittelt, die Abweichung zum Original bilinear vergrößert wieder addiert und auf die Werte der umgebenden Originaltexel begrenzt (keine Säume). Danach stimmt das Paket aus der Entfernung (Mipstufe 1 = Originalgröße) wieder mit dem Original überein, und schwache gemalte Details (Risse im Hangarboden, Schmutz, Rost), die die KI glättet, kommen zurück. Ein Durchgang reicht (Restabweichung halbiert); mehr Durchgänge bringen auch das DXT-Rauschen des Originals zurück.
4. **Mischung:** 0,75 bleibt. Die KI allein (1,0) glättet bei einem Teil der Wand- und Bodentexturen die gemalte Struktur deutlich (gemessen: von 250 strukturierten Texturen ab 64 Texeln aus 1-1 behalten 16 weniger als 80 % der Feinstruktur des Originals, die schlimmste 50 %); mehr `cartoon`-Anteil würde die Linien wieder breiter machen. Die Detailrückführung löst das, ohne die Mischung zu verschieben. Im Spiel (Vergleich Original / KI ohne / KI mit Rückführung, `l11_tanks3.png`) wirkt das Ergebnis weder „zu knackig“ noch matschig.
5. Alpha wie `cartoon` (Ein/Aus-Alpha bei 50 % geschwellt: Gitter, Blätter, Zäune bleiben sauber ohne Halbtransparenz), Ausgabe DDS BC1/BC3 mit Mipmaps.

### Schrift: nachzeichnen statt hochrechnen (`trace`)

(Stand v2. Seit v3 ist für diese Texturen `--text-method conservative` Standard – die Nachzeichnung machte die Kontur fetter, siehe „Nachbesserung“; `--text-method trace` wählt das folgende Verfahren.)

Schrift darf weder durch die KI (wellige Buchstaben) noch durch Filter (weiche Kanten). Texturen mit Schrift werden deshalb **nachgezeichnet**, egal welche `--method` gilt:

1. Abdeckung: Alpha (Schriftatlas, Schilder mit Alpha) oder bei deckenden Texturen der Tintenanteil einer Zwei-Farben-Trennung (Otsu-Schwelle auf der Helligkeit, „reine“ Farben als 10.- bzw. 90.-Perzentil).
2. **Vorzeichenbehaftetes Abstandsfeld** dieser Abdeckung in Originalgröße (Konturtexel behalten ihre Teilabdeckung als Lage), bikubisch auf 2×, dann neu geschwellt mit einer Rampe von genau einem Pixel. Ergebnis: glatte, scharfe Konturen wie bei Vektorschrift, keine Treppen, keine Unschärfe.
3. In jeder Glyphe (8-zusammenhängender Teil des Alphas) wird die Innenzeichnung genauso nachgezeichnet: weißer Kern, schwarze Kontur. Die Farben kommen aus den eindeutigen Texeln ihrer Klasse (5×5-Mittel gegen das 16-Bit-Rauschen). Weiße Schrift, schwarze Kontur und deren Breite bleiben (Alpha-Fläche des Atlas auf 0,03 % gleich, helle Fläche 1,4 % größer).
4. Teile, die nicht zweifarbig sind (die farbigen Tastensymbole im Atlas: Kreuz, Kreis, Dreieck, Quadrat, A/B/X/Y, Pfeile), behalten eine weiche Farbe (Lanczos, begrenzt) innerhalb ihrer nachgezeichneten Alphakontur.
5. Keine Kontur wächst über die Originalabdeckung + 1 Texel hinaus, dicht gepackte Glyphenzellen laufen also nicht ineinander (eine `.fnt`-Metrik ist dafür nicht nötig).
6. Ausgabe als PNG (verlustfrei wie die 16-Bit-Originale; BC1 würde die dünnen Konturen verschmieren). Ein „echtes“ Vektor-Nachzeichnen (Marching Squares + Kurvenglättung) wurde nicht gebraucht: Das Abstandsfeld gibt bei 2× schon glatte Kurven und trifft die Strichstärke besser.

Gefunden wurden die Schrifttexturen über die Fontdateien der Disc (`fonts\*.nif` aus `SysPAL.img` und `FutPAL02.img`, Pixelabgleich mit dem Abzug), über das Format (16 Bit, wenig Farbe, viele kleine Teile) und durch Ansehen aller Kandidaten. **Das Spiel benutzt nur einen Schriftatlas**: `fonts\latin1.nif` (512×256, ARGB4444) für Menüs, HUD-Zähler, Hinweise und Dialoge. Die anderen Fonts (arial…, comic…, georgia…, font_bas, latin1_xbox/_ps2/_short) wurden in keinem der 23 Läufe geladen. Die Liste steht im Werkzeug (`TEXT_HASHES`; `--trace-list <Datei>` ersetzt sie, `--trace-list none` schaltet ab):

| Hash | Inhalt |
|---|---|
| `1b29f9843b56fd49` | `fonts\latin1.nif`, der Schriftatlas (auch HUD-Ziffern und Tastensymbole) |
| `684736283da5d540`, `da595f0f469225ed` | Copyright-Bildschirm (zwei Hälften) |
| `40dbf373555b807e` | „This game uses NDL graphics technology“ (Handschrift) |
| `db2fdf19f78765d5` | Liste ART / CODE (Level 5) |
| `18d5aea7ffae8f80` | Schild „Closed till Dark God appeased“ (Level 3) |
| `c4ffe9de8637ef9e` | Straßenschild „W. 4th St“ |
| `cddbb855f7cace5a` | Nummernschild „27831769“ |
| `eeeb7e98836a77d8` | Türschilder Planet Express „Dr. Zoidberg / Bureaucrat / Hermes Conrad“ |
| `3da2b52472bf6e5a`, `8b50b6fc02be0891`, `2a1e67e9e3a1afb2`, `a1403dfc150bb2c1`, `8595cb6c3c24d5cd`, `34be2fad3496c9e8`, `7d39892595509ab6` | Namensschilder Zoidberg, Professor, Leela, Bender, Hermes, Fry, Amy |
| `5337acf412eeba09`, `b18fecb4dc22d862` | Aufschriften „Mail“, „In / Out“ |

### Text in Bildern (Ladebildschirme)

Die 25 Ladebildschirme (je ein 1024×512-Bild und vier 256×256-Kacheln; im Spiel werden die Kacheln gezeichnet) und die Frontend-Bilder enthalten gedruckten Text: „LADEVORGANG, BITTE WARTEN…“, Werbesprüche („Für den verwöhnten Schritt:“, „Mit 10 % mehr Liebe als andere führende Marken hergestellt!“, „Fragen Sie nach unserer Hochdrucktaktik!“ …), Copyright-Zeilen. Das Bild bleibt beim KI-Ergebnis, der **Text wird darüber nachgezeichnet** (`text_overlay`):

* Buchstaben finden: Teile, die deutlich dunkler oder heller als ihre Umgebung sind (Abstand zum 15×15-Mittel > 0,22), buchstabengroß (4–26 Texel hoch), in einer Reihe von mindestens drei Buchstaben, ringsum einfarbiges Papier.
* Weil das auch Zähne, Augen, Glanzlichter und Logostücke findet, gilt es nur in **festen Rechtecken je Textur** (`TEXT_RECTS`: 54 Texturen, 189 Rechtecke). Ausgewählt durch Ansehen aller 510 gefundenen Gruppen (213 davon echter Text) und danach der Ergebnisse: 24 Textgruppen auf gestreiftem oder schattiertem Grund (Flaggenstreifen bei „WO STECKST DU, DU FEIGLING? Komm-komm-komm“, weiße Schrift mit Schlagschatten bei „Auf gute, alte …“, das Gamebryo-Logo) wurden durch Nachzeichnen schlechter als mit der KI allein (Säume, Flecken) und stehen nicht in der Liste.
* Im Umkreis von 4 Texeln um die Buchstaben (weich ausgeblendet) wird das Bild ersetzt durch die Papierfarbe aus dem Bild direkt neben der Schrift und die Buchstaben aus dem Abstandsfeld ihrer Abdeckung in der Druckfarbe.
* **Große Schlagzeilen und Logos** (LIGHTSPEED SLIPS, Glagnar's Menschenchips, WERDE SOLDAT, Haben Sie Protoplasma?, MA'S NOSTALGISCHES ROBOTERÖL, Fehlzündung Eddie's, Thompson's Teeth, Robo Fresh …) stehen nicht in der Liste: Die KI zeichnet sie mit sauberen Konturen, und ihre Schatten und Verläufe würde das Nachzeichnen zerstören.
* `--text-overlay auto` sucht ohne Rechteckliste in jedem 16-Bit-RGB-Bild ab 256×256 (für andere Abzüge; findet auch Nicht-Text), `none` schaltet ab.

### Ergebnis

* **Abzug** (`run\tex\dump`, ein gemeinsamer Benutzerdatenordner; je Lauf eine INI unter `run\tex\d_<Level>\`, Skript: Intro → „Neues Spiel“ mit `[game] start_level` → steuerbar → einmal im Kreis drehen → Pause; in 1-1 zusätzlich der Levelende-Bildschirm; ein Frontend-Lauf mit den Menüpunkten; 1280×960, MSAA aus, Lautstärke 0, Rumble aus, je 32–41 s, alle Rückgabe 0): **2768 verschiedene Texturen**, 23 MB PNG. Neue Texturen je Lauf (Dateinamen der Level): Frontend 128, Level1-1 599, 1-2 142, 1-3 108, 1-4 198, 1-5 270, 1-7 63, 1-6 99, 2-1 122, 2-6 35, 2-2 84, 2-3 98, 2-4 87, 2-5 59, 3-1 161, 3-2 120, 3-3 56, 3-6 17, 3-4 31, 4-1 141, 5-1 82, 5-2 47, 5-3 21. Formate: DXT1 1284, DXT3 560, RGB565 550, ARGB4444 374; Alpha: 1864 ohne, 30 Ein/Aus, 874 weich; größere Seite: 16 Texel 348×, 32 583×, 64 814×, 128 742×, 256 254×, 512 1×, 1024 25×.
* **Paket** (`D:\FuturamaDecomp\run\texpack_ai_x2`, nicht im Repository): 2738 Dateien (2720 DDS, 18 PNG mit nachgezeichneter Schrift), **116 MB**, 30 Texturen übersprungen (kleiner als 16 Texel oder einfarbig), 0 Fehler. Bauzeit **49 s** (KI 7 s, Rest mit 16 Prozessen, davon etwa 20 s für den Text der 54 Bilder). Keine Ausschlüsse nötig: Gitter, Zäune, Blätter (Ein/Aus-Alpha) bleiben sauber; Schrift ist durch `trace` und `TEXT_RECTS` abgedeckt.
* **Im Spiel** (Läufe `run\tex\ls_base_*` / `ls_pk_*`: Ladebildschirm und Levelstart aller 22 Level; `v_base_*` / `v_fin_*`: Hauptmenü, 1-1, 1-2, 2-1, 3-1 mit drei Blickrichtungen und Freecam-Nahaufnahmen; `v_hibase/v_hifin_Level2-1`: 2560×1920):
  * Je Level 308–737 statische Texturen nachgeschlagen, 98–99 % Treffer, 0 Fehler, Ersatztexturen auf der Grafikkarte 12–16 MiB, dekodiert 21–26 MiB (Zwischenspeicher 96 MiB, nichts verdrängt), Dekodierzeit 103–159 ms insgesamt, höchstens 12 ms je Datei. Keine Speicherfehler (32-Bit-Prozess; „0 out of memory“ in allen Protokollen).
  * **Stil:** bleibt. Konturen der Figuren und Gegenstände sind glatter und gleichmäßig dunkel, flache Comicflächen bleiben flach, die gemalten Risse und Flecken von Böden und Wänden bleiben dank Detailrückführung sichtbar. Nicht „zu knackig“: kein Überschärfen, keine Säume, keine erfundenen Strukturen. Bei 1280×960 ist der Unterschied in der Totalen mäßig, deutlich in Nahaufnahmen (Freecam), bei hohen Auflösungen und bei den Ladebildschirmen.
  * **Schrift:** Menütexte („Neues Spiel“, „Optionen“), HUD-Zähler („0/11“, „5“) und Hinweise („Drücken Sie X, um einen Wirbel …“) behalten Farbe und Konturbreite, die Kanten sind glatt statt verwaschen, die Tastensymbole behalten ihre Farben. In allen 22 Ladebildschirmen ist „LADEVORGANG, BITTE WARTEN…“ randscharf ohne wellige Buchstaben; Werbesprüche und Schlagzeilen sauber.
  * Vergleichsbilder (Spielgrafik, nur lokal unter `run\tex\sheets\`): `l11_*.png`, `l12_*.png`, `l21_*.png`, `l31_*.png`, `ov_*.png` (Level), `fin_menu.png`, `fin_hint21.png`, `hi_hint.png`, `hi_menu.png`, `hi_hud.png` (Schrift), `lsF1.png` … `lsF7.png`, `lsT1.png`, `ls11_*.png` (Ladebildschirme im Spiel), `tr_*.png` (Schriftatlas), `ovz0.png` … `ovz5.png` (jedes Textrechteck: KI gegen KI + Nachzeichnen).

### Anleitung (eigene Real-ESRGAN-Kopie)

(Empfohlen ist seit v3 waifu2x: in Schritt 2 `--method waifu2x --w2x-exe <Ordner>\waifu2x-ncnn-vulkan.exe`, daneben der Ordner `models-cunet`. Etwa eine halbe Minute.)

1. Abzug wie in „Anleitung für Spieler“, Schritt 1 – für das ganze Spiel jedes Level einmal laden (Startlevel im Launcher, Reiter „Spiel“, macht das schnell; ein paar Sekunden im Level genügen, die Texturen werden beim Laden hochgeladen).
2. `python tools\texture_upscale.py "%LOCALAPPDATA%\FuturamaPC\texture_dump" "%LOCALAPPDATA%\FuturamaPC\texture_pack_ai" --method ai --ai-exe <Ordner>\realesrgan-ncnn-vulkan.exe` (daneben muss `models\realesr-animevideov3-x2.param/.bin` liegen). Etwa eine Minute.
3. Im Launcher, Reiter „Grafik“, „Texturpaket“ auf diesen Ordner setzen.
4. Stellschrauben: `--ai-mix` (0,6 körniger, 0,9 glatter), `--ai-detail 0` (glatter), `--text-overlay none`, `--trace-list none` (Schrift dann auch per KI, nicht empfohlen), `--exclude datei.txt` für einzelne Texturen, `--scale 4` (16-facher Speicher, nicht empfohlen).

## Nachbesserung: Alpha, Schrift, waifu2x (2026-10-13)

Rückmeldung zum KI-Paket: „Das Textur-Upscaling ist nicht zufriedenstellend. Besonders Texturen mit Alpha scheinen kaputt zu sein (besonders im Hauptmenü erkennbar, oder bei blinkenden Lampen).“ Dazu: Rechtecke hinter „Neues Spiel“, Schrift zu fett, und ein mit waifu2x hochskaliertes PS2-Paket gefällt besser.

### Ursache 1: Rechtecke hinter dem gewählten Menüpunkt (Renderer, auch ohne Paket)

* Die Rechtecke gibt es **auch ohne Texturpaket** (Lauf `run\tex2\base_menu`, Bild 843; schon in `hi_menu.png` des ersten Berichts zu sehen). In den xemu-Referenzbildern des Hauptmenüs (`run\xemu_ref\t0075.0s.png` … `t0132.0s.png`) gibt es sie nicht.
* Draw-Protokoll (`[render] trace_frames`) des Menübilds: Der Text „Neues Spiel“ (Draw 289, Schriftatlas, 10 Glyphen-Quads, Blend SRCALPHA/INVSRCALPHA, **kein Alphatest, Tiefe schreiben an**) wird vor dem pulsierenden Hervorhebungs-Quad des Knopfes gezeichnet (Draw 340: Kopie der Knopffläche, Materialfarbe 0,8 grau, Alpha pulsiert 0…0,5, Tiefe nur testen). Nach D3D8-Regeln schreibt auch der völlig durchsichtige Teil jedes Glyphen-Quads Tiefe – die spätere Hervorhebung wird dort verworfen. Ergebnis: Wort-große Rechtecke, die im Takt der Hervorhebung dunkler wirken (Bild 805: Alpha 0, keine Rechtecke; Bild 843: deutlich).
* **Behebung** `ff::DiscardsTransparentFragments` (`ff_pipeline.h/.cpp`): Draws mit Blend SRCALPHA/INVSRCALPHA oder SRCALPHA/ONE, ohne Alphatest und mit Tiefenschreiben verwerfen Fragmente mit Alpha < 0,5/255 (Alphatest GREATER 0 in den Draw-Konstanten). Die Farbe ändert sich dadurch nirgends (solche Fragmente tragen nichts bei), nur die Tiefe. Ergebnis: keine Rechtecke mehr, mit und ohne Paket (`menu_fix_cmp.png`). Test: `ff: alpha-blended draws discard fully transparent fragments`. Abweichung von D3D8 in `GFX_CONTRACT.md` 6.4 vermerkt.
* Weder die Spur-PNG noch der Sampler waren schuld: Der Schriftatlas hat unter Alpha 0 im Original wie im Paket Schwarz (0/0/0 bzw. ≤ 4), Alpha 0 bleibt 0; der Text wird mit POINT/POINT/NONE gelesen, `AdjustSampler` gibt Mipstufe 1 dazu, die alphagewichteten PNG-Mips ändern das Alpha nicht.

### Ursache 2: Alpha-Texturen im alten Paket (Werkzeug)

Prüfung aller 894 Alpha-Texturen (Paket per BC-Dekoder gelesen, Stufe 0 auf Originalgröße gemittelt, mit dem Abzug verglichen):

| Paket | mittl. Alphafehler (von 255) | 95 % | max | Farbe unter Alpha < 32: mittl. / max Abweichung | Texturen mit Farbabw. > 32 | mit Alphafehler > 4 |
|---|---|---|---|---|---|---|
| `texpack_ai_x2_old` | 2,31 | 6,31 | 14,1 | 14,8 / **255** | **110** | **165** |
| `texpack_ai_x2` (neu) | 0,73 | 1,93 | 4,5 | 0,9 / 23,9 | 0 | 4 |
| `texpack_w2x_x2` | 0,73 | 1,93 | 4,5 | 0,9 / 23,9 | 0 | 4 |

Die Fehler im Einzelnen:

1. **Farbe unter durchsichtigen Texeln überschrieben.** Schritt 1 (Farbe aus den Nachbarn auffüllen, damit die KI keine dunklen Säume an Alphakanten erzeugt) blieb im Paket stehen. Das Spiel zeichnet aber einige Alphatexturen additiv: In 1-2 kommen neben SRCALPHA/INVSRCALPHA auch ONE/ONE, SRCALPHA/ONE und ZERO/SRCALPHA vor (Draw-Protokoll `run\tex2\l12_base`, Bild 1700). Bei ONE/ONE ist die Farbe unter Alpha 0 voll sichtbar: Aus einem Lampen-Quad mit schwarzem Rand wurde ein ganz leuchtendes (Beispiele `viz_bad2.png`: Gitter `2aa5fe52dc2ca391`, `8e1ecbd0ab59a8d4`, Lava-Risse `b47be6a57c6f7e10`).
2. **BC1 für Ein/Aus-Alpha** auch bei DXT3/ARGB4444-Originalen mit weißer oder farbiger Farbe unter Alpha 0 – BC1 macht daraus Schwarz (22 Texturen).
3. **Weiches Alpha** nur mit Lanczos + Abschneiden auf 0…1: Überschwinger um harte Kanten blieben als zusätzliche Deckung stehen (bis 14/255 im Mittel einer Textur). Im Spiel: Glanzschein des Geld-Symbols und Lichtschein am Gitter in 1-2 breiter und heller (`l12_money.png`, `l12_gate.png`).
4. **Alphagewichtete Mipstufen** schoben die sichtbare Farbe unter durchsichtige Texel (für additive Texturen falsch).

Behebung im Werkzeug (v3, für alle Verfahren):

* Farbe und Alpha gehen getrennte Wege. Die KI bekommt weiter die aufgefüllte Farbe (saubere sichtbare Kanten), **unter (fast) durchsichtigen Texeln wird aber die Originalfarbe** (Lanczos, begrenzt) eingesetzt: Gewicht der KI-Farbe = min(1, 4 × hochskaliertes Alpha) (`keep_transparent_colour`).
* Ein/Aus-Alpha wie bisher bei 50 % geschwellt (Alphatest). **Weiches Alpha nie schwellen:** Lanczos, auf die vier umgebenden Originaltexel begrenzt (keine Ringe um Lampen), dann vier Rückprojektionen, damit das Ergebnis auf Originalgröße gemittelt dem Original entspricht (`upscale_alpha`).
* BC1 nur für deckende Texturen und Ein/Aus-Alpha, unter dessen durchsichtigen Texeln das Original schwarz ist; sonst BC3 (882 statt 860 BC3-Dateien, Größe unverändert 117 MB).
* Mipstufen als einfacher 2×2-Mittelwert wie die Originale.

### Ursache 3: Schrift zu fett

Das Nachzeichnen (`trace`) macht aus der weichen dunklen Kontur des Atlas (äußere Hälfte halbtransparent grau) ein volles schwarzes Band: Die Fläche stimmt, der Eindruck ist fetter (`menu_text_zoom.png`, Mitte). Neu ist **`--text-method conservative`** (Standard) für Schriftatlas, Schilder und Namensschilder: Lanczos von Farbe und Deckung, auf die Originaltexel begrenzt, eine Rückprojektion der Deckung (gleiche Strichstärke), dann eine leichte Unschärfemaske (`--text-sharpen 0.8`, 3×3), wieder begrenzt (`text_upscale`). Im Spiel ist „Neues Spiel“ jetzt eine saubere, schärfere Fassung des Originals mit derselben Strichstärke, ohne Kästen (`menu_text_zoom.png`, unten; Vergleich am Atlas: `font_proto.png`). `--text-method trace` (alt) und `original` (Schrift bleibt original) gibt es weiter.

### waifu2x statt Real-ESRGAN

* **`--method waifu2x`** (neu): `waifu2x-ncnn-vulkan.exe` (selbst installiert, `--w2x-exe` oder `FUT_WAIFU2X`), Modell `cunet` (`--w2x-model`, Ordner `models-cunet` neben der exe; auch `upconv_7_anime_style_art_rgb`, `upconv_7_photo`), Rauschstufe 1 (`--w2x-noise`, `-n`), `-s 2`, ein Lauf über alle Texturen (2720 Texturen in 7 s). 100 % waifu2x (`--w2x-mix 1.0`), keine Rückprojektion (`--ai-detail` ist für waifu2x 0: waifu2x behält die gemalte Struktur selbst). Alpha und Schrift wie oben, nie durch die KI.
* **Rauschstufe** gegen das PS2-waifu2x-Paket (nur als lokale Sichtreferenz, nichts davon geht in ein Paket): Xbox- und PS2-Texturen über Inhalt zugeordnet (Vorschaubilder 16×16 inkl. Alpha mit PS2-Alpha × 2, gleiches Seitenverhältnis, dann 32×32-Abstand < 0,002, Gradientenkorrelation > 0,85, Mindestkontrast): **272 sichere Paare** von 544 Kandidaten. An 60 davon (≥ 64 Texel) Ergebnis gegen PS2-Bild auf gleicher Größe, Mediane:

| Verfahren | Gradientenverhältnis (1 = so scharf wie PS2) | Hochpass-Energie | Hochpass-Korrelation |
|---|---|---|---|
| Lanczos | 0,965 | 0,80 | 0,77 |
| alt: animevideov3 75 % | 0,852 | 0,96 | 0,84 |
| cunet n −1 / 0 | 1,024 / 1,018 | 1,11 / 1,08 | 0,82 / 0,83 |
| **cunet n 1** | **0,986** | 1,06 | 0,83 |
| cunet n 2 / 3 | 1,024 / 0,935 | 1,09 / 1,04 | 0,83 / 0,85 |

  Die Stufen liegen nah beieinander; n 1 trifft die Schärfe des PS2-Pakets am besten, n 3 glättet schon sichtbar. Real-ESRGAN animevideov3 ist deutlich weicher (0,85) – das war der „flache“ Eindruck. Ein PS2-Hybridpaket wurde auf Wunsch nicht gebaut: Quelle bleibt das Xbox-Original.
* **Text in Ladebildschirmen:** waifu2x hält „LADEVORGANG, BITTE WARTEN…“, Werbesprüche und Copyright-Zeilen allein sauber, ohne wellige Buchstaben (`rects1.png`); das Nachzeichnen machte sie etwas fetter. `--text-overlay` ist daher standardmäßig `none` (`builtin` geht weiter).

### Pakete und Vergleich im Spiel

* `D:\FuturamaDecomp\run\texpack_w2x_x2` (**empfohlen**): 2738 Dateien (2720 DDS, 18 PNG), 117 MB, 30 übersprungen, 0 Fehler, 26 s (waifu2x 7 s). `run\texpack_ai_x2`: mit v3 neu gebaut (Real-ESRGAN animevideov3, sonst gleich). `run\texpack_ai_x2_old`: das alte Paket zum Vergleich.
* Läufe (`run\tex2\<Lauf>\futurama.ini` im Arbeitsbaum, 2560×1440, 16:9, MSAA 8×, Lautstärke 0, Rumble aus, Filme übersprungen, je 14–33 s, Rückgabe 0): `base_menu`/`base_fix` (Rechtecke vor/nach der Renderer-Korrektur), `l11_base|old|w2x` (Hauptmenü + Level 1-1), `l12_base|old|w2x` (Level 1-2). Im 1-1-Lauf: 737 statische Texturen nachgeschlagen, 730 Treffer, 339 ersetzt, 0 Fehler, 14 MiB Ersatz auf der Grafikkarte (alt und neu gleich).
* Ergebnis (Ausschnitte unter `D:\FuturamaDecomp\run\tex2_compare\`, je Original / altes KI-Paket / waifu2x):
  * **Hauptmenü** (`menu_fix_cmp.png`, `menu_text_cmp.png`, `menu_text_zoom.png`, `menu_full.png`, `menu_lamps.png`, `menu_steam.png`): keine Rechtecke; Schrift wie das Original, nur sauberer; Lampen, Dampf und Elektroden-Glühen wie im Original.
  * **1-2 Kanal** (`l12_1521.png`, `l12_gate.png`, `l12_money.png`, `l12_crates.png`): Lichtschein am Gitter und Glanzschein des Geld-Symbols wieder so weich und schmal wie im Original (altes Paket: breiter, heller); Kisten mit erhaltener gemalter Struktur.
  * **1-1 Hangar** (`l11_start.png`, `l11_tanks.png`, `l11_1745_wall.png`): Tanks, Warnstreifen und Schmutz bleiben wie gemalt (animevideov3 glättete den Schmutz), rote Lampen unverändert.
  * Alpha-Texturen einzeln (`viz_bad2.png`: Farbe, Alpha, über Grau, Farbe × Alpha): Farbe unter Alpha 0 wieder wie im Original.


## Prüfung

* Nachbesserung: RelWithDebInfo `build\tex2` mit `FUT_WERROR=ON` ohne Warnungen, `fut_tests` 527 Fälle bestanden, 1 übersprungen (ein neuer Fall). Läufe und Vergleich: Abschnitt „Nachbesserung“.
* Bauten: RelWithDebInfo `build\g2`, Debug `build\g2-werr` und clang-cl Debug `build\g2-clang` (beide `FUT_WERROR=ON`), ohne Warnungen. `fut_tests` in allen drei Bauten: 493 Fälle bestanden, 1 übersprungen (wie vorher), davon 9 neue G2-Fälle mit 142 Prüfungen.
* Läufe (`run\g2\<Lauf>\futurama.ini`, eigene Benutzerdaten, `[debug] movies = skip`, Lautstärke 0, Rumble aus, 1280×960, 4× MSAA, Skript `g2_textures.txt`, je etwa 34 s, Rückgabe 0):

| Lauf | Einstellung | Ergebnis |
|---|---|---|
| `dump` | `texture_pack_dump = 1` | 737 Texturen hochgeladen, **721 PNG** geschrieben (16 doppelt), 4,8 MB; DXT1 319, DXT3 111, RGB 565 151, RGBA 4444 140; Alpha: 475 ohne, 11 Ein/Aus, 235 weich |
| – | `texture_upscale.py` 2× / 4× | 714 Dateien (7 übersprungen: zu klein oder einfarbig), 0 Fehler, 5–7 s bzw. 19 s; 20 MiB bzw. 65 MiB |
| `base` | ohne Paket | keine G2-Zeile im Protokoll, 738 Texturen angelegt |
| `pack` | 2×-Paket | 737 statische Texturen nachgeschlagen, 730 Treffer, **296 ersetzt** (gezeichnet), 0 Fehler, 376 seit dem Laden nicht mehr gezeichnet (Hauptmenü), 58 vorher freigegeben, 7 ohne Datei; Ersatz 8 MiB auf der Grafikkarte |
| `pack4` | 4×-Paket | wie `pack`, Ersatz 33 MiB |

* Vorher/Nachher (Ausschnitte in `run\g2\compare\`, je links original, Mitte 2×, rechts 4× bzw. links/rechts):
  * **Tanks** (`tank.png`, Freecam): Original mit sichtbaren 4×4-Blöcken auf der grauen Fläche; 2× und vor allem 4× glatt, Warnstreifen mit sauberen Kanten, Schmutzflecken etwas weicher.
  * **Boden** (`floor.png`, `floor2.png`): Risse etwas klarer, Blockrauschen weg; die feine Körnung wird ruhiger.
  * **Schiff** (`ship.png`): Fensterrahmen glatter, weniger ausgefranst.
  * **Geld-Symbol, Hauptmenü** (`hud.png`, `menu.png`): praktisch unverändert, keine Säume, Schrift sauber.
  * Mittlere Farben gleich (z. B. ganzes Bild 73,4/71,4/45,7 vorher und nachher), mittlere Abweichung je Pixel 0,5–0,8.
* Ein Fehler im ersten BC1-Kodierer (falsche Endpunkte nach der Ausgleichsrechnung → dunkle 4×4-Blöcke auf dem Boden) wurde im ersten Paketlauf gefunden und behoben: Jetzt gewinnt je Block die beste von drei Endpunkt-Varianten.

## Anleitung für Spieler

1. **Abzug:** Launcher, Reiter **Grafik** → **Texturen abspeichern** an. Spielen und dabei die Level besuchen, die aufgewertet werden sollen (jede geladene Textur wird einmal gespeichert; Hauptmenü und Level 1-1 ergeben etwa 720 Dateien). Danach die Option wieder ausschalten. Die Dateien liegen in `<Benutzerdaten>\texture_dump\` (Standard `%LOCALAPPDATA%\FuturamaPC\texture_dump\`; „Ordner öffnen“ im Reiter „Pfade“ zeigt die Benutzerdaten).
2. **Paket bauen:** Python mit numpy und Pillow muss installiert sein (mit KI: Abschnitt „KI-Paket“, Anleitung). In einer Eingabeaufforderung im Spielordner:
   `python tools\texture_upscale.py "%LOCALAPPDATA%\FuturamaPC\texture_dump" "%LOCALAPPDATA%\FuturamaPC\texture_pack"`
   (2×, Standard). Für mehr Schärfe in der Nähe `--scale 4`; weicher: `--strength 0.3`; knackiger: `--strength 0.8`.
3. **Einschalten:** Launcher, Reiter **Grafik** → **Texturpaket** → Ordner `texture_pack` wählen. Spielen. Leer lassen = Originaltexturen.
4. Rückmeldung:
   * [ ] Passt der Look zum Comic-Stil? Zu weich, zu knackig (`--strength`)?
   * [ ] Fallen einzelne Texturen negativ auf (Säume, Flecken, Blöcke)? Den Hash findet man über `index.tsv` bzw. den Dateinamen im Abzug; mit `--exclude datei.txt` bleibt eine Textur original.
   * [ ] Ladezeiten und Bildrate mit 4×?

Pakete enthalten abgeleitete Spielgrafik: nicht weitergeben und nie ins Repository legen.

## Bekannte Grenzen

* **Wenig neue Details.** Das Standardverfahren macht Kanten und Flächen sauberer, erfindet aber keine Strukturen. Mehr Detail bringt nur ein KI-Upscaler (selbst installieren) oder Handarbeit an einzelnen Texturen (PNG in den Paketordner legen).
* BC1 rundet Farben auf 5/6/5 Bit wie die DXT1-Originale; sehr feine Farbverläufe können leicht stufig werden. Wer das vermeiden will: `--format png` (mehr Speicher) oder eigene BC7-DDS.
* Nach dem Laden eines Levels kann für einige Bilder noch das Original zu sehen sein, bis die Datei eingelesen ist (gemessen höchstens einige Millisekunden je Datei).
* Ersatztexturen brauchen mehr Grafikspeicher (4×: 33 MiB statt 8 MiB im gemessenen Abschnitt). Der Port ist ein 32-Bit-Prozess; sehr große Pakete (alles 4× als PNG) sollte man vermeiden.
* `--method ai` ist mit dem ganzen Spiel geprüft, `--method external` nur an Einzeltexturen. Schriftliste und Rechteckliste gelten für die EU-Retail-Disc; andere Fassungen haben andere Hashes (dann `--text-overlay auto` bzw. eine eigene `--trace-list`).
* Text auf gestreiftem oder schattiertem Grund in Ladebildschirmen und große Schlagzeilen bleiben beim KI-Ergebnis: sauberer als das Original, aber nicht nachgezeichnet.
* Nachbesserung, offen: (1) Die Seitenfläche der Werkbank in 1-1 (`l11_1847_box2.png`) ist mit beiden Paketen etwas dunkler als im Original, bei mittlerer Farbe der Texturen ohne Verschiebung (Paketmittel ±0,1/255) – vermutlich andere Mipstufen (die Originale bringen eigene von der Disc mit, das Paket den 2×2-Mittelwert); nicht weiter verfolgt. (2) Blinkende Lampen wurden in 1-1 und 1-2 nur zu festen Bildnummern angesehen; ein Fehlerbild wie im Hauptmenü war dort mit dem alten Paket nicht zu finden, die messbaren Alphafehler (Tabelle) sind behoben. (3) Die Ersatz-PNGs (Schrift) bekommen ihre Mips weiter vom Port, alphagewichtet; für die Schrift (Schwarz unter Alpha 0, SRCALPHA-Blend) ist das richtig.
* Die fünf Texturen, die derzeit auf die 8×8-Standardtextur zurückfallen, und Render-to-Texture gehören zu M3e/M8 und werden nicht ersetzt.
