# Meilenstein M7: Spielstände (Speichern am Levelende, Laden, Optionen, Extras, Signatur, Löschen) – Bericht

Stand: 2026-10-12 · Ziel-XBE: `orig\retail_eu\default.xbe` (Retail EU) · Verbindliches Design: [`docs/loader_design.md`](loader_design.md) (M7, 4.5, Abweichungen 8.18) · Analyse: [`analysis/loader/m7_saves.md`](../analysis/loader/m7_saves.md) (neu), [`files.md`](../analysis/loader/files.md) Abschnitt 4 · Schnittstellen: [`src/INTERFACES.md`](../src/INTERFACES.md) 4.33 · Nichts committet.

## 1. Ergebnis in Kürze

**Spielstände funktionieren** – mit dem Code des Spiels. Die ganze Spielstand-Logik (Menüs, Dialoge, Container, Signatur) ist XAPILIB- und Spielcode im XBE und läuft unverändert auf der Kernel-HLE aus M1/M2. Sie lief beim ersten Versuch ohne Kernel-Fehler durch; M7 hat sie erforscht, an wenigen Stellen nachgebessert und vollständig geprüft.

- **Speichern:** Das Spiel speichert nur **am Ende eines Levels** im Statistikbildschirm („Breakdown“: *Speichern / Weiter / Wiederholen / Beenden*). Es gibt keine automatische Sicherung und keine Namenseingabe: Der Name ist das nächste Level plus Gesamtspielzeit, z. B. „Kanalisation 0h 0m 2s“.
- **Neustart und Laden:** „Spiel laden“ im Hauptmenü listet die Spielstände (älteste zuerst) und lädt das gespeicherte Level; die Spielzeit läuft weiter. Optionen werden beim Start des Hauptmenüs, Extras beim ersten Bestätigen geladen.
- **Optionen und Extras:** „Optionen → Übernehmen“ speichert nach einer Änderung (`Options.dat`), die Extras (`Unlock.dat`: Freischaltungen, Bestzeiten) schreibt das Spiel mit jeder Spielstandsicherung mit.
- **Signatur:** HMAC-SHA1 mit dem Schlüssel des Ports über die ersten 0xFEC Byte, Ergebnis bei 0xFEC. `tools\verify_save.py` rechnet unabhängig in Python und stimmt mit dem Spiel überein.
- **Beschädigt:** Eine von Hand geänderte Datei meldet das Spiel als beschädigt und bietet an, sie zu löschen; `verify_save.py` meldet sie ebenfalls.
- **Löschen:** über diese Frage und beim Überschreiben – das Verzeichnis verschwindet. (Einen eigenen Menüpunkt „Löschen“ hat das Spiel nicht.)
- **25 Spielstände:** Bei 25 meldet „Neues Spiel“ die Höchstzahl, das Speichermenü bietet nur noch Überschreiben an; danach sind es wieder genau 25.
- **Zu wenig Platz:** Der Dialog „nicht genügend freie Blöcke“ hat „Steuerung“ (Xbox-Dashboard) **vorausgewählt**. Bisher beendete der Port dabei den Prozess. Jetzt meldet er es und kehrt ins Spiel zurück.
- **Prüfungen:** `tools\m7_check.py` mit fünf kurzen Spielläufen (21–53 s): **alle acht Kriterien PASS**. `fut_tests` 467/467 in RelWithDebInfo, Debug und clang-cl (0 Warnungen). `tools\m4_check.py` ALL PASS.
- **Fehler im Original gefunden:** Die Bestzeiten der Extras gehen nach jedem Neustart verloren (Abschnitt 6). Der Port lässt das wie auf der Xbox.

## 2. Was gebaut und geändert wurde

| Teil | Dateien | Inhalt |
|---|---|---|
| Beobachter-Proben | `src\host\xapi\save_probes.*` (neu), `core\guest.h` (Abschnitt M7) | 7 Proben auf XAPI und Spielcode: Anlegen, Löschen, Lesen, Signaturprüfung, Schreiben, Speichermenü, Dialoge; Protokollzeilen „save: …“, Summe „save summary: …“. Ändern nichts am Ablauf |
| Debug-Aktion | `xapi\save_probes.cpp`, `input\input_script.*` | Eingabeskript-Befehl `game level_end Level1-2`: ruft zum nächsten Tick das Levelskript-Native FsChangeLevel – das echte Levelende mit Statistikbildschirm, ohne das Level durchzuspielen |
| Verzeichnisreihenfolge | `vfs\host_dir_device.cpp` | Beschreibbare Host-Ordner liefern ihre Einträge nach Erstellzeit wie die Xbox-Festplatte (FATX); die Spielstandlisten sind so chronologisch statt nach Hash-Namen sortiert. Jeder Durchlauf ist ein Schnappschuss (stabil beim Löschen) |
| Dashboard-Aufruf | `xapi\xapi_hooks.cpp` | „Steuerung“ im Platzdialog: Meldung (Protokoll, bei `[debug] msgbox` ein Fenster mit dem Spielstand-Ordner) und Rückkehr ins Spiel statt Prozessende |
| Testschlüssel | `core\config_schema.cpp`, `vfs\vfs.h`, `vfs\vfs_init.cpp` | `[saves] free_space_limit` (Entwickler): begrenzt den gemeldeten freien Platz, um den Platzdialog ohne volle Platte zu prüfen |
| Messung R16 | `kernel\io_apc.cpp` | Protokollzeile mit der Zustellzeit der Schreib-Completion des Spielstands |
| Werkzeuge | `tools\verify_save.py`, `tools\m7_check.py`, `tools\input_scripts\m7_*.txt` (5) | Signatur prüfen/neu signieren; fünf geskriptete Läufe mit Auswertung |
| Tests | `tests\save_tests.cpp` (neu), `tests\schema_tests.cpp` | Reihenfolge und Schnappschuss, Platzgrenze, Signatur über die XcSHA-Exporte gegen Python, `game level_end` |

## 3. So speichert und lädt das Spiel

```
Level spielt ─ Levelskript am Ende: FsChangeLevel("Level1-2") ─▶ Statistikbildschirm (Breakdown)
   Speichern ─▶ Speichermenü: [Leerer Platz] [Kanalisation 0h 0m 2s] …   (älteste zuerst, ≤ 25)
      Leerer Platz ─▶ „Daten werden gespeichert“ ─▶ U:\<hash>\Futurama.dat + SaveMeta.xbx + saveimage.xbx
                                                 ─▶ Unlock.dat (Extras) ─▶ „Speichern erfolgreich“
      vorhandener ─▶ „überschreiben?“ (Nein vorausgewählt) ─Ja▶ alte Sicherung löschen, neue anlegen
   Weiter ─▶ nächstes Level · Wiederholen · Beenden
Hauptmenü: Optionen ─ Übernehmen (nur nach Änderung) ─▶ „speichern?“ ─Ja▶ Options.dat
           Spiel laden ─▶ Liste ─A▶ Signatur ok: Level laden │ falsch: „beschädigt, löschen?“ ─Ja▶ Ordner weg
```

Ort im Port: `<Benutzerdaten>\hdd\partition1\UDATA\46490002\` (Standard `%LOCALAPPDATA%\FuturamaPC\…`), Aufbau wie auf der Xbox-Festplatte. Details, Adressen und Dialoglabels: [`m7_saves.md`](../analysis/loader/m7_saves.md).

## 4. Was gemessen wurde

`python tools\m7_check.py --exe build\m7\bin\futurama.exe` (eigene Benutzerdaten `run\m7\check`, Deutsch, 1280×960, `[debug] movies = skip`), Ausgabe `run\m7\m7_check.txt`:

| Kriterium | Ergebnis |
|---|---|
| M7-1 alle Läufe bis `quit 0` | save 49 s, load 53 s, damaged 21 s, slots 37 s, nospace 23 s; keine Fehlerzeile |
| M7-2 Speichern | `Options.dat`, `Futurama.dat`, `Unlock.dat` geschrieben; `SaveMeta.xbx` mit Namen, Ordnername = Hash des Namens, `saveimage.xbx` (4096 B, XPR0), `Futurama.dat` = `Level=Level1-2 Health=100.000000 …` |
| M7-3 `verify_save.py` | nach jedem Lauf alle Container gültig (3/4/3), mit einem Null-Schlüssel keiner |
| M7-4 Neustart + Laden | `Options.dat`/`Unlock.dat` gelesen, Signatur ok, keine Fehlerdialoge; SFX-Lautstärke 0,40 aus Lauf 1 geladen und auf 0,05 weiter gesenkt (mit Standardwerten wäre wieder 0,40 herausgekommen); geladene Spieldaten = gespeicherte, `scripts\Level1-2.Ful` geladen, nächste Sicherung `Level=Level1-3` mit `AgeSeconds` 2,65 > 1,58; ein vorher gesetztes Freischalt-Flag bleibt erhalten |
| M7-5 manipulierte Datei | ein Bit gekippt: `verify_save` Exitcode 1 (DAMAGED), im Spiel „signature … MISMATCH“ und Dialog `MEMCARD_052` |
| M7-6 Löschen über das Menü | „Ja“ → `XDeleteSaveGame("Kanalisation 0h 0m 2s") -> 0`, Ordner weg; beim Überschreiben ebenso („U-Bahn 0h 0m 3s“) |
| M7-7 25 Spielstände | Höchstzahl-Dialog bei „Neues Spiel“, Speichermenü ohne leeren Platz, Überschreiben: vorher 25, nachher 25, alle 27 Container gültig |
| M7-8 zu wenig Platz | Dialog `MEMCARD_047` („8 zusätzliche Blöcke“), „Steuerung“ → Rückkehr ins Spiel, Level 1-1 geladen, Exitcode 0 (vorher 4) |
| Information | Schreib-Completion nach 13,7–16,4 ms zugestellt (höchstens ein Frame, design R16) |

Screenshots (alle unter `run\m7\check\shots_*`): Statistikbildschirm, Speichermenü mit „Leerer Platz“ und mit 25 Einträgen, „Speichern erfolgreich“, Ladeliste, „beschädigtes Spiel löschen?“, Platzdialog mit vorausgewählter „Steuerung“. Alle zeigen die Menüs des Spiels richtig.

## 5. Einstellungen und Werkzeuge

| Schlüssel / Werkzeug | Wirkung |
|---|---|
| `[saves] key_mode` | `fixed` (Standard): fester Projektschlüssel, Spielstände des Ports untereinander gültig. `xbox` + `xbox_cert_key`: Schlüssel einer echten Konsole (ungeprüft, R11) |
| `[saves] signature_key` | eigener fester Schlüssel (32 Hex-Ziffern) |
| `[saves] free_space_limit` | Entwickler: höchstens so viele freie Bytes melden (0 = aus) |
| `python tools\verify_save.py --config <ini>` | prüft alle Spielstände des Benutzerordners; `--resign` signiert eine Datei mit dem eingestellten Schlüssel neu (z. B. importierte), `--json`, `--key`, `--xbox-cert-key` |
| `python tools\m7_check.py --exe <futurama.exe>` | die fünf Prüfläufe; nie `%LOCALAPPDATA%\FuturamaPC` |
| Eingabeskript `game level_end <Level>` | beendet das laufende Level wie das Levelskript (nur für Tests) |

**Spielstände sichern:** Spiel beenden, den Ordner `…\hdd\partition1\UDATA\46490002` kopieren; zum Wiederherstellen zurückkopieren. Einzelne Spielstand-Ordner (12 Hex-Zeichen) lassen sich einzeln kopieren oder löschen; `B4B8BDB51434` sind die Optionen, `95EEB682BD97` die Extras.

## 6. Bekannte Probleme und offene Punkte

- **Bestzeiten der Extras (Original):** Das Spiel führt Datei und Speicher mit einem vorzeichenbehafteten Minimum zusammen; „keine Zeit“ (−1) der frischen Tabelle gewinnt. Nach jedem Neustart sind die Bestzeiten weg, Freischaltungen bleiben. Auf der Xbox genauso (derselbe Code). Ein optionaler Fix wäre ein Hook auf `FUN_000F3B90`.
- **R11:** Spielstände einer echten Xbox nicht geprüft (kein Xbox-Spielstand, kein XboxCERTKey). Ausweg: `verify_save.py --resign`.
- **Echtes Levelende:** Die Prüfläufe erreichen den Statistikbildschirm über das Native des Levelskripts, nicht durch Durchspielen. Danach ist alles Spielcode; das echte Ende von Level 1-1 steht in der Checkliste.
- **Nicht im Lauf:** „Spiel laden“ im Pausenmenü, Schreib- und Löschfehler (`MEMCARD_006/026`), beschädigte Optionen/Extras (`MEMCARD_049/053`), „Weiter“ im Platzdialog.
- **Überschreiben und Reihenfolge:** Ein überschriebener Spielstand ist ein neuer Ordner und steht danach am Ende der Liste; auf FATX nähme er vermutlich den frei gewordenen Platz ein.

## 7. Worauf der Nutzer achten sollte (Checkliste)

1. **Echtes Levelende:** Level 1-1 durchspielen bis zum Statistikbildschirm, „Speichern“ → „Leerer Platz“ → „Speichern erfolgreich“. Danach „Weiter“: Level 1-2 startet.
2. **Laden nach Neustart:** Spiel beenden, neu starten, „Spiel laden“ → der Spielstand steht in der Liste mit Levelname und Spielzeit; A lädt Level 1-2.
3. **Optionen:** Eine Lautstärke ändern, „Übernehmen“ → „Ja“; nach einem Neustart steht der Regler noch dort und die Lautstärke passt.
4. **Mehrere Spielstände:** An zwei Levelenden speichern; die Liste zeigt den älteren oben. Einen überschreiben („Ja“) – der alte verschwindet.
5. **Ordner ansehen:** `%LOCALAPPDATA%\FuturamaPC\hdd\partition1\UDATA\46490002\` enthält die Ordner; einmal sichern und zurückkopieren.
6. **Optional:** `python tools\verify_save.py --config "%LOCALAPPDATA%\FuturamaPC\futurama.ini"` meldet alle Dateien als OK.

## 8. Nächste Schritte

- **M8** spielbares Level von Anfang bis Ende (dabei das echte Levelende mit Speichern), **G3** Breitbild, **G2** Texturen (Reihenfolge laut [`ROADMAP.md`](ROADMAP.md)).
- Optional: Bestzeiten-Fix als Schalter; Xbox-Spielstände prüfen, sobald einer vorliegt (R11).
