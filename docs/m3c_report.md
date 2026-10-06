# Meilenstein M3c: Launcher und Bildqualität – Bericht

Stand: 2026-10-06 (Phase 4, nach Behebung der Review-Befunde) · Ziel-XBE: `orig\retail_eu\default.xbe` (Retail EU) · Verbindliches Design: [`docs/loader_design.md`](loader_design.md) (Abweichungen 8.5 K1–K6, 8.6 G1–G10, 8.7 I1–I8, 8.8 R1–R23) · Schnittstellen: [`src/INTERFACES.md`](../src/INTERFACES.md) 4.21–4.23 · Renderer-Vertrag: [`src/host/gfx/GFX_CONTRACT.md`](../src/host/gfx/GFX_CONTRACT.md) Abschnitt 12 · Launcher: [`launcher/README.md`](../launcher/README.md) · Vorgänger: [`docs/m3b_60hz_report.md`](m3b_60hz_report.md)

## 1. Ergebnis in Kürze

**Spieler starten den Port jetzt über einen eigenen Launcher und bekommen ein kantenglattes Bild in voller Bildschirmhöhe.**

- **Launcher `FuturamaLauncher.exe`** (Qt 6.8.3 Widgets, 64 Bit, Deutsch/Englisch): Einrichtungsassistent mit ISO-Prüfung, Reiter Spiel/Grafik/Pfade/Erweitert, die vollständig aus dem Schema von `futurama.exe` entstehen, Voreinstellungen Klassisch/Standard/Maximum, Spielstart mit Ergebnisfeld (Rückgabecode, Ursache, Protokollende) und eine Automatik für Skripte. Eigenes, abstraktes Symbol; keine Grafiken, Logos oder Schriften des Spiels.
- **Ein Vertrag statt doppelter Pflege:** `futurama.exe --print-config-schema`, `--check-iso`, `--query-gpu`. Jeder Schlüssel, den der Host liest, steht in einer Schema-Tabelle; die Prüfung beim Laden nutzt dieselbe Tabelle.
- **Bildqualität:** Renderauflösung bis zur vollen Bildschirmhöhe (4:3 mit Seitenbalken; auf dem 4K-Fernseher 2880×2160), MSAA 2×/4×/8× mit Alpha-to-Coverage, Supersampling (Render-Skala 1,0–2,0), anisotrope Filterung und – neu nach dem Review – helligkeitsrichtige Kantenglättung (`linear_aa`). **Kein FXAA, kein TAA.** „Klassisch“ (640×480 ohne Kantenglättung) bleibt bitgleich zu M3: Legal-Bild und Tech-Notice texelgenau wie xemu (0,00), Ladebild 0,29–0,30, Frontend 0,01.
- **Takt:** 60 Hz Logik und bildsynchroner Takt aus M3b bleiben erhalten (60,01 Hz, 99,8–99,9 % der Bilder genau 2 Refreshes bei 120 Hz). Die Bildrate bis 120 Hz folgt mit M3d; der Launcher zeigt sie bis dahin nur an.

Das Review (Phase 3) hat **23 Befunde** erhoben. Alle sind nachgeprüft und bestätigt; 21 sind behoben, einer ist mit Laufzeitregel und Dokumentation abgesichert (A2C und Ziel-Alpha), einer ist bewusst auf M3d verschoben (Latenz). Die wichtigsten Änderungen: schärferes Verkleinern bei gebrochenen Skalen (Catmull-Rom mit Anti-Ringing), „Maximum“ jetzt Skala 2 + 8× statt 1,5 + 8×, Abstufen statt unsichtbarem Null-Renderer, wenn die Puffer nicht passen, ein Spiel je Benutzerdaten-Ordner (Exitcode 6), Lizenztexte für Qt im Paket und viele Korrekturen im Launcher (Abschnitt 8).

## 2. Was gebaut wurde

| Teil | Inhalt | Dateien |
|---|---|---|
| Schema und Werkzeug-Modi (Phase 1, Host-CLI) | Schema-Tabelle aller Schlüssel (Typ, Standard, Auswahl mit de/en-Texten, Bereich, Reiter, Bedienelement, Flags „erweitert“/„Entwickler“/„dynamischer Standard“), Prüfung beim Laden, Werkzeug-Modi vor Stufe 1 ohne Konfiguration, Log und XBE; Ablageort `%LOCALAPPDATA%\FuturamaPC\futurama.ini` bzw. portabler Modus | `core\config_schema.*`, `core\cli_tools.*`, `core\config.*` |
| Renderauflösung und Kantenglättung (Phase 1, Renderer) | Der Gast sieht weiter 640×480; nur D3D-Viewport und Clear-Rechtecke wachsen auf die interne Größe. Multisampled persistente Backbuffer (Xbox-Flip), Resolve, Verkleinern (Box bzw. Catmull-Rom), Alpha-to-Coverage mit Alpha-Schärfung, „anti-aliased nearest“ für punktgefilterte 2D-Texturen, anisotrope Filterung, Screenshots in Ausgabe- oder interner Größe, GPU-Zeitmessung | `gfx\nipc_gpu.*`, `gfx\ff_pipeline.*`, `gfx\rhi\d3d11\*`, `gfx\shaders\*`, `platform\screenshot.*` |
| Launcher (Phase 1) | Eigenes CMake-Projekt, INI-Rundlauf mit den Leseregeln des Hosts, Editoren je Bedienelement, Assistent, Ergebnisfeld, Automatik, Testdouble des Spiels, Paket-Skript | `launcher\**`, `build_launcher.cmd`, `tools\package.py` |
| Integration (Phase 2) | Monitorwahl `[render] monitor`, ISO-Kasten im Spiel-Reiter, korrekte „Automatisch“-Texte und 4:3-Liste, Fenstergröße in Pixeln, Messungen mit eingeschalteter Anzeige | `platform\window.*`, `launcher\src\ui\*` |
| Review-Fixes (Phase 4, dieser Bericht) | Abschnitt 8 | siehe `src\INTERFACES.md` Changelog |

## 3. Für Spieler: den Launcher benutzen

Voraussetzungen: Windows 10 oder 11 (64 Bit), Grafikkarte mit Direct3D 11, ein Abbild (ISO) der eigenen europäischen Futurama-Disc für die Xbox. Das Paket enthält keine Spieldaten.

1. **Paket entpacken** und `FuturamaLauncher.exe` starten (`dist\FuturamaPC\`, Abschnitt 4).
2. **Erster Start – Assistent:**
   - *Willkommen:* Oberflächensprache (Deutsch/Englisch) und wo die Einstellungen gespeichert werden.
   - *Spielabbild:* ISO wählen oder den Pfad einfügen (auch „Als Pfad kopieren“ mit Anführungszeichen). Der Launcher prüft das Abbild sofort: „✓ Unterstützte Version erkannt“ für die europäische Verkaufsversion. Der NTSC-Prototyp oder eine US-Fassung werden erkannt, aber das Spiel startet damit nicht – der Assistent sagt das deutlich.
   - *Sprache des Spiels:* Deutsch, Englisch, Französisch, Spanisch, Italienisch.
   - *Anzeige:* Bildschirm, Fenster oder randloses Vollbild, Fenstergröße (mit der Pixelgröße bei der Windows-Skalierung). Im Spiel schaltet Alt+Eingabetaste um.
   - *Bildqualität:* eine Voreinstellung (unten). Die Zusammenfassung zeigt alles noch einmal; „Fertigstellen“ speichert.
3. **Voreinstellungen** (an Grafikkarte und Grafikspeicher angepasst, später im Reiter Grafik änderbar):

   | Voreinstellung | Werte | Für |
   |---|---|---|
   | Klassisch | 640×480, ohne Kantenglättung, ohne anisotrope Filterung | das Bild wie auf der Xbox (bitgleich zu M3) |
   | Standard | Fenster- bzw. Bildschirmgröße (bis 2880×2160 auf 4K), MSAA 4×, Alpha-to-Coverage, anisotrop 16× | die Empfehlung: glatt, scharf, sehr günstig |
   | Maximum | wie Standard, dazu Supersampling 2× (exakt 2×2 Pixel) und MSAA 8× = 32 Abtastungen je Pixel, helligkeitsrichtige Kantenglättung | die bestmögliche Glättung; braucht etwa 2,3 GB Grafikspeicher bei 4K. Mit weniger Speicher nimmt der Launcher automatisch 2× mit MSAA 4× oder natives MSAA 8× |

4. **Reiter:**
   - *Spiel:* Kasten „Spielabbild“ mit dem Prüfergebnis und „Ändern…“, Spielsprache, Logik-Takt (60 Hz Standard, 50 Hz = PAL-Original).
   - *Grafik:* Voreinstellung, Grafikkarte und VRR-Hinweis, Bildrate (bis M3d nur Anzeige), Bildschirm, Vollbild, Fenstergröße, Renderauflösung, Supersampling (Zwischenwerte wie 1,5 zeichnen etwas weicher als 1,0 oder 2,0 – der Launcher sagt das), MSAA, Alpha-to-Coverage und helligkeitsrichtige Kantenglättung (beide ohne MSAA ausgegraut), anisotrope Filterung, Bildskalierung, VSync.
   - *Pfade:* Spielabbild, Benutzerdaten- und Screenshot-Ordner, darunter Konfigurationsdatei, Protokolle, Spielstände und Spielordner mit „Öffnen“. „Auf Standard zurücksetzen“ behält das Spielabbild.
   - *Erweitert:* Protokollstufe, Fehlersuche, Bildtakt, Screenshot-Größe, Hinweise zur INI-Datei (unbekannte Einträge mit „Entfernen“), Programmversion. „Entwickleroptionen anzeigen“ blendet Schlüssel für Tests ein.
5. **Spielen:** Der Launcher prüft die Eingaben, speichert, startet `futurama.exe --config=<ini>` und verschwindet. Danach zeigt er das Ergebnis: Titel mit Rückgabecode (z. B. „Startphase fehlgeschlagen – Rückgabecode 2“), die Ursache in einer Zeile, die letzten 30 Protokollzeilen und „Protokoll öffnen“, „Ordner öffnen“, „Erneut versuchen“ bzw. „Erneut spielen“.
6. **Ablageorte:** `%LOCALAPPDATA%\FuturamaPC\futurama.ini`, Protokolle unter `…\logs\`, Screenshots (F12) unter `…\screenshots\`, Spielstände unter `…\hdd\`. Mit einer leeren Datei `portable.txt` neben `futurama.exe` liegt alles im Spielordner. Es kann nur ein Launcher je Konfiguration und nur ein Spiel je Benutzerdaten-Ordner laufen.

## 4. Für Entwickler: bauen, testen, verpacken

Alle Befehle aus `D:\FuturamaDecomp`; Build-Skripte immer mit vollem Pfad.

```powershell
# Spiel (x86): drei Builds ohne Warnungen
$env:FUT_CMAKE_ARGS = '-DFUT_WERROR=ON'
cmd /c "D:\FuturamaDecomp\build.cmd" build\m3c-final Debug
cmd /c "D:\FuturamaDecomp\build.cmd" build\m3c-final-rel RelWithDebInfo
cmd /c "D:\FuturamaDecomp\build.cmd" build\m3c-final-clang Debug all clang
build\m3c-final\bin\fut_tests.exe                      # 259 Testfälle

# Launcher (x64, Qt 6.8.3 unter D:\Qt\6.8.3\msvc2022_64), mit ctest
$env:LAUNCHER_CMAKE_ARGS = '-DLAUNCHER_WERROR=ON'
cmd /c "D:\FuturamaDecomp\build_launcher.cmd" build\m3c-final-launcher Debug test
cmd /c "D:\FuturamaDecomp\build_launcher.cmd" build\m3c-final-launcher-rel Release test

# Paket dist\FuturamaPC\ (RelWithDebInfo-Spiel, Release-Launcher)
python tools\package.py --game-build build\m3c-final-rel --launcher-build build\m3c-final-launcher-rel [--portable] [--zip]

# Prüfungen (klassische Werte in den INIs für den xemu-Vergleich)
python tools\m1_check.py --exe build\m3c-final\bin\futurama.exe --config run\m3c_final\m1\futurama.ini --runs 3 --xbe orig\retail_eu\default.xbe --fresh
python tools\m2_check.py --exe build\m3c-final\bin\futurama.exe --config run\m3c_final\m2\futurama.ini --runs 1 --fresh
python tools\m3_check.py --exe build\m3c-final\bin\futurama.exe --config run\m3c_final\m3-debug\futurama.ini --runs 1 --fresh
python tools\m3c_check.py --exe build\m3c-final\bin\futurama.exe --config run\m3c_final\m3c-debug\futurama.ini --fresh
```

Der Ablauf aller Prüfungen steht in `run\m3c_final\run_checks.sh`. Für das Kriterium M3b-7 von `m3_check` muss das Spielfenster im Vordergrund sein: Anzeige an und während des Laufs nicht am Rechner arbeiten (sonst verweigert Windows dem Prozess den Vordergrund).

Ctest des Launchers: `launcher_tests` (INI, Schema, Validierung, Pfade, CLI, Sitzungen, Voreinstellungen, Übersetzungen), `launcher_ui_tests` (Hauptfenster, Assistent, Ergebnisfeld, Rückfrage bei nicht unterstütztem Abbild, Pfade mit Anführungszeichen; Offscreen), `launcher_screenshots` (alle Reiter und Seiten als PNG), `launcher_cli_positional`.

Das Paket enthält 24 Dateien (29,7 MB): `FuturamaLauncher.exe`, `futurama.exe`, `futurama_host.dll`, Qt6Core/Gui/Widgets, die Plugins `platforms\` und `styles\`, die MSVC-Laufzeit, `LIESMICH.txt` und `LICENSES\Qt\` (LGPL-3.0, GPL-3.0, Hinweis zur Bezugsquelle des Qt-Quelltexts). Spieldaten, Debug-Builds und das Testdouble lehnt das Skript ab.

## 5. Schema- und CLI-Vertrag

| Aufruf | Ergebnis | Rückgabecode |
|---|---|---|
| `futurama.exe --print-config-schema=<datei.json>` | `{"schema_version":1,"build":"release, built 2026-10-06 12:06:58, msvc 19.44.35228","sections":[{"name":"render","label":{"de","en"},"keys":[{"name","type","default","choices","min","max","step","label","help","advanced","restart_required","ui":{"tab","order","widget"}, …}]}]}` – 57 Schlüssel in 10 Abschnitten, optional `developer`, `dynamic_default`, `hidden_choices`, `range_enforced`, `placeholder`, `ui.filter` | 0 |
| `futurama.exe --check-iso=<abbild> --result=<datei.json>` | `{"ok","reason","xdvdfs","layout":"xiso\|redump\|unknown","xbe_sha1","version":"retail_eu\|proto_ntsc\|unknown","supported","title","size"}` + `message` (de/en), `reason_code`, `title_id` | 0 wenn unterstützt, sonst 2 |
| `futurama.exe --query-gpu=<datei.json>` | `{"adapters":[{"name","vendor_id","dedicated_mb"}],"outputs":[{"name","device","primary","rect","refresh":{"num","den"},"modes","dpi"}],"msaa":[1,2,4,8],"max_anisotropy":16,"tearing","vrr_hint"}` | 0 |
| `futurama.exe --config=<ini>` | Spielstart | 0 ok, 1 Konfiguration, 2 Startstufe, 3 schwerer Fehler, 4 Spiel beendet, 5 Zeitlimit, **6 Spiel läuft bereits mit denselben Benutzerdaten**, 10–14 Starter |

Die neuen `[render]`-Schlüssel: `resolution = auto | <B>x<H>`, `render_scale = 1.0..2.0` (Schritt 0,25), `msaa = off|2|4|8`, `alpha_to_coverage` (an), `anisotropy = off|2|4|8|16` (16), `screenshot_size = output|internal`, `monitor = primary | \\.\DISPLAYn` und nach dem Review `linear_aa` (aus). Standard ohne INI: `auto`, 1,0, MSAA 4×, A2C an, anisotrop 16×. „Klassisch“ = `resolution = 640x480`, `render_scale = 1.0`, `msaa = off`, `anisotropy = off`.

Die Automatik des Launchers: `FuturamaLauncher.exe --config=<ini> --set section.key=value … --save --launch --wait --result=<json>`; eigene Rückgabecodes 64 Aufruf (auch Positionsargumente), 65 ungültiger Wert, 66 Spiel fehlt, 67 Schema, 68 Speichern (auch INI ohne gültiges UTF-8), 69 Screenshots, 70 ein anderer Launcher nutzt dieselbe INI.

## 6. Auflösung und Kantenglättung

### 6.1 Aufbau

```
Gast (640×480): Viewport, Kamera, Pretransform, 1/16-Pixel-Raster bleiben in Gast-Pixeln
  → D3D-Viewport und Clear-Rechtecke × (intern / 640)
  → 2 persistente Backbuffer, je MSAA-Samples, Tiefenpuffer gleich (Xbox-Flip)
  → Present: Resolve (Gamma oder mit linear_aa linear) → Verkleinern auf die Ausgabe
             (Skala 2: exakter Box; 1,25/1,5/1,75: Catmull-Rom mit Anti-Ringing)
  → Present-Pass: 1:1, [render] scaling beim Vergrößern, Catmull-Rom wenn das Bild größer als das Fenster ist
```

- **MSAA** glättet Geometriekanten; **Alpha-to-Coverage** ersetzt bei Alpha-Test ohne Blending das `discard` durch eine geschärfte Abdeckung (Kante an derselben Stelle wie der D3D8-Test, ein Pixel breit).
- **Anti-aliased nearest:** Punktgefilterte 2D-Texturen (alle Intro-Bilder) bleiben bei Faktor 4,5 gleich breit statt abwechselnd 4 und 5 Pixel; Schrift bleibt scharf.
- **Supersampling** ist exakt bei Skala 2 (je 2×2 Pixel). Gebrochene Skalen verkleinert seit dem Review ein Catmull-Rom-Filter, dessen Ergebnis auf die 2×2 nächsten Quellpixel begrenzt wird (kein Überschwingen).
- **Helligkeitsrichtige Kantenglättung (`linear_aa`):** Resolve und Verkleinern mitteln lineares Licht; dünne helle Linien (Blitze, Umrisse) wirken gleichmäßiger. Das Rendern und Blenden des Spiels bleibt unverändert. Standard aus (wie die Xbox), in „Maximum“ an.
- **Abstufen:** Passen die Puffer nicht (Grafikspeicher, D3D11-Grenze je Ressource), nimmt der Renderer Skala 1, dann weniger Samples, zuletzt 640×480 und schreibt das ins Protokoll; der Launcher plant die Voreinstellungen schon nach dem Grafikspeicher.

### 6.2 Leistung je Voreinstellung

Paket-Läufe über den Launcher (RelWithDebInfo, RTX 5090, LG-Fernseher 3840×2160 bei 120 Hz, Vollbild bzw. Fenster bei 225 %, 1300 Bilder, Fenster im Vordergrund; `run\m3c_final\dist\*\summary.txt`):

| Variante | Intern | Backbuffer | GPU-Spanne Ø / p99 | Frame-Arbeit Ø / p99 | Bildrate |
|---|---|---|---|---|---|
| Klassisch (Fenster) | 640×480, 1× | 3 MiB | 0,28 / 1,1 ms | 0,33 / 1,3 ms | 60,04 Hz |
| Standard (Vollbild) | 2880×2160, 4× | 308 MiB | 1,05 / 3,4 ms | 0,35 / 1,5 ms | 60,05 Hz |
| Maximum (Vollbild) | 5760×4320, 8×, linear | 2396 MiB | 1,36 / 5,7 ms | 0,32 / 1,3 ms | 59,97 Hz |
| Standard im Fenster | 2631×1973, 4× | 257 MiB | 0,91 / 3,3 ms | 0,34 / 1,4 ms | 60,04 Hz |

Die GPU-Spanne misst vom ersten Clear/Draw bis zum Ende des Present-Passes (GPU-Arbeit und Lücken, solange die CPU noch übermittelt). Alle Varianten haben viel Reserve; ein Bild darf 16,7 ms dauern. Vergleich der Filter-Varianten (gleicher Rechner, Vollbild, Integrations-Build): natives MSAA 8× 0,45 ms, 1,25/4× 0,76 ms, 1,5/8× 1,12 ms, 1,75/4× 1,31 ms, 2,0/8× 1,30–1,38 ms.

### 6.3 Kantenmaß und Filtervergleich

`m3c_check` X2 (Frontend, statische Kantenpixel, Abweichung zur Referenz Skala 2 + 8×): ohne Kantenglättung 3,06–3,09, mit MSAA 4× 1,36–1,41 (Verhältnis 0,44–0,46, Grenze 0,7); harte Stufen 1,5–1,6 % → 0,0 %.

Filter für gebrochene Skalen (Befund 1), offline an internen Aufnahmen gegen die Referenz Skala 2 + 8× (Box auf 2880×2160); Kantenfehler = mittlere Abweichung auf Kantenpixeln der Referenz (im Frontend nur auf statischen Pixeln), Schärfe = mittlerer Gradient dort (Referenz 56,9), Überschwingen = Werte außerhalb des 3×3-Bereichs der Referenz (`run\m3c_final\kernel\kernels*.txt`):

| Fall | Mitchell-Netravali (vorher) | Catmull-Rom | Flächenmittel | **Catmull-Rom + 2×2-Begrenzung (neu)** | natives MSAA 8× |
|---|---|---|---|---|---|
| Legal, Skala 1,5: Kantenfehler / Schärfe / Überschwingen | 8,76 / 52,9 / 0,09 | 6,94 / 54,9 / 0,59 | 5,32 / 54,3 / 0 | **5,03 / 54,7 / 0** | 0,07 / 56,9 / 0 |
| Frontend, Skala 1,5: Kantenfehler | 3,22 | 2,27 | 3,08 | **2,05** | 1,89 |
| Legal, Bild größer als Fenster (2880 → 2631) | 7,44 | 7,01 | 6,39 | **6,03** | – |
| Frontend, 2880 → 2631 (alle Pixel) | 2,91 | 2,17 | 3,07 | **2,07** | – |

Die Begrenzung auf die 2×2 nächsten Pixel ist der richtige Rahmen: Eine Begrenzung auf die Fläche des Zielpixels wäre bei Verhältnissen nahe 1 fast ein Nächster-Nachbar-Filter (Kantenfehler 12,6). Natives MSAA 8× ist auf den 2D-Szenen gleich der Referenz und im Frontend nur knapp schlechter – deshalb ist „Maximum“ jetzt Skala 2 (exakt) und, wenn der Speicher dafür nicht reicht, natives 8×, nicht mehr 1,5.

### 6.4 xemu-Regression und Bildvergleich

- **Klassisch = M3:** `m3_check` (Debug, RelWithDebInfo mit Debug-Layer, clang-cl): Legal 0,00, Tech-Notice 0,00, Ladebild 0,29–0,30, Frontend 0,01 auf den 61 % statischen Pixeln; 0 Debug-Layer-Meldungen.
- **Hohe Auflösung verkleinert gegen xemu** (`m3c_check` X1, Standard-Werte): Legal 0,73, Tech-Notice 0,18, Ladebild 0,60, Frontend 0,98 (Grenzen 1,0 / 1,0 / 2,0 / 2,0).
- **Standard und Maximum gegen Klassisch** (Spiel-Screenshots der Paket-Läufe, Flächenmittel auf 640×480; `run\m3c_final\dist\compare\metrics.txt`): Legal 0,70, Tech-Notice 0,18, Ladebild 0,56, Frontend 2,42 (animierte Szene). 2D-Elemente stehen an derselben Stelle. Standard und Maximum unterscheiden sich nur an Kantenpixeln (2 % der Pixel auf dem Legal-Bild, 11 % im Frontend).
- **Desktop-Aufnahme im Vollbild** (3840×2160): Das Bild liegt bei x = 480…3359, die Seitenbalken sind schwarz.

Gesehen in den Ausschnitten `run\m3c_final\dist\compare\*.png`: Logo-, Zahnrad- und Menükanten sind in Standard und Maximum glatt (Klassisch zeigt die Treppen des Originals), Menü- und Legal-Schrift bleibt scharf, das TM-Zeichen ist sauber. Maximum glättet die feinen Linien (Kabel, Blitze) noch etwas ruhiger als Standard.

## 7. Screenshots

Alle Bilder liegen unter `run\` (nicht im Repository):

| Was | Pfad |
|---|---|
| Launcher, Windows-Plattform 225 %, dunkel | `run\m3c_final\shots\win\de_main_graphics.png`, `de_main_game.png`, `de_main_paths.png`, `de_tab_advanced_full.png`, `de_status_fatal.png`, `de_status_ok.png`, `de_main_small_status.png` (1280×690 logisch), `de_wizard_2_iso.png`, `de_wizard_5_quality.png`, `en_wizard_5_quality.png`, `en_main_graphics.png` |
| Launcher, hell bzw. offscreen | `run\m3c_final\shots\win_light\`, `off_dark\de_tab_advanced_full.png`, `off_light\de_status_fatal.png` |
| Launcher mit dem NTSC-Prototyp | `run\m3c_final\shots\proto\de_wizard_2_iso.png`, `de_main_game.png` |
| Spiel, eigene Screenshots | `run\m3c_final\dist\{classic,standard,maximum,window}\userdata\screenshots\frame_000180.png` (Legal), `…480` (Tech-Notice), `…816` (Ladebild), `…1200` (Frontend) |
| Spiel, Desktop | `run\m3c_final\dist\maximum\screen_20.6s.png`, `standard\screen_20.6s.png`, `window\screen_20.6s.png` |
| Vergleich Klassisch / Standard / Maximum | `run\m3c_final\dist\compare\frontend_logo_tm.png`, `frontend_gears.png`, `frontend_menu_text.png`, `legal_text.png`, `tech_notice.png`, `loading.png` |

## 8. Review-Befunde

Alle 23 Befunde sind nachgeprüft. Die Spalte „Entscheidung“: **behoben**, **abgesichert** (Dokumentation + Laufzeitregel) oder **verschoben**. Keiner ist abgelehnt; die Belege des Reviews ließen sich nachvollziehen. Einzelheiten: `docs\loader_design.md` 8.8.

| # | Schwere | Befund | Entscheidung und Umsetzung | Beleg |
|---|---|---|---|---|
| 1 | mittel | „Maximum“ (1,5 + 8×) weicher als nativ 8×, Mitchell-Netravali zu weich | **behoben:** Catmull-Rom mit 2×2-Begrenzung; „Maximum“ = 2,0 + 8× + `linear_aa`, nach Speicher abgestuft; Hinweis bei Zwischenwerten | Abschnitt 6.3; Unit-Test (keine Über-/Unterschwinger, Kante ≤ 3 Pixel); `m3c_check` Variante `frac` |
| 2 | mittel | Kein Rückfall bei zu großen Puffern, Null-Renderer unsichtbar | **behoben:** Abstufung in Create und beim Nachführen, sonst `core::Fatal` | Fall `ladder`; Unit-Test |
| 3 | niedrig | Resolve/Verkleinern im Gamma-Raum | **behoben als Option** `[render] linear_aa` (Standard aus = originaltreu, „Maximum“ an) | Unit-Test 128 → 188; Varianten `max`/`frac` |
| 4 | niedrig | Anisotropie bei MAG = POINT | **behoben:** nur bei linearer Vergrößerung; Nutzen ab M4b prüfen | Unit-Test |
| 5 | niedrig | A2C überschreibt Ziel-Alpha | **abgesichert:** dokumentiert, erster DESTALPHA-Blend schaltet A2C ab, Zähler im Protokoll; Bewegungstest in M4/M8 | Frontend 0 solche Draws |
| 6 | niedrig | Latenz Present→sichtbar schwankt um 1–2 Refreshes | **verschoben nach M3d** (Present-Phase, Swapchain über den ganzen Monitor) | Median 12,9–14,0 ms (`m3_check`), 16,6 ms (Paket) |
| 7 | hoch | „Ursache“ zeigte den Protokollpfad | **behoben:** `GameRunResult::cause()` | UI-Test, Fall `proto` |
| 8 | mittel | Protokoll waagerecht ans Zeilenende gescrollt | **behoben** | UI-Test |
| 9 | mittel | Nicht unterstütztes Abbild ohne Rückfrage gestartet, falscher Hinweis, doppelter Satz | **behoben** | UI-Test, `shots\proto\` |
| 10 | mittel | Pfade in Anführungszeichen | **behoben** (`CleanPathInput`, Fehler bei Anführungszeichen in der INI) | Fall `quoted`, UI-Test |
| 11 | mittel | Positionsargumente ignoriert | **behoben:** Rückgabecode 64 | ctest, Fall `positional` |
| 12 | mittel | Fenster höher als der Arbeitsbereich | **behoben:** höchstens 90 % der freien Fläche | 3484×1925 im Arbeitsbereich 3840×2052 (vorher 2192 hoch); `de_main_small_status.png` |
| 13 | mittel | Englische Core-Texte in der deutschen Oberfläche | **behoben:** Problemkennungen, übersetzte Speicherfehler mit Hinweis, lesbarer Build-String | Unit-Tests, Screenshots |
| 14 | mittel | Keine Qt-Lizenztexte im Paket | **behoben:** `LICENSES\Qt\` | Paketliste |
| 15 | mittel | Mehrere Instanzen | **behoben:** Mutex je Benutzerdaten (Exitcode 6), `QLockFile` je INI (70) | Fälle `game_twice`, `lock` |
| 16 | niedrig | Uneinheitliche Begriffe Schema/Launcher | **behoben** (Sie, Zeitquelle der Spiellogik, Alt+Eingabetaste, Protokoll, Spielabbild, Verkaufsversion, Fehlende D3D-Funktionen, Host-Meldung ohne Quelltext) | Screenshots |
| 17 | niedrig | Text und Typografie | **behoben** (Großschreibung, „nicht gewählt“, „Einstellungsliste und Grafikkarte neu abfragen“, „Erneut spielen“, „Rückgabecode“, „Zeitüberschreitung“, Ellipsen, englische Preset-Zeilen, „Fenster- oder Bildschirmgröße“, VRR-Satz, „…“ in LIESMICH) | Screenshots |
| 18 | niedrig | Bildrate im Reiter Erweitert falsch | **behoben:** Entwickleroption, wirksamer Wert angezeigt | – |
| 19 | niedrig | Zurücksetzen leerte das Spielabbild, doppelte Einträge | **behoben** | `de_main_paths.png` |
| 20 | niedrig | Ungültiges UTF-8 still ersetzt, Einfügen hinter kaputter Zeile | **behoben:** Automatik 68, GUI mit `.bak`, Einfügen nur hinter gültigen Zeilen | Fall `latin1`, Unit-Test |
| 21 | niedrig | Voreinstellungen ohne Grafikspeicher | **behoben** | Unit-Test `presetMemory` |
| 22 | niedrig | windeployqt-Warnung im Build-Log | **behoben:** gefiltert, Rückgabecode zählt | Build-Log |
| 23 | niedrig | Rückfall-Stile schwach | **behoben:** Palette, Stilwahl nach Windows-Version und Plattform | `off_dark`, `off_light` |

## 9. Prüfergebnisse

| Prüfung | Ergebnis | Ausgabe |
|---|---|---|
| Spiel bauen (MSVC Debug, RelWithDebInfo, clang-cl Debug, `FUT_WERROR=ON`) | ohne Warnungen | `run\m3c_final\build\game_*.txt` |
| `fut_tests` | 259/259 in allen drei Builds (1 übersprungener Handtest), 30 303 Assertions | `run\m3c_final\build\fut_tests_*.txt` |
| Launcher bauen (Debug, Release, `LAUNCHER_WERROR=ON`) und ctest | ohne Warnungen, 4/4 | `run\m3c_final\build\launcher_*.txt` |
| `m1_check` | Debug 3 + RelWithDebInfo 2 Läufe: ALL PASS | `run\m3c_final\final\checks_m1_*.txt` |
| `m2_check` | d3d11 und Null-Renderer: ALL PASS (99,60 % der Frames genau ein Schritt) | `checks_m2_*.txt` |
| `m3_check` (klassisch) | erster Durchgang alles außer M3b-7 (Fenster im Hintergrund); Wiederholung Debug/RelWithDebInfo/clang-cl: **ALL PASS**, Vordergrund 99,9–100 %, 99,80–99,90 % der vblank-Bilder im Plan, 60,01 Hz, Frame-Arbeit p99 3,3–4,4 ms | `checks_m3_*_retry.txt` |
| `m3c_check` (7 Varianten inkl. `max`, `frac`) | Debug und RelWithDebInfo: **ALL PASS**, Debug-Layer ohne Meldung, 60,01–60,04 Hz, Frame-Arbeit p99 ≤ 7,2 ms (Debug) bzw. ≤ 4,7 ms (RelWithDebInfo) | `checks_m3c_*.txt` |
| Paket über den Launcher | 4/4 Varianten Exitcode 0 | `run\m3c_final\dist\run.txt` |
| Endfälle der Review-Fixes | 7/7 | `run\m3c_final\cases\report.txt` |
| Fenstergröße auf kleiner Fläche | im Arbeitsbereich | `run\m3c_final\cases\winsize\result.txt` |

## 10. Bekannte Probleme und offene Punkte

- **M3b-7 hängt von der Umgebung ab:** Arbeitet jemand am Rechner, verweigert Windows dem Spielfenster den Vordergrund; `m3_check` meldet dann „environment invalid“. Über den Launcher gestartet war das Fenster in allen Läufen vorn.
- **Latenz** Present→sichtbar je Sitzung 1–2 Refreshes (Befund 6) – M3d.
- **Vollbild** bleibt eine 4:3-Swapchain mit schwarzem Fensterrahmen als Seitenbalken; für Independent Flip mit VRR muss sie den Monitor bedecken (M3d).
- **Render-to-Texture mit MSAA, Linien- und Punktbreite** folgen mit P2 (8.6 G9).
- **Zweiter Monitor:** `[render] monitor` ist nur mit dem einen Fernseher des Messrechners geprüft; andere Skalierung und WM_DPICHANGED ungeprüft.
- **Anisotropie und A2C unter Bewegung** sind in den erreichbaren, statischen Szenen nicht bewertbar (Freecam M4b, Level in M8).
- **Launcher von Hand:** Native Dateidialoge und das Wechseln des Farbschemas zur Laufzeit sind nur automatisiert (Offscreen, unsichtbare Fenster, Automatik) geprüft.
- **Abstufen zur Laufzeit** (Fenster wird größer, Puffer passen nicht mehr) ist im Code vorhanden, aber nur indirekt geprüft; der Start-Fall ist mit 16384×12288 belegt.
- Alte Prüfkonfigurationen `run\m3b\*` und `run\m3final\*` setzen die klassischen Werte nicht und rendern daher in Standard-Qualität (8.7 I6).

## 11. Ausblick

- **M3d – bis 120 Bilder pro Sekunde:** Die Logik bleibt bei 60 Hz, der Renderer interpoliert Kamera und Objekte zwischen den letzten beiden Ticks (`alpha = Rest / dt`). Dazu die Present-Phase vor das Ziel-Vblank legen (Befund 6), die Swapchain über den ganzen Monitor (Independent Flip, VRR) und im Launcher die Bildrate „60“, „120 (interpoliert)“ oder „Monitor (max. 120)“ freischalten.
- **M4 – Eingabe:** XInput, Tastatur und Maus mit frei belegbaren Tasten, Rumble; dazu ein Reiter „Steuerung“ im Launcher (Belegung, Empfindlichkeit, Vibration).
- **M4b – Freecam:** auch zur Prüfung von Anisotropie und Alpha-to-Coverage in Bewegung.
