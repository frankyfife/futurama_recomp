# Audio (DSOUND), Video (XMV), Input (XPP): was der Host bereitstellen muss

Ziel-Binary: `orig\retail_eu\default.xbe` (Retail EU/PAL). Alle Adressen sind VAs dieses Images.
Reproduzierbar über die Skripte in `analysis\loader\scripts\avi_*.py`. Die Ausgaben liegen als `analysis\loader\avi_*.out` vor,
die maschinenlesbare API-Tabelle in `analysis\loader\avi_api_surface.tsv`.
Der Bericht wurde in zwei unabhängigen Runden nachgeprüft und korrigiert; Prüfskripte `scripts\verify_avi_*.py` (2. Runde: `scripts\verify_avi_r2_*.py`), Ausgaben `scripts\out\verify_avi_*`, Ergebnis im Abschnitt **Verifikation** am Ende.

## 0. Kurzfazit

* **Die vom Spiel genutzte API-Oberfläche ist klein und klar abgegrenzt.**
  * **DSOUND:** 40 Einstiegspunkte. 38 davon ruft der Spielcode auf, 2 nur XMV. Dazu kommen 5 indirekte `XMediaObject`-Vtable-Methoden am Stream-Objekt.
  * **XMV:** 7 Einstiegspunkte.
  * **XPP/XInput:** 7 Einstiegspunkte.
  * Alle sind `__stdcall` (belegt durch `ret N`, siehe `avi_callconv.out`).
  * NetImmerse ruft keine dieser Funktionen auf. Alle DSOUND-Aufrufe des Spiels liegen im Sound-Manager-Code 0xcb540..0xccdc7, dazu kommt ein einziger Aufruf im Movie-Player (0xee471).
* **Der Ghidra-Callgraph ist hier unvollständig.** Virtuelle Methoden, die nur über eine Vtable erreichbar sind, hat Ghidra nicht als Funktionen erkannt. Dazu gehören die Voice-Update-Methode 0xcc5e0, der WAV-Parser 0xcb720/0xcb960, der Controller-Update 0x9c3d0 und die I3DL2-Presetwahl 0xcbfa0. Der Callgraph kennt nur 42 Aufrufer→Ziel-Kanten in DSOUND (34 aus `.text`, 8 aus XMV). Erst ein Rohscan nach `E8/E9 rel32` (`avi_rawcalls.py`, unabhängig nachgeprüft mit `verify_avi_rawscan.py`) findet **69 echte DSOUND-Callsites (61 im Spielcode, 8 in XMV)** plus den Linker-Thunk 0xf9cc0 und auch den einzigen `XInputGetState`-Aufruf. *(Korrigiert bei der Verifikation; vorher stand hier „70 statt 44“.)*
* **Audio-Assets:**
  * Effekte sind überwiegend Xbox-ADPCM (Tag 0x69), mono, 22050 Hz (2837 von 2982 WAVs).
  * 36 Dateien (352 MB) sind PCM 16 Bit stereo, 22050 Hz: 18 Musikstücke in `sounds/music/` von `FutPAL00.img` und 18 Ambience-Loops in `sounds/level1..5/ambient/` von `FutPAL03.img` *(korrigiert in Runde 2; vorher „im Root“)*. Musik wird gestreamt (10 Pakete à 0,3 s). Mit `bStream=1` fordert der Code außerdem `sounds/music/EndOfLevel_theme.wav` (0xa645d), `sounds/%s.wav` (0x1ee1b) und **lokalisierte Sprachzeilen** über FUN_000d8720 (0xd88d6) an. Letztere sind Xbox-ADPCM, **ADPCM-Streams sind also möglich** (siehe 2.4.3). Ob auch die Ambience-Loops gestreamt werden, ist weiter offen.
  * 3D-Sound läuft über DS3D (deferred) mit eigener Rolloff-Kurve, HRTF, Crosstalk-Mixbins und I3DL2-Reverb aus `dsstdfx.bin`.
* **Videos:** Alle 47 `.xmv` (alle in `/video`, alle mit Suffix `-PAL`) sind XMV v2, 640x480. 44 laufen mit etwa 15 fps, `Attract-PAL`, `Credits-PAL` und `SCI-PAL` mit 25 fps (pts-Abstände aller 47 Dateien gemessen, `verify_avi_r2_pyav.out`; *korrigiert in Runde 2, vorher „alle etwa 15 fps“*). Die Videospur ist bei allen 47 **WMV2 (WMV8)**, der Ton **Xbox-IMA-ADPCM** stereo. Zwischensequenzen haben 5 Tonspuren, eine pro Sprache. **ffmpeg (PyAV 18 / libavcodec 62.28) demuxt und dekodiert beide Beispiel-Videos fehlerfrei** (`xmv`, `wmv2`, `adpcm_ima_xbox`).
* **Input:**
  * 4 Gamepad-Ports mit Hotplug über `XGetDeviceChanges`.
  * Analogtasten werden als Wert/256 gelesen, digitale Tasten als 0/1. Die Schwelle für "gedrückt" liegt bei 0.1171875 (= 30/256) für 14 der 16 Kanäle; die Stick-Klicks (Kanal 12/13) behalten den Basiswert 1/256.
  * Stick-Schwelle 0.4.
  * Rumble über `XInputSetState` mit 2 Motoren (0..65535).
  * Keine MU-Mounts, kein Headset, kein `XInputGetCapabilities`/`XInputPoll`.
* **Sound-Laden und Streaming laufen über XAPI-Fibers.** Der Spielcode liest die aktuelle Fiber direkt aus `[TLS+0xC]`. Ein naiver Ersatz durch Windows-Fibers bricht das; die XAPI-Fibers sollten im Original bleiben (2.4.3, Runde 2).

| Bereich | Host-Empfehlung | Aufwand |
|---|---|---|
| DSOUND | XAudio2 + X3DAudio + XAudio2-Reverb (I3DL2); alternativ OpenAL Soft (portabel) | mittel (ca. 1-2 Wochen) |
| XMV | ffmpeg/libav* (libavformat `xmv`, libavcodec `wmv2`/`adpcm_ima_xbox`), Ton über die eigenen DSOUND-HLE-Streams | mittel (ca. 1 Woche) |
| XInput | XInput 1.4 bzw. SDL3 Gamepad | leicht (1-2 Tage) |

## 1. Methodik und Werkzeuge

| Skript | Zweck | Ausgabe |
|---|---|---|
| `avi_api_surface.py` | Aufrufer aus `callgraph.tsv` außerhalb der jeweiligen Section | stdout |
| `avi_rawcalls.py` | Rohscan nach `E8/E9 rel32` in allen Code-Sections hin zu DSOUND/XMV/XPP-Funktionsstarts, mit Capstone-Validierung der Instruktionsgrenze | `avi_rawcalls.out` |
| `avi_callconv.py` | `ret N` jeder Einstiegsfunktion | `avi_callconv.out` |
| `avi_datarefs.py` | Datenzeiger in DSOUND/XMV/XPP/DOLBY (Init-Tabellen, Vtables) | `avi_datarefs.out` |
| `avi_section_imports.py` | Was DSOUND/XMV/XPP selbst aufrufen (Kernel, XAPI, D3D) | `avi_section_imports.out` |
| `avi_vtables.py`, `avi_disasm.py`, `avi_dump.py` | Vtables, Disassembly mit Namen/Strings, Ghidra-C | ad hoc |
| `avi_getsound_sites.py` | Callsites von `GetSound` (FUN_000c97d0); Argumente nur heuristisch | `avi_getsound_sites.out` |
| `avi_i3dl2.py` | I3DL2-Listener-Presets dekodieren | stdout |
| `avi_img_audio.py` | WAV-Formatstatistik aus den `.img`-Archiven im ISO (nur lesend) | `avi_img_audio.out` |
| `avi_wav_probe.py` | einige WAVs **im Speicher** mit PyAV dekodieren | `avi_wav_probe.out` |
| `avi_xmv_headers.py` | Header aller `.xmv` im ISO (nur lesend) | `avi_xmv_headers.out` |
| `avi_extract_samples.py` | 2 kleine Beispiel-Videos nach `loader\samples\` | `uds-PAL.xmv`, `2-1-03-PAL.xmv` |
| `avi_pyav_probe.py`, `avi_pyav_timing.py` | ffmpeg-Demux/Decode-Test, Frame-Timing | `avi_pyav_probe.out`, `avi_pyav_timing.out`, `samples\*.frame0.png` |
| `avi_make_tsv.py` | erzeugt `avi_api_surface.tsv` | |

Das Disc-Image wurde nur gelesen. Extrahiert wurden ausschließlich die zwei genannten XMV-Dateien.

---

## 2. DSOUND

### 2.1 Überblick über die Section

* Die Section `DSOUND` (0x246d00..0x264354) enthält 401 Funktionen. 116 davon hat XbSymbolDatabase benannt, 285 sind `FUN_`.
* Von den benannten öffentlichen APIs (`IDirectSound*`, `DirectSound*`, `XAudio*`) wird jede vom Spiel oder von XMV aufgerufen, mit zwei Ausnahmen: `DSOUND::XAudioCalculatePitch` (0x246ed1) und `DSOUND::DirectSoundEnterCriticalSection` (0x246e0d), die nur intern laufen. `XAudioCreateAdpcmFormat` ist nicht gelinkt: das Spiel baut das ADPCM-Format selbst (siehe 2.4.3).
* **Statische Initialisierer in der C++-Init-Tabelle** laufen in `_cinit`. Es sind 4 kleine Store-Funktionen, die nur DSOUND-interne Zeiger setzen und keine Hardware anfassen:
  * Tabelleneinträge @0x2d3e70..0x2d3e7c
  * Ziele: FUN_0024968f (`mov [0x2641c8],0x263af4`), FUN_0024969a, FUN_0024f3d4, FUN_0024f3df
  * Sie sind harmlos und dürfen laufen (`avi_datarefs.out`, Disassembly in `avi_disasm.py 24968f 2496a5 24f3d4 24f3ea`).
* **Die Section `DOLBY` (0x310740, Flags 0x26) ist DSP-Mikrocode.** DSOUND kopiert ihn in den APU-DSP-Speicher: `FUN_0024f537(0,&DAT_00310740,800)` und direkt danach `FUN_0024f537(0x600,&DAT_00310a60,0x5fc)` in `DSOUND_00250000.c:988/991` (FUN_0024f537 ist ein reines memcpy an `[this+0x10]+offset`). Vermutung: AC-3-Encoder bzw. GP/EP-Programm (nicht aus dem Binary belegt). Für den PC-Host ist das irrelevant.
* **`dsstdfx.bin`:** Das Spiel lädt `XBOX\dsstdfx.bin` (String 0x2a3c3c, von 0xccd4e an FUN_000cbeb0 übergeben) in FUN_000cbeb0 und übergibt es an `IDirectSound_DownloadEffectsImage(ds, buf, size, &loc{0,1}, &desc)`.
  * FUN_000cbeb0 liefert `true`, sobald die Datei geöffnet werden konnte (0xcbf09); der HRESULT von `DownloadEffectsImage` wird **nicht** geprüft, der Puffer wird direkt danach freigegeben (`FUN_000a3880(buf)` @0xcbf6d).
  * Kann die Datei **nicht** geöffnet werden, überspringt der Init in 0xccd00 HRTF, Doppler-/Distanz-/Rolloff-Faktor, Listener-Position, I3DL2-Listener **und das Schreiben der Rolloff-Kurve** (die dann in BSS auf 0 bleibt). Der Host muss also dafür sorgen, dass der Datei-Layer des Spiels `XBOX\dsstdfx.bin` findet (verifiziert in 0xccd55..0xcce09).
  * Die Datei liegt als `xbox/dsstdfx.bin` (25256 Bytes) in `FutPAL00.img` (der Pfad entspricht also direkt `XBOX\dsstdfx.bin`, nur in anderer Schreibweise; `verify_avi_r2_disc.out`).
  * `loc` = `{I3DL2-Index 0, Crosstalk-Index 1}`; die Feldnamen folgen dem XDK-Header (Vermutung).
  * Für den Host gilt: Image ignorieren, Reverb und Crosstalk selbst nachbilden.
* **Abhängigkeiten von DSOUND selbst** (`avi_section_imports.out`): Kernel-Interrupt/DPC/Timer (`KeConnectInterrupt`, `KeInsertQueueDpc`, `KeSetTimer`, ...), `MmAllocateContiguousMemoryEx`, `MmGetPhysicalAddress`, `HalRegisterShutdownNotification`, `XAPILIB::XGetAudioFlags`. **Ersetzt man alle 40 Einstiegspunkte, läuft davon nichts mehr.** Die Kernel-HLE muss diese Funktionen für Audio dann nicht korrekt implementieren.
* **Linker-Thunk:** In `.text` liegt bei 0xf9cbd ein `sub ecx,8; jmp 0x24aebe` (Bytes `83 e9 08 e9 f9 11 15 00`), also ein this-Adjustor für einen DSOUND-Destruktor. Er wird nur aus der DSOUND-Vtable bei 0x2a5fac referenziert. Der Rohscan meldet ihn als `.text`-Aufruf, er ist aber **kein** Spielaufruf und kann ignoriert werden.

### 2.2 API-Oberfläche (aus `avi_rawcalls.out` und `avi_callconv.out`)

Alle Funktionen sind `__stdcall`, der Interface-Zeiger ist das erste Argument (`this`). "nofunc@" kennzeichnet Callsites in Code, den Ghidra nicht als Funktion kennt. 22 davon liegen in der Voice-Update-Methode 0xcc5e0 (0xcc6b9..0xcca27; die Methode reicht mit einem frühen `ret` bei 0xcc646 bis 0xcca62), die übrigen 5 in der Presetwahl 0xcbfa0. Summe Spielcode: 61 Sites (60 in 0xcb563..0xccdc7, 1 in 0xee471), XMV: 8.

| Adresse | Funktion | ret | Sites Spiel/XMV | Aufrufer |
|---|---|---|---|---|
| 0x24935c | DirectSoundCreate(pGuid, ppDS, pUnkOuter) | 12 | 1/1 | 0xccd00 (Init), XMV 0x264962 |
| 0x246e7d | IDirectSound_Release | 4 | 1/1 | 0xcbc30, XMV 0x264962 |
| 0x247911 | IDirectSound_DownloadEffectsImage | 20 | 1/0 | 0xcbeb0 |
| 0x246ea9 | DirectSoundUseFullHRTF() | 0 | 1/0 | 0xccd00 |
| 0x248d84 | IDirectSound_SetDopplerFactor | 12 | 1/0 | 0xccd00 (2.0, sofort) |
| 0x248d60 | IDirectSound_SetDistanceFactor | 12 | 1/0 | 0xccd00 (0.2) |
| 0x248e27 | IDirectSound_SetRolloffFactor | 12 | 1/0 | 0xccd00 (1.0) |
| 0x248df2 | IDirectSound_SetPosition | 20 | 2/0 | 0xccd00, 0xcbcb0 |
| 0x248e4b | IDirectSound_SetVelocity | 20 | 1/0 | 0xcbcb0 |
| 0x248da8 | IDirectSound_SetOrientation | 32 | 1/0 | 0xcbcb0 |
| 0x248e80 | IDirectSound_SetI3DL2Listener | 12 | 6/0 | 0xccd00, 0xcbfbb/0xcbfce/0xcbfe1/0xcbff4/0xcc007 |
| 0x248380 | IDirectSound_CommitDeferredSettings | 4 | 1/0 | 0xcbcb0 |
| 0x247a07 | DirectSoundDoWork() | 0 | 1/2 | 0xcbcb0, XMV 0x264677/0x264962 |
| 0x247938 | IDirectSound_SynchPlayback | 4 | 0/1 | XMV 0x264962 |
| 0x2493a3 | DirectSoundCreateBuffer(pDesc, ppBuf) | 8 | 1/0 | 0xcc150 |
| 0x248ea0 | IDirectSoundBuffer_SetBufferData(buf, pv, size) | 12 | 3/0 | 0xcc150, 0xcb580, 0xcc030 |
| 0x247988 | IDirectSoundBuffer_Play(buf, 0, 0, flags) | 16 | 4/0 | 0xcc390, 0xcc410, 0xcc4b0, nofunc@0xcc72e |
| 0x2479ac | IDirectSoundBuffer_Stop | 4 | 1/0 | 0xcb540 |
| 0x2479c4 | IDirectSoundBuffer_GetStatus | 8 | 2/0 | 0xcc030, nofunc@0xcca27 |
| 0x247950 | IDirectSoundBuffer_SetVolume | 8 | 1/0 | nofunc@0xcc91d |
| 0x248398 | IDirectSoundBuffer_SetFrequency | 8 | 1/0 | nofunc@0xcc910 |
| 0x24796c | IDirectSoundBuffer_SetMixBins | 8 | 2/0 | 0xcc150, nofunc@0xcc9d3 |
| 0x2483fc | IDirectSoundBuffer_SetPosition | 20 | 2/0 | nofunc@0xcc6df, 0xcc7f7 |
| 0x248431 | IDirectSoundBuffer_SetVelocity | 20 | 2/0 | nofunc@0xcc6fc, 0xcc814 |
| 0x2483d8 | IDirectSoundBuffer_SetMinDistance | 12 | 2/0 | nofunc@0xcc6b9, 0xcc829 |
| 0x2483b4 | IDirectSoundBuffer_SetMaxDistance | 12 | 2/0 | nofunc@0xcc6ce, 0xcc83e |
| 0x248466 | IDirectSoundBuffer_SetRolloffCurve | 16 | 1/0 | 0xcc150 |
| 0x24848a | IDirectSoundBuffer_SetI3DL2Source | 12 | 1/0 | nofunc@0xcca0e |
| 0x246e93 | IDirectSoundBuffer_Release | 4 | 1/0 | 0xcc030 |
| 0x2493fa | DirectSoundCreateStream(pDesc, ppStream) | 8 | 1/1 | 0xcc150, XMV 0x2647c6 |
| 0x2479ea | IDirectSoundStream_Pause(s, dwPause) | 8 (jmp) | 5/1 | 0xcb540, 0xcc390, 0xcc410, 0xcc4b0, nofunc@0xcc711, XMV |
| 0x2479e0 | IDirectSoundStream_SetVolume | 8 (jmp) | 2/0 | nofunc@0xcc8c4, **Movie 0xee471** |
| 0x2484aa | IDirectSoundStream_SetFrequency | 8 (jmp) | 1/0 | nofunc@0xcc8ba |
| 0x2479e5 | IDirectSoundStream_SetMixBins | 8 (jmp) | 2/0 | 0xcc150, nofunc@0xcc9c0 |
| 0x2484df | IDirectSoundStream_SetPosition | 20 | 1/0 | nofunc@0xcc7ab |
| 0x248508 | IDirectSoundStream_SetVelocity | 20 | 1/0 | nofunc@0xcc7c5 |
| 0x2484c7 | IDirectSoundStream_SetMinDistance | 12 | 1/0 | nofunc@0xcc7d7 |
| 0x2484af | IDirectSoundStream_SetMaxDistance | 12 | 1/0 | nofunc@0xcc7e9 |
| 0x248531 | IDirectSoundStream_SetRolloffCurve | 16 (jmp) | 1/0 | 0xcc150 |
| 0x2479ef | IDirectSoundStream_FlushEx(s, rtLo, rtHi, flags) | 16 | 0/1 | XMV 0x264677 (`FlushEx(s,0,0,1)`) |

**Indirekte Aufrufe über die Stream-Vtable.** Das DSOUND-Stream-Objekt `CDirectSoundStream` erhält im Konstruktor 0x248c91 die Vtable 0x2a5f5c. Slot 4 hat XbSymbolDatabase als `CDirectSoundStream_Process` benannt. Das bestätigt die XMediaObject-Reihenfolge `AddRef, Release, GetInfo, GetStatus, Process, Discontinuity, Flush`; die Namen der übrigen Slots stammen aus dem XDK (Vermutung).

| Vtable-Offset | Methode | Aufrufer (Beleg) |
|---|---|---|
| +0x00 | AddRef | XMV 0x264943, 0x2647c6 |
| +0x04 | Release | Spiel 0xee47d (`call [edx+4]` nach SetVolume) und 0xcc107 (Voice-Destruktor 0xcc030 gibt `voice+0x74` frei), XMV 0x264677/0x2647c6 |
| +0x0c | GetStatus(this,&dw) | XMV 0x264677 (wartet, solange `dw & 0x10000`) |
| +0x10 | Process(this, pInPacket, pOutPacket) | **Spiel 0xcb6b7** (`push 0; push &pkt; push this; call [ecx+0x10]`), XMV 0x264962 |
| +0x14 | Discontinuity(this) | XMV 0x264962 |

Nicht genutzt werden: `SetHeadroom`/`SetMixBinHeadroom` (nur intern `CMcpxAPU_SetMixBinHeadroom`), `SetPitch`, `SetLFO`/`SetEG`, `SetOutputBuffer`, `GetCurrentPosition`/`SetCurrentPosition`, `SetLoopRegion`, `IDirectSoundBuffer_Lock` sowie Notifications.

### 2.3 Objektmodell und Konventionen, auf die der Host achten muss

* **`DirectSoundCreate` liefert ein Singleton.** FUN_002492ef legt das Objekt einmal an (`DAT_002641a0`) und gibt danach AddRef-Kopien zurück. Der Rückgabewert ist `obj+8` (0x24935c). XMV ruft pro `GetNextFrame` mit Sync `DirectSoundCreate` + `SynchPlayback` + `Release` auf. Der Host braucht deshalb einen Refcount und darf beim Release das Gerät nicht abbauen.
* **Buffer-Zeiger sind für den Spielcode opak.** `CreateSoundBuffer` liefert `obj+0x1c` (0x2490ff). Das Spiel ruft nur die C-Funktionen auf, also kann der Host beliebige Handles zurückgeben.
* **Stream-Zeiger müssen auf ein Objekt mit Vtable bei +0** zeigen, mit dem Layout der Tabelle oben (stdcall, `this` auf dem Stack), weil das Spiel `Process` und `Release` virtuell aufruft. Die C-Funktionen `IDirectSoundStream_*` erhalten denselben Zeiger.
* **`DSBUFFERDESC`** (0xcc150): `dwSize=0x18`, `dwFlags = (3D ? 0x60010 : 0) | 0x100`, `dwBufferBytes=0`, `lpwfxFormat`, `lpMixBins=NULL`, `dwInputMixBin=0`.
  * Der Buffer bekommt seine Daten danach über `SetBufferData(ptr,size)` direkt aus dem Spiel-Speicher.
  * Flag-Bedeutungen nach XDK-Header (Vermutung, im Binary nicht verifiziert): 0x10 `DSBCAPS_CTRL3D`, 0x20000 `MUTE3DATMAXDISTANCE`, 0x40000 `LOCDEFER`, 0x100 `CTRLPOSITIONNOTIFY`.
* **`DSSTREAMDESC`** (0xcc1c3): `dwFlags = 3D ? 0x60010 : 0`, `dwMaxAttachedPackets=10`, `lpwfxFormat`, `lpfnCallback=NULL`, `lpvContext=NULL`, `lpMixBins=NULL`.
  * `lpwfxFormat` ist bei Streams **immer** `SoundData+0x34` (das rohe `fmt `-Chunk mit erzwungenem `cbSize=0x12`), auch bei ADPCM; nur der Buffer-Weg wählt bei ADPCM `+0x59` (0xcc18e vs. 0xcc279).
  * XMV nutzt stattdessen `dwMaxAttachedPackets=2` mit Callback 0x264775 (siehe 3.1).
* **`XMEDIAPACKET`** (0xcb680..0xcb6b7): 6 DWORDs mit `pvBuffer`, `dwMaxSize`, `pdwCompletedSize=0`, `pdwStatus=&voice[0x7c+4*i]`, Kontext 0 und `prtTimestamp=0`.
  * Das Spiel **pollt** `*pdwStatus` und füllt nach, sobald der Wert nicht mehr 0x8000000A (E_PENDING) ist (0xcb5d4).
  * **Das Spiel schreibt E_PENDING nie selbst** (einzige Verwendung der Konstante 0x8000000A im `.text` ist der Vergleich bei 0xcb5d4, `verify_avi_misc.out`). Daraus folgt: Der Host muss in `Process` den Status **synchron auf 0x8000000A setzen**, sonst füllt die Pumpe denselben Slot beim nächsten Durchlauf erneut und reicht ihn noch einmal ein (Queue-Überlauf, doppelte Pakete).
  * Das Original macht genau das: `CDirectSoundStream_Process` 0x247676 → FUN_0024bdef → FUN_0024b1c4 setzt `*pdwCompletedSize = 0` und `*pdwStatus = 0x8000000A` (`mov dword [eax],0x8000000a` @0x24b1d9). Kann der Stream keine Daten annehmen (Status-Bit 0 aus `CMcpxStream_GetStatus` gelöscht), liefert `Process` 0x88780032 und setzt den Status nicht; das Spiel wertet den HRESULT von `Process` nicht aus (0xcb6b7). (Runde 2)
  * Beim Verbrauch eines Pakets setzt der Host `*pdwStatus` asynchron auf 0 (Erfolg). Ein 32-Bit-Store reicht, x86 schreibt atomar.
* **`IDirectSoundBuffer_GetStatus`:**
  * Das Spiel testet Bit 0 (`test byte [esp+0x20],1` @0xcca2c; `DSBSTATUS_PLAYING` nach XDK).
  * Der Destruktor 0xcc030 macht `Stop` + `SetBufferData(0,0)` und **schleift danach, solange `GetStatus != 0`**.
  * Daraus folgt: Nach `Stop` muss der Host sofort 0 melden, sonst hängt das Spiel.
* **Gleiche Falle bei XMV-Streams:** `CloseDecoder` (0x264677) macht `FlushEx(s,0,0,1)` und schleift danach mit `Sleep(1)` + `DirectSoundDoWork`, solange `GetStatus & 0x10000` gesetzt ist (nach XDK `DSSTREAMSTATUS_PLAYING`, Vermutung).
* **3D-Aufrufe** des Spiels übergeben fast immer `dwApply=1` (deferred). Pro Frame folgt `IDirectSound_CommitDeferredSettings` (0xcbe95). Ausnahmen mit `dwApply=0` (sofort): `SetDopplerFactor(..,0)` in 0xccd00 und `SetRolloffCurve(.., 6, 0)` bei der Voice-Erzeugung (0xcc334/0xcc365).
* **Lautstärken in mB:**
  * Das Spiel rechnet `lVolume = (1 - v) * -1000`; bei `v <= 0` setzt es -10000 (Konstanten 0x2939f8 = 1.0 und 0x2a3be4 = -1000.0, Code 0xcc87c bzw. 0xee450).
  * Der lineare Regler deckt also nur 0..-10 dB ab.
  * Der Host rechnet `gain = 10^(mB/2000)` und behandelt -10000 als stumm.
* **Pegel der Mixbins** kommen als mB-Werte in `DSMIXBINVOLUMEPAIR` (2D-Panning, siehe 2.4.5).

### 2.4 Das Soundsystem des Spiels

#### 2.4.1 Klassen (Namen frei vergeben)

| Klasse | Größe | Vtable (Basis) | Belege |
|---|---|---|---|
| SoundManager (DSound-Implementierung) | 0x7c | 0x2a3b80 (Basis 0x2a3978) | `new(0x7c)` + Konstruktor 0xccd00 @0xd28a0. Wird an `app+0xf0` gespeichert (`app` = `*DAT_002eb310`). `IDirectSound*` liegt bei +0x48 |
| SoundData ("Sample") | 0x70 | 0x2a3be8 (Basis 0x2a3920) | Konstruktor 0xcca70 / 0xca160, Verkettung in `mgr+8` |
| SoundVoice (Instanz) | 0xbc | 0x2a3b18 (Basis 0x2a38b8) | 0xcb300 / 0xccaa0 / 0xc9da0. `IDirectSoundBuffer*` bei +0xa4, `IDirectSoundStream*` bei +0x74 |

Wichtige virtuelle Slots, gelesen mit `avi_vtables.py`:

* **SoundManager 0x2a3b80:**
  * [1] 0xcbcb0 `Update` pro Frame: Listener-Position und -Orientierung aus der Kamera, Velocity = globaler Vektor 0x2f90bc (BSS; im `.text` gibt es keinen schreibenden Zugriff darauf, nur Lese- und Adress-Übergaben, `verify_avi_writers.py`; also praktisch der Nullvektor, Vermutung: NiPoint3::ZERO), dann FUN_000c95f0 (Voices), `CommitDeferredSettings` und `DirectSoundDoWork`
  * [4] 0xccee0 `GetSound(name, bStream)`: sucht per Namensvergleich ohne Endung, sonst `new SoundData` + `LoadByName`
  * [5] 0xcce40 `CreateSoundFromFileObject`: aus einem eingebetteten `NiUDSFileObject`
  * [9] 0xc9650 `GetCategoryVolume(i)` → +0x34/+0x38/+0x3c (3 Kategorien)
  * [22] 0xcbfa0 `SetEnvironment(0..4)`
* **SoundData 0x2a3be8:**
  * [0] 0xcb960 WAV-Parse aus einer Datei (Fiber-Callback)
  * [2] 0xcb300 bzw. [4] 0xcb3b0 `CreateVoice`; [4] erlaubt **max. 8 Voices pro Sample** (`if (iVar2 < 8)`)
  * [9] 0xccc00 `LoadByName(name, bStream)`
  * [10] 0xcb450 `LoadFromUDSFileObject`
  * [11] 0xcb720 WAV-Parse aus dem Speicher
* **SoundVoice 0x2a3b18:**
  * [0] 0xcb6e0 Stream-Pumpe (Fiber)
  * [5] 0xcc390 / [6] 0xcc4b0 / [7] 0xcc410 `Play`-Varianten
  * [8] 0xcb540 `Stop`
  * [10] 0xcb580 `Reset`
  * [11] 0xcc5e0 `Update` (alle 3D-, Volume- und Frequency-Aufrufe)

#### 2.4.2 Init (0xccd00)

1. `DirectSoundCreate(0,&ds,0)`.
2. `dsstdfx.bin` laden und per `DownloadEffectsImage` übergeben (0xcbeb0).
3. Nur wenn die Datei geöffnet werden konnte, geht es weiter (der HRESULT von `DownloadEffectsImage` ist egal, siehe 2.1):
   * `DirectSoundUseFullHRTF()`
   * `SetDopplerFactor(2.0, immediate)`
   * `SetDistanceFactor(0.2)`
   * `SetRolloffFactor(1.0)`
   * `SetPosition(0)`
   * `SetI3DL2Listener(&0x2d6884)`
   * Rolloff-Kurve bei 0x2ead3c (BSS) mit 6 Punkten: `{1.0, 0.9, 0.8, 0.5, 0.25, 0.0}`, per `mov dword [..], imm` in 0xccdcd..0xccdff geschrieben

#### 2.4.3 Laden (WAV)

* `GetSound` erreicht man über den Wrapper FUN_000c97d0(name, bStream). Er hat **144 direkte Callsites**: 131 in 38 Ghidra-Funktionen und 13 in Code, den Ghidra nicht als Funktion kennt (`avi_getsound_sites.out`, `verify_avi_callsites.out`).
* Beispiel: Die Frontend-Musik wird mit `push 1` gestreamt (0x219c6 → `"Sounds/Music/01 - front-end.wav"`), lokalisierte Sprache mit `bStream=0` (0x219ea).
* Alle Callsites mit `bStream=1` (Rohscan mit Rückverfolgung der Pushes, `verify_avi_r2_getsound.out`): 0x219e2 (Frontend-Musik), 0xa645d (`sounds/music/EndOfLevel_theme.wav`, `edi=1`), 0x1ee1b (`sprintf("sounds/%s.wav", …)`; Vermutung: Musik-Jukebox im Extras-Menü, String `"Music/01 - front-end"` 0x293fa0 wird von 0x1f0e1 referenziert) und 0xd88d6 (lokalisierte Sprachzeilen, siehe unten). 124 Sites übergeben nachweislich 0: 87 als Literal, 35 über ein vorher genulltes Register und 2 von Hand geprüfte (0x219ea `push ebx`, 0x2c29a `push 0`). 16 Register-Argumente ließen sich nicht statisch auflösen: 12 in FUN_0004b5b0, 3 in FUN_000e3ac0 (Ghidra-C zeigt dort 0) und 1 bei 0x33444. Zusammen mit den 4 Streams sind das alle 144 Sites.
* **`GetSound` (0xccee0) ignoriert `bStream`, wenn der Name schon geladen ist.** Der Namensvergleich `_strnicmp(name, data->name, strlen(name)-3)` findet ein vorhandenes SoundData und gibt es zurück; nur der erste Aufruf entscheidet über Stream oder Buffer. (Runde 2)
* Ein zweiter Ladeweg nimmt WAV-Bytes aus `NiUDSFileObject`-Objekten in NIF-Dateien (UDS-NetImmerse-Erweiterung). Das geschieht z.B. im Fußschritt-/Sound-Setup 0xc5b40, der auch `Sounds\SoundLookup.nif` (10014 Bytes, in `FutPAL03.img`) verwendet.
* **Parser 0xcb720 (Speicher) und 0xcb960 (Datei)**, belegt durch die Disassembly:
  * Er prüft `RIFF`/`WAVE`, kopiert `fmt ` nach `SoundData+0x34` (WAVEFORMATEX) und setzt `cbSize` (+0x44) **immer auf 0x12**. Daraus folgt: Der Host darf bei PCM nicht auf `cbSize` vertrauen.
  * Bei `wFormatTag==0x69` baut er an **SoundData+0x59 (unaligned)** ein XBOXADPCMWAVEFORMAT: `nBlockAlign=36*ch`, `wBitsPerSample=4`, `cbSize=2`, `wSamplesPerBlock=64`. Der Buffer wird dann mit `lpwfxFormat=SoundData+0x59` erzeugt.
  * **`nAvgBytesPerSec` (+0x61) ist falsch berechnet:** Der Code multipliziert `nBlockAlign` mit der **Kanalzahl**, nicht mit der Rate (`lea eax,[ecx+ecx*8]; shl eax,2; ... movzx ecx,cx; imul eax,ecx; ... sar eax,6` @0xcb831..0xcb84d und 0xcba81..0xcba9d). Ergebnis: `36*ch*ch/64`, also 0 bei mono und 2 bei stereo. *(Korrigiert; vorher stand hier `rate*36*ch/64`.)* Die WAV-Dateien selbst enthalten den richtigen Wert (alle 2943 ADPCM-Dateien, `verify_avi_assets.out`), XMV rechnet ebenfalls richtig (0x2647c6). Der Host darf `nAvgBytesPerSec` aus diesem Format nicht verwenden.
  * **Stream-Entscheidung** (nur im Datei-Parser 0xcb960, 0xcbaab..0xcbabb): `bStream && dataSize >= nAvgBytesPerSec` aus dem **rohen** `fmt `-Chunk (+0x3c, mindestens 1 s). Dann gilt:
    * Paketgröße `+0x4c = rate*blockAlign*3/10`, abgerundet auf `blockAlign` (= 0,3 s bei PCM). Bei einer ADPCM-Datei ergibt dieselbe Formel 19,2 s pro Paket (22050 Hz mono: 238140 Bytes, 10 Pakete = 2,38 MB). *Korrigiert in Runde 2:* Das kommt vor. FUN_000d8720 fordert lokalisierte Sprachdateien (Name aus der Sprach-DB, `"%s\\%s"` + `.wav`) mit `GetSound(name, 1)` an (0xd88ce `push 1`, Call 0xd88d6). Alle 2028 Dateien unter `sounds/localized` sind Xbox-ADPCM (`verify_avi_r2_disc.out`). Jede solche Datei ab 1 s Länge wird also als ADPCM-Stream mit `lpwfxFormat = SoundData+0x34` (rohes `fmt `, `cbSize` auf 0x12 gezwungen, `wSamplesPerBlock` an +0x46) angelegt. Ob dieser Zweig zur Laufzeit genommen wird, hängt von `param_1[2]/[3]` ab und ist nicht ausgewertet.
    * Puffer = 10 Pakete (+0x14 = 10*Paket)
  * Sonst wird alles in den Speicher geladen, und `SetBufferData(+0x48, +0x14)` setzt die Daten.
  * Mono (`nChannels<=1`) setzt das Flag "3D-fähig" (+0x25).
* **Asynchrones Laden:** Das Datei-Objekt FUN_0013f250 (0x6c Bytes) liest über `CreateFiber`/`SwitchToFiber`/`ConvertThreadToFiber` (0x13f42e..). Das ist ein Hinweis für die XAPI-HLE: **Fibers müssen funktionieren.**
  * *Korrigiert in Runde 2 (vorher „Windows-Fibers passen 1:1“):* Der Spielcode liest die aktuelle Fiber **inline** aus dem XAPI-TLS-Block statt über eine API. Dabei gilt `mov ecx,fs:[4]; mov edx,[ecx+_tls_index*4]; mov eax,[edx+0xc]` @0x13f443 (FUN_0013f250) und `cmp eax,[edx+0xc]` @0x13f5d5..0x13f5df (FUN_0013f590), `_tls_index` liegt bei 0x2f3d18.
  * `XAPILIB::SwitchToFiber` (0x17094f) tauscht nur `esp`, `fs:[0]` und genau diesen Slot `[TLS+0xC]`. `ConvertThreadToFiber` (0x17098e) legt seine Fiber-Struktur in `[TLS+0x10]` an und liest `fs:[0x28]`→`+0x1C`. `CreateFiber`/`DeleteFiber` holen den Stack über Kernel-Thunk 0x29370c (Ordinal 169, `MmCreateKernelStack`) bzw. 0x293710 (Ordinal 170, `MmDeleteKernelStack`); Namen laut Ghidra-C und `kernel_imports.tsv`.
  * Ersetzt der Host die Fiber-API durch Windows-Fibers, ohne `[TLS+0xC]` nachzuführen, nimmt FUN_0013f590 den falschen Zweig. Er ruft dann `SwitchToFiber` auf die eigene Fiber, statt zurückzuschalten. Einfacher ist es, die XAPI-Fiber-Funktionen im Original zu lassen und nur die beiden Kernel-Stack-Funktionen sowie die `fs:[4]`/`fs:[0x28]`-Emulation bereitzustellen (Details in `cpu.md`, Abschnitt Fibers/TLS).

#### 2.4.4 Streaming (Musik)

* Die Voice-Pumpe 0xcb5b0 läuft über die 10 Paket-Slots.
* Für jeden Slot, dessen Status nicht `E_PENDING` ist, liest sie 0,3 s aus der Datei, füllt am Dateiende mit Nullen auf, schlägt die Position modulo der Datenlänge um (Loop) und ruft `stream->Process(&pkt, NULL)` über die Vtable auf.
* Start und Stop laufen über `IDirectSoundStream_Pause(s,0)` und `Pause(s,1)`.

#### 2.4.5 2D/3D und Mixbins (0xcc150, 0xcc5e0)

* **3D-Voice** (Sample mono und 3D gewünscht):
  * Flags 0x60010
  * `SetMixBins` mit 6 Einträgen `{6,8,7,9,10,3}`, Pegel jeweils 0
  * `SetRolloffCurve(curve,6)`
  * Pro Update `SetPosition` / `SetVelocity` / `SetMin` / `SetMaxDistance` (deferred)
  * `SetI3DL2Source` mit lRoom = -600 mB, alle übrigen Felder 0
  * Nach XDK sind die Mixbins 6..9 = XTLK_FL/FR/BL/BR (HRTF→Crosstalk), 10 = I3DL2-Send, 3 = LFE (Vermutung anhand XDK-Konstanten).
  * Die Zuordnung von -600 zu lRoom folgt aus der XDK-Struktur `DSI3DL2BUFFER` (Vermutung).
* **2D-Voice:**
  * `SetMixBins` mit 2 Einträgen (Bin 0 und 1), Pegel aus dem Pan-Wert (+0x28) berechnet (0xcc92f..0xcc9d3)
  * Genaue Formel (aus der FPU-Sequenz): `vol(bin0) = mB * min(1, 1+pan)`, `vol(bin1) = mB * min(1, 1-pan)`, wobei `mB` derselbe Wert ist, der auch an `SetVolume` geht. Folge: Bei voller Lautstärke (`mB = 0`) wirkt das Panning **gar nicht**, es skaliert nur eine vorhandene Dämpfung. Nach DSOUND-Semantik addieren sich Buffer- und Mixbin-Lautstärke in mB (XDK-Wissen, Vermutung). Ein originalgetreuer Host übernimmt das so.
  * `SetFrequency(sampleRate * pitch)` und `SetVolume(mB)` pro Update
* **Distanzen:** Min/Max-Distanz-Defaults sind 5.0/80.0 (SoundData-Konstruktor 0xca160: `param_1[10]=0x40a00000`, `[11]=0x42a00000`).
* **Velocity der Voices ist immer der Nullvektor:**
  * Voices mit Owner-Objekt (`voice+8 != 0`) nehmen 0xcb500, das einen statischen, auf 0 initialisierten Vektor liefert. Voices ohne Owner nehmen 0x2f90bc (0xcc647/0xcc742).
  * 0x2f90bc nutzt auch der Listener. Es liegt in BSS und wird im `.text` nirgends geschrieben (`verify_avi_writers.py`), ist also 0.
  * Daraus folgt: **Doppler hat praktisch keinen Effekt**, der Host darf ihn weglassen.
* **Umgebungen** (`SetEnvironment` = SoundManager-Slot 22, Spielnamen per String in 0xc96e0, Werte per `avi_i3dl2.py`):
  * Die Presets liegen in einer Tabelle mit 24 Einträgen à 0x34 Bytes ab 0x2d6818 (`.data`). Jeder Eintrag besteht aus `const char *name` und 12 Feldern `DSI3DL2LISTENER`; der erste Feldblock beginnt bei 0x2d681c. *Präzisiert in Runde 2:* Die Tabelle gehört wahrscheinlich zum **Sound-Modul des Spiels**, nicht zur DSOUND-Bibliothek. Ihre Namen (`"Default"` 0x2a3b10 … `"Underwater"` 0x2a3a18) liegen in `.rdata` direkt zwischen den Spiel-Strings `PlaySound` (0x2a3a04) und der Voice-Vtable 0x2a3b18. Referenziert wird sie nur aus 0xcbfa0/0xccd00. Die Daten der DSOUND-Bibliothek liegen dagegen in der Section `DSOUND` selbst (z.B. 0x2641a0, 0x263b14). Die Namen der 5 benutzten Einträge (Zeiger direkt vor der Preset-Adresse) und die Werte sind exakt die **Standard-I3DL2-Presets** (`verify_avi_misc.py`). Auf XAudio2 entsprechen sie direkt `XAUDIO2FX_I3DL2_PRESET_PADDEDCELL/ROOM/LIVINGROOM/STONEROOM/CONCERTHALL`.

| Index | Spielname (0xc96e0) | Tabellenname / I3DL2-Preset | Preset @ | lRoom, lRoomHF, DecayTime, DecayHFRatio, lReflections, ReflDelay, lReverb, RevDelay |
|---|---|---|---|---|
| 0 | PADDED ROOM (auch Init) | Padded Cell | 0x2d6884 | -1000, -6000, 0.17, 0.10, -1204, 0.001, 207, 0.002 |
| 1 | SMALL ROOM | Room | 0x2d68b8 | -1000, -454, 0.40, 0.83, -1646, 0.002, 53, 0.003 |
| 2 | LARGE ROOM | Living Room | 0x2d6920 | -1000, -6000, 0.50, 0.10, -1376, 0.003, -1104, 0.004 |
| 3 | LARGE HALL | Stone Room | 0x2d6954 | -1000, -300, 2.31, 0.64, -711, 0.012, 83, 0.017 |
| 4 | OPERA | Concert Hall | 0x2d69bc | -1000, -500, 3.92, 0.70, -1230, 0.020, -2, 0.029 |

Bei allen 5 benutzten Presets gilt: RolloffFactor 0, Diffusion 100, Density 100, HFReference 5000.

* **Animationsgesteuerte Sounds:** Text-Keys `PlaySound <name> <vol> <..>`, `PlaySound3D <name> ..` und `PlayFootstepSound` in 0xcaf60. Sie laufen über FUN_000cad70.

#### 2.4.6 Audio-Assets auf der Disc (`avi_img_audio.out`, 2982 WAVs in den `.img`-Archiven)

| Format | Anzahl | Bytes | Beispiel |
|---|---|---|---|
| Xbox ADPCM mono 22050 | 2837 | 70.1 MB | sounds/button.wav |
| Xbox ADPCM mono 44100 | 79 | 1.8 MB | sounds/footsteps/gravel1.wav |
| PCM16 stereo 22050 | 36 | 351.8 MB | 18 Musikstücke in `sounds/music/` von FutPAL00.img (z.B. `sounds/music/01 - front-end.wav`) und 18 Ambience-Loops in `sounds/levelN/ambient/` von FutPAL03.img (z.B. `sounds/level1/ambient/1-1_ambience.wav`); Pfade in Runde 2 korrigiert |
| Xbox ADPCM stereo 22050 | 18 | 2.6 MB | sounds/menu/theme22.wav (Pfad in Runde 2 korrigiert); 17 davon unter sounds/localized |
| Xbox ADPCM stereo/mono 48000 | 5+4 | 4.3 MB | 8 lokalisierte Dialoge (`sounds/localized/english/…`) und `sounds/reanimator.wav` (Runde 2 präzisiert) |
| PCM16 mono 22050 / PCM8 11025 / PCM16 stereo 44100 | je 1 | | `sounds/music/03 - sewers.wav`, `sounds/misc12.wav`, `sounds/level1/ambient/sewers.wav` |

ffmpeg dekodiert die ADPCM-WAVs als `adpcm_ima_xbox` mit genau 64 Samples pro 36-Byte-Block und Kanal, getestet an 3 Dateien (`avi_wav_probe.out`).

### 2.5 Empfohlene Host-Implementierung (DSOUND-HLE)

* **Backend: XAudio2 2.9** (bzw. XAudio2Redist) mit X3DAudio.
  * Eine Mastering-Voice, eine Reverb-Submix-Voice: `XAudio2CreateReverb` mit `XAUDIO2FX_I3DL2_PARAMETERS` und `ReverbConvertI3DL2ToNative`. Damit lassen sich die 5 Presets oben direkt übernehmen.
  * Portable Alternative: **OpenAL Soft** (EFX-Reverb hat I3DL2-kompatible Parameter, HRTF eingebaut).
  * SDL-Audio allein hätte kein 3D und keinen Reverb, man müsste selbst mischen.
* **Buffer:** Bei `SetBufferData` die Daten kopieren; Xbox-ADPCM dabei sofort nach PCM16 dekodieren (64 Samples/Block). Dauer und Bytes/s aus `nSamplesPerSec`, `nChannels` und `nBlockAlign` selbst berechnen, nicht aus `nAvgBytesPerSec` (siehe 2.4.3). `SetBufferData(0,0)` gibt frei. Danach eine XAudio2-Source-Voice pro Buffer und `Play(DSBPLAY_LOOPING=1)` als Loop. Die Bedeutung von Flag 1 folgt dem XDK; das Spiel übergibt `loop != 0` (0xcc3db).
* **Streams:** eine Source-Voice mit Paket-Queue (max. 10).
  * Format kommt immer aus `SoundData+0x34`: PCM16 (Musik) **oder Xbox-ADPCM** (lokalisierte Sprache über 0xd88d6, siehe 2.4.3). ADPCM-Pakete sind auf `nBlockAlign` gerundet und lassen sich paketweise dekodieren. `cbSize` (0x12) und bei ADPCM `nAvgBytesPerSec` nicht verwenden.
  * `Process` legt ein Paket ein und setzt `*pdwStatus = 0x8000000A` **sofort** (siehe 2.3, das Spiel tut das nicht selbst).
  * Der `OnBufferEnd`-Callback setzt `*pdwStatus = 0` und `*pdwCompletedSize`, falls vorhanden.
  * `Pause(0/1)` = Start/Stop, `FlushEx` = Queue leeren und Status auf 0. `GetStatus` liefert `PLAYING=0x10000` nur bei laufender Wiedergabe.
  * Die Vtable aus 2.2 nachbauen.
* **3D:**
  * Listener- und Emitter-Parameter sammeln, in `CommitDeferredSettings` mit `X3DAudioCalculate` anwenden.
  * Rolloff-Kurve als `X3DAUDIO_DISTANCE_CURVE` mit 6 Punkten zwischen MinDistance und MaxDistance.
  * Die Mixbins 6..9 als HRTF/Panning interpretieren, Mixbin 10 als Reverb-Send mit lRoom-Pegel.
  * `DirectSoundUseFullHRTF` ist optional, z.B. über HRTF-XAPO oder OpenAL-HRTF.
* **2D:** Die beiden Mixbin-Pegel (Bin 0/1) als Output-Matrix 2→2 setzen.
* **`DownloadEffectsImage`:** `S_OK` zurückgeben, `*ppDesc` mit einem Dummy-Desc füllen. Das Spiel liest weder den Rückgabewert noch den Desc (Stack-Variable in 0xcbeb0) und gibt den Bildpuffer sofort danach frei.
* **`DirectSoundDoWork`:** no-op. **`SynchPlayback`** wird nur vom Original-XMV aufgerufen (nach `Pause(s,2)` auf den Filmton-Streams). Solange XMV ersetzt ist, ist beides egal. Läuft dagegen das Original-XMV über der DSOUND-HLE, muss `Pause(2)` als "bereit, wartet auf Sync" und `SynchPlayback` als gemeinsamer Start umgesetzt werden; zwei no-ops würden den Filmton stumm lassen (Semantik von `Pause(2)` nach XDK, Vermutung).
* **Patching:** Jeden der 40 Einstiegspunkte mit einem 5-Byte-`jmp` auf eine stdcall-kompatible Host-Funktion überschreiben. DSOUND-Interna und die DSP-Images bleiben ungenutzt. Geprüft: Es gibt keine Funktionszeiger auf DSOUND/XMV/XPP-Einstiege im Spielcode (kein `push imm32`/`mov reg,imm32`/`call [abs]` darauf, `verify_avi_rawscan.out`), die Patches fangen also alle Aufrufe. Einzige Datenzeiger (Runde 2, `verify_avi_r2_rawscan.out`): Die XAPI-Bibliothekstabelle in `.XTLID` (0x31a2c0..0x31a2f4, Paare aus ID und Adresse) enthält die 7 XPP-Einstiege. Kein Code im Image liest `.XTLID` (Scan auf Zeiger in 0x31a0c0..0x31a2f8: kein Treffer). Für den Host ist das bedeutungslos (Vermutung: für Debug-Monitor/Tools).
* **Aufwand:** mittel. Die Logik ist klein; die Arbeit steckt in korrekter Paket-, Status- und Loop-Semantik und im Abgleich von 3D und Reverb nach Gehör.
* **Alternative später (re3-Stil):** die drei Spielklassen (SoundManager, SoundData, SoundVoice, etwa 16 KB Code in 0xc9xxx..0xcdxxx) direkt nativ auf XAudio2 neu schreiben. Dann entfällt die DSOUND-Emulation ganz.

---

## 3. XMV (Videowiedergabe)

### 3.1 Einstiegspunkte (Section `XMV`, 64 Funktionen, keine Symbole)

Die Namen sind aus Verhalten und Argumentzahl abgeleitet und als **Vermutung** markiert. Belegt sind Argumentzahl (`ret N`) und Verhalten.

| Adresse | Vermuteter XDK-Name | ret | Verhalten (Beleg) | Aufrufer |
|---|---|---|---|---|
| 0x264360 | XMVDecoder_CreateDecoderForPackets(dwFlags, pvFirstPacketHeader, Context, pfnGetNextPacket, pfnReleasePreviousPacket, ppDecoder) | 0x18 | liest Header: Breite +0x14, Höhe +0x18, Audio-Anzahl +0x20, Audio-Deskriptoren ab +0x24 (12 Bytes). `XMemAlloc`; `QueryPerformanceFrequency`; `D3DDevice_GetDisplayMode` (60 Hz → Frameperiode 0x10aef0, sonst `0x3e80000/Hz`, 16.16 ms). Liefert 0x8007000E bei OOM | 0xee220 |
| 0x264759 | XMVDecoder_GetVideoDescriptor(dec, pDesc) | 8 | `desc[0]=W`, `desc[1]=H`, `desc[3]=AudioStreamCount` (`desc[2]` wird nicht geschrieben) | 0xee220 |
| 0x2647c6 | XMVDecoder_EnableAudioStream(dec, iStream, dwFlags, pMixBins, ppStream) | 0x14 | `DirectSoundCreateStream` mit `{flags, 2 Pakete, wfx (0x69 → 36*ch/64 Samples), Callback 0x264775, ctx=dec}`. Flag 1 im Deskriptor aktiviert 3 Streams auf einmal | 0xee220 |
| 0x264943 | XMVDecoder_GetAudioStream? (dec, iStream, ppStream) | 0xc | `*pp = streams[i]; AddRef` | 0xee220 |
| 0x264962 | XMVDecoder_GetNextFrame(dec, pD3DSurface, pResult, pTimestamp) | 0x10 | `*pResult`: 0 = kein neuer Frame, 1 = neuer Frame, 2 = Dateiende, 3 = Fehler (Default). Ruft Paket-Callbacks, `Process`/`Discontinuity` auf den Streams, `Pause(2)` + `SynchPlayback`, `GetDisplayFieldStatus` (VBlank-Takt), `FUN_00265b0b` (Farbkonvertierung, schreibt per `D3DSurface_LockRect` in die Surface) | 0xede90 |
| 0x264fdf | XMVDecoder_GetPlaybackTimeMs? (dec) | 4 | ms seit Wiedergabestart (QPC); das Spiel nutzt es für Untertitel | 0xede90 |
| 0x264677 | XMVDecoder_CloseDecoder(dec) | 4 | `FlushEx` + Warten auf Stream-Status + `Release`, Datei-Handle schließen, `XMemFree` | 0xee080 |
| (0x264775) | intern: XMO-Stream-Callback | 0xc | Sync-Offset per QPC; kein API-Einstieg | |

Ein `CreateDecoderForFile` ist nicht gelinkt. Alle 64 XMV-Funktionen werden referenziert (Scan in diesem Bericht), es gibt also keinen toten Code.

**Abhängigkeiten von XMV:**

* DSOUND: Create, CreateStream, Pause, FlushEx, SynchPlayback, DoWork, Release, Vtable-Aufrufe
* D3D: `GetDisplayMode`, `GetDisplayFieldStatus`, `D3DSurface_GetDesc`, `LockRect`
* XAPI: `XMemAlloc`/`XMemFree`, QPC, `Sleep`, `GetOverlappedResult`/`CloseHandle`

Wird XMV als Ganzes ersetzt, entfällt das alles.

### 3.2 Movie-Player des Spiels

* **Aufruf:**
  * Die Methode 0xa2380 (App-Vtable 0x2a1810 Slot 6, keine Ghidra-Funktion) baut bei 0xa2434 per `sprintf` `"%s-%s"` aus Name und dem String bei 0x2e7b70.
  * **0x2e7b70 ist der Regions-Suffix:** FUN_000a26a0 schreibt `"NTSC"` (0x4353544e) wenn `app+0xe8 == 1`, sonst `"PAL"` (0x4c4150) (0xa2707/0xa2744); im selben Zweig setzt es 60.0 bzw. 50.0 (0x42700000/0x42480000) und die Frame-Zeit 1/60 bzw. 0.02. `app+0xe8` setzt FUN_000d26a0 **anhand der vorhandenen Archive**: `sysPAL.img`/`FutPAL%02d.img` → 0, `sysNTSC.img`/`FutNTSC%02d.img` → 1. Auf der Disc gibt es nur `-PAL.xmv`. Der Host muss also die PAL-Archivnamen sichtbar lassen, sonst sucht das Spiel `*-NTSC.xmv` und findet keine Filme.
  * Danach folgt FUN_000caf40 (Sound-Manager, vor dem Film) und dann bei 0xa244c FUN_000ee220 (`Open`). Liefert `Open` 0, räumt 0xa2455 den Player ab und die Methode gibt `false` zurück; wie die Aufrufer von Slot 6 darauf reagieren, ist nicht geprüft.
  * Pro Frame läuft FUN_000a24a0 → FUN_000ede90 (`Update`/`Draw`); FUN_000ee080 ist `Close`.
  * FUN_000ee080 (und damit `CloseDecoder` 0x264677) wird nur aus zwei Stellen gerufen (`verify_avi_r2_sitecount.out`, Rohscan):
    * aus dem Player-Destruktor FUN_000ee190, den 0xa23ae, 0xa2461, 0xa24e7 (App-Vtable-Slot 7, 0xa24c0) und 0xa25d8 aufrufen;
    * aus `Open` 0xee220 (Fehlerpfade 0xee367/0xee4dc).
  * Nach dem Filmende (Ergebnis 2) setzt `Update` nur das Fertig-Flag. `CloseDecoder` kommt erst, wenn die App den Player zerstört; die XMV-HLE muss den Decoder bis dahin gültig halten. (Runde 2)
* **`Open` (0xee220):**
  1. `"Video\\%s.xmv"` über das **spieleigene** Datei-Objekt FUN_0013f250 öffnen (Archiv bzw. Disc). Die Filme liegen in keinem `.img`, deshalb fällt FUN_0013f250 auf ein `CreateFile` mit `"D:\" + name` zurück. Belege: Präfix `"D:\\"` bei 0x2d7ad8, danach der Name, danach der BSS-String 0x30e598, der nie geschrieben wird und damit leer ist; `/` wird zu `\`; aufgerufen wird FUN_000f76e0 mit `(path, 0x80000000, 1, 0, 3, 0x68000000, 0)`, also CreateFileA-Signatur mit GENERIC_READ und OPEN_EXISTING (XDK-Konstanten). Auf der Disc heißt das Verzeichnis `/video`, der Host-VFS muss also `D:\` auf die Disc-Wurzel abbilden und Groß/Kleinschreibung ignorieren. (Runde 2)
  2. 0x1000 Bytes Header lesen, auf Offset 0 zurückspulen.
  3. `CreateDecoderForPackets(0, hdr, this, 0xedba0, 0xede60, &dec)`.
  4. Zwei Paketpuffer `XPhysicalAlloc(hdr.maxPacketSize)` anlegen und das erste Paket (`hdr.firstPacketSize`) ab Offset 0 lesen.
* **Paket-Callbacks** (stdcall, vom Spiel implementiert):
  * `GetNextPacket(ctx, void **ppPacket, DWORD *pNextSize)` @0xedba0 (`ret 0xc`) tauscht die Puffer und liefert `*pNextSize = *(DWORD*)packet`.
  * `ReleasePreviousPacket(ctx, LONGLONG offset, DWORD nextSize)` @0xede60 (`ret 0x10`) liest das nächste Paket **sequenziell** (der Offset wird ignoriert) über `file->Read`.
  * Die XMV-Datei ist eine lückenlose Paketkette; der erste DWORD jedes Pakets ist die Größe des nächsten.
* **Tonspur:**
  * Bei `AudioStreamCount==5` wählt FUN_000edbe0 die Spur nach Sprache: english 0, german 1, french 2, spanish 3, italian 4, Fallback 0.
  * Bei 1..4 Spuren wird Spur 0 genommen.
  * Danach `EnableAudioStream(dec,i,0,0,0)`, `GetAudioStream(dec,i,&s)`, `IDirectSoundStream_SetVolume(s, mB(Lautstärke-Kategorie 0))` und `s->Release()` (0xee3f2..0xee47d).
* **Bild:**
  * `D3DDevice_CreateTexture2(W, H, 1, 1, 0, 0x24, 3)` (0xee4xx). Nach Xbox-`d3d8types.h` ist 0x24 = `D3DFMT_YUY2` (XDK-Wissen). Konsistent damit sind `+0x58 = W*H*2` (16 bpp) und der Renderstate.
  * `GetSurfaceLevel2(tex,0)` liefert die Surface für `GetNextFrame`.
  * Gezeichnet wird ein Vollbild-`NiScreenPolygon` mit 4 Vertices (Konstruktor FUN_00106d50 setzt `NiScreenPolygon::vftable`) mit dieser Textur, eingerahmt von `D3DDevice_SetRenderState_YuvEnable(1)` / `(0)` um FUN_00100c50 (0xede90).
  * Bei Ergebnis 2 oder 3 setzt das Spiel das Fertig-Flag. Bei 0 vor dem ersten Frame wird nicht gezeichnet (Flag +0x41).
* **Untertitel:**
  * FUN_000ed8b0 lädt `"Video/%s.txt"` über FUN_0013fb10. Format: `start_marker_table`, dann `HH MM SS.s:KEY` bis `end_marker_table`. Die Texte kommen per Key aus `Language\L1A1-INT.ldb`, zeitgesteuert mit `GetPlaybackTimeMs`.
  * **Auf der PAL-Disc gibt es keine `Video/*.txt`**, weder in `/video` noch in einem `.img` (`avi_img_audio.py`; die einzigen `.txt` sind build/wm/readme/unlock).
  * FUN_0013fb10 liefert dann 0, und Untertitel bleiben aus. Vermutung: Das Feature ist auf dieser Disc tot.

### 3.3 Dateiformat (`avi_xmv_headers.out`)

Es gibt **47** `.xmv`-Dateien, alle in `/video` (das ISO hat insgesamt nur 59 Verzeichniseinträge; daher stammte die frühere, falsche Zahl 59). 36 haben 5 Spuren mit 44100 Hz, `Credits-PAL` 5 Spuren mit 22050 Hz, 7 Dateien 1 Spur mit 22050 Hz (Attract, 5 DVDTrailer, fox) und 3 Dateien 1 Spur mit 44100 Hz (SCI, temple_open, uds) (`verify_avi_iso.out`, in Runde 2 mit `verify_avi_r2_disc.out` erneut bestätigt).

**Bildrate** (nicht im Header; aus den Paket-pts aller 47 Dateien, Demux im Speicher direkt aus dem ISO, `verify_avi_r2_pyav.out`): 44 Dateien haben Abstände von 66/67 ms (≈15 fps). `Attract-PAL`, `Credits-PAL` und `SCI-PAL` haben 40 ms (25 fps). Viele 15-fps-Dateien enthalten einzelne Abstände von 40, 133, 200 ms bis über 2 s (z.B. `4-2-01-PAL` 2066 ms). Der Host muss also strikt nach pts takten.

| Offset | Feld | Wert (alle 47 Dateien) |
|---|---|---|
| +0x00 | next packet size | variiert (0x6000..0x28000) |
| +0x04 | this/first packet size | wird vom Spiel als erste Lesegröße benutzt |
| +0x08 | max packet size | Größe der zwei Spielpuffer |
| +0x0c | magic | `xobX` |
| +0x10 | version | 2 |
| +0x14/+0x18 | width/height | 640 x 480 |
| +0x1c | duration (ms) | 3,3 s .. 338,9 s |
| +0x20 | u16 Audio-Spuren | 5 (Story-Zwischensequenzen, Intro, Credits) oder 1 (Logos, Trailer, Attract) |
| +0x24.. | je 12 Bytes: u16 tag, u16 ch, u32 rate, u16 bits, u16 flags | immer 0x69 (Xbox ADPCM), 2 ch, 4 bit, flags 0. 44100 Hz, bei Credits/Trailer/Attract/fox 22050 Hz |

### 3.4 ffmpeg-Test (PyAV 18.0.0; libavformat 62.12.102, libavcodec 62.28.102)

Getestet wurden `samples\uds-PAL.xmv` (528 KB, 1 Spur) und `samples\2-1-03-PAL.xmv` (1,08 MB, 5 Spuren):

* Container `xmv` ("Microsoft XMV").
* Video: Stream 0 `wmv2` ("Windows Media Video 8"), Tag `WMV2`, `yuv420p`, 640x480, Zeitbasis 1/1000.
* Audio: Streams 1..5 `adpcm_ima_xbox`, 44100 Hz, 2 ch, `s16p`.
* **0 Dekodierfehler.** Ergebnisse:
  * `2-1-03`: 45 Videoframes (1 Keyframe), Abstände 66/67 ms (= 15 fps), je Tonspur 130944 Samples (2,969 s).
  * `uds`: 71 Frames, 260928 Samples (5,917 s). Hier sind die Frameabstände **nicht konstant** (zusätzlich je ein Abstand von 133 ms und 800 ms). Der Host muss also nach pts takten, nicht mit fester Rate.
* Der erste Frame wurde als PNG gespeichert und sichtgeprüft: korrektes Bild (`samples\2-1-03-PAL.xmv.frame0.png`).
* Runde 2: Beide Beispiele erneut vollständig dekodiert (gleiche Zahlen, 0 Fehler). Zusätzlich wurden **alle 47** Dateien ohne Extraktion über ein lesendes `IsoFile`-Objekt komplett gedemuxt (nur Demux, kein Decode). Alle melden `xmv`/`wmv2`, ohne Abbruch (`verify_avi_r2_pyav.out`).

### 3.5 Empfohlene Host-Implementierung (XMV-HLE mit libav*)

Die 7 Einstiegspunkte werden ersetzt.

* **`CreateDecoderForPackets`:**
  * Ein `AVIOContext` mit eigener `read`-Funktion holt die Bytes über die **Callbacks des Spiels**. Das sind native stdcall-Funktionszeiger im selben 32-Bit-Prozess und direkt aufrufbar.
  * Ablauf: zuerst `pfnGetNextPacket`, nach Verbrauch `pfnReleasePreviousPacket(ctx, offset, nextSize)`.
  * Die Paketkette ergibt exakt den Dateistrom, also funktioniert `avformat_open_input(..., "xmv")`.
  * Alternative: im Spiel-Movie-Player den Dateinamen abgreifen und die Datei über die Host-VFS öffnen. Das bedeutet mehr Eingriffe in den Spielcode.
* **`GetVideoDescriptor`:** Werte aus dem Header bzw. aus ffmpeg, `desc[3]` = Anzahl der Audio-Streams.
* **`EnableAudioStream` / `GetAudioStream`:**
  * Einen **Host-DSOUND-Stream** (dieselbe Klasse wie in 2.5) anlegen und mit dekodiertem PCM aus `adpcm_ima_xbox` füttern.
  * Grund: Das Spiel ruft auf dem zurückgegebenen Zeiger `IDirectSoundStream_SetVolume` und `vtbl->Release` auf.
* **`GetNextFrame`:**
  * Audio-Uhr bzw. QPC seit Start mit dem pts des nächsten Frames vergleichen; 1 bei neuem Frame, sonst 0, 2 am Ende.
  * Den Frame aus `yuv420p` nach **YUY2** packen und in den Speicher der übergebenen Surface schreiben, so wie das Original. Dann bleiben Spielcode und YuvEnable-Rendering unverändert.
  * Die D3D-HLE muss dafür Texturen im Format 0x24 und `D3DRS_YUVENABLE` unterstützen. Möglich ist Shader-Konvertierung, auf D3D11 auch `DXGI_FORMAT_YUY2`.
  * Pragmatische Alternative: Die D3D-HLE bildet 0x24 auf RGBA8 ab, und XMV-HLE schreibt per `sws_scale` direkt RGBA. YuvEnable wird dann ignoriert. Das koppelt beide HLE-Teile.
  * `*pTimestamp` ist optional (das Spiel übergibt 0).
* **`GetPlaybackTimeMs`:** ms seit Start. **`CloseDecoder`:** alles freigeben, Host-Stream stoppen.
* **Lizenz:** ffmpeg dynamisch als LGPL linken (wmv2 und adpcm brauchen kein GPL).
* **Aufwand:** mittel. Die eigentliche Arbeit ist A/V-Sync und die YUY2-Übergabe an die D3D-HLE. Demux und Decode sind durch ffmpeg gedeckt und getestet.

---

## 4. Input (XPP / XInput)

### 4.1 Genutzte APIs (Section `XPP`: 203 Funktionen, 13 benannt)

| Adresse | Funktion | ret | Sites | Aufrufer | Nutzung |
|---|---|---|---|---|---|
| 0x28cfe1 (→0x28c3b5) | XInitDevices(n, pPreallocTypes) | 8 | 1 | 0x9c2a0 | `n=1`, `{&g_DeviceType_Gamepad (0x28bf10), 4}` |
| 0x28cfe6 | XGetDevices(pType) | 4 | 2 | 0x9c2a0 | Gamepad → Port-Maske; **MU (0x28bfc0) einmal, Ergebnis verworfen** |
| 0x28d008 | XGetDeviceChanges(pType, &ins, &rem) | 12 | 1 | 0x9c140 | Hotplug pro Frame (0xd25d3, 0xf3004) |
| 0x28ce5c | XInputOpen(pType, port, slot, pPoll) | 16 | 2 | 0x9c140, 0x9c2a0 | Port aus Tabelle 0x2d65a0 = {0,1,2,3}, slot 0, NULL |
| 0x28ceb2 | XInputClose(h) | 4 | 3 | 0x9c140, 0x9c2a0 | |
| 0x28cebe | XInputGetState(h, &state) | 8 | 1 | nofunc@0x9c3e6 (Controller-Update 0x9c3d0, Vtable 0x2a14f0[0]) | bei Rückgabe ≠ 0: Zustand zurücksetzen |
| 0x28cf2a | XInputSetState(h, &feedback) | 8 | 1 | 0x9c220 | Rumble |

`XInputGetState` gibt im Original 0, 0x48F (Gerät nicht verbunden) oder 0x57 (ungültiger Parameter) zurück; Beleg: Konstanten in 0x28cebe.

**Nicht genutzt:** `XInputGetCapabilities`, `XInputPoll` (Auto-Poll), `XMountMU`/`XUnmountMU`, Voice/Headset (XHV) und Tastatur. Spielstände laufen über `XCreateSaveGame` & Co. auf der Festplatte; das gehört nicht zu diesem Thema.

**Statischer Initialisierer:** FUN_0028d254 (Init-Tabelle @0x2d3e68) kopiert den Wert des Kernel-Thunks 0x293580 (Ordinal 74, Ghidra-Name `IoInvalidDeviceRequest`) in 10 Slots einer MU-Treiber-Dispatch-Tabelle (0x28c004..). Der Thunk wird dabei nur **gelesen** (`mov eax,[0x293580]`, Thunk-Index 8, Rohwert im XBE 0x8000004a). Der Init kopiert also einfach, was der Loader in diesen Thunk-Slot geschrieben hat (oder den Rohwert 0x8000004a, wenn der Slot nicht aufgelöst wird); er stürzt in keinem Fall ab. Aufgerufen werden die 10 Slots nur vom XPP/MU-Treiber, und der läuft nach dem Ersetzen von `XInitDevices` nicht mehr (`MU_Init` 0x28c09c wird nur aus `XInitDevices` 0x28c3b5 gerufen). Ein gültiger Zeiger für Ordinal 74 ist daher sauber, aber nicht zwingend.

### 4.2 Input-Code des Spiels

* **Init (0x9c2a0):** Nach `XInitDevices` legt 0x9c2a0 selbst 4 Controller-Objekte à 0x19c Bytes an (Konstruktor 0x9bf50, Array 0x2e6f84). Für bereits gesteckte Ports (Maske aus `XGetDevices(Gamepad)`) folgt `XInputOpen`.
* **Pro Frame:** 0xd25d0 / 0xf2fe0 rufen `FUN_0009c140` (Hotplug) und `FUN_0009be10` auf (Update aller Controller, Rumble-Timer).
* **Controller-Objekt:**
  * 16 Tasten-Slots à 0x10 Bytes ab +4: +0 Schwelle, +4 Wert, +8 Vorwert, +0xc Flag (gelöscht, wenn Wert < Schwelle) (FUN_0009b3c0 / FUN_0009b7a0)
  * 2 Stick-Objekte à 0x18 Bytes ab +0x104
  * +0x140/+0x144 Rumble links/rechts (float 0..1), +0x148 Rumble-Dirty
  * +0x14c Port, +0x150 XInput-Handle
  * +0x154 `XINPUT_FEEDBACK` (Header 0x42 Bytes, `wLeftMotorSpeed` bei +0x196, `wRightMotorSpeed` bei +0x198)
* **Schwellen:** Der Basis-Konstruktor FUN_0009b7d0 setzt für alle 16 Kanäle 0x3b800000 = 1/256 und für die Stick-Achsen 0x3e19999a = 0.15. Der Konstruktor 0x9bf50 überschreibt **14 Kanäle** (0..11, 14, 15) mit 0x3df00000 = 0.1171875 und beide Achsen beider Sticks mit 0x3ecccccd = 0.4 (Vermutung: Richtungsschwelle oder Deadzone). Kanal 12/13 (Stick-Klicks, digital) behalten 1/256. *(Korrigiert; vorher „alle 16 Tasten“.)*
* **Skalierung** (0x9c3fd..): Analogtasten × 0.00390625 (1/256), Sticks × 3.0517578e-05 (1/32768).
* **"Aktiver Controller":** Solange `DAT_002e6fa4 == 0`, macht der erste Controller mit einer Eingabe sich selbst aktiv (Taste > 0.1171875 oder |Stick| > 0.4 → `DAT_002e6fa4 = this`). Das Spiel ist ein Einspieler-Spiel.

**Kanal-Mapping** (0x9c643..0x9c985; Bit- und Indexnamen nach XDK `XINPUT_GAMEPAD`):

| Kanal | Xbox-Eingabe | Quelle im State | PC-Vorschlag (XInput) |
|---|---|---|---|
| 0 | D-Pad links | `wButtons & 0x04` | DPAD_LEFT |
| 1 | D-Pad rechts | `wButtons & 0x08` | DPAD_RIGHT |
| 2 | D-Pad hoch | `wButtons & 0x01` | DPAD_UP |
| 3 | D-Pad runter | `wButtons & 0x02` | DPAD_DOWN |
| 4 | X | `bAnalogButtons[2]` | X (0/255) |
| 5 | B | `bAnalogButtons[1]` | B |
| 6 | Y | `bAnalogButtons[3]` | Y |
| 7 | A | `bAnalogButtons[0]` | A |
| 8 | Linker Trigger | `bAnalogButtons[6]` | bLeftTrigger |
| 9 | Rechter Trigger | `bAnalogButtons[7]` | bRightTrigger |
| 10 | START | `wButtons & 0x10` | START |
| 11 | BACK | `wButtons & 0x20` | BACK |
| 12 | Linker Stick-Klick | `wButtons & 0x40` | LEFT_THUMB |
| 13 | Rechter Stick-Klick | `wButtons & 0x80` | RIGHT_THUMB |
| 14 | WHITE | `bAnalogButtons[5]` | LB (0/255) |
| 15 | BLACK | `bAnalogButtons[4]` | RB (0/255) |
| Stick 0 / 1 | linker / rechter Stick | `sThumbLX/LY`, `sThumbRX/RY` (int16) | gleiche Wertebereiche; Y positiv = oben wie bei XInput; SDL invertiert Y |

**Rumble:**

* FUN_0009b6b0 `SetRumble(left, right, force)` hat **16 Callsites** (0x2198b, 0x28ab3, 0x28ae2, 0x297cf, 0x32e88, 0x32fa4, 0x334bb, 0x434a8, 0x5eff7, 0x5f0f6, 0xa5b90, 0xa81e0, 0xb04b2, 0xc22e6, 0xc83a3, 0xc89ce). Werte im Bereich 0..1 setzen das Dirty-Flag.
* FUN_0009ba40 (13 Callsites) schaltet die Vibration-Option (`DAT_002d659c`, Default 1) mit Timer.
* Das eigentliche Senden passiert in 0x9c220 am Ende von `Update`:
  * Nur wenn `feedback.dwStatus != 0x3E5` (ERROR_IO_PENDING, also kein Request mehr in Arbeit).
  * Motorwerte = float × 65535.0 (0x2a14f8), bei ausgeschalteter Option 0.
  * Danach `XInputSetState`.

### 4.3 Empfohlene Host-Implementierung

* **Backend:** **XInput 1.4** (Windows, gleiche Struktur- und Wertebereiche) oder **SDL3 Gamepad** (portabel, inklusive Rumble per `SDL_RumbleGamepad(low, high, ms)`). Tastatur und Maus mappt der Host auf ein virtuelles Gamepad.
* **`XInitDevices`:** Typen merken. **`XGetDevices(Gamepad)`:** Bitmaske der verbundenen Pads (Port 0..3). **`XGetDevices(MU)`:** 0.
* **`XGetDeviceChanges`:** Differenz zum letzten Aufruf als Insertion/Removal-Masken, Rückgabe BOOL (≠ 0 bei Änderung).
* **`XInputOpen` / `XInputClose`:** opakes Handle ≠ 0 pro Port.
* **`XInputGetState`:**
  * `dwPacketNumber` hochzählen.
  * `wButtons` aus D-Pad/START/BACK/Thumbs; die Bits im Low-Byte sind identisch zu XInput.
  * `bAnalogButtons[8]` = A, B, X, Y, BLACK, WHITE, LT, RT; digitale Tasten als 0/255.
  * Sticks unverändert übernehmen.
  * Rückgabe 0 bzw. 0x48F, wenn das Pad nicht verbunden ist.
* **`XInputSetState`:** Motoren weiterreichen und **`feedback.dwStatus` synchron auf 0 setzen**. Das Spiel selbst setzt `dwStatus` nur einmal im Konstruktor auf 0 (0x9c112); danach sendet es nur, wenn er ≠ 0x3E5 ist (0x9c229). Setzte der Host ihn auf 0x3E5 und nie zurück, käme kein Rumble-Update mehr an. Das Original-XPP schreibt außerdem in den Header-Reserve-Bereich (+0x40/+0x41 des Feedbacks, 0x28cf2a); das Spiel liest diese Bytes nicht.
* **Patching:** 7 Einstiegspunkte (bei `XInitDevices` den 5-Byte-Thunk 0x28cfe1). Die Adressen `g_DeviceType_*` (0x28bf10, 0x28bfc0) bleiben gültig, weil sie nur als Kennung verglichen werden.
* **Aufwand:** leicht (1-2 Tage).

---

## 5. Hinweise für die anderen Loader-Teile

1. **Statische Initialisierer in `_cinit`**: 4 aus DSOUND und 1 aus XPP laufen beim Start. Sie sind ungefährlich. Für den XPP-Init muss Kernel-Ordinal 74 einen nicht-null Wert haben (4.1).
2. **Fibers:** Das Datei-Streaming (FUN_0013f250/0x13f590) nutzt `ConvertThreadToFiber`, `CreateFiber(0x4000, 0x13f1a0, obj)`, `SwitchToFiber` und `DeleteFiber`. Sound-Laden und Musik-Streaming hängen daran. Der Spielcode liest die aktuelle Fiber inline aus `[TLS+0xC]` (0x13f443, 0x13f5d5). Ein Ersatz durch Windows-Fibers muss diesen Slot pflegen; einfacher ist, die XAPI-Fibers im Original zu lassen (2.4.3, `cpu.md`). Jedes gestreamte Voice-Objekt öffnet ein eigenes Datei-Objekt mit eigener Fiber (0xcc1e3..0xcc206).
3. **Von Ghidra nicht erkannte Funktionsstarts** mit Bezug zu diesem Thema: 0xcc5e0, 0xcb720, 0xcb960, 0xccc00, 0xccee0, 0xcbfa0, 0x9c3d0, 0x9c3c0, 0xc9630/0xc9650 sowie die meisten Slots der Vtables 0x2a3978, 0x2a3b80, 0x2a3b18, 0x2a3be8 und 0x2a14f0. Ein Re-Run der Ghidra-Analyse mit diesen Einstiegen würde den Callgraph vervollständigen.
4. **Einziger DSOUND-Aufruf außerhalb des Sound-Managers:** 0xee471 (Movie-Lautstärke). Einziger XMV-Nutzer: 0xede90/0xee080/0xee220.
5. **Dateinamen:** Das Spiel fordert `XBOX\dsstdfx.bin` und `Sounds/Music/01 - front-end.wav` an. Im Archiv `FutPAL00.img` liegen sie unter denselben Pfaden in Kleinschreibung (`xbox/dsstdfx.bin`, `sounds/music/01 - front-end.wav`). *Korrigiert in Runde 2; vorher stand hier „im Root“, das war ein Fehler des alten Archiv-Parsers bei verschachtelten Verzeichnissen. Belegt mit `tools/futimg.py` in `verify_avi_r2_disc.out`.* Den Abgleich (Groß/Klein, `/` vs. `\`) erledigt der Archiv-Lookup des Spiels (FUN_0013fe10); das gehört zum Dateisystem-Thema. Wird `XBOX\dsstdfx.bin` nicht gefunden, fehlt die komplette 3D-Audio-Initialisierung (2.1). Filme liegen nicht im Archiv, sondern als `D:\Video\*.xmv` lose auf der Disc (3.2).
6. **Regionswahl über Archivnamen:** FUN_000d26a0 setzt `app+0xe8` aus der Existenz von `sysPAL.img`/`FutPAL%02d.img` bzw. `sysNTSC.img`/`FutNTSC%02d.img`; FUN_000a26a0 leitet daraus den Film-Suffix (`PAL`/`NTSC`) und 50/60-Hz-Konstanten ab (3.2). Das Dateisystem-HLE darf die Archivnamen nicht umbenennen.
7. **Stream-Status:** `Process` muss `*pdwStatus = E_PENDING` setzen (2.3). Das gilt auch für den Stub in Schritt 1 unten.

## 6. Reihenfolge und Aufwand

| Schritt | Inhalt | Aufwand |
|---|---|---|
| 1 | XInput-HLE (7 Funktionen) und Stub-DSOUND: alle 40 Funktionen geben `S_OK` zurück, `GetStatus` liefert 0. Stream-Pakete: in `Process` auf E_PENDING setzen und nach Ablauf ihrer Spieldauer (z.B. 0,3 s per Timer) auf 0. Werden sie sofort als erledigt markiert, hängt das Spiel zwar nicht, aber die Pumpe liest bei jedem Durchlauf alle 10 Pakete neu von der Disc. Damit läuft das Spiel stumm, ohne zu hängen. *Einfachere Variante (Runde 2 geprüft):* `DirectSoundCreateBuffer`/`DirectSoundCreateStream` liefern `S_OK` mit `*pp = NULL`. Alle virtuellen Aufrufe am Stream sind im Spiel durch Null-Tests geschützt: Pumpe 0xcb5b6, Destruktor 0xcc0fd/0xcc10d, Movie-Lautstärke in 0xee220. Der GetStatus-Spin im Destruktor wird bei `+0xa4 == 0` übersprungen (0xcc0ab). Die übrigen C-Funktionen müssen nur `NULL` tolerieren und `GetStatus` muss `*pdwStatus = 0` schreiben | 1-2 Tage |
| 2 | XMV-Stub: `CreateDecoderForPackets` liefert Fehler oder `GetNextFrame` sofort Ergebnis 2. Movies werden übersprungen (Spielcode behandelt das in 0xede90 bzw. 0xa2455; was die Aufrufer der Film-Methode bei `false` tun, ist nicht geprüft, Ergebnis 2 ist daher der sicherere Weg) | < 1 Tag |
| 3 | DSOUND-HLE auf XAudio2: 2D-Buffer, ADPCM-Dekoder, Streams, danach 3D/X3DAudio und I3DL2-Reverb | 1-2 Wochen |
| 4 | XMV-HLE mit libav* (AVIO über Spiel-Callbacks, YUY2-Surface, Audio über Host-Streams) | ca. 1 Woche |

## 7. Offene Fragen

* Die exakten XDK-Namen von 0x264943 und 0x264fdf und die Bedeutung von `desc[2]` im Video-Deskriptor sind nur aus dem Verhalten erschlossen.
* Die Mixbin-Nummern (6..10, 3) und die Flag-Werte von `DSBUFFERDESC`/`DSSTREAMDESC` sind mit XDK-Wissen benannt, nicht aus dem Binary bewiesen. Die DSOUND-interne Auswertung (FUN_00248f91/FUN_00248c58) ist nicht nachgeprüft.
* Wie viele Voices maximal gleichzeitig laufen, ist statisch nicht bestimmbar. Belegt ist nur die Grenze von 8 pro Sample in einem der beiden CreateVoice-Wege.
* Die Belegung der drei Lautstärke-Kategorien 0/1/2 ist offen. Kategorie 0 wird für die Movie-Lautstärke benutzt (vermutlich Musik); Menü-Hinweise sind `MusicClip.wav`, `sfxloop.wav` und `speechloop.wav`.
* Die Spieler-Aktionen hinter den Kanälen 0..15 (z.B. welcher Kanal Springen auslöst) sind nicht ermittelt.
* Ob die Reihenfolge der 5 XMV-Tonspuren wirklich EN, DE, FR, ES, IT ist, steht nur fest, soweit das Spiel sie so anfordert (FUN_000edbe0). Angehört wurde nicht.
* Ob die 18 Ambience-Loops (PCM16 stereo) gestreamt oder komplett geladen werden, hängt vom `bStream`-Argument ihrer `GetSound`-Aufrufe ab. Unter den 144 direkten `GetSound`-Sites gibt es kein Literal mit `ambient`; geladen werden sie also über einen Namen aus Daten (Kandidat: 0x1ee1b `sounds/%s.wav` mit `bStream=1`, oder 16 nicht aufgelöste Register-Argumente). Weiter offen.
* Ob der ADPCM-Stream-Zweig in FUN_000d8720 (lokalisierte Sprache, `bStream=1`) zur Laufzeit genommen wird (`param_1[2] == 0 && param_1[3] == 0`), ist nicht ausgewertet.
* Wie die Aufrufer der Film-Methode (App-Vtable-Slot 6, 0xa2380) auf `false` reagieren, ist nicht geprüft.

## Verifikation

Unabhängige Nachprüfung aller 20 Kernaussagen und weiterer Einzelaussagen direkt gegen das XBE, den Ghidra-Export und das ISO (nur lesend). Eigene Skripte in `analysis\loader\scripts\`, Ausgaben in `scripts\out\`:

| Skript | Prüft | Ausgabe |
|---|---|---|
| `verify_avi_common.py` | Hilfsfunktionen (XBE, Funktionsliste, Capstone, Ghidra-C) | |
| `verify_avi_rawscan.py` | E8/E9 in **beliebige** Adressen von DSOUND/XMV/XPP (nicht nur Funktionsstarts), Grenzprüfung ab Funktionsstart, `FF15/FF25 [abs]`, alle imm32 gleich einem Einstieg, Section-Statistik | `verify_avi_rawscan.out` |
| `verify_avi_writers.py` | schreibende Zugriffe auf 0x2f90bc, 0x2ead3c, 0x2e7b70, 0x2d659c, 0x2e6fa4 | stdout |
| `verify_avi_callsites.py` | Callsite-Zahlen von 0x9b6b0, 0x9ba40, 0xc97d0, 0x13f250, 0x9c140, 0x9be10, 0x9c2a0, 0x9c220 | `verify_avi_callsites.out` |
| `verify_avi_iso.py` | eigener XDVDFS-Walk (Baum + linearer Sektor-Scan), Anzahl/Namen der `.xmv`, Header aller `.xmv` | `verify_avi_iso.out` |
| `verify_avi_assets.py` | eigener `.img`-Parser, vollständiger RIFF-Chunk-Walk aller 2982 WAVs, Sonderdateien | `verify_avi_assets.out` |
| `verify_avi_pyav.py` | erneuter ffmpeg/PyAV-Test der zwei Beispiele | `verify_avi_pyav.out` |
| `verify_avi_xmvrefs.py` | ob alle 64 XMV-Funktionen referenziert sind | `verify_avi_xmvrefs.out` |
| `verify_avi_misc.py` | E_PENDING-Verwendung, I3DL2-Tabelle mit Namen, Regions-Suffix, dsstdfx-Rückgabe, ADPCM-Formel, Schwellen-Kanäle, Callsites in 0xcc5e0 | `verify_avi_misc.out` |
| (Listings) | Disassembly 0xcc5e0..0xcca70 und 0x9c3d0..0x9c9c0 mit `verify_avi_common.pdis` | `verify_avi_cc5e0.lst`, `verify_avi_9c3d0.lst` |

### Kernaussagen

| # | Aussage (kurz) | Ergebnis | Beleg |
|---|---|---|---|
| 1 | 40 DSOUND-Einstiege, stdcall, „62 + 8 Sites ohne Thunk“ | **korrigiert:** 40 Einstiege und alle `ret N` bestätigt. Die 62 `.text`-Sites enthalten den Thunk; echte Sites: 61 Spiel + 8 XMV = **69** (70 mit Thunk). 38 vom Spiel, 2 nur von XMV (`SynchPlayback`, `FlushEx`) bestätigt | `verify_avi_rawscan.out` (totals: DSOUND←.text 62, DSOUND←XMV 8, 41 Ziele inkl. 0x24aebe) |
| 2 | Callgraph unvollständig; 0xcc5e0 mit 20 Sites; XInputGetState nur aus 0x9c3e6 | **teilweise korrigiert:** 0xcc5e0 (Vtable 0x2a3b18 Slot 11) enthält **22** DSOUND-Sites (frühes `ret` bei 0xcc646, Ende 0xcca62). XInputGetState nur bei 0x9c3e6 in 0x9c3d0 = Vtable 0x2a14f0[0] bestätigt. Callgraph: 42 Kanten, nicht 44 | `verify_avi_misc.out`, `verify_avi_cc5e0.lst` |
| 3 | Spiel ruft `Process` virtuell; Vtable bei +0 | **bestätigt** (0xcb6ab..0xcb6b7 `lea edx,[esp+0x18]; push edx; ...; push eax; call [ecx+0x10]`; Konstruktor schreibt 0x2a5f5c bei 0x248caa an +0; `CDirectSound_CreateSoundStream` gibt den Objektzeiger ohne Offset zurück; Slot 4 = 0x247676). **Ergänzt:** `Process` muss E_PENDING setzen | Disassembly, Ghidra-C 0x248ec0 |
| 4 | GetStatus-Schleife im Destruktor, XMV-Close wartet auf 0x10000 | **bestätigt** (0xcc0b3..0xcc0d0 ohne Sleep; 0x264677 `Sleep(1)`+`DoWork`). `Stop` ruft der Destruktor über `call [0x2a3b38]` (= Slot 8) | Disassembly 0xcc030 |
| 5 | Stream mit 10 Paketen à 0,3 s, Polling auf E_PENDING, Musik `push 1` | **bestätigt** (0xcc1bb `mov [esp+0x20],0xa`; 0xcbabd..0xcbae7; 0xcb5d4; 0x219c6/0x219c8) | Disassembly |
| 6 | ADPCM-Format an +0x59, `cbSize=0x12` | **teilweise korrigiert:** Lage, Tag, Block, `cbSize` bestätigt; `nAvgBytesPerSec` ist aber `36*ch*ch/64` (Bug im Spiel), nicht `rate*36*ch/64` | `verify_avi_misc.out` (0xcb831..0xcb84d) |
| 7 | Asset-Statistik | **bestätigt** (eigener Parser, identische Zahlen); **präzisiert:** die 36 PCM16-Stereo-Dateien sind 18 Musikstücke + 18 Ambience-Loops | `verify_avi_assets.out` |
| 8 | 3D-Konfiguration, Presets, Doppler wirkungslos | **bestätigt** (`push 0x40000000/0x3e4ccccd/0x3f800000`, Kurve per imm-Stores 0xccdcd..0xccdff). **Ergänzt:** Presets = Standard-I3DL2 Padded Cell/Room/Living Room/Stone Room/Concert Hall; 0x2f90bc wird nirgends direkt geschrieben | `verify_avi_misc.out`, `verify_avi_writers.py` |
| 9 | 7 XMV-Einstiege mit Argumentzahl | **bestätigt** (`ret` 24/4/8/20/12/16/4; Verhalten laut Ghidra-C). Namen bleiben Vermutung | `verify_avi_rawscan.out` |
| 10 | Paket-Callbacks 0xedba0/0xede60 | **bestätigt** (`ret 0xc`/`ret 0x10`, Offset ignoriert, `XPhysicalAlloc(hdr+8)` zweimal) | Disassembly, Ghidra-C 0xee220 |
| 11 | YUY2-Textur 0x24, YuvEnable, Sprachwahl | **bestätigt** (`CreateTexture2(W,H,1,1,0,0x24,3)`, `YuvEnable(1/0)` um FUN_00100c50, Fallback 0) | Ghidra-C 0xee220/0xede90/0xedbe0 |
| 12 | „Alle 59 .xmv“ | **korrigiert: 47** `.xmv`; 59 ist die Zahl aller Verzeichniseinträge des ISO. Header-Werte bestätigt | `verify_avi_iso.out` |
| 13 | ffmpeg demuxt/dekodiert fehlerfrei, 15 fps | **bestätigt** (Neulauf, gleiche Zahlen). **Ergänzt:** `uds` hat unregelmäßige Abstände (133/800 ms) | `verify_avi_pyav.out` |
| 14 | Untertitel-Code tot (keine `Video/*.txt`) | **bestätigt** (String 0x2a5700, FUN_0013fb10 liefert 0; einzige `.txt`: build/wm/readme/unlock) | `verify_avi_assets.out`, Ghidra-C |
| 15 | XInitDevices(1,{Gamepad,4}), Hotplug, MU-Abfrage verworfen | **bestätigt** (0x9c2bc..0x9c334; 0x28bfc0 wird auch von `MU_Init` referenziert) | Disassembly |
| 16 | Kanal-Mapping und Skalierung | **teilweise korrigiert:** Mapping-Tabelle und Konstanten vollständig bestätigt; Schwelle 0.1171875 aber nur für 14 Kanäle (12/13 behalten 1/256) | `verify_avi_misc.out`, `verify_avi_9c3d0.lst` |
| 17 | Rumble | **bestätigt** (16 identische Sites; `cmp eax,0x3e5` @0x9c229; 0x2a14f8 = 65535.0; 0x9c220 nur aus 0x9c996 bei gesetztem Dirty-Flag) | `verify_avi_callsites.out` |
| 18 | XPP-Initialisierer liest Thunk Ordinal 74 | **bestätigt** (Index 8, Rohwert 0x8000004a, Ghidra `IoInvalidDeviceRequest_exref`); Folgerung abgeschwächt: kein Absturzrisiko, der Wert wird nur vom ersetzten Treiber benutzt | Disassembly 0x28d254 |
| 19 | DOLBY = DSP-Mikrocode, irrelevant | **bestätigt** für die Kopie (zwei `FUN_0024f537`-Aufrufe); „AC-3/GP/EP“ als Vermutung markiert | `DSOUND_00250000.c:988/991` |
| 20 | Datei-Streaming nutzt Fibers | **bestätigt** (`ConvertThreadToFiber(0)`, `CreateFiber(0x4000,FUN_0013f1a0,this)`, `SwitchToFiber`/`DeleteFiber` in FUN_0013f590) | Ghidra-C, Callgraph |

### Weitere geprüfte Einzelaussagen (alle bestätigt, sofern nicht anders vermerkt)

* Section-Statistik: DSOUND 401 Funktionen / 116 benannt; XMV 64 / 0; XPP 203 / 13 von XbSymbolDatabase (+1 Ghidra-Name `thunk_FUN_0028f438`).
* `XAudioCalculatePitch` und `DirectSoundEnterCriticalSection` sind die einzigen benannten öffentlichen DSOUND-APIs ohne externen Aufrufer.
* Linker-Thunk 0xf9cbd (Bytes, Ziel 0x24aebe, Referenz nur aus DSOUND-Vtable 0x2a5fac).
* `DirectSoundCreate`: Singleton `DAT_002641a0`, Rückgabe `obj+8`; `CreateSoundBuffer`: Rückgabe `obj+0x1c` (`piVar3+7`).
* `DSBUFFERDESC`/`DSSTREAMDESC`/`XMEDIAPACKET`-Aufbau in 0xcc150/0xcb680 (ergänzt: Streams nehmen immer `+0x34`).
* Lautstärkeformel und Konstanten 0x2939f8 = 1.0, 0x293a18 = 0.0, 0x2a3be4 = -1000.0 (0xcc86f..0xcc901, 0xee441..0xee49a).
* Klassen: SoundManager 0x7c (`push 0x7c` vor 0xd28a0, Speicherung an `+0xf0` in FUN_000d26a0), SoundData 0x70 (`push 0x70` @0xccf4b), Voice 0xbc (0xcb3e6); Vtable-Slots per `avi_vtables.py`; Grenze 8 Voices (0xcb3e1).
* `GetSound`-Wrapper 144 Sites (präzisiert: 13 davon außerhalb von Ghidra-Funktionen).
* Mixbins `{6,8,7,9,10,3}` mit Pegel 0; `SetI3DL2Source` mit drittem LONG = -600, Rest 0 (0xcc9df..0xcca0e).
* Min/Max-Distanz 5.0/80.0 im SoundData-Basiskonstruktor 0xca160 (+0x28/+0x2c); die Voice holt sie über SoundData-Vtable +0x3c/+0x40 nach +0x30/+0x34 (0xc9da0).
* XMV: Header-Auswertung, Frameperiode 0x10aef0 bzw. `0x3e80000/Hz`, OOM-Code 0x8007000E; `GetNextFrame`-Ergebnis startet mit 3, 2 am Ende, 1 bei neuem Frame, 3 wird beim normalen Ende zu 0.
* `XInputGetState` gibt 0, 0x48F oder 0x57 zurück (kopiert den Zustand auch bei 0x48F).
* Port-Tabelle 0x2d65a0 = {0,1,2,3}; 4 Objekte à 0x19c; Hotplug-Sites 0xd25d3/0xf3004; FUN_0009ba40 13 Sites; Vibrations-Default `DAT_002d659c` = 1; aktiver Controller wird bei 0x9c63d gesetzt.
* Keine Funktionszeiger auf DSOUND/XMV/XPP-Einstiege im Spielcode; die einzigen imm32-Treffer (0x2484c7) sind Zufallsbytes ohne gültige Instruktion.
* „Alle 64 XMV-Funktionen werden referenziert“: **nicht widerlegt**, der Test ist aber schwach (zählt auch unausgerichtete 32-Bit-Treffer).

### Korrigiert bzw. ergänzt (im Text oben eingearbeitet)

1. Callsite-Zahlen (69 statt 70, 22 statt 20 in 0xcc5e0, Callgraph 42 statt 44).
2. Anzahl der `.xmv`-Dateien: 47 statt 59.
3. `nAvgBytesPerSec` im ADPCM-Format des Spiels ist falsch berechnet (Bug), Host muss selbst rechnen.
4. Schwellen: 14 statt 16 Kanäle; Reihenfolge der Slot-Felder (Schwelle, Wert, Vorwert, Flag).
5. 36 PCM16-Stereo-Dateien = 18 Musik + 18 Ambience.
6. **Neu:** `Process` muss `*pdwStatus = E_PENDING` setzen, das Spiel tut es nicht (sonst Doppel-Einreichung).
7. **Neu:** Film-Suffix `PAL`/`NTSC` wird aus den vorhandenen Archivnamen abgeleitet (FUN_000d26a0 → FUN_000a26a0); auf der Disc gibt es nur `-PAL.xmv`.
8. **Neu:** `XBOX\dsstdfx.bin` muss öffnbar sein, sonst entfällt die ganze 3D-Initialisierung inkl. Rolloff-Kurve; der HRESULT von `DownloadEffectsImage` wird ignoriert.
9. **Neu:** 2D-Panning-Formel `mB*min(1,1±pan)` (bei voller Lautstärke kein Panning).
10. **Neu:** `SetRolloffCurve` mit `dwApply=0`; Voice-Destruktor ruft `stream->Release` virtuell (0xcc107), in der TSV nachgetragen. (`voice+0x78` wird ebenfalls virtuell freigegeben, ist aber in allen Konstruktoren 0.)
11. **Neu:** I3DL2-Presets entsprechen den Standard-Presets (direkt als XAudio2-Presets nutzbar).
12. **Neu:** `Pause(2)`/`SynchPlayback` brauchen eine echte Implementierung, falls das Original-XMV über der DSOUND-HLE läuft.
13. **Neu:** Frame-Timing der XMVs ist nicht konstant (pts verwenden).
14. Der Stub in Schritt 1 sollte Pakete nicht sofort als erledigt melden (sonst Dauer-Lesen von Disc).
15. `avi_api_surface.tsv` (über `avi_make_tsv.py`) nachgezogen: Host-Hinweise für `DirectSoundCreate`, `IDirectSound_Release`, `DownloadEffectsImage`, `SynchPlayback`, `CreateBuffer`, `GetStatus`, `Pause`, `FlushEx`, `XInputSetState`, Vtable-Release-Aufrufer und `Process`-Statuspflicht. Zeilen und Site-Zahlen (61/8) bleiben konsistent. Die vorherige Fassung liegt als `scripts\out\avi_api_surface.before_verify.tsv`.

### Weiterhin unsicher

* Alle Bedeutungen aus dem XDK (Flag-Bits von `DSBUFFERDESC`, Mixbin-Nummern, `Pause(2)`, `D3DFMT_YUY2 = 0x24`, Feldnamen von `DSI3DL2BUFFER`) sind nicht aus dem Binary bewiesen. Für die I3DL2-Listener-Felder ist die Reihenfolge durch die exakte Übereinstimmung mit den Standard-Presets praktisch gesichert.
* 0x2f90bc = Nullvektor: Es gibt keinen direkten Schreibzugriff; ein Schreiben über einen berechneten Zeiger ist nicht ausgeschlossen (die Adresse wird u.a. an FUN_00068c60 übergeben).
* Reaktion der Aufrufer der Film-Methode auf `false`; ob die Ambience-Loops gestreamt werden; wie oft die Stream-Pumpe (Fiber) pro Frame läuft.
* „`XAudioCreateAdpcmFormat` ist nicht gelinkt“ heißt nur, dass XbSymbolDatabase keine solche Funktion erkannt hat.

### 2. Runde: unabhängige Nachprüfung (Skripte `scripts\verify_avi_r2_*.py`)

Die zweite Runde hat weder die Skripte noch die Ausgaben der ersten Runde benutzt. Alles wurde mit eigener Methodik neu erhoben: Linear-Sweep-Disassembly mit Capstone als Grenzprüfung, `tools/futimg.py` und `tools/xdvdfs.py` für das ISO (nur lesend), PyAV auf In-Memory-Datenströmen. Ausgaben in `scripts\out\verify_avi_r2_*`.

| Skript | Prüft | Ausgabe |
|---|---|---|
| `verify_avi_r2_common.py` | Hilfsfunktionen (XBE, Funktionsliste, Linear Sweep, Ghidra-C) | |
| `verify_avi_r2_rawscan.py` | E8/E9 aus allen Code-Sections in DSOUND/XMV/XPP, nur an Linear-Sweep-Instruktionsstarts und nur auf Funktionsstarts. Vergleich mit `callgraph.tsv`. Alle Dwords im Image, die einem Einstieg gleichen | `verify_avi_r2_rawscan.out` |
| `verify_avi_r2_callconv.py` | `ret N` aller 54 Einstiege, der Paket-Callbacks und von 0xc97d0/0x9b6b0 (rekursiv über alle Zweige) | `verify_avi_r2_callconv.out` |
| `verify_avi_r2_vtables.py` | Vtables 0x2a3978/0x2a3b80/0x2a3920/0x2a3be8/0x2a38b8/0x2a3b18/0x2a14f0/0x2a5f5c/0x2a1810/0x2a5fac und ihre Konstruktor-Stores | `verify_avi_r2_vtables.out` |
| `verify_avi_r2_dis.py` | annotiertes Disassembly beliebiger Bereiche (u.a. 0xcc5e0, 0x9c3d0, 0xcb5b0, 0xcb960, 0xcc150, 0xccd00, 0xcbeb0, 0xcbfa0, 0x9bf50, 0x9c220, 0x9c2a0, 0x1708b0..0x1709c4) | `verify_avi_r2_cc5e0.lst`, `verify_avi_r2_9c3d0.lst`, stdout |
| `verify_avi_r2_getsound.py` | alle 144 `GetSound`-Sites mit rückverfolgtem `bStream`/Namen | `verify_avi_r2_getsound.out` |
| `verify_avi_r2_sitecount.py` | Callsite-Zahlen von 21 Spielfunktionen (SetRumble, Hotplug, Movie-Player, Fiber-Klasse …) | `verify_avi_r2_sitecount.out` |
| `verify_avi_r2_inittab.py` | Einträge der C/C++-Init-Tabellen, die in DSOUND/XMV/XPP/D3D zeigen | `verify_avi_r2_inittab.out` |
| `verify_avi_r2_i3dl2.py` | komplette I3DL2-Tabelle (24 Einträge) | `verify_avi_r2_i3dl2.out` |
| `verify_avi_r2_writers.py` | alle Zugriffe auf 0x2f90bc, 0x2ead3c, 0x2eae50 | `verify_avi_r2_writers.out` |
| `verify_avi_r2_disc.py` | ISO: alle `.xmv`-Header, WAV-Formate je Verzeichnis und Sprache, `.txt`, PCM-Dateien mit vollem Archivpfad | `verify_avi_r2_disc.out` |
| `verify_avi_r2_pyav.py` | PyAV: beide Beispiele voll dekodiert; **alle 47 XMV** im Speicher gedemuxt (pts-Abstände) | `verify_avi_r2_pyav.out` |
| `verify_avi_r2_wav.py` | 3 ADPCM-WAVs im Speicher dekodiert (Samples = Blöcke·64), Musik-`fmt ` | `verify_avi_r2_wav.out` |
| `verify_avi_r2_tsvcheck.py` | `avi_api_surface.tsv` gegen Rohscan und `ret N` (54 Zeilen) | `verify_avi_r2_tsvcheck.out` |

**Kernaussagen (in der Fassung, die dem Prüfer vorgelegt wurde):**

| # | Ergebnis Runde 2 |
|---|---|
| 1 | **korrigiert (bestätigt Runde 1):** 41 Ziele inkl. Linker-Thunk-Ziel 0x24aebe. Aus `.text` kommen 62 Sites, **inklusive** des Thunks 0xf9cc0; echte Sites sind 61 Spiel + 8 XMV = 69. 38 Ziele ruft das Spiel, 2 nur XMV (`SynchPlayback`, `FlushEx`). Alle 40 `ret N` stimmen. Callgraph: 42 Kanten (34 `.text`, 8 XMV) |
| 2 | **korrigiert (bestätigt Runde 1):** 0xcc5e0 = Slot 11 von 0x2a3b18 mit **22** DSOUND-Sites (0xcc6b9..0xcca27; Rückkehr bei 0xcc646 und 0xcca62). `XInputGetState` nur bei 0x9c3e6 in 0x9c3d0 = 0x2a14f0[0], gesetzt im Konstruktor 0x9bf8b |
| 3 | **bestätigt:** `call [ecx+0x10]` @0xcb6b7. `CDirectSound_CreateSoundStream` liefert das 0x28-Byte-Objekt selbst, die Vtable 0x2a5f5c steht bei +0 (Konstruktor 0x248cac). Slots: [0] AddRef (`inc [this+8]`, ret 4), [1] Release, [2] GetInfo, [3] GetStatus (ret 8), [4] Process (ret 0xc), [5] Discontinuity |
| 4 | **bestätigt** (0xcc0b3..0xcc0d0; gilt nur für Buffer-Voices, Stream-Voices warten stattdessen per Fiber auf das Datei-Objekt 0xcc090..0xcc0a7; 0x2646ca..0x2646df) |
| 5 | **bestätigt**, ergänzt: Auch ADPCM-Dateien können gestreamt werden (lokalisierte Sprache, 0xd88d6). Original-`Process` setzt E_PENDING selbst (0x24b1d9) |
| 6 | **korrigiert (bestätigt Runde 1):** Lage/Tag/Block/`cbSize` stimmen; `nAvgBytesPerSec` = `36*ch*ch/64` (0xcba81..0xcba9d, gleiche Sequenz) |
| 7 | **Zahlen bestätigt** (eigener Parser über `futimg.py`, identische Statistik; 3 ADPCM-Dateien dekodiert: 7424/12992/161344 Samples = Erwartung). **Pfade korrigiert:** Musik in `sounds/music/`, Ambience in `sounds/levelN/ambient/`, `xbox/dsstdfx.bin` (nicht „Root“) |
| 8 | **bestätigt** (0xccd6d/0xccd8e/0xccda1, Kurve 0xccdcd..0xccdff, Jump-Table 0xcc010 → 5 Presets, Werte = Standard-I3DL2). 0x2f90bc: 681 Lesezugriffe, kein direkter Schreibzugriff, 3 Adressübergaben. **Präzisiert:** Tabelle ab 0x2d6818 mit 24 Einträgen; sie gehört vermutlich zum Spiel-Sound-Modul, nicht zur DSOUND-Bibliothek |
| 9 | **bestätigt** (`ret` 24/8/20/12/16/4/4; Verhalten aus Ghidra-C; `EnableAudioStream` rechnet `nAvgBytesPerSec` korrekt als `36*ch*rate>>6`) |
| 10 | **bestätigt** (0xedba0 `ret 0xc` tauscht Puffer +0x34/+0x38/+0x3c, `*pNextSize = *(DWORD*)packet`; 0xede60 `ret 0x10` liest `nextSize` Bytes, Offset ungenutzt) |
| 11 | **bestätigt** (`CreateTexture2(W,H,1,1,0,0x24,3)`, +0x58 = W·H·2, `YuvEnable(1)`/`(0)` um FUN_00100c50, Sprachwahl mit Fallback 0) |
| 12 | **korrigiert (bestätigt Runde 1):** 47 `.xmv`, alle Header-Werte mit eigenem Walk erneut bestätigt |
| 13 | **korrigiert (neu):** Demux/Decode fehlerfrei bestätigt. „15 fps“ gilt aber nur für 44 der 47 Dateien; `Attract-PAL`, `Credits-PAL` und `SCI-PAL` laufen mit 25 fps (40 ms). Viele Dateien haben einzelne längere Lücken |
| 14 | **bestätigt** (String 0x2a5700, FUN_0013fb10 gibt 0 zurück; einzige `.txt` im ISO/Archiv: build/wm/readme/unlock) |
| 15 | **bestätigt** (0x9c2bc..0x9c334; Ghidra-Symbol `XAPILIB::g_DeviceType_Gamepad` in FUN_0009c140; 0x28bfc0 wird von `MU_Init` 0x28c0ed referenziert) |
| 16 | **korrigiert (bestätigt Runde 1):** Mapping aller 16 Kanäle und beider Sticks bestätigt (0x9c643..0x9c985). 0.1171875 gilt für 14 Kanäle (0..11, 14, 15; 0x9bf78..0x9c0b7) |
| 17 | **bestätigt** (16 identische Sites; `cmp eax,0x3e5` @0x9c229; Motoren ×65535.0 nach +0x196/+0x198) |
| 18 | **bestätigt** (einziger XPP-Eintrag der Init-Tabelle @0x2d3e68; Thunk-Index 8, Rohwert 0x8000004a; kein Absturzrisiko) |
| 19 | **bestätigt** (800 Bytes ab 0x310740 und 0x5fc Bytes ab 0x310a60 per memcpy FUN_0024f537; Section-Flags 0x26) |
| 20 | **bestätigt**, aber Folgerung **korrigiert:** Fibers sind nicht 1:1 durch Windows-Fibers ersetzbar, weil das Spiel `[TLS+0xC]` inline liest (0x13f443, 0x13f5d5) |

**Weitere 28 Einzelaussagen geprüft.** 25 davon sind bestätigt:

* Section-Statistik (DSOUND 401/116, XMV 64/0, XPP 203/13).
* 42 benannte öffentliche DSOUND-APIs, davon 40 benutzt.
* Init-Tabellen: nur 4 DSOUND-Stores und 1 XPP-Init.
* `DirectSoundCreate`: Singleton, Rückgabe +8; `CreateSoundBuffer`: Rückgabe +0x1c.
* `DSBUFFERDESC`/`DSSTREAMDESC` (0xcc150).
* `XMEDIAPACKET`-Aufbau und Pumpe 0xcb5b0.
* Lautstärke- und Panning-Formel.
* `SetI3DL2Source` mit lRoom=-600.
* `DownloadEffectsImage`-Argumente; HRESULT wird ignoriert.
* I3DL2-Presetwerte.
* Statischer Nullvektor 0xcb500.
* Klassengrößen 0x7c/0x70/0xbc, `app+0xf0`, `+0x48`, Grenze von 8 Voices.
* Distanz-Defaults 5/80.
* `GetSound` mit 144 Sites (131 + 13), cdecl.
* XMV-Header-Auswertung, Frameperiode, `GetVideoDescriptor`, `GetAudioStream`, `GetPlaybackTimeMs`, `Pause(2)` + `SynchPlayback`, `FlushEx(s,0,0,1)`.
* Imports von XMV.
* Regions-Suffix und Archiv-Erkennung.
* Untertitel-Parser.
* XMV-Headertabelle für alle 47 Dateien.
* `XInputGetState`-Rückgaben 0/0x48F/0x57.
* Port-Tabelle, Vibrations-Default, Offsets im Controller-Objekt.
* `SetRumble` 16, `FUN_0009ba40` 13, `FUN_0009be10` 11 Sites, Hotplug-Sites.
* Linker-Thunk-Bytes.
* `avi_api_surface.tsv`: 54 Zeilen ohne Abweichung von Rohscan und `ret N`.
* ADPCM-Dekodierung mit 64 Samples/Block.

Drei waren falsch und sind korrigiert (Liste unten, Punkte 1–3).

**Korrigiert bzw. ergänzt in Runde 2 (im Text oben eingearbeitet):**

1. **Archivpfade:** Musik `sounds/music/*.wav`, Ambience `sounds/levelN/ambient/*.wav`, `xbox/dsstdfx.bin`. „Im Root“ war ein Parserfehler bei verschachtelten Verzeichnissen (Kurzfazit, 2.1, 2.4.6, 5.5).
2. **ADPCM-Streams sind möglich:** FUN_000d8720 lädt lokalisierte Sprache mit `bStream=1` (0xd88d6). Alle 2028 Dateien unter `sounds/localized` sind ADPCM, Paketgröße dann 19,2 s. Der Host-Stream muss ADPCM dekodieren (2.4.3, 2.5, TSV-Zeile `DirectSoundCreateStream`). Vorher stand dort „kommt praktisch nicht vor“.
3. **I3DL2-Tabelle:** beginnt bei 0x2d6818 (Namenszeiger) mit 24 Einträgen und gehört vermutlich zum Spiel-Sound-Modul, nicht zur DSOUND-Bibliothek (2.4.5).
4. **Fibers:** nicht „1:1 Windows-Fibers“. Der Spielcode liest die aktuelle Fiber aus `[TLS+0xC]`; empfohlen wird, die XAPI-Fibers im Original zu lassen (+`MmCreateKernelStack`/`MmDeleteKernelStack`, Ordinal 169/170) (2.4.3, 5.2).
5. **XMV-Bildrate:** 3 Dateien mit 25 fps; alle 47 Dateien von ffmpeg vollständig demuxbar (Kurzfazit, 3.3, 3.4).
6. **Neu:** Alle `bStream=1`-Sites von `GetSound` aufgelistet. `GetSound` ignoriert `bStream`, wenn der Name schon im Cache liegt (2.4.3).
7. **Neu:** Original-`Process` setzt `*pdwStatus = E_PENDING` selbst (FUN_0024b1c4 @0x24b1d9) und liefert 0x88780032, wenn der Stream keine Daten annimmt (2.3). Das stützt die Host-Regel aus Runde 1.
8. **Neu:** `.XTLID` (0x31a2c0..0x31a2f4) enthält Zeiger auf die 7 XPP-Einstiege, wird aber von keinem Code gelesen (2.5).
9. **Neu:** Filme werden per `CreateFile("D:\Video\…")` von der Disc gelesen, nicht aus einem Archiv. Der Decoder bleibt nach dem Filmende offen, bis der Player zerstört wird (3.2).
10. **Neu:** Einfachster DSOUND-Stub: `*pp = NULL` ist überall null-geprüft (Schritt 1 in Abschnitt 6).
11. TSV: Host-Hinweis für `DirectSoundCreateStream` (ADPCM), AddRef-Aufrufer 0x2647c6 und Beleg für `Process` ergänzt (über `avi_make_tsv.py` neu erzeugt; Vorversion `scripts\out\avi_api_surface.before_verify_r2.tsv`).

**Weiterhin unsicher (zusätzlich zur Liste der Runde 1):**

* Ob FUN_000d8720 zur Laufzeit den Stream-Zweig nimmt und wie die Ambience-Loops angefordert werden (16 `bStream`-Register-Argumente unaufgelöst, Kandidat 0x1ee1b).
* Die Zuordnung von 0x1ee1b zur Musik-Jukebox der Extras ist Vermutung.
* Die Herkunft der I3DL2-Tabelle (Spielmodul statt DSOUND-Bibliothek) ist aus der Lage der Strings geschlossen, nicht bewiesen.
* Die 25-fps-Messung stützt sich auf die Paket-pts des ffmpeg-Demuxers, nicht auf den XMV-Decoder des Spiels.
