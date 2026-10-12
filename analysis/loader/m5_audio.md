# M5 – Audio: DSOUND-Vertrag, Soundsystem des Spiels, Audiodaten und Abbildung auf XAudio2

Ziel-Binary: `orig\retail_eu\default.xbe` (Retail EU/PAL). Alle Adressen sind VAs dieses Images. Stand: 2026-10-12.

Grundlage: [`avi.md`](avi.md) (Abschnitt 2 DSOUND, 3 XMV), [`../../docs/loader_design.md`](../../docs/loader_design.md) (E7, E10, 5.x, 8),
[`../../src/INTERFACES.md`](../../src/INTERFACES.md) 4.13, [`../../docs/m4_report.md`](../../docs/m4_report.md). Dieser Bericht vertieft
`avi.md` 2 und korrigiert ihn an mehreren Stellen (Abschnitt 11). Belege stehen an jeder Aussage: Adresse im XBE,
Ausgabe eines Skripts oder der Trace-Lauf. „Vermutung“ markiert, was nicht am Binary, an den Daten oder durch einen
Lauf belegt ist.

Nichts aus `src\` oder `launcher\` wurde geändert. Spieldaten wurden nur gelesen (ISO, In-Memory-Dekodierung); alles
Abgeleitete aus Spieldaten liegt unter `run\analyse_m5\` (git-ignoriert), im Repo stehen nur Skripte und
Zusammenfassungen ohne Audioinhalte.

---

## 0. Kurzfazit

**Für den Host entscheidend (neu gegenüber `avi.md`):**

1. **Takt.** `SoundManager::Update` (Vtable-Slot 1, 0xCBCB0) läuft **pro Logiktick** aus `CApp::vf10` 0xD25D0
   (0xD25F5), nicht pro Bild. Bei `tick_rate = 60` also 60-mal je Sekunde; der Trace-Lauf zählt 2345 Ticks in 39,5 s
   und genau so viele `CommitDeferredSettings`, `DirectSoundDoWork` und Listener-Aufrufe (`m5_trace_level.out`). Fast
   alle DSOUND-Aufrufe fallen innerhalb dieses Updates an.
2. **Lautstärke-Kategorien sind geklärt:** 0 = Effekte (Optionen „Soundlautstärke“, global 0x2E790C),
   1 = Sprache (0x2E7920), 2 = Musik (0x2E7918); Regler 0..0,95 in 0,05-Schritten, Standard 0,75 (0xA1AE0), wirksam
   erst nach dem Laden von `Options.dat` – vorher 1,0 (6.7). **Filme benutzen Kategorie 0 (Effekte)**, nicht die
   Musik (`Movie::Open` 0xEE43D `push edi` = 0). `avi.md` 7 vermutete Musik.
3. **Die Lautstärke-Rechnung ist ungewöhnlich, aber vollständig belegt** (Abschnitt 3.6, `m5_gain_model.out`):
   2D-Stimmen bekommen die mB-Dämpfung **doppelt** (Puffer-Lautstärke plus gleich große Mixbin-Lautstärke) und
   **6 dB DSOUND-Headroom** (0x248A63: 600 mB für Nicht-3D-Voices), 3D-Stimmen weder das eine noch das andere.
   Beim Standardregler 0,75 liegen 2D-Musik/Sprache bei −11 dB, 3D-Effekte ohne Entfernung bei −2,5 dB.
4. **Die Rolloff-Kurve** wirkt genau so (FUN_00249893): Verstärkung 1 bis MinDistance, danach lineare
   Interpolation in der Amplitude über 6 Stützpunkte {1, 0.9, 0.8, 0.5, 0.25, 0} bei Min + (i+1)·(Max−Min)/6,
   jenseits MaxDistance stumm (`DSBCAPS_MUTE3DATMAXDISTANCE` 0x20000 belegt). Damit ist sie exakt als
   `X3DAUDIO_DISTANCE_CURVE` mit 8 Punkten darstellbar.
5. **Hall:** Das Spiel kennt fünf I3DL2-Umgebungen, schaltet aber **nie um**: Der einzige Aufrufer von
   `SetEnvironment` ist das Skript-Native `FsSetReverb`, das in keinem Level-Skript aufgerufen wird
   (`m5_script_sound.out`: 0 Aufrufe), und kein Archiv enthält einen der fünf Umgebungsnamen (`m5_grep_disc.py`).
   Aktiv ist das ganze Spiel über „Padded Cell“ (Init 0xCCDC1). Nur 3D-Stimmen schicken Signal in den Hall
   (Mixbin 10, −6 dB per `DSI3DL2BUFFER.lRoom = −600`, entfernungsgedämpft).
6. **Streaming:** Musik **und die Ambience-Loops** werden gestreamt: Alle Level-Skripte laden sie mit
   `FsLoadSound(name, 1)` (27 Aufrufe), alle übrigen 679 Sounds mit `0` (`m5_script_sound.out`). Musik läuft über `FsPlayMusic`
   (Kategorie 2, Loop), Ambience je nach Level über `FsPlayMusic` oder `FsPlaySound` (Kategorie 0). Die Laufzeit
   bestätigt es (zweiter `DirectSoundCreateStream` beim Laden von Level 1-1). Die Pumpe (0xCB5B0) füllt das letzte Paket mit Nullen auf
   und springt dann an den Anfang: **An jeder Loop-Stelle entsteht eine Stille von 0,5–293 ms** (Tabelle 4.3);
   außerdem fallen die letzten 44 Byte der Daten weg (Größe und Offset werden vermischt). Die `smpl`-Loop-Punkte der
   18 Ambience-Dateien ignoriert das Spiel. Das ist Originalverhalten und muss nicht „repariert“ werden.
7. **Xbox-ADPCM:** Variante „Kopf-Sample + 63 Nibbles, Schritt `((2n+1)·step)>>3`“ ist **bitgleich mit ffmpeg
   `adpcm_ima_xbox`** auf 5 Spieldateien (`m5_adpcm_check.out`); die klassische Bit-Formel weicht um bis zu 28 LSB ab.
8. **Listener:** Das Spiel übergibt Front = −Blickrichtung, Oben = Up der Kamera (0xCBE85, `m5_listener_orient.out`).
   Mit der DS3D-Regel rechts = oben × vorne ergibt das die **richtige Seite, aber vertauschtes Vorne/Hinten**.
   In Stereo hört man das kaum; bei 5.1 kämen Geräusche vor der Kamera aus den hinteren Lautsprechern
   (Entscheidung in 6.5).
9. **Status-Fallen** (ergänzt): Pufferstimmen leben genau so lange, wie `GetStatus` Bit 0 meldet (0xCCA27);
   `Stop` merkt sich die Abspielposition, `Play` setzt dort fort (CMcpxBuffer_Stop 0x24E04E); Streams enden nie von
   selbst (die Pumpe loopt immer); nach `Release` eines Streams gibt das Spiel Paketpuffer und Status-DWORDs sofort
   frei (0xCC107 → 0xCC121) – danach darf der Host nichts mehr schreiben.
10. **XMV (M6):** braucht Callback-Streams (Aufruf auf dem Gast-Thread), `Pause(2)` + `SynchPlayback`,
    `FlushEx(…, ASYNC)` mit sofort fallendem `PLAYING` und `GetStatus` über die Vtable (Abschnitt 5).
11. **Auf diesem Rechner ist das einzige aktive Audio-Ausgabegerät der LG-Fernseher** (Registry `MMDevices`,
    DeviceState 1). Ist er aus, gibt es kein Gerät; XAudio2 kann dann keine Mastering-Voice anlegen. Automatische
    Läufe brauchen deshalb einen **Ausgabe-losen Modus mit Software-Mix** für `capture_wav` (6.8).

**Empfehlung in einem Satz:** DSOUND-HLE auf XAudio2 2.9 mit einer Source-Voice je DSOUND-Puffer/-Stream, der
DSOUND-Pegelrechnung aus Abschnitt 3.6 (exakt, eigene Funktion), X3DAudio nur für die Richtung, einem Hall-Submix
aus dem I3DL2-Preset, Kategorie-Submixes als Messpunkte, allen Gast-Speicherzugriffen auf dem Gast-Thread in
`DirectSoundDoWork` und einem Capture-Tap auf dem Mastering-Voice bzw. einem Software-Mix ohne Gerät.

---

## 1. Methodik und Werkzeuge

Neue Skripte in `analysis\loader\scripts\` (alle nur lesend auf XBE, Ghidra-Export und ISO):

| Skript | Zweck | Ausgabe |
|---|---|---|
| `m5_common.py` | XBE/Capstone, Namen (inkl. vtable-only-Methoden des Soundmoduls), Rohindex aller Befehle, Ghidra-C | – |
| `m5_dump_module.py` | annotierte Disassembly des Soundmoduls 0xC9300..0xCDD00 | `run\analyse_m5\sound_module.lst` |
| `m5_dis.py` | annotierte Disassembly beliebiger Bereiche | stdout |
| `m5_mgr_calls.py` | alle virtuellen Aufrufe auf `CApp+0xF0` (SoundManager) mit Slot | `out\m5_mgr_calls.out` |
| `m5_api_callers.py` | Aufrufer der Sound-Wrapper 0xC96E0..0xC9CB0 | `out\m5_api_callers.out` |
| `m5_x87sym.py` | kleiner symbolischer x87-Auswerter (Listener-Orientierung) | `out\m5_listener_orient.out` |
| `m5_assets.py` | alle 2982 WAVs: RIFF-Walk (korrekt und wie das Spiel), Format, `smpl`, Stream-Entscheidung, Loop-Lücke; WAVs in NIFs (Zahl); Umgebungsnamen in Skripten | `out\m5_assets.out`, `run\analyse_m5\wav_inventory.tsv` |
| `m5_nif_audio.py` | 3367 in NIFs eingebettete WAVs (Format, Dauer) | `out\m5_nif_audio.out`, `run\analyse_m5\nif_wav_inventory.tsv` |
| `m5_grep_disc.py` | Byte-Suche über alle Archivdateien (ohne Extraktion, Groß/Klein egal) | stdout |
| `m5_script_sound.py` | Sound-Natives im Bytecode der 22 Level-Skripte: `FsLoadSound`-Dateien mit `bStream`, Abspiel-Native, Aufrufzahlen | `out\m5_script_sound.out` |
| `m5_adpcm.py` | Referenz-Codec Xbox-ADPCM (Decoder in 2×2 Varianten, Encoder für synthetische Tests) | – |
| `m5_adpcm_check.py` | Referenz-Decoder gegen ffmpeg an 5 Spieldateien, synthetischer Rundweg | `out\m5_adpcm_check.out` |
| `m5_levels.py` | RMS/Spitze/Clipping/Loop-Naht von 1300 Dateien je Kategorie | `out\m5_levels.out`, `run\analyse_m5\levels.tsv` |
| `m5_i3dl2.py` | I3DL2-Tabelle 0x2D6818 und XAudio2-Native-Parameter der 5 Presets | `out\m5_i3dl2.out` |
| `m5_gain_model.py` | Pegelmodell Spiel + DSOUND (Referenzwerte für Host-Tests) | `out\m5_gain_model.out` |
| `m5_trace_summary.py` | Auswertung des Trace-Laufs | `out\m5_trace_level.out` |
| `m5_capture_check.py` | Werkzeug für die Prüfung ohne Ohren (Lokalisieren, Pegel, Tempo, SNR, Lücken, Clipping); `--selftest` mit synthetischen Signalen: PASS | `out\m5_capture_check_selftest.out` |

Die Ausgaben unter `scripts\out\` sind wie die der `verify_*`-Skripte git-ignoriert (`.gitignore`: `out/`); jedes
Skript erzeugt seine Ausgabe neu (ISO-Pfad wie in der INI, Laufzeiten 1–20 min für die ISO-Scans).

**Trace-Lauf:** vorhandener Build `build\m4-final-rel` mit `[debug] trace = snd,vid`, `[audio] dummy_streams = 1`
und `tools\input_scripts\m4_newgame.txt` (Intro, Frontend, „Neues Spiel“, Level 1-1, 39,5 s; Konfiguration
`run\analyse_m5\trace_level\futurama.ini`). Er liefert den ersten Aufruf je Aufrufstelle mit Argumenten und die
Aufrufzahlen. Einschränkung: Die Stubs liefern `NULL`-Puffer und `GetStatus = 0`, jede Pufferstimme endet deshalb
nach einem Tick; Pufferzahlen sind dadurch überhöht (geloopte Emitter werden ständig neu angelegt).

---

## 2. Der DSOUND-Vertrag

### 2.1 Gemeinsames

* Alle 40 Einstiege sind `__stdcall`, `this` zuerst (`avi.md` 2.2, `avi_callconv.out`). `apply`/`dwApply`: 0 = sofort
  (`DS3D_IMMEDIATE`), 1 = verzögert bis `CommitDeferredSettings` (`DS3D_DEFERRED`); DSOUND prüft nur Bit 0
  (`CDirectSoundVoice_SetI3DL2Source` 0x247DB5, `CDirectSound_SetOrientation` 0x2487B1).
* **Einheiten:** Lautstärken in mB (1/100 dB, ≤ 0; −10000 = stumm). Frequenz in Hz, 0 = Originalrate des Puffers
  (`CDirectSoundVoice_SetFrequency` 0x247C42), intern `XAudioCalculatePitch` = round(4096·log2(f/48000)) (0x246ED1, 0x24C022).
  Entfernungen in Spieleinheiten. Die Hardware-Dämpfung je Mixbin ist ein 12-Bit-Wert in 1/64 dB, auf 0xFFF
  (63,98 dB) begrenzt (FUN_0024C3CA) – −10000 mB werden also zu −64 dB, nicht zu „−∞“.
* **Spielseitiger Ablauf je Tick:** `SoundMgr::Update` setzt den Listener (3 Aufrufe, verzögert), aktualisiert alle
  Stimmen (`SoundMgrBase::vf1` 0xC95F0 → je `SoundData::vf7` 0xCCB80 → je Stimme `SoundVoice::vf11` 0xCC5E0), ruft
  `CommitDeferredSettings` und `DirectSoundDoWork` (0xCBE95, 0xCBE9A).

### 2.2 Einstiege, Argumente, Semantik und Häufigkeit

Spalte „Takt“: **T** = jeden Tick (Listener), **V** = jeden Tick je aktiver Stimme, **E** = Ereignis.
„Trace“ = erste Argumente aus `m5_trace_level.out`.

| Einstieg | Argumente vom Spiel (Beleg) | Semantik, die der Host liefern muss | Takt |
|---|---|---|---|
| `DirectSoundCreate(guid, ppDS, outer)` | `(0, &mgr+0x48, 0)` 0xCCD41; XMV je Sync | Singleton mit Referenzzähler (`avi.md` 2.3) | E (Start) |
| `IDirectSound_Release(ds)` | SoundMgr-Dtor 0xCBC7F; XMV | nur Zähler, Gerät nie abbauen | E |
| `IDirectSound_DownloadEffectsImage(ds, img, size, loc, ppDesc)` | `size 25256`, `loc = {0, 1}` (I3DL2-Index 0, Crosstalk-Index 1) 0xCBF67 | `S_OK`; Image ignorieren; Datei muss aber öffnbar sein (`avi.md` 2.1) | E (Start) |
| `DirectSoundUseFullHRTF()` | 0xCCD64 | wählt in DSOUND den vollen HRTF-Algorithmus (0x246DF1); Host: Option „HRTF“ (6.5) | E (Start) |
| `IDirectSound_SetDopplerFactor(ds, 2.0, 0)` | sofort, 0xCCD73 | wirkungslos, alle Geschwindigkeiten sind 0 (3.7) | E (Start) |
| `IDirectSound_SetDistanceFactor(ds, 0.2, 1)` | 0xCCD98 | wirkt in DS3D nur auf Doppler | E (Start) |
| `IDirectSound_SetRolloffFactor(ds, 1.0, 1)` | 0xCCDA7 | wirkt nur ohne Rolloff-Kurve (FUN_00249893 Zweig 0x24997E); das Spiel setzt immer eine | E (Start) |
| `IDirectSound_SetPosition(ds, x, y, z, 1)` | Init (0,0,0); jeden Tick Kameraposition 0xCBE42 | Listener-Position, verzögert | T |
| `IDirectSound_SetVelocity(ds, 0,0,0, 1)` | 0xCBE5C (Nullvektor 0x2F90BC) | ignorierbar | T |
| `IDirectSound_SetOrientation(ds, fx,fy,fz, tx,ty,tz, 1)` | Front = −Zeile 0, Oben = Zeile 1 der Kamera-Weltrotation 0xCBE85; Trace: front (−1,0,0), top (0,1,0) | Listener-Basis; rechts = oben × vorne (FUN_002497AC) | T |
| `IDirectSound_SetI3DL2Listener(ds, &preset, 1)` | Init 0x2D6884 (Padded Cell) 0xCCDC7; `SetEnvironment` 0xCBFBB..0xCC007 | Hall-Parameter (12 Felder `DSI3DL2LISTENER`), verzögert | E (nur Init beobachtet) |
| `IDirectSound_CommitDeferredSettings(ds)` | 0xCBE95 | alle verzögerten 3D-Werte gemeinsam anwenden | T |
| `DirectSoundDoWork()` | 0xCBE9A; XMV in `GetNextFrame`/`CloseDecoder` | Host: Ereignisse anwenden (Status-DWORDs, Callbacks), 7 | T |
| `IDirectSound_SynchPlayback(ds)` | nur XMV | gemeinsamer Start aller mit `Pause(2)` vorbereiteten Streams (5) | E |
| `DirectSoundCreateBuffer(&desc, &pBuf)` | `DSBUFFERDESC{0x18, flags, 0, wfx, NULL, 0}` 0xCC296; Flags 0x100 (2D) bzw. 0x60110 (3D); wfx = `SoundData+0x59` (ADPCM) oder `+0x34` | eine Quelle; Format übernehmen (2.3) | E (je Play einer Pufferstimme) |
| `IDirectSoundBuffer_SetBufferData(buf, p, n)` | Spielspeicher `SoundData+0x48`, Länge `+0x14` 0xCC2AF; `(0,0)` vor dem Freigeben 0xCC06F/0xCB597 | DSOUND **kopiert nicht** (Flag 0x80000000, 0x248B1E); Host kopiert/dekodiert sofort; Cursor auf 0 (0x24E102) | E |
| `IDirectSoundBuffer_Play(buf, 0, 0, flags)` | `flags = loop ? 1 : 0` (`DSBPLAY_LOOPING`) 0xCC72E, Fortsetzen 0xCC3DB/0xCC45D/0xCC548 | ab gespeichertem Cursor weiterspielen (0x24DF9E); Status sofort `PLAYING` | E |
| `IDirectSoundBuffer_Stop(buf)` | `SoundVoice::vf8` 0xCB575 (Pause-Menü, Stop) | anhalten, **Cursor merken** (0x24E04E), `GetStatus` sofort 0 | E |
| `IDirectSoundBuffer_GetStatus(buf, &st)` | jede Pufferstimme jeden Tick 0xCCA27 (Bit 0); Dtor-Schleife 0xCC0BF (≠ 0) | 1 = spielt, 1+4 = spielt geloopt, 2 = pausiert (0x24DB55); 0 nach Ende/`Stop` | V |
| `IDirectSoundBuffer_SetVolume(buf, mB)` | 0xCC91D, Formel 3.6 | Puffer-Lautstärke (minus Headroom, 3.6) | V |
| `IDirectSoundBuffer_SetFrequency(buf, Hz)` | `(int)(rate · pitch)`, pitch 0..4 (0xCA110), 0 ⇒ Originalrate | Tonhöhe | V |
| `IDirectSoundBuffer_SetMixBins(buf, &mb)` | 2D: `{2, {0,v0},{1,v1}}` je Tick 0xCC9D3; 3D: `{6, {6,0},{8,0},{7,0},{9,0},{10,0},{3,0}}` einmal 0xCC35A | Ausgabematrix (3.6) | V (2D) / E (3D) |
| `IDirectSoundBuffer_SetPosition/SetVelocity(buf, x,y,z, 1)` | Position des Besitzerknotens (`owner+0xA0`) oder feste Position, Velocity immer 0 (0xCC6DF, 0xCC7F7) | Emitter-Position, verzögert | V (3D) |
| `IDirectSoundBuffer_SetMinDistance/SetMaxDistance(buf, d, 1)` | Voice +0x30/+0x34, Standard 5/80 (0xCA1E5), per Skript geändert (Trace 30/40) | Kurvenbereich, verzögert | V (3D) |
| `IDirectSoundBuffer_SetRolloffCurve(buf, 0x2EAD3C, 6, 0)` | sofort, bei der Erzeugung 0xCC36D | DSOUND speichert **den Zeiger** (0x247D72), Inhalt ist konstant | E |
| `IDirectSoundBuffer_SetI3DL2Source(buf, &src, 1)` | `{lDirect 0, lDirectHF 0, lRoom −600, lRoomHF 0, flRoomRolloff 0, Obstruction 0/0, Occlusion 0/0}` 0xCC9DF..0xCCA0E; bei 3D-**Streams** mit `buf = NULL` | Hall-Send −6 dB (3.6) | V (3D) |
| `IDirectSoundBuffer_Release(buf)` | Voice-Dtor 0xCC0F2 | Quelle freigeben | E |
| `DirectSoundCreateStream(&desc, &pStream)` | `DSSTREAMDESC{flags 0 bzw. 0x60010, 10, wfx = SoundData+0x34, NULL, NULL, NULL}` 0xCC1C3; XMV: 2 Pakete + Callback (5) | Objekt mit XMediaObject-Vtable bei +0 | E |
| `XMediaObject::Process(s, &pkt, NULL)` (Vtable +0x10) | Pumpe 0xCB6B7, Paket 3.5 | Paket annehmen, `*pdwStatus = E_PENDING` **synchron**; Warteschlange voll ⇒ `0x88780032`, Status unberührt (0x247676) | E (je Paket) |
| `IDirectSoundStream_Pause(s, n)` | 0 = Start 0xCC711/0xCC3B9/0xCC500, 1 = Stopp 0xCB563, 2 = auf Sync warten (XMV) | 0x24BEBD; 3 unbenutzt | E |
| `IDirectSoundStream_SetVolume/SetFrequency/SetMixBins` | wie Puffer, 0xCC8C4/0xCC8BA/0xCC9C0; Film-Lautstärke 0xEE471 | wie Puffer | V |
| `IDirectSoundStream_SetPosition/Velocity/Min/MaxDistance` | 0xCC7AB..0xCC7E9 (nur 3D-Streams, im Trace nie) | wie Puffer | V |
| `IDirectSoundStream_SetRolloffCurve` | 0xCC33C | wie Puffer | E |
| `IDirectSoundStream_FlushEx(s, 0, 0, 1)` | nur XMV `CloseDecoder` | asynchron stoppen, Pakete mit `0x80004004` beenden (0x24773E, 0x24BA46) | E |
| `XMediaObject::Release` (Vtable +0x04) | Voice-Dtor 0xCC107, Film 0xEE47D | letzter Verweis: Stimme synchron abbauen, danach keine Schreibzugriffe | E |
| `XMediaObject::AddRef/GetStatus/Discontinuity` | nur XMV | 5 | E |

Nicht benutzt: `SetHeadroom`, `SetMixBinHeadroom`, `SetPitch`, `SetLFO`/`SetEG` (`DSENVELOPEDESC`), `SetFilter`
(`DSFILTERDESC`), `SetLoopRegion`, `Lock`, Positionen und Notifications (`avi.md` 2.2). Es gibt also **keine
Hüllkurven und keine Filter** zu emulieren.

### 2.3 Strukturen

| Struktur | Layout, wie das Spiel sie füllt | Beleg |
|---|---|---|
| `WAVEFORMATEX` (18 Byte) | Kopie des `fmt `-Chunks an `SoundData+0x34`, `cbSize` immer 0x12 (falsch), bei ADPCM `wSamplesPerBlock` aus der Datei an +0x46 | 0xCBA42..0xCBA4F |
| Xbox-ADPCM-Format (20 Byte, unausgerichtet an `SoundData+0x59`) | `wFormatTag 0x69`, `nChannels`, `nSamplesPerSec`, `nAvgBytesPerSec = 36·ch·ch/64` (**falsch**, `avi.md` 2.4.3), `nBlockAlign = 36·ch`, `wBitsPerSample 4`, `cbSize 2`, `wSamplesPerBlock 64` | 0xCBA5B..0xCBAA0 |
| `DSBUFFERDESC` (0x18) | `dwSize 0x18`, `dwFlags` = (3D ? 0x60010 : 0) + 0x100, `dwBufferBytes 0`, `lpwfxFormat`, `lpMixBins NULL`, `dwInputMixBin 0` | 0xCC243..0xCC292 |
| `DSSTREAMDESC` (0x18) | `dwFlags (3D ? 0x60010 : 0)`, `dwMaxAttachedPackets 10`, `lpwfxFormat = SoundData+0x34`, `lpfnCallback NULL`, `lpvContext NULL`, `lpMixBins NULL` | 0xCC18A..0xCC1C3 |
| `XMEDIAPACKET` (0x18) | `pvBuffer = voice+0x60 + i·P`, `dwMaxSize = P` (immer volle Paketgröße), `pdwCompletedSize NULL`, `pdwStatus = &voice[0x7C + 4i]`, `hCompletionEvent 0`, `prtTimestamp NULL` | 0xCB680..0xCB6B7 |
| `DSMIXBINS` | `{DWORD dwMixBinCount, DSMIXBINVOLUMEPAIR* }`, Paar = `{DWORD dwMixBin, LONG lVolume}`; DSOUND legt die Lautstärke **je Bin-Nummer** ab (FUN_00247080) | 0xCC93C..0xCC9D3, 0xCC2C9..0xCC31A |
| `DSI3DL2LISTENER` (48 Byte) | lRoom, lRoomHF, flRoomRolloffFactor, flDecayTime, flDecayHFRatio, lReflections, flReflectionsDelay, lReverb, flReverbDelay, flDiffusion, flDensity, flHFReference | Tabelle 0x2D6818, `m5_i3dl2.out` |
| `DSI3DL2BUFFER` (36 Byte) | lDirect, lDirectHF, lRoom, lRoomHF, flRoomRolloffFactor, Obstruction{lHFLevel, flLFRatio}, Occlusion{lHFLevel, flLFRatio} | 0xCC9DF..0xCCA0E; DSOUND kopiert 9 DWORDs (0x247DB5) |
| `DS3DBUFFER`/`DS3DLISTENER` | nicht als Struktur übergeben, nur Einzelaufrufe | 2.2 |
| `DSENVELOPEDESC`, `DSFILTERDESC` | nicht benutzt | 2.2 |

**Flag-Bits** (am Binary nachgeprüft, wo angegeben): 0x10 = 3D (DSOUND prüft überall `flags & 0x10`, z. B.
0x248A63, FUN_0024C3CA); 0x20000 = stumm jenseits MaxDistance (FUN_00249893 `test byte [flags+2], 2`); 0x40000
(Vermutung: `LOCDEFER`, ohne Wirkung für den Host); 0x100 (Vermutung: `CTRLPOSITIONNOTIFY`, nie genutzt).

### 2.4 Status-Semantik

| Objekt | Wert | Bedeutung | Beleg |
|---|---|---|---|
| Puffer | 0 | spielt nicht (Ende, `Stop`, noch nie gestartet) | CMcpxBuffer_GetStatus 0x24DB55 |
| Puffer | 1 / 1+4 | spielt / spielt geloopt | ebd. |
| Puffer | 2 | pausiert (nur über `Pause`, vom Spiel nie benutzt) | ebd. |
| Stream | 0x00000001 | `READY`: Warteschlange hat Platz (sonst lehnt `Process` mit 0x88780032 ab) | CMcpxStream_GetStatus 0x24B3CE, 0x247676 |
| Stream | 0x00010000 | `PLAYING` (XMV-Close wartet, bis es fällt) | ebd., 0x264677 |
| Stream | 0x00020000 / 0x00040000 / 0x00080000 | pausiert / pausiert wegen Starvation / Hüllkurve (Vermutung für die letzten beiden) | ebd. |
| Paket | 0x8000000A | `E_PENDING`, gesetzt von `Process` | 0x24B1D9, Spiel vergleicht 0xCB5D4 |
| Paket | 0 | fertig abgespielt (`*pdwCompletedSize = Größe`) | XDK; Spiel prüft nur ≠ E_PENDING |
| Paket | 0x80004004 | `E_ABORT` = verworfen (Flush/Release) | CMcpxStream_Flush 0x24BA46 |

---

## 3. Das Soundsystem des Spiels

### 3.1 Klassen und Felder

**SoundManager** (0x7C Byte, Vtable 0x2A3B80, Basis 0x2A3978, an `CApp+0xF0`; Konstruktor 0xCCD00, Basis 0xCA310):

| Offset | Inhalt | Beleg |
|---|---|---|
| +0x04 | `NiNode*` (Refcount), im Basis-Ctor angelegt, `vf8` setzt ihn | 0xCA37B, 0xCA500 |
| +0x08 | Liste aller `SoundData` (next +0x04, prev +0x08) | 0xCA194..0xCA1CA |
| +0x0C | Liste aller Stimmen (next +0x4C, prev +0x54) | 0xC9E5E..0xC9EA4 |
| +0x10..+0x24 | `NiTPrimitiveArray` der beim Pausieren gemerkten Stimmen-IDs | 0xCA348..0xCA358, 0xCACA0 |
| +0x28/+0x2C, +0x30 | `vf6`/`vf7` (Init 1.0/1.0 bzw. 0; kein virtueller Aufruf über `CApp+0xF0` gefunden, `m5_mgr_calls.out`; keine DSOUND-Wirkung) | 0xCA3F0, 0xCA410 |
| +0x34 / +0x38 / +0x3C | Kategorie-Lautstärke 0 Effekte / 1 Sprache / 2 Musik (Init 1.0; Setter vf10/vf12/vf14, Getter vf9) | 0xCA3C0..0xCA3C6, 0xC9650 |
| +0x40, +0x44 | `vf19`/`vf22` (ohne Wirkung auf DSOUND) | 0xC9690, 0xC9680 |
| +0x48 | `IDirectSound*` | 0xCCD27 |

**SoundData** („Sample“, 0x70 Byte, Vtable 0x2A3BE8, Basis 0x2A3920; Ctor 0xCCA70/0xCA160):

| Offset | Inhalt |
|---|---|
| +0x0C | Liste der Stimmen dieses Samples (next an Voice+0x48) |
| +0x10 | Datei-Objekt (`NiUDSStream`, 0x13F250) während des asynchronen Ladens |
| +0x14 | Puffergröße (Bytes), bei Streams 10 Pakete |
| +0x18 | Datengröße des `data`-Chunks |
| +0x1C | `nSamplesPerSec` (für `SetFrequency`) |
| +0x20 | Name (Kopie des angeforderten Pfads; Cache-Schlüssel ohne Endung, 0xCCF21..0xCCF42) |
| +0x24 / +0x25 / +0x26 / +0x27 | geladen / mono (= 3D-fähig) / beim nächsten Update löschen / Stream |
| +0x28 / +0x2C | MinDistance 5.0 / MaxDistance 80.0 |
| +0x30 | Offset des `data`-Chunks in der Datei |
| +0x34 | `WAVEFORMATEX` (2.3) |
| +0x48 | Datenzeiger (Spielspeicher) |
| +0x4C | Paketgröße |
| +0x58 / +0x59 | ADPCM-Flag / Xbox-ADPCM-Format |

**SoundVoice** (0xBC Byte, Vtable 0x2A3B18, Basis 0x2A38B8; Ctor 0xCB240/0xC9DA0, Dtor 0xCC030):

| Offset | Inhalt | Setter (Beleg) |
|---|---|---|
| +0x08 | Besitzer-`NiAVObject*` für 3D-Stimmen (Position = `owner+0xA0`, Weltposition) | `vf6` 0xCC4B0 |
| +0x0C | feste 3D-Position | `vf7` 0xCC410 |
| +0x18 | `SoundData*` | Ctor |
| +0x1C / +0x1D / +0x1E / +0x1F / +0x20 / +0x21 / +0x22 | DS-Objekt angelegt / Start ausstehend / 3D / angehalten / fertig / „geändert“ / Loop | 3.3 |
| +0x24 | Lautstärke 0..1 (geklemmt) | `vf13` 0xC9430 |
| +0x28 | Panorama (−1 links … +1 rechts, nicht geklemmt) | `vf14` 0xC9F50 |
| +0x2C | Tonhöhe 0..4 (geklemmt) | `vf16` 0xCA110 |
| +0x30 / +0x34 | Min-/MaxDistance (aus SoundData) | `vf20` 0xC9FA0 / `vf21` 0xC9FB0 |
| +0x38 / +0x3C / +0x44 | „Fade-In/Fade-Out/vergangene Zeit“ (3.4: wirkungslos) | 0xC9CB0 |
| +0x60 | Paketpuffer des Streams (10 · P Byte) | 0xCC1E5 |
| +0x64 | eigenes Datei-Objekt des Streams (mit eigener Fiber) | 0xCC206 |
| +0x68 | Stream-Fiber läuft | 0xCB6E4 |
| +0x6C | Stimmen-ID (Zähler 0x2D67E0, Rückgabe der Sound-API) | 0xC9DD0 |
| +0x70 | Kategorie 0/1/2 | `vf4` 0xC9F30 |
| +0x74 / +0x78 | `IDirectSoundStream*` / zweites XMO (immer 0) | 0xCC1AE |
| +0x7C..+0xA0 | 10 Paket-Status-DWORDs | 0xCB5C3 |
| +0xA4 | `IDirectSoundBuffer*` | 0xCC286 |
| +0xAC / +0xB0 | Paketgröße / Leseposition des Streams | 0xCC1CE, 0xCB668 |

### 3.2 Laden

* `Snd_GetSound(name, bStream)` (0xC97D0, 144 Aufrufstellen) → `SoundMgr::vf4` 0xCCEE0: Cache-Suche über
  `_strnicmp(name, data->name, strlen(name) − 3)`; sonst neues `SoundData` + `LoadByName` 0xCCC00, das ein
  Datei-Objekt mit Fiber anlegt und `ParseWavFile` (0xCB960) **asynchron über mehrere Ticks** laufen lässt
  (`SoundData::vf7` 0xCCB80 pumpt die Lade-Fiber, solange `+0x24` = 0).
* Zweiter Weg: WAVs aus NIF-Dateien (`NiUDSFileObject`) über `SoundMgr::vf5` 0xCCE40 → `ParseWavMemory` 0xCB720
  (synchron, nie Stream). Auf der Disc sind das 3367 WAVs in 137 NIFs: je Level ein `soundcache/levelX-Y.nif` mit den
  Level-Effekten und je Level und Sprache `ingame_dlg_<sprache>.nif` mit den Dialogen (`m5_nif_audio.out`).
* Der Parser liest nur `fmt ` und `data`; andere Chunks (`smpl`, `LIST`) werden übersprungen. Die Chunk-Schleife
  rundet ungerade Größen **nicht** auf (0xCBA15); auf der Disc betrifft das keinen `data`-Chunk (0 Abweichungen,
  `m5_assets.out`).
* Stream nur, wenn `bStream` **und** `dataSize ≥ nAvgBytesPerSec` aus dem rohen `fmt ` (mindestens 1 s); Paketgröße
  `P = rate · nBlockAlign · 3 / 10`, auf `nBlockAlign` abgerundet (0xCBAAB..0xCBAE7). PCM16 stereo 22,05 kHz: 26460 Byte
  = 0,3 s. Xbox-ADPCM mono 22,05 kHz: 238140 Byte = 19,2 s je Paket (`avi.md` 2.4.3).

### 3.3 Lebenszyklus einer Stimme

1. **Anlegen und Abspielen** über die Sound-API (Tabelle 3.8). Das setzt nur Flags: `+0x1E` (3D), `+0x22` (Loop),
   Besitzer/Position und `+0x1D` (Start ausstehend) – `vf5`/`vf6`/`vf7` 0xCC390/0xCC4B0/0xCC410.
2. **Erstes Update nach dem Laden** (`vf11`, 0xCC611): `CreateDsObject` 0xCC150 legt Puffer oder Stream an.
   Ist die Stimme 3D, das Sample aber nicht mono (`SoundData+0x25` = 0), wird sie **sofort beendet** – der eben
   angelegte DSOUND-Puffer wird ohne `Play` wieder freigegeben (0xCC631..0xCC646). Sonst bei 3D: Min/Max/Position/
   Velocity, dann `Pause(0)` (Stream) bzw. `Play(0, 0, loop)` (Puffer) (0xCC70B..0xCC72E).
3. **Jedes weitere Update** (nur wenn geladen, nicht angehalten, nicht fertig): 3D-Werte, Lautstärke, Frequenz,
   Mixbins bzw. I3DL2-Send; bei Puffern danach `GetStatus` – **Bit 0 nicht gesetzt ⇒ Stimme fertig** (0xCCA27..0xCCA3F,
   außer im Starttick). Stream-Stimmen werden nie von selbst fertig; danach `FileObj::Wait` = ein Pumpendurchlauf
   (0xCCA56).
4. **Löschen** im selben Durchlauf, wenn `+0x20` gesetzt ist (FUN_000C9560): Dtor 0xCC030 = `Stop`, bei Puffern
   `SetBufferData(0,0)` und **Warten, bis `GetStatus` 0 liefert** (0xCC0B3..0xCC0D0), bei Streams Warten auf das Ende
   der Pumpen-Fiber (0xCC090..0xCC0A7); dann `IDirectSoundBuffer_Release`, `stream->Release` (Vtable), Freigabe des
   Paketpuffers (0xCC121) und der Stimme.
5. **Anhalten/Fortsetzen:** `Snd_PauseVoice`/Pause-Menü → `vf8` 0xCB540 (`Stop` bzw. `Pause(1)`, `+0x1F = 1`);
   `Play` auf eine angehaltene Stimme setzt fort (`Play` bzw. `Pause(0)`) **ohne** neues DSOUND-Objekt. Vor Filmen und
   im Pause-Menü hält `FUN_000CACA0` alle Stimmen an und merkt sich ihre IDs; `FUN_000CA580` setzt sie fort
   (Aufrufer: Film 0xA243C/0xA23D8, 0xA250A; Pause-Menü 0xA8234/0xA81E5).
6. **Doppeltes `Play`** auf eine laufende, nicht angehaltene Stimme legt beim nächsten Update ein **zweites**
   DSOUND-Objekt an und überschreibt `+0xA4`/`+0x74`, ohne das erste freizugeben (0xCC286, 0xCC1AE). Ein geloopter
   Puffer liefe dann weiter, ohne dass das Spiel ihn je wieder anspricht (Originalverhalten; Host: zählen und loggen).

**Grenzen und Prioritäten:** Es gibt keine Prioritäten und kein Stehlen von Stimmen. Die einzige Grenze sind 8
Stimmen je Sample bei `Snd_Play3DAt` (`SoundData::vf4` 0xCB3E1); alle anderen Wege legen unbegrenzt an.
Auf der Xbox begrenzt die Hardware (Vermutung nach XDK: 256 Voices, davon 64 mit HRTF); der Host braucht keine
Grenze, sollte aber die Höchstzahl gleichzeitiger Quellen messen und loggen (9.3).

### 3.4 Lautstärke, Tonhöhe, Panorama, Fades je Tick

Rechnung in `vf11` (0xCC843..0xCC91D):

```
v  = GetCategoryVolume(voice+0x70) * voice.volume          // 0..1
v  = FUN_000C9480(v)                                       // "Fade", siehe unten
mB = v >= 1 ? 0 : v <= 0 ? -10000 : (int)((1 - v) * -1000)  // linear 0..-10 dB, darunter stumm
SetFrequency((int)(SoundData.rate * voice.pitch));  SetVolume(mB)
2D: SetMixBins({2, {0, (int)(mB*min(1, 1+pan))}, {1, (int)(mB*min(1, 1-pan))}})
```

* `v` liegt als float32 im Speicher (0xCC862), `_ftol` (0x167080) schneidet zur Null hin ab: 0,9 → −100 mB,
  0,1 → −899 mB. Für bitgenaue Host-Tests ist das in `m5_gain_model.py` nachgebildet.
* Panorama wirkt nur, wenn `mB < 0`; bei voller Lautstärke gar nicht (`avi.md` 2.4.5).
* **Fades der Engine sind wirkungslos:** `Snd_FadeOut` 0xC9CB0 setzt nur `+0x3C`; FUN_000C9480 rechnet im
  Ausblendzweig mit `+0x38` (immer 0) statt `+0x3C` und multipliziert im Einblendzweig mit Dauer/Zeit statt
  Zeit/Dauer (0xC94A7, 0xC94EA). Beides ändert die Lautstärke nicht hörbar und beendet die Stimme nicht. Ein- und
  Ausblendungen der Skripte (`FadeInSound`, `CrossFade` in den `.dbg`-Symbolen) laufen über
  `FsSetVolumeSound` je Tick.
* Der Leerlauf im Frontend setzt ab einem Zählerstand von 57 (Feld +0x7C, vermutlich Sekunden) die
  Musik-Kategorie auf Regler − 1/180 (0x22BAE, praktisch unhörbar) und stellt sie bei 60 vor dem Wechsel in den
  Attract-Film wieder her (0x22C0B).

### 3.5 Streaming (Musik, Ambience, ggf. Sprache)

* **Pumpe** `SoundVoice::StreamPump` 0xCB5B0, ausgeführt in der Fiber des Stimmen-Datei-Objekts (`vf0` 0xCB6E0,
  FUN_0013F1A0); **ein Durchlauf je Tick** über `FileObj::Wait` aus `vf11` (0xCCA56).
* Für jeden der 10 Slots mit Status ≠ `E_PENDING`: Datei auf `dataOffset + pos` setzen, `P` Bytes lesen (am Ende nur
  den Rest, Rest des Pakets mit Nullen), `pos = (pos + gelesen) mod (dataSize − dataOffset)`, `Process(&pkt, NULL)`
  mit `dwMaxSize = P` (0xCB5E0..0xCB6B7).
* **Folgen (Originalverhalten):**
  * Jeder Stream **loopt**; es gibt kein Ende. Gestoppt wird nur über `Pause(1)` + Freigeben der Stimme.
  * Am Loop-Punkt spielt das **genullte Paketende**: eine Stille von `(P − (dataSize − dataOffset) mod P)` Byte
    (Tabelle 4.3, bis 293 ms).
  * Die Pumpe vergleicht `pos + dataOffset + P` mit der **Größe** `+0x18` statt mit dem Datenende: Die letzten
    `dataOffset` Byte (44 bei PCM) werden nie gespielt.
  * Bei einem ADPCM-Stream (Sprache mit `bStream = 1`, 3.9) ist ein Paket 19,2 s lang; eine kurze Zeile wird also
    mit bis zu 19 s Stille aufgefüllt und danach wiederholt, bis das Spiel die Stimme stoppt. Im Trace-Lauf kam
    dieser Weg nicht vor (nur 2 PCM-Streams).
* **Start/Stopp:** `Pause(0)` beim ersten Update bzw. beim Fortsetzen, `Pause(1)` beim Anhalten (0xCB563); kein
  `FlushEx` durch das Spiel. Beim Freigeben ruft der Dtor `stream->Release` und gibt den Paketpuffer sofort frei
  (0xCC107, 0xCC121) – der Host muss die Paketdaten deshalb **beim `Process` kopieren** und darf nach `Release` keine
  Status-DWORDs mehr schreiben.

### 3.6 Pegelmodell (Spiel + DSOUND, vollständig)

DSOUND speichert je Stimme `lVolume − Headroom` (CDirectSoundVoice_SetVolume 0x24719B). **Headroom ist 600 mB für
jede Nicht-3D-Stimme und 0 für 3D** (0x248A63, `flags & 0x10`). Je belegtem Mixbin rechnet FUN_0024C3CA die Dämpfung

```
att(bin) = -( lVolume - headroom + mixbinVolume[bin] ) - 3D-Terme(bin)        // mB, auf 0..63,98 dB begrenzt
3D-Terme: Bins 0..5 und LFE: dist + kegel;   Bins 6,7 (XTLK vorne): dist + kegel + vorne(+0x28) + I3DL2-direkt
          Bins 8,9 (XTLK hinten): dist + kegel + hinten(+0x2C) + I3DL2-direkt;  Bin 10 (I3DL2): dist + kegel + lRoom
```

Ein Kanal einer Stimme bedient `Mixbinzahl / Kanalzahl` Bins der Reihe nach: mono 2D → Bins 0 und 1, stereo 2D →
links Bin 0, rechts Bin 1, mono 3D → alle 6 Bins (FUN_0024C3CA, `uVar6`).

| Fall (Regler/Kategorie · Stimme = v) | links | rechts | Beleg |
|---|---|---|---|
| 2D, v = 1,0 (erster Start ohne `Options.dat`), Mitte | −6,0 dB | −6,0 dB | `m5_gain_model.out` |
| 2D, v = 0,95 (Reglermaximum), Mitte | −7,0 dB | −7,0 dB | ebd. (mB −50) |
| 2D, v = 0,75 (Standard), Mitte | −11,0 dB | −11,0 dB | ebd. |
| 2D, v = 0,75, Panorama −1 | −8,5 dB | −11,0 dB | ebd. |
| 2D, v = 0,5 | −16,0 dB | −16,0 dB | ebd. |
| 3D, v = 0,75, innerhalb MinDistance | −2,5 dB (direkt) | Hall-Send −8,5 dB, LFE −2,5 dB | ebd. |
| Film (XMV-Stream, Standard-Mixbins 0/1 zu 0 mB), Kategorie 0 = 0,75 | −8,5 dB | −8,5 dB | 0xEE441..0xEE471, Standard-Mixbins 0x2638AC |

**Entfernung** (FUN_00249893, Kurve 0x2EAD3C, `SetRolloffCurve(…, 6, sofort)` bei der Erzeugung):
Gewinn 1 bis MinDistance; danach linear in der Amplitude zwischen 1 (bei Min) und `curve[i]` bei
`Min + (i+1)·(Max−Min)/6`; ab Max −10000 mB (Flag 0x20000). Umrechnung `2000·log10`. Die Kurve ist so gewählt, dass
das erste Sechstel flach bleibt. Beispiel Min 5/Max 80: 1,0 bis 17,5; 0,9 bei 30; 0,8 bei 42,5; 0,5 bei 55;
0,25 bei 67,5; 0 bei 80. Als `X3DAUDIO_DISTANCE_CURVE` (Skalierer = Max):
`(0, 1) (Min/Max, 1) ((Min+s)/Max, 1,0) ((Min+2s)/Max, 0,9) … (1, 0)` mit `s = (Max−Min)/6` (Zahlen für 5/80 und
30/40 in `m5_gain_model.out`).

**Vorne/hinten** (FUN_00249AE5): Leistungsaufteilung zwischen XTLK vorne (Bins 6,7) und hinten (8,9) nach Azimut
und Elevation, nahe am Hörer zur Mitte gezogen. **Links/rechts** entsteht auf der Xbox nicht über Pegel, sondern
über das HRTF-Filterpaar (CFullHrtfSource_GetHrtfFilterPair 0x249D60, Tabellen ab 0x251300, Azimut in 3/6/12°-
Schritten, Elevation 6°) und den Crosstalk-Effekt aus `dsstdfx.bin` auf dem DSP. Vermutung (Reihenfolge der
DSOUND-Standard-Mixbins 0x26388C = {6, 8, 7, 9, 10}): Kanal-Paar 0/1 = linkes Ohr, 2/3 = rechtes Ohr.

**Nicht bestimmt:** die Standard-Headroom der Mixbins im APU (Byte-Tabelle, die `CMcpxAPU_SetMixBinHeadroom`
0x24AB76 liest) und die Pegel der DSP-Effekte (I3DL2, Crosstalk). Beides verschiebt nur absolute Pegel bzw. das
Verhältnis 2D zu 3D; Abhilfe in 6.6 und 9.

### 3.7 3D-Listener und Emitter

* **Listener je Tick** (0xCBCB0): Position = `cam+0x48` mit `cam = [[0x2DABC4]+0x8C]`, Basis aus
  `[cam+0x30]+0x70` (Weltrotation, 3 Zeilen à 16 Byte; Zeile 0 = Blickrichtung, Zeile 1 = oben, vgl.
  `m3d_interpolation.md` 3.7 `NiCamera::vf39`). Übergeben werden **Front = −Zeile 0**, Oben = Zeile 1
  (`m5_listener_orient.out`). Ohne Kamera gilt die Einheitsmatrix 0x2D7780: Front (−1,0,0), Oben (0,1,0) (Trace).
  Geschwindigkeit = Nullvektor 0x2F90BC.
* **Emitter:** Position des Besitzers (`owner+0xA0`) oder feste Position; Geschwindigkeit immer 0 (0xCB500 bzw.
  0x2F90BC). Doppler ist damit wirkungslos.
* **Händigkeit:** DSOUND bildet rechts = oben × vorne (FUN_002497AC) und Azimut aus vorne/rechts (FUN_00249E94).
  Für eine NetImmerse-Kamera (rechtshändig, rechts = Blick × oben, Zeilen 0/1/2 = Blick/oben/rechts) ergibt
  oben × (−Blick) = Blick × oben = rechts: **links/rechts stimmen, vorne und hinten sind vertauscht**. Folge auf der
  Xbox: Geräusche vor der Kamera werden als „hinten“ behandelt (Bins 8/9, HRTF für hinten).

### 3.8 Sound-API des Spiels und Skript-Natives

Wrapper um `CApp+0xF0` (`m5_api_callers.out`, 644 Aufrufstellen):

| Wrapper | Wirkung | Kategorie | Stellen |
|---|---|---|---|
| `Snd_GetSound(name, bStream)` 0xC97D0 | laden/Cache | – | 144 |
| `Snd_Play2D_Sfx(data, loop)` 0xC9910 | 2D-Stimme | 0 | 81 |
| `Snd_Play2D_Music(data, loop)` 0xC9970 | 2D-Stimme | 2 | 7 |
| `Snd_Play2D_Speech(data, loop)` 0xC99D0 | 2D-Stimme | 1 | 4 (darunter die Sprachwarteschlange 0xD96EE) |
| `Snd_Play3DAt(data, pos, loop)` 0xC9A30 | 3D an fester Position, max. 8 je Sample | 0 | 33 |
| `Snd_Play3DOnNode(data, nodeName, loop)` 0xCAA10 / `Snd_Play3DAttached(data, node, loop)` 0xCAAF0 | 3D am Knoten (Position folgt dem Knoten) | 0 | 1 (Skript) / 10 |
| `Snd_StopVoice(id)` 0xC9A80, `Snd_StopAll()` 0xC9AB0, `Snd_StopAllOf(data)` 0xC9AE0 | `Stop` + fertig | – | 85/1/1 |
| `Snd_PauseVoice(id)` 0xC9B10, `Snd_ResumeVoice(id, loop)` 0xC98E0 | anhalten/fortsetzen | – | 1/2 |
| `Snd_SetVolume/SetPitch/SetPan(id, f)` 0xC9BE0/0xC9B40/0xC9B70 | Felder der Stimme | – | 13/5/1 |
| `Snd_SetMaxDistance/SetMinDistance(id, f)` 0xC9C10/0xC9C60 | Kurvenbereich (Max ≥ Min erzwungen) | – | 82/2 |
| `Snd_IsVoiceAlive(id)` 0xC9870, `Snd_HasVoices(data)` 0xC9830, `Snd_GetDuration(data)` 0xC98A0 | Abfragen | – | 107/4/1 |
| `Snd_SetEnvironmentByName(name)` 0xC96E0 | Hall-Preset per Name | – | 1 (Skript) |

`Snd_GetDuration` (SoundData `vf17` 0xCB290) rechnet ADPCM mit 0,5 Byte je Sample statt 36/64 und überschätzt die
Dauer um 12,5 %. Zwei Aufrufe übergeben einen `SoundData*` statt einer Stimmen-ID (`Snd_SetPitch` 0x33451,
`FsSetVolumeSound` vor `FsPlayMusic` in `level1-2`) und bleiben ohne Wirkung.

**Skript-Natives** (Dispatcher FUN_000B0C40, Fall = Native − 3, Namen aus der Symboltabelle der `.dbg`-Skripte mit
Index − 1): `FsLoadSound` → `Snd_GetSound(name, bStream aus dem Skript)` (0xB414F), `FsPlaySound`/`FsPlaySFX` →
`Play2D_Sfx`, `FsPlay3DSFX` → `Play3DOnNode`, **`FsPlayMusic` → `Play2D_Music(data, 1)`** (0xB4064, immer Loop),
`FsPlaySpeech` → `Play2D_Speech`, `FsStopSound`, `FsStopAllSounds`, `FsSetVolumeSound`, `FsSetPitchSound`,
`FsSetPanSound`, `FsSetRadiusSound` (Max), `FsSetMinRadiusSound`, `FsSetReverb` → `SetEnvironmentByName`.
Der Bytecode legt die Argumente als Konstanten ab (Befehl = 4 Byte Kopf `[Flags][Zeile u16][Opcode]` + Operanden;
0x09 String, 0x08 Konstante, 0x13 Native-Aufruf, 0x0B/0x05 globale Variable schreiben/lesen; `m5_script_sound.py`).
Über alle 22 Level-Skripte: `FsLoadSound` 706-mal, `FsPlaySFX` 266, `FsPlay3DSFX` 150, `FsPlaySound` 125,
`FsPlayMusic` 24, `FsPlaySpeech` 0, `FsSetReverb` 0. `bStream = 1` steht genau bei den Musikstücken und den
Ambience-Dateien `Sewers`, `1-3_Ambience`, `2-2_Ambience`, `2-2_Machines`, `2-3_Alien`, `3_Lava`; in Level 1-2/1-3
spielt die Ambience über `FsPlayMusic` (Kategorie 2), in Level 2-2 über `FsPlaySound` (Kategorie 0).

### 3.9 Sprache, Dialoge, Menüstimmen

* Lokalisierte Pfade baut das Spiel aus dem Sprachnamen (`Sounds\Localized\%s\…`, 0xF0580; Menüstimmen
  `sounds/localized/%s/menu/…`, 0x26BF0). Nur `level2-1.ful` enthält 2 fest englische Pfade.
* Ingame-Dialoge kommen aus `levels/<level>/ingame_dlg_<sprache>.nif` (NIF-WAVs, synchron geladen).
* Die Sprachwarteschlange (Einträge mit Vtable 0x2A4450: Play 0xD96C0 = `Play2D_Speech(data, 0)`, Stop 0xD9720,
  Pause 0xD9740, Dauer 0xD9770) wird über `Speech_Play` 0xD8720 befüllt; dieser lädt je nach Quelle mit
  `GetSound(pfad, 1)` (Stream, 0xD88D6), aus einer NIF oder einem Paket. Welche Zeilen tatsächlich als ADPCM-Stream
  laufen, ist statisch offen (9.3).

### 3.10 Hall-Umgebungen

`SetEnvironment(0..4)` (SoundManager-Slot 22, 0xCBFA0) wählt Padded Cell, Room, Living Room, Stone Room, Concert Hall
aus der Tabelle 0x2D6818 (24 Standard-I3DL2-Presets, `m5_i3dl2.out`). Aufrufer ist nur `Snd_SetEnvironmentByName`
(„PADDED ROOM“ … „OPERA“) aus `FsSetReverb`. Kein Level-Skript ruft `FsSetReverb` auf (Bytecode,
`m5_script_sound.out`: 0 von 22 Skripten), und keine Archivdatei enthält einen der Namen (Groß/Klein egal,
`m5_grep_disc.py`), obwohl Skripte Strings im Klartext speichern. Auch im Trace-Lauf kam nur der Init-Aufruf vor.
**Damit gilt im ganzen Spiel „Padded Cell“** (DecayTime 0,17 s, lRoom −1000, lRoomHF −6000, Reflections −1204, Reverb +207): ein kurzer,
dumpfer Raumeindruck nur auf 3D-Geräuschen.

---

## 4. Audiodaten auf der Disc

### 4.1 Lose WAVs in den Archiven (`m5_assets.out`)

Außerhalb der sechs `.img`-Archive liegt kein Ton auf der Disc außer den Tonspuren der 47 XMV-Filme (Xbox-ADPCM
stereo, `avi.md` 3.3; Dateiliste `orig\retail_eu\disc_files.txt`).

| Format | Dateien | MB | Minuten |
|---|---:|---:|---:|
| Xbox-ADPCM mono 22050 Hz | 2837 | 70,1 | 94,0 |
| Xbox-ADPCM mono 44100 Hz | 79 | 1,8 | 1,2 |
| PCM16 stereo 22050 Hz | 36 | 351,8 | 66,5 |
| Xbox-ADPCM stereo 22050 Hz | 18 | 2,6 | 1,7 |
| Xbox-ADPCM stereo/mono 48000 Hz | 5 + 4 | 4,3 | 1,5 |
| PCM16 mono 22050 / PCM8 mono 11025 / PCM16 stereo 44100 | je 1 | 46,6 | 6,9 |

* Kategorien: Musik 19 (18 PCM16 stereo + `03 - sewers.wav` mono), Ambience 19 (`sounds/levelN/ambient/`, PCM16 stereo,
  `sewers.wav` mit 44,1 kHz), Sprache 2028 (5 Sprachen, je 375–423 Dateien, 14,8–16,6 min; fast alles ADPCM mono
  22,05 kHz, einzelne Stereo- und 48-kHz-Dateien), Level-Effekte 802, Menü 19, Fußschritte 35, Waffen 33.
* Chunks: alle haben `fmt ` und `data`; **18 Ambience-Loops haben einen `smpl`-Chunk** mit einer Schleife über die
  ganze Datei, nur `3_lava.wav` schleift bis Sample 800757 von 1262775. Das Spiel ignoriert `smpl` (3.2).
  2 Dateien haben `LIST`, 1 Datei (`misc12.wav`, PCM8) einen ungeraden Chunk hinter `data`.
* Keine Abweichungen: `nAvgBytesPerSec` in den Dateien korrekt, `data` immer ein Vielfaches von `nBlockAlign`,
  ADPCM immer 36·ch/64, RIFF-Größe passt, der Chunk-Walk des Spiels findet überall `data`.

### 4.2 WAVs in NIF-Dateien (`m5_nif_audio.out`)

3367 eingebettete WAVs in 137 NIFs, alle Xbox-ADPCM: 2951 mono 22,05 kHz (79 min), 391 mono 44,1 kHz, 24 mono
48 kHz, 1 stereo. `soundcache/levelX-Y.nif` (je Level 69–151 Effekte, 22 Level) und `levels/…/ingame_dlg_<sprache>.nif`.

### 4.3 Stream-Entscheidung und Loop-Lücken

Jede Musik- und Ambience-Datei erfüllt die Stream-Bedingung. Lücke am Loop-Punkt nach 3.5 (`m5_assets.out`, neu
berechnet mit `dataSize − dataOffset`):

| Datei (Auswahl) | Länge | Lücke |
|---|---:|---:|
| `music/01 - front-end.wav` | 113,6 s | 103,4 ms |
| `music/01 - planet express.wav` (Level 1-1) | 185,2 s | 186,5 ms |
| `music/12 - track end battle level 5.wav` | 245,4 s | 287,5 ms |
| `music/12 - track swamp race.wav` | 204,9 s | 8,1 ms |
| `level3/ambient/3-4_ambience.wav` | 57,6 s | 293,2 ms |
| `level4/ambient/4-1_village.wav` | 56,1 s | 1,0 ms |

Musikstücke haben ohnehin 0–20 s Stille am Ende (`m5_levels.out`), dort fällt die Lücke kaum auf; die
Ambience-Loops sind dagegen nahtlos gemastert (Lead/Tail 0 ms) – dort ist die Lücke auf der Xbox hörbar.

### 4.4 Pegel des Materials (`m5_levels.out`, 1300 Dateien)

| Kategorie | n | RMS Median | RMS p10..p90 | Spitze Median | Dateien mit Vollaussteuerung |
|---|---:|---:|---|---:|---:|
| Musik | 19 | −17,1 dBFS | −22,3..−13,2 | −0,9 | 5 |
| Ambience | 19 | −24,3 | −34,8..−16,7 | −6,3 | 1 |
| Effekte (lose, Stichprobe) | 250 | −16,2 | −25,0..−10,2 | 0,0 | 118 |
| Effekte (soundcache, Stichprobe) | 421 | −17,1 | −29,1..−11,3 | −0,8 | 144 |
| Sprache deutsch (lose / NIF) | 397 / 140 | −16,1 / −16,2 | −21,0..−13,4 | −0,1 / 0,0 | 129 / 62 |
| Menü / Fußschritte | 19 / 35 | −16,5 / −27,4 | | −3,0 / −5,8 | 2 / 1 |

Das Material ist heiß gemastert (Spitzen bei 0 dBFS). Mehrere gleichzeitige 3D-Effekte (−2,5 dB) übersteuern einen
Fließkomma-Mix ohne Reserve; der Host braucht Headroom oder einen Limiter (6.6).

### 4.5 Xbox-ADPCM

Blockaufbau je Block (`nBlockAlign = 36·ch`): je Kanal 4 Byte Kopf (int16 Prädiktor, uint8 Schrittindex,
uint8 reserviert), dann 8 Runden mit je 4 Byte je Kanal (8 Nibbles, unteres zuerst). Ausgabe je Kanal und Block:
**64 Samples = Kopf-Sample + die ersten 63 Nibbles**; das 64. Nibble wird dekodiert, aber nicht ausgegeben. Schritt
`diff = ((2·(n & 7) + 1)·step) >> 3`, Vorzeichen Bit 3, Klemmen auf int16, Index ±(Tabelle) auf 0..88.
Diese Variante ist bitgleich mit ffmpeg auf `button.wav`, `footsteps/gravel1.wav` (44,1 kHz), `menu/theme22.wav`
(stereo), `reanimator.wav` (48 kHz), `localized/german/menu/voice/6zb1-01.wav` (`m5_adpcm_check.out`). Ob der
Xbox-Chip genauso rundet, ist nicht belegt (Abweichung der Bit-Formel höchstens 28 LSB, unhörbar).

---

## 5. Was XMV von DSOUND braucht (für M6)

Belege: `XMVDecoder_EnableAudioStream` 0x2647C6, Callback 0x264775, `GetNextFrame` 0x264962, `CloseDecoder`
0x264677, Film-Player 0xEE3E3..0xEE47D.

1. **Stream mit Callback:** `DSSTREAMDESC{flags (Spiel 0), dwMaxAttachedPackets 2, wfx, lpfnCallback 0x264775,
   lpvContext = Decoder, lpMixBins = NULL bzw. Tabelle 0x26C938 bei Mehrkanal-Spuren}`. Format aus dem XMV-Kopf:
   `0x69`, 2 Kanäle, 44,1 bzw. 22,05 kHz, hier korrekt `nAvgBytesPerSec = 36·ch·rate >> 6`, `cbSize 2`,
   `wSamplesPerBlock 64`.
2. **Callback** `stdcall(pStreamContext, pPacketContext, dwStatus)`, `ret 0xC`: nur bei `dwStatus == 0` und
   `pPacketContext ≠ 0` rechnet XMV den A/V-Versatz `decoder+0xC8 = (QPC − Start)/ms − pPacketContext`. Auf der Xbox
   ruft DSOUND ihn aus dem DPC. **Im Host darf er nur auf dem Gast-Thread laufen (E10)**: aus `DirectSoundDoWork`
   (XMV ruft es in `GetNextFrame` und `CloseDecoder`) per `core::CallGuest`.
3. **Pakete:** `pvBuffer` zeigt in den XMV-Paketpuffer, `dwMaxSize` = Größe des Tonteils, `pdwCompletedSize`
   und `pdwStatus` in Decoder-Arrays (+0x150/+0x140), fünftes Feld (Union `hCompletionEvent`/`pContext`) =
   Zeitstempel in ms nur für die Sync-Spur – nach XDK reicht DSOUND es als `pPacketContext` an den Callback –, `prtTimestamp`
   NULL (0x264A95..0x264AE3). XMV pollt den Status ebenfalls auf `E_PENDING` (0x264F9F ff.)
   und gibt das XMV-Paket erst danach zurück. `Process` muss also synchron `E_PENDING` setzen.
4. **Start:** vor dem ersten `Process` je Stream `Pause(2)`; nach dem ersten Bild `DirectSoundCreate` +
   `SynchPlayback` + `Release` – alle wartenden Streams starten gleichzeitig (0x264A81; 0x264E0B/0x264E1B/0x264E26).
5. **Ende:** `Discontinuity` (Vtable +0x14) am Datenende bzw. für abgeschaltete Spuren.
6. **Schließen:** `FlushEx(s, 0, 0, 1)` (asynchron), dann Schleife `GetStatus` (Vtable +0x0C) mit `Sleep(1)` +
   `DirectSoundDoWork`, **bis Bit 0x10000 fällt**, dann `Release`. Der Host muss nach `FlushEx` spätestens im
   nächsten `DoWork` `PLAYING` löschen und alle Pakete mit `0x80004004` abschließen.
7. **Spielseite:** `GetAudioStream` liefert einen AddRef-Zeiger; der Film-Player ruft `IDirectSoundStream_SetVolume`
   (Kategorie 0, einmal bei `Open`) und `stream->Release` über die Vtable.

**Für die geplante XMV-HLE mit FFmpeg** (`avi.md` 3.5) folgt daraus: Die DSOUND-HLE soll eine interne Schnittstelle
anbieten, die einen **vom Host gespeisten Stream** als gültiges Gast-Objekt (XMediaObject-Vtable bei +0, Refcount)
erzeugt, PCM vom Host annimmt, die abgespielte Sample-Position als Audio-Uhr liefert und über dieselben C-Einstiege
(`SetVolume`, Vtable-`Release`) steuerbar ist. M5 liefert außerdem `SynchPlayback`, `Pause(2)`, `FlushEx`,
Callback-Streams und `Discontinuity` funktionsfähig, damit beide M6-Varianten (Original-XMV über Host-DSOUND oder
XMV-HLE) möglich bleiben.

---

## 6. Abbildung auf XAudio2, X3DAudio und Hall

### 6.1 Graph

```
 Source-Voices (eine je DSOUND-Puffer bzw. -Stream, Originalformat; ADPCM vorher zu PCM16 dekodiert)
   2D Kat. 2 (Musik, Ambience) ───────────► Submix "Musik"   (Kanäle = Ausgabe) ─┐
   2D Kat. 1 (Sprache)        ───────────► Submix "Sprache"                     ├─► Mastering-Voice (Gerät 2.0/5.1)
   2D/3D Kat. 0 (Effekte)     ───────────► Submix "Effekte"                     │     └ Effektkette: [Limiter] → [Capture-Tap]
   Film (XMV, M6)             ───────────► Submix "Film"                        │
   3D-Stimmen, mono Send      ───────────► Submix "Hall" (1 Kanal ein, XAudio2-Reverb 100 % nass, Ausgabe = Gerät) ─┘
```

* Die Kategorie kennt die DSOUND-API nicht. Sie kommt aus einer **Probe auf `SoundVoice::CreateDsObject` 0xCC150**
  (`FUT_PROBE`, thiscall ohne Argumente, Prolog `mov eax, fs:[0]` = 6 kopierbare Byte): `ecx` = Stimme →
  Kategorie (+0x70), 3D (+0x1E), Stream (`[+0x18]+0x27`), Name (`[+0x18]+0x20`), ID (+0x6C). Die Probe hinterlegt
  das als „nächstes DSOUND-Objekt“; `DirectSoundCreateBuffer/Stream` innerhalb desselben Aufrufs übernehmen es.
  Objekte ohne Markierung (XMV) gehen in „Film“.
* Die Kategorie-Submixe sind **Messpunkte** (Pegel je Kategorie, Stems für die Prüfung) und optionaler Host-Abgleich.
  Die eigentlichen Kategorie-Lautstärken rechnet das Spiel selbst in `SetVolume` ein (3.4) und speichert sie im
  Spielstand; der Launcher soll sie nicht doppelt anbieten (6.9).

### 6.2 Puffer

* `DirectSoundCreateBuffer`: Format merken (`wFormatTag` 1 oder 0x69, Kanäle, Rate; `nAvgBytesPerSec`/`cbSize`
  ignorieren). Source-Voice mit PCM16 (bei ADPCM) bzw. dem Originalformat (PCM8 kann XAudio2 direkt),
  `MaxFrequencyRatio ≥ 4` (Tonhöhe bis 4, 0xCA110; 48000·4 = 192 kHz < `XAUDIO2_MAX_SAMPLE_RATE`). Senden an die
  Kategorie-Submix; 3D zusätzlich an „Hall“. Voices je (Kanäle, Rate) poolen.
* `SetBufferData(p, n)`: Daten sofort kopieren bzw. dekodieren (4.5); Cursor = 0. `(0,0)`: Daten freigeben.
* `Play(…, DSBPLAY_LOOPING)`: `XAUDIO2_BUFFER` mit `PlayBegin = Cursor`, `LoopCount = XAUDIO2_LOOP_INFINITE`
  (Loop über die ganze Datei, `smpl` ignorieren), dann `Start`. Status sofort „spielt“.
* `Stop`: Cursor aus `SamplesPlayed` errechnen (modulo Länge bei Loop), `Stop` + `FlushSourceBuffers`; Status sofort 0.
* `GetStatus`: aus Host-Zustand und `GetState(XAUDIO2_VOICE_NOSAMPLESPLAYED)` – 1 (bzw. 5 bei Loop) solange gestartet und
  `BuffersQueued > 0`, sonst 0. Muss nach `Stop`, `SetBufferData(0,0)` und Pufferende sofort 0 liefern.
* `SetFrequency(f)`: `SetFrequencyRatio(f == 0 ? 1 : f / rate)`. `SetVolume`/`SetMixBins`: Matrix nach 6.4.
* `Release`: `DestroyVoice` (bzw. zurück in den Pool).

### 6.3 Streams

* `DirectSoundCreateStream`: Source-Voice im Format aus `lpwfxFormat` (immer `SoundData+0x34`, auch bei ADPCM;
  `cbSize` 0x12 ignorieren) bzw. vom XMV; Gast-Objekt mit Vtable bei +0 (Slots `AddRef, Release, GetInfo, GetStatus,
  Process, Discontinuity, Flush`, `avi.md` 2.2), Refcount 1. Callback/Kontext merken.
* `Process`: Warteschlange voll (`dwMaxAttachedPackets`) → `0x88780032` ohne Statusänderung. Sonst Paket **kopieren**
  (ADPCM paketweise dekodieren; ein unvollständiger letzter Block durch Nullen ist erlaubt), `SubmitSourceBuffer`
  mit Paketsatz als `pContext`, `*pdwStatus = 0x8000000A`.
* `OnBufferEnd` (XAudio2-Thread): nur Ereignis „Paket k fertig“ einreihen. **`DirectSoundDoWork`** (Gast-Thread):
  `*pdwCompletedSize = Größe`, `*pdwStatus = 0`, ggf. Callback (5.2).
* `Pause(0/1/2)`: `Start` / `Stop` (Warteschlange bleibt) / „wartet auf Sync“. `SynchPlayback`: alle wartenden mit
  einer `OperationSet` starten und `CommitChanges`.
* `FlushEx(…, 1)` und `Flush`: `Stop`, `FlushSourceBuffers`, alle Pakete als „verworfen“ markieren, `PLAYING` sofort
  aus dem Host-Status nehmen; Status-Schreiben im nächsten `DoWork`.
* `Release` (letzter Verweis): synchron `DestroyVoice` (danach liefert XAudio2 keine Callbacks mehr), offene Pakete
  **noch im `Release`** mit `0x80004004` schreiben (die Stimme lebt bis zum Rücksprung, 0xCC107) und die
  Ereignisliste dieses Streams leeren. Danach kein Zugriff mehr.
* `Discontinuity`: letztes Paket mit `XAUDIO2_END_OF_STREAM` markieren.

### 6.4 Pegel und Matrix

Exakt nach 3.6, eigene Funktion (Tests mit `m5_gain_model.out`):

* **2D mono:** `g_L = 10^((vol − 600 + bin0)/2000)`, `g_R = 10^((vol − 600 + bin1)/2000)` (nach 1/64-dB-Rundung und
  Klemmung auf −64 dB); Matrix 1×N mit FL = g_L, FR = g_R, Rest 0. **2D stereo:** L → FL mit g_L, R → FR mit g_R.
  Ohne `SetMixBins` (XMV) gelten die DSOUND-Standard-Mixbins zu 0 mB (0x2638B4/0x2638AC).
* **3D mono:** Richtungsmatrix aus `X3DAudioCalculate(X3DAUDIO_CALCULATE_MATRIX)` mit **flacher** Distanzkurve
  (nur Richtung), multipliziert mit `10^((vol + dist)/2000)`; `dist` aus der eigenen Kurvenfunktion (identisch
  zu FUN_00249893). Alternativ X3DAudio-Kurve aus 3.6 – dann aber `MUTE3DATMAXDISTANCE` (ab Max stumm) beachten.
* **Hall-Send:** `10^((vol − 600 + dist)/2000)` auf den Hall-Submix (mono), nicht über X3DAudio.
* **LFE (nur 5.1):** 3D-Stimmen zusätzlich `10^((vol + dist)/2000)` auf LFE (Spiel-Mixbin 3); bei Stereo entfällt LFE
  (Vermutung zum Xbox-Stereo-Downmix).
* **Änderungen bündeln:** Alles, was zwischen zwei `CommitDeferredSettings` gesetzt wird, mit einer `OperationSet`
  je Tick anwenden und in `CommitDeferredSettings`/`DoWork` committen; unveränderte Werte nicht erneut setzen
  (jede Stimme bekommt je Tick 4–8 Aufrufe).

### 6.5 3D-Richtung, HRTF, Vorne/Hinten

* X3DAudio ist wie DS3D linkshändig mit rechts = oben × vorne; Listener- und Emitter-Vektoren können
  **unverändert** durchgereicht werden (Front/Oben vorher orthonormalisieren, X3DAudio verlangt das).
* Konkrete Einstellungen: `X3DAudioInitialize(Kanalmaske der Mastering-Voice, X3DAUDIO_SPEED_OF_SOUND, h)`;
  `X3DAUDIO_LISTENER{OrientFront, OrientTop, Position, Velocity = 0}` aus dem letzten Commit;
  `X3DAUDIO_EMITTER{ChannelCount 1, Position, Velocity 0, CurveDistanceScaler 1, InnerRadius ≈ 1, pVolumeCurve`
  = flache Kurve `{(0,1),(1,1)}`, `DopplerScaler 0}`; `X3DAUDIO_DSP_SETTINGS{SrcChannelCount 1, DstChannelCount =
  Ausgabekanäle, pMatrixCoefficients}` mit `X3DAUDIO_CALCULATE_MATRIX` (bei 5.1 zusätzlich LFE selbst setzen, 6.4).
  Entfernungsgewinn, Hall-Send und LFE rechnet der Host nach 3.6 selbst; X3DAudio liefert nur die Richtung.
* **Vorne/hinten** (3.7): Durchreichen bildet die Xbox nach; in Stereo ist der Unterschied gering, bei 5.1 kämen
  Geräusche vor der Kamera von hinten. Vorschlag: `[audio] positional = xbox | corrected` mit Standard `corrected`
  (Front-Vektor des Listeners negieren) – als Abweichung in `loader_design.md` 8 eintragen. Zur Entscheidung durch
  den Nutzer nach Gehör.
* **HRTF:** Die Xbox nutzt volle HRTF plus Crosstalk-DSP; das ist ohne DSP-Emulation nicht nachbaubar. Standard ist
  X3DAudio-Amplitudenpanning; eine Kopfhörer-Option mit Windows-HRTF (`CreateHrtfApo`, 48 kHz mono je Quelle) kann
  später folgen. `InnerRadius` klein setzen (etwa 1 Einheit), damit nahe Quellen nicht hart springen.

### 6.6 Hall und Gesamtpegel

* Hall-Submix: 1 Eingangskanal, Effekt `XAudio2CreateReverb`, Parameter aus dem Spiel-Preset über
  `XAUDIO2FX_REVERB_I3DL2_PARAMETERS{WetDryMix = 100, …}` und `ReverbConvertI3DL2ToNative` (Werte für alle 5 Presets
  in `m5_i3dl2.out`). `SetI3DL2Listener` mit verzögertem Apply erst in `CommitDeferredSettings` übernehmen.
  `flRoomRolloffFactor` ist 0 und wird von XAudio2 ohnehin ignoriert.
* **Headroom:** Weil die APU-Mixbin-Headroom und die DSP-Pegel unbekannt sind (3.6), Startwert für die
  Mastering-Lautstärke −6 dB plus `FXMasteringLimiter` (XAPOFX, in `xaudio2_9.dll` über `CreateFX`) an; Feinabgleich
  2D zu 3D per Entwickler-Schlüssel und nach Gehör bzw. gegen eine xemu-Aufnahme (9.5).

### 6.7 Kategorien und Spieloptionen

| Kategorie | Optionen-Regler (Szenenknoten) | Global | Standard | SoundMgr | Nutzer |
|---|---|---|---|---|---|
| 0 Effekte | `slider_soundvolume` | 0x2E790C | 0,75 | +0x34 (vf10 0xCA420) | alle Effekte, 3D, Ambience über `FsPlaySound` (Level 2-2), **Filme** |
| 1 Sprache | `slider_speechvolume` | 0x2E7920 | 0,75 | +0x38 (vf12 0xCA430) | Sprache, Dialoge |
| 2 Musik | `slider_mucisvolume` (sic) | 0x2E7918 | 0,75 | +0x3C (vf14 0xCA450) | Musik, Ambience über `FsPlayMusic` (Level 1-2, 1-3) |

Zuordnung belegt durch `FUN_000279C0` (Knotennamen ↔ `param_1[0x42D..0x42F]` ↔ Globals) und die Regler-Handler
0x29217/0x292C9/0x293AC. Die Regler laufen in Schritten von 0,05 zwischen 0 und 0,95 (0x291B3 `fadd 0.05`, Obergrenze
0x294F3C = 0,95; 0x296C8 `fsub 0.05`, Untergrenze 0). Gespeichert werden sie in `Options.dat` (`files.md` 4.1: erste
drei Floats Sprache, Musik, Effekte); `FUN_000A1FE0` übernimmt sie beim Laden nur im Bereich 0..0,95 (sonst Standard)
und setzt dann die drei Kategorien (0xA2173..0xA21A1). **Ohne geladene `Options.dat` (erster Start) bleiben die
Kategorien im SoundManager auf 1,0** (Init 0xCA3C0..0xCA3C6), obwohl die Regler 0,75 anzeigen; der Trace-Lauf mit
leerem Benutzerordner zeigt entsprechend 0 mB. Der erste Start ist also um 2,5 dB (3D) bzw. 5 dB (2D) lauter als jeder
spätere. Der Host muss nichts tun, außer `SetVolume` richtig umzusetzen.

### 6.8 Capture (`[audio] capture_wav`) und Betrieb ohne Gerät

* **Mit Gerät:** eigenes XAPO als letztes Glied der Effektkette der Mastering-Voice (nach dem Limiter oder per
  Schalter davor), kopiert jeden Verarbeitungsblock unverändert in einen Ringpuffer; ein Host-Thread schreibt
  `WAVE_FORMAT_IEEE_FLOAT` mit Geräterate und -kanälen. Optional `capture_stems`: dieselbe Tap-Effekt-Instanz auf den
  Kategorie- und Hall-Submixes (je eine Datei). Dazu eine Ereignisliste (`.tsv`: Zeit in Samples, Tick,
  Ereignis, DSOUND-Objekt, Name/Kategorie aus der Probe 6.1, mB, Frequenz, 3D-Position).
* **Ohne Gerät** (Fernseher aus; auch für automatische Prüfungen): XAudio2 kann ohne Endpunkt keine Mastering-Voice
  anlegen. Dann übernimmt ein **Software-Mixer** dieselben Quellen, Formate, Matrizen (6.4) und Submix-Pegel,
  getaktet von einem Host-Thread in 10-ms-Blöcken (Echtzeit), mit eigener Resampling-Stufe (z. B. 4-Punkt-Hermite
  oder windowed sinc) und ohne Hall (oder mit einem einfachen Hall). Er erzeugt die gleichen Paket-Ereignisse wie
  XAudio2, damit Streams normal weiterlaufen, und schreibt `capture_wav`. Wenn das Gerät später verfügbar ist
  (`OnCriticalError`/Gerätewechsel), wechselt der Host im nächsten `DoWork`.
* Der heutige Stub-Modus (`dummy_streams`) entfällt bzw. wird zu diesem Modus ohne Ausgabe.

### 6.9 Launcher-Reiter „Audio“ und Schlüssel

Vorschlag für `[audio]` (jede Zeile ein Schema-Eintrag mit de/en-Bezeichnung, Reiter Audio bzw. Erweitert):

| Schlüssel | Standard | Bedeutung |
|---|---|---|
| `enabled` | 1 | Ton an/aus (aus = Software-Mix ohne Ausgabe, Spiel läuft gleich) |
| `device` | leer | Ausgabegerät (Endpunkt-ID; leer = Windows-Standard; Launcher listet Geräte) |
| `speakers` | `auto` | `auto`, `stereo`, `5.1` (Kanalzahl der Mastering-Voice) |
| `master_volume` | 100 | Gesamtlautstärke in % (zusätzlich zu den Spielreglern) |
| `reverb` | 1 | I3DL2-Hall an/aus |
| `positional` | `corrected` | `xbox` (Vorne/Hinten wie Original) oder `corrected` (6.5) |
| `limiter` | 1 | Mastering-Limiter |
| `background` | `play` | `play` oder `mute` bei inaktivem Fenster |
| `capture_wav` | leer | Entwickler: Datei für die Gesamtmischung |
| `capture_stems` | 0 | Entwickler: zusätzlich je Submix eine Datei |
| `event_log` | 0 | Entwickler: Ereignisliste neben `capture_wav` |
| `trim_3d` | 0 | Entwickler: dB-Korrektur 3D gegenüber 2D (Abgleich 6.6) |

Die drei Spielregler (Musik, Effekte, Sprache) bleiben im Spiel (Optionen, Spielstand). `dummy_streams` wird mit M5
überflüssig.

---

## 7. Threading-Modell (E10)

| Thread | Was er tut | Gast-Speicher |
|---|---|---|
| Gast-Hauptthread (und seine Fibers: Lade- und Stream-Pumpen) | alle 40 DSOUND-Einstiege und Vtable-Methoden; `DoWork` je Tick wendet Ereignisse an (Paket-Status, `CompletedSize`, Callbacks über `core::CallGuest`), erkennt Geräteverlust | liest/schreibt |
| XAudio2-Engine-Thread | Mischen; `OnBufferEnd`/`OnVoiceError`/`OnCriticalError` reihen nur Ereignisse in eine lock-freie Warteschlange ein | **nie** |
| Capture-Schreiber | leert den Ringpuffer des Taps in die Datei | nie |
| Software-Mixer (nur ohne Gerät) | mischt in Echtzeit, erzeugt dieselben Ereignisse | nie |

* E10 würde das Schreiben von Status-DWORDs aus Host-Threads erlauben. Empfohlen ist trotzdem, **alle**
  Gastzugriffe in `DoWork` zu bündeln: Es gibt dann kein Rennen mit `Release` (3.5) und keine Schreibzugriffe in
  freigegebenen Speicher, und die Zustellung ist an den Tick gebunden (für Replay/M9 günstig). Kosten: höchstens ein
  Tick (16,7 ms) Verzögerung bei der Paketrückmeldung; die Warteschlange hält 3 s (Musik) bzw. 2 Pakete (XMV).
* Die XAudio2-Engine wird beim Hoststart auf dem Boot-Thread angelegt (COM im MTA initialisieren, Vermutung zum
  Bedarf in 2.9), nicht im Gast-Thread. IXAudio2 ist frei-threaded; Aufrufe aus dem Gast-Thread sind erlaubt,
  aus den Callbacks dagegen keine Voice-Erzeugung/-Zerstörung.
* Host-Code läuft dabei teils auf Fiber-Stacks (Pumpe, 512 KiB): `Process` darf nicht blockieren und keinen
  großen Stack brauchen (Dekodieren eines 19,2-s-ADPCM-Pakets ≈ 423000 Samples in einen Heap-Puffer).
* Keine Gastfunktion wird je von einem Host-Thread aufgerufen; Callbacks (XMV) nur aus `DoWork`.

---

## 8. Fallen-Checkliste für die Umsetzung

1. `Process` setzt `E_PENDING` synchron; volle Warteschlange → `0x88780032`, Status unverändert.
2. Paketdaten beim `Process` kopieren; nach `Stream::Release` keine Schreibzugriffe (Voice und Paketpuffer werden
   sofort freigegeben).
3. `GetStatus` (Puffer) sofort 0 nach `Stop`, `SetBufferData(0,0)` und Ende; Bit 0 steuert die Lebensdauer der
   Stimme. Bei einem nie gestarteten Puffer 0.
4. `Stop`/`Play` setzt am gemerkten Cursor fort; `SetBufferData` setzt den Cursor zurück.
5. `SetFrequency(0)` = Originalrate.
6. ADPCM: `nAvgBytesPerSec` und `cbSize` aus dem Spielformat nie verwenden; 64 Samples je Block (4.5).
7. `SetRolloffCurve` übergibt einen Zeiger in Spielspeicher (Inhalt konstant) – Werte beim Aufruf kopieren.
8. `SetI3DL2Source` und andere Puffer-Aufrufe kommen bei 3D-Streams mit `NULL`-Puffer.
9. Der Hall-Send und LFE hängen nur an 3D-Stimmen; 2D-Stimmen bleiben trocken.
10. Headroom 600 mB nur für Nicht-3D; Mixbin-Lautstärken addieren sich zur Puffer-Lautstärke.
11. 3D auf Stereo-Sample: Puffer wird angelegt und ohne `Play` wieder freigegeben.
12. Doppelte `Play`-Aufrufe hinterlassen nie freigegebene Objekte (zählen, nicht abstürzen).
13. `DirectSoundCreate` mehrfach (XMV) und `Release` ohne Abbau; `DownloadEffectsImage` → `S_OK`; die Datei
    `XBOX\dsstdfx.bin` muss weiter öffnbar sein (`avi.md` 2.1).
14. XMV: Callback nur aus `DoWork`; `FlushEx` lässt `PLAYING` sofort fallen.
15. Gerät weg (Fernseher aus): kein Absturz, Software-Mix, Streams laufen weiter.

---

## 9. Verifikationsplan (ohne Ohren)

### 9.1 Unit-Tests (`fut_tests`, nur synthetische Signale)

| Test | Erwartung (Quelle) |
|---|---|
| ADPCM-Decoder | bitgleich mit dem Referenzmodell `m5_adpcm.py` (Variante 64, exakte Formel) an synthetisch codierten Blöcken (Sinus, Rauschen, Sprünge; mono und stereo); Referenzvektoren aus `m5_adpcm.encode` erzeugt |
| Stream-Status | `Process` → `E_PENDING`; Fertig erst nach `DoWork` (0, `CompletedSize`); 11. Paket → `0x88780032`; `FlushEx` → `0x80004004` und `PLAYING` weg; `Release` → keine Schreibzugriffe danach (Wächtermuster im Testspeicher); `Pause(2)` + `SynchPlayback` startet zwei Streams im selben Block |
| Puffer-Status | `Play` → 1 (Loop 5); `Stop` → 0 sofort; `Play` setzt fort; `SetBufferData` setzt zurück; Ende → 0 |
| Pegelmodell | Tabellen aus `m5_gain_model.out` (2D-Regler, Panorama, 3D-Kurve für Min/Max 5/80 und 30/40, Hall-Send) |
| Hall | Spiel-Presets → `ReverbConvertI3DL2ToNative` = Werte aus `m5_i3dl2.out` |
| Richtung | Listener vorne (0,0,1)/oben (0,1,0), Quelle bei +x → rechter Kanal lauter; Spiel-Basis aus 3.7 → links/rechts wie erwartet |

### 9.2 Aufnahmen und automatische Auswertung

Werkzeug: `m5_capture_check.py` (Selbsttest PASS: Lokalisieren auf 1 Sample, Pegel auf 0,01 dB, Tempo 1,00000,
Lücke 103,4 ms gefunden). Es dekodiert die Originale im Speicher und vergleicht sie mit `capture_wav` bzw. den Stems.
Alle Läufe skriptgesteuert (`[debug] input_script`), Ergebnisse nur unter `run\analyse_m5\`.

| Lauf | Prüfung | Kriterium |
|---|---|---|
| R1 Frontend 150 s | `music/01 - front-end.wav` im Stem „Musik“ lokalisieren | NCC > 0,95; Tempo 1 ± 0,001; Pegel = 2D-Formel ± 0,5 dB; Rest-SNR > 30 dB; Loop-Lücke 103,4 ± 2 ms bei 113,6 s, sonst keine Lücke > 5 ms |
| R2 Menütasten | `sounds/button.wav` (o. ä.) je Tastendruck im Stem „Effekte“ | Anzahl = Tastendrücke, Pegel stabil |
| R3 Optionen | Musikregler 0,75 → 0,5 → 0 | Musik-Stem −5,0 dB, dann ≤ −60 dB (Formel 2·Δ mB); Effekte unverändert |
| R4 Level 1-1 (m4_newgame, 3 min) | `music/01 - planet express.wav` durchgehend; 3D-Effekte im Stem „Effekte“, Hall-Stem ≠ 0 | keine Lücken außer Loop; Glitch-Zähler (`GetPerformanceData`) 0 |
| R5 Kamera drehen | Effekte mit bekannter Position (Ereignisliste) | Vorzeichen des L/R-Pegelverhältnisses = Erwartung aus Position und Listener; dreht bei 180° |
| R6 Pause-Menü | START, warten, START | Stimmen stumm, danach Fortsetzung am selben Offset (Korrelation läuft weiter) |
| R7 Sprache | deutsch/englisch | Ereignisliste nennt `localized/<sprache>`; Stem „Sprache“ korreliert mit den Originalen |
| R8 30 min Dauerlauf (Zufallseingaben) | kein Hänger (Voice-Abbau, Kontextwechsel), Speicher stabil, Quellenzahl begrenzt, keine Glitches | |
| R9 Clipping | Gesamtmix vor dem Limiter | Anteil der Samples mit Betrag ≥ 1 ≈ 0; Limiter-Absenkung protokolliert |
| R10 mit/ohne Gerät | gleicher Lauf mit Fernseher an und aus | Stems bis auf Hall und Resampler gleich (SNR > 25 dB) |

### 9.3 Laufzeit-Messungen, die die statische Analyse offen lässt

* Höchstzahl gleichzeitiger Quellen (2D/3D/Streams) je Szene; Zahl nie freigegebener Objekte (3.3 Punkt 6).
* Welche Sprachzeilen als ADPCM-Stream laufen (3.9) und wie lange solche Stimmen leben.
* Kategorie, unter der jede Ambience läuft (statisch: Level 1-2/1-3 Musik, 2-2 Effekte; für `2-2_Machines`, `2-3_Alien`, `3_Lava` kein Abspielaufruf erkannt, `m5_script_sound.out`).
* Ob irgendwann `SetEnvironment` aufgerufen wird (erwartet: nie).

### 9.4 Exit-Kriterien M5 (`loader_design.md` 6) und ihre Prüfung

| Kriterium | Prüfung |
|---|---|
| Menüklänge hörbar, Frontend-Musik streamt und loopt | R1, R2 |
| Positionsabhängige Effekte im Level | R4, R5 |
| Sprache in der eingestellten Sprache | R7 |
| Kein Hänger bei Voice-Zerstörung/Kontextwechsel, 30 min ohne Aussetzer | R8, R4 |
| Unit-Tests ADPCM und Stream-Status | 9.1 |

### 9.5 Optional: Referenz aus xemu

xemu emuliert den Audio-DSP (I3DL2, Crosstalk). Eine Aufnahme des Original-Spiels (Prozess-Loopback per WASAPI
`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`) bei gleicher Eingabe würde das Verhältnis 2D/3D und den Hallpegel
eichen (6.6). Dafür wäre ein kleines Aufnahmewerkzeug nötig; xemu liegt laut `tools\xemu_capture.py` vor.

### 9.6 Für das Abhören durch den Nutzer

Musik-Loops (Lücke wie Original), Ambience-Loops (Lücke bis 0,3 s ist Original), Lautstärkeverhältnis Musik/Effekte/
Sprache bei Standardreglern, Links/Rechts beim Drehen der Kamera, ob Geräusche vor der Kamera „vorne“ klingen
(`positional`), Hall (dezent), Pause-Menü, Regler im Optionsmenü.

---

## 10. Offene Punkte und Vermutungen

* Standard-Headroom der APU-Mixbins und Pegel der DSP-Effekte (3.6) – nur über Abgleich (6.6, 9.5).
* Kanalbelegung der HRTF-Ausgänge auf die XTLK-Bins (3.6) und der Xbox-Stereo-Downmix von Rück- und LFE-Kanälen.
* Bedeutung der Flags 0x100 und 0x40000 in `DSBUFFERDESC` (ohne Wirkung für den Host).
* Ob der Xbox-Chip ADPCM wie ffmpeg rundet (4.5).
* Laufzeitfragen aus 9.3.
* COM-Initialisierung für XAudio2 2.9 (7) – beim Bau prüfen.

---

## 11. Korrekturen und Ergänzungen zu `avi.md`

| `avi.md` | Richtig (Beleg) |
|---|---|
| 2.4.1 „[1] 0xCBCB0 `Update` pro Frame“ | pro **Tick** aus `CApp::vf10` 0xD25D0 (0xD25F5); zusätzlich direkt nach Musikstarts (0x21358, 0x21B1E, 0xA6482) |
| 7 „Kategorie 0 … vermutlich Musik“ | 0 = Effekte, 1 = Sprache, 2 = Musik (6.7); Filme = Kategorie 0 |
| 2.4.4 „liest 0,3 s, füllt am Dateiende mit Nullen auf, schlägt die Position modulo der Datenlänge um“ | modulo `dataSize − dataOffset`; letzte `dataOffset` Byte fehlen; Lücke = genulltes Paketende (3.5, 4.3) |
| 2.4.5 „Umgebungen“ (implizit benutzt) | nur „Padded Cell“ ist je aktiv; kein Skript ruft `FsSetReverb` auf (3.10) |
| 2.4.6/7 „Ambience … ob gestreamt, offen“ | gestreamt (`FsLoadSound(name, 1)`), gespielt über `FsPlayMusic` (Kategorie 2, Loop) oder `FsPlaySound` (Kategorie 0) (3.8) |
| 2.5 „3D: Mixbins 6..9 als HRTF/Panning, Mixbin 10 als Reverb-Send mit lRoom-Pegel“ | ergänzt: Headroom 600 mB nur für 2D; Hall-Send und LFE auch entfernungsgedämpft; Kurvensemantik (3.6) |
| 2.3 „Lautstärken in mB … Host rechnet gain = 10^(mB/2000)“ | 2D: Puffer- plus Mixbin-Lautstärke (doppelte Dämpfung) minus 6 dB (3.6) |
| 2.3 `Stop` | `Stop` merkt den Cursor, `Play` setzt fort (0x24E04E); im Pause-Menü genutzt |
| 2.4.2 Doppler/Velocity | bestätigt wirkungslos; zusätzlich: Listener-Front = −Blickrichtung (3.7) |
