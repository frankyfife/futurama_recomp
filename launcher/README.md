# FuturamaLauncher (M3c)

`FuturamaLauncher.exe` ist der normale Einstieg für Spieler: Einstellungen bearbeiten, Spielabbild prüfen und das Spiel starten. Qt 6.8.3 Widgets, 64 Bit, eigenes CMake-Projekt neben dem x86-Loader. `futurama.exe` bleibt der technische Starter bei 0x10000.

## Bauen und testen

```
cmd /c "D:\FuturamaDecomp\build_launcher.cmd" build\launcher Debug test
cmd /c "D:\FuturamaDecomp\build_launcher.cmd" build\launcher-rel Release test
```

- Argumente: `[builddir] [Debug|Release] [test]`. Das Skript ruft `vcvars64`, setzt `CMAKE_PREFIX_PATH` auf Qt (`FUT_QT_DIR`, Standard `D:\Qt\6.8.3\msvc2022_64`), baut mit Ninja und ruft `windeployqt` in `<builddir>\bin` auf. Release-Builds bekommen zusätzlich die MSVC-Laufzeit-DLLs app-lokal.
- Weitere Cache-Optionen über `LAUNCHER_CMAKE_ARGS`, z. B. `-DLAUNCHER_WERROR=ON` (Warnungen als Fehler, `/W4`).
- `test` startet `ctest`:
  - `launcher_tests`: INI-Rundlauf, Schema, Validierung, Pfade, CLI-Aufrufe, Sitzungen, Übersetzungen (Qt Test).
  - `launcher_ui_tests`: Hauptfenster (Bearbeiten, Voreinstellung, Speichern, Sprachwechsel, Spielstart mit Ergebnis) und Assistent, mit der Offscreen-Plattform.
  - `launcher_screenshots`: alle Reiter und Assistentenseiten als PNG in `<builddir>\screenshots`, dazu das Hauptfenster mit Ergebnisfeld bei 1280 × 690 logischen Pixeln (`*_main_small_status.png`, freie Fläche eines Notebooks mit 1920 × 1080 bei 150 %).
  - `launcher_cli_positional`: überzählige Angaben auf der Befehlszeile (ein Pfad mit Leerzeichen ohne Anführungszeichen) sind ein Aufruffehler.
- `windeployqt` 6.8.3 meldet auch mit `--no-translations` „Warning: Translations will not be available … catalogs.json“. Die Qt-Installation hat keine Übersetzungen und der Launcher bringt eigene Tabellen mit; `build_launcher.cmd` filtert genau diese Zeilen aus der Ausgabe (vollständig in `<builddir>\windeployqt.log`), der Rückgabecode von `windeployqt` entscheidet.
- Qt Test schreibt auf diesem Rechner nichts nach stdout (auch nicht bei einem minimalen `QTEST_GUILESS_MAIN`). Die Testprogramme schreiben deshalb pro Klasse nach `bin\test_results\<Klasse>.txt` und geben die Datei danach aus.
- `fake_futurama` (`<builddir>\fakegame\futurama.exe`) ist ein Testdouble des Spiels. Es liefert die Musterdaten aus `testdata\`, prüft ISOs nur oberflächlich (XDVDFS-Kennung) und spielt einen Spiellauf mit Log, Exitcode und stderr-Zeile vor (Umgebungsvariablen `FAKE_FUTURAMA_*`, siehe Dateikopf). Es wird nie verpackt.

## Aufbau

| Teil | Inhalt |
|---|---|
| `src\core` (nur QtCore) | `IniDocument` (Rundlauf mit den Leseregeln des Hosts), `ConfigSchema` (Vertrag Punkt 1), `validation` (Severity wie der Host: was er mit Exitcode 1 ablehnt, ist ein Fehler; Bereiche mit `range_enforced: false` und Schrittweiten sind Warnungen), `SettingsModel` (Schema + INI + offene Änderungen), `GameCli`/`IsoCheckJob` (Vertrag Punkte 1–3), `CliCache`, `game_paths` (Vertrag Punkt 4), `GameSession` (Start, Exitcode, Log-Ende), `presets`, `l10n` (Tabellen de/en) |
| `src\ui` | Hauptfenster, Reiter aus dem Schema, Editoren je `ui.widget`, Ergebnisfeld, Assistent, Stil (Dark/Light), Automatisierung |
| `resources` | eigenes, abstraktes Symbol (`tools\gen_icon.py`, prüfbar mit `--check`), Manifest (`longPathAware`), Versionsinfo |
| `testdata` | handgeschriebenes Musterschema und GPU-Beschreibung |

Es gibt keine Spielgrafiken, Logos, Schriften oder Screenshots des Spiels im Launcher. Titel und Symbol sind neutral.

## Ablauf

1. **Spiel finden:** `--game-dir`, dann der Ordner des Launchers, dann `game/dir` aus `launcher.ini`. Wird nichts gefunden, fragt der Launcher nach dem Ordner.
2. **Konfiguration (Vertrag Punkt 4):** `--config=<ini>` gilt immer. Sonst `%LOCALAPPDATA%\FuturamaPC\futurama.ini`, im portablen Modus (`portable.txt` neben `futurama.exe`) der Spielordner. Der Launcher übergibt dem Spiel immer `--config=<ini>`. Seine eigenen Dateien liegen neben der INI: `launcher.ini` (Oberflächensprache, Spielordner, Fenstergröße, Entwickleroptionen) und `launcher_cache\`.
3. **Schema und Grafikkarte:** `futurama.exe --print-config-schema` und `--query-gpu`. Die Ergebnisse werden pro Build-String zwischengespeichert. Schlüssel ist ein Fingerabdruck aus Pfad, Größe und Zeitstempel von `futurama.exe` und `futurama_host.dll`; für die GPU zusätzlich eine Signatur der angeschlossenen Monitore. Schlägt `--query-gpu` fehl, kommen die Monitore aus `QScreen`.
4. **Erster Start** (keine INI oder kein ISO): Assistent mit Oberflächensprache, ISO (`--check-iso`, asynchron; ein Pfad aus „Als Pfad kopieren“ mit Anführungszeichen wird bereinigt; eine erkannte, aber nicht unterstützte Version lässt „Weiter“ zu, sagt aber deutlich, dass das Spiel damit nicht startet), Sprache des Spiels, Anzeige, Qualität (Klassisch / Standard / Maximum, an Grafikkarte und Grafikspeicher angepasst) und Zusammenfassung.
5. **Reiter:**
   - **Spiel:** Kasten „Spielabbild“ mit dem Ergebnis von `--check-iso` für das eingestellte ISO (läuft bei jeder Änderung im Hintergrund, „Ändern…“ öffnet den Dateidialog), darunter Spielsprache und Logik-Takt (Gruppe „Spiellogik“).
   - **Grafik:** Voreinstellung (Klassisch/Standard/Maximum/Benutzerdefiniert), Grafikkarte und VRR-Hinweis, Bildrate (zeigt bis M3d den Logik-Takt), dann Bildschirm (`[render] monitor`, Liste aus `--query-gpu`), Vollbild, Fenstergröße mit der Pixelgröße bei der Windows-Skalierung des Monitors (225 %: 2× = 2880 × 2160, wird auf den Bildschirm verkleinert), Renderauflösung („Automatisch“ nennt, was das Spiel rendert: im Vollbild das größte 4:3-Rechteck des Monitors, im Fenster die Fenstergröße; die Liste bietet nur 4:3-Größen bis zur Bildschirmhöhe, freie Eingabe bleibt möglich), Supersampling (Zwischenwerte wie 1,5 tragen den Hinweis, dass sie etwas weicher zeichnen als 1,0 oder 2,0), MSAA, Alpha-to-Coverage (ohne MSAA ausgegraut), helligkeitsrichtige Kantenglättung (`linear_aa`, ohne MSAA und Supersampling ausgegraut), anisotrope Filterung, Bildskalierung, VSync.
   - **Pfade** und **Erweitert** wie im Schema; alle Beschriftungen eines Reiters stehen in einer Spalte. Die Ordnerübersicht im Reiter Pfade zeigt nur, was sich dort nicht einstellen lässt (Konfigurationsdatei, Protokolle, Spielstände, Spielordner); „Auf Standard zurücksetzen“ behält das gewählte Spielabbild. Zahlen und Größen folgen der Oberflächensprache („1,75 GB“ / „1.75 GB“). `[render] frame_rate` ist bis M3d eine Entwickleroption; wird sie angezeigt, steht ohne eigenen Wert der wirksame Logik-Takt im Feld.
6. **Spielen:** Prüfen (ungültige Werte, unbekannte Schlüssel, fehlendes ISO, ein Abbild, das laut `--check-iso` nicht unterstützt wird oder kein Xbox-Abbild ist – dann mit Rückfrage, „Abbrechen“ ist Standard) → atomar speichern → `futurama.exe --config=<ini>` starten. Der Launcher verschwindet und zeigt beim Ende das Ergebnisfeld mit der Bedeutung des Rückgabecodes, der Ursache (die Zeile `futurama: …` des Hosts, nicht seine Zeile `log file: …`), den letzten 30 Protokollzeilen oder der stderr-Ausgabe (Konfigurationsfehler entstehen vor dem Protokoll; die Zeilen beginnen immer links) und den Knöpfen „Protokoll öffnen“, „Ordner öffnen“ und „Erneut versuchen“ bzw. nach einem normalen Ende „Erneut spielen“. stdout und stderr gehen über eine Pipe an den Launcher, deshalb zeigt der Host keine Meldungsfenster. Rückgabecode 6 heißt: Mit denselben Benutzerdaten läuft schon ein Spiel.
7. **Eine Instanz je INI:** Der Launcher sperrt `launcher_cache\<ini>.lock` (QLockFile). Ein zweiter Start mit derselben INI holt das erste Fenster nach vorn (oder meldet, dass der Launcher schon läuft, solange er während des Spiels verborgen ist); in der Automatik endet er mit Rückgabecode 70. Das Spiel selbst lässt je Benutzerdaten-Ordner nur einen Prozess zu.
8. **Fenstergröße:** Start mit 64 × 50 Schrifthöhen, höchstens 90 % der freien Bildschirmfläche; ein gespeichertes Fenster, das unter die Taskleiste reichen würde, wird verschoben.

INI-Regeln: Kommentare, Reihenfolge, Schreibweise, unbekannte Abschnitte und Schlüssel, Zeilenenden und BOM bleiben erhalten. Neue Schlüssel stehen hinter dem letzten gültigen Schlüssel ihres Abschnitts (nie hinter einer kaputten Zeile wie „[render“) und werden ausgerichtet, wenn der Abschnitt ausgerichtet ist. Ist die Datei kein gültiges UTF-8 (z. B. im ANSI-Editor gespeichert), zeigt der Launcher einen Hinweis und legt vor dem Speichern `<ini>.bak` an; die Automatik überschreibt eine solche Datei nicht (Rückgabecode 68). Hinweise zu fehlerhaften Zeilen und Speicherfehler erscheinen in der Oberflächensprache, Speicherfehler mit einem Hinweis auf Schreibrechte, den portablen Modus und `--config`. Pfade in Anführungszeichen werden beim Eingeben bereinigt; steht ein Pfad mit Anführungszeichen in der INI, ist das ein Fehler (der Host läse sie als Teil des Namens). Ein Wert wird nur geschrieben, wenn er vom Standard abweicht oder schon in der Datei steht. Bei Schlüsseln mit `dynamic_default` wird jede ausdrückliche Wahl geschrieben. „Auf Standard zurücksetzen“ entfernt den Schlüssel. Beim Speichern wird die Datei neu gelesen; Änderungen von außen an anderen Schlüsseln bleiben also erhalten. Werte, die der Host nicht unverändert zurücklesen könnte, setzt der Launcher in Anführungszeichen oder lehnt sie ab.

## Kommandozeile

| Option | Wirkung |
|---|---|
| `--config=<ini>` / `--game-dir=<dir>` | siehe oben |
| `--lang=de\|en` | Oberflächensprache (Standard: `launcher.ini`, sonst Systemsprache) |
| `--wizard` | Assistent öffnen |
| `--refresh` | Zwischenspeicher für Schema und GPU ignorieren |
| `--schema-file=<json>` / `--gpu-file=<json>` | Entwicklung: Musterdaten statt `futurama.exe` |
| `--set section.key=value` | Wert setzen, mehrfach möglich, gegen das Schema geprüft |
| `--save` | INI speichern und beenden |
| `--screenshots=<dir>` | alle Reiter (sichtbarer Teil und `*_full`), Ergebnisfeld und Assistentenseiten auf Deutsch und Englisch als PNG; mit `-platform offscreen` ohne Fenster |
| `--screenshot-scale=<f>` / `--color-scheme=dark\|light` | Skalierung (z. B. `2.25`) bzw. Farbschema für Screenshots |
| `--launch [--wait] [--result=<json>]` | Spiel ohne Fenster starten, vorher speichern. Mit `--wait` warten, Ergebnis-JSON schreiben (mit `cause`) und mit dem Rückgabecode des Spiels enden |

Eigene Rückgabecodes in den Automatikmodi: 64 Aufruf (auch überzählige Angaben: Pfade mit Leerzeichen gehören in Anführungszeichen), 65 ungültiger Wert, 66 Spiel fehlt bzw. startet nicht, 67 Schema, 68 Speichern (auch INI ohne gültiges UTF-8), 69 Screenshots, 70 ein anderer Launcher verwendet dieselbe INI.

Beispiel (Testlauf unter `run\`):

```
FuturamaLauncher.exe --game-dir=build\m3c\bin --config=run\x\futurama.ini ^
  --set "paths.iso=Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso" ^
  --set paths.userdata=run\x\userdata --set debug.frames=300 --launch --wait --result=run\x\result.json
```

## Paket

```
python tools\package.py --game-build build\x86-relwithdebinfo --launcher-build build\launcher-rel [--portable] [--zip]
```

Das Skript erstellt `dist\FuturamaPC\` mit `futurama.exe`, `futurama_host.dll`, `FuturamaLauncher.exe` und der nötigen Qt- und MSVC-Laufzeit (Allow-List) sowie `LIESMICH.txt` und `LICENSES\Qt\` (Texte der GNU LGPL 3.0 und GPL 3.0 aus `launcher\licenses\`, unverändert von der FSF, und ein Hinweis zu Qt 6.8.3 mit der Bezugsquelle des Quelltexts – LGPLv3 Abschnitt 4). Spieldaten (ISO, XBE, Archive, Medien), Dateien über 200 MB, Debug-Builds des Launchers und das Testdouble werden abgelehnt.
