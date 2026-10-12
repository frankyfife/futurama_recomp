# M7: Spielstände (Speicherpunkte, Dateien, Signatur, Laden, Löschen, Platz)

Stand: 2026-10-12. Analyse der Retail-EU-XBE für M7 und Prüfung im Port. Grundlage: [`files.md`](files.md) Abschnitt 4 (Layout, Container, Signatur, Platz) und [`m4_input.md`](m4_input.md) 4.3 (Speicherdialoge beim ersten Bestätigen). Dieses Dokument ergänzt die Spielseite: wo das Spiel speichert, wie die Menüs und Dialoge ablaufen, wie Laden, Überschreiben und Löschen funktionieren und was der Port dafür braucht.

Anders als bei M4 gab es **Läufe**: Alle Abläufe aus Abschnitt 1–6 sind mit Eingabeskripten im Port durchgespielt (Abschnitt 10), mit den Beobachter-Proben aus `src\host\xapi\save_probes.cpp` (Protokollzeilen „save: …“) und Screenshots. Statische Aussagen stammen aus dem Ghidra-Export (`analysis\retail_eu_export`) und Disassembly (capstone). Unbelegte Deutungen sind als **Vermutung** markiert.

Adressen sind VAs der Retail-EU-XBE. `FUN_xxx` sind Ghidra-Namen, `XAPILIB::…` Namen aus der XbSymbolDatabase. Dialogtexte stehen als Label (`MEMCARD_0xx` aus `Language/XBOXMEM.ldb`, `MEMHINT_00x` aus `MEMHINTS_XBOX.ldb`), Übersetzungen nur als kurze Umschreibung.

## Inhalt

0. Kurzfassung
1. Wo und wann gespeichert und geladen wird
2. Dateien und Inhalte
3. Signatur und Schlüssel
4. Laden, Überschreiben, Löschen
5. Freier Platz und Höchstzahl 25
6. Reihenfolge der Listen (FATX)
7. Zustellung der Schreib-APC (R16)
8. Was der Port braucht (Kernel/XAPI/VFS)
9. Offen, Vermutungen, nicht geprüft
10. Läufe, Skripte, Werkzeuge

---

## 0. Kurzfassung

* **Einziger Speicherpunkt ist das Levelende.** Das Levelskript ruft das Native FID 0xA1 (Name nach der Liste in `Level1-1.dbg`: **FsChangeLevel**, Abschnitt 1.1) → `FUN_000B0520` → Statistikbildschirm „Breakdown“ mit *Speichern / Weiter / Wiederholen / Beenden*. „Speichern“ öffnet das Speichermenü (`FUN_000E4F70`). Es gibt keine automatische Sicherung beim Spielstart oder an Kontrollpunkten (FsQuickSave/FsQuickLoad schreiben keine Dateien, **Vermutung**: sie halten den Stand im Speicher).
* **Optionen** werden nur über „Optionen → Übernehmen“ gespeichert, und nur wenn sich etwas geändert hat (Frage `MEMCARD_037`, Standard „Nein“). **Extras** (`Unlock.dat`: Freischaltungen und Bestzeiten je Level) schreibt das Spiel nach jeder Spielstandsicherung mit.
* **Dateien** wie in [FIL] 4.1: je Sicherung ein Verzeichnis `U:\<12 Hex>\` (Name = Hash des Sicherungsnamens, `FUN_000F6544`) mit `SaveMeta.xbx`, der 4096-Byte-Datei und `saveimage.xbx`. Der Sicherungsname entsteht automatisch: **Name des nächsten Levels + Gesamtspielzeit**, z. B. „Kanalisation 0h 0m 2s“. Eine Namenseingabe gibt es nicht.
* **Signatur:** HMAC-SHA1 mit `XboxSignatureKey` über 0x000–0xFEB, Ergebnis bei 0xFEC. `tools\verify_save.py` rechnet dasselbe in Python und stimmt mit dem Spiel überein (alle Container gültig, mit einem anderen Schlüssel alle ungültig).
* **Laden:** Hauptmenü „Spiel laden“ → Liste (älteste zuerst) → A. Ein beschädigter Container (Größe, Lesefehler, Signatur) führt zu `MEMCARD_052` („nicht geladen, löschen?“, Standard „Nein“); „Ja“ löscht das Verzeichnis mit `XDeleteSaveGame`.
* **Löschen** gibt es nur auf zwei Wegen: nach `MEMCARD_052` und beim **Überschreiben** (Speichermenü, A auf eine vorhandene Sicherung → `MEMCARD_016`, „Ja“ löscht die alte Sicherung und legt die neue an). Der Hinweis `HINTS_040` („… mit Y löschen“) wird im Code nirgends benutzt; Y wirkt im Speichermenü nicht.
* **Höchstzahl 25:** Bei 25 Sicherungen meldet „Neues Spiel“ `MEMCARD_050` (nur „OK“), und das Speichermenü bietet keinen leeren Platz mehr an – nur Überschreiben.
* **Zu wenig Platz:** `MEMCARD_047` beim ersten „Neues Spiel“ mit „Steuerung“ (Dashboard, **vorausgewählt**) und „Weiter“. „Steuerung“ ruft `XLaunchNewImageA(NULL, {2, 0, 'U', Blöcke})`. Bis M7 beendete der Port dabei den Prozess (Exitcode 4); jetzt meldet er es und kehrt ins Spiel zurück (Abschnitt 5.2).
* **Port:** Die gesamte Spielstand-Logik ist XAPILIB-Code im XBE und lief auf der Kernel-HLE aus M2 **ohne Fehler** durch. Geändert bzw. ergänzt wurden: Listenreihenfolge der beschreibbaren Verzeichnisse (Erstellreihenfolge wie FATX statt NTFS-Namensordnung), Rückkehr aus dem Dashboard-Aufruf, ein Testschlüssel für wenig freien Platz, Beobachter-Proben und die Debug-Aktion `game level_end` für Eingabeskripte (Abschnitt 8).
* **Fehler im Original (Vermutung, belegt durch Code und Lauf):** Die Bestzeiten in `Unlock.dat` gehen nach einem Neustart verloren. Das Zusammenführen von Datei und Speicher (`FUN_000F3B90`) nimmt das Minimum mit **vorzeichenbehaftetem** Vergleich, und der Wert −1 („keine Zeit“) der frischen Tabelle im Speicher gewinnt (Abschnitt 3.4). Die Freischalt-Flags (ODER) bleiben erhalten.

## 1. Wo und wann gespeichert und geladen wird

### 1.1 Levelende: Breakdown und Speichermenü

| Schritt | Code | Wirkung |
|---|---|---|
| Levelskript ruft Native CID 0 / FID 0xA1 (FsChangeLevel, 1 Argument: Name des nächsten Levels) | Native-Verteiler `FUN_000B0C40` case 0xA1 → `FUN_000B0520` (cdecl, `ret` 0xB0634) | Levelzeit in die Extras-Tabelle (`FUN_000F3AD0`/`FUN_000F3B00`, Bestzeit; `FUN_000F3AB0` setzt das Flag, wenn alle Sammelobjekte gefunden sind), `FUN_000A2340(name)` (nächstes Level, `strncpy` 8 nach CApp+0xAC), Breakdown-Objekt `[0x2E8E4C]` anlegen (`FUN_000A4DE0`) und öffnen (`FUN_000A62B0`), Speichertext bauen (`FUN_000C26E0`) |
| Breakdown-Menü | `FUN_000A5940` | Einträge 0 Speichern (`HINTS_BD_002`), 1 Weiter, 2 Wiederholen, 3 Beenden; Bestätigen = Code 4 (A oder START) |
| „Speichern“ | 0xA5D32 → `FUN_000E4F70` (thiscall auf das Speicher-UI-Objekt `[[0x2EB310]+0xF4]`) | nur wenn `[0x2D6D80]` ≠ 0 (Speichern erlaubt, `FUN_000E0010`), sonst Fehlerton (`+0x44`, `toggleerror.wav`) |
| Speichermenü | `FUN_000E1FF0` (lädt `memcard\Savemenu.nif`), Eingabe `FUN_000DD420` | Liste: „Leerer Platz“ (falls < 25) und die vorhandenen Sicherungen; hoch/runter, A, B. A auf „Leerer Platz“ → `MEMCARD_005` („wird gespeichert“), A auf eine Sicherung → `MEMCARD_016` (überschreiben?) |
| Schreiben | Dialogzustand `0xDD020` → `FUN_000E1390` (vf16 des Speicherobjekts, Tabelle 0x2A4DB0+0x40) | Container füllen, signieren (`FUN_000E03B0`), `FUN_000E0B00("Futurama.dat", Name aus dem Menü, Symbol 0)` |
| Extras mitschreiben | `0xDD200`–`0xDD2BC` | `Unlock.dat` lesen (`FUN_000E1160`), mit der Tabelle im Speicher zusammenführen (`FUN_000F3B90`), `FUN_000E1080` schreibt `Unlock.dat` |
| Erfolg | `MEMCARD_007` („Speichern erfolgreich“, nur OK), danach `[0x2E8E4C]+0x90 = 1` (`FUN_000A4960`, 0xDD37E) | „Weiter“ fragt danach nicht mehr `BD_CONFIRM_CONTINUE` |

Gemessen (Lauf `explore3`, `m7_check` Lauf `save`): vom Druck auf „Leerer Platz“ bis `MEMCARD_007` etwa 3,5 s; der Dialog „wird gespeichert“ bleibt mindestens rund 3 s stehen (Vergleich mit dem Float an 0x29559C, `0xDD360`).

**Name der Natives (Vermutung, gut gestützt):** `Level1-1.dbg` enthält die Native-Namen aus `FuNative.fis` in umgekehrter FID-Reihenfolge. Die Zuordnung passt an mehreren Stellen zum Code: 0xBC FsGameSave (ohne Argument, `FUN_000B0180`), 0xBB/0xBA FsConsole/FsConsoleOutput (je 1 Argument, im Retail-Build ohne Wirkung), 0xB9 FsSetDebugOutput (2 Argumente `bEngine`, `bScript`), 0xA1 FsChangeLevel (1 Argument), 0xA0 FsReplayLevel (`FUN_000C2290`, derselbe Aufruf wie „Wiederholen“ im Breakdown), 0x9F FsPlayerIsRolling (liefert ein Bool).

**FsGameSave mitten im Level stürzt ab:** Das Native 0xBC springt direkt in `FUN_000E4F70`. Nach einer erfolgreichen Sicherung schreibt der Dialog aber `[0x2E8E4C]+0x90` (0xDD37E–0xDD386), und ohne Breakdown ist `[0x2E8E4C]` = 0. Im Port beobachtet (Lauf `explore2`): Zugriffsverletzung „write of 0x00000090“ bei 0xA4964, Aufrufer 0xDD38B. Das gilt genauso auf der Xbox (derselbe Code). **Vermutung:** Kein Levelskript benutzt FsGameSave. Nicht geprüft, weil die Skripte ihre Natives meist über einen anderen Opcode aufrufen; direkte Aufrufe mit Opcode 0x13000000 enthalten in allen 23 Levelskripten nur die FIDs 142, 153, 154, 202–207, 250, 332–334 und 337.

### 1.2 Optionen

Hauptmenü „Optionen“ (Eintrag 1). Die Seite hat 8 Einträge: SFX-, Musik- und Dialog-Lautstärke (Schieber, LINKS/RECHTS gehalten), Vibration, Ziel umkehren (Ein/Aus), Bildschirm einst., Vorgaben wiederherst., Übernehmen. „Übernehmen“:

* ohne Änderung: zurück ins Hauptmenü, **kein** Dialog, nichts geschrieben (Lauf `check/load`, erster Versuch);
* mit Änderung: `MEMCARD_037` („Änderungen speichern?“, Ja/Nein, **Standard Nein**) → „Ja“ → `MEMCARD_005` → `FUN_000E0EA0` (vf12) schreibt `Options.dat` in „Futurama Options“ → `MEMCARD_007`. Nach OK kehrt B ins Hauptmenü zurück (Cursor auf „Optionen“).

Geladen wird `Options.dat` **beim Wechsel ins Frontend** (Protokoll: Leseversuch 17 ms nach „context Intro -> Frontend“, noch vor dem ersten Tastendruck; `FUN_000E0F80`, vf13, wendet die Werte mit `FUN_000A1FE0` an). Fehler → `MEMCARD_049`.

### 1.3 Extras

`Unlock.dat` („Futurama Extras“) lädt das Spiel beim **ersten Bestätigen** im Hauptmenü (`FUN_000DC6E0` → `FUN_000E1160`, [M4] 4.3 b; Lauf `check/load`: 50 ms nach dem ersten A) und führt die Datei mit der Tabelle in CApp+0xA4 zusammen (`FUN_000F3B90`, `FUN_000A2530`). Fehler → `MEMCARD_053`. Geschrieben wird nur nach einer Spielstandsicherung (1.1).

### 1.4 Laden eines Spielstands

| Schritt | Code | Wirkung |
|---|---|---|
| Hauptmenü „Spiel laden“ (Eintrag 2) | Menüobjekt mit `images\%s\Loadmenu.nif` | ohne Sicherungen `MEMCARD_051` (OK); sonst Liste (`FUN_000E1270`: `XFindFirst/NextSaveGame("U:\")`, „Futurama Options“ und „Futurama Extras“ werden übersprungen) |
| A auf eine Sicherung | `FUN_000DDEA0` → `FUN_000E0690` (LoadGame) | `FUN_000E0210` lesen (muss 4096 Byte sein), `FUN_000E0340` Signatur prüfen, Nutzdaten kopieren |
| gültig | `FUN_000C1D80`/`FUN_000C1D30` (Text parsen), `[0x2ECF60]` | Kontext Ingame mit dem gespeicherten Level |
| ungültig (Flag `+0x1D`) | `FUN_000DD8B0` | `MEMCARD_052` (Abschnitt 4.2) |

Im Lauf geladen: `Level=Level1-2 …` → `scripts\Level1-2.Ful` geöffnet, Level geladen, steuerbar. Die Gesamtspielzeit läuft weiter: Die nächste Sicherung hatte `AgeSeconds=2.65` gegenüber 1,58 in der geladenen (Lauf `check/load`).

Im Pausenmenü gibt es ebenfalls „Spiel laden“ (`LABEL_014`, Rückfrage `MEMCARD_028` „aktuelles Spiel geht verloren“). **Nicht im Lauf geprüft.**

## 2. Dateien und Inhalte

| Pfad unter `U:\` = `<userdata>\hdd\partition1\UDATA\46490002\` | Größe | Schreiber | Inhalt |
|---|---|---|---|
| `TitleMeta.xbx` | 42 B | XapiMapLetterToDirectory (Start) | UTF-16LE `FEFF "TitleName=Futurama\r\n"` ([FIL] 1.1) |
| `TitleImage.xbx` | 10240 B | dto. | Abschnitt `$$XTIMAGE` |
| `<hash>\SaveMeta.xbx` | 46–58 B | `XCreateSaveGame` | UTF-16LE `FEFF "Name=<Sicherungsname>\r\n"` |
| `<hash>\Futurama.dat` | 4096 B | `FUN_000E1390` | Text, siehe unten |
| `B4B8BDB51434\Options.dat` („Futurama Options“) | 4096 B | `FUN_000E0EA0` | 0x214 Byte Optionen |
| `95EEB682BD97\Unlock.dat` („Futurama Extras“) | 4096 B | `FUN_000E1080` | 0x78 Byte, 24 × {u8 Flag, u32 Bestzeit in s, −1 = keine} |
| `<hash>\saveimage.xbx` | 4096 B | `CopyFileA` aus `d:\SaveGameIcons\<variante>.xbx` | XPR0-Symbol; behält den Zeitstempel der Disc (Win32-Semantik von CopyFile) |

**Verzeichnisname:** `FUN_000F6544` rechnet den UTF-16-Namen als Zahl zur Basis 0x10000 modulo 0xFFFFFFFFFFC5 und schreibt sie als 12 Hex-Ziffern (Großbuchstaben). Nachgerechnet: „Kanalisation 0h 0m 2s“ → `ED2E0E8B528A`, „Futurama Extras“ → `95EEB682BD97`, „Futurama Options“ → `B4B8BDB51434`, „Planet Express 0h 0m 2s“ → `605CBFF5B911` (alle wie im Lauf). `tools\verify_save.py` (`save_dir_name`) enthält dieselbe Rechnung.

**Sicherungsname:** Name des nächsten Levels aus `LEVELNAMES.ldb` (das Breakdown zeigt ihn als „Nächster Level“) und die Gesamtspielzeit, z. B. „Kanalisation 0h 0m 2s“ (Ende 1-1), „U-Bahn 0h 0m 3s“ (Ende 1-2). Zwei Sicherungen mit gleicher Spielzeit am selben Levelende haben denselben Namen und damit dasselbe Verzeichnis: `XCreateSaveGame(…, OPEN_ALWAYS)` öffnet es dann einfach wieder.

**`Futurama.dat`** (Lauf): `Level=Level1-2\nHealth=100.000000\nAmmo=0|0.000000\n … Ammo=4|0.000000\nPower=0.000000\nLives=5\nAgeSeconds=1.583333\nAgeHours=0\n` (Länge inklusive abschließendem NUL). Das gespeicherte Level ist das **nächste** Level.

**`Options.dat`**: Die ersten drei Dwords sind Floats der Lautstärken (Standard 0,75). Nach „SFX leiser“ (40 Ticks LINKS) stand im dritten Dword 0,40, nach einem zweiten Mal 0,05 (**Vermutung**: Dword 2 = SFX). Die übrigen Felder sind nicht entschlüsselt. Die Bytes 0x210–0x213 der Nutzdaten schreibt das Spiel nie ([FIL] 4.1).

**`Unlock.dat`**: Eintrag i gehört zum Level i (`FUN_000F3940` bildet die Level-ID ab): nach dem Ende von 1-1 stand in Eintrag 0 die Zeit 2 s, nach dem Ende von 1-2 in Eintrag 1 die Zeit 1 s. Das Flag setzt `FUN_000B0520`, wenn `[level+0x18C] == [level+0x190] > 0` (alle Sammelobjekte, **Vermutung**); es schaltet Inhalte im Menü „Extras“ frei (`menu/extras/unlock.txt`, [FIL] 1.2).

## 3. Signatur und Schlüssel

### 3.1 Container

Wie [FIL] 4.2: `u32` Länge, Nutzdaten, Nullen, 20 Byte Signatur bei 0xFEC. Signieren `FUN_000E03B0`, prüfen `FUN_000E0340` (vergleicht 5 Dwords). Geprüft wird nur die Signatur und beim Lesen die Größe 4096; die Länge wird nicht begrenzt.

### 3.2 HMAC

`XCalculateSignatureBegin(0)` 0xF7163 → `BeginEx(0, cert.TitleID)` 0xF706F: Kontext mit `LocalAlloc(0, 0x7C)`, Schlüssel `XboxSignatureKey` (eigene Title-ID; sonst `XboxAlternateSignatureKeys[i]` für `AlternateTitleIDs[i]`). `FUN_000F6F26`: Schlüssel auf 64 Byte mit Nullen, `^ 0x36`, `XcSHAInit` + `XcSHAUpdate(64)`. `Update` = `XcSHAUpdate`. `End` → `FUN_000F6F88`: `XcSHAFinal` (innerer Hash), `XcSHAInit`, **ein** `XcSHAUpdate` über 0x54 Byte (Schlüssel `^ 0x5C` und direkt dahinter der innere Hash auf dem Stack), `XcSHAFinal`. Das ist Standard-HMAC-SHA1. Mit Flag 1 („nicht roamable“) käme noch `XboxHDKey` dazu; das Spiel übergibt 0.

Gegenprobe: `tests\save_tests.cpp` rechnet genau diese Aufruffolge über die Kernel-Exporte und vergleicht mit einem Wert aus Pythons `hmac` (`tools\verify_save.py`); `m7_check` prüft alle Container, die das Spiel geschrieben hat, mit `verify_save` (gültig) und mit einem Null-Schlüssel (ungültig).

### 3.3 Schlüsselmodi des Ports (design 4.5)

| `[saves] key_mode` | `XboxSignatureKey` | Folge |
|---|---|---|
| `fixed` (Standard) | SHA-1(„FuturamaPC/XboxSignatureKey/fixed/v1“)[0..15] = `2E0FF62FBBA6AD3D6BDA674C37DB8C66`, oder `[saves] signature_key` | PC-Sicherungen untereinander gültig; Xbox-Sicherungen gelten als beschädigt |
| `xbox` | HMAC-SHA1(`xbox_cert_key`, cert.SignatureKey `98c52648dc61d9aa5aaa4eebf25a4ae4`)[0..15] | **Vermutung** nach Cxbx/xemu, nicht mit einer echten Xbox-Sicherung geprüft (R11 bleibt offen) |

Umsignieren: `python tools\verify_save.py --config <ini> --resign <Datei>` schreibt die Signatur eines 4096-Byte-Containers mit dem eingestellten Schlüssel neu, z. B. um eine Xbox-Sicherung zu übernehmen, deren Schlüssel man nicht hat.

### 3.4 Zusammenführen der Extras (Fehler im Original)

`FUN_000F3B90(out, Datei, Speicher)` bildet je Level `Flag = Datei | Speicher` und `Zeit = (Datei == −1 || Speicher < Datei) ? Speicher : Datei` mit **vorzeichenbehaftetem** Vergleich (`cmp [eax-8], esi; jge` bei 0xF3C5E). Hat der Speicher noch keine Zeit (−1), gewinnt −1. Beim Laden der Extras (`FUN_000DC6E0`) ist die Tabelle im Speicher frisch (`FUN_000F3B60`: alle −1). Darum gehen die Bestzeiten nach jedem Neustart verloren, die Flags bleiben. Beobachtet: Bestzeit von 1-1 nach Lauf 1 = 2 s, nach Lauf 2 (Neustart, Laden, nächste Sicherung) = −1. **Vermutung:** Das passiert auf der Xbox genauso; der Port ändert es nicht (Spieltreue). Wer es will, könnte später einen optionalen Hook auf `FUN_000F3B90` setzen (vorzeichenlos vergleichen).

## 4. Laden, Überschreiben, Löschen

### 4.1 Ergebnisse von ReadContainer `FUN_000E0210`

| Rückgabe | Ursache | Folge in `FUN_000E0690` |
|---|---|---|
| 3 | gelesen | Signatur prüfen; falsch → `+0x1D = 1` (beschädigt) |
| 2 | Datei fehlt (`CreateFileA` scheitert) | beschädigt |
| 1 | `ReadFile` scheitert | beschädigt |
| 0 | Größe ≠ 0x1000 | beschädigt |

### 4.2 Beschädigte Sicherung

`MEMCARD_052` mit Ja/Nein (Standard **Nein**). Eingabe `FUN_000DE1F0`: Code 4 (A) mit Auswahl 0 („Ja“) → `FUN_000E0740` → `XDeleteSaveGame("U:\", Name)`; Erfolg → Eintrag aus der Liste nehmen; noch Sicherungen da → zurück zur Liste (`FUN_000DD810`), sonst Menü schließen (`FUN_000DD5B0`, **Vermutung**); Fehler → `MEMCARD_026`. LINKS/RECHTS wechseln die Auswahl (Codes 2/3 → `FUN_000E3510(0/1)`).

Im Lauf (`check/damaged`): ein Bit der Nutzdaten gekippt → `verify_save` meldet DAMAGED (Exitcode 1) → im Spiel „signature … MISMATCH“, `MEMCARD_052`, LINKS + A → `XDeleteSaveGame("Kanalisation 0h 0m 2s") -> 0`, Verzeichnis `ED2E0E8B528A` weg.

### 4.3 Überschreiben

Speichermenü, A auf eine vorhandene Sicherung → `MEMCARD_016` („überschreiben?“, Standard **Nein**). Eingabe `FUN_000DCDE0`: „Ja“ → `XDeleteSaveGame(alter Name)` (0xDCE39) **vor** dem Schreiben, Eintrag aus der Liste (`FUN_000E17F0`), dann `MEMCARD_005` und die neue Sicherung unter dem aktuellen Namen. Schlägt das Löschen fehl → `MEMCARD_006`. Folge: Bricht das Schreiben danach ab (z. B. Platz), ist die alte Sicherung schon weg.

### 4.4 Rekursives Löschen im Kernel (Trace `--trace=vfs,krnl`, Lauf `explore7`)

`XDeleteSaveGame` liest `SaveMeta.xbx` (Name prüfen), öffnet `\??\U:\<hash>\` (`NtOpenFile`, 0x4021), dann je Eintrag: `NtQueryDirectoryFile` (Klasse 1, RestartScan beim ersten Mal) → `NtOpenFile` relativ zum Verzeichnis-Handle → `NtSetInformationFile` Klasse 0xD → `NtClose` (löscht). Zum Schluss Klasse 4 (Attribute) und 0xD auf das Verzeichnis selbst. Alle Aufrufe `STATUS_SUCCESS`; der Port braucht dafür nichts Neues (M2-Umfang, `tests\vfs_tests.cpp`).

## 5. Freier Platz und Höchstzahl 25

### 5.1 Rechnung

Clustergröße `cs` = `XGetDiskClusterSizeA("U:\")` = 0x4000. Je Container `((cs+0x29)/cs + 1 + 2·((cs+0xFFF)/cs))·cs` = 4 Cluster = 64 KiB ([FIL] 4.4); Summe für Optionen + Extras + ein Spielstand 192 KiB. Freier Platz: `GetDiskFreeSpaceExA("U:\")`, verglichen wird `high | low` ([FIL] 4.4; der Port meldet höchstens 0xFFFF0000 Byte).

| Prüfung | Code | Wann | Bedarf |
|---|---|---|---|
| beim ersten „Neues Spiel“ | `FUN_000DC3A0` → `FUN_000E0C60` | [M4] 4.3 c | keine Sicherung vorhanden: 192 KiB; sonst nur die fehlenden Teile (Optionen, Extras, ein Spielstand, falls < 25) |
| Höchstzahl | `FUN_000E0D60` | ebenda | `FUN_000E07D0` zählt Verzeichnisse mit `Futurama.dat`; > 24 → `MEMCARD_050` (nur OK) |
| im Spiel | `FUN_000E0D70` (`FUN_000DC910`, `FUN_000DCBE0`) | Speichermenü | nur bei < 25 Sicherungen: 64 KiB + 64 KiB, falls `Unlock.dat` fehlt |
| Optionen | `FUN_000E0E00` (`FUN_000DECD0`) | Übernehmen | 64 KiB, falls `Options.dat` fehlt; sonst `MEMCARD_003` |

### 5.2 Zu wenig Platz: `MEMCARD_047`

Text: „nicht genügend freie Blöcke … Sie müssen # zusätzliche Blöcke frei machen“ mit der Zahl aus `FUN_000DFFF0` (fehlende Bytes / Cluster, aufgerundet; mit 64 KiB frei: 8 Blöcke). Auswahl **„Steuerung“ (Dashboard) ist vorausgewählt**, rechts „Weiter“ (Screenshot `check/shots_nospace`). Eingabe `FUN_000DC290`:

* „Steuerung“ (Auswahl 0) → `FUN_000E4B00`: `XGetDiskClusterSizeA`, dann `XLaunchNewImageA(NULL, {2, 0, 0x55, Blöcke})`. Auf der Xbox kehrt das nicht zurück (Dashboard „Speicher“). Danach schließt `FUN_000DC290` den Dialog (`+0x18 = 2`), das Spiel ignoriert die Rückgabe.
* „Weiter“ → gibt es noch keine Sicherung, verbietet `FUN_000E0020(0)` das Speichern (`[0x2D6D80] = 0`); „Speichern“ im Breakdown gibt dann nur den Fehlerton.

**Im Port bis M7:** Der Hook `XLaunchNewImageA` beendete den Prozess (Exitcode 4, `kExitGuestShutdown`). Ein Eingabeskript, das hier A drückt, beendete also das Spiel ([M4] 4.3 Warnung). **Seit M7:** Der Hook protokolliert die Anfrage („asks for the Xbox dashboard … back to the game“), zeigt bei `[debug] msgbox` ein Meldungsfenster mit dem Ordner der Spielstände und gibt `ERROR_NOT_SUPPORTED` zurück. Das Spiel macht weiter wie nach „Weiter“, nur ohne das Speichern zu verbieten; eine spätere Sicherung auf voller Platte scheitert mit der Meldung des Spiels (`MEMCARD_006`). Geprüft mit `[saves] free_space_limit = 65536` (Lauf `check/nospace`: Dialog, Rückkehr, Level 1-1 geladen, Exitcode 0).

### 5.3 25 Sicherungen

Lauf `check/slots`: 25 Spielstände (einer vom Spiel, 24 signierte Kopien mit eigenen Namen „M7 Test 01“ … „M7 Test 24“ und passenden Hash-Verzeichnissen). „Neues Spiel“ zeigt `MEMCARD_050`, nach OK startet das Level. Das Speichermenü am Levelende hat **keinen** „Leeren Platz“; A auf Eintrag 0 → `MEMCARD_016` → „Ja“ → die älteste Sicherung wird gelöscht und die neue angelegt; danach wieder genau 25, alle gültig.

## 6. Reihenfolge der Listen (FATX)

Lade- und Speichermenü zeigen die Sicherungen in der Reihenfolge von `XFindFirst/NextSaveGame` (`FUN_000E1270` hängt in Aufzählungsreihenfolge an, es wird nicht sortiert). Diese Reihenfolge liefert `NtQueryDirectoryFile` auf `U:\`. Auf der Xbox ist das die Reihenfolge der FATX-Verzeichniseinträge: neue Einträge kommen ans Ende bzw. in den ersten freien Platz, also im Wesentlichen **Erstellreihenfolge** (älteste zuerst). NTFS liefert dagegen nach Namen sortiert, hier also nach den Hash-Namen – für den Spieler eine zufällige Reihenfolge.

Der Port liefert deshalb für beschreibbare Host-Verzeichnisse (partition1, Z:) die Einträge nach Erstellzeit (bei Gleichstand nach Namen) und für schreibgeschützte (entpackte Disc, XDVDFS: nach Namen sortiert) weiter in Namensreihenfolge. Beobachtet: Liste „U-Bahn 0h 0m 3s“, „M7 Test 01“, „M7 Test 02“, … (Screenshot `check/shots_slots`). Ein Überschreiben legt ein neues Verzeichnis an, das dann am Ende steht; auf FATX würde es den frei gewordenen Platz der gelöschten Sicherung nehmen (**Vermutung**, kleine Abweichung). NTFS-„Tunneling“ übernimmt die alte Erstellzeit, wenn ein gleichnamiges Verzeichnis innerhalb von 15 s neu entsteht (gleicher Sicherungsname).

## 7. Zustellung der Schreib-APC (R16)

`FUN_000E0B00` schreibt mit `WriteFileEx` (Completion 0xE01A0); die Save-Schicht pollt mit `SleepEx(0, TRUE)` (0xE0030, vf14, aus dem Update des Speicher-UI `FUN_000E52F0`) einmal pro Frame. Gemessen (neue Protokollzeile „I/O APC: save write completion … delivered N ms after the write“): **10,6–16,4 ms** in allen Läufen, also höchstens ein Frame. Die Completion schließt das Handle und gibt den Puffer frei; die Schritte danach (Symbol kopieren, Extras) laufen sofort weiter. R16 ist damit erledigt.

## 8. Was der Port braucht (Kernel/XAPI/VFS)

| Bereich | benutzt von der Save-Schicht | Stand |
|---|---|---|
| `NtCreateFile`/`NtOpenFile` | alle Dispositionen (`CREATE_ALWAYS` → 5, `OPEN_EXISTING`, `OPEN_IF` für Verzeichnisse, `FILE_CREATE` für `CreateDirectoryA`), relativ zu einem Verzeichnis-Handle | M2, unverändert |
| `NtReadFile`/`NtWriteFile` | synchron (`SaveMeta.xbx`, Container lesen, `CopyFileA`), asynchron mit APC (`WriteFileEx`) | M2, unverändert |
| `NtQueryDirectoryFile` | Klasse 1, ein Eintrag je Aufruf, Maske, RestartScan | **geändert:** Erstellreihenfolge (Abschnitt 6) und Schnappschuss je Durchlauf (stabil, während XAPI die zurückgegebenen Einträge löscht) |
| `NtSetInformationFile` | 4 (Attribute/Zeiten: `SetFileAttributesA(0x80)`, `CopyFileA`), 0xD (Löschen), 0xE, 0x14 | M2, unverändert |
| `NtQueryInformationFile` | 0x22 (Größe), 0xE | M2 |
| `NtQueryVolumeInformationFile` | Klasse 3: Cluster 0x4000, freier Platz < 4 GiB | M2; **neu** `[saves] free_space_limit` (Entwickler) |
| `MmAllocate/FreeSystemMemory` | `CopyFileA` (64-KiB-Puffer) | M2 |
| `XcSHAInit/Update/Final`, `XboxSignatureKey` | Signatur | M1/M2; Gegenprobe neu in `tests\save_tests.cpp` |
| `XLaunchNewImageA` (Hook) | `FUN_000E4B00` | **geändert:** Meldung und Rückkehr (5.2) |
| `LocalAlloc` | `XCalculateSignatureBeginEx` (0x7C Byte) | darf nie NULL liefern ([FIL] 4.2) – im Lauf nie |

Neu für Prüfungen: Beobachter-Proben `probe:XAPILIB::XCreateSaveGame`, `probe:XAPILIB::XDeleteSaveGame`, `probe:Save::ReadContainer`, `probe:Save::VerifySignature`, `probe:Save::WriteContainer`, `probe:Save::OpenSaveMenu`, `probe:Save::Dialog` (`src\host\xapi\save_probes.cpp`) und die Eingabeskript-Aktion `game level_end <Level>`, die das Native FsChangeLevel (`FUN_000B0520`) zum nächsten Tick ruft – so erreicht ein kurzer Lauf das Breakdown, ohne das Level durchzuspielen.

## 9. Offen, Vermutungen, nicht geprüft

* **R11:** Schlüsselableitung im Modus `xbox` nicht mit einer echten Xbox-Sicherung geprüft.
* **Nicht im Lauf:** „Spiel laden“ aus dem Pausenmenü (`MEMCARD_028`), `MEMCARD_003` (Optionen ohne Platz), `MEMCARD_006`/`MEMCARD_026` (Schreib- bzw. Löschfehler), `MEMCARD_053`/`MEMCARD_049` (Extras/Optionen beschädigt), das Breakdown nach einem echt durchgespielten Level (nur über das Native erreicht), „Weiter“ in `MEMCARD_047`.
* **Vermutungen:** Namen der Natives (1.1), kein Levelskript ruft FsGameSave, Dword 2 von `Options.dat` = SFX, Bedeutung der Unlock-Flags, FATX-Platzwiederverwendung beim Überschreiben, das Extras-Verhalten (3.4) auf der Xbox.
* `MEMCARD_029` („beschädigte Spielstände gefunden, löschen?“) und `HINTS_040` (Löschen mit Y) haben keine Referenz im Code.

## 10. Läufe, Skripte, Werkzeuge

| Lauf | Skript | Inhalt | Dauer |
|---|---|---|---|
| `run\m7\explore` (`explore1`–`explore7`) | Erkundung (nicht im Repo) | Ladeliste leer, FsGameSave-Absturz, Breakdown, Laden, Speichermenü, Optionen, Überschreiben mit Trace | je 20–40 s |
| `run\m7\check\save` | `tools\input_scripts\m7_save.txt` | Optionen speichern, Neues Spiel, Levelende 1-1, Speichern | 49 s |
| `run\m7\check\load` | `m7_load.txt` | Neustart, Optionen/Extras geladen, Optionen erneut gespeichert, Spiel laden (1-2), Levelende, zweite Sicherung | 53 s |
| `run\m7\check\damaged` | `m7_damaged.txt` | manipulierte Sicherung laden, `MEMCARD_052`, löschen | 21 s |
| `run\m7\check\slots` | `m7_slots.txt` | 25 Sicherungen, `MEMCARD_050`, Überschreiben | 37 s |
| `run\m7\check\nospace` | `m7_nospace.txt` | `[saves] free_space_limit = 65536`, `MEMCARD_047`, Dashboard → zurück ins Spiel | 23 s |

Auswertung: `python tools\m7_check.py --exe build\m7\bin\futurama.exe` (alle fünf Läufe, eigene Benutzerdaten unter `run\m7\check`, nie `%LOCALAPPDATA%\FuturamaPC`). Prüfwerkzeug für Dateien: `tools\verify_save.py`. Die Dialogtexte wurden mit `analysis\loader\scripts\m4_ldb.py xboxmem` aus dem ISO gelesen (Ausgabe unter `run\m4_input_analysis\`, nicht im Repo).
