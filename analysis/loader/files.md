# Dateisystem, Archive und Spielstände (Retail EU)

Ziel-Binary: `orig/retail_eu/default.xbe` (Title-ID `0x46490002`, Zertifikat-Zeitstempel `0x3F0A8B95`).
Disc-Image: `Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso`. Es ist ein getrimmtes XISO mit dem XDVDFS-Volume ab Offset 0 und dem Root bei Sektor 264.
Alle Adressen sind VAs im Retail-XBE. `FUN_xxxxxxxx` sind Ghidra-Namen. Funktionsnamen mit „(V)“ habe ich selbst vergeben, und zwar nach dem Verhalten (Vermutung). Sie stehen nicht in der XbSymbolDatabase.
Die Hilfsskripte liegen unter `analysis/loader/scripts/files_*.py`. Abschnitt 7 sagt, welches Skript welche Aussage belegt.

---

## 0. Kurzfassung

* **Der Spielcode ruft den Kernel nie direkt auf.** `NtCreateFile` und `NtReadFile` werden nur aus XAPILIB aufgerufen, außerdem aus einer Debug-Routine in D3D (Tabelle 2.3, Aufrufer laut `callgraph.tsv`). Alle Spieldateizugriffe laufen über die XAPI-Funktionen `CreateFileA`(V) `0xF76E0`, `ReadFile`, `ReadFileEx`, `WriteFileEx`, `GetFileSize`, `CopyFileA`(V) und die SaveGame-API.
* **Der Hauptpfad für Assets ist `NiUDSStream`** (UDS-Ersatz für NetImmerse-`NiFile`, Konstruktor `0x13F250`, Objekt mit 0x6C Byte). Dieser Pfad
  1. sucht den Namen zuerst in den gemounteten `.img`-Archiven (`FUN_0013FE10` → `FUN_0013FC40`);
  2. öffnet sonst `"D:\" + name` als lose Datei mit `CreateFileA(GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0x68000000 = OVERLAPPED|NO_BUFFERING|SEQUENTIAL_SCAN)`;
  3. liest asynchron mit `ReadFileEx` (APC-Completion `0x13F1D0`) in einen Ringpuffer mit 128 KB (Videos 512 KB). Offsets und Längen sind immer auf 2048 Byte ausgerichtet.
* **Archive:** `sysPAL.img` und `FutPAL00.img` bis `FutPAL04.img`. Die Mount-Reihenfolge ist zugleich die Suchreihenfolge, der erste Treffer gewinnt. Loose files auf `D:\` dienen nur als Fallback und können archivierte Dateien **nicht** überschreiben. PAL oder NTSC (50 oder 60 Hz, Video-Suffix `-PAL` oder `-NTSC`) ergibt sich **aus den Archivnamen**, nicht aus `XGetVideoStandard`. Die Sprache stammt aus `XGetLanguage`. Gesetzt wird sie im `sysPAL`-Zweig von `FUN_000D26A0` und **zusätzlich** in `FUN_000A26A0` (`0xA28DB`–`0xA2900`), sobald `game+0xE8 == 0` (PAL-Modus) gilt. Bei „English“ bleibt es nur ohne `sysPAL.img` im NTSC-Modus, also mit `FutNTSC00.img` und ohne `FutPAL00.img` (Korrektur durch die Verifikation, Abschnitt 3.2). Den PAL/NTSC-Endwert bestimmen allein die `Fut*`-Archive.
* **`tools/futimg.py` hat den Verzeichniseintrag falsch interpretiert** (Stand bei Abfassung des Berichts). Das u32 hinter einem Verzeichnisnamen ist **relativ** zur Position dieses Feldes, nicht absolut. Dadurch stimmten in `orig/retail_eu/*_files.txt` 3654 von 4058 Pfaden nicht. Korrekte Listen: `scripts/files_<img>_gamelist.txt`, Abschnitt 3.1. *Nachtrag Verifikation:* `tools/futimg.py` und `orig/retail_eu/*_files.txt` wurden am 2026-10-04 um 20:22 (nicht durch diesen Bericht) auf die relative Semantik umgestellt. Sie sind jetzt byte-identisch mit den `files_*_gamelist.txt`.
* **Spielstände:** drei Container-Typen auf `U:\`, nämlich `Futurama.dat` (Spielstand, Text `Key=Value`), `Options.dat` (in der Sicherung „Futurama Options“) und `Unlock.dat` (in „Futurama Extras“). Jede Datei ist **genau 4096 Byte** groß: `u32 len`, Nutzdaten, Nullen und am Ende bei `0xFEC` eine 20-Byte-Signatur `XCalculateSignature` (HMAC-SHA1 mit `XboxSignatureKey`, nur „roamable“, Flags 0). Das Spiel liest oder schreibt **keine** EEPROM-Einstellungen; `ExSaveNonVolatileSetting` wird nicht importiert.
* **Empfehlung:** den Kernel auf Ebene von `Nt*`/`Io*` emulieren. Es sind 17 Datei-Imports nötig, zusätzlich die APC-Zustellung bei alertable Waits. `D:` wird direkt aus dem ISO über einen XDVDFS-Reader bedient. `\Device\Harddisk0\partition1` wird auf ein Host-Verzeichnis abgebildet. `XMountUtilityDrive` wird gestubbt, weil `Z:` vom Spiel nie benutzt wird.

---

## 1. Laufwerke, Geräte und Pfade

### 1.1 Laufwerkszuordnung beim Start (XAPILIB)

Startreihenfolge im Hauptthread `0xF8469`: `FUN_000F9804` (Kernel-Patch abhängig von `XboxKrnlVersion`) → `XapiInitProcess` `0xF92C0` → `_rtinit` → `_cinit` → `FUN_000D29B0` (Spiel) → `XapiBootToDash` (Disassembly `0xF8469`–`0xF84D3`).
XBE-InitFlags = `0x5` (Header-Offset `0x124`). AllowedMedia = `0x2`, GameRatings = `0` (Zertifikat an `0x10180`, Feld `+0xA4`).

| Schritt in `XapiInitProcess` | Bedingung (Disassembly) | Wirkung | bei Fehler |
|---|---|---|---|
| `FUN_000F90E6` (V: DVD-Authentifizierung) | immer (`0xF9317`), Ergebnis wird verworfen (`0xF931E jmp`) | `NtOpenFile("\Device\CdRom0")` und `NtDeviceIoControlFile(0x4D014)` mit CDB `0x5A`, nur bei `(AllowedMedia & 0xFFFFFF) == 2` oder `cert.SecurityFlags & 1`; im zweiten Fall zusätzlich CDB `0xAD` und `XcVerifyPKCS1Signature` (`FUN_000F902F`, Ord 344/355) | egal |
| `FUN_000F92AF("\Device\Harddisk0\partition1\")` | `!(InitFlags & 8)` (`0xF9326`) | `NtQueryVolumeInformationFile(FileFsSizeInformation)`: SectorsPerAllocationUnit × BytesPerSector **muss 0x4000 sein** (`FUN_000F8AE2`) | Dashboard |
| `IoCreateSymbolicLink("\??\D:", "\Device\CdRom0")` (Strukturen `0x2D70A0`/`0x2D70A8`) | `!(XboxHardwareInfo->Flags & 8)` (`0xF933D`) | `D:` | Dashboard, außer bei `0xC0000035` |
| `FUN_000F8F75(TitleID, &cert.TitleName)` (V: Laufwerke T:/U: anlegen) | `!(InitFlags & 0x18)` (`0xF9375`) | `T:` → `...partition1\TDATA\46490002`, `U:` → `...partition1\UDATA\46490002`, jeweils über `XapiMapLetterToDirectory` `0xF8C41` | Dashboard |
| `XMountUtilityDrive(InitFlags & 2)` `0xF7F7C` | `!(InitFlags & 8) && (InitFlags & 1)` (`0xF93B3`–`0xF93CC`) | Cache-Partition wählen (`XapiSelectCachePartition` `0xF7D51` liest/schreibt `\Device\Harddisk0\partition0` bei Offset `0x800`, 512 Byte, Magic `0x97315286`), bei neuer Zuordnung mit FATX formatieren (`FUN_000F946D`), `\??\Z:` → `\Device\Harddisk0\Partition<n>\` | Dashboard |
| `XGetParentalControlSetting()` (NV-Setting 10) | Vergleich mit cert GameRatings `0` (`0xF93DD`–`0xF93EE`, `jae`) | – | Dashboard, wenn der Wert > 0 ist |

`XapiMapLetterToDirectory` legt beide Verzeichnisse mit `FILE_OPEN_IF` an. Nur bei `U:` (dort ist param_5 ≠ 0) schreibt es zusätzlich:
* `U:\TitleMeta.xbx`: UTF-16LE, `FEFF "TitleName=Futurama\r\n"` (Format `L"%lc%ls%lc%ls%ls"` `0x2A5B5C`). Der Inhalt wird nur geschrieben, wenn die Datei 0 Byte groß ist. Einen Abschnitt `$$XTINFO` gibt es nicht.
* `U:\TitleImage.xbx`: Inhalt des XBE-Abschnitts `$$XTIMAGE` (`0x3178C0`, 0x2800 Byte, Flags `0x38`, also nicht vorab geladen). Der Abschnitt wird über `XLoadSectionByHandle` → `XeLoadSection` geholt. Wie bei `TitleMeta.xbx` wird nur geschrieben, wenn die Datei 0 Byte groß ist (`FUN_000F8B5A`: `NtCreateFile(..., FileAttributes 4 = SYSTEM, Share 1, FILE_OPEN_IF, 0x22)`, danach Abfrage Klasse `0x22`, EndOfFile == 0).
* `SaveImage.xbx` wird nicht geschrieben, weil kein Abschnitt `$$XSIMAGE` existiert (Abschnittsliste des XBE).

| Laufwerk | Kernel-Ziel | vom Spielcode benutzt? |
|---|---|---|
| `D:` | `\Device\CdRom0` | ja, alle Assets. Groß-/Kleinschreibung beliebig (`"d:\SaveGameIcons\..."`, `"D:\"` an `0x2D7AD8`) |
| `T:` | `\Device\Harddisk0\partition1\TDATA\46490002` | **nein**, kein `T:`-String außerhalb von XAPI |
| `U:` | `\Device\Harddisk0\partition1\UDATA\46490002` | ja, Spielstände (`"U:\"` `0x2A4D80` und als Immediate `0x5C3A55`) |
| `Z:` | `\Device\Harddisk0\Partition3..` (3 + Cache-Index) | **nein**, kein `Z:`-Pfad im Spielcode |
| `W:`, `X:` | – | ANSI_STRINGs an `0x2D7090`/`0x2D7098` werden nicht referenziert |
| Memory Units | – | **nicht unterstützt**: weder `XMountMU` noch weitere `IoCreateSymbolicLink`-Aufrufer (nur die drei oben). Der MU-Klassentreiber ist aber gelinkt: `XInitDevices` (`0x28C3B5`, vom Spiel in `FUN_0009C2A0` nur mit `{g_DeviceType_Gamepad, 4}` aufgerufen) ruft `MU_Init` (`0x28C09C`), das per `IoCreateDevice` (Ord 65) bis zu 8 Geräte `\Device\MU_0`… anlegt (bei Fehler bricht die Schleife ab). Relevant nur, falls XPP nicht komplett ersetzt wird |

### 1.2 Pfade und Pfadvorlagen

„Archiv“ heißt: Die Datei wird über `NiUDSStream` geöffnet und in einem `.img` gefunden. „Lose“ heißt: Die Datei liegt direkt im Disc-Dateisystem. Spalte „VA“: Adresse des Strings.

**Archive und Videos (lose auf D:)**

| Pfad/Vorlage | VA | Benutzer | Zugriff | Bemerkung |
|---|---|---|---|---|
| `sysPAL.img`, sonst `sysNTSC.img` | `0x2A3F50`/`0x2A3F44` | `FUN_000D26A0` → `FUN_0013FE70` (mount) | NiUDSStream `D:\sysPAL.img` | Auf der Disc heißt die Datei **`SysPAL.img`**, die Suche muss also case-insensitiv sein |
| `FutPAL%02d.img` (00 bis 99, bis zum ersten Fehlschlag) | `0x2A3F34` | `FUN_000D26A0` | dto. | Disc: 00 bis 04. `FutPAL05.img` muss „nicht gefunden“ liefern |
| `FutNTSC%02d.img` | `0x2A3F24` | `FUN_000D26A0` | dto. | nur wenn kein `FutPAL00.img` vorhanden ist |
| `Video\%s.xmv` mit `%s = "<name>-PAL"` (`"%s-%s"` `0x2A1838`, Aufruf `0xA2434`) | `0x2A5748` | `FUN_000EE220` | NiUDSStream mit Ring `0x80000`, liest den ersten Packet-Block `0x1000`, danach `FUN_00264360` (XMV-Packet-Decoder mit Callbacks `0xEDBA0`/`0xEDE60`) | 47 Dateien unter `D:\video\`, in keinem Archiv. Die XMV-Bibliothek öffnet selbst keine Dateien (Handle `+0x7C` = −1) |
| `Video/%s.txt` | `0x2A5700` | `FUN_000ED8B0` über `FUN_0013FB10` | ganze Datei lesen, „start_marker_table“ | existiert nirgends, also optional |
| `d:\SaveGameIcons\saveimage.xbx`, `options_saveimage.xbx`, `extras_saveimage.xbx` | `0x2A4D60`/`0x2A4D38`/`0x2A4D10` | `FUN_000E0420` | `CopyFileA` nach `<savedir>\saveimage.xbx` | lose, je 4096 Byte, Magic `XPR0` |

**Inhalte der Archive** (Auswahl. Mit `files_resolve_literals.py` lassen sich 452 der literalen Pfade im Code in den Archiven auflösen.)

| Pfad/Vorlage | VA | Benutzer | Archiv |
|---|---|---|---|
| `BUILD.TXT` | `0x2A1840` | `FUN_000A26A0` (≤ 0x100 Byte nach `0x2E7A68`) | `SysPAL` (in allen 6, erstes gewinnt), Inhalt `1.25` |
| `scripts\%s.Ful` | `0x2A3618` | `FUN_000C5B40` (Level laden) | `FutPAL01`. Die `.dbg`-Dateien liegen daneben, werden aber nirgends referenziert (kein String „.dbg“) |
| `Levels\%s\%s.nif`, `defs.nif`, `backdrop.nif`, `backdrop_static.nif`, `camerapathtest.nif`, `ingame_dlg_%s.nif` (Sprache) | `0x2A35CC`… | `FUN_000C5B40` | `FutPAL00` |
| `SoundCache\%s.nif` | `0x2A35EC` | `FUN_000C5B40` | `FutPAL00` |
| `Sounds\SoundLookup.nif` | `0x2A355C` | `FUN_000C5B40` | `FutPAL03` |
| `XBOX\dsstdfx.bin` | `0x2A3C3C` | `FUN_000CCD00` → `FUN_000CBEB0` (DSP-Effektimage für DSOUND) | `FutPAL00` |
| `Effects\%s.nif`, `images\%s\%s.nif`, `images\levels\%s.nif` | `0x2A3318`/`0x2A3328`/`0x2A34F0` | `FUN_000C22B0`, `FUN_000C5340` | `FutPAL01`/`FutPAL02` |
| `Language/*.ldb` (FRONTEND, HINTS_XBOX, LEVELNAMES, EXTRAS, BREAKDOWN, XBOXMEM, MEMHINTS_XBOX, XBOXERROR, `Language\FileNames.ldb`, `Language\L1A1-INT.ldb`) | `0x294654`, `0x293DF0`, `0x294F74`, `0x293D64`, `0x2A18CC`, `0x2A505C`, `0x2A5040`, `0x2A59EC`, `0x2951DC`, `0x2A56E8` | String-Tabellen-Manager `FUN_000D9CF0`/`FUN_000D9D90` | `SysPAL` (Duplikate in `FutPAL02`). Format „0BDL“ mit allen 5 Sprachen in einer Datei (`files_peek.py`) |
| `sounds/...wav`, `sounds/localized/%s/...` (`%s` = english/german/french/spanish/italian) | viele | Sound-System `FUN_000CC150` u. a. | `FutPAL03` und `FutPAL04` (FutPAL04 enthält alle 5 Sprachen) |
| `fonts/*.fnt`, `fonts/*.nif`, `fonts/kanjii/kanjii.fnt` | `0x2A12D4`…, `0x2A14C8` | `FUN_000985A0` über `FUN_0013FB10`; `fonts/kanjii/kanjii.fnt` wird von `FUN_00099C10` referenziert (`0x99D72`) | `SysPAL`. Achtung: `fonts/kanjii.fnt` (`0x2A12D4`, Ref. `0x988DC`), `fonts/kanjii.nif`, `fonts/arial2t20px32x32.fnt`/`.nif` (`0x2A12E8`, `0x2A133C`, `0x2A1358`) liegen in keinem Archiv und nicht lose auf der Disc |
| `menu\frontend.nif`, `menu/extras/*`, `memcard\Savemenu.nif`, `pause\pause.nif`, `HUD\*.nif`, `Cockpit\*.nif`, `images\%s\Loadmenu.nif`, `images\%s\legal.nif` oder `legal-NTSC.nif`, `images\english\NI_Splash_PAL.nif` oder `_NTSC.nif` | diverse | Menüs und HUD | `FutPAL02`/`SysPAL` |
| `menu/extras/unlock.txt` | `0x293DD8` | `FUN_0001C900` | `FutPAL02`, Text („Futurama Extras Unlock configuration file“) |

**Spielstände und XAPI-eigene Dateien:** Abschnitt 4.

**Nur Entwicklung oder Debug, in Retail inaktiv:**

| Pfad | Benutzer | Warum inaktiv |
|---|---|---|
| `\Device\Harddisk0\Partition1\devkit\xbmovie.dat` (`0x2D9400`) | D3D `FUN_00241C40`, aufgerufen aus `D3DDevice_Swap` | nur wenn `D3D_g_pDevice+0x498/0x49C` ≠ 0, also vom Debugger gesetzt |
| `NiTCache.dat` unter `"D:\"` (`0x2AB008`, Präfix `0x2D85E0` → `0x2AC0D8`) | NiXBox-Texturcache `FUN_00156B40`/`FUN_00161520` | wird nur bei `DAT_0030EDD4` ∈ {1, 2} erzeugt (`0x147618`). Das ist bss, alle 11 Referenzen sind Lesezugriffe (`files_xref.py 30edd4`, `verify_files_r2_globals.py`) |
| `Export\Chars\LipsyncMeshes\*.nif` | `FUN_000D3F40` | nicht auf der Disc |
| `Language\Test.ldb` (`"Language\"` `0x2A428C` + `"Test.ldb"` `0x2A4280`, zusammengesetzt bei `0xD3F61`–`0xD4009`, geladen über `FUN_000D9D90`) | `FUN_000D3F40` (einziger Aufrufer `0xD56AC`) | **liegt auf der Disc** als `language/test.ldb` in `SysPAL` und `FutPAL02` (Korrektur durch die Verifikation). Ob `FUN_000D3F40` in Retail läuft, ist nicht geprüft |
| NiStream-Laden per Dateiname (`NiFile`, CRT `fopen "rb"`, `0x14F360`) | `FUN_00109310`, erreicht über `FUN_000411A0` → `FUN_000410E0` → `FUN_0012F1D0` („anim:“-Einträge), Image-Reader `0x151290`/`0x151390` (ohne Aufrufer im Callgraph) | Umgeht die Archive. `FUN_000411A0` lädt per Dateiname nur, wenn sein dritter Parameter 0 ist; sonst nimmt es die Sequenz aus dem schon geladenen Objekt. Die „anim:“-Zeilen stehen in eingebetteten `NiUDSFileObject "char.cfg"` in `chars/*.ucf` (`FutPAL00`) und haben die Form `anim:idle "idle 0"` (Sequenzname + Index, keine Dateinamen; `verify_files_grep_img.py`). Auf der Disc gibt es keine `.kf`/`.cfg`-Dateien (Vermutung: dieser Pfad ist in Retail tot) |

Keine Dateien sind `EffectList.txt` und `inst_count.txt`: Das sind Objektnamen (`NiUDSFileObject`), die mit `strcmp` verglichen werden (String-Referenzen `0xBFA43`, `0xC6014`). Ebenso sind die vielen `*.tga`-Namen Texturnamen innerhalb von `.nif`.
Fehlende Assets, die der Code trotzdem referenziert, z. B. `sounds/menu/MusicClip.wav` oder `Sounds/Mine_Activate.wav`: Die VFS muss sauber „nicht gefunden“ melden.

---

## 2. Datei-API

### 2.1 Schichten

```
Spielcode ──► NiUDSStream (0x13F250..0x13FFB0) ─┐            (Assets, Archive, Videos)
          ├─► Save-Schicht (0xE0040..0xE1500) ──┤            (U:\, SaveGame-API)
          └─► NiFile/CRT fopen (0x14F360) ──────┤            (inaktiv, s. o.)
                                                ▼
                     XAPILIB (0xF6987..0xF9783): CreateFileA, ReadFile(Ex), WriteFile(Ex),
                     GetFileSize, SetFilePointer, CopyFileA, FindFirstFile, XCreateSaveGame, ...
                                                ▼
                     xboxkrnl: NtCreateFile, NtOpenFile, NtReadFile, NtWriteFile, ... (Tabelle 2.3)
```

### 2.2 XAPI-Funktionen und ihre Aufrufer

„ret“ ist die stdcall-Bereinigung aus dem Binary (`files_xapi_rets.py`). Sie passt jeweils zur Win32-Signatur.

| Adresse | Name | ret | Kernel darunter | Aufrufer im Spiel (`callgraph.tsv`) |
|---|---|---|---|---|
| `0xF76E0` | `CreateFileA` (V) | `0x1C` | `NtCreateFile` mit `RootDirectory = (HANDLE)-3`, `OBJ_CASE_INSENSITIVE`; ohne `FILE_FLAG_BACKUP_SEMANTICS` wird `FILE_NON_DIRECTORY_FILE (0x40)` gesetzt (`0xF77DD`) | NiUDSStream `0x13F250`, Saves `0xE0210`/`0xE07D0`/`0xE0B00`, CRT `_sopen` `0x16FA19`; XAPI-intern `FUN_000F67A9`/`FUN_000F68AB`/`FUN_000F692B` (`SaveMeta.xbx`); inaktiver NiTCache (`FUN_00161520`, Code bei `0x160F3B`/`0x1614CE`) |
| `0xF7195` | `ReadFile` | `0x14` | `NtReadFile` (+ `NtWaitForSingleObject` bei `STATUS_PENDING`) | `0xE0210` (Save laden), CRT `0x16DFAA`, XAPI-intern `FUN_000F65D6`/`FUN_000F661E` (Parser für `SaveMeta.xbx`), inaktiver NiTCache (Code nach `FUN_00161520`) |
| `0xF7603` | `ReadFileEx` | `0x14` | `NtReadFile(h, 0, NtUserIoApcDispatcher, lpCompletion, lpOverlapped, buf, len, &ovl.Offset)` (`0xF7625`–`0xF7630`) | **nur** NiUDSStream `0x13F660` |
| `0xF7654` | `WriteFileEx` | `0x14` | `NtWriteFile` mit APC, analog | **nur** Save schreiben `0xE0B00` |
| `0xF7282` | `WriteFile` | `0x14` | `NtWriteFile` | nur `XCreateSaveGame`, CRT und inaktiver NiTCache (`0x160F00`–`0x161520`) |
| `0xF76A5`/`0xF75C5` | `GetFileSize`/`GetFileSizeEx` | 8/8 | `NtQueryInformationFile` Klasse `0x22` (FileNetworkOpenInformation, 0x38 Byte) | NiUDSStream, `0xE0210`, Completion `0xE01A0` (Ergebnis dort verworfen) |
| `0xF73DC` | `SetFilePointer` | `0x10` | `NtQuery/SetInformationFile` Klasse `0xE`; für `FILE_END` zusätzlich `NtQueryInformationFile` Klasse `0x22` (`0xF7418`) | CRT, XAPI-intern, inaktiver NiTCache |
| `0xF7358` | `SetEndOfFile` | 4 | Klassen `0x14`/`0x13` | inaktiver NiTCache, D3D-Debug, CRT `FUN_001704F1` |
| `0xF7B4D` → `0xF786B` | `CopyFileA` → `CopyFileExA` (V) | `0xC` / `0x18` | `MmAllocateSystemMemory(0x10000, PAGE_READWRITE)` als Kopierpuffer (`0xF7894`, Ord 167; Freigabe `MmFreeSystemMemory`, Ord 172), `NtOpenFile` (Quelle), `NtCreateFile` (`FILE_OVERWRITE_IF`), Chunks zu 64 KB mit `NtRead`/`NtWriteFile`, EOF über Klasse `0x14`, Zeitstempel über Klasse 4 | `0xE0420` (saveimage.xbx); außerdem inaktiver NiTCache `FUN_00161370` |
| `0xF7B6C` | `SetFileAttributesA` (V) | 8 | `NtOpenFile` + Klasse 4 (FileBasicInformation, 0x28 Byte) | `0xE0420` |
| `0xF7C12` | `GetFileAttributesA` (V) | 4 | `NtQueryFullAttributesFile` | NiFile-Access `0x14F0A0` (inaktiv) |
| `0xF7C5D` | `GetDiskFreeSpaceExA` (V) | `0x10` | `NtOpenFile(dir, 0x100001, Share 3, Options 0x800021 = SYNC_IO_NONALERT\|DIRECTORY_FILE\|FILE_OPEN_FOR_FREE_SPACE_QUERY)` + `NtQueryVolumeInformationFile` Klasse 3 | Save-Platzprüfung `0xE0C60`, `0xE0D70`, `0xE0E00`; D3D-Debug `FUN_00241C40` (inaktiv) |
| `0xF8081` | `XGetDiskClusterSizeA` (V, liefert SectorsPerAllocationUnit × BytesPerSector) | 4 | wie oben | **C++-Initialisierer `0x20EFC0` (`"U:\"`)**, `0xE4B00` |
| `0xF8120` | `XGetDiskSectorSizeA` (V, liefert BytesPerSector) | 4 | wie oben | nur der inaktive NiTCache `0x156830` (`"D:\"`) |
| `0xF85AA`/`0xF8616` | `CreateDirectoryA`/`RemoveDirectoryA` (V) | 8/4 | `NtCreateFile(FILE_CREATE, DIRECTORY)` bzw. Klasse `0xD` (Disposition) | nur `XCreateSaveGame` |
| `0xF8961`/`0xF8A6E` | `FindFirstFileA`/`FindNextFileA` (V) | 8/8 | `NtOpenFile(dir)` + `NtQueryDirectoryFile(h, 0, 0, 0, iosb, buf, 0x148, 1, &mask, FALSE)` (Klasse 1 = FileDirectoryInformation, Maske als ANSI_STRING, `"*.*"` → leer, `RestartScan` = FALSE; einen Parameter `ReturnSingleEntry` hat die Xbox-Signatur nicht, der 0x148-Byte-Puffer fasst aber nur einen Eintrag, `0xF8A2D`) | nur `XFindFirst/NextSaveGame` |
| `0xF8886` → `0xF8709` | rekursives Löschen (V) | 8 | `NtQueryDirectoryFile` + Klasse `0xD` | nur `XDeleteSaveGame` |
| `0xF6987` | `XCreateSaveGame` | `0x18` | s. o. + `SaveMeta.xbx` | `0xE0420`, `0xE0B00` |
| `0xF6BEA`/`0xF6CBA`/`0xF6DC1`/`0xF6E08` | `XDeleteSaveGame`/`XFindFirstSaveGame`/`XFindNextSaveGame`/`XFindClose` | 8/8/8/4 | – | `0xE0740` / `0xE0040`, `0xE0540`, `0xE07D0`, `0xE1270`, dazu `0xE0500` (von Ghidra nicht als Funktion erkannt; vf4 des Save-Objekts, vtable `0x2A4DB0`, Eintrag `0x2A4DC0`; prüft „gibt es irgendeine Sicherung?“) |
| `0xF7163`/`0xF70EC`/`0xF7106` | `XCalculateSignatureBegin`/`Update`/`End` | 4/`0xC`/8 | `XcSHAInit`/`Update`/`Final`, `XboxSignatureKey` | `0xE0340` (prüfen), `0xE03B0` (signieren) |
| `0xF7F7C` | `XMountUtilityDrive` | 4 | s. 1.1 | nur `XapiInitProcess` |
| `0xF946D` | FATX formatieren (V, schreibt `"FATX"` `0x58544146`) | 8 | `NtDeviceIoControlFile(0x70000, 0x74004)`, `NtWriteFile`, `NtFsControlFile(0x90020)` | nur `XMountUtilityDrive`. **Ein `XFormatUtilityDrive` existiert im Binary nicht** |

Nicht vorhanden oder nicht benutzt: `FindFirstFile` wird nur von der SaveGame-API verwendet. `MoveFile`, `DeleteFile` und `XGetDiskSectorSize` gibt es im aktiven Pfad nicht.

### 2.3 Benötigte Kernel-Imports (Datei-Teil)

Ordinal = Rohwert aus der Thunk-Tabelle `0x293560` (`raw & 0x7FFFFFFF`). Den Namen liefert die Kandidatentabelle `kernel_common.py`. Die Spalte „Ghidra“ prüft, ob ghidra-xbe denselben Namen an die Aufrufer gehängt hat. „Pushes“ ist die häufigste Anzahl von `push`-Befehlen vor `call [slot]` (`files_kthunks.py`, lineare Rückwärtssuche; abweichende Werte entstehen durch verschachtelte Pushes).

| Slot | Ord | Name | Ghidra | Call-Sites | Pushes (Modus) | Benutzt von |
|---|---|---|---|---|---|---|
| `0x2935C4` | 190 | `NtCreateFile` | ja | 10 | 9 | CreateFileA, CopyFileEx, CreateDirectory, Map-Letter, XCreateSaveGame, D3D-Debug |
| `0x29360C` | 202 | `NtOpenFile` | ja | 13 | 6 | Volume-, Directory- und Attributfunktionen |
| `0x2935E8` | 219 | `NtReadFile` | ja | 6 | 8 | ReadFile(Ex), CopyFileEx, XapiSelectCachePartition |
| `0x2935EC` | 236 | `NtWriteFile` | ja | 12 | 8 | WriteFile(Ex), Copy, Format, Map-Letter |
| `0x2935F0` | 211 | `NtQueryInformationFile` | ja | 9 | 5 | Klassen `0xE`, `0x22` |
| `0x2935B8` | 226 | `NtSetInformationFile` | ja | 12 | 5 | Klassen 4, `0xD`, `0xE`, `0x13`, `0x14` |
| `0x2935F4` | 218 | `NtQueryVolumeInformationFile` | ja | 4 | 5 (der sechste `push esi` ist eine Registersicherung, `0xF80E5`) | Klasse 3 (FileFsSizeInformation) |
| `0x293618` | 207 | `NtQueryDirectoryFile` | ja | 3 | 10 (Xbox-Signatur ohne `ReturnSingleEntry`; Mehrwerte stammen aus Registersicherungen) | Klasse 1 |
| `0x293614` | 210 | `NtQueryFullAttributesFile` | ja | 1 | 2 | GetFileAttributes |
| `0x2935F8` | 198 | `NtFlushBuffersFile` | ja | 1 | 2 (`0xF75A6`/`0xF75A7`; die früher gezählten 5 enthielten den Prolog `push ebp/ecx/ecx`) | `FlushFileBuffers` `0xF759E`, Aufrufer CRT `FUN_0016F8DE` |
| `0x2935C8` | 187 | `NtClose` | ja | 27 | 1 | – |
| `0x293604` | 196 | `NtDeviceIoControlFile` | Aufruf über Register | 0 direkt | – | DVD-Auth, Format |
| `0x293600` | 200 | `NtFsControlFile` | ja | 1 | 10 | Format (Dismount) |
| `0x293620` | 67 | `IoCreateSymbolicLink` | ja | 3 | 2 | D:, T:, U:, Z: |
| `0x293628`/`0x293624` | 203/215 | `NtOpenSymbolicLinkObject`/`NtQuerySymbolicLinkObject` | ja | 1/1 | 2/3 (bei `0xF8387` ist `push edi` eine Registersicherung) | nur `XLaunchNewImageA` |
| `0x2935FC` | 232 | `NtUserIoApcDispatcher` | Datenreferenz | 0 (`push [slot]`) | – | ApcRoutine für `ReadFileEx`/`WriteFileEx` |
| `0x2935E4`/`0x2935B0` | 233/234 | `NtWaitForSingleObject`/`Ex` | ja | 2/1 | 3/4 | Sync-I/O, alertable Waits |
| `0x2935CC`–`0x2935D4` | 336/335/337 | `XcSHAUpdate`/`Init`/`Final` | Stub-Thunks `0xF9CCC`… | – | – | HMAC der Signatur |
| `0x2935DC` | 325 | `XboxSignatureKey` (Daten) | – | – | – | Schlüssel der Save-Signatur |
| `0x2935D8`/`0x2935E0` | 323/354 | `XboxHDKey`/`XboxAlternateSignatureKeys` | – | – | – | nur für nicht-roamable Signaturen bzw. Alternate-Title-IDs, vom Spiel nicht benutzt |
| `0x29361C` | 40 | `HalDiskCachePartitionCount` (Daten) | – | – | – | XMountUtilityDrive |
| `0x2935A4` | 24 | `ExQueryNonVolatileSetting` | Stub `0xF9CC6` | – | – | Einstellungen, Abschnitt 4.5 |

Zusätzlich vom Datei-, Save- und Streaming-Pfad benötigt (Thunk-Tabelle, Namen aus `kernel_common.py`, gegen ghidra-xbe-Aufrufer geprüft; `verify_files_kthunks.py`):

| Slot | Ord | Name | Benutzt von |
|---|---|---|---|
| `0x29356C` | 289 | `RtlInitAnsiString` | jede XAPI-Pfadfunktion (16 Call-Sites), z. B. `CreateFileA` `0xF7745` |
| `0x2935C0` | 301 | `RtlNtStatusToDosError` | `XCreateSaveGame`, `XDeleteSaveGame`, `XQueryValue` |
| `0x293610`/`0x293608` | 167/172 | `MmAllocateSystemMemory`/`MmFreeSystemMemory` | 64-KB-Kopierpuffer in `CopyFileExA` `0xF786B` (Save-Icon) |
| `0x2935BC` | 128 | `KeQuerySystemTime` | Zeitstempel in `FUN_000F692B` (XCreateSaveGame), `XapiMapLetterToDirectory`, FATX-Format |
| `0x2936B4`/`0x2936B8` | 327/328 | `XeLoadSection`/`XeUnloadSection` | `XLoadSectionByHandle` für `$$XTIMAGE` → `TitleImage.xbx` |
| `0x2936B0` | 279 | `RtlEqualString` | Abschnittssuche nach Namen (`FUN_000F9B8E`) |
| `0x2935B4` | 99 | `KeDelayExecutionThread` | `SleepEx` (`WaitMode = UserMode`, `Alertable` = Parameter, `0xF64E9`) – einziger Ort, an dem APCs zugestellt werden. Die beiden XPP-Aufrufe (`0x28CE51`, `0x292D46`) übergeben `KernelMode`, `Alertable = FALSE`; `WaitForSingleObject` (`0xF6510`) und `Sleep` (`0xF6522`) übergeben `FALSE` |
| `0x29370C`/`0x293710` | 169/170 | `MmCreateKernelStack`/`MmDeleteKernelStack` | `CreateFiber`/`DeleteFiber` für asynchrone NiUDSStreams (2.4) |

Weitere direkte Aufrufer von `Nt*`-Datei-Funktionen außerhalb von XAPILIB gibt es laut `callgraph.tsv` nicht, mit Ausnahme von D3D `FUN_00241C40` (xbmovie, inaktiv; die `NtReadFile`/`NtWriteFile`-Sites `0x24192B`/`0x241B95` liegen in nicht als Funktion erkanntem Code zwischen `0x24189C` und `0x241C40`, der ebenfalls nur über den xbmovie-Zustand erreicht wird).

### 2.4 Asynchrones Laden und Streaming (NiUDSStream)

Belege: `FUN_0013F250` (Konstruktor), `FUN_0013F660` (Lesen anstoßen), `0x13F1D0` (Completion), `FUN_0013F590`/`FUN_0013F730` (warten), `FUN_0013F7B0` (Chunkgröße), `FUN_0013F870` (Seek), `FUN_0013F8F0` (Read), `FUN_0013F750` (Ringgröße).

* **Öffnen:** Der Name wird mit Präfix `"D:\"` (`0x2D7AD8`) und Suffix `DAT_0030E598` zusammengesetzt. Das Suffix liegt in bss; es gibt nur eine lesende Referenz, also ist es leer. Danach wird `'/'` durch `'\'` ersetzt. Zuerst läuft die Archivsuche (3.3). Ohne Treffer folgt `CreateFileA(path, 0x80000000, 1, 0, 3, 0x68000000, 0)` und `GetFileSize`.
* **Lesen:** Der Ringpuffer (Standardgröße `0x20000` aus `0x2D7AE4`, Videos `0x80000`) wird bei Bedarf mit `FUN_000A48E0(size, 0x40)` angelegt. `ReadFileEx` liest in `ring + (pos % ringSize)`. Der Offset im OVERLAPPED ist `archiveBase(+0x4C) + alignedPos(+0x5C)`. Positionen sind `& 0xFFFFF800` (Seek `0x13F8CA`, Ringwechsel `0x13F792`), Längen ebenfalls `& 0xFFFFF800` (`0x13F85F`). Der letzte Chunk wird auf das nächste Vielfache von 2048 aufgerundet (`+0x7FF`, `0x13F847`). **Lesevorgänge dürfen also bis zu 2047 Byte über das logische Dateiende hinausgehen**: in den nächsten Archiveintrag oder über das Ende des `.img`. Ein Teil-Read muss dann mit Erfolg und kleinerem `Information` enden, nicht mit einem Fehler.
* **Completion** `0x13F1D0` (`ret 0xC`, also `LPOVERLAPPED_COMPLETION_ROUTINE`): `hEvent` in OVERLAPPED (`+0x28`) enthält den Stream-Zeiger und dient als „pending“-Marke. Die Routine schreibt die Bytes nach `+0x48` und löscht die Marke. Bei `dwErrorCode ≠ 0` wird der Fehler-Handler `DAT_0030E68C` aufgerufen. Gesetzt wird er von `FUN_000F33E0` über `FUN_0013FAD0` und zeigt den Disc-Fehler aus `Language/XBOXERROR.ldb` und `HUD\ErrorIcon.nif`. `FUN_0013F660` wiederholt `ReadFileEx`, solange der Aufruf `FALSE` liefert, aber nur, wenn `DAT_0030E68C` gesetzt ist; sonst gibt es 0 zurück. Endet ein Read per APC mit Fehler, setzt die Completion `+0x48 = 0`. `FUN_0013F8F0` fordert danach denselben Bereich erneut an (Vermutung aus dem Dekompilat: Position rückt um 0 Byte vor). **Die VFS darf Lesevorgänge nie fehlschlagen lassen.**
* **Warten:** Ohne asynchronen Eigentümer ruft `FUN_0013F590` `SleepEx(0, TRUE)` auf. Alertable Waits gibt es an **drei** Stellen: NiUDSStream `0x13F4C0` und `0x13F590` sowie die Save-Schicht `0xE0030` (`push 1; push 0; call SleepEx; mov al,1; ret`). `0xE0030` hat Ghidra nicht als Funktion erkannt, deshalb fehlt sie in `callgraph.tsv` (rohe E8-Suche: `0xE0034`). Sie ist vf14 der Save-Klasse (vtable `0x2A4DB0`, Eintrag `0x2A4DE8`, Konstruktor `FUN_000E0930`, angelegt aus `FUN_000E51C0`). Aufgerufen wird sie über `[this+0xC]->vf14` (`call [eax+0x38]`): in `FUN_000E4CB0`, `FUN_000E4D60` und `FUN_000E4E10` jeweils 10-mal hintereinander, in `FUN_000E52F0` einmal pro Aufruf (Aufrufer `FUN_000D25D0`, `0xD2619`; Vermutung: Update pro Frame). Damit wird die APC von `WriteFileEx` (Completion `0xE01A0`) auch ohne laufenden Stream zugestellt. Dass `[this+0xC]` genau dieses Save-Objekt ist, ist Vermutung. Dafür spricht, dass die Completion `0xE01A0` das Objekt über `[[0x2EB310]+0xF4]+0xC` holt und `game+0xF4` das in `FUN_000D26A0` angelegte `FUN_000E4AA0`-Objekt ist.
* **Asynchroner Modus mit Fibers:** Ist das zweite Konstruktor-Argument ≠ 0, legt der Konstruktor einen Fiber an (`ConvertThreadToFiber`, `CreateFiber(0x4000, FUN_0013F1A0, this)`) und schaltet in `FUN_0013F590` mit `SwitchToFiber` um. Es gibt keinen eigenen Lade-Thread: `CreateThread` (`0xF9A72`) hat als einzigen Aufrufer `entry` (`0xF8531`, Hauptthread), und `PsCreateSystemThreadEx` wird nur dort referenziert (`0xF9AAD`). Alle Datei-APCs gehen also an den Hauptthread. Benutzt wird das von der Lade-Warteschlange `FUN_000D21F0` und `FUN_000D2370` (Zustände 0 bis 5, Fortschrittsrückmeldung).
* **Ganze Datei laden:** `FUN_0013FB10(name, &size)` (Fonts, `.ldb`, `Video/*.txt`).
* **Gleichzeitigkeit:** Alle Streams einer Datei im Archiv benutzen den **gemeinsamen Handle** des `.img`-Streams (`+0x60` → `+0x14`), jeweils mit eigenem OVERLAPPED-Offset. Der Host muss deshalb mehrere gleichzeitige positionelle Reads auf einem Handle unterstützen und darf keinen gemeinsamen Dateizeiger verwenden.

Objektlayout von NiUDSStream (0x6C Byte, aus dem Konstruktor): `+0x08` Interface-vtable (`0x2A9E58`: vf1 IsOpen = Byte `+0x2C`, vf2 Read `0x13F8F0`, vf3 Write → 0), `+0x0C/+0x10` Fibers, `+0x14` HANDLE, `+0x18` OVERLAPPED, `+0x2C` open, `+0x30` async owner, `+0x38/+0x3C` Ringzeiger/-größe, `+0x40` Dateigröße, `+0x4C` Basis-Offset im Archiv, `+0x58/+0x5C` logische/ausgerichtete Position, `+0x60` Archiv-Stream.

### 2.5 CreateFile-Parameter des Spiels

| Wo | Access | Share | Disposition | Flags |
|---|---|---|---|---|
| NiUDSStream `0x13F404` (Flags-Push `0x13F3F0`) | `GENERIC_READ` | `FILE_SHARE_READ` | `OPEN_EXISTING` | `0x68000000` (OVERLAPPED, NO_BUFFERING, SEQUENTIAL_SCAN) |
| Save lesen `0xE0210` | `GENERIC_READ` | 0 | `OPEN_EXISTING` | `0x20000080` (NO_BUFFERING, NORMAL), synchron |
| Save schreiben `0xE0B00` | `GENERIC_WRITE` | 0 | `CREATE_ALWAYS` | `0x60000080` (OVERLAPPED, NO_BUFFERING, NORMAL), `WriteFileEx` mit 0x1000 Byte, Completion `0xE01A0` (schließt den Handle) |
| Save-Existenz `0xE07D0` | `GENERIC_READ` | 0 | `OPEN_EXISTING` | `1` |

`CreateFileA`(V) bildet die Dispositions ab: `CREATE_NEW→FILE_CREATE(2)`, `CREATE_ALWAYS→FILE_OVERWRITE_IF(5)`, `OPEN_EXISTING→FILE_OPEN(1)`, `OPEN_ALWAYS→FILE_OPEN_IF(3)`, `TRUNCATE_EXISTING→FILE_OVERWRITE(4)` (`0xF76E0`). `FILE_SYNCHRONOUS_IO_NONALERT (0x20)` wird nur gesetzt, wenn kein OVERLAPPED angefordert ist, `FILE_NO_INTERMEDIATE_BUFFERING` bei NO_BUFFERING.

---

## 3. Das `.img`-Archiv

### 3.1 Format (korrigiert)

Belegt durch die Lookup-Schleife `FUN_0013FC40` und einen Hexdump von `SysPAL.img` aus dem ISO:

```
u32 header_size                  ; Header inkl. dieses Felds, darf auf bis zu 3 Füllbytes enden (SysPAL: 0xCF7)
entries bis header_size:
  name   : NUL-terminiert, Länge (strlen+4)&~3   ; Name bei Verzeichnissen: erstes Zeichen | 0x80
  Datei  : u32 size, u32 offset                  ; Offset absolut im .img, immer Vielfaches von 0x800
  Verz.  : u32 rel                               ; nächster Geschwistereintrag = &rel + rel
                                                 ; Kinder: Einträge, die vor &rel+rel-4 BEGINNEN
                                                 ;   (Code-Schranke; der letzte Kindeintrag endet bei &rel+rel)
```

Beispiel `SysPAL.img`: Bei Header-Offset `0x4C4` steht `EB "anjii"` (Verzeichnis `kanjii`), das u32 an `0x4CC` ist `0x50`. Die Kinder belegen `0x4D0` bis `0x51C`: `kanjii.fnt`, `out.bit`, `raw2bit.exe`, `vssver.scc` (letzter Eintrag ab `0x508`). Die Schranke im Code ist `0x4CC + 0x50 − 4 = 0x518` und gilt für den **Beginn** eines Eintrags (`piVar7 <= piVar10` in `FUN_0013FC40`). `0x4CC + 0x50 = 0x51C` ist der nächste Geschwistereintrag `E8 "ud"` (Verzeichnis `hud`). Der Code referenziert genau `fonts/kanjii/kanjii.fnt` (`0x2A14C8`, Aufruf `0x99D72`). Alle sechs Header enden mit 3 Füllbytes (`header_size` ≡ 3 mod 4, leerer Name an `header_size − 3`, `verify_files_r2_img.py`).
`tools/futimg.py` las `rel` (Stand bei Abfassung) als **absolutes** Ende. Dadurch fielen Dateien aus Unterverzeichnissen nach oben: In `orig/retail_eu/*_files.txt` waren 90 von 134 (SysPAL), 299 von 366, 47 von 78, 462 von 512, 838 von 938 und 1918 von 2030 Pfaden falsch. Größe und Offset stimmten (`files_futimg_diff.py`; die Zahlen hat `verify_files_r2_img.py` mit einer Nachbildung der alten Logik bestätigt). Spielgetreue Listen erzeugt `files_imgheader.py`: `scripts/files_<img>_gamelist.txt`. Die Namen im Archiv sind kleingeschrieben. *Nachtrag:* `tools/futimg.py` und `orig/retail_eu/*_files.txt` sind seit 2026-10-04 20:22 korrigiert und stimmen mit den Gamelists überein.

### 3.2 Mounten und Auswahl (`FUN_000D26A0`, `FUN_0013FE70`)

1. `thunk_FUN_0013FE70("sysPAL.img")`. Bei Erfolg wird `game+0xE8 = 0` (PAL) gesetzt, der String-Manager angelegt und **die Sprache gesetzt**: `FUN_000D9990()` → `XGetLanguage()`: 3 → `"german"`, 4 → `"french"`, 5 → `"spanish"`, 6 → `"italian"`, sonst `"english"`. Danach folgt `FUN_000D9B20(lang)`.
   Fehlt `sysPAL.img`, wird `sysNTSC.img` probiert (dann `+0xE8 = 1`). Im `sysNTSC`-Zweig wird keine Sprache gesetzt.
   **Korrektur (Verifikation):** `FUN_000A26A0` (am Ende von `FUN_000D26A0` aufgerufen) setzt die Sprache noch einmal genauso (`0xA28DB` `mov eax,[esi+0xE8]` / `jne 0xA29C8`, dann `FUN_000D9CF0` → `FUN_000D9990` (`0xA28F3`) → `FUN_000D9B20`). Das geschieht immer, wenn am Ende `+0xE8 == 0` gilt (PAL). Den Endwert von `+0xE8` bestimmt allein Schritt 3: Er ist 1 genau dann, wenn `FutNTSC00.img` gefunden wurde und `FutPAL00.img` nicht. Das `+0xE8 = 1` aus dem `sysNTSC`-Zweig wird dort immer überschrieben. Bei `"English"` (`FUN_000D9A30`) bleibt es also nur, wenn `sysPAL.img` fehlt **und** der NTSC-Modus aus den `FutNTSC`-Archiven folgt. Ohne jedes Archiv endet `+0xE8` bei 0, und die Sprache wird gesetzt.
2. Danach wird der Fehler-Handler registriert (`FUN_000F33E0`).
3. `FutPAL%02d.img` für i = 0 bis 99, bis ein Mount fehlschlägt. Bei mindestens einem Treffer gilt `+0xE8 = 0`. Ohne Treffer wird `FutNTSC%02d.img` analog probiert (dann `+0xE8 = 1`, ohne Treffer wieder 0).
4. `FUN_0013FE70(name)`: Während des Mountens ist die Archivsuche über `DAT_002D7AE8 = 0` abgeschaltet, das `.img` wird also als lose Datei `D:\<name>` geöffnet. Die Funktion liest 4 Byte (`header_size`), springt zurück auf 0, allokiert `header_size` und liest den ganzen Header (`0x13FEE0`–`0x13FF29`). Ein 8-Byte-Eintrag `{Stream (refcounted, bleibt offen), Header}` wird an das Array `DAT_002D7AF0` angehängt (Zähler `DAT_002D7AF8`). Ein Unmount existiert nicht; `FUN_000D2920` ruft beim Beenden `thunk(0)` auf, ohne Wirkung.

`game+0xE8` steuert in `FUN_000A26A0` (`0xA26BD`–`0xA2744`) Video-Suffix (`"NTSC"`/`"PAL"` → `DAT_002E7B70`), Bildrate (60.0f/50.0f) und Frame-Zeit, außerdem dort die Sprachwahl (siehe oben). Die Auswahl von `images\%s\legal-NTSC.nif` bzw. `images\english\NI_Splash_NTSC.nif` liest `[[0x2E7B84]+0xE8]` im Intro-Code (`0x2B65B`–`0x2B67C` bzw. um `0x2B81x`–`0x2B84E`), nicht in `FUN_000A26A0`. **Die Retail-EU-Disc läuft also immer im 50-Hz-Takt des Spiels, unabhängig vom AV-Modus.**

### 3.3 Lookup (`FUN_0013FE10` → `FUN_0013FC40`)

`FUN_0013FE10(name, &offset, &size)` durchsucht die gemounteten Archive in Mount-Reihenfolge und liefert beim ersten Treffer den Archiv-Stream. `FUN_0013FC40` (thiscall auf dem Eintrag) arbeitet so:
* Kopiert den Namen, wandelt mit `FUN_00170B77` A–Z in Kleinbuchstaben und ersetzt `/` durch `\`.
* Für jede Pfadkomponente vor einem `\`: erstes Zeichen `|= 0x80`, danach `__stricmp` gegen die Einträge der aktuellen Ebene, nicht passende Einträge werden übersprungen (Datei: +8; Verzeichnis: `&rel + rel`). Bei Treffer steigt die Suche in den Bereich `[&rel+4, &rel+rel-4)` ab.
* Letzte Komponente: `__stricmp`, liefert `size` (erstes u32) und `offset` (zweites u32).

Folgen: Groß-/Kleinschreibung und Trennzeichen sind egal. Archivinhalte haben Vorrang vor losen Dateien. Duplikate wie `build.txt` (in allen 6 Archiven) sowie `fonts/*`, `hud/*`, `language/*` (SysPAL und FutPAL02) werden aus dem ersten Archiv gelesen (`files_archives.py`).

### 3.4 Inhalt der Archive (spielgetreu geparst)

| Archiv | Größe | Header | Dateien | oberste Verzeichnisse |
|---|---|---|---|---|
| `SysPAL.img` | 7 742 704 | `0xCF7` | 134 | `language` (57), `fonts` (46), `hud` (29) |
| `FutPAL00.img` | 603 480 112 | `0x270B` | 366 | `levels` (254), `chars` (67, `.ucf`), `soundcache` (22), `sounds` (19), `xbox` (2) |
| `FutPAL01.img` | 32 596 776 | `0x7BF` | 78 | `scripts` (46, `.ful` + `.dbg`), `effects` (30) |
| `FutPAL02.img` | 287 900 703 | `0x316F` | 512 | `images` (288), `menu` (67), `language` (57), `fonts` (46), `hud` (29), `memcard` (17) |
| `FutPAL03.img` | 155 787 088 | `0x6013` | 938 | `sounds` (936) |
| `FutPAL04.img` | 65 127 064 | `0xB003` | 2030 | `sounds/localized/{english 423, french 420, german 397, italian 413, spanish 375}` |

Alle Offsets in allen 6 Archiven sind 2048-ausgerichtet. Kein Eintrag reicht über das Dateiende hinaus (`files_imgheader.py`).

### 3.5 Caching

* Die Header werden vollständig im RAM gehalten (maximal 45 KB, FutPAL04). Die Archiv-Handles bleiben die ganze Sitzung offen.
* Darüber hinaus gibt es pro Stream nur den Ringpuffer (128 KB bzw. 512 KB) und das vollständige Einlesen kleiner Dateien (`FUN_0013FB10`). Es gibt weder einen dateiübergreifenden Cache noch Prefetch auf die HDD: `Z:` bleibt ungenutzt, der NiTCache ist inaktiv. `SoundCache\*.nif` sind Soundbänke, kein Dateicache.

---

## 4. Spielstände und Einstellungen

### 4.1 Layout auf `U:` (Xbox-Konvention, durch XAPI erzeugt)

```
U:\  = \Device\Harddisk0\partition1\UDATA\46490002\
  TitleMeta.xbx              UTF-16 "TitleName=Futurama\r\n"   (XAPI, 1.1)
  TitleImage.xbx             0x2800 Byte aus $$XTIMAGE         (XAPI)
  <12 Hex-Zeichen>\          ein Verzeichnis je Sicherung; Name = Hash des Sicherungsnamens (FUN_000F6544)
     SaveMeta.xbx            UTF-16LE: FEFF "Name=<Sicherungsname>\r\n" (Formate 0x2A5B5C/0x2A5B7C, "NoCopy=1" nur bei Flag 1, Spiel übergibt 0)
     Futurama.dat | Options.dat | Unlock.dat   genau 4096 Byte
     saveimage.xbx           Kopie von d:\SaveGameIcons\<variante>.xbx (4096 Byte, XPR0)
```

| Sicherung (`XCreateSaveGame`-Name) | Datei | Icon-Quelle | Nutzdaten |
|---|---|---|---|
| frei vom Spieler (Unicode), maximal **25** (`FUN_000E0D60`: Anzahl > 0x18 heißt voll) | `Futurama.dat` (`0x2A4CD8`) | `saveimage.xbx` | ASCII-Text aus `FUN_000C26E0`: `Level=%s`, `Health=%f`, `Ammo=%d\|%f`, `Power=%f`, `Pickup=%s`, `Equip=%s`, `Lives=%d`, `AgeSeconds=%f`, `AgeHours=%d` (Vorlagen für den Levelstart an `0x2D5BC0`…) |
| `"Futurama Options"` (`0x2A4D94`) | `Options.dat` (`0x2A4E04`) | `options_saveimage.xbx` | `0x214` Byte (`FUN_000A1860` allokiert und meldet 0x214): 5 Dwords, 4 Bools, 0x7E Dwords aus dem Optionsobjekt. Das sind nur 0x210 Byte; **die letzten 4 Byte (0x210–0x213) werden nie beschrieben** und enthalten Heap-Reste. `Options.dat` ist deshalb zwischen zwei Läufen nicht byte-identisch, die Signatur deckt diese Bytes trotzdem ab |
| `"Futurama Extras"` (`0x2A4D84`) | `Unlock.dat` (`0x2A4E10`) | `extras_saveimage.xbx` | `0x78` Byte (`FUN_000F3A10`/`FUN_000F3A20`): 24 × {u8, u32} |

### 4.2 Container-Format (alle drei Dateien)

```
0x000  u32   payload_len
0x004  u8[]  payload
...    00    Nullen bis 0xFEC
0xFEC  u8[20] Signatur = XCalculateSignature(Bytes 0x000..0xFEB)
```

Beim Laden (`FUN_000E0210`) muss `GetFileSize == 0x1000` sein, sonst Rückgabe 0. Dann folgen `ReadFile(0x1000)`, die Prüfung in `FUN_000E0340` (vergleicht 5 Dwords) und das Kopieren von `payload_len` Bytes ab `+4`. `payload_len` wird nicht gegen 0xFE8 begrenzt, das schützt nur die Signatur. Das Flag `+0x1D` setzt `FUN_000E0690` (bzw. der analoge Code ab `0xE0F80`) nicht nur bei falscher Signatur, sondern auch, wenn die Datei fehlt (Rückgabe 2), die Größe falsch ist (0) oder `ReadFile` scheitert (1). Das Spiel meldet die Sicherung dann als beschädigt. Schlägt `XCalculateSignatureBegin` fehl (`LocalAlloc` → NULL), dereferenziert `FUN_000E0340` einen NULL-Zeiger (`piVar2 = 0`, danach `*piVar2`).
Beim Speichern füllt `FUN_000E03B0` die Signatur bei `0xFEC` ein. `FUN_000E0B00` ruft `XCreateSaveGame("U:\", name, OPEN_ALWAYS = 4, 0, out, 0x104)` auf, erzeugt die Datei mit `CREATE_ALWAYS` und schreibt per `WriteFileEx(0x1000)`. Danach folgt `FUN_000E0420`: `XCreateSaveGame(..., OPEN_EXISTING = 3, ...)`, `SetFileAttributesA(0x80)`, `CopyFileA(icon, "<dir>\saveimage.xbx", FALSE)`, noch einmal `SetFileAttributesA`.
Löschen: `FUN_000E0740` → `XDeleteSaveGame("U:\", name)`. Es prüft den Namen in `SaveMeta.xbx` und löscht den Baum rekursiv.
Aufzählen: `FUN_000E0540`/`FUN_000E1270` gehen per `XFindFirst/NextSaveGame("U:\")` alle Verzeichnisse durch und lassen die beiden festen Namen aus. `FUN_000E07D0` zählt die Verzeichnisse, die `Futurama.dat` enthalten.

### 4.3 Signatur

`XCalculateSignatureBegin(0)` (`0xF7163`) ruft `BeginEx(flags = 0, cert.TitleID)` auf. Weil die Title-ID der eigenen entspricht, ist der Schlüssel `XboxSignatureKey` (Kernel-Daten, Ord 325). `FUN_000F6F26`/`FUN_000F6F88` berechnen damit ein Standard-HMAC-SHA1: Schlüssel 16 Byte, auf 64 Byte mit Nullen aufgefüllt, `^0x36`/`^0x5C`, über `XcSHAInit`/`Update`/`Final`. `XboxHDKey` würde nur bei `flags & 1` (nicht roamable) verwendet; das Spiel setzt dieses Flag nicht.
**Der Host muss also SHA-1 korrekt implementieren und einen festen 16-Byte-Schlüssel `XboxSignatureKey` bereitstellen.** Mit einem eigenen Schlüssel sind am PC erzeugte Sicherungen untereinander gültig, echte Xbox-Sicherungen werden dann aber als beschädigt abgelehnt.
Für Kompatibilität mit Xbox-Sicherungen muss der Schlüssel wie im Xbox-Kernel abgeleitet werden. Nach öffentlicher Doku (Cxbx-Reloaded/xemu) ist das HMAC-SHA1(XboxCERTKey, cert.SignatureKey), gekürzt auf 16 Byte. **Das ist eine Vermutung und nicht aus diesem Binary belegt.** Der Wert von `cert.SignatureKey` an `0x10180+0xC0` ist `98c52648dc61d9aa5aaa4eebf25a4ae4`.

### 4.4 Platzbedarf und Clustergröße

* Die C++-Initialisierer `0x20EFC0`–`0x20F075` laufen in `_cinit` **nach** `XapiInitProcess`. Sie stehen als fünf aufeinanderfolgende Einträge `0x2D0F84`–`0x2D0F94` in der C++-Init-Tabelle. Sie berechnen `cs = XGetDiskClusterSizeA("U:\")` → `0x2EDB0C`, für jeden Container-Typ `((cs+0x29)/cs + 1 + 2*((cs+0xFFF)/cs)) * cs` (`0x2EDB40`, `0x2EDB60`, `0x2EDB3C`) und die Summe `0x2EDB88`. Gerechnet wird mit `idiv ecx` (`0x20EFDD`, `0x20EFE6`, `0x20F00D` usw.). **Liefert der Host 0, stürzt das Spiel mit einer Division durch null ab.** `XGetDiskClusterSizeA` liefert auch 0, wenn `NtOpenFile("U:\")` oder `NtQueryVolumeInformationFile` scheitert (`0xF8081`). Dieselbe Division steckt in `FUN_000DFFF0` (`(x+cs-1)/cs`, Aufrufer `FUN_000DC3A0` und `FUN_000DECD0`). Zum Vermeiden des Absturzes reicht ein Wert ≠ 0. `0x4000` ist wegen der `partition1`-Prüfung in `XapiInitProcess` und für korrekte Blockzahlen trotzdem der richtige Wert. Bei `cs = 16384` ergeben sich 64 KiB je Container und 192 KiB für den ersten Start.
* Freier Platz (`0xE0CA8`–`0xE0CB5`): `GetDiskFreeSpaceExA("U:\", &free64)` und dann `edx = high | low`. Verglichen wird ohne Vorzeichen mit der Summe `[0x2EDB88]` (`0xE0CD3` `mov ecx,[0x2EDB88]`, `0xE0CD9` `cmp edx,ecx`, `jae`). In `FUN_000E0C60` läuft die Prüfung nur, wenn noch keine Options-, Extras- oder Futurama-Sicherung existiert (`0xE0CC3`–`0xE0CD1`). Dasselbe `high | low` steht auch in `FUN_000E0D70` (`0xE0DA3` `or edi,[esp+0xC]`) und `FUN_000E0E00` (`0xE0E33` `or eax,[esp]`). Deren Vergleichswerte sind nicht einzeln geprüft. **Fehler im Spiel:** Liegt der freie Platz bei 4 GiB oder mehr, kann `high | low` klein ausfallen (z. B. 4 GiB + 100 Byte ergibt 101), und das Spiel meldet zu wenig Platz. Schlägt der Aufruf fehl, rechnet das Spiel mit `0xFFFFFFFF`, also mit unbegrenztem Platz. → Der Host sollte für `U:` einen freien Platz unter 4 GiB melden, z. B. höchstens `0xFFFF0000`.
* Bei zu wenig Platz ruft `FUN_000E4B00` `XLaunchNewImageA(NULL, {2, 0, 0x55 ('U'), Blöcke})` auf und will ins Dashboard (Speicherverwaltung). Am PC: abfangen und als Meldung anzeigen.

### 4.5 Persistente Einstellungen und Abfragen

| API | Quelle | Aufrufer | Wirkung im Spiel | Host-Empfehlung |
|---|---|---|---|---|
| `XGetLanguage` `0xF6440` | `ExQueryNonVolatileSetting(7)`, bei > 9 Rückfall auf `XGetGameRegion` (NV `0x104`): `(region == 2) + 1` | nur `FUN_000D9990` (dieses wiederum aus `FUN_000D26A0` `0xD2795` und `FUN_000A26A0` `0xA28F3`) | Sprache (3 = de, 4 = fr, 5 = es, 6 = it, sonst en; Fehler der NV-Abfrage → 0 → en), wirksam im `sysPAL`-Zweig und immer im PAL-Modus (`game+0xE8 == 0`) | Konfigurierbar: 1, 3, 4, 5 oder 6 zurückgeben |
| `XGetParentalControlSetting` `0xF9C3E` | NV 10 | `XapiInitProcess` | > cert GameRatings (0) führt ins Dashboard | 0 |
| `XGetAudioFlags` `0x1707A0` | NV 9 und `XGetAVPack` (= `*HalBootSMCVideoMode`) | DSOUND `FUN_00248536` | AV-Pack 3 erzwingt 1, AV-Pack 6 maskiert auf 16 Bit | NV 9 = 0 (Stereo, Vermutung), AV-Pack nicht 3 |
| `XQueryValue(0x11)` | NV `0x11` | `FUN_000F9B1A` (Auto-Power-Down-Timer) | Bit 0 | 0 |
| `XGetVideoStandard`/`XGetVideoFlags` | – | **nicht vorhanden** | PAL/NTSC folgt aus den Archivnamen (3.2). D3D fragt selbst `AvSendTVEncoderOption(…,6,…)` und `AvGetSavedDataAddress` (D3D wird ersetzt) | – |
| Optionen des Spiels | `Options.dat` (4.1) | – | Lautstärken, Steuerung usw. | wie Spielstände |

Das Spiel schreibt nichts ins EEPROM (`ExSaveNonVolatileSetting`, Ord 29, wird nicht importiert).

---

## 5. Empfehlung für das virtuelle Dateisystem des Hosts

### 5.1 Ebene: Kernel (`Nt*`/`Io*`), XAPI unverändert lassen

Begründung: Alle Spielzugriffe laufen durch rund 17 Kernel-Funktionen (Tabelle 2.3). XAPI erledigt dabei Laufwerkszuordnung, Sicherungsverzeichnisse, Signaturen und Metadateien originalgetreu, und die Verzeichnisstruktur bleibt mit Xbox-HDD-Dumps kompatibel. Eine Emulation auf XAPI-Ebene müsste dagegen auch `XapiInitProcess`, `XCreateSaveGame` usw. nachbauen.

Pflichtsemantik:
1. **Objekt-Namensraum:** `OBJECT_ATTRIBUTES` = {RootDirectory, PANSI_STRING ObjectName, Attributes = `0x40`}.
   * `RootDirectory = (HANDLE)-3` (`0xFFFFFFFD`) bedeutet relativ zu `\??\`, also DOS-Pfade wie `"D:\Video\..."`, `"U:\"`, `"d:\SaveGameIcons\..."`.
   * `RootDirectory = 0` mit absolutem Namen kommt ebenfalls vor: `"\??\U:\..."` (`XDeleteSaveGame`), `"\Device\Harddisk0\partition1\TDATA"`, `"\Device\CdRom0"`.
   * Ein Handle als RootDirectory für relative Namen nutzt `FUN_000F8709`.
   * `IoCreateSymbolicLink` muss echte Links anlegen und `0xC0000035` liefern, wenn der Name schon existiert.
   * Laufwerksbuchstaben und Pfade sind case-insensitiv, `/` und `\` gleichwertig. Nötig mindestens wegen `sysPAL.img` vs. `SysPAL.img` und `Video\` vs. `video/`.
2. **`NtReadFile`/`NtWriteFile` asynchron:** Bei Handles ohne `FILE_SYNCHRONOUS_IO_*` mit `ApcRoutine = NtUserIoApcDispatcher` wird die I/O ausgeführt (gern synchron) und eine User-APC an den aufrufenden Thread gehängt. Zugestellt wird sie **erst** in einem alertable Wait (`KeDelayExecutionThread(Alertable = 1)`, `NtWaitForSingleObjectEx(.., Alertable = 1, ..)`). `NtUserIoApcDispatcher` muss als aufrufbare Funktion im Thunk-Slot `0x2935FC` stehen und `Completion(err = RtlNtStatusToDosError(status) oder 0, iosb.Information, (OVERLAPPED*)iosb)` aufrufen. Belegt sind dabei nur die Parameter: `ReadFileEx`/`WriteFileEx` übergeben `ApcContext = lpCompletionRoutine` und `IoStatusBlock = lpOverlapped` (`0xF7621`/`0xF7622`), und die Completions `0x13F1D0`/`0xE01A0` erwarten `(dwErrorCode, dwBytes, lpOverlapped)` mit `ret 0xC`. Die Abbildung des Status auf `err` ist Windows-Konvention (Vermutung), die Spiel-Completions prüfen nur `err ≠ 0`. `ByteOffset` stammt immer aus dem OVERLAPPED; mehrere gleichzeitige Reads pro Handle müssen funktionieren. Es gibt nur einen Spiel-Thread (2.4), die APCs gehen also immer an den Hauptthread.
3. **Dateiende:** Ein Read über das Dateiende hinaus liefert `STATUS_SUCCESS` mit kleinerem `Information`. Fehler darf es nie geben (2.4).
4. **Informationsklassen:** `NtQuery/SetInformationFile` mit 4 (Basic, 0x28), `0xD` (Disposition), `0xE` (Position), `0x13` (Allocation), `0x14` (EOF), `0x22` (NetworkOpen, 0x38). `NtQueryVolumeInformationFile` mit 3 (FsSize, 0x18): für `partition1` und `U:` **SectorsPerAllocationUnit × BytesPerSector = 0x4000**, für `U:` freie Einheiten unter 4 GiB/16 KiB. `NtQueryDirectoryFile` mit 1 (FileDirectoryInformation; Xbox-Signatur mit 10 Argumenten `…, Length, FileInformationClass, FileMask (PANSI_STRING), RestartScan`, ohne `ReturnSingleEntry`; Puffer 0x146/0x148 Byte, also ein Eintrag pro Aufruf; ohne `.` und `..`, Verzeichnisse mit Attribut `0x10`). `NtQueryFullAttributesFile`.
5. **Abfangen oder stubben:**
   * `XMountUtilityDrive` `0xF7F7C` (stdcall, 1 Argument): `\??\Z:` auf ein Cache-Verzeichnis legen und `TRUE` liefern. So entfällt die Emulation von `partition0`, der Cache-Tabelle, der Disk-IOCTLs und der FATX-Formatierung.
   * `NtDeviceIoControlFile` auf `\Device\CdRom0`: Fehler zurückgeben, das Ergebnis wird ignoriert.
   * `XLaunchNewImageA` und `HalReturnToFirmware`: beenden oder Meldung anzeigen.
   * `XeLoadSection("$$XTIMAGE")`: auf den gemappten Abschnitt verweisen.

### 5.2 `D:` direkt aus dem ISO (empfohlen) oder aus extrahierten Dateien

* **ISO mit XDVDFS-Reader** (Logik wie `tools/xdvdfs.py`): Volume-Descriptor `MICROSOFT*XBOX*MEDIA` bei Sektor 32. Für Redump-Images liegt die Basis bei `0x18300000`, für dieses XISO bei 0. Jede Datei ist ein zusammenhängender Extent, also ist `read(path, off, len)` gleich `pread(iso, base + sector*2048 + off, len)`. Der Baum hat nur 59 Einträge (Root: 7 Dateien, nämlich `default.xbe`, `SysPAL.img`, `FutPAL00`–`04.img`, und 2 Verzeichnisse; `video/` mit 47 und `SaveGameIcons/` mit 3), und das Spiel liest ausschließlich 2048-ausgerichtet. Nichts muss extrahiert werden, und das Disc-Image bleibt unverändert.
* **Extrahiertes Verzeichnis als Alternative:** funktioniert ebenso, wenn die `.img` unverändert bleiben. Achtung: Ohne `.img` würde jeder Pfad auf eine lose Datei unter `D:\` zurückfallen. *Korrektur (Verifikation):* Die frühere Aussage, ohne `sysPAL.img` werde keine Sprache gesetzt, stimmt nicht. Ohne jedes Archiv endet `game+0xE8` bei 0 (PAL), und `FUN_000A26A0` setzt die Sprache aus `XGetLanguage` (3.2). Mit dem korrigierten Parser entpackte Archive funktionieren also ohne Patch der Sprachwahl, sofern der Host Groß-/Kleinschreibung und `/`/`\` tolerant auflöst. Es bleiben zwei Unterschiede zum Original. Erstens wird `sysPAL.img` im Spiel nicht gemountet, also werden auch der String-Manager und `FUN_000D9B20` im `sysPAL`-Zweig nicht durchlaufen; `FUN_000A26A0` holt das per `FUN_000D9CF0` (Singleton) nach. Zweitens öffnet jeder Asset-Zugriff einen eigenen Host-Handle.
* **Mods:** Lose Dateien können archivierte nicht überschreiben, weil das Archiv zuerst durchsucht wird. Ein Override-Mechanismus gehört in den Host, z. B. als Hook auf `FUN_0013FE10`, der für Override-Pfade 0 liefert.

### 5.3 `T:`, `U:` und `Z:` auf dem PC, Ort der Spielstände

* `\Device\Harddisk0\partition1\` wird auf `%LOCALAPPDATA%\FuturamaPC\hdd\partition1\` abgebildet. XAPI legt dann selbst `TDATA\46490002\` und `UDATA\46490002\` an. Die Spielstände landen in `...\UDATA\46490002\<hash>\`, in derselben Struktur wie auf der Xbox-HDD, sodass ein Import aus xemu-/FATX-Dumps 1:1 möglich ist (sofern die Signaturschlüssel passen, 4.3).
* `Z:` wird auf `%LOCALAPPDATA%\FuturamaPC\cache\` gelegt oder gar nicht angelegt; das Spiel benutzt es nicht.
* FATX-Regeln, die die VFS einhalten oder wenigstens nicht verletzen sollte: Dateinamen höchstens 42 Zeichen, case-insensitiv mit erhaltener Schreibweise. Die längsten Namen hier sind 12 Hex-Zeichen und `options_saveimage.xbx`.

### 5.4 Was byte-identisch sein muss

| Was | Warum |
|---|---|
| alle `.img`-Dateien | Header mit absoluten Offsets, Lesen über Rohoffsets (3.1, 3.3) |
| `video/*.xmv` | Packet-Parser der XMV-Bibliothek, solange sie nicht ersetzt ist |
| `SaveGameIcons/*.xbx` | werden 1:1 in Sicherungen kopiert, nur für Dashboard und Kompatibilität |
| XBE-Abschnitt `$$XTIMAGE` | wird zu `TitleImage.xbx` |
| Spielstand-Container | Die Signatur deckt `0x000`–`0xFEB` ab. `XboxSignatureKey` muss über alle Sitzungen konstant bleiben, für Xbox-Kompatibilität wie in 4.3 abgeleitet |

### 5.5 Stolperfallen (Checkliste)

1. `XGetDiskClusterSizeA("U:\")` darf nie 0 liefern, sonst Division durch null in `_cinit` (`0x20EFDD`).
2. Freien Platz für `U:` unter 4 GiB melden (Fehler mit `high | low`, `0xE0CB5`).
3. `FutPAL05.img` und `sysNTSC.img` sauber mit „nicht gefunden“ beantworten, `sysPAL.img` case-insensitiv finden.
4. Reads nie fehlschlagen lassen, auch nicht beim Lesen über das Dateiende hinaus; sonst erscheint der Disc-Fehlerbildschirm in einer Endlosschleife.
5. APCs nur in alertable Waits zustellen. Das Spiel nutzt dafür ausschließlich `SleepEx(0, TRUE)`, und zwar in NiUDSStream (`0x13F4C0`, `0x13F590`) und in der Save-Schicht (`0xE0030`, vf14 des Save-Objekts).
6. `XGetParentalControlSetting` muss 0 liefern, `partition1` muss einen Cluster von `0x4000` melden, sonst geht es ins Dashboard.
7. `XboxHardwareInfo->Flags & 8` gelöscht lassen, sonst legt XAPI `D:` nicht an und überspringt NV-Abfragen (`0xF9342`, `0xF9B52`).
8. `FUN_000F9804` patcht vor allen Datei-Operationen Kernel-Code, wenn `XboxKrnlVersion.Build` ∈ {3944, 4034, 4039, 4817, 4818} (`0xF68`, `0xFC2`, `0xFC7`, `0x12D1`–`0x12D2`). Eine andere Build-Nummer melden, z. B. 5838; dann kehrt die Funktion sofort zurück.

### 5.6 Später (Dekompilierung)

* Eine C++-Implementierung von `NiUDSStream` (`0x13F250`, `0x13F4C0`, `0x13F590`, `0x13F660`, `0x13F7B0`, `0x13F870`, `0x13F8F0`, `0x13FB10`, `0x13FC40`, `0x13FE10`, `0x13FE70`) ersetzt den ganzen Asset-Pfad. Dann reichen `pread` auf ISO oder Verzeichnis; Fibers und APCs entfallen.
* Die Save-Schicht `0xE0040`–`0xE1500` kann danach direkt Host-Pfade nutzen. Das Container-Format inklusive Signatur sollte erhalten bleiben, damit Xbox-Sicherungen importierbar bleiben.

---

## 6. Offene Punkte

* Die genaue Ableitung von `XboxSignatureKey` ist nicht aus dem Binary belegt (4.3). Am besten mit einer echten Xbox-Sicherung prüfen.
* ~~Ob die APC von `WriteFileEx` beim Speichern im normalen Ablauf rechtzeitig zugestellt wird, hängt davon ab, ob in dieser Phase ein NiUDSStream gepollt wird.~~ Durch die Verifikation weitgehend geklärt: Die Save-Schicht hat mit `0xE0030` (vf14, `SleepEx(0, TRUE)`) einen eigenen alertable Poll. Er wird z. B. aus `FUN_000E52F0` und `FUN_000E4CB0` aufgerufen (2.4). Offen ist nur noch, wie oft das zur Laufzeit geschieht.
* Die NiFile-/CRT-Pfade (`FUN_00109310`, `0x151290`/`0x151390`, `0x107980`) sind vermutlich tot, das ist aber nicht zur Laufzeit bestätigt. Der Host sollte `fopen` trotzdem über `CreateFileA` bedienen können.
* Die Bedeutung der einzelnen Felder von `Options.dat` (`0x214` Byte) und `Unlock.dat` (24 × 5 Byte) ist noch nicht entschlüsselt.
* `Language\FileNames.ldb` (Lokalisierung von Dateinamen, `0x2B246`): unklar, welche Dateien darüber laufen. Das betrifft vermutlich `DVDTrailer<Sprache>-PAL.xmv`.
* Die Werte für `XGetAVPack`/NV 9 sind nach XDK-Konvention gedeutet und nicht belegt.

---

## 7. Reproduzierbarkeit (Skripte in `analysis/loader/scripts/`)

| Skript | Belegt |
|---|---|
| `files_strings.py` → `files_strings.out` | alle pfadartigen Strings (ASCII und UTF-16) mit Rohreferenzen (1.2) |
| `files_xref.py <va…>` | Rohreferenzen auf Globals und Strings (z. B. `2d7ad8`, `30edd4`, `2edb3c`) |
| `files_disasm.py <start> <end>` | Disassembly mit String- und Funktionsnamen, z. B. `f92c0 f9470`, `e0c60 e0ce0`, `20efc0 20eff6`, `a26a0 a2760` |
| `files_imgheader.py` | Archiv-Header direkt aus dem ISO, spielgetreu geparst → `files_<img>_gamelist.txt` (3.1, 3.4) |
| `files_futimg_diff.py` | Ausmaß des Fehlers in `futimg.py`. Seit der Korrektur von `orig/retail_eu/*_files.txt` (20:22) meldet es überall 0; die alten Zahlen reproduziert `verify_files_r2_img.py` |
| `files_archives.py` | Verzeichnisse je Archiv, Sprachen, Duplikate, Ausrichtung |
| `files_resolve_literals.py` | 452 Code-Pfade in den Archiven aufgelöst, die übrigen klassifiziert |
| `files_peek.py <img> <pfad>` | Hexdump einzelner Archivdateien aus dem ISO (`build.txt` = „1.25“, `.ldb` = „0BDL“) |
| `files_kthunks.py` | Kernel-Slots, Ordinale, Namensabgleich, Push-Anzahlen (2.3) |
| `files_xapi_rets.py` | `ret N` der XAPI-Funktionen (2.2) |
| `files_udsopen_sites.py` | alle Aufrufstellen des NiUDSStream-Konstruktors |
| `verify_files_r2_*.py` | Zweitprüfung, siehe Abschnitt „Verifikation“ |

---

## Verifikation

Unabhängige Zweitprüfung (Review-Lauf 2, 2026-10-04). Alle Prüfungen laufen gegen das Rohbinary `orig/retail_eu/default.xbe` (capstone), gegen den Ghidra-Export und gegen das ISO (nur lesend). Die Skripte liegen unter `scripts/verify_files_r2_*.py`:

| Skript | Prüft |
|---|---|
| `verify_files_r2_common.py` | Hilfsfunktionen: rohe E8-Aufrufsuche, Dword-Referenzen, Thunk-Tabelle, Dekompilat-Index |
| `verify_files_r2_kslots.py` | jede Referenz auf jeden der 111 Kernel-Slots aus allen Code-Abschnitten, Eigentümerfunktion, Abgleich mit den ghidra-xbe-Namen der aufrufenden Funktionen |
| `verify_files_r2_insn.py` | 45 Befehlsbehauptungen (Adresse → Mnemonic/Operand, Befehlsgrenze), 0 Abweichungen nach Korrektur |
| `verify_files_r2_rets.py` | `ret N` von 31 XAPI-Funktionen plus Push-Anzahl an allen E8-Aufrufstellen |
| `verify_files_r2_infoclass.py` | Argumentlisten aller Aufrufe von `NtCreateFile`, `NtOpenFile`, `NtQueryInformationFile`, `NtSetInformationFile`, `NtQueryVolumeInformationFile` und `NtQueryDirectoryFile` |
| `verify_files_r2_img.py` | XDVDFS-Baum des ISO; eigene Nachbildung von `FUN_0013FC40` (relativ); Nachbildung des alten `futimg.py` (absolut); Header-Größen, Dateizahlen, Ausrichtung, Duplikate, Hexdump `SysPAL.img` 0x4C0–0x520, Inhalt von `build.txt` |
| `verify_files_r2_strings.py` | Inhalt und Code-Referenzen von 49 zitierten String-Adressen |
| `verify_files_r2_globals.py` | Lese- und Schreibzugriffe auf `0x30E598`, `0x30EDD4`, `0x2D7AE4/E8/F0/F8`, `0x30E68C`, `0x2EDB0C`, `0x2EDB88`, `0x30E594` |
| `verify_files_r2_header.py` | XBE-Header, Zertifikat, Abschnittsliste |
| `verify_files_r2_drives.py` | alle Laufwerksbuchstaben-Literale (ASCII, UTF-16, Dword-Immediate) |

### Kernaussagen 1–17

| # | Aussage (Kurzform) | Ergebnis | Beleg der Zweitprüfung |
|---|---|---|---|
| 1 | Spielcode ruft keine `Nt*`-Datei-Funktionen direkt auf | **bestätigt** | Rohsuche: Die Datei-Slots (`0x2935C4`, `0x2935E8`, `0x2935EC`, `0x29360C` usw.) werden nur aus `0xF6987`–`0xF946D` (XAPILIB) und D3D `0x241810`–`0x242160` referenziert. Kein Kernel-Slot wird aus Spielcode unterhalb `0xF3000` referenziert (Ausnahme CRT `0x16xxxx`: Critical Sections, `KeBugCheck`). Der Treffer `0x23BCC6` ist kein Befehl, sondern ein Zufallstreffer in `call 0x23E600` |
| 2 | `CreateFileA` `0xF76E0`, 7 stdcall-Argumente, Root `-3`, `OBJ_CASE_INSENSITIVE`, Dispositionen | **bestätigt** | `ret 0x1C`; 8 von 11 Aufrufstellen mit genau 7 Pushes; Dekompilat `local_24 = 0xFFFFFFFD`, `local_1c = 0x40`, Abbildung 1→2, 2→5, 3→1, 4→3, 5→4 (5 nur mit `GENERIC_WRITE`) |
| 3 | NiUDSStream: erst Archiv, dann `D:\`+Name mit `0x68000000` | **bestätigt** | `0x13F3B2 call 0x13FE10`, `0x13F3F0 push 0x68000000`, `0x13F404 call 0xF76E0`; `"D:\"` `0x2D7AD8` einzige Referenz `0x13F2B5` |
| 4 | `ReadFileEx` in Ringpuffer, 2048-Ausrichtung, Aufrunden | **bestätigt** | `0x13F85F`/`0x13F8CA`/`0x13F792` `and …,0xFFFFF800`; `0x13F847 lea eax,[ecx+0x7FF]`; `[0x2D7AE4] = 0x20000`; `FUN_0013F750(0x80000)` in `FUN_000EE220`; `0x13F1D0` endet mit `ret 0xC` |
| 5 | `NtUserIoApcDispatcher` als ApcRoutine; alertable nur in NiUDSStream | **teilweise korrigiert** | `0xF7625 push [0x2935FC]`, `0xF7630 call [0x2935E8]`, 8 Pushes: bestätigt. Es gibt aber einen **dritten** `SleepEx(0, TRUE)` bei `0xE0030` (Save-Schicht, fehlt in `callgraph.tsv`) → 2.4, 5.5 und 6 korrigiert |
| 6 | `.img`-Verzeichnisfeld relativ; 3654/4058 falsche Pfade | **bestätigt (historisch)** | Nachbildung der alten `futimg.py`-Logik: 90/299/47/462/838/1918 = 3654; Hexdump `0x4CC` = `0x50`, nächster Eintrag `0x51C`. Inzwischen extern korrigiert (20:22), siehe 0 und 3.1. Präzisiert: `&rel+rel−4` ist eine Schranke für den Eintragsbeginn |
| 7 | Archivauswahl und Suchreihenfolge; case-insensitiv | **bestätigt** | `FUN_000D26A0` (Strings `0xD276B`/`0xD27AA`/`0xD2809`/`0xD2849`); `FUN_0013FE10` iteriert `DAT_002D7AF0[0..DAT_002D7AF8)`; `FUN_00170B77` wandelt A–Z in Kleinbuchstaben, danach `__stricmp`; alle Archivnamen sind kleingeschrieben (0 Großbuchstaben) |
| 8 | PAL/NTSC aus `game+0xE8`, nicht `XGetVideoStandard` | **bestätigt, präzisiert** | `0xA26BD mov eax,[esi+0xE8]` / `0xA26C8 cmp eax,ebx(=1)` (kein `cmp [esi+0xE8],1`); 60.0f/50.0f, `'NTSC'`/`'PAL'`; `XGetVideoStandard`/`Flags` fehlen in `functions.tsv`, und keine NV-Abfrage nutzt Index 8 oder 0x103. Die Auswahl von legal/NI_Splash steht im Intro-Code `0x2B65B`/`0x2B81x`, nicht in `FUN_000A26A0` |
| 9 | Sprache nur bei `sysPAL.img` | **falsch → korrigiert** | `FUN_000D9990` hat zwei Aufrufer: `0xD2795` (sysPAL-Zweig) **und** `0xA28F3` in `FUN_000A26A0`, Letzterer bei `[esi+0xE8] == 0` (`0xA28DB`–`0xA28E6`). Der Endwert von `+0xE8` hängt nur von den `Fut*`-Schleifen ab (das `= 1` aus dem `sysNTSC`-Zweig wird immer überschrieben). Switch 3/4/5/6 und `XGetLanguage` = NV 7 sind bestätigt |
| 10 | Archivinhalte, Zahlen, Ausrichtung | **bestätigt** | Größen, Header `0xCF7/0x270B/0x7BF/0x316F/0x6013/0xB003`, Dateizahlen 134/366/78/512/938/2030, Oberverzeichnisse, FutPAL04 423/420/397/413/375; 0 nicht ausgerichtete Offsets, 0 Einträge über das Dateiende hinaus; 452 aufgelöste Literale |
| 11 | Videos lose, 47 Stück, XMV öffnet selbst nichts | **bestätigt** | ISO: 47 `video/*-PAL.xmv`; `FUN_000EE220`: `Video\%s.xmv` → `FUN_0013F250` → `FUN_0013F750(0x80000)` → Read `0x1000` → `FUN_00264360(0, buf, this, 0xEDBA0, 0xEDE60, …)`; kein `CreateFileA`-Aufrufer im XMV-Abschnitt; `FUN_00264677` schließt `+0x7C` nur bei `≠ −1` (`0x264706`) |
| 12 | Container 4096 Byte, HMAC-SHA1 bei `0xFEC` | **bestätigt** | `FUN_000E0210`: `GetFileSize != 0x1000` → 0; `FUN_000E0340`/`FUN_000E03B0`: Update über `len−0x14`, 5 Dwords; `XCalculateSignatureBegin` = `BeginEx(flags, cert.TitleID)` → `XboxSignatureKey` (`mov eax,[0x2935DC]`); ipad `0x36363636`, opad `0x5C5C5C5C`, zweite SHA über 0x54 Byte |
| 13 | Drei Sicherungstypen, Größen, max. 25 | **bestätigt, ergänzt** | Strings bestätigt; Formatstrings `0x2A3344`–`0x2A33AC` in `FUN_000C26E0`; `FUN_000E0D60`: `0x18 < count`; `FUN_000F3A10` → `0x78`; `FUN_000A1860` → `0x214`, beschreibt aber nur 0x210 Byte (4 Byte uninitialisiert, ergänzt in 4.1) |
| 14 | `XGetDiskClusterSizeA("U:\")` als Divisor | **bestätigt, präzisiert** | `0x20EFC0 push 0x2A4D80; call 0xF8081; mov [0x2EDB0C],eax`; Divisionen in eigenen Initialisierern `0x20EFD0`/`0x20F000`/`0x20F030` (Init-Tabelle `0x2D0F84`–`0x2D0F94`); Start `0xF846E` XapiInitProcess vor `0xF84BA` `_cinit`. Für die Division genügt ≠ 0; zusätzlich `FUN_000DFFF0` |
| 15 | `high OR low` beim freien Platz | **bestätigt, erweitert** | `0xE0CB1`/`0xE0CB5`; Vergleich bei `0xE0CD3`/`0xE0CD9` (Bericht nannte nur `0xE0CD9`); derselbe Fehler bei `0xE0DA3` und `0xE0E33` |
| 16 | Laufwerke D/T/U/Z, keine T:/Z:-Nutzung, keine MUs | **bestätigt** | `0xF934C`/`0xF9351` (`\??\D:` → `\Device\CdRom0`), `FUN_000F8F75` (T: `TDATA`, U: `UDATA`, `%08lx`), `0xF93CC`; `IoCreateSymbolicLink` 3 Referenzen; keine `T:\`/`Z:\`-Literale, nur `U:\` (`0x2A4D80`, Immediates `0xE0058`/`0xE0B6A`) und `D:\`/`d:\` |
| 17 | Kein EEPROM-Schreiben; gelesene NV-Indizes | **bestätigt** | Ordinal 29 fehlt in der Thunk-Tabelle; NV-Aufrufe: `push 7` (`0xF6451`), `0x104` (`0xF8565`), `0xA` (`0xF9C4F`), `9` (`0x1707B1`), `XQueryValue(0x11)` (`FUN_000F9B1A`). Dazu kommt `XQueryValue(0xFFFF)` in `FUN_000F6FFA`, aber nur bei nicht-roamable Signaturen mit leerem `XboxHDKey`, also nie im Spiel |

### Weitere geprüfte Einzelaussagen (Auswahl)

| Aussage | Ergebnis |
|---|---|
| Title-ID `0x46490002`, Zeitstempel `0x3F0A8B95`, InitFlags `0x5`, AllowedMedia `0x2`, GameRatings `0`, `cert.SignatureKey` `98c52648…4ae4` | bestätigt (`verify_files_r2_header.py`) |
| `$$XTIMAGE` 0x2800 Byte, Flags `0x38`, Inhalt `XPR0`; kein `$$XSIMAGE`, kein `$$XTINFO` | bestätigt |
| `ret N` aller 31 Funktionen in Tabelle 2.2 | bestätigt (31/31) |
| Slot-, Ordinal- und Namenszuordnung sowie Call-Site-Zahlen in Tabelle 2.3 (z. B. `NtCreateFile` 10, `NtOpenFile` 13, `NtClose` 27, `NtReadFile` 6, `NtWriteFile` 12) | bestätigt; die ghidra-xbe-Namen an den aufrufenden Funktionen stimmen, `0x2935DC`/`0x2935D8`/`0x2935E0` entsprechen den `_exref`-Namen im Dekompilat |
| Informationsklassen 4/0xD/0xE/0x13/0x14/0x22, Volume-Klasse 3 mit 0x18 Byte, Directory-Klasse 1 | bestätigt; „ReturnSingleEntry“ war falsch (Xbox: 10 Argumente mit FileMask und RestartScan), korrigiert in 2.2 und 5.1 |
| `XapiInitProcess`-Bedingungen (`0xF9326`, `0xF933D`, `0xF9375`, `0xF93B3`, `0xF93E8 jae`), `partition1`-Cluster `0x4000` (`FUN_000F92AF` → `FUN_000F8AE2(…, 0x4000)`) | bestätigt |
| `TitleMeta.xbx` nur bei EndOfFile 0, UTF-16 `%lc%ls%lc%ls%ls`, `TitleImage.xbx` per `XLoadSectionByHandle` | bestätigt (Dekompilat `XapiMapLetterToDirectory`) |
| `CopyFileExA`: `MmAllocateSystemMemory(0x10000, 4)` | bestätigt (`0xF7878`–`0xF7894`) |
| `XInitDevices(1, {0x28BF10, 4})`; `MU_Init` über die Klassentabelle `0x28BEF4`, `IoCreateDevice` `\Device\MU_x` | bestätigt |
| `FUN_000E4B00`: `XLaunchNewImageA(NULL, {2, 0, 0x55, n})` | bestätigt |
| Save schreiben/lesen: `CreateFileA`-Flags `0x60000080`/`0x20000080`/`1`, `XCreateSaveGame(…, 4/3, …)`, `WriteFileEx(0x1000, 0xE01A0)` | bestätigt |
| Verzeichnisname der Sicherung: 12 Hex-Zeichen aus einem 48-Bit-Hash (`FUN_000F6544`, mod `0xFFFFFFFFFFC5`) | bestätigt |
| `"anim:"`-Zeilen in `chars/*.ucf` (1143 Treffer, keine `.kf`) | bestätigt |
| `build.txt` in allen 6 Archiven, Inhalt `1.25`; `.ldb`-Magic `0BDL` | bestätigt |
| `DAT_0030EDD4` ohne Schreibzugriff, `DAT_0030E598` in bss mit einer Referenz | bestätigt (`verify_files_r2_globals.py`) |
| `FUN_000F9804`-Builds 3944/4034/4039/4817–4818 | bestätigt (Dekompilat) |
| `NiTCache`-Bedingung bei `0x147619` | Adresse korrigiert auf `0x147618` (`0x147619` ist kein Befehlsanfang) |
| `Test.ldb` nicht auf der Disc | **falsch**: Der Code baut `Language\Test.ldb`, die Datei liegt in SysPAL und FutPAL02 (korrigiert in 1.2) |
| ISO-Root „9 Dateien“ | präzisiert: 7 Dateien und 2 Verzeichnisse (59 Einträge insgesamt bestätigt) |

### Ergänzungen (vom Bericht nicht erfasst)

* Dritter alertable Wait `0xE0030` in der Save-Schicht (vf14, vtable `0x2A4DB0`). Er stellt die `WriteFileEx`-APC zu und erledigt damit den offenen Punkt aus Abschnitt 6 zum größten Teil.
* Nur **ein** Spiel-Thread: `CreateThread` wird nur aus `entry` aufgerufen. Alle Datei-APCs gehen an den Hauptthread, Fibers laufen auf demselben Thread.
* `Options.dat`: 4 uninitialisierte Bytes in den Nutzdaten, deshalb ist die Datei zwischen zwei Läufen nicht reproduzierbar.
* Zweite Division durch die Clustergröße in `FUN_000DFFF0`; `XGetDiskClusterSizeA` liefert 0 auch dann, wenn `U:` sich nicht öffnen lässt.
* Der `high OR low`-Fehler steckt in drei Funktionen (`0xE0CB5`, `0xE0DA3`, `0xE0E33`).
* `XFindFirstSaveGame` hat einen weiteren Aufrufer, `0xE0500` (vf4 des Save-Objekts). Außerdem rufen `WriteFile`, `ReadFile`, `SetFilePointer` und `XGetDiskSectorSizeA` noch Code im inaktiven NiTCache-Bereich auf (`0x160F00`–`0x161520`).
* Das Flag „beschädigt“ (`+0x1D`) wird auch bei fehlender Datei, falscher Größe oder Lesefehler gesetzt. Scheitert `XCalculateSignatureBegin`, folgt in `FUN_000E0340` ein NULL-Zugriff.

### Korrigierte Stellen im Text

Abschnitt 0 (Sprache, `futimg.py`), 1.2 (NiTCache-Adresse, `Test.ldb`), 2.2 (FindFirstFile, Aufruferlisten), 2.3 (`KeDelayExecutionThread`), 2.4 (Warten, Fibers/Threads, Retry), 3.1 (Kinderbereich, Füllbytes, Nachtrag `futimg.py`), 3.2 (Sprache, legal/NI_Splash), 4.1 (`Options.dat`), 4.2 (Flag `+0x1D`), 4.4 (Clustergröße, Freiplatz-Vergleich), 4.5 (`XGetLanguage`), 5.1 (APC-Dispatcher, `NtQueryDirectoryFile`), 5.2 (ISO-Root, extrahierte Archive), 5.5 (Punkt 5), 6 (APC-Punkt), 7 (`files_futimg_diff.py`).

### TSV- und Listen-Dateien

Der Bericht hat keine eigene TSV-Datei erzeugt; `fs_sites.tsv` gehört zu `cpu.md`. Die Listen `scripts/files_<img>_gamelist.txt` sind korrekt: Jeder Pfad stimmt mit der Nachbildung von `FUN_0013FC40` überein (0 Abweichungen), sie wurden nicht geändert. `tools/futimg.py` und `orig/retail_eu/*_files.txt` liegen außerhalb von `analysis/loader/`. Während dieser Prüfung (20:22:56) wurden sie extern auf die relative Semantik umgestellt und sind jetzt byte-identisch mit den Gamelists (`cmp`).

### Offen und unsicher

* Der Name `NtUserIoApcDispatcher` für Ordinal 232 und dessen Verhalten (Statusabbildung) folgen nur aus der Position in der Thunk-Tabelle und der Verwendung als `ApcRoutine`. ghidra-xbe benennt den Slot nicht, weil er nur als Daten referenziert wird.
* Dass die xbmovie-Routine (`FUN_00241C40`) nur über einen Debugger aktiviert wird, ist Vermutung. `FUN_0023EF0D` schreibt indiziert nach `[eax+ecx*4+0x498/0x49C]`, und ob das dasselbe Device-Feld trifft, ist nicht geklärt.
* Nicht einzeln nachgeprüft: DVD-Authentifizierung (`FUN_000F90E6`, CDB `0x5A`/`0xAD`), Z:-Partitionsnummer „3 + Cache-Index“, `RootDirectory`-Handle in `FUN_000F8709`, Verwendung von `XDeleteSaveGame` mit `\??\U:\`.
* Ob `FUN_000D3F40` (Lipsync/`Test.ldb`) in Retail erreichbar ist, ist offen.
* Wie oft vf14 (`0xE0030`) zur Laufzeit aufgerufen wird, hängt davon ab, wie oft `FUN_000D25D0` und `FUN_000E52F0` laufen. Das ist nicht zur Laufzeit beobachtet.
