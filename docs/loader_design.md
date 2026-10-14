# Loader-Design: Futurama-PC-Port („Loader-Weg“)

Stand: 2026-10-04 · Zielbinary: `orig\retail_eu\default.xbe` (Retail EU/PAL, SHA-1 `58F76DE5BC739BF016D74C1D7F23E348F65EA4B4`, 2 973 696 Byte)

Grundlage sind die sieben geprüften Analyseberichte in `analysis\loader\` und der empirische Toolchain-Test:

| Kürzel | Datei | Thema |
|---|---|---|
| [KRN] | `kernel.md`, `kernel_imports.tsv` | 111 Kernel-Thunks, Konventionen, Host-Plan |
| [CPU] | `cpu.md`, `fs_sites.tsv`, `cpu_priv_sites.tsv`, `cpu_hw_refs.tsv` | fs:-Zugriffe, privilegierte Befehle, MMIO, Mapping |
| [D3D] | `d3d.md`, `d3d_functions.tsv` | Xbox-D3D8-Bibliothek, HLE-Grenze |
| [REN] | `renderer.md`, `nixboxrenderer_vtable.tsv` | NiRenderer/NiXBoxRenderer |
| [AVI] | `avi.md`, `avi_api_surface.tsv` | DSOUND, XMV, XPP/XInput |
| [FIL] | `files.md` | Dateisystem, Archive, Spielstände |
| [STA] | `startup.md` | Start, Hauptschleife, Threads, Speicher |
| [PRB] | `launcher_probe.md`, `toolchain_probe\` | Launcher bei 0x10000 (empirisch getestet) |

Für dieses Dokument neu erhoben (reproduzierbar): `analysis\loader\scripts\design_header_refs.py` → `design_header_refs.out` (welche XBE-Header-Bytes Code direkt liest). Alle anderen Belege verweisen auf Abschnitte der Berichte. „Vermutung“ markiert, was nicht am Binary oder durch einen Test belegt ist.

---

## 0. Kurzfassung der Entscheidungen

| # | Thema | Entscheidung | Begründung (Kurz) |
|---|---|---|---|
| E1 | Prozessmodell | **Launcher-EXE** (`/BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE /SAFESEH:NO`, MASM-Platzhalter `.text$a` = 0x11000–0x31B000) + **Host-DLL** mit aller Logik | einzig getesteter Weg, 0x10000 zu bekommen [PRB] |
| E2 | XBE-Header | XBE-Header nach 0x10000 kopieren und als **PE/XBE-Polyglott** herrichten: `MZ` bei 0x10000, `e_lfanew` (0x1003C, liegt in der nie gelesenen RSA-Signatur) zeigt auf eine Kopie der PE-Header bei 0x10970 | Windows (`RtlImageNtHeader`, `CreateThread`, SEH) braucht weiter gültige PE-Header; Code liest den Header erst ab 0x10108 (Abschnitt 1.4) |
| E3 | FS-Segment | **Strategie A aus [CPU] 2.4**: die 38 fs:-Stellen mit Offset ≠ 0 bekommen ein neues disp32 auf `TEB.TlsSlots[k]` (`fs:[0xE10+4k]`); fs:[0] (SEH, 5168 Stellen) bleibt | gleiche Befehlslänge, pro Thread korrekt, Windows-TEB bleibt intakt; LDT unter WoW64 verworfen |
| E4 | Kernel | alle 111 Thunk-Slots aus einer **generierten Tabelle** (`kernel_imports.tsv`) füllen; 74 echte HLE-Funktionen, 37 protokollierende Stubs (nur von ersetzten Bibliotheken genutzt) | [KRN] 6/7 |
| E5 | XAPI | **XAPI bleibt Originalcode** (Heap, Datei-API, SaveGame, Signaturen, `XapiInitProcess`, `CreateThread`, CRT). Ersetzt werden nur `QueryPerformanceCounter/Frequency`, `OutputDebugStringA`, `XMountUtilityDrive`, `XLaunchNewImageA`, `SwitchToFiber` | [FIL] 5.1, [KRN] 8.8, [CPU] 2.5/3 |
| E6 | **Grafikgrenze** | **NiRenderer-Ebene**: `NiXBoxRenderer::Create` (0x14C170) wird umgeleitet und liefert einen eigenen `NiPCRenderer` mit 75-Slot-VTable. Die Xbox-D3D8-Bibliothek läuft **nie**; nur 8 vom Spiel direkt gerufene D3D-Funktionen bekommen einen kleinen Shim, alle übrigen D3D-Einstiege werden zu Fallen | Abschnitt 2.2 |
| E7 | Audio/Video/Input | API-HLE an den Einstiegspunkten: DSOUND 40 + Stream-VTable → XAudio2; XMV 7 → FFmpeg; XPP 7 → XInput 1.4 + Tastatur. Bis zum jeweiligen Meilenstein sichere Stubs | [AVI] 2.5/3.5/4.3/6 |
| E8 | Dateisystem | Emulation auf **Nt\*/Io\*-Ebene** mit eigenem Objekt-Namensraum; `D:` direkt aus dem ISO (XDVDFS), `\Device\Harddisk0\partition1` auf ein Host-Verzeichnis | [FIL] 5 |
| E9 | Fibers | Original-XAPI-Fibers bleiben (Spiel liest `[TLS+0xC]` inline); HLE für `MmCreateKernelStack/MmDeleteKernelStack` mit großen Stacks; `SwitchToFiber` (0x17094F) wird durch eine strukturgleiche Host-Version ersetzt, die zusätzlich `TEB.StackBase/StackLimit/DeallocationStack` umschaltet | [AVI] 2.4.3 + [CPU] 2.5 vereint |
| E10 | Threads | Genau ein Gast-Thread (Hauptthread 0xF8469) plus der Boot-Thread für `entry`. Host-Threads führen **nie** Gastcode aus | [STA] 4.1, [CPU] 0.7 |
| E11 | Hooks | 5-Byte-`jmp`-Detours an Funktionsanfängen, zentral registriert, gegen eine generierte Funktionstabelle geprüft, einzeln abschaltbar; re3-artige Ersetzung dekompilierter Funktionen | Abschnitt 3 |
| E12 | Spielstände | XAPI-SaveGame-API unverändert auf `partition1` = `%LOCALAPPDATA%\FuturamaPC\hdd\partition1\`, Signatur-Schlüssel konfigurierbar | [FIL] 4/5.3 |

---

## 1. Architektur

### 1.1 Prozess- und Thread-Modell

```
futurama.exe (Launcher, Basis 0x10000, fest)          futurama_host.dll (beliebige Basis)
  ├─ 0x10000  PE-Header  → wird XBE/PE-Polyglott         ├─ core   : Config, Log, Crash, XBE-Loader, Patcher, Hooks
  ├─ 0x11000  .text$a Platzhalter (0x30A000, 0xCC)      ├─ kernel : xboxkrnl-HLE (111 Slots), Handles, APCs
  │           → XBE-Sektionen 0x11000..0x31A2F8          ├─ vfs    : Namensraum, XDVDFS, Host-Verzeichnis
  ├─ 0x31B000 Launcher-Code (minimal, ohne CRT)          ├─ xapi   : 5 XAPI-Ersatzfunktionen
  └─ ruft HostMain(LauncherInfo*)                        ├─ gfx    : NiPCRenderer, D3D-Shim, RHI (D3D11 → Vulkan/D3D12)
                                                         ├─ audio, video, input : HLE der XDK-Bibliotheken
                                                         └─ game   : dekompilierter Spielcode (statische Lib)
```

| Thread | Erzeugt von | Aufgabe | darf Gastcode ausführen |
|---|---|---|---|
| UI-/Boot-Thread | Windows (Primärthread des Launchers) | Initialisierung, `entry` 0xF84DD (adoptiert, TlsData = 0), danach Fenster + Message-Loop, Tastaturzustand | nur während `entry` |
| Gast-Hauptthread | HLE-`PsCreateSystemThreadEx` (einziger Aufruf aus `CreateThread`, [STA] 4.1) | `XapiThreadStartup` → 0xF8469 → `XapiInitProcess`, `_cinit`, `main`, Spiel, Rendern, Dateifibers | ja (adoptiert) |
| Host-Worker | XAudio2, FFmpeg, ggf. Datei-Prefetch | Audio-Callbacks schreiben nur Status-DWORDs in Gastspeicher (`*pdwStatus = 0`, [AVI] 2.3) | **nein** |

Der Renderer arbeitet auf dem Gast-Hauptthread (und dessen Fibers, gleicher OS-Thread). DXGI-Swapchains dürfen ein Fenster eines anderen Threads benutzen; der UI-Thread pumpt die Nachrichten. Gegenseitiger Ausschluss im Gastcode läuft über IRQL und Critical Sections, nicht über `lock`-Präfixe ([CPU] 3, Uniprozessor-Annahme). Weil nur ein Gast-Thread existiert, ist das unkritisch, solange Regel E10 gilt.

### 1.2 Adressraum (32 Bit, WoW64, LARGEADDRESSAWARE)

| Bereich | Inhalt | Schutz |
|---|---|---|
| 0x00010000–0x00010FFF | XBE-Header (0x968 Byte) + PE-Header-Kopie ab 0x10970 (Polyglott, 1.4) | RW |
| 0x00011000–0x0031A2F8 | XBE-Sektionen inkl. BSS, `$$XTIMAGE`, `.XTLID` | M1: RWX durchgehend; ab M9 optional `.text` RX außer Grenzseiten |
| 0x0031B000– | Launcher-Code/Daten | Image |
| frei, bottom-up | Gast-Allokationen (`NtAllocateVirtualMemory`, Kernel-Stacks, Contiguous) | RW |
| beliebig | Host-DLL, Treiber, FFmpeg, Host-Heaps | Windows |
| 0xFD000000–0xFEFFFFFF (Debug-Option) | als `PAGE_NOACCESS` reserviert: NV2A/APU/AC97/OHCI-MMIO-Fänger ([CPU] 4, Vermutung zur Machbarkeit) | NOACCESS |

Regel: Gastspeicher (alles, was HLE an Gastcode zurückgibt) muss unter 0x80000000 liegen. Auf der Xbox liegen Titel-Allokationen unter 2 GiB. Gastcode könnte Zeiger vorzeichenbehaftet vergleichen oder Bit 31 als Marke nutzen; im Speichermanager wird Bit 31 nur in Größen-Headern benutzt ([CPU] 4), für den übrigen Code ist das ungeprüft. Der Allocator-HLE prüft das per Assertion. Bei Engpässen wird ein frühes Arena-Reservat unter 2 GiB eingeführt. Contiguous-Speicher braucht **kein** 0x80000000-Fenster, weil D3D nicht läuft (E6). Den einzigen Spielnutzer (Movie-Player, `XPhysicalAlloc` mit PhysicalAddress = −1, [KRN] Verifikation) bedient normaler Speicher.

### 1.3 Launcher (`futurama.exe`)

Vorgehen wie in [PRB] getestet, mit drei Ergänzungen:

1. **Link-Optionen** auf der Kommandozeile, nicht per `#pragma` ([PRB]: LNK4229): `/BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE /SAFESEH:NO /INCREMENTAL:NO /NODEFAULTLIB /ENTRY:LauncherEntry /SUBSYSTEM:WINDOWS kernel32.lib user32.lib`.
   - `/INCREMENTAL:NO`: Inkrementelles Linken fügt Thunks und Padding ein und würde die Lage des Platzhalters gefährden.
   - `/SAFESEH:NO`: Die 1117 `___CxxFrameHandler`-Thunks und `_except_handler3` 0x168DCC liegen im Image-Bereich des Launchers ([CPU] 2.5 Punkt 4). Ohne SafeSEH-Tabelle akzeptiert `RtlIsValidHandler` sie (Vermutung, Test in M1).
2. **Platzhalter** `placeholder.asm` aus [PRB]: Code-Segment `ALIAS(".text$a") READ EXECUTE 'CODE'`, 0x30A000 Byte. Der Launcher prüft beim Start `&xbe_space == 0x11000` und `0x11000 + size >= 0x31A2F8`.
3. **Ablauf `LauncherEntry`** (ohne CRT, kein `__declspec(thread)`):
   1. Fünf `TlsAlloc()`-Slots **vor** jedem `LoadLibrary`: S_TLS, S_KTHREAD, S_PRCB, S_IRQL, S_DPC. Jeder Index muss < 64 sein, sonst Abbruch mit Meldung ([CPU] 2.4 Risiko a).
   2. Debug-Option: MMIO-Fänger reservieren (1.2).
   3. `LoadLibraryW(L"futurama_host.dll")`, `GetProcAddress("HostMain")`.
   4. `HostMain(&LauncherInfo{ size, tlsSlots[5], xbeSpace=0x11000, xbeSpaceSize, cmdline })` aufrufen; kehrt erst bei Prozessende zurück.

### 1.4 XBE laden und mappen

Ablauf in `core/xbe_loader` (Host-DLL, Boot-Thread):

1. **XBE beschaffen:** `default.xbe` direkt aus dem ISO lesen (XDVDFS-Reader, 4.2) oder aus einer konfigurierten Datei. SHA-1 muss `58F76DE5…A4B4` sein, sonst Abbruch: Alle Adressen sind build-spezifisch.
2. **Header prüfen:** Magic `XBEH`, Basis 0x10000, `SizeOfImage` 0x30A300, Entry (XOR Retail-Key) = 0xF84DD, Thunk-Tabelle (XOR) = 0x293560 ([KRN] 1).
3. `VirtualProtect(0x10000, 0x30B000, PAGE_EXECUTE_READWRITE)`; danach den ganzen Bereich 0x11000–0x31B000 mit 0 füllen. Damit sind alle BSS-Enden und Lücken zwischen den Sektionen genullt ([CPU] 5: BSS gibt es außer bei `.data` auch bei D3D, DSOUND, XMV, `.rdata`, DOLBY).
4. **Sektionen kopieren**, alle 10 inklusive der nicht vorgeladenen `$$XTIMAGE` (0x3178C0, 0x2800) und `.XTLID` (0x31A0C0). `XapiInitProcess` lädt `$$XTIMAGE` per `XeLoadSection` und schreibt es nach `TitleImage.xbx` ([KRN] 7, [FIL] 1.1).

   | Sektion | VA | VSize | Raw | BSS |
   |---|---|---|---|---|
   | .text | 0x011000 | 0x2232C0 | 0x2232C0 | – |
   | D3D | 0x2342C0 | 0x12A38 | 0xF5D0 | 0x3468 |
   | DSOUND | 0x246D00 | 0x1D654 | 0x1D40C | 0x248 |
   | XMV | 0x264360 | 0x27B6C | 0x27B5C | 0x10 |
   | XPP | 0x28BEE0 | 0x767C | 0x767C | – |
   | .rdata | 0x293560 | 0x2A97C | 0x2A96C | 0x10 |
   | .data | 0x2BDEE0 | 0x52858 | 0x1B558 | 0x37300 |
   | DOLBY | 0x310740 | 0x7180 | 0x716C | 0x14 |
   | $$XTIMAGE | 0x3178C0 | 0x2800 | 0x2800 | – |
   | .XTLID | 0x31A0C0 | 0x238 | 0x238 | – |

5. **Header-Polyglott** (E2):
   - Die PE-Header des Launchers (bei der Probe `e_lfanew` = 0xD8, NT-Header plus 2 Sektionen = 0x148 Byte) werden vorher gesichert.
   - Danach werden die 0x968 Byte XBE-Header nach 0x10000 kopiert.
   - Dann `WORD [0x10000] = 'MZ'` und `DWORD [0x1003C] = 0x970`, und die gesicherten NT-Header plus Sektionstabelle kommen nach 0x10970. In der Seite sind 0x10968–0x10FFF frei (0x698 Byte).
   - **Beleg** (`design_header_refs.out`): Absolute Speicherzugriffe des Codes auf den Header gibt es nur ab 0x10108 (0x10108, 0x10118 ×14, 0x1011C, 0x10120, 0x10124 ×6, 0x10130 ×2, 0x10134, 0x10138). Hinzu kommen Zugriffe über Zeiger auf Zertifikat (0x10180) und Sektionsheader (0x1036C–0x1059C).
   - Die Obermenge aller Decodier-Startpunkte findet nur zwei Fehldecodierungen mitten in Befehlen (0x71735, 0xF84E9). Die Werte 0x10000/0x10001 als Immediate sind Konstanten (`cmp ecx,0x10000` u. a.).
   - Die Bytes 0x10000–0x10107 (Magic und RSA-Signatur) liest also kein Code. `MZ` und `e_lfanew` stören den Gast nicht, und `RtlImageNtHeader(0x10000)` bleibt gültig.
   - Wozu: Ohne gültige NT-Header würden u. a. `CreateThread` mit Stackgröße 0 (`BaseCreateStack` liest SizeOfStackReserve aus dem Image-Header; Vermutung nach ReactOS-Verhalten), Debugger und Treiber-DLLs scheitern, die Threads anlegen.
   - Die Header-Seite bleibt RW, weil `entry` die Zertifikatsgröße bei 0xF84F3 schreiben **kann**. Laut [STA] 9.10 wird der Zweig mit diesem XBE nicht genommen.
6. **Kernel-Thunks auflösen** (1.5), dann **fs-Patches** (1.6), dann **Hooks** (3). Jeder Patch vergleicht vorher die Originalbytes.
7. **Schutzrechte:** In M1 bleibt alles RWX. Die Sektionsgrenzen sind nicht seitenausgerichtet: Gemeinsame Seiten 0x234000, 0x246000, 0x264000, 0x28B000, 0x293000, 0x2BD000, 0x310000, 0x317000, 0x31A000 brauchen die Vereinigung der Rechte ([CPU] 5). D3D/DSOUND/XMV/XPP enthalten beschriebene Daten. `.rdata` muss beim Laden beschreibbar sein (Thunk-Tabelle).
8. **Selbsttests** (`core/selftest`, abschaltbar, in M1 Pflicht):
   - Jeder TLS-Slot: `TlsSetValue` und `__readfsdword(0xE10+4k)` liefern denselben Wert.
   - `RtlImageNtHeader(GetModuleHandle(NULL))` ≠ NULL nach dem Polyglott.
   - `CreateThread(…, 0 Stack, …)` aus der Host-DLL funktioniert.
   - Auf einem adoptierten Thread liefert `XAPILIB::IsBadReadPtr(0,4)` (0x1709C9, stdcall) TRUE. Das prüft Gast-SEH-Frame, `_except_handler3` im Image und den Windows-Dispatcher ([CPU] 6.6).
   - Mapping-Vergleich: Sektionen gegen die Datei, BSS = 0.

### 1.5 Kernel-Thunks und Datenimporte

- Die Tabelle bei 0x293560 hat 111 Einträge `0x80000000|Ordinal` und endet mit 0 bei 0x29371C ([KRN] 1). Der Loader ersetzt jeden Eintrag anhand des Ordinals durch eine Host-Adresse. Ein unbekanntes Ordinal bricht ab; es gibt keines.
- **Generierte Tabelle** `src/host/kernel/gen/kernel_table.gen.cpp` aus `kernel_imports.tsv` (Spalten `ordinal`, `name`, `kind`, `convention`, `args`, `purge_bytes`, `slot`). Je Eintrag: Ordinal, Name, Art (FUNC/DATA/ADDR), Konvention, Purge, Implementierungszeiger, Status (`impl` | `stub_ok` | `unimpl`).
- **Konventionen**: 93 stdcall, 4 fastcall (KfLowerIrql, KfRaiseIrql, IofCompleteRequest, ObfDereferenceObject), 1 cdecl (DbgPrint) ([KRN] 4). Host-Funktionen werden mit genau dieser Konvention deklariert. Ein `static_assert` auf die Argumentbytes des Funktionstyps prüft den Purge, z. B. `NtCreateFile` = 36 Byte, `NtReadFile` = 32 (Xbox ohne `Key`), `KeSetTimer` = 16. Ein falscher Purge zerstört den Gast-Stack, deshalb wird das zur Compile-Zeit geprüft.
- **ABI**: Host-Funktionen müssen ebx/esi/edi/ebp erhalten (49 Sites rufen Importe über callee-saved Register, [KRN] 4). MSVC-Standard-ABI erfüllt das. HLE-Einstiege sind `noexcept` und fangen jede C++-Ausnahme selbst (Abbruch mit Log), damit nie eine Host-Ausnahme durch Gast-Frames läuft.
- **Datenimporte** (11 DATA, 2 ADDR; [KRN] 5). Die Objekte liegen in der Host-DLL (`.data`, beschreibbar):

| Slot | Name | Host-Wert | Beleg/Grund |
|---:|---|---|---|
| 8 | IoInvalidDeviceRequest (ADDR) | Adresse einer Stub-Funktion | XPP-Initialisierer kopiert sie in `_cinit` ([STA] 2.4) |
| 18 | ExEventObjectType | Dummy-Objekt | nur XPP |
| 30 | XboxHDKey | 16 Byte, **≠ 0** | bei 0 liest `FUN_000f6ffa` das EEPROM ([KRN] Verifikation) |
| 31 | XboxSignatureKey | 16 Byte aus Konfiguration (4.5) | Save-HMAC ([FIL] 4.3) |
| 32 | XboxAlternateSignatureKeys | 16 × 16 Byte Dummy | |
| 39 | NtUserIoApcDispatcher (ADDR) | Adresse der HLE-Routine (Marker + aufrufbar) | ApcRoutine von `ReadFileEx`/`WriteFileEx` |
| 47 | HalDiskCachePartitionCount | DWORD 3 (Vermutung; nur `XMountUtilityDrive`, das ersetzt wird) | |
| 53 | LaunchDataPage | beschreibbares DWORD = NULL | `XapiInitProcess` liest/nullt |
| 64 | XboxHardwareInfo | Flags = 0 (Bit 3 **muss** 0 sein, sonst kein `D:` und keine NV-Abfragen; [FIL] 5.5.7), übrige Bytes 0 | |
| 73 | XePublicKeyData | Dummy-Puffer | nur `XcVerifyPKCS1Signature` |
| 75 | XboxKrnlVersion | {1, 0, **5838**, 1} | Build ∉ {3944, 4034, 4039, 4817, 4818}, sonst Kernel-Patch nach 0x800299A0..0x800398CD ([KRN] 8.2) |
| 87 | IdexChannelObject | genullter Puffer ≥ 0x40 Byte | Shutdown-Routine wartet dann nicht |
| 110 | HalBootSMCVideoMode | DWORD 6 (Vermutung, nur `XGetAudioFlags` aus DSOUND) | |

### 1.6 FS-Segment und TLS (Strategie A)

**Entscheidung:** disp32-Retarget wie in [CPU] 2.4 A. Verworfen werden LDT (WoW64, [CPU] 2.4 D), TEB-Überschreiben (E) und „fs:[4] lassen“ (F, kollidiert mit Fibers). Das Funktions-Hooking nach [STA] 7 (Strategie C) ersetzt die Patches erst nach und nach, wenn die betroffenen Funktionen dekompiliert sind. Allein reicht C nicht: Die Inline-Stellen 0x13F443/0x13F5D5 im Spielcode und der Hauptthread-Stub 0xF8469 blieben übrig.

- **Patchliste**: alle Zeilen aus `fs_sites.tsv` mit `offset != 0x0`, genau 38 Stellen ([CPU] 2.3; die Tabelle dort nennt Adresse, Bytes und disp-Position). Der Generator `tools/gen/gen_fs_patches.py` leitet die disp-Position aus dem Encoding ab: `64 A1` → +2 (×19), `64 8B 0D`/`64 8B 3D` → +3 (×11), `64 0F B6 05` → +4 (×8). Er erzeugt `fs_patch_table.gen.inc` mit {Adresse, Originalbytes, disp-Position, Ziel-Slot}.
- **Zielwerte** (zur Laufzeit, k = TlsAlloc-Index): disp `0x04` → S_TLS, `0x20` → S_PRCB, `0x24` → S_IRQL (Bytezugriff liest das Low-Byte), `0x28` → S_KTHREAD, `0x58` → S_DPC. Neues disp = `0xE10 + 4k`.
- **Werte pro Gast-Thread** (gesetzt vom Adopt-Prolog, `kernel/thread.cpp`):

| Slot | Inhalt | Beleg |
|---|---|---|
| S_TLS | `TlsData + 0x24` (TlsDataSize). Dann gilt `[S_TLS + (−9)·4] = [TlsData] = TLS-Block` | `_tls_index` = −9, [CPU] 2.2 |
| S_KTHREAD | Fake-KTHREAD (0x140 Byte, genullt): +0x1C StackBase, +0x20 StackLimit, +0x28 TlsData, +0x12C Thread-ID | [CPU] 2.2 |
| S_PRCB | ein globaler, genullter Fake-KPRCB (≥ 0x254 Byte), +0x250 = 0 (alle Debug-Monitor-Hooks aus) | [CPU] 2.2 |
| S_IRQL | aktueller IRQL (DWORD, normal 0) | `_getptd` ruft bei ≥ 2 `KeBugCheck(0xA)` |
| S_DPC | 0 | nur DSOUND (ersetzt) |

- **Boot-Thread** (führt `entry` aus): `KTHREAD.TlsData = 0`, S_TLS zeigt trotzdem auf einen gültigen Dummy-Block. Grund: Der `CloseHandle`-Fehlerpfad erreicht `SetLastError` ohne TlsData-Prüfung ([CPU] 2.2 Nachtrag).
- **Hauptthread**: Das HLE-`PsCreateSystemThreadEx` legt TlsData (0x24 Byte, getrennt vom Stack) und den Fake-KTHREAD an und setzt die Slots. Danach ruft es `SystemRoutine(StartContext1, StartContext2)` = `0xF99DA(0xF8469, NULL)` wie der Xbox-Kernel. `XapiThreadStartup` schreibt `[TlsData] = TlsData+4` und nullt den Block selbst ([CPU] 2.2).
- **Konsequenz für Werkzeuge:** Die 38 gepatchten Stellen sind in `fs_patch_table.gen.inc` aufgeführt. Byte-Vergleiche zwischen Original und Dekompilat müssen sie ausnehmen ([CPU] 2.4 Risiko d).

### 1.7 Threads, Fibers, APCs, IRQL

- **`PsCreateSystemThreadEx`** (10 Argumente, [KRN] 7.4): `CreateThread` des Hosts mit Stack-Reservierung `max(KernelStackSize, cfg.minGuestStack = 2 MiB)`. Grund: Auf dem 64-KiB-Xbox-Stack laufen künftig Grafiktreiber und Win32 ([CPU] 6.3). Rückgabe: Gast-Handle (Handle-Tabelle) und Thread-ID. `CreateSuspended` wird beachtet.
- **`PsTerminateSystemThread`**: Thread-Zustand freigeben, `ExitThread(status)`. Die Rückkehr von `entry` per `ret` (0xF854E) behandelt der Boot-Thread selbst: Er geht danach in die Message-Loop.
- **Fibers** (E9):
  - `CreateFiber` 0x1708B0, `DeleteFiber` 0x17093C, `ConvertThreadToFiber` 0x17098E und das Trampolin 0x17086B bleiben Original.
  - Das HLE-`MmCreateKernelStack(Size, …)` reserviert `max(Size, cfg.fiberStack = 512 KiB)` vollständig committed. Oben liegt ein 8-Byte-SEH-Terminal-Record (Kopie des letzten Records der Thread-Kette); zurück kommt die Obergrenze darunter. Die Bookkeeping-Map Top → {Base, Limit, Allocation} merkt sich die echte Allokation. `CreateFiber` übernimmt StackBase/Limit aus dem Rückgabewert ([STA] 9.6), größere Stacks sind also transparent.
  - `SwitchToFiber` 0x17094F (63 Byte) wird durch eine MASM-Funktion mit identischer Semantik ersetzt: ebp/esi/edi/ebx und fs:[0] sichern, `cur+0xC = esp`, `esp = new+0xC`, `[TLS+0xC] = new`, wiederherstellen ([STA] 4.2). Zusätzlich setzt sie `TEB.NtTib.StackBase/StackLimit` und `TEB.DeallocationStack` (0xE0C) aus der Bookkeeping-Map. Für die Thread-Fiber aus `ConvertThreadToFiber` (`[f+8] = 0`) werden die beim ersten Wechsel gesicherten echten TEB-Werte genommen ([CPU] 2.5).
  - SEHOP: Ist die gesicherte ExceptionList eines neuen Fibers −1 (`CreateFiber` 0x170931), setzt der Ersatz sie auf den Terminal-Record oben im Fiber-Stack. Das ist nur nötig, wenn auf dem Fiber eine Ausnahme verteilt wird (Vermutung: SEHOP ist aktiv; Test in M2).
  - Warum nicht Windows-Fibers: Das Spiel liest die aktuelle Fiber inline aus `[TLS+0xC]` (0x13F443, 0x13F5D5) und vergleicht sie mit gespeicherten Fiber-Zeigern ([AVI] 2.4.3).
- **APCs** (Datei-I/O, [FIL] 5.1.2, [STA] 4.3):
  - Jeder Gast-Thread hat eine **eigene Gast-APC-Queue**. Es gibt kein `QueueUserAPC`, damit keine APC in fremden alertable Waits von Host-Code (Treiber, XAudio2) landet.
  - `NtReadFile`/`NtWriteFile` mit `ApcRoutine == &NtUserIoApcDispatcher` führen die I/O sofort synchron aus, schreiben IoStatusBlock (= OVERLAPPED) erst bei Zustellung, liefern `STATUS_PENDING` und hängen {Completion = ApcContext, IoStatusBlock} an die Queue.
  - Zugestellt wird nur in `KeDelayExecutionThread(…, Alertable = TRUE, …)` und `NtWaitForSingleObjectEx(…, Alertable = TRUE, …)`. Rückgabe danach ist `STATUS_USER_APC` (0xC0); `SleepEx` macht daraus `WAIT_IO_COMPLETION` ([STA] 4.3).
  - Die Zustellung ruft `Completion(dwErr, Information, lpOverlapped)` (stdcall, `ret 0xC`); `dwErr` = `RtlNtStatusToDosError(status)` bzw. 0.
  - Alertable Waits gibt es nur an drei Stellen: 0x13F4C0, 0x13F590, 0xE0030 ([FIL] 2.4).
- **IRQL/DPC/Timer**:
  - `KeRaiseIrqlToDpcLevel`, `KfRaiseIrql` und `KfLowerIrql` (fastcall, `cl`) pflegen S_IRQL und einen globalen rekursiven „Dispatcher-Lock“.
  - `KeInitializeDpc` und `KeInitializeTimerEx` füllen die Gast-Struktur nicht aus, sondern legen Host-Objekte in einer Map (Gastadresse → Host) an.
  - `KeSetTimer` merkt sich den Timer nur und feuert **nie**: Einziger Nicht-Bibliotheks-Nutzer ist der AutoPowerDown-Timer (6 h), und mit NV-Setting 0x11 = 0 ruft die DPC ohnehin nichts auf ([KRN] 8.6).
  - Eine echte DPC-Ausführung entfällt, weil D3D, DSOUND und XPP ersetzt sind.
- **Critical Sections**: `RtlInitialize/Enter/LeaveCriticalSection` werden über eine Map Gastadresse → Host-`CRITICAL_SECTION` abgebildet, bei unbekannter Adresse lazy angelegt. Damit hängt nichts vom Xbox-Layout ab, das [KRN] 10 offen lässt. Restrisiko: Gastcode, der Felder direkt liest (Abschnitt 7, R5).
- **Waits**: `KeWaitForSingleObject`/`KeSetEvent` braucht nur die XAPI-Shutdown-Routine 0xF9C7F, die nie läuft, weil `HalRegisterShutdownNotification` ein No-op ist. Die minimale Implementierung arbeitet auf einer Map.

### 1.8 Speicher

| Gast-API | Host | Hinweise |
|---|---|---|
| `NtAllocateVirtualMemory` (5 Args, kein ProcessHandle) / `NtFreeVirtualMemory` (3) / `NtQueryVirtualMemory` (2) | `VirtualAlloc`/`VirtualFree`/`VirtualQuery` | In/Out-Parameter zurückschreiben, Protect immer 4 ([KRN] 7.1); Assertion < 0x80000000 |
| `MmAllocateContiguousMemory(Ex)`, `MmFreeContiguousMemory`, `MmQueryAllocationSize` | `VirtualAlloc` (64-KiB-Granularität deckt Alignment ≤ 0x4000) + Größen-Map | `WRITECOMBINE` (0x400) ignorieren |
| `MmAllocate/FreeSystemMemory` | `VirtualAlloc`/`VirtualFree` | `CopyFileExA`-Puffer 64 KiB |
| `MmPersistContiguousMemory` | No-op | |
| `MmQuery/SetAddressProtect` | `VirtualQuery`/`VirtualProtect` | nur Kernel-Patcher (nicht erreicht) und XPP |
| `MmCreate/DeleteKernelStack` | siehe 1.7 | |

- Der **XAPI-Heap** (`RtlCreateHeap`-artig 0xF4EA8, `RtlAllocateHeap` 0xF52CC) bleibt Originalcode. Damit landet der Schreibzugriff des Spielpools auf Poolanfang − 4 ([STA] 5.2) wie auf der Xbox im Eintragskopf des XAPI-Heapblocks. Kein Windows-Heap wird beschädigt. `RtlAllocateHeap` darf deshalb **nicht** auf den Windows-Heap umgeleitet werden.
- Budget: Spielpool 38 MiB (fest, aus dem XAPI-Heap), XAPI-Heap wachsend, Fiber-Stacks je 512 KiB, XBE 3 MiB ([STA] 5.3). Für einen 32-Bit-LAA-Prozess ist das unkritisch.

### 1.9 Zeitbasis

`XAPILIB::QueryPerformanceCounter` (0xF641B, `rdtsc`) und `QueryPerformanceFrequency` (0xF642C, Konstante 733 333 333) werden auf Win32-QPC/QPF umgeleitet (stdcall, 1 Argument, Rückgabe TRUE). Das Spiel berechnet `1/QPF` einmalig, ein konsistentes Paar genügt ([CPU] 3, [STA] 3.3). `KeQuerySystemTime` → `GetSystemTimeAsFileTime`. Später kann die QPC-Umleitung im Replay-Modus virtuelle Zeit liefern (M9).

**Fester Zeitschritt (seit M3b, 8.4 B38–B42):** Das Spiel simuliert in festen Schritten dt ohne Interpolation ([STA] 3.2). Die EU-Disc liefert immer PAL (CApp+0xE8 = 0), und der PAL-Zweig von `Game::Init` 0xA26A0 setzt fps 50 und dt = 0,02 s. Der ursprüngliche Plan „dt bleibt 0,02 s, weil die PAL-Archive 50 Hz festlegen“ ist überholt: Die Archive legen nur die Inhalte fest, nicht den Takt. **Standard ist 60 Hz:** `[game] tick_rate = 60` setzt im PAL-Zweig fünf Operanden und zwei `.data`-Anfangswerte auf die Zeitwerte des NTSC-Zweigs (fps 60, dt = 1/60 s, Tastenwiederholung 15 Ticks). CApp+0xE8, das Filmsuffix „PAL“, die PAL-Archive, die Sprachwahl sowie PAL-Splash und -Legal bleiben unverändert. `[game] tick_rate = 50` patcht nichts (Original-PAL, nur für xemu-Vergleiche und Regressionen). Tick-gebundene Stellen (UI, Kamera, Starrkörper-Dämpfung, Chicken Walker, Skriptschleifen mit kurzen Waits; timing60.md 4.10, 12) verhalten sich bei 60 Hz wie in der NTSC-Fassung, die denselben Code und dieselben Skripte mit 60 Hz betrieb (bewusst akzeptiert, B40). Die drei Zeitproben von `Game::Frame` laufen über eine Lockstep-Uhr (`[game] frame_clock = lockstep`, B42), damit jeder präsentierte Frame genau einen Schritt zeigt; alle anderen Aufrufer sehen weiter echte QPC-Zeit. Den Bildtakt koppelt der Host an die Anzeige (`[render] pacing`, B43, B47). Mehr als 60 verschiedene Bilder pro Sekunde erfordern Interpolation (M3d).

### 1.10 Start- und Endablauf

```
LauncherEntry: TlsAlloc×5 → LoadLibrary(host) → HostMain
HostMain:
  1 Config, Log, Crash-Handler (VEH-Ringpuffer, SetUnhandledExceptionFilter, Minidump)
  2 XBE lesen/prüfen → mappen → Polyglott → Thunks → fs-Patches → Hooks → Selbsttests
  3 VFS aufbauen (Geräte \Device\CdRom0, \Device\Harddisk0\partition1; Symlink-Tabelle leer)
  4 Fenster anlegen (unsichtbar bis zum ersten Present)
  5 Boot-Thread adoptieren (TlsData=0, Dummy-TLS-Anker) → call entry 0xF84DD
       entry: TlsDataSize=0x24, _tls_index=-9, CreateThread → HLE PsCreateSystemThreadEx → ret
  6 Message-Loop bis Prozessende
Gast-Hauptthread: XapiThreadStartup 0xF99DA → 0xF8469:
  FUN_000f9804 (kehrt sofort zurück, Build 5838) → XapiInitProcess → _rtinit → _cinit → main 0xD29B0
Ende: main kehrt zurück → XapiBootToDash → XLaunchNewImageA (Hook) → Prozess beenden.
      Jeder HalReturnToFirmware-Aufruf beendet den Prozess (XapiBootToDash ist NICHT noreturn, [STA] 3.5).
```

### 1.11 Ausnahmen und Absturzdiagnose

- fs:[0] ist die Windows-SEH-Kette. `RtlUnwind` (stdcall 4) und `RtlRaiseException` (stdcall 1) werden direkt an die ntdll-Exporte weitergereicht ([KRN] 7.5, [CPU] 2.5).
- Der VEH protokolliert First-Chance-Ausnahmen mit EIP im XBE-Bereich in einen Ringpuffer pro Thread, ohne einzugreifen; `IsBadReadPtr` nutzt solche Ausnahmen absichtlich.
- `KeBugCheck` gibt den Code, den letzten Ringpuffer-Eintrag, einen EBP-Backtrace mit Symbolen aus der generierten Funktionstabelle (`0x13F5D5 = FUN_0013f590+0x45`) und einen Minidump aus und beendet dann den Prozess. Unbehandelte C++-Ausnahmen enden über `terminate` → `KeBugCheck(0xC0000144)` ([STA] 8).
- `int 0x2d` in `OutputDebugStringA` wird durch den Hook gegenstandslos. Die drei D3D-`int3`-Fehlerpfade sind unerreichbar, weil D3D nicht läuft.

---

## 2. HLE-Schnitt

### 2.1 Übersicht je Teilsystem

| Teilsystem | bleibt Original | nativ (HLE) | Stub/Falle | Begründung |
|---|---|---|---|---|
| xboxkrnl (111 Slots) | – | 74 Importe mit echter Semantik (51 trivial, 21 mittel, 2 schwer: `NtCreateFile`, `PsCreateSystemThreadEx`) | 37 nur von D3D/DSOUND/XPP genutzte Importe → protokollierende Stubs mit plausiblen Rückgaben ([KRN] 6) | [KRN] 7; Plan je Meilenstein in Anhang A |
| XAPILIB (0xF3DE0–0xFA5A0, 0x1707A0–0x170ABA) | Heap, Datei-API, `CopyFile`, `FindFirstFile`, SaveGame-API, `XCalculateSignature*`, `XapiInitProcess`, `CreateThread`/`XapiThreadStartup`, `Get/SetLastError` (über fs-Patches), `SleepEx`, Fiber-Create/Delete/Convert | `QueryPerformanceCounter` 0xF641B, `QueryPerformanceFrequency` 0xF642C, `OutputDebugStringA` 0xF3DE0 → Host-Log, `XMountUtilityDrive` 0xF7F7C → `Z:` auf Host-Cache, TRUE, `XLaunchNewImageA` 0xF82E3 → Meldung + Prozessende, `SwitchToFiber` 0x17094F (1.7) | – | Auf Kernel-Ebene laufen Laufwerke, Saves und Metadateien originalgetreu ([FIL] 5.1). `XMountUtilityDrive` erspart partition0, Cache-Tabelle und FATX-Formatierung ([FIL] 5.1.5) |
| LIBCMT | vollständig (inkl. `_getptd`/`_mtinit` über fs-Patches) | – | – | |
| Spielcode und NetImmerse-Kern | vollständig, bis zur Dekompilierung (3.4) | – | – | rufen den Kernel fast nie direkt ([KRN] 1) |
| **NiXBoxRenderer (REN, 0x146020–0x14CD50 ∪ 0x1552C0–0x166871)** | Code bleibt im Image (Referenz für die Dekompilierung), läuft aber nie | **`NiPCRenderer`** über Detour von `Create` 0x14C170 (2.2) | – | E6 |
| **D3D8** (Section D3D) | statischer Initialisierer 0x235770 (harmlos) | Shim für die 8 vom Spiel direkt gerufenen Funktionen (2.2) | alle übrigen extern gerufenen D3D-Einstiege (`d3d_functions.tsv`, `ext_call_sites > 0`) → Falle: Log + Abbruch | Nach dem Create-Detour ruft sie niemand mehr; die Falle meldet Irrtümer sofort |
| Movie-Player des Spiels (0xEDE40–0xEE220) | **ja, unverändert** | – | – | läuft gegen D3D-Shim + XMV-HLE + layout-kompatible RendererData (2.2) |
| DSOUND (40 Einstiege + 5 Stream-VTable-Methoden) | 4 statische Initialisierer in `_cinit` (harmlos, [AVI] 2.1) | M5: XAudio2 + X3DAudio + I3DL2-Reverb ([AVI] 2.5) | M2–M4: Stub ([AVI] 6 Schritt 1, Variante `*pp = NULL`) | Das Spiel nutzt nur die C-API und virtuelle Stream-Methoden; DOLBY bleibt ungenutzt |
| XMV (7 Einstiege) | – | M6: FFmpeg (avformat `xmv`, avcodec `wmv2`/`adpcm_ima_xbox`), YUY2 in die Shim-Surface ([AVI] 3.5) | M2–M5: Decoder anlegen, `GetNextFrame` → Ergebnis 2 sofort ([AVI] 6 Schritt 2) | |
| XPP/XInput (7 Einstiege, Thunk 0x28CFE1 für `XInitDevices`) | XPP-Initialisierer 0x28D254 (liest Slot 8) | M4: XInput 1.4 + Tastatur ([AVI] 4.3) | M2–M3: keine Geräte, `XGetDevices` = 0 | USB/MU-Treiberstapel wird so nie erreicht |
| Dateisystem | XAPI-Datei-API | Nt\*/Io\*-VFS (4) | `NtDeviceIoControlFile` auf CdRom0 → Fehler (Ergebnis wird ignoriert), `NtFsControlFile` → Erfolg | [FIL] 5.1 |
| Spielstände | XAPI-SaveGame- und Signatur-API, Spiel-Save-Schicht | SHA-1 (`XcSHA*`), Schlüssel (4.5) | `XcVerifyPKCS1Signature` → TRUE | [FIL] 4 |
| Einstellungen | – | `ExQueryNonVolatileSetting` aus der Konfiguration: 7 = Sprache (1 en, 3 de, 4 fr, 5 es, 6 it), 0xA = 0 (Pflicht), 0x11 = 0, 9 = 0, 0x104 = Region (nur bei Sprache > 9), sonst `STATUS_OBJECT_NAME_NOT_FOUND` + Log | – | [STA] 9.2, [FIL] 4.5 |

### 2.2 Grafikgrenze: Entscheidung für die NiRenderer-Ebene

Die Berichte empfehlen Verschiedenes:
- [D3D] 10.3 schlägt einen Hybrid vor: zuerst D3D-API-HLE mit 88 Einstiegen und einem Lazy-State-Leser, dann NiXBox schrittweise dekompilieren.
- [REN] 7 empfiehlt die NiRenderer-Ebene.

**Entschieden wird: NiRenderer-Ebene, ohne Xbox-D3D8-Emulation.** Begründung aus den Belegen:

1. **Nur REN berührt D3D-Interna.** 22 REN-Funktionen schreiben bzw. lesen mit 175 Befehlen 43 D3D-BSS-Globals; Spiel, NI-Kern und CRT tun das nie, auch im Lückencode nicht ([REN] 6, [D3D] 5). Ersetzt man REN, verschwinden damit alle schwierigen D3D-Themen:
   - Lazy-State-Arrays und Dirty-Flags
   - fehlendes `Unlock` mit Änderungserkennung beim Gebrauch ([D3D] 10.1)
   - physische Adressen in `Data` und das 0x80000000-Fenster ([D3D] 7)
   - Swizzling und Register-Combiner
   - die offene Zuordnung der Renderstate-Indizes 0x39–0x48 und 0x5C–0x73 ([D3D] 11)
2. **Die NiRenderer-API ist datenorientiert** (`RenderShape(NiTriShapeData*, NiSkinInstance*, NiTransform*, NiBound*)`, PropertyState an +0xC) und damit genau die Ebene, die ein Vulkan-/D3D12-Renderer mit Widescreen und hoher Auflösung braucht. Es entsteht keine Wegwerf-Schicht; README-Strategie Punkt 4 sieht diese Grenze ohnehin vor.
3. **Wenige Slots werden wirklich gebraucht.** Rund 30 der 75 Slots werden aufgerufen. P0 (erstes Bild) umfasst etwa 13 Slots, P1 (Geometrie, Texturen) etwa 6 weitere plus die Property→State-Abbildung ([REN] 7.2).
4. **Die zwei Umgehungen des Interfaces sind beherrschbar, ohne Spielcode zu ändern:**
   - Der Movie-Player liest `renderer+0x60` (Device) und ruft 8 D3D-Funktionen direkt auf ([REN] 6). Abhilfe: Shim (unten) und ein Dummy-Device-Zeiger ≠ 0 an +0x60.
   - Der Movie-Player schreibt in die Textur-RendererData (`NiTexture+0x20`) die Felder +0x3C (D3DTexture*), +0x48/+0x4C (W/H), +0x54, +0x58, +0x60 ([REN] 5.2). Abhilfe: Die RendererData von `NiPCRenderer` für Quelltexturen ist **0x70 Byte groß und layoutkompatibel** (diese Offsets sind reserviert). Der Renderer erkennt eine Movie-Textur am Zeiger bei +0x3C, der in der Shim-Registry steht.
5. Gegenargumente aus [D3D] und wie sie aufgefangen werden:
   - „Kein Referenz-Renderer“: Der REN-Code im Image dient als ausführbare Spezifikation. Er wird gezielt gelesen bzw. dekompiliert (Property→State `FUN_00159cd0`, Lichter `FUN_0015ba40`/`FUN_0015b6a0`, Texture-Pipeline `FUN_0015ab40`/`FUN_00162020`). Seiteneffektfreie Helfer wie die CPU-Skinning-Deformation `FUN_0014f710` lassen sich eventuell direkt aufrufen (Vermutung, prüfen).
   - Optische Regressionen: Vergleich mit xemu-Screenshots an festen Prüfpunkten (M3, M8).
   - „Das Movie-Problem bleibt“: gelöst durch Shim und Layoutkompatibilität (Punkt 4).
   - **Rückfallposition**: Die D3D-Fallen bleiben bestehen. Ein späteres D3D-HLE könnte ohne Konflikt ergänzt werden, weil REN unverändert im Image liegt.

**Aufbau `NiPCRenderer`** (gastsichtbares Objekt, 0x6D0 Byte, aus dem Host-Heap):

| Offset | Feld | Pflicht |
|---|---|---|
| +0x00 | eigene VTable mit 75 Slots | Slot-Belegung und `ret N` wie [REN] 2 / `renderer_retn.py`; Slots 2–10 (NiObject) zeigen auf die Original-Einträge aus 0x2AA7C0 |
| +0x04 | Refcount | NiRefObject |
| +0x08 | `NiAccumulator*` | Spiel schreibt direkt ([REN] 1.2) |
| +0x0C / +0x10 | `NiPropertyState*` / `NiDynamicEffectState*` | Aufrufer schreiben direkt vor jedem Render-Call |
| +0x60 | Dummy-Device-Zeiger ≠ 0 | Movie-Player 0xEDE40 |
| +0x6C0 | `HostRenderer*` (C++-Implementierung) | intern |

- Konstruktion: Original-Basis-ctor `NiRenderer` `FUN_00140550` aufrufen (thiscall; Vermutung nach ctor-Konvention, vor M2 am Aufruf in `FUN_0014af00` prüfen), damit der Renderer in der globalen Liste 0x2D7B1C für die Purge-Broadcasts steht. Danach VTable setzen.
- Zerstörung: Slot 0 ruft `FUN_001406e0` auf.
- Slot 1 liefert `&NiXBoxRenderer::ms_RTTI` (0x30EE00) ([REN] 7.1).
- Pflicht-Rückgaben: Slots 65 und 66 liefern TRUE, sonst werden Texturen NULL. Slot 39 liefert FALSE und lässt die CPU-Daten unangetastet ([REN] 5.3/7.1).

**D3D-Shim** (`gfx/d3d_shim.cpp`, stdcall, `ret` laut `d3d_functions.tsv`):

| Adresse | Funktion | ret | Shim |
|---|---|---|---|
| 0x234440 / 0x234460 | `D3DDevice_AddRef` / `D3DDevice_Release` | 0 / 0 | Zähler, kein Abbau |
| 0x2370D0 | `D3DDevice_CreateTexture2` | 0x1C | Fake-Ressource im Gastspeicher mit Xbox-ähnlichem Header {Common, Data, Lock, Format, Size} (Feldlage Vermutung nach XDK) und Pixelpuffer W·H·2 für Format 0x24 (YUY2); in die Registry eintragen |
| 0x2371A0 | `D3DTexture_GetSurfaceLevel2` | 8 | Fake-Surface aus der Registry |
| 0x2373B0 / 0x2373F0 | `D3DResource_AddRef` / `Release` | 4 / 4 | Refcount; unbekannte Zeiger und NULL ignorieren (einmal loggen) |
| 0x236DC0 | `D3DDevice_SetRenderState_YuvEnable` | 4 | Flag an den Renderer |
| 0x2354F0 | `D3DDevice_SetScreenSpaceOffset` | 8 | Offset (±12 px) an den Renderer; `vf15`-Logik nachbilden ([REN] 7.1.3) |

**RHI und Backends**: Eine dünne RHI (Puffer, Texturen, State-Objekte per Hash, Draw) liegt unter `HostRenderer`. Für Bring-up und Debug-Layer kommt zuerst D3D11 (Windows SDK, keine Zusatzabhängigkeit), dann Vulkan bzw. D3D12 als zweites Backend (M10). Die Fixed-Function-Pipeline (TSS-artige Texturstufen, D3D-Lichter, Fog, Alpha-Test) wird als Ubershader nachgebildet. Es gibt kein Swizzling: `NiPixelData` liegt linear vor; DXT wird als BCn hochgeladen, P8 expandiert ([REN] 5.2).

---

## 3. Hooking-Mechanik

### 3.1 Patch-Arten

| Art | Mechanik | Wo |
|---|---|---|
| Thunk-Slot | DWORD in `.rdata` 0x293560+4·Slot | Kernel (1.5) |
| fs-Retarget | disp32 im Befehl | 38 Stellen (1.6) |
| Funktions-Detour | `E9 rel32` auf die ersten 5 Byte des Funktionsanfangs, Rest unverändert | XAPI-Ersatz, D3D-Shim/-Fallen, DSOUND/XMV/XPP-HLE, `Create`, dekompilierte Funktionen |
| VTable-Slot | Zeiger in `.rdata`-VTable | nur, wenn ein Einstieg kein eigener Funktionsanfang ist (Vermutung: selten) |
| Operanden-/Datenpatch (M3b) | Operandenbytes eines Befehls bzw. `.data`-Anfangswert aus einer Tabelle, ganzer Originalbefehl gegen Abbild und XBE-Datei geprüft | 60-Hz-Zeitbasis in `Game::Init` (8.4 B38/B39) |

Im 32-Bit-Adressraum erreicht `rel32` jede Adresse. Trampoline („Original aufrufen“) gibt es anfangs nicht. Wird eine Funktion ersetzt, ruft der Ersatz entweder eigene Logik oder die noch nicht ersetzten **Callees** auf, nicht die ersetzte Funktion selbst. Wrapper-Hooks mit Trampolin (Logging, Profiling) kommen später über einen kleinen Längen-Disassembler (z. B. hde32, Vermutung zur Bibliothekswahl).

### 3.2 Hook-Registrierung und Validierung

```cpp
// in jeder Übersetzungseinheit, re3-Stil
FUT_HOOK(0x0013F250, NiUDSStream::Ctor, Conv::Thiscall, "NiUDSStream::NiUDSStream");
FUT_HOOK(0x000A4900, Mem_New,           Conv::Cdecl,    "operator new (memory.cpp)");
```

- `FUT_HOOK` legt einen statischen Registry-Eintrag an: Adresse, Ziel, Konvention, Name, Quelldatei, Default an/aus.
- Der Installer prüft jeden Eintrag gegen die generierte Funktionstabelle `xbe_functions.gen.inc` (aus `functions.tsv` plus `extra_function_starts.tsv` für Code außerhalb von Ghidra-Funktionen, z. B. 0xF8469, 0xF99DA, 0x13F1D0, 0xCC5E0; [STA] 10, [AVI] 5.3):
  1. Die Adresse ist ein bekannter Funktionsanfang.
  2. Die Funktion ist ≥ 5 Byte lang. Sonst Abbruch, weil der Patch die Nachbarfunktion träfe, z. B. `ret`-Stub 0xB0390.
  3. Keine Doppelbelegung.
  4. Keine Überschneidung mit fs-Patches oder anderen Hooks.
- Abschalten: `futurama.ini [hooks] disable=0x13F250,NiUDSStream::*`. Ein abgeschalteter Hook lässt die Originalbytes stehen. Das ermöglicht A/B-Vergleiche zwischen Dekompilat und Original im selben Build.
- Eine Falle ist ein Hook auf eine generische Funktion, die Name, Rücksprungadresse und Argumente loggt und abbricht. Für jede Falle wird ein Stub mit passendem `ret N` erzeugt, damit sie im Modus „nur loggen“ den Stack korrekt bereinigt.

### 3.3 Aufrufkonventionen und Adapter

| Original | Ersatz in C++ | Bemerkung |
|---|---|---|
| stdcall (Kernel, XDK-APIs, viele XAPI) | `extern "C" T __stdcall f(...)` | Purge per `static_assert` prüfen |
| cdecl (z. B. `FUN_000a4900`, `NiXBoxRenderer::Create` mit 8 Args) | `T __cdecl f(...)` | |
| thiscall (Methoden) | Methode einer layoutgleichen Klasse; Adresse per Member-Pointer-Cast (re3-Muster). Für freie Funktionen: `T __fastcall f(C* self, void* /*edx*/, ...)` | |
| fastcall (`KfLowerIrql`, `SetRenderState_Simple`) | `__fastcall` | |
| Sonderformen (z. B. `__SEH_prolog` mit eax; Vermutung: LTCG-Registerkonventionen in Spielcode) | MASM-Adapter je Funktion | vor dem Dekompilieren die Aufrufstellen prüfen (R13) |
| Stackwechsel (`SwitchToFiber`), Gast-Aufrufe aus dem Host (`entry`, `SystemRoutine`) | MASM (`src/host/asm/*.asm`) | keine Inline-Assembly; das funktioniert mit MSVC und clang-cl |

Aufrufe von Host nach Gast nutzen typisierte Funktionszeiger auf feste Adressen, z. B. `inline auto const XapiThreadStartup = reinterpret_cast<void(__stdcall*)(void*, void*)>(0xF99DA);`. Für thiscall nimmt man `reinterpret_cast<R(__thiscall*)(C*, A...)>(addr)`.

### 3.4 Schrittweise Dekompilierung (re3-Stil)

- **Globals** werden über generierte Referenzen auf feste Adressen angesprochen: `FUT_GLOBAL(CApp*, g_pApp, 0x2EB310);`, `FUT_GLOBAL(float, g_dt, 0x2D6748);`. Ein Global wird erst „umgezogen“, wenn kein Originalcode mehr darauf zugreift. Prüfen lässt sich das mit `startup_dataref.py`-artigen Scans.
- **Klassen** behalten das Originallayout. Jede Klasse hat `static_assert(sizeof(X) == …)` und `offsetof`-Asserts auf alle Felder, die Originalcode liest, z. B. `CApp` 0x108 mit +0xE8 Videonorm und +0x104 Renderer ([STA] 2.5). VTables bleiben, solange Originalcode virtuell aufruft. Ersetzte virtuelle Methoden werden am Funktionsanfang gehookt.
- **Granularität**: immer ganze Funktionen, nie halbe. Eine Funktion gilt als fertig, wenn sie gehookt ist, standardmäßig aktiv ist, der A/B-Vergleich unauffällig ist und der Regressionstest (M9) besteht.
- **Reihenfolge** (Vorschlag, wegen Hebelwirkung auf die Emulation):
  1. Speichermanager `memory.cpp` (0xA3190–0xA4920)
  2. `NiUDSStream` (0x13F250–0x13FFB0): danach sind Fibers und APCs für Assets überflüssig ([FIL] 5.6)
  3. Save-Schicht (0xE0040–0xE1500)
  4. Sound-Klassen (0xC9xxx–0xCDxxx): danach entfällt die DSOUND-Emulation ([AVI] 2.5)
  5. Controller (0x9B3C0–0x9C985)
  6. danach Spiellogik nach Subsystemen
- **Rückfluss nach Ghidra**: Namen und Signaturen aus der Registry erzeugen `analysis\symbols\retail_eu_decomp_symbols.tsv` für `ApplySymbols.java`. Das passt zur Regel im README, dass dauerhafte Namen in eine Symbol-TSV gehören.

### 3.5 Mapping-Datei im Repo

`docs/function_map.tsv` wird aus Code und Analyse erzeugt (`tools/gen/update_function_map.py`) und eingecheckt. Ein CI-Check (`--check`) schlägt fehl, wenn sie veraltet ist.

| Spalte | Inhalt |
|---|---|
| `address`, `size`, `section` | aus `functions.tsv` bzw. `extra_function_starts.tsv` |
| `ghidra_name` | z. B. `FUN_0013f250`, `XAPILIB::SwitchToFiber` |
| `cpp_symbol`, `source_file` | aus der `FUT_HOOK`-Registry |
| `convention`, `ret` | aus Registry bzw. `ret N`-Scan |
| `status` | `orig` · `hle` · `shim` · `trap` · `decomp` · `decomp_default_on` · `fs_patched` |
| `notes` | Beleg, Vermutungen |

Zusätzlich erzeugt `tools/gen/progress.py` daraus Kennzahlen: Anteil `.text`-Bytes in `decomp_default_on`, getrennt nach Spiel, NI-Kern und REN.

---

## 4. Virtuelles Dateisystem und Spielstände

### 4.1 Objekt-Namensraum und Geräte

| Gastname | Host-Gerät | Inhalt |
|---|---|---|
| `\Device\CdRom0` | `IsoDevice` (XDVDFS, nur lesen) oder optional `DirDevice` (extrahierte Disc, nur lesen) | `D:` |
| `\Device\Harddisk0\partition1` | `HostDirDevice` = `<userdata>\hdd\partition1\` | `T:`/`U:` (XAPI legt `TDATA\46490002`, `UDATA\46490002` selbst an) |
| `\??\Z:` (vom `XMountUtilityDrive`-Ersatz) | `HostDirDevice` = `<userdata>\cache\Z\` | vom Spiel ungenutzt ([FIL] 1.1) |
| `\??\X:` | Symlink-Tabelle, gefüllt von `IoCreateSymbolicLink` (doppelter Name → `0xC0000035`) | `D:` → `\Device\CdRom0` usw. |

`<userdata>` ist standardmäßig `%LOCALAPPDATA%\FuturamaPC`; in der Konfiguration ist ein Portable-Modus `.\userdata` wählbar. Beim ersten Start werden `partition1\TDATA` und `partition1\UDATA` angelegt (Vermutung: auf einer Xbox-HDD existieren sie schon, und XAPI legt nur die Titel-Unterverzeichnisse an).

Seit M3c (Abschnitt 8.5, K3) liegt auch die Konfiguration dort: ohne `--config` liest `futurama.exe` die Datei `%LOCALAPPDATA%\FuturamaPC\futurama.ini`. Der Portable-Modus wird durch eine Datei `portable.txt` neben `futurama.exe` eingeschaltet; dann liegen `futurama.ini` und die Benutzerdaten im Ordner der exe (wie im Standardmodus nebeneinander; das ersetzt `.\userdata` von oben). `--config=<Datei>` hat immer Vorrang (der Launcher übergibt es immer), `[paths] userdata` ersetzt in jedem Modus den Standardordner.

### 4.2 Pfadauflösung und Geräte-Implementierung

1. `OBJECT_ATTRIBUTES` = {RootDirectory, `PANSI_STRING` ObjectName, Attributes 0x40}:
   - `RootDirectory = (HANDLE)−3` → Präfix `\??\`.
   - `0` → absoluter Name.
   - Ein Verzeichnis-Handle → relativ dazu (`FUN_000F8709`).
   ([FIL] 5.1.1)
2. Normalisieren: `/` → `\`, Vergleiche case-insensitiv (`sysPAL.img` vs. `SysPAL.img`, `Video\` vs. `video/`).
3. Symlinks auflösen (`\??\U:\x` → `\Device\Harddisk0\partition1\UDATA\46490002\x`), dann das längste Geräte-Präfix suchen. Der Rest ist der gerätelokale Pfad, das Geräte-Root auch mit abschließendem `\` (`FUN_000f8ae2` öffnet `…\partition1\`).
4. **`IsoDevice`**:
   - Volume-Deskriptor `MICROSOFT*XBOX*MEDIA` in Sektor 32 relativ zur Basis (0 bei XISO, 0x18300000 bei Redump).
   - Verzeichnisbaum beim Start einlesen (59 Einträge); jede Datei ist ein Extent.
   - `read(off,len)` = `ReadFile` mit OVERLAPPED-Offset auf das ISO-Handle. Damit sind mehrere positionelle Reads auf dasselbe Gast-Handle möglich, ohne gemeinsamen Dateizeiger ([FIL] 2.4).
   - **Reads über das Dateiende hinaus liefern `STATUS_SUCCESS` mit kleinerem `Information`** (bis zu 2047 Byte Überhang, [FIL] 2.4).
   - Reads dürfen nie fehlschlagen, sonst kommt der Disc-Fehlerbildschirm in Endlosschleife.
   - Nicht vorhandene Namen (`FutPAL05.img`, `sysNTSC.img`, `fonts/kanjii.fnt`, `Video/*.txt`) liefern sauber `STATUS_OBJECT_NAME_NOT_FOUND`.
5. **`HostDirDevice`**:
   - Abbildung auf Win32 (`CreateFileW`, UTF-16-Konvertierung).
   - `FILE_FLAG_NO_BUFFERING` und `OVERLAPPED` vom Gast werden entfernt; I/O läuft synchron, die APC-Semantik erzeugt der Kernel-Layer (1.7).
   - Synchrone Handles führen einen eigenen Dateizeiger, wenn `ByteOffset == NULL`.
   - FATX-Regeln werden geprüft (Name ≤ 42 Zeichen).
6. **Handle-Tabelle**: Gast-Handles sind kleine Werte (z. B. 0x1000 + 4n), also nie −1/−2/−3. Sie zeigen auf Host-Objekte (Datei, Verzeichnis-Enumeration, Thread, Symlink, Gerät). `NtClose` gibt frei.

### 4.3 Kernel-Datei-Funktionen (Semantik)

Pflichtumfang laut [FIL] 2.3/5.1 und [KRN] 7.3:
- `NtCreateFile` (9 Args) und `NtOpenFile` (6), mit Dispositionen nach [FIL] 2.5.
- `NtReadFile`/`NtWriteFile` (8 Args, ohne `Key`) synchron oder mit APC.
- `NtQueryInformationFile`, Klassen 0x22 (NetworkOpen, 0x38) und 0xE.
- `NtSetInformationFile`, Klassen 4, 0xD, 0xE, 0x13, 0x14.
- `NtQueryVolumeInformationFile`, Klasse 3 (FsSize, 0x18): für `partition1` und `U:` SectorsPerAllocationUnit 32 × BytesPerSector 512 = **0x4000**. Freie Einheiten werden so gekappt, dass der freie Platz < 4 GiB bleibt, z. B. ≤ 0xFFFF0000 Byte (Spielfehler `high | low`, [FIL] 4.4). Für `D:` gilt Sektor 2048.
- `NtQueryDirectoryFile` (10 Args, Klasse 1, `FileMask` als ANSI, ein Eintrag pro Aufruf, ohne `.`/`..`).
- `NtQueryFullAttributesFile`, `NtFlushBuffersFile`, `NtClose`.
- `IoCreateSymbolicLink`, `NtOpen/QuerySymbolicLinkObject`.

### 4.4 Archive und Mods

Die `.img`-Archive liest das Spiel selbst (`FUN_0013FE70`/`FUN_0013FC40`). Der VFS liefert nur die Disc-Dateien unverändert ([FIL] 5.4: byte-identisch). Da der Archiv-Lookup vor losen Dateien greift ([FIL] 3.3), kommt ein Override-Mechanismus für Mods später als Hook auf `FUN_0013FE10` (liefert 0 für überschriebene Pfade, [FIL] 5.2). Er ist nicht Teil der ersten Meilensteine.

### 4.5 Spielstände

- **Ort**: `<userdata>\hdd\partition1\UDATA\46490002\<12-Hex-Hash>\` mit `SaveMeta.xbx`, `Futurama.dat` | `Options.dat` | `Unlock.dat` (je genau 4096 Byte) und `saveimage.xbx`. Daneben `TitleMeta.xbx` und `TitleImage.xbx` im Titelverzeichnis ([FIL] 4.1). Die Struktur gleicht der Xbox-HDD, Import und Export aus xemu-/FATX-Dumps gehen 1:1.
- **Signatur**: HMAC-SHA1 mit `XboxSignatureKey` über 0x000–0xFEB, Ergebnis bei 0xFEC ([FIL] 4.2/4.3).
  - SHA-1 ist im HLE implementiert. **Der Zustand liegt im Gast-Kontextpuffer**: Dieser ist 0x74 Byte groß, ein SHA-1-Zustand braucht 0x5C. Beleg: `FUN_000f902f` legt den Kontext als `local_8c[116]` an, und `XCalculateSignatureBeginEx` 0xF706F alloziert `LocalAlloc(0,0x7C)` mit dem Kontext ab +8, also 0x74 Byte (Decompilat). Damit ist die offene Frage zur Kontextgröße aus [KRN] 10 beantwortet.
  - Schlüsselmodus (`futurama.ini [saves] key_mode`): `fixed` (Default) ist ein fester 16-Byte-Projektschlüssel. PC-Saves sind dann untereinander gültig, Xbox-Saves gelten als beschädigt. `xbox` leitet den Schlüssel aus einem vom Nutzer eingetragenen `XboxCERTKey` über HMAC-SHA1(CERTKey, cert.SignatureKey `98c52648…4ae4`)[0..15] ab (Vermutung nach Cxbx/xemu, [FIL] 4.3; mit einem echten Save prüfen).
  - Der Schlüssel muss über alle Sitzungen konstant bleiben.
- **Pflichtwerte** (Checkliste aus [FIL] 5.5, [STA] 9):
  - `XGetDiskClusterSizeA("U:\")` ≠ 0 (0x4000), sonst Division durch null in `_cinit` 0x20EFDD.
  - Freier Platz < 4 GiB.
  - `LocalAlloc` darf nicht scheitern (NULL-Zugriff in `FUN_000E0340`).
  - `WriteFileEx`-APC wird über den alertable Poll 0xE0030 zugestellt.
  - `XLaunchNewImageA` mit Reason 2/`'U'` (zu wenig Platz) → Meldung statt Dashboard (umgesetzt in M7: Meldung und Rückkehr ins Spiel, 8.18 S3).

---

## 5. Projektstruktur, Build, Konventionen, Logging

### 5.1 Verzeichnisse (Vorschlag; Code liegt außerhalb von `analysis\`)

```
D:\FuturamaDecomp\
  CMakeLists.txt  CMakePresets.json  .clang-format
  src\
    launcher\        placeholder.asm, launcher.c                → fut_launcher (futurama.exe)
    host\                                                         → fut_host (futurama_host.dll)
      core\          config.*, log.*, crash.*, xbe_loader.*, xbe_header_polyglot.*, fs_patcher.*, hooks.*, selftest.*
      kernel\        ob_handles.*, io_*.cpp, ke_thread.*, ke_apc.*, ke_irql.*, ke_timer.*, mm.*, rtl.*, xc_sha1.*, hal.*, ex_settings.*, data_exports.*
      kernel\gen\    kernel_table.gen.cpp (aus kernel_imports.tsv)
      vfs\           namespace.*, symlinks.*, iso_xdvdfs.*, dir_device.*, file_objects.*
      xapi\          xapi_hooks.cpp (QPC/QPF, OutputDebugString, XMountUtilityDrive, XLaunchNewImageA)
      gfx\           ni_pc_renderer.*, renderer_slots.cpp, d3d_shim.*, rhi\{rhi.h, d3d11\, vulkan\}
      audio\         dsound_hle.*, xbox_adpcm.*, xaudio2_backend.*
      video\         xmv_hle.*, ffmpeg_avio.*
      input\         xinput_hle.*, keyboard_map.*
      platform\      window.*, msgloop.*
      asm\           fiber_switch.asm, guest_call.asm, adopt.asm, trap_stubs.asm
      gen\           xbe_functions.gen.inc, fs_patch_table.gen.inc, d3d_traps.gen.inc, xdk_entries.gen.inc
    game\            dekompilierter Code, gegliedert nach Original-Subsystem (z. B. Memory\memory.cpp)  → fut_game (static)
    shared\          Gast-Strukturen (Xbox-Kernel, XAPI, Ni*-Layouts) mit static_asserts                → fut_shared (INTERFACE)
  tools\gen\         gen_kernel_table.py, gen_fs_patches.py, gen_functions.py, gen_xdk_entries.py, update_function_map.py, progress.py,
                     extra_function_starts.tsv (Funktionsstarts außerhalb von Ghidra-Funktionen, von Hand gepflegt mit Beleg)
  tests\             Host-Unit-Tests (doctest): sha1, xbox_adpcm, xdvdfs, img_lookup (Nachbau FUN_0013FC40), path_resolve, apc_queue
  docs\              loader_design.md, function_map.tsv
```

### 5.2 CMake-Targets

| Target | Typ | Wesentliche Optionen |
|---|---|---|
| `fut_launcher` | EXE (`OUTPUT_NAME futurama`) | `enable_language(ASM_MASM)`; `target_link_options(... /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE /SAFESEH:NO /INCREMENTAL:NO /NODEFAULTLIB /ENTRY:LauncherEntry /SUBSYSTEM:WINDOWS)`; kein CRT, keine TLS |
| `fut_host` | SHARED (`futurama_host.dll`), Export `HostMain` | `/std:c++20 /permissive- /W4 /utf-8 /EHsc`; `/SAFESEH:NO` (asm-Stubs); Links: `d3d11 dxgi xaudio2 xinput` (Windows-SDK 10.0.26100), FFmpeg (ab M6, LGPL, dynamisch) |
| `fut_game` | STATIC | dekompilierter Code, in `fut_host` gelinkt |
| `fut_shared` | INTERFACE | Header |
| `fut_gen` | Custom Target | `tools/gen/*.py --check`: generierte Dateien sind eingecheckt, der Build braucht kein Python, CI prüft die Aktualität |
| `fut_tests` | EXE | doctest, läuft ohne XBE |
| `fut_selftest` | Test-Kommando | `futurama.exe --selftest --stop-at=main` (braucht die eigene Disc). Registriert, wenn die Cache-Variable `FUT_TEST_CONFIG` auf eine Test-INI zeigt; dann zusätzlich `fut_m1_check` (`tools\m1_check.py`, zehn Läufe). `enable_testing()` steht in der obersten `CMakeLists.txt`, `ctest` läuft im Build-Wurzelverzeichnis |

- Toolchain: MSVC 14.44 x86 (VS 2022 Build Tools, `vcvars32.bat`), Ninja, CMake-Presets `x86-debug`/`x86-relwithdebinfo`. clang-cl 23 als zweiter CI-Build für Warnungen: Preset `x86-clang-debug` bzw. `build.cmd <dir> Debug all clang` (Ziel `i686-pc-windows-msvc`, MASM `ml.exe`, Linker MSVC `link.exe`; Abschnitt 8, A12). Asm bleibt in MASM-Dateien.
- Abhängigkeiten: Windows SDK (D3D11, DXGI, XAudio2 2.9, XInput 1.4). FFmpeg x86 über vcpkg `x86-windows`, nur `avformat`/`avcodec`/`swscale`, dynamisch (LGPL).
- Keine Spieldaten im Repo. Die Generatoren lesen nur Analyse-TSVs (Adressen und Namen, keine Spielinhalte). XBE und Assets kommen zur Laufzeit aus dem ISO des Nutzers.

### 5.3 Coding-Konventionen

- Code und Kommentare auf Englisch, Doku auf Deutsch. Jede HLE-Funktion trägt einen Kopf mit Ordinal/Slot bzw. Adresse, Konvention, Argumenten und Beleg (Bericht und Abschnitt bzw. Adresse). Vermutungen werden als `// ASSUMPTION:` markiert.
- Gast-Strukturen sind POD mit `static_assert(sizeof/offsetof)`. Gastzeiger sind echte 32-Bit-Zeiger im selben Adressraum. Es gibt keine STL-Typen in gastsichtbaren Strukturen.
- Keine C++-Ausnahme verlässt einen HLE- oder Hook-Einstieg (`noexcept` + Fangnetz). Keine Inline-Assembly.
- Namensräume: `krnl::`, `xapi::`, `vfs::`, `gfx::`, `snd::`, `vid::`, `inp::`, `game::` (dekompiliert), `guest::` (feste Adressen und Strukturen).
- Adressen nur über benannte Konstanten bzw. `FUT_HOOK`/`FUT_GLOBAL`/`FUT_FUNC`, nie als nackte Zahl im Code.
- Formatierung per `.clang-format`; Warnungen als Fehler im CI.

### 5.4 Logging und Tracing

- **Kanäle** `CORE KRNL VFS XAPI GFX SND VID INP HOOK GAME`, **Stufen** `error warn info debug trace`. Ausgabe in eine Datei (`<userdata>\logs\futurama.log`, rotierend), im Debug-Build zusätzlich Konsole und `OutputDebugString`.
- **Nicht implementierte Kernel-Aufrufe**: Für jeden Slot mit Status `unimpl` erzeugt der Generator einen Stub mit korrektem Purge. Er loggt `UNIMPLEMENTED Ord <n> <Name>(<Rohargumente gemäß Argumentzahl>) ret=<Rücksprungadresse+Symbol>` einmal pro Rücksprungadresse und liefert den Fehlerwert seiner Rückgabeart: `STATUS_NOT_IMPLEMENTED` (0xC0000002) nur bei Exporten mit NTSTATUS-Rückgabe, sonst 0 (NULL, FALSE, 0). Ein Zeiger 0xC0000002 oder `al` = 2 sähe für den Gast wie ein Erfolg aus (Abschnitt 8, A11). Die Rückgabeart steht in der Tabelle `RETURN_KIND` des Generators. Slots, für die kein Rückgabewert sicher ist (`FATAL_STUBS`; bis M2 107/108 `MmCreate/DeleteKernelStack`, seit M2 leer, Abschnitt 8.2 B13), enden immer mit `core::Fatal`. Mit `[kernel] strict=1` folgt bei jedem Stub `__debugbreak()` und Abbruch. `stub_ok`-Slots loggen nur auf `trace`.
- **Kernel-Trace** (`--trace=krnl`): Jeder HLE-Aufruf mit dekodierten Argumenten (Pfade aus ANSI_STRING, Handles, NTSTATUS-Namen) in einen Ringpuffer; bei Absturz oder `KeBugCheck` wird er gesichert.
- **VFS-Trace** (`--trace=vfs`): Jede Öffnung mit Gastpfad → Gerät → Ergebnis. Daraus entsteht die Dateizugriffsliste (M2-Exit).
- **Falle** (D3D, unbekannte XDK-Einstiege): `TRAP <Name> @<Adresse> von <Rücksprung>` und Abbruch.
- **Laufzeitschalter**: `--stop-at=<Adresse|main>` (Hook loggt und beendet mit Code 0), `--frames=N` (Zähler im `DisplayFrame`-Slot des `NiPCRenderer`, danach geordnetes Ende), `--selftest`.

---

## 6. Meilensteine

Jeder Meilenstein endet mit überprüfbaren Kriterien. Alle Kriterien gelten zusätzlich mit „0 Fallen, 0 `UNIMPLEMENTED` auf dem geprüften Pfad, Log ohne `error`“.

### M1 – XBE gemappt, Kernel aufgelöst, bis `main()`

**Umfang:**
- CMake-Gerüst; Launcher (1.3); Host-DLL mit Config, Log und Crash-Handler.
- XBE-Loader (1.4) inkl. Polyglott; Thunk-Tabelle für alle 111 Slots mit protokollierenden Stubs (1.5, 5.4); fs-Patches (1.6).
- Kernel-Pflichtmenge M1 (Anhang A).
- HostDir-Gerät für `partition1`, Symlinks, `CdRom0` als Gerät ohne Dateien.
- XAPI-Hooks: QPC/QPF, `OutputDebugStringA`, `XMountUtilityDrive`, `XLaunchNewImageA`.
- Hook auf `main` 0xD29B0 für `--stop-at=main`.

**Exit-Kriterien:**
1. Alle Selbsttests aus 1.4.8 bestanden, insbesondere `IsBadReadPtr(0,4) == TRUE` über Gast-SEH.
2. Log zeigt `XapiInitProcess` ohne `XapiBootToDash`/`HalReturnToFirmware`.
3. Unter `partition1\` liegen `TDATA\46490002\` und `UDATA\46490002\TitleMeta.xbx` (UTF-16) sowie `TitleImage.xbx` (0x2800 Byte, identisch mit `$$XTIMAGE`).
4. `_cinit` läuft durch: Allokation des 38-MiB-Pools sichtbar, `XGetDiskClusterSizeA("U:\")` = 0x4000.
5. Der `main`-Hook greift, Exitcode 0.
6. Zehn Läufe ergeben bis auf Zeitstempel und Adressen identische Logs.

### M2 – Dateisystem, Archive, Hauptschleife ohne Bild und Ton

**Umfang:**
- `IsoDevice` (XDVDFS), asynchrones `NtReadFile`/`NtWriteFile` mit Gast-APC-Queue, alertable Waits, `NtQueryDirectoryFile`, `NtQueryFullAttributesFile`.
- `MmCreate/DeleteKernelStack` und `SwitchToFiber`-Ersatz (1.7).
- **Null-Renderer**: `NiPCRenderer` mit allen 75 Slots als No-op mit korrektem `ret N`, Pflicht-Rückgaben aus 2.2, layoutkompatible RendererData.
- D3D-Shim (8) und D3D-Fallen.
- Stubs: DSOUND (`*ppBuffer`/`*ppStream = NULL`, `GetStatus` → 0), XPP (keine Geräte), XMV (Ergebnis 2).

**Exit-Kriterien:**
1. Log: `SysPAL.img`, `FutPAL00`–`04.img` gemountet, `FutPAL05.img` nicht gefunden, PAL-Modus (`CApp+0xE8 = 0`).
2. Sprache aus der Konfiguration greift (z. B. `german` → `XGetLanguage` = 3).
3. `BUILD.TXT` = „1.25“ gelesen.
4. Der Intro-Kontext läuft; `--frames=3000` endet geordnet ohne Hänger.
5. Fiber-Wechsel > 0 und APC-Zustellungen > 0 gezählt; der Disc-Fehler-Handler (`DAT_0030E68C`-Ziel) wird nie aufgerufen.
6. Selbsttest „SEH auf Fiber-Stack“ (`IsBadReadPtr` innerhalb einer Gast-Fiber) bestanden.
7. `vfs_trace.log` listet alle geöffneten Pfade; jede Öffnung ist ein Treffer oder ein dokumentiertes „fehlt“ ([FIL] 1.2).

### M3 – Erstes Bild

**Umfang:**
- Fenster, RHI mit D3D11-Backend.
- `NiPCRenderer` P0+P1 nach [REN] 7.2: Clear, DisplayFrame, BackgroundColor, Width/Height, SetRenderTarget(NULL), Begin/EndScene, SetCameraData, RenderScreenPolygon (54), RenderShape/Triangles/Strip/Strips (48–51), CreateSourceTextureRendererData (65) mit Formaten aus [REN] 5.2.
- Property→State für Alpha, ZBuffer, Texturing (Basis-Map), VertexColor, Material; Fixed-Function-Licht im Shader.
- Present mit VSync, optionaler 50-Hz-Limiter; Screenshot-Taste. (M3b: 60-Hz-Tick und bildsynchroner Takt, 8.4 B38, B43.)

**Exit-Kriterien:**
1. Splash (`NI_Splash_PAL.nif`), Legal-Bild und Frontend-Menü erscheinen vollständig; Sichtvergleich mit xemu-Screenshots derselben Szenen.
   - **Referenzaufnahmen (vorhanden seit 2026-10-05):** `run\xemu_ref\` (git-ignoriert), erzeugt mit `tools\xemu_capture.py` aus dem Original in xemu (Retail EU, EEPROM-Sprache Deutsch; Fenster 1280×960 mit Nearest-Skalierung einer 1×-Darstellung: jeder 2×2-Block ist gleich, Korrektur in M3 Phase 5 – die Aufnahmen sind also keine 2×-Kantenglättung). `scenes.tsv` ordnet Zeitpunkte den Szenen zu (Legal 12–15 s, Splash 63 s, Frontend 75–132 s, Attract ab 138 s); je Szene liegt ein Referenzbild `ref_<szene>.png` vor, alle Einzelbilder als `t<sekunden>s.png` (Intervall 3 s), dazu `contact_sheet.png`. In frühen Bildern überdeckt die xemu-Menüleiste die obersten etwa 24 Pixel; Vergleiche ignorieren diesen Streifen. Filmszenen (XMV) sind erst ab M6 vergleichbar.
   - Neue Aufnahmen: `python tools\xemu_capture.py --exe F:\emulatoren\xemu-win-release\xemu.exe --out run\xemu_ref --duration 180 --interval 3` (nutzt die xemu-Konfiguration des Nutzers unverändert; öffnet ein Fenster mit Ton).
   - **Zusätzlich (M3 Phase 5):** `run\xemu_ref_steam\` mit `--start 70 --duration 112 --interval 0.62 --dvd <ISO>`: 68 Aufnahmen, Frontend ab 74 s. Das Intervall von 3 s ist ein Vielfaches des 1-s-Zyklus der Dampfwolke am rechten Rohr und trifft immer dieselbe (unsichtbare) Phase; 0,62 s durchläuft alle Phasen (Befund 7 des M3-Reviews).
2. Kein D3D-Debug-Layer-Fehler.
3. `--frames=3000` ohne Absturz, **Bildrate ≥ 60 fps** (Vorgabe des Nutzers; M3b, 8.4 B45, B48): `[game] tick_rate = 60` (Standard), Ø präsentierte Bildrate ≥ 59,5 Hz, Frame-Arbeit im 99. Perzentil ≤ 16,7 ms, ≥ 95 % der Frame-Starts im Takt; auf einer Anzeige mit k·60 Hz zusätzlich ≥ 95 % der Frames vblank-gekoppelt und ≥ 99 % davon genau k Refreshes, Fenster ≥ 95 % im Vordergrund. Szenen werden in Sekunden geprüft. Das ursprüngliche Kriterium „≥ 50“ gilt nur noch für `tick_rate = 50`.

### M3d – Hohe Bildraten (> 60 Hz) mit Interpolation

Reihenfolge laut [`docs/ROADMAP.md`](ROADMAP.md): nach M3c (Launcher), vor M4. Vorgabe: mindestens 60 fps, höchstens 120 Hz.

**Ziel:** Auf Anzeigen mit mehr als 60 Hz bis zu 120 verschiedene Bilder pro Sekunde zeigen, ohne den 60-Hz-Tick zu ändern. Heute zeigt jeder präsentierte Frame genau einen Simulationsstand (8.4 B42); auf 120 Hz bleibt jedes Bild 2 Refreshes stehen. Entwurf: [`analysis/loader/pacing60.md`](../analysis/loader/pacing60.md) Abschnitt 7. Ein höherer Tick (dt = 1/120) ist ausdrücklich kein Ersatz: Er beschleunigte alle tick-gebundenen Stellen (Dämpfung, Walker, Skriptschleifen, UI; timing60.md 4.10, 12) über das getestete NTSC-Verhalten hinaus.

**Voraussetzungen:**
- 60-Hz-Tick, Lockstep-Uhr und bildsynchroner Takt aus M3b. Die Frame-Clock muss Zeit mit der Render-Rate ausgeben (z. B. +½ Tick je Frame bei 120 Hz), damit jeder zweite Frame 0 Schritte hat; der Takt plant mit der Render-Rate statt mit der Tickrate (`vblank k = 1` bei 120 Hz, VRR oder Timer bei 144/165 Hz mit Grenze 120).
- Alpha je Frame: `α = Rest / dt` aus der Restschuld CApp+0xC8 nach `Game::Frame` 0xA2C20 (ohne Dekompilierung bestimmbar, pacing60.md 7). Sauberer wird es mit `Game::Frame`/`CApp::Frame` als Host-Code (M9), das ist aber keine Bedingung.
- Transform-Historie in `ni_extract`: je Gastobjekt die Weltmatrix der letzten beiden Ticks (Geometrie, Kamera aus `SetCameraData`, Knochen für das CPU-Skinning); Erkennung von Sprüngen (Teleport, Kontextwechsel, Schnitt, wiederverwendete Zeiger), die nicht interpoliert werden.
- Prüfung des Codes, der pro Frame statt pro Tick läuft, auf doppelte Frame-Zahl: `Render()` aller Kontexte (pacing60.md 7.3), die Lade-Pumpen (`FUN_000D2370` u. a.), die Speicherpflege `FUN_000A46D0`, `g_frameDelta` 0x2E7B88, die Zeitbasis der NI-Controller und Partikel.
- Messbarkeit: Bildschirmstatistik (jeder Refresh ein neues Bild), Simulationsrate unverändert 60 Schritte/s; ab M8 die Replay-Regression.

**Exit-Kriterien (Entwurf):**
1. Bei 120 Hz zeigt jeder Refresh ein eigenes Bild; bei 144/165 Hz mit VRR höchstens 120 Bilder/s gleichmäßig; die Simulation läuft unverändert mit 60 Schritten/s (`game ticks`, Lockstep-Statistik).
2. Die xemu-Vergleiche aus M3 bleiben bei abgeschalteter Interpolation unverändert; mit Interpolation keine Artefakte an Schnitten und Kontextwechseln (Screenshot-Serien).
3. Interpolation per Konfiguration wählbar („60“, „120 (interpoliert)“, „Monitor (max. 120)“); Preis etwa ein Tick zusätzliche Latenz, dokumentiert.

### M4 – Eingabe

**Umfang:** XInput-HLE (7 Einstiege) nach [AVI] 4.3, Tastatur → virtuelles Pad (konfigurierbar), Hotplug über `XGetDeviceChanges`, Rumble mit synchronem `dwStatus = 0`.

**Exit-Kriterien:**
1. Menünavigation per Pad und Tastatur.
2. Level 1-1 lässt sich starten.
3. Pad abziehen und wieder anstecken wird erkannt.
4. Rumble-Ereignisse kommen an (Log und Gerät).

### M5 – Audio

**Umfang:** DSOUND-HLE auf XAudio2 nach [AVI] 2.5:
- Buffer mit Xbox-ADPCM → PCM16 (`nAvgBytesPerSec` nicht verwenden).
- Streams mit Paketstatus-Semantik: `Process` setzt sofort E_PENDING, das Pufferende setzt 0. ADPCM-Streams werden paketweise dekodiert.
- 2D-Mixbins, X3DAudio mit 6-Punkt-Rolloff-Kurve, I3DL2-Reverb mit 5 Presets, Lautstärke in mB.
- `GetStatus` liefert nach `Stop` sofort 0.

**Exit-Kriterien:**
1. Menüklänge sind hörbar, die Frontend-Musik streamt und loopt.
2. Positionsabhängige Effekte im Level.
3. Lokalisierte Sprache in der eingestellten Sprache.
4. Kein Hänger bei Voice-Zerstörung oder Kontextwechsel; 30 Minuten Dauerlauf ohne Aussetzer.
5. Unit-Tests für ADPCM und Stream-Status bestehen.

### M6 – Video

**Umfang:**
- XMV-HLE mit FFmpeg: AVIO über die Paket-Callbacks des Spiels (0xEDBA0/0xEDE60), YUY2 in die Shim-Surface, Renderer mit YUY2 + `YuvEnable`.
- Ton über Host-DSOUND-Streams, Takt strikt nach pts.
- Decoder bleibt bis `CloseDecoder` gültig ([AVI] 3.2/3.5).

**Exit-Kriterien:**
1. Intro und Zwischensequenzen laufen bildgenau mit der Tonspur der eingestellten Sprache (FUN_000edbe0).
2. Die 25-fps-Dateien (Attract, Credits, SCI) laufen in richtiger Geschwindigkeit.
3. Ein Testmodus dekodiert alle 47 Dateien vollständig ohne Fehler.
4. Abbruch per Taste funktioniert.

### M7 – Spielstände

**Umfang:** SHA-1/HMAC verifiziert, Schlüsselmodi (4.5), `CopyFileA` für `saveimage.xbx`, Freiplatz und Cluster-Werte, „zu wenig Platz“-Meldung.

**Exit-Kriterien:**
1. Speichern, Neustart und Laden eines Spielstands, Optionen und Extras.
2. `tools/verify_save.py` prüft die HMAC-Signatur bei 0xFEC mit dem konfigurierten Schlüssel.
3. Eine manipulierte Datei wird als beschädigt gemeldet.
4. Löschen über das Menü entfernt das Verzeichnis.
5. Bis zu 25 Spielstände funktionieren.

**Stand (M7, 2026-10-12):** alle fünf Kriterien erfüllt, `tools\m7_check.py` M7-1 bis M7-8 PASS (fünf Läufe von 21–53 s); Abweichungen in 8.18, Bericht [`m7_report.md`](m7_report.md), Analyse [`m7_saves.md`](../analysis/loader/m7_saves.md). Offen: R11 (Schlüssel einer echten Xbox).

### M8 – Spielbares Level

**Umfang:**
- Renderer P2: Partikel (52), Linien (53), Purge-Slots, Palette (69), BltSource (70), Render-to-Texture (66+15) für Schatten und Effekte, Skinning, NiTextureEffect/Mehrpass, Fog, Stencil, Wireframe.
- Performance-Pass.
- Eingabe-Aufzeichnung und -Wiedergabe als Regressionswerkzeug (QPC-Hook liefert im Replay virtuelle Zeit).

**Exit-Kriterien:**
1. Level 1-1 durchspielbar inkl. Intermission bis Level 1-2.
2. 30 Minuten Spiel ohne Absturz.
3. Sichtvergleich an 10 definierten Prüfpunkten mit xemu.
4. Ein aufgezeichnetes Replay läuft reproduzierbar durch.

### M9 – Inkrementelle Dekompilierung

**Umfang:**
- Hook-Registry im Großbetrieb, `function_map.tsv`, Fortschrittskennzahlen, A/B-Schalter, Replay-Regression.
- Zielreihenfolge aus 3.4; mit jeder dekompilierten Funktion entfallen fs-Patches bzw. Emulationsteile, z. B. Fibers und APCs nach `NiUDSStream`.

**Exit-Kriterien je Charge:**
1. Alle Hooks der Charge sind standardmäßig an.
2. Die Replays aus M8 laufen identisch.
3. `function_map.tsv` ist aktuell.
4. Kein neuer `trap` und kein neues `UNIMPLEMENTED`.

### M10 – Vulkan-/D3D12-Renderer

**Umfang:** zweites RHI-Backend (Vulkan bevorzugt, D3D12 optional), native Auflösung und Widescreen (Projektion aus `NiFrustum` anpassen, [REN] 7), MSAA. (Die frühere Option „Interpolation gegen das 50-Hz-Ruckeln“ ist durch den 60-Hz-Tick aus M3b erledigt; Interpolation für höhere Bildraten ist M3d.)

**Exit-Kriterien:**
1. Bildparität zum D3D11-Backend an allen Prüfpunkten aus M8.
2. Alle Level spielbar.
3. Backend per Konfiguration umschaltbar.

Danach (außerhalb dieses Dokuments): Ablösung des XBE, sobald der gesamte Code dekompiliert ist ([README] Punkt 5).

---

## 7. Risiken und offene Fragen

| # | Risiko / offene Frage | Auswirkung | Auflösung (wann, wie) |
|---|---|---|---|
| R1 | SEH-Validierung, wenn der PE-Header überschrieben ist ([PRB] offen) | Gast-SEH (`IsBadReadPtr`, Heap-`RaiseException`) bricht den Prozess ab | Polyglott (1.4.5) + `/SAFESEH:NO`. Selbsttest in M1; sonst Handler-Validierung über den VEH umgehen (Notlösung) |
| R2 | SEHOP und Ausnahmen auf Fiber-Stacks; SEHOP-Default unter Windows 11 (Vermutung: aktiv) | Abbruch bei einer Ausnahme in einer Fiber | Terminal-Record oben im Kernel-Stack + `SwitchToFiber`-Ersatz (1.7); Selbsttest in M2 |
| R3 | TlsAlloc-Index ≥ 64 (nicht per fs-disp adressierbar) | fs-Strategie A unmöglich | im Launcher vor jeder DLL allozieren, hart prüfen; Fallback: betroffene Funktionen nach Strategie C hooken ([CPU] 2.4 C) |
| R4 | Übersehene fs:-Stellen oder unentdeckter Code (Ghidra-Lücken: 120 KB im Spiel, 23 KB NI, 14 KB REN; [REN] 8) | Absturz an seltener Stelle | Der VEH meldet Zugriffsfehler mit Befehls-Dekodierung; `fs_sites.tsv` stammt aus Recursive Descent + Lückensweep ([CPU] 1). Bei jedem Fund Liste und `extra_function_starts.tsv` ergänzen |
| R5 | Layout von RTL_CRITICAL_SECTION/KEVENT/KTIMER/KDPC im Gast unbekannt ([KRN] 10) | Gastcode, der Felder direkt liest, sähe falsche Werte | Map-basierte HLE (1.7); Scan-Skript nach direkten Feldzugriffen in XAPI/LIBC (`design_cs_fields.py`, vor M2) |
| R6 | Host-Code auf kleinen Gast-Stacks (Treiber, FFmpeg, XAudio2) | Stack-Überlauf | Hauptthread 2 MiB, Fibers 512 KiB (1.7); in M3/M5 Hochwassermarken messen und Defaults anpassen |
| R7 | LAA: Gastzeiger ≥ 2 GiB | falsche Vorzeichenvergleiche im Gastcode | Bottom-up-Allokation + Assertion (1.2); bei Treffern Arena unter 2 GiB |
| R8 | Renderer-Semantik unbekannt (Mehrpass, NiTextureEffect, Lichttypen, VS-Tabellen; [REN] 8, [D3D] 11) | optische Fehler | REN-Code als Spezifikation lesen bzw. dekompilieren; xemu-Vergleich (M3/M8); Laufzeitinspektion der Pipeline-Objekte |
| R9 | 50-Hz-Simulation auf 60-Hz-Monitoren (keine Interpolation, [STA] 3.2) | Ruckeln | **Für 60 Hz gelöst (M3b, 8.4 B38–B48):** 60-Hz-Tick mit PAL-Inhalten, Lockstep-Uhr und bildsynchroner Takt; auf dem 120-Hz-Messrechner 60,00–60,01 Hz mit 99,7–100 % der Frames genau 2 Refreshes. Rest: Auf Anzeigen mit 59,94 Hz läuft der Port mit 59,94 fps und holt alle 16,6 s einen Doppelschritt auf; Anzeigen ohne ganzzahliges Verhältnis brauchen VRR (sonst ein Spielbild je Refresh mit 0 oder 1 Schritten). **Mehr als 60 Hz** (120/144/165 Hz mit verschiedenen Bildern) braucht Interpolation zwischen zwei Simulationsständen (M3d, bis 120 Hz). Ein höherer Tick ist kein Ersatz: Er beschleunigt alle tick-gebundenen Stellen (timing60.md 4.10) über das getestete NTSC-Verhalten hinaus |
| R10 | FFmpeg für x86: Verfügbarkeit, LGPL | M6 verzögert | vcpkg `x86-windows` früh testen (vor M5); Fallback: eigener XMV-Demuxer ([AVI] 3.3) + nur libavcodec. **M6: ohne FFmpeg gelöst** – eigener Demuxer + WMV-Decoder von Windows (8.16 V1) |
| R11 | Ableitung von `XboxSignatureKey` unbelegt ([FIL] 4.3) | Xbox-Saves nicht importierbar | Modus `fixed` als Default; `xbox`-Modus mit einem echten Save prüfen. **Weiter offen nach M7** (kein echter Xbox-Spielstand vorhanden); `tools\verify_save.py --xbox-cert-key … --check-xbox` prüft ihn, sobald einer vorliegt; Umweg ohne Schlüssel: `--resign` signiert eine importierte Datei mit dem eigenen Schlüssel neu |
| R12 | Ghidra-Lücken und fehlende Funktionsstarts | Hooks und Callgraph unvollständig | `extra_function_starts.tsv` pflegen; Ghidra-Neuanalyse nur in einem eigenen, exklusiven Schritt (Projektsperre, keine parallelen Agenten) |
| R13 | Nicht standardisierte Registerkonventionen im Spielcode (MSVC 7 /GL, Vermutung) | falsche Ersatz-Signatur | vor jeder Dekompilierung die Aufrufstellen auf Registerargumente prüfen; MASM-Adapter (3.3) |
| R14 | Widersprüche zwischen Berichten: Grafikgrenze [D3D]↔[REN]; Fibers [CPU] „ersetzen“ ↔ [AVI] „Original lassen“; Header-Schreibzugriff [CPU] 5 ↔ [STA] 9.10 | – | entschieden in E6, E9 und 1.4.5 (Header RW, der Zweig wird nicht genommen) |
| R15 | [PRB] hat keinen unabhängigen Verifikationsabschnitt | Launcher-Annahmen falsch | M1 wiederholt alle [PRB]-Tests automatisch (Selbsttest) |
| R16 | Häufigkeit des Save-APC-Polls 0xE0030 zur Laufzeit ([FIL] 6) | verzögertes Speichern | **Erledigt (M7, 8.18 S8):** gemessen 10,6–16,4 ms vom `WriteFileEx` bis zur Completion 0xE01A0 (höchstens ein Frame), Protokollzeile „save write completion … delivered N ms after the write“ |
| R17 | Erreichbarkeit von „toten“ Pfaden (NiTCache, CRT-`fopen`, `Test.ldb`/Lipsync, Ambience-Streaming; [FIL] 6, [AVI] 7) | unerwartete Datei-/API-Nutzung | Fallen und VFS-Trace zeigen sie sofort; `fopen` über `CreateFileA` funktioniert ohnehin |
| R18 | Uniprozessor-Annahmen im Gastcode ([CPU] 3) | Races | Regel E10; Prozessaffinität optional auf einen Kern (Konfig, Vermutung: nur zur Fehlersuche nötig) |
| R19 | Identität der Ordinale 258/357/360 ohne Ghidra-Beleg | – | durch ghidra-xbe `kernelExports[]` bestätigt ([KRN] Verifikation); keine Aktion |
| R20 | Rechtliches | – | keine XBE- oder Asset-Daten im Repo; der Nutzer gibt sein ISO an |

Offen, aber für die ersten Meilensteine unkritisch:
- Bedeutung der KPRCB-+0x250- und fs:[0x58]-Felder (bleiben 0).
- Exakte Namen der XMV-Funktionen 0x264943/0x264FDF.
- Mixbin- und Flag-Namen in DSOUND.
- Zuordnung der 16 Eingabekanäle zu Spieleraktionen.
- Werte von Options.dat/Unlock.dat.
- Basisklasse `FUN_001091a0`.
- Ob die Hauptschleife im Retail-Spiel je endet ([STA] 10).

---

## 8. Abweichungen in M1, M2, M3, M3b, M3c, M3d, M4, M5, M6, G4, M7, G5, G6, G7, G3, G2 und M8

### 8.1 Abweichungen in M1

Stand: Phase 5 (Review-Befunde behoben), 2026-10-05; zuvor Integration (Phase 3), 2026-10-04. M1 ist erreicht: `tools\m1_check.py` (zehn Läufe, Debug und RelWithDebInfo, mit und ohne leeres `userdata`) meldet alle sechs Exit-Kriterien als erfüllt. Die Logs der zehn Läufe sind nach Entfernen der Zeitstempel identisch, bis auf 5 von 127 Zeilen, die sich nur in Heap-, Stack- und KTHREAD-Adressen unterscheiden. Die Entscheidungen E1–E12 gelten unverändert. Die folgenden Punkte weichen von den Abschnitten oben ab oder ergänzen sie.

| # | Abschnitt | Design | Umsetzung in M1 | Begründung / Beleg |
|---|---|---|---|---|
| A1 | 3.1, 3.2 | Trampoline („Original aufrufen“) erst später, über einen Längen-Disassembler | **Beobachter-Probes** (`FUT_PROBE`, `core\hooks.*`, `xapi\boot_probes.cpp`): Der Hook loggt und ruft das Original über ein Trampolin. Statt eines Längen-Disassemblers nennt jede Probe ihre Prologbytes explizit (5–9 Byte, ganze, lageunabhängige Befehle). Der Installer vergleicht sie mit der XBE-Datei, der Selbsttest `HooksInstalled` prüft das Trampolin. Sechs Probes: `XapiInitProcess` 0xF92C0, `_rtinit` 0xF97DB, `_cinit` 0xF9783, `XGetDiskClusterSizeA` 0xF8081, Pool-Init `FUN_000a4920`, `XapiBootToDash` 0xF8FCA. Abschaltbar mit `[hooks] disable = probe:*` | Die Exit-Kriterien 2 und 4 verlangen, dass `XapiInitProcess`, die 38-MiB-Allokation und `XGetDiskClusterSizeA("U:\") = 0x4000` im Log stehen. Ohne Probes stand davon auf Stufe info nichts im Log, auf trace nur indirekt. Beleg Prologe: Mit capstone wurden alle Ghidra-Funktionen geprüft. Kein Sprung und kein Aufruf zielt in die gestohlenen Bytes, und jede Befehlsgrenze liegt genau am Ende des gestohlenen Bereichs |
| A2 | 1.4 Schritt 8 | `IsBadReadPtr(0,4) == TRUE` prüft Gast-SEH | Zusätzlich gibt es den Selbsttest `GuestSehIsBadReadPtr` mit einer `PAGE_NOACCESS`-Seite: AV bei 0x1709EF → `_except_handler3` → TRUE, der VEH zeichnet das auf. `GuestIsBadReadPtrNull` bleibt | `IsBadReadPtr(0,4)` liefert TRUE ohne Speicherzugriff (NULL-Prüfung 0x1709DC–0x1709E1, `je 0x170A29`). Nur der SEH-Prolog läuft, der Dispatcher nicht. Mit dem neuen Test ist R1 für den Boot-Thread gelöst: Polyglott und `/SAFESEH:NO` genügen. R2 (Fiber-Stacks) bleibt für M2 |
| A3 | 1.10, 1.7 | Prozessende über `XLaunchNewImageA`/`HalReturnToFirmware`; `PsTerminateSystemThread` = `ExitThread` | `core::ExitHostProcess` schreibt das Log fertig und beendet dann mit `TerminateProcess` statt `ExitProcess`. Neue Exitcodes: 4 (Gast beendet sich) und 5 (Watchdog `[debug] timeout`). Endet der letzte Gast-Thread, signalisiert `PsTerminateSystemThread` `krnl::GuestThreadsExitedEvent()`, und die Message-Loop des Boot-Threads beendet den Prozess mit Code 4 | Ohne `DLL_PROCESS_DETACH` laufen keine statischen Destruktoren, während Gast-Threads an beliebiger Stelle eingefroren sind. Ohne das Ereignis würde der Prozess nach dem letzten Gast-Thread hängen |
| A4 | 1.3 Schritt 2 | MMIO-Fänger reserviert der Launcher | Der Host reserviert ihn in der Stufe `InstallCrashHandlers` (`[debug] mmio_trap`, Standard 1) | Der Launcher bleibt minimal. Die Reservierung gelang in allen Läufen. Der Lauf ohne `--stop-at` (`run\integ-main`) endet genau dort: `NiXBoxRenderer::Create` 0x14C170 → `Direct3D_CreateDevice` → `CMiniport_InitHardware` liest NV2A 0xFD001804. Das ist der erwartete Einstieg in M2 (E6, Create-Detour) |
| A5 | 1.4 Schritt 5 | gesicherte PE-Header des Launchers 0x148 Byte (Probe: 2 Sektionen) | 0x198 Byte (NT-Header + 4 Sektionen des echten Launchers) ab 0x10970 | Der Platz reicht (Ende 0x10B08 < 0x11000). `RtlImageNtHeader(GetModuleHandle(NULL))` = 0x10970 (Selbsttest `PolyglotNtHeader`) |
| A6 | 1.8 | `NtAllocateVirtualMemory` → `VirtualAlloc` | Weiterleitung an die ntdll-`Nt*`-Funktionen für den eigenen Prozess. Xbox-spezifische Flags werden entfernt, Ergebnisse ≥ 0x80000000 freigegeben und als `STATUS_NO_MEMORY` gemeldet | genaue NT-Rundung der In/Out-Parameter und NT-Statuscodes (XAPI-Heap wertet sie aus). Die Regel < 0x80000000 aus 1.2 wird dabei erzwungen |
| A7 | 4.2 | fehlender Name → `STATUS_OBJECT_NAME_NOT_FOUND` | Fehlt ein Zwischenverzeichnis, liefert der VFS `STATUS_OBJECT_PATH_NOT_FOUND` (NT-Konvention), z. B. bei `D:\fonts\kanjii.fnt` | Das Spiel prüft nur das Handle ([FIL] 1.2) |
| A8 | 4.2 Punkt 4 | Reads über das Dateiende hinaus liefern Erfolg | Reads, die über das Ende **hinausgehen**, liefern Erfolg mit gekürzter Länge. Ein Read, der **am oder nach** dem Ende beginnt, liefert `STATUS_END_OF_FILE` (wie NT) | Im Boot-Pfad kommt das nicht vor. In M2 prüfen, ob ein asynchrones `ReadFileEx` ab EOF den Disc-Fehler-Handler auslöst |
| A9 | Anhang A | M2-Slots in M2 | Vorgezogen, mit Unit-Tests in `tests\kernel_tests.cpp` und `tests\vfs_tests.cpp` (im Spiel noch nicht erreicht): alertable `KeDelayExecutionThread`/`NtWaitForSingleObject(Ex)` mit APC-Zustellung, `NtUserIoApcDispatcher`, asynchrones `NtReadFile`/`NtWriteFile`, `NtQueryDirectoryFile`, `NtQueryFullAttributesFile`, `NtFlushBuffersFile`, `Mm*ContiguousMemory*`, `DbgBreakPoint` | 76 von 111 Slots sind implementiert. Auf dem M1-Pfad bleibt kein Stub (`--strict` erreicht `main`). Offen: 107/108 `Mm*KernelStack` (M2, Fibers), 49/50 (M7) und die Slots der ersetzten Bibliotheken |
| A10 | 4.1 | `\??\Z:` → `HostDirDevice` | Gerätename `\Device\Harddisk0\Partition3` (Vermutung: erste Cache-Partition); `fFormatClean` leert `<userdata>\cache\Z` | Das Spiel benutzt `Z:` nie ([FIL] 1.1); `InitFlags & 2` = 0 für dieses XBE |
| A11 | 5.4 | Stubs liefern immer `STATUS_NOT_IMPLEMENTED` | Rückgabe nach Rückgabeart: NTSTATUS-Exporte 0xC0000002, alle anderen 0. Die Slots 107/108 (`MmCreate/DeleteKernelStack`) brechen auch ohne `strict` ab. Die Log-Zeile nennt den gelieferten Wert (`-> 0x00000000 (pointer)`) | Review-Befund 1: XAPI `CreateFiber` 0x1708DA–0x1708E4 prüft das Ergebnis von Slot 107 nur auf NULL und schreibt danach unter den vermeintlichen Stack (0xBFFFFFC2..0xBFFFFFFE). Rückgabearten: 33 NTSTATUS, 7 Zeiger, 10 BOOLEAN, 12 ULONG, 36 void (Vermutung nach den öffentlichen NT-/xboxkrnl-Prototypen, am Aufruf geprüft nur für 107) |
| A12 | 5.2 | clang-cl als zweiter Build | Preset `x86-clang-debug`, `build.cmd … clang`. Gelinkt wird mit MSVC `link.exe`, nicht mit `lld-link`. Im Launcher legt `#pragma code_seg(".text$mn")` (nur clang) den Code hinter den Platzhalter; der Host wird mit `/Oy-` übersetzt | clang-cl legt Code in `.text`, das beim Zusammenführen vor `.text$a` sortiert (Platzhalter sonst bei 0x11500). Eine von `lld-link` 23 gelinkte Host-DLL hängt bzw. endet mit 0xC0000409 schon in `LoadLibrary` (nicht weiter untersucht). `KeBugCheck` liest den EBP des Gastaufrufers neben der Rücksprungadresse; clang kennt `#pragma optimize("y", off)` nicht. Mit `link.exe` baut clang-cl ohne Warnung (`FUT_WERROR=ON`) und besteht alle M1-Kriterien |
| A13 | 5.3 / Konfiguration | – | INI: Kommentare am Zeilenende (`;`/`#` nach Leerraum, außerhalb von Anführungszeichen, wie Python `configparser`). Alle Schlüssel stehen in `kKnownKeys` (`core\config.cpp`); unbekannte Schlüssel in bekannten Sektionen und ungültige Bool-/Zahlenwerte brechen mit Exitcode 1 ab, unbekannte Sektionen erzeugen eine Warnung. `[hooks] disable` vergleicht Adressen numerisch und warnt bei Einträgen ohne Treffer | Review-Befunde 2, 4 und 8: Tippfehler wie `stop-at`, `timeout = 60s` oder `0x000F641B` wirkten bisher stillschweigend nicht |
| A14 | 4.1 | `<userdata>` beliebig | Der VFS legt Verzeichnisse über `\\?\`-Pfade an (wie `HostDirDevice` beim Öffnen). Der Launcher trägt `longPathAware` im Manifest. Ohne Win32-Langpfad-Unterstützung lehnt die Konfiguration `userdata` mit mehr als 200 Zeichen ab | Review-Befund 11: Ab etwa 228 Zeichen scheiterte `InitVfs` an `create_directories`. Getestet mit 277 Zeichen (`TitleImage.xbx` bei 322 Zeichen) |
| A15 | 1.11 | Fehler stehen im Log | Scheitert eine HostMain-Stufe, gibt der Host die letzte Fehlerzeile und den Pfad der Logdatei auf stderr aus, ohne stderr (GUI-Start) oder mit `[debug] msgbox = 1` als MessageBox. `core::Fatal` schreibt ohne Konsolen-Sink ebenfalls eine Zeile auf stderr | Review-Befund 5: Im Release-Build endete ein Start mit fehlendem ISO still mit Exitcode 2 |

Gemessene M1-Werte (Debug, `run\integ`, Trace-Lauf): Der XAPI-Heap reserviert 1 MiB (`NtAllocateVirtualMemory`, MEM_RESERVE 0x100000) und legt den Spielpool als eigenen Großblock an (Commit 0x2600050 Byte). `RtlAllocateHeap` liefert Basis + 0x30, der Pool umfasst 0x2600000 Byte. Der Schreibzugriff auf Poolanfang − 4 ([STA] 5.2) trifft damit wie auf der Xbox den Kopf dieses Heapblocks. `XGetDiskClusterSizeA("U:\")` = 0x4000, freie Einheiten 262 140 (0xFFFF0000 Byte). `TitleMeta.xbx` = UTF-16 mit BOM `TitleName=Futurama\r\n`, `TitleImage.xbx` ist byte-identisch mit `$$XTIMAGE`.

### 8.2 Abweichungen in M2

Stand: M2 Phase 4 (Review-Befunde behoben), 2026-10-05; zuvor Phase 2 (Integration). M2 ist erreicht: `tools\m2_check.py` (Abschnitt 6, M2) meldet alle sieben Exit-Kriterien als erfüllt, nach den Review-Fixes über je zehn Läufe mit leerem `userdata` in MSVC Debug, MSVC RelWithDebInfo und clang-cl Debug (`run\m2fix\de|rel|clang`), dazu Varianten mit `english`/ohne Sprachschlüssel, `french`, 100 Frames und 30 000 Frames sowie ctest (`FUT_WERROR=ON`). Bericht und Belege: [`docs/m2_report.md`](m2_report.md). `tools\m1_check.py` besteht weiterhin in allen drei Builds. Die Entscheidungen E1–E12 gelten unverändert. B1–B6 stammen von den Modul-Agenten (gfx, xdk, fiber, io), B7–B13 aus der Integration, B14–B18 aus der Behebung der Review-Befunde (Phase 4).

| # | Abschnitt | Design | Umsetzung in M2 | Begründung / Beleg |
|---|---|---|---|---|
| B1 | 2.2 (NiPCRenderer) | Objekt „aus dem Host-Heap“; D3D-Fallen für alle Einstiege mit `ext_call_sites > 0` | Das 0x6D0-Byte-Objekt kommt wie beim Original (0x14C1B6) aus dem Spielpool (`operator new` 0xA4900) und wird vom Original-Basis-ctor `FUN_00140550` gebaut. Nur der Host-Zustand an +0x6C0 liegt auf dem Host-Heap. Slot 0 zerstört über `FUN_001406E0` + `operator delete` 0xA3880. Es gibt **79** Fallen: 88 extern gerufene D3D-Einstiege minus 8 Shims minus den Import-Thunk `MmFreeContiguousMemory` 0x2417B0 (Kernel-Slot 54, HLE) | Lebensdauer und Freigabe wie im Original. Der Basis-ctor ist thiscall ohne Stackargumente (0x14AF18 `mov esi,ecx`, 0x14AF1F `call` ohne Pushes, `ret` 0x1405EF); der Renderer steht danach in der Liste 0x2D7B18 für die Purge-Broadcasts. `ret N` aller 75 Slots und 79 Fallen per capstone gegen das XBE geprüft (`gen_renderer_slots.py`, `gen_d3d_traps.py`) und zur Laufzeit gemessen (`gfx::MeasureCalleePurge`, Selbsttests `GfxRendererThunkPurge`, `GfxD3DShimTrapPurge`) |
| B2 | 2.2 (D3D-Shim), 5.4 | Header-Feldlage „Vermutung nach XDK“; `--frames` zählt `DisplayFrame` | Shim-Header: `Data` ist eine Gast-VA, `Format`/`Size` in XDK-Kodierung (`// ASSUMPTION`; gelesen nur von D3D/REN-Code, der nie läuft). Slot 65 wählt das Bild in der Reihenfolge von `FUN_0015E010`: Ist ein Dateipfad (+0x30) gesetzt, liest er die Datei über den `NiImageConverter` des Spiels, sonst nimmt er die `NiPixelData` (+0x34). Fehlt das Bild, ist die Datei unlesbar oder sind Breite/Höhe keine Zweierpotenz, gilt wie im Original still die Default-Textur des Renderers (8×8, `FUN_0014AB50`; Format A8R8G8B8 als `// ASSUMPTION`). Die D3D-Formatwahl `FUN_001572D0` wird nicht nachgebildet, ihr NULL-Fall also auch nicht (Review-Befund 1). `[render] load_textures = 0` überspringt das Lesen (Default-Textur). Im M2-Lauf haben alle 140 Quelltexturen keinen Pfad: 0 Dateien gelesen, 4 Default-Texturen (die vier Filmtexturen ohne Bild), 0 Nicht-Zweierpotenzen. Slot 15 ruft die Spiel-Helfer 0xA1930/0xA1920 wie vf15 (im Lauf nicht erreicht). `DisplayFrame` taktet mit 50 Hz (`[render] frame_rate`, 0 = ungebremst). Die Sektion heißt `[render]`, nicht `[gfx]` | Das Spiel wartet auf der Xbox in `D3DDevice_Swap` auf den PAL-VBlank (PresentationInterval ONE). Ohne Takt laufen zeitbasierte Ladeschleifen leer: 3000 Frames vergehen in etwa 2 ms, bevor die Hauptschleife läuft; Kriterium 4 braucht daher 50 Hz (60 s). `[gfx]` dient in `tests\test_core.cpp` als Beispiel einer unbekannten Sektion |
| B3 | 2.1 (DSOUND/XMV), 6 M2 | DSOUND-Stub „`*pp = NULL`“; XMV „Ergebnis 2 sofort“ | `DirectSoundCreate` liefert S_OK und ein **nicht-NULL** Dummy-Device; Buffer und Streams bleiben NULL (optional Stub-Streams mit `[audio] dummy_streams = 1`). `XMVDecoder_CreateDecoderForPackets` liefert einen nicht-NULL Dummy-Decoder, `GetNextFrame` Ergebnis 2: Jeder Film endet im ersten Update | `SoundManager::Update` 0xCBCB0 kehrt bei `+0x48 == 0` sofort zurück und übersprünge das Voice-Update `FUN_000C95F0`, das beendete Voices abschließt und SoundData freigibt; alle Lesezugriffe auf `+0x48` in 0xC9000–0xCD000 reichen den Zeiger nur an DSOUND-Einstiege weiter. Das Movie-Update 0xEDE9F setzt bei Decoder 0 nie das Fertig-Flag, der Film hinge. Beleg im Lauf: vier Filme (`fox`, `sci`, `uds`, `intro`) enden je im ersten Update, danach wechselt Intro zu Frontend |
| B4 | 1.7 (Fibers), R2 | 8-Byte-SEH-Terminal-Record „oben“, Rückgabe direkt darunter; `cfg.fiberStack` 512 KiB | Stack `max(Size, [kernel] fiber_stack = 0x80000)` committed plus eine RW\|GUARD-Seite darunter. Terminal-Record bei StackBase−0x10, gefüllt von ntdll `RtlInitializeExceptionChain` (ntdll-`FinalExceptionHandler`), darüber bei StackBase−0x18 ein Fiber-Top-Record mit Host-Handler (ruft `UnhandledExceptionFilter` = Host-Crash-Report), Rückgabe Top = StackBase−0x20; die obersten 8 Byte bleiben 0. Der `SwitchToFiber`-Ersatz ersetzt die gesicherte ExceptionList −1 eines neuen Fibers durch den Fiber-Top-Record. `fiber_stack` wird erst beim ersten `MmCreateKernelStack` geprüft und geklemmt (Warnung statt Exitcode 1) | Experiment und Disassembly von 32-Bit-ntdll (Windows 11 WoW64): Ohne Umschalten von TEB StackBase/StackLimit beendet jede Ausnahme auf einem fremden Stack den Prozess. SEHOP läuft standardmäßig nur im Audit-Modus (ExecuteFlags 0xD); erzwungen verlangt es Records < StackBase−8 (strikt) und als letzten Handler ntdlls FinalExceptionHandler. Ein Record genau bei StackBase−8 oder eine Kette mit Ende −1 beendet dann den Prozess. Ohne Fiber-Top-Record erreicht eine unbehandelte Fiber-Ausnahme den Top-Level-Filter nicht. Belege: Kopf von `kernel\mm_kernel_stack.cpp`, Unit-Tests in Kindprozessen mit erzwungenem SEHOP, Selbsttest `GuestSehOnFiberStack` (Kriterium 6), 600-Frame-Lauf unter erzwungenem SEHOP |
| B5 | 1.7 (APCs), 4.2 | Zustellung „ruft `Completion(dwErr, Information, lpOverlapped)`“ in alertable `KeDelayExecutionThread`/`NtWaitForSingleObjectEx`; A8 „in M2 prüfen“ | Zustellung nur bei `WaitMode == UserMode` **und** Alertable; `NtWaitForSingleObject` stellt nie zu (`// ASSUMPTION`: Die Xbox reicht es mit KernelMode weiter; beide XAPI-Aufrufer übergeben ohnehin FALSE). Für `ApcRoutine == NtUserIoApcDispatcher` ruft die Zustellung die Completion direkt über `core::CallGuest` (stdcall `ret 0xC`, 0x13F1D0/0xE01A0). Neuer Handle-Typ `EventObject` für den Event-Parameter von `NtReadFile`/`NtWriteFile`/`NtQueryDirectoryFile`/`NtDeviceIoControlFile`/`NtFsControlFile` (Reset beim Start, Signal bei Abschluss). **A8 bleibt** (Read ab EOF → `STATUS_END_OF_FILE`) | XAPI `SleepEx` 0xF64E7 und `WaitForSingleObjectEx` 0xF6492 pushen WaitMode 1 (Disassembly). A8: `FUN_0013F7B0` stellt einen Chunk nur an, solange die ausgerichtete Position (+0x5C) unter der Größe (+0x40) liegt; Archiveinträge liegen innerhalb ihres `.img` ([FIL] 3.4); der letzte Chunk wird höchstens um 0x7FF aufgerundet. Der Gast kann keine Events anlegen (`NtCreateEvent` ist nicht importiert); `EventObject` dient Host-Tests und künftigen HLE-Pfaden. Gemessen: 207 Zustellungen, 0 Fehler-Completions, 0 gescheiterte APC-Requests pro Lauf. **Review-Befund 2:** Ein synchron gescheitertes `NtReadFile` mit `NtUserIoApcDispatcher` (`ReadFileEx`, einziger Aufrufer NiUDSStream `FUN_0013F660` → Disc-Fehler-Handler) zählt als `failedRequests` (KRNL/error), ein gescheitertes `NtWriteFile` (`WriteFileEx`, einziger Aufrufer Speicherschicht `FUN_000E0B00`, Completion 0xE01A0) getrennt als `failedWriteRequests` (KRNL/warn). Die Statistikzeile lautet `failed APC reads N (disc error path), failed APC writes N (save layer)`; Kriterium 5 wertet nur die Reads aus |
| B6 | 5.4 (Tracing) | „VFS-Trace: jede Öffnung mit Gastpfad → Gerät → Ergebnis“; Kernel-Trace-Ringpuffer | `core\trace.*`: `--trace=vfs` schreibt `<userdata>\logs\vfs_trace.log` (pro Lauf neu, ungepuffert) mit `hit` / `missing (documented: …)` / `missing … UNDOCUMENTED` / `error`. Die dokumentierten Fehlstellen stehen mit Begründung als Tabelle in `vfs\vfs_known_missing.cpp`. `--trace=krnl` hält die letzten 1024 KRNL-Zeilen in einem Ring, den der Crash-Reporter ausgibt. Die Integration ergänzt Zeilen `archive mount` und `archive lookup` (B9). **Review-Befunde 8 und 9:** Der Trace benutzt dieselbe Uhr wie `futurama.log` (`core::LogSeconds`, vorher Prozessstart, etwa 18 ms Versatz) und nennt Thread-ID und Thread-Namen (`8228:guest-main`). Öffnungen der Loader-Selbsttests tragen am Ende `[self-test]`. Auf `partition1` gelten nur noch Spielstand-Verzeichnisse (12 Hex-Zeichen) und die fünf Spielstand-Dateien (`SaveMeta.xbx`, `Futurama.dat`, `Options.dat`, `Unlock.dat`, `saveimage.xbx`) als dokumentierte Fehlstellen, nicht mehr alles unter `\UDATA\46490002\` | Kriterium 7 verlangt eine geprüfte Liste. Ohne Tabelle der bekannten Fehlstellen wäre „dokumentiert“ nicht maschinell prüfbar |
| B7 | Anhang B, 3.1 | M2-Hooks: SwitchToFiber, Create-Detour, D3D-Shim und -Fallen, XDK-Stubs | Zusätzlich fünf **Beobachter-Probes** auf Spielcode und eine Falle (`xapi\game_probes.cpp`, Tabelle unten): `probe:Game::ArchiveMount` (`FUN_0013FE70`, 7 Byte `push -1; push 0x17CED6`), `probe:Game::ArchiveLookup` (`FUN_0013FE10`, 5 Byte `mov al,[0x2D7AE8]`), `probe:Game::Init` (`FUN_000A26A0`, 6 Byte `mov eax,fs:[0]`), `probe:CApp::Frame` (`FUN_000D2630`, 6 Byte `mov edx,[0x2D6740]`), `probe:XAPILIB::XGetLanguage` (0xF6440, 5 Byte `push ebp; mov ebp,esp; push ecx; push ecx`) und `trap:Game::DiscErrorScreen` (0xF2FD0, B10). Seit Phase 4 zusätzlich `probe:Game::ContextSwitch` (`FUN_000A48C0`, B14) | Die Kriterien 1–5 betreffen Zustände, die nur der Spielcode kennt (gemountete Archive, `CApp+0xE8`, gewählte Sprache, `BUILD.TXT`, aktueller Kontext). Prüfung mit capstone: ganze, lageunabhängige Befehle; kein direkter Sprung einer Ghidra-Funktion und kein rohes E8/E9 in 0x11000–0x293560 zielt in einen gestohlenen Bereich; keine der Funktionen liest eax/edx (bzw. ecx außer als `this`) vor dem Schreiben. Konventionen an den Aufrufern geprüft: cdecl mit `add esp,4` nach 0xD277C/0xD27AE (Mount über den Thunk 0x13FFB0), Lookup nur von 0x13F3B2, `Game::Init` per `mov ecx,esi; call` (0xD28F6); überall plain `ret` |
| B8 | 5.4 | „`--frames=N` … danach geordnetes Ende“ | **Exit-Reporter** in core (`core::AddExitReporter`; `RunExitReporters` läuft in `ExitHostProcess`, nach Namen sortiert, nicht nach einem Watchdog-Timeout): `fibers` (Fiber-Summary), `game` (Spielzustand), `gfx` (Renderer-Statistik), `io apc` (APC-Statistik), `xdk` (Aufrufzahlen). Das `--frames`-Ende in `gfx\nipc_renderer.cpp` ruft nur noch `ExitHostProcess` | Bisher rief der gfx-Agent die Statistiken der anderen Module direkt auf (Kopplung gfx → xapi/io/xdk). Die Zeilen erscheinen jetzt bei jedem geordneten Ende, auch bei `--stop-at=main` (M1-Logs haben fünf Zeilen mehr und bleiben über die Läufe identisch). Nach einem Watchdog-Timeout könnte ein hängender Gast-Thread ein Lock halten, das ein Reporter nimmt |
| B9 | 5.4, 4.4, [FIL] 1.2 | VFS-Trace listet Öffnungen von Dateien | Der Trace enthält zusätzlich jede Archivsuche des Spiels: `archive mount "sysPAL.img" -> archive #0 (…) : mounted` bzw. `not mounted` und `archive lookup "BUILD.TXT" -> sysPAL.img+0x00001000 (4 bytes) : hit archive #0` bzw. `not archived (loose file D:\ follows)` | Archivmitglieder findet der Gastcode selbst (`FUN_0013FE10`/`FUN_0013FC40`); sie werden nie über `NtCreateFile` geöffnet. Erst damit listet `vfs_trace.log` alle geöffneten Pfade: pro Asset ein Archivtreffer oder die folgende Öffnung der losen Datei (prüft `m2_check.py`). Ein Lauf hat etwa 100 Archivsuchen, davon 89 Treffer |
| B10 | 4.2, 6 M2 (Kriterium 5) | Der Disc-Fehler-Handler wird „nie aufgerufen“ | `trap:Game::DiscErrorScreen` ersetzt vf0 0xF2FD0 des Handler-Objekts 0x2D707C: `TRAP DiscErrorScreen …` (mit Stream, Handle, Offset) und `core::Fatal` (Exitcode 3). `[hooks] disable = trap:Game::DiscErrorScreen` stellt das Original wieder her. Die Funktion ist kein Ghidra-Funktionsstart und steht neu in `tools\gen\extra_function_starts.tsv` (0xF2FD0, 0xD9 Byte) | `DAT_0030E68C` wird nur bei 0x13F1E3 (Completion mit dwErr ≠ 0) und 0x13F6B2 (`ReadFileEx` liefert FALSE) gelesen (rohe Xref-Suche des io-Agenten); `FUN_000F33E0` setzt ihn über `FUN_0013FAD0` auf 0x2D707C (vtable 0x2A59BC). vf0 endet mit `jmp 0xF3004` (0xF30A4) und hat kein `ret`: Das Original zeigt die Meldung für immer, der Lauf liefe erst in den Watchdog. Die Falle macht einen Lesefehler sofort sichtbar. Im Log steht in jedem Lauf `game: disc error handler [0x0030E68C] = 0x002D707C (… = trap:Game::DiscErrorScreen)` und am Ende `disc error screen calls 0` |
| B11 | 6 M2 (Kriterium 4) | „Der Intro-Kontext läuft; `--frames=3000` endet geordnet ohne Hänger“ | `m2_check.py` verlangt ab 1000 Frames zusätzlich, dass der Intro-Kontext **endet** (Wechsel Intro → Frontend), und wie in M1 ein Log ohne `warn`-Zeile. Unter 1000 Frames warnt das Skript und prüft nur „Intro läuft + geordnetes Ende“ (Review-Befund 6) | Ein Kontext, der auf einen Film oder eine Eingabe wartet, ginge sonst als „läuft“ durch. Gemessen: Intro (Legal-Bild, Filme `fox`, `sci`, `uds`, Splash, Ladebild, Film `intro`) wechselt nach `CApp::Frame` #780 (15,9 s) zu Frontend |
| B12 | 6 M2 | – | `m2_check.py` setzt `--selftest`; `--frames=3000` und `--trace=vfs` nur, wenn die INI sie nicht setzt. Die erwartete Sprache kommt aus `[settings] language` (Name oder 1–9, Standard `english`). Die Spielsprache wird ohne Rücksicht auf Groß-/Kleinschreibung verglichen (Review-Befund 3) | Kriterien 2 und 6 werden im selben Lauf geprüft. Gegenproben: `french` → `XGetLanguage() = 4`, Spielsprache `french`, Sounds aus `sounds/localized/french/`; `english` bzw. ohne Schlüssel → `XGetLanguage() = 1`, Spielsprache `English`: `FUN_000D9B20` ersetzt den Vorgabenamen „English“ des String-Managers nur, wenn `__stricmp` einen Unterschied findet. Werte außer 3–6 (z. B. `japanese`, `portuguese`) bildet `FUN_000D9990` ebenfalls auf „english“ ab |
| B13 | Anhang A, 5.4 | `FATAL_STUBS` = 107/108 „bis M2“ | `FATAL_STUBS` ist leer (107/108 implementiert in `kernel\mm_kernel_stack.cpp`); der Mechanismus bleibt im Generator | Abschnitt 5.4 ist entsprechend angepasst |
| B14 | 3.1, B7 | Kontextwechsel aus `CApp+0xBC` nach jedem `CApp::Frame` | Neue Probe `probe:Game::ContextSwitch` auf `FUN_000A48C0` (cdecl ohne Argumente, Pflege des Spielpools 0x2E7D60; 5 Byte `mov ecx,0x2E7D60`). `Game::Frame` ruft sie bei 0xA2E40 direkt nach `cur = next` (0xA2E28) und `next = 0` (0xA2E34) und vor `Enter()` des neuen Kontexts. Die Probe loggt nur für diese Aufrufstelle (Rücksprung 0xA2E45; der Lader `FUN_0002A760` ruft sie auch). Der Vergleich nach `CApp::Frame` bleibt als Rückfall (`[seen after the frame]`) | Review-Befund 4: `Game::Frame` rechnet bis zu sechs Schritte pro Frame. Endet ein Kontext im ersten `Update()`, kann das Spiel innerhalb eines Frames hin und zurück wechseln (Frontend → Attract → Frontend, weil der XMV-Stub `Attract-PAL.xmv` sofort beendet; Review-Lauf bei 530 s). Der alte Vergleich sah das nicht. Die Probe läuft einmal pro Wechsel innerhalb der Schrittschleife und ist von Frame-Grenzen unabhängig. capstone: ganze, lageunabhängige Befehle; der einzige rohe Sprungkandidat in 0xA48C1–0xA48C4 (0xA4892) liegt mitten im Befehl bei 0xA4890 |
| B15 | 5.4 (Prozessende) | Watchdog beendet Hänger | Hängt das geordnete Ende selbst (ein Exit-Reporter oder der Log-Flush blockiert), ruft der Watchdog `ExitHostProcess(kExitWatchdog)` erneut. Dieser Aufruf beendet jetzt sofort mit Exitcode 5 (Zeile auf stderr, ohne Log), statt auf das erste Ende zu warten. Testschalter `[debug] exit_hang_test` | Review-Befund 7: Vorher endete der zweite Aufruf in `Sleep(INFINITE)`, der Prozess lief ewig. Lauf `run\m2fix\exit-hang` (100 Frames, Timeout 20 s): Exitcode 5 nach 20 s |
| B16 | 6 M2 (Kriterien 5, 7) | – | Selbsttests laufen mit `core::InSelfTest()`. Ihre Fiber-Wechsel zählen getrennt (`fiber summary: N switches (G game, S self-test; …)`, je eine Zeile `fiber: first SwitchToFiber (self-test\|game)`), ihre Öffnungen tragen im VFS-Trace `[self-test]`. `m2_check.py` verlangt Fiber-Wechsel des Spiels | Review-Befunde 5 und 8b: `GuestSehOnFiberStack` lieferte schon vor `entry` zwei Wechsel und die einzige „first SwitchToFiber“-Zeile; ein Lauf ohne einen Fiber-Wechsel des Spiels hätte Kriterium 5 bestanden |
| B17 | 5.3 / Konfiguration (A13) | Ungültige Werte → Exitcode 1 | Auswahl-Schlüssel (`KeyType::Choice`): `[settings] language` (Namen aus `kernel_settings.cpp` oder 1–9), `[saves] key_mode` (fixed\|xbox), `[render] d3d_traps` (abort\|log) werden schon beim Laden der Konfiguration geprüft | Review-Befund 10: `language = klingon`, `0` oder `10` endeten als Stufenfehler mit Exitcode 2 (`stage InitKernel failed`). Jetzt Exitcode 1 mit `invalid value for [settings] language`. Ein Unit-Test prüft, dass jede Auswahl auch vom Kernel angenommen wird |
| B18 | 4.4 (Default-Textur) | – | Slot 65 nach der Reihenfolge von `FUN_0015E010` (Pfad vor `NiPixelData`, Default-Textur 8×8 für fehlende, unlesbare und Nicht-Zweierpotenz-Bilder), siehe B2 | Review-Befund 1. Die Warnung „could not read it (size stays 0)“ entfällt; das Original weicht still aus |

Neue Hook-Ziele (Ergänzung zu Anhang B):

| Adresse | Ziel | Art | ab |
|---|---|---|---|
| 0x13FE70 | `probe:Game::ArchiveMount` (Archiv einhängen, cdecl 1) | Probe | M2 |
| 0x13FE10 | `probe:Game::ArchiveLookup` (Archivsuche, cdecl 3) | Probe | M2 |
| 0xA26A0 | `probe:Game::Init` (thiscall) | Probe | M2 |
| 0xD2630 | `probe:CApp::Frame` (thiscall, jeder Frame) | Probe | M2 |
| 0xF6440 | `probe:XAPILIB::XGetLanguage` | Probe | M2 |
| 0xF2FD0 | `trap:Game::DiscErrorScreen` (vf0 des Disc-Fehler-Handlers, Endlosschleife) | Falle (Detour) | M2 |
| 0xA48C0 | `probe:Game::ContextSwitch` (Spielpool-Pflege, cdecl 0; loggt nur den Aufruf aus `Game::Frame` 0xA2E40, B14) | Probe | M2 (Phase 4) |

Gemessene M2-Werte (MSVC Debug, `run\m2fix\de` nach den Review-Fixes, 3000 Frames bei 50 Hz, je Lauf etwa 60 s): 2985 Aufrufe von `CApp::Frame` (die übrigen 15 `DisplayFrame` kommen aus `Game::Init` und dem Kontextwechsel `FUN_0002A660`), 4932 Fiber-Wechsel (4930 des Spiels, 2 des Selbsttests), 27 Kernel-Stacks (höchstens 10 gleichzeitig), 207 APC-Zustellungen (205 an XBE-Completions, höchstens 10 gleichzeitig ausstehend), 0 Fehler-Completions. Archive: `sysPAL.img` (Header 0xCF7) und `FutPAL00`–`04` als #0–#5, `FutPAL05.img` → `STATUS_OBJECT_NAME_NOT_FOUND`. `CApp+0xE8 = 0` (PAL, 50 fps, dt 0,02 s, Film-Suffix `PAL`), `XGetLanguage() = 3`, Spielsprache `german`, `BUILD.TXT` = `1.25` (`sysPAL.img`+0x1000, 4 Byte). Kontexte: Intro, Frontend, Attract, Ingame, Death, Intermission. `vfs_trace.log` hat 140 Zeilen: 89 Archivtreffer, 20 Treffer des Spiels und 1 des Selbsttests (`[self-test]`), 8 dokumentierte Fehlstellen (`FutPAL05.img`, `Video\*-PAL.txt`, `sounds\menu\MusicClip.wav`), 11 nicht archivierte Assets, 0 UNDOCUMENTED. Renderer: Slot 65 hat 161 Aufrufe, davon 140 mit neuer Source-RendererData (21-mal lag schon RendererData an); 4 davon bekommen die Default-Textur (Filmtexturen ohne Bild). Das Log ist frei von `warn`/`error`, `UNIMPLEMENTED` und `TRAP`.

Beobachtungen und offene Punkte nach M2:

- **Attract-Modus:** Nach etwa 60 s ohne Eingabe wechselt Frontend zu Attract, öffnet `Video\Attract-PAL.xmv` und kehrt wegen des XMV-Stubs sofort zu Frontend zurück: meist im nächsten `CApp::Frame`, je nach Lage der festen Schritte auch im selben (B14; 10-Minuten-Lauf `run\m2fix\long`, 30 000 Frames: 9 Zyklen, 19 Wechsel, 9 Öffnungen von `Attract-PAL.xmv`). Das ist das erwartete Stub-Verhalten bis M6.
- **Im Lauf nicht erreicht:** Slot 15 (`SetRenderTarget` auf eine Textur), der Texturdatei-Pfad von Slot 65 (keine der 140 Texturen hat einen Pfad), die Stub-Streams (`[audio] dummy_streams`), APC-Fehlerpfade im Spiel (nur Unit-Tests), der Spielstand-Pfad mit `WriteFileEx` (0xE0030, M7) und die Falle `trap:Game::DiscErrorScreen` selbst (ein Lesefehler lässt sich ohne Fehlerinjektion im VFS nicht auslösen; geprüft sind Installation, Prologbytes und die Registrierung des Handlers im Lauf).
- **`[render] frame_rate = 0`** taugt nicht für Kriterium 4 (B2); `m2_check.py` warnt dann.
- **Doppelte Purge-Messung:** `xdk::MeasureEntryPurge` und `gfx::MeasureCalleePurge` implementieren dieselbe Methode (Kontext eines Probe-Threads umbiegen) und sind Kandidaten zum Zusammenlegen.
- **APC-Queues beendeter Threads** werden nicht verworfen. Das XBE hat genau einen Gast-Thread.
- **Jeder installierte Hook** loggt eine `HOOK INFO`-Zeile, dazu kommt eine Summenzeile. Seit Phase 4 sind es 161 Hooks: 162 Zeilen im M2-Log, 163 im M1-Log (zusätzlich der Hook `stop-at`). Die Logs bleiben über Läufe identisch (M1-Kriterium 6).
- **Kontext-Historie** der Spielzusammenfassung: höchstens 32 Einträge; die `game: context`-Zeilen im Log sind vollständig (B14).

### 8.3 Abweichungen in M3

Stand: M3 Phase 5 (Review-Fixes), 2026-10-05; B30–B37 und die Korrektur von B22 stammen aus Phase 5, Bericht: [`docs/m3_report.md`](m3_report.md). Die Slots von `NiPCRenderer` zeichnen jetzt über `ni_extract` → `ff_pipeline` → RHI (D3D11). Vertrag der Phase: `src\host\gfx\GFX_CONTRACT.md` (Abschnitt 9 „Phase-3 results“). Prüfung: `tools\m3_check.py` (alle drei M3-Exit-Kriterien plus alle M2-Kriterien und die allgemeine Regel) meldet ALL PASS in MSVC Debug (`run\m3`, sechs Läufe), MSVC RelWithDebInfo mit Debug-Layer (`run\m3\rel`) und clang-cl Debug (`run\m3\clang`); alle drei Builds mit `FUT_WERROR=ON` ohne Warnung, `fut_tests` 187/187 in Debug und RelWithDebInfo (neu `tests\nipc_gpu_tests.cpp`: Namen, Halbpixel-Verschiebung, ClearBuffer auf WARP); `tools\m1_check.py` und `tools\m2_check.py` bestehen weiter, `m2_check` sowohl mit dem neuen Standard `d3d11` (`run\m3\m2`) als auch mit `[render] backend = null` (`run\m3\m2-null`). Die Entscheidungen E1–E12 gelten unverändert. B19–B29 ergänzen 8.2.

| # | Abschnitt | Design / Vertrag | Umsetzung in M3 | Begründung / Beleg |
|---|---|---|---|---|
| B19 | GFX_CONTRACT 3, 11 (Pixelzentren) | Vortransformierte Quads (Slot 55) +0,5 Pixel im Shader; transformierte Geometrie folgt den D3D11-Regeln („Subpixel-Versatz, bei 640×480 unsichtbar“) | **Halbpixel-Korrektur auch für transformierte Geometrie:** `GpuRenderer` verschiebt die an `ff_pipeline` übergebene Projektion (und die Bildschirmprojektion von Slot 54) um +0,5 Pixel nach rechts und unten (`clip.x += w/vw`, `clip.y -= w/vh`, Spalte 3 zu den Spalten 0/1 addiert) | D3D8 und der NV2A legen Pixelzentren auf ganzzahlige Koordinaten, D3D10+ auf +0,5. Vergleich mit den 19 xemu-Aufnahmen des Frontends auf den Pixeln, die in allen Aufnahmen statisch sind (73 % des Bildes): ohne Korrektur 61–62 % exakt gleich, mittlere Abweichung 3,9–4,3; mit Korrektur 93–94 % exakt, 0,26–0,43 (je 3 Frames, `run\m3\ab`). Die vortransformierten Szenen (Legal, Tech-Notice) waren schon vorher texelgenau |
| B20 | GFX_CONTRACT 8 (`[render] backend`) | Standard `null` bis zur Integration | Standard **`d3d11`**; `backend = null` bleibt der M2-Pfad (Null-Renderer, `[render] validate`) | Vertrag: „the integration makes d3d11 the default when M3 passes“. `m1_check` (Fenster wird in Stufe 12 versteckt angelegt) und `m2_check` bestehen in beiden Einstellungen |
| B21 | GFX_CONTRACT 9 | `HostRenderer` erhält Device, `FixedFunctionPipeline`, Arena, `StateShadow`, Kamera | Eigene Klasse **`gfx::GpuRenderer`** (`gfx\nipc_gpu.h/.cpp`) hinter `HostRenderer::gpu`; `nipc_renderer.cpp` behält die gastsichtbare Seite (Objekt, RendererData, Slot-65-Quellreihenfolge, Zähler, `--frames`) | Trennung von Gast- und Hostseite; ohne Fenster oder bei einem Device-Fehler bleibt `gpu` leer und der Renderer ist der Null-Renderer (GFX/error) |
| B22 | B2, B18 (Slot 65), m2_report 9 | `FUN_001572D0` nicht nachgebildet, Pixelformat der Quelle, Default-Format `// ASSUMPTION` | Slot 65 nach `FUN_0015E010` (**Korrektur Phase 5:** bis auf den letzten Schritt, die Freigabe des Pixelpuffers – nachgetragen in B30): Quellreihenfolge, Zweierpotenz, **Formatwahl `FUN_001572D0`** (`tex::SelectTextureSource` mit Bildformat und FormatPrefs +0x14, in beiden Backends), +0x14..+0x33 = NiPixelFormat des gewählten Tabelleneintrags (Default-Textur: Eintrag 5 A8R8G8B8 mit den gepackten Deskriptoren aus texconv), +0x50 = Mip-Stufen der Gerätetextur (Filterregel `FUN_00161E40`), +0x54 aus FormatPrefs +0x18; Upload: unkomprimiert → BGRA8 (Bitreplikation), DXT → BC1–3; texconv-Fehler (Bump, 8-Bit-RGB) → Default-Textur (gezählt, das Xbox-Original zeigte sie). Host-Felder +0x64 Handle, +0x68 Quelle + `kHostOwnsTexture`, +0x6C Revision | Lauf (3000 Frames): 140 Source-Texturdaten, 136 hochgeladen (alle 16 Bit → BGRA8, 15 MB Quelle), 4 Default (Filmtexturen ohne Bild), 0 ohne Xbox-Format, 0 Konvertierungsfehler. `keep16Bit` (halber Speicher) bleibt aus: die GPU tastet 565/4444 als exaktes UNORM ab, die xemu-Aufnahmen passen texelgenau zur Bitreplikation |
| B23 | GFX_CONTRACT 9 (TextureResolver) | Resolver liest +0x64 entlang der Kette | Resolver = Texturmanager vf2 **`FUN_0015F930`**: fehlende RendererData einer NiSourceTexture wird angelegt (`FUN_0015E010`; `// ASSUMPTION` IsKindOf = exakte VTable 0x2A8034), **`FUN_0015E260`** lädt nicht-statische Texturen (NiSourceTexture +0x38 = 0) bei geänderter NiPixelData-Revision neu hoch; Filmtexturen (+0x3C = YUY2-Fläche des Shims) und gerenderte Texturen (P2) → schwarze 1×1-Textur | Reihenfolge des Originals. Im Lauf: 0 bei Bedarf angelegt, 0 Neu-Uploads, 0 Film/gerendert (die Filme enden im ersten Update, M6) |
| B24 | 2.2, GFX_CONTRACT 11 (Render to Texture P2) | – | Ist eine Textur Renderziel (Slot 15 ≠ NULL), sind Draws und Clears gezählte No-ops (`texture target`); Slot 15(NULL) bindet Backbuffer + Tiefe und setzt die Viewport auf das ganze Ziel (D3D8-Regel) | Im Lauf nie erreicht (0 Zählungen) |
| B25 | Slot 27 (M2) | M2 klemmte die Hintergrundfarbe auf 0..255 | wie 0x14B0C6..0x14B10B: `_ftol(c * 255) & 0xFF` je Kanal (Überlauf wie das Original), Alpha 0xFF | Disassembly (Konstante 0x2A14C4 = 255,0) |
| B26 | GFX_CONTRACT 8 (Risiko R6) | Slot 65 ruft D3D11 auf einem Lade-Fiber-Stack | Messung statt Annahme: Slot 65 füllt den freien Stack unterhalb des Einstiegs mit einem Muster und sucht danach die tiefste überschriebene Stelle (Zeile `slot 65 stack`) | 81 von 140 Aufrufen laufen auf Fiber-Stacks; tiefste Nutzung 21 680 Byte, kleinster Abstand zur Guard-Page 502 444 von 524 288 Byte – `[kernel] fiber_stack` 512 KiB reicht mit großem Abstand (Debug-Build mit Debug-Layer) |
| B27 | 6 M3 Kriterium 2 | „Kein D3D-Debug-Layer-Fehler“ | `m3_check` verlangt Zustand `enabled` (oder `not installed` mit Hinweis, nichts geprüft); `off` scheitert. Die RelWithDebInfo-Prüfkonfiguration setzt `[render] debug_layer = on` (`auto` = nur Debug-Builds) | Sonst bestünde ein Release-Lauf ohne jede Validierung |
| B28 | GFX_CONTRACT 8 (`trace_frames`) | Draw-Trace der Szenen-Frames | Die Prüfkonfiguration verfolgt nur die drei Intro-Szenen (6–7 Draws); das Frontend wird über die Zeile `draw summary frame N` gezählt. Ein vollständig geloggter Frontend-Frame (≈345 Zeilen) kostet ≈30 ms Logging und kostet den 50-Hz-Takt einen Frame (Durchschnitt 49,99 Hz statt 50,01 Hz). Diagnose-Läufe mit Frontend-Trace: `run\m3\trace` | Gemessen mit einem temporären Langsame-Frames-Log: der einzige Frame > 40 ms war der getracte |
| B29 | `ni_guest_layouts.h` | `NiTextureFormatPrefs` +0x18 alphaFormat, +0x1C mipMapped | getauscht: +0x18 mipMapped, +0x1C alphaFormat | Befund des texconv-Agenten (`FUN_0015E010` 0x15E13B, `FUN_001572D0`, NIF-Reihenfolge); kein Code las die Felder unter den alten Namen |

**Phase 5 (Review-Fixes, Befunde siehe [`docs/m3_report.md`](m3_report.md) Abschnitt 8):**

| # | Abschnitt | Design / Vertrag | Umsetzung in M3 Phase 5 | Begründung / Beleg |
|---|---|---|---|---|
| B30 | B22, texconv.h Schritt 5 | Freigabe des Pixelpuffers „Sache der Integration“, nicht umgesetzt | Slot 65 gibt nach dem Upload den Pixelpuffer der NiPixelData frei wie `FUN_0015E010` 0x15E1A6..0x15E1DD: Bild ≠ Default-Textur, NiSourceTexture +0x3C ≠ 0 (`ni::kNiSourceTextureDiscardPixelsHint`, aus dem NIF-Strom ab 4.1.0.13), Stufe 0 ≤ 512×512 → `FUN_001511B0` (`FUN_000A3880(+0x30)`, +0x30 = 0). Beide Backends (der Null-Renderer so, als wäre der Upload gelungen; das Original scheitert nur ohne Speicher). Der Resolver lässt einen Revisions-Neu-Upload ohne Puffer aus (gezählt). Exit-Zeilen `NiPCRenderer pixel buffers …` und `game memory pool: …` | Befund 1. Frontend-Lauf (3000 Frames): 115 Puffer, 2558 KiB freigegeben, 18 behalten (+0x3C = 0), 3 behalten (> 512); Spielpool beim Ende 5839 KiB statt 8295 KiB, Spitze 6855 KiB statt 8892 KiB (Gegenprobe ohne Freigabe `run\m3final\diag\nofree`), gleich in d3d11 und null |
| B31 | 2.2, GFX_CONTRACT 8 (Swapchain) | Swapchain 640×480, `FLIP_DISCARD`, Inhalt nach Present undefiniert | Zwei **persistente Offscreen-Backbuffer** in Rotation (`DeviceDesc::bufferCount`): nach Present enthält der Backbuffer das Bild von vor zwei Presents wie beim Xbox-Flip (`FUN_00146260` liefert `D3DSWAPEFFECT_DISCARD` = 1 bei `BackBufferCount` 1, also Front + Back). Present kopiert ihn mit einem Present-Pass in eine Swapchain in Fenstergröße (`FLIP_DISCARD` ist dafür richtig, sie wird jedes Mal ganz neu gezeichnet; `ResizeBuffers` bei Größenänderung, Größe im DPI-Kontext des Fensters gelesen). Screenshots lesen immer den 640×480-Backbuffer | Befunde 2 und 8. Legal-Bild/Tech-Notice zeichnen pro Frame 0, 6 oder 12 Mal und löschen nur Z; ein Frame ohne Draw zeigt jetzt definiert das Bild von vor zwei Presents (`draw summary frame 115: 0 draw calls …; shows the picture of frame 113 (6 draw calls, 6 drawn)`, Bild identisch). `d3d11 frames:` zählt solche Frames (je 3000 Frames: 23–57 ohne Draw, 5–39 ohne Draw und ohne Farb-Clear) |
| B32 | GFX_CONTRACT 8/11 (Fenster) | Compositor streckt 640×480 bilinear | `[render] scaling = linear \| point \| integer` (Standard `linear`): bilinear bzw. nächster Texel auf das größte 4:3-Rechteck, oder ganzzahliger Faktor zentriert mit schwarzem Rand. Höhere interne Auflösung bleibt M10 | Befund 11. 4K-Monitor, 225 %: Ansicht 2631×1973, `integer` = Faktor 4 (2560×1920); Fensteraufnahmen `run\m3final\diag\window_*.png` |
| B33 | B19 (Pixelzentren) | +0,5-Pixel-Verschiebung, D3D11-Rasterregeln mit 8 Bit Subpixel | Der Ubershader rastet transformierte Vertizes in D3D8-Pixelkoordinaten auf **1/16 Pixel** (Rundung zum Nächsten, NV2A-Genauigkeit), bevor D3D11 sie sieht (`SnapToSubpixelGrid`) | Befund 9: die Oberkante von „Neues Spiel“ liegt bei y = 189,000..189,008; ohne Raster blieb Zeile 189 dunkel. Die xemu-Aufnahmen sind 1×-Darstellungen (B37) und bestätigen die Abweichung. Statische Frontend-Pixel exakt gleich: 93,0–93,8 % → 98,0–98,8 % (Mittel 0,07–0,28 → 0,011–0,015); Abschneiden statt Runden: 89 % |
| B34 | Slot 60 (M2 No-op) | – | Slot 60 `PurgeEffect` (ret 4): IsKindOf(NiLight) über die RTTI-Kette wie 0x146E4C..0x146E65, dann `ni::PurgeLight` (Licht verlässt die aktiven Lichter wie `FUN_0015B9E0`; der letzte Effektzustand bleibt) | Befund 4. In den M3-Szenen 0 Aufrufe |
| B35 | 1.10 / platform (WM_CLOSE) | WM_CLOSE → `ExitHostProcess` auf dem UI-Thread | WM_CLOSE setzt `platform::CloseRequested()`; der nächste `DisplayFrame` beendet den Prozess auf dem Gast-Hauptthread (Exit-Reporter auf dem Renderthread); Rückfall nach 3 s auf dem UI-Thread. Der Reporter „d3d11“ pumpt dort noch einmal die Info-Queue, `GpuRenderer::LogStatistics` loggt auf einem fremden Thread nur einen Hinweis | Befunde 5 und 10. Live-Test (`run\m3final\diag\live*`): `window closed - ending after frame 1297` auf `guest-main`, Exitcode 0 nach 0,06–0,08 s |
| B36 | 6 M3 Kriterium 3 | „Bildrate ≥ 50“ = Durchschnitt des 50-Hz-gepacten Laufs ≥ 50,0 | `m3_check` M3-3: Exitcode 0 + Frame-Limit, **Frame-Arbeit** (Gast + Renderer ohne Present und Takt, neue Zeile `present: frame work …`) im 99. Perzentil ≤ 20 ms und Durchschnitt des gepacten Laufs ≥ 49,5 Hz; Takt-Neustarts werden gemeldet | Befund 3: jeder Frame, der mehr als eine Periode zu spät ist, startet den Takt neu und kostet ≈ 0,017 Hz für immer. Gemessen: Ø 2,3–3,1 ms, 99 % ≤ 3,4–4,6 ms (≈ 330–445 Hz tragfähig), gepact 50,01 Hz |
| B37 | 6 M3 Kriterium 1 (xemu) | Referenz „2× interne Auflösung“, Dampfwolke als ungeklärter Unterschied | Die xemu-Aufnahmen sind 2×-Nearest-Vergrößerungen einer 1×-Darstellung. Neue Aufnahmen `run\xemu_ref_steam` (0,62 s) zeigen die Dampfwolke wie bei uns; `m3_check` bildet die statische Maske aus beiden Verzeichnissen und maskiert die Dampfregion ausdrücklich | Befund 7: Zyklus 50 Frames = 1,0 s, 3-s-Aufnahmen treffen immer die unsichtbare Phase. Regionshelligkeit in xemu 22,2 / 24,8 / 28,5 / 35,8 / 39,1 = dieselben Werte wie bei uns |

Gemessene M3-Werte nach Phase 5 (MSVC Debug, `run\m3final\debug`, 3000 Frames, Debug-Layer an, NVIDIA RTX 5090, Feature-Level 11_1): ≈ 762 000 `RenderShape` (≈ 342–345 pro Frontend-Frame), 44–45 `RenderScreenPolygon`, ≈ 4840 `RenderScreenTexture` – **alle gezeichnet**, 0 übersprungen, 0 Fehler, 0 „unsupported“, 0 außerhalb einer Szene; 49–51 und 60 nicht erreicht. 137 Texturen angelegt (136 hochgeladen, 15 101 KiB BGRA8), 6618 geskinnte Draws, 0 Multi-Map-Näherungen, 0 Textur-Effekte. `D3D11 debug layer: enabled - 0 errors, 0 warnings, 0 corruption`. `present: 3000 frames in 59.96 s (average 50.01 Hz, slowest 1 s window 49.0–49.4 Hz, longest frame 32–40 ms)`, 0 Takt-Neustarts, Frame-Arbeit Ø 3,06–3,07 ms (RelWithDebInfo 2,25 ms), 99 % ≤ 4,6 ms. Szenen wie in Phase 3: Legal-Bild Frames 1–≈310, Tech-Notice ≈320–≈570, Ladebild ≈575–≈785, Frontend ab ≈795.

**Vergleich mit xemu** (`run\xemu_ref` + `run\xemu_ref_steam`, 640×480-Box-Verkleinerung der 2×-Nearest-Aufnahmen): Legal-Bild und Tech-Notice mittlere Abweichung 0,00 (texelgenau), Ladebild 0,00–0,20 (nur die animierte Dose), Frontend auf den 61 % Pixeln, die in allen 81 Aufnahmen statisch sind: 0,01–0,02, 98,0–98,8 % exakt gleich (vor Phase 5: 93 %).

**Verbleibende sichtbare Unterschiede (ehrlich):**
- **Animationsphasen** (Logo-Buchstaben, Blitze, Zahnräder, Dampfwolke, Dose im Ladebild, Puls der Menü-Hervorhebung): andere Zeitpunkte als in den Aufnahmen; kein Renderer-Unterschied. Die in Phase 3 als ungeklärt geführte Dampfwolke ist auch im Original sichtbar (B37).
- **Restliche Rasterkanten:** knapp 2 % der statischen Frontend-Pixel weichen ab, vor allem an Kanten dünner Geometrie (Rohre, Zahnräder). xemu ist selbst eine Emulation (Vulkan-Rasterizer); ohne Hardware-Aufnahme bleibt offen, welche Seite dem NV2A näher ist.
- **Filme** (fox/sci/uds/intro): XMV-Stub, kein Bild (M6); die Filmtextur würde schwarz gezeichnet.
- Nicht nachgebildet (P2, im Lauf 0-mal gebraucht): Multi-Pass- und Multi-Map-Texturpipelines (nur die Basis-Map-Stufe), NiTextureEffect, Render to Texture, Punkte/Linien (52/53), Batch-Slots, `SetScreenSpaceOffset` (Slot 15 wird nie mit Textur gerufen).
- Die Phase-3-Aussage „xemu rendert mit doppelter Auflösung, deshalb weiche Kanten“ war falsch (B37); die Kantenunterschiede waren Raster- (B33), keine Glättungsunterschiede.

Offene Punkte nach M3:
- `[render] validate` läuft nur mit dem Null-Renderer.
- Ein Device-Verlust (`DeviceLost`) endet mit `core::Fatal` (GFX/error); auf WARP nicht provozierbar, ungetestet.
- Kein Hardware-Vergleich (nur xemu); die Vertex-Rasterung von 1/16 Pixel ist am xemu-Bild gemessen, nicht am NV2A-Datenblatt belegt.
- Der Rückfall-Timer von WM_CLOSE (3 s) beendet den Prozess auf dem UI-Thread, falls der Gast keinen Frame mehr zeigt; dann gelten die Einschränkungen von B35 (Statistik nur teilweise).

### 8.4 Abweichungen in M3b (60-Hz-Zeitbasis und bildsynchroner Takt)

Stand: M3b Phase 5 (Review-Korrekturen und Abschluss), 2026-10-06; Bericht [`docs/m3b_60hz_report.md`](m3b_60hz_report.md). Anlass ist die verbindliche Vorgabe des Nutzers: Der Port läuft mit **mindestens 60 fps**, das 50-Hz-PAL-Timing ist unerwünscht. Auf dem Messrechner (3840×2160, 120 Hz, RTX 5090, 225 %) fiel der 50-Hz-Takt mit VSync auf 49,2–49,3 Hz (35–41 Takt-Neustarts), weil 50 Hz kein Teiler von 120 Hz ist; 60 Hz sind genau zwei Vblanks. Analysen: [`analysis/loader/timing60.md`](../analysis/loader/timing60.md) (Zeitbasis, Patch-Tabelle, tick-gebundene Stellen, NTSC-Prototyp) und [`analysis/loader/pacing60.md`](../analysis/loader/pacing60.md) (Bildtakt). Schnittstellen: `src\INTERFACES.md` 4.19 (Zeitbasis) und 4.20 (Takt), `GFX_CONTRACT.md` Abschnitt 8 (Nachtrag M3b). Die Entscheidungen E1–E12 gelten unverändert. Die PAL-Inhalte bleiben: CApp+0xE8 = 0, PAL-Archive, Sprachwahl, `*-PAL.xmv`, PAL-Splash und -Legal.

| # | Abschnitt | Design | Umsetzung in M3b | Begründung / Beleg |
|---|---|---|---|---|
| B38 | 1.9, 7 R9, [STA] 3.4 | Fester Zeitschritt dt = 0,02 s, „weil die PAL-Archive 50 Hz festlegen“; 50-Hz-Simulation, Interpolation erst M10 | **Spiellogik mit 60 Hz bei PAL-Inhalten.** Neue HostMain-Stufe 8b `ApplyGameTiming` (zwischen `ApplyFsPatches` und `InstallHooks`): `[game] tick_rate = 60` (Standard) setzt im PAL-Zweig von `Game::Init` 0xA26A0 fünf Operanden auf die Werte des NTSC-Zweigs (0xA2714 fps-Quelle 50.0f → 60.0f @0x2942CC, 0xA271A, 0xA272A, 0xA2734 dt-Quelle 0.02f → 1/60 @0x29E390, 0xA273A) und zwei `.data`-Anfangswerte (fps 0x2D6744 = 60.0f, dt 0x2D6748 = 1/60). Das Tag-Store 0xA2744 („PAL“) und CApp+0xE8 bleiben unberührt. `tick_rate = 50` patcht nichts (Original-PAL, xemu-Vergleiche). Andere Werte: Exitcode 1 | Vorgabe ≥ 60 fps. Der NTSC-Zweig desselben Blocks ergibt genau diesen Zustand; 3112 von 3116 Spielfunktionen, alle 19 timing-relevanten Funktionen und 21 von 23 Level-Skripten sind im NTSC-Prototyp identisch, der mit dt = 1/60 lief (timing60.md 2, 5). Der `.data`-Wert fps wirkt über den Pause-ctor `FUN_000E4AA0` vor `Game::Init`: Tastenwiederholung 15 Ticks = 0,25 s (PAL 0,24 s). Rücklesen nach `Game::Init` in jedem Lauf: `fps 60 (0x42700000), dt 0.0166666675 s (0x3C888889), …, CApp+0xE8 = 0, movie tag "PAL" - ok` |
| B39 | 3.1 (Patch-Arten) | Thunk-Slot, fs-Retarget, Funktions-Detour, VTable-Slot | Neue Patch-Art **Operanden-/Datenpatch** (`xapi::ApplyTimingPatches`): Tabelle {Adresse, ganzer Originalbefehl bzw. Originalwert, neue Bytes}. Nur Operandenbytes ändern sich (Strukturprüfung), keine Überlappung mit fs-Stellen. Vor dem Schreiben müssen alle 7 Zeilen und 16 unveränderte Referenzzeilen (Verzweigung 0xA26BD, NTSC-Befehle, Tag-Store, gemeinsamer Schluss, `.rdata`-Konstanten 60.0f und 1/60) im Abbild **und** in der XBE-Datei stimmen, sonst wird nichts geschrieben; danach Rücklesen. Selbsttest `GameTimingPatched` (23/23 Zeilen, keine Hook-Überlappung); `MappingMatchesFile` nimmt die 7 Bereiche aus | Die gemeinsamen `.rdata`-Konstanten haben 9 bzw. 13 weitere Nutzer und dürfen nicht geändert werden; nur der Operand ist patchbar. Ein Immediate-Patch allein wirkt nicht, weil fps/dt im Schluss aus dem x87-Stapel gespeichert werden (timing60.md 2.1–2.3) |
| B40 | 6 M3, timing60.md 4.10, 12 | – | **Tick-gebundene Stellen bleiben unverändert (NTSC-Parität)** und laufen wie in der NTSC-Fassung. Kosmetik (20 % schneller): Frontend-Fade (50 Ticks), Eingabesperre (30), Menü-Entprellung und „Neues Spiel“ (5), Overlay-Einblendung (25), Kamerawackeln, Glättungen pro Tick und die Kamerafeder (feste 0,01-s-Unterschritte, 2 je Tick), Intro-Ladebeginn (21 Ticks). **Spielrelevant (Review M3b Phase 5):** Starrkörper-Dämpfung ×0.98/×0.9 je Tick (`FUN_000AFA30`: verschiebbare Objekte, Wippen, Minen rollen rund 17 % kürzer aus), Zielen und Drehen des Chicken Walker mit fest kodiertem Schritt 0.02 (`FUN_000445A0`, 20 % schneller), Skriptschleifen mit kurzen Waits (jede Schleife endet mit einem vom Compiler eingefügten `Wait(0)`: −17 % bis +11 % je Durchlauf, kumulativ) | Bewusst wie das Original-NTSC-Verhalten, das die Entwickler mit demselben Code und denselben Skripten betrieben haben (alle betroffenen Funktionen im Prototyp identisch, `timing_review.out` 5). Ein Teilausgleich auf PAL ergäbe eine Mischung, die es nie gab; die Patchstellen dafür sind dokumentiert (timing60.md 12.2). Gemessen: Fade 43–44 Ticks nach dem ersten Frontend-Frame (+6 Ticks im ersten Frame), 0,864 s → 0,737 s (Messtabelle unten); Dämpfung und Walker sind erst ab M8 messbar (Observablen 8.17b, 8.19) |
| B41 | 3.2 (Hooks), 4.16 | – | Zwei neue Probes: `probe:Game::FixedStep` auf `FUN_0002C890` (genau ein Aufruf pro festem Schritt, 0xA2DD3; zählt Schritte/s und das Histogramm Schritte pro `CApp::Frame`) und `probe:Game::Timer` auf `FUN_000D2B90` (Zeitgeber des Spiels). Hooks 161 → 163; Exit-Reporter „game timing“ (`game ticks: …`, `game clock summary: …`) | Messbarkeit der Observablen 8.2/8.3 aus timing60.md; Prolog-Prüfung per capstone (kein Sprung in die verschobenen Bytes) |
| B42 | 1.9 (QPC-Umleitung 1:1) | `QueryPerformanceCounter` liefert echte Zeit, „ein konsistentes Paar genügt“ | **Lockstep-Uhr für `Game::Frame`** (`[game] frame_clock = lockstep`, Standard; `realtime` = Original): Die drei Zeitproben von `Game::Frame` (Rücksprünge 0xA2C6F, 0xA2FA6, 0xA2FBB) bekommen eine virtuelle Zeit, die pro Frame um genau ticks(dt) = trunc(QPF·dt) wächst. Liegt die echte Zeit mehr als ¾ Schritt daneben, holt sie in ganzen Schritten auf bzw. hält an (Phase bleibt innerhalb ±12,5 ms). Alle anderen Aufrufer (Intro/Legal/Death-Zeiten, Streaming, 2,5-s-Grenze 0xD3167) sehen weiter echte QPC-Zeit | Ohne Interpolation zeigt jeder Frame genau einen Simulationsstand; mit echter Zeit liegt der Rest nach jeder Kappung auf der Schrittgrenze, und Abtastjitter erzeugt 0- und 2-Schritt-Frames (Doppelbild + übersprungener Tick). Gemessen mit dem M3b-Takt (je 3000 Frames, 60 Hz): `realtime` 26 Frames mit 0 und 31 mit ≥ 2 Schritten von 2985 (98,02 % genau ein Schritt), `lockstep` 0–1 mit 0 und 5–6 mit ≥ 2 Schritten (99,73–99,80 %; der Rest sind Aufhol-Frames nach dem Kontextwechsel und nach Ladehängern). Mit dem alten Takt waren es 9,8 % je Richtung |
| B43 | 6 M3 („Present mit VSync, optionaler 50-Hz-Limiter“), 7 R9 | 50-Hz-CPU-Takt nach Present(1) | **Bildsynchroner Takt** (`platform\frame_pacer.*`, `pacer_policy.*`, `pacer_display.*`): `[render] pacing = auto` (Standard) \| `vsync` \| `timer` \| `off`; `[render] frame_rate` folgt `[game] tick_rate` (der feste Standard 50 entfällt; ein abweichender Wert loggt GFX/warn). Auto: Refresh = k·60 (60/120/180/240 Hz, auch 59,94/119,88) → Swapchain mit Frame-Latency-Waitable (maximale Latenz 1) und Present(k); sonst Timer + Present(0, ALLOW_TEARING) mit VRR-Prüfung über die DXGI-Frame-Statistik, nach Ablehnung Present(1) mit einem Frame pro Refresh; im Hintergrund, minimiert, bei Display aus oder gesperrter Sitzung „free“ (Timer, nicht blockierendes Present). Refresh je Ausgabe aus `QueryDisplayConfig` (rational), neu abgefragt bei Monitor- oder Display-Wechsel. Neue Exit-Zeilen `present: mode/display/frame start intervals/swap chain/on screen/environment`, optional `[render] pacing_trace` (CSV) | Vorgabe ≥ 60 fps und gleichmäßige 2 Vblanks pro Frame bei 120 Hz. Gemessen (auto, 3 Läufe): 2969–2970 von 3000 Frames vblank-gekoppelt (die ersten 30–31 laufen „free“, weil das Fenster bis zum ersten Present unsichtbar und damit nicht im Vordergrund ist; Hysterese 30 Frames), davon 99,97–100 % genau 2 Refreshes, gemessener Refresh 120,001 Hz |
| B44 | rhi.h (M3: `DXGI_PRESENT_ALLOW_TEARING` bei jedem Sync-Intervall 0) | – | Tearing nur noch auf Anforderung (`PresentDesc::allowTearing`); neu `PresentDesc::skipSwapChain` (nur die persistenten Backbuffer rotieren, B31), `DeviceDesc::frameLatencyWaitable`, `Device::LastPresentTiming()` (Present-Zähler, Frame-Statistik) | Tearing gehört nur zum VRR-/Timer-Pfad; ein „free“-Frame ohne freien Slot darf die Swapchain nicht blockieren |
| B45 | 6 M3 Kriterium 3 („Bildrate ≥ 50“), B36, m2_check | Frame-Nummern und Schwellen bei 50 Hz | **Prüfskripte in Sekunden:** `m3_check` rechnet Szenen als Sekunden × Rate (Legal 3,0 s, Tech-Notice 8,0 s, Ladebild 13,6 s, Frontend 20,0 s → 180/480/816/1200 bei 60 Hz, 150/400/680/1000 bei 50 Hz). M3-3 verlangt Ø ≥ Rate − 0,5 Hz (59,5), Frame-Arbeit im 99. Perzentil ≤ eine Tick-Periode (16,7 ms) und ≥ 95 % der Frame-Starts innerhalb ±5 % der geplanten Periode; Takt-, Display-, Bildschirm- und Umgebungsstatistik wird je Lauf gemeldet. Neue Kriterien M3b-5/6 = `m2_check.check_timing` (T1: Patch-Tabelle der Rate, Rücklesen „ok“ mit fps/dt der Rate, CApp+0xE8 = 0, Tag „PAL“, nur `*-PAL.xmv`; T2: stetige Schrittrate ±0,2 %, ≥ 99 % der Frames mit genau einem Schritt bei Lockstep). `m2_check`: Intro muss ab 20 s × Rate verlassen sein (1000 Frames bei 50 Hz wie bisher, 1200 bei 60 Hz; Intro → Frontend nach 15,2–16,0 s), Timeout Frames/Rate + 90 s. Die 3000 Frames bleiben (Kriterium nennt `--frames=3000`); bei 60 Hz decken sie 50 s statt 60 s ab, darin der ganze Intro-Kontext und 35 s Frontend. Konfigurationen: `run\m3b\*` (60 Hz), `run\m3b\pal50` und `run\m3final\*` mit `[game] tick_rate = 50` + `[render] pacing = timer` | Die Vorgabe „≥ 60 fps“ ersetzt „≥ 50“. Ohne Umrechnung hätte z. B. „loading“ (Frame 680) bei 60 Hz 11,5 s statt 13,8 s getroffen, 0,3 s nach dem Splash-Ende. `pacing = timer` für 50 Hz, weil `auto` bei 50 Hz auf 120 Hz ohne VRR einen Frame pro Refresh zeigt und Frame-Nummern dann keine 1/50 s mehr sind |
| B46 | 6 (Ablauf des Intros), timing60.md 8 | Erwartung: alles in Sekunden gleich | **Ladephasen sind frame-gebunden und bei 60 Hz kürzer.** Das Ladebild (`Loadmenu.nif` → Intro-Film) dauert in beiden Raten 220 Frames: 4,400 s bei 50 Hz, 3,667 s bei 60 Hz; das Neuladen des Frontends nach Attract ≈ 222 Frames. Ebenso frame-gebunden: Film-Stubs (2 Frames je Film) und Kontextwechsel (8 Clear-Frames + 2). Intro → Frontend damit 15,98 s → 15,17 s, Attract-Zyklus 64,86 s → 64,07 s | Das Spiel pumpt seine Async-Ladewarteschlange einmal pro `Game::Frame` (`FUN_000D2370`), und die Host-E/A ist sofort fertig. Gegenprobe mit `[render] pacing = off` (ungebremst, 27 kHz im Legal-Bild): Ladebild 0,100 s, Legal 6,016 s, Tech-Notice 5,017 s, Frontend-Leerlauf 60,15 s – Echtzeit-Szenen unverändert, das Laden skaliert mit der Bildrate. Auf der Xbox begrenzt das DVD-Laufwerk; die xemu-Aufnahmen erlauben für das Ladebild nur 0–6 s |
| B47 | B43 (Phase-5-Review des Takts) | Hysterese 30 Frames für jeden Wechsel aus „free“; Timer-Pläne ohne Phasenbezug; VRR-Messung ab dem ersten Frame; langsame Frames aus den Present-Zeiten; gemessener Refresh aus d = 1-Paaren | **Review-Korrekturen am Takt:** (1) Ein Wechsel von einem Plan ohne Presents (minimiert, Display aus, gesperrt) zu einem mit Presents gilt sofort (`PlanHysteresis`), sonst blieb nach dem Wiederherstellen 30 Frames lang das letzte Bild stehen. (2) **Kadenzwächter** (`CadenceGuard`) für `timer`/`free` auf einer Anzeige mit k·Ziel: weichen 6 von 60 Bildschirmdauern von k Refreshes ab, verschiebt der Takt sein Raster um einen halben Refresh (weg vom DWM-Latch-Zeitpunkt). Eine Dauer, die zwar nicht k Refreshes zählt, aber zeitlich genau eine Zielperiode sichtbar war (das VRR-Panel folgt den Presents), gilt als pünktlich; die Verschiebung wechselt das Vorzeichen (±½ Refresh ergibt dieselbe Phase, die mittlere Rate driftet nicht); nach 4 Verschiebungen ohne sauberes Fenster dazwischen bleibt das Raster bis zum nächsten Planwechsel stehen. (3) VRR-Prüfung erst nach 2 s Aufwärmzeit, eine Ablehnung wird nach 10 s einmal wiederholt (je Display-Ereignis). (4) „frames longer than 1.5 periods“ zählt Frame-Start-Abstände; die Present-Abstände (mit Screenshot-Zeit) stehen in Klammern. (5) Bildschirmstatistik zählt Sprünge über d ≥ 2 Presents samt Abweichung von der Kadenz; der gemessene Refresh ist eine Regression von SyncQPCTime über SyncRefreshCount mit Segmentwechsel bei Zählerstillstand, Lücken > 1 s und Zählersprüngen. (6) `--set`-Werte werden wie INI-Werte getrimmt und entquotet; wirkungslose Kombinationen (`frame_rate = 0` mit `pacing = timer`/`vsync`, `vsync = 0` mit `pacing = vsync`) loggen GFX/warn; die Planzeile nennt „no Present“ auch für `timer`/`unthrottled` | Review M3b Befunde 5, 6, 8, 9, 10, 11, 13a. Gemessen: `pacing = timer` bei 60 Hz auf 120 Hz streute vorher je nach zufälliger Phase von 1 % bis 37 % Frames mit 1 oder 3 statt 2 Refreshes (Review-Läufe „1:406 2:1934 3:571“), mit Kadenzwächter 99,6–99,8 % genau 2 Refreshes nach 0–1 Verschiebungen (Läufe vom 2026-10-05, fester 120-Hz-Refresh); gemessener Refresh 120,001–120,002 Hz statt 119,66–119,98 Hz. Am 2026-10-06 lief das Fenster auf dem Messrechner mit VRR (Ursache der Änderung unbekannt): Jedes 60-Hz-Present blieb einen verlängerten Refresh von ≈ 16,7 ms sichtbar. Der Wächter in seiner ersten Fassung zählte das als „1 statt 2 Refreshes“, verschob 50-mal in 30 s und senkte `timer` auf 59,62 Hz; daher die Härtung oben (Messtabelle unten). Unit-Tests in `tests\pacer_tests.cpp` |
| B48 | 6 M3 Kriterium 3, B45 | M3-3 bewertet Rate, Arbeit und Frame-Starts, nicht die Kopplung | **Neues Prüfkriterium M3b-7** (`m3_check`): Fenster ≥ 95 % der Frames im Vordergrund (sonst „Umgebung ungültig“, Lauf wiederholen); bei `pacing = auto|vsync`, VSync an und Refresh = k·Ziel (k 1..4) zusätzlich ≥ 95 % der Frames im vblank-Plan und ≥ 99 % der vblank-gekoppelten Frames genau k Refreshes auf dem Bildschirm (`--min-foreground`, `--min-vblank-frames`, `--min-on-plan`) | Review M3b Befund 7: Ein Lauf, der komplett im „free“-Pfad lief (Fenster im Hintergrund), bestand M3-3 genauso |

**Messwerte der Prüfläufe** (Endstand Phase 5, 2026-10-06; FUT_WERROR=ON, alle drei Builds ohne Warnung; `fut_tests` 229/229 mit 22 470 Assertions in Debug, RelWithDebInfo und clang-cl; `gen_all.py --check` aktuell; alle Läufe ALL PASS):

| Prüfung | Build / Konfiguration | Ergebnis |
|---|---|---|
| `m3_check`, 3 Läufe `--fresh`, 60 Hz | MSVC Debug, `run\m3b\debug` | 3000 Frames in 49,98 s: **Ø 60,01 / 60,03 / 60,01 Hz**, langsamstes 1-s-Fenster 58,92–58,94 Hz, längster Frame 34–35 ms (Ladehänger), 0/1/0 Takt-Neustarts; Frame-Arbeit Ø 2,74–2,78 ms, 99 % ≤ 4,4 ms (tragfähig 360–365 Hz); 99,77–99,80 % der Frame-Starts im Takt; M3b-7: Vordergrund 99,9 %, 2969 von 3000 Frames vblank k = 2, davon 99,76 % genau 2 Refreshes; gemessener Refresh 120,001–120,002 Hz; Present → sichtbar Median 13,0 ms; 59,995–59,997 Schritte/s, 99,56–99,60 % der Frames genau ein Schritt; Debug-Layer 0/0/0; xemu: Legal 0,00, Tech-Notice 0,00, Ladebild 0,29–0,30, Frontend 0,01 (61 % statische Pixel) |
| `m3_check`, 1 Lauf | MSVC RelWithDebInfo (`debug_layer = on`), clang-cl Debug | Ø 60,01 Hz; Frame-Arbeit 99 % ≤ 3,4 ms (487 Hz) bzw. ≤ 4,2 ms (386 Hz); 99,90 % der vblank-gekoppelten Frames genau 2 Refreshes |
| `m3_check`, 50 Hz | MSVC Debug, `run\m3b\pal50` | Ø 50,01 Hz, Rücklesen fps 50 / dt 0,02, keine Patches, 50,000 Schritte/s; xemu-Vergleiche wie M3 (Ladebild 0,00) |
| `m1_check` | Debug 3 Läufe, RelWithDebInfo 2 Läufe, `run\m3b\m1` | 29 Selbsttests inkl. `GameTimingPatched`; Logs nach Maskierung gleich |
| `m2_check` | Debug + RelWithDebInfo `run\m3b\m2` (d3d11), Debug `run\m3b\m2-null`, Debug `run\m2fix\ctest-en` | inkl. T1/T2; 59,997–59,999 Schritte/s, 99,60–99,80 % genau ein Schritt; Intro verlassen bei #889–890 |
| `m2_check`, Dauerlauf 30 000 Frames (Phase 3) | MSVC Debug, `run\m3b\long` | 500 s, 8 Attract-Zyklen zu je 64,07–64,08 s; Ø 59,99 Hz (13 Frames über 1,5 Perioden durch Ladehänger beim Neuladen des Frontends, je ein verlorener Vblank), 99,92 % der Starts im Takt, 99,95 % der vblank-gekoppelten Frames genau 2 Refreshes; 60,010 Schritte/s, 99,89 % genau ein Schritt; Spielpool am Ende 5783 KiB (Spitze 7563 KiB) |

**Dauern 50 Hz gegen 60 Hz** (MSVC RelWithDebInfo, je 155 s, `run\m3b\dur50` mit `pacing = timer`, `run\m3b\dur60` mit `auto`; Auswertung `python tools\timing_compare.py --run run\m3b\dur50 --run run\m3b\dur60`; Dampfwolke aus `run\m3b\steam50/60` mit Screenshots jedes Frames):

| Observable (timing60.md 8) | 50 Hz | 60 Hz | Differenz | Mechanik / Erklärung |
|---|---|---|---|---|
| 8.1 Zeitbasis nach `Game::Init` | fps 50, dt 0,02 s, „PAL“ | fps 60, dt 1/60 s, „PAL“ | – | Patch (B38) |
| 8.2 Simulationsrate (stetig) | 50,000 Schritte/s | 60,000 Schritte/s | – | fester Schritt |
| 8.3 Schritte pro Frame | 99,87 % genau 1 | 99,88 % genau 1 | – | Lockstep (B42); Rest = Aufholen nach Kontextwechseln |
| 8.4 Legal-Bild (Logik / Bild) | 6,020 / 6,011 s (301 Frames) | 6,001 / 5,994 s (360/361 Frames) | −19 / −17 ms | QPC 6,0 s; Rest = Quantisierung auf einen Tick (20 bzw. 16,7 ms) – **gleich** |
| Logo-Filme fox/sci/uds (Stub) | 0,120 s (6 Frames) | 0,100 s (6 Frames) | −20 ms | frame-gebunden (XMV-Stub, M6) |
| 8.5 Tech-Notice `NI_Splash_PAL.nif` (Logik / Bild) | 5,060 / 5,068 s | 5,050 / 5,058 s | −10 ms | QPC max. 5,0 s + Laden – **gleich** |
| Ladebild `Loadmenu.nif` (Logik / Bild) | 4,400 / 4,388 s (220 Frames) | 3,667 / 3,654 s (220 Frames) | −734 ms (×0,833) | frame-gebunden (B46) |
| Intro-Film-Stub + Kontextwechsel | 0,200 s (10 Frames) | 0,167 s (10 Frames) | −33 ms | frame-gebunden |
| 8.6 Intro → Frontend (ab Prozessstart) | 15,983 s (`CApp::Frame` #780) | 15,170 s (#890) | −813 ms | Summe; 734 ms davon Ladebild |
| 8.7 Frontend-Fade (erster Frontend-Frame → Plateau ±3 %) | 0,864 s (43 Frames) | 0,737 s (44 Frames) | −127 ms (×0,85) | tick-gebunden (B40): 50 Ticks, davon 6 im ersten Frame (Kappung 6·dt nach den 10 Wechsel-Frames: zweiter Frame bei 14 % Helligkeit) |
| 8.8 Frontend-Leerlauf → Attract (1. Zyklus) | 60,220 s | 60,199 s | −21 ms | dt-Sekunden 60,0 s – **gleich** |
| 8.9 Attract → Frontend (Film-Stub) | 0,200 s | 0,167 s | −33 ms | frame-gebunden (8 Clear-Frames + 2) |
| 8.10 Attract-Zyklus (Frontend → Attract, aufeinanderfolgend) | 64,860 s | 64,066 s | −794 ms | 60 s + frame-gebundenes Neuladen (≈ 222 Frames: 4,44 s → 3,70 s) |
| 8.11 Dampfwolke | 1,000 s (50 Frames) | 1,004 s (60 Frames) | +4 ms | NIF-Zeit – **gleich** (Zeitstempel der Screenshots ±1 Frame) |
| 8.12 Filmnamen | `fox/sci/uds/intro/Attract-PAL.xmv` | dieselben | – | Tag „PAL“ unverändert |
| xemu (PAL 50 Hz, Aufnahmen alle 3 s, echte Filme) | Legal 3–9 s, Tech-Notice 3–9 s, Ladebild 0–6 s, Frontend bis Attract 57–63 s, Dampfwolke 1,0 s (`run\xemu_ref_steam`) | – | – | alle Messwerte beider Raten liegen in diesen Schranken |

8.13–8.19 (Eingabesperre, Tastenwiederholung, Entprellung, Skript-Ereignisse, Physik und Starrkörper-Dämpfung, Spielzeit-Uhr, Walker) brauchen Eingabe bzw. Ingame-Szenen und folgen in M4/M8.

**Takt-Modi** (Phase 3, 2026-10-05: Fenster komponiert, kein VRR; MSVC Debug, `run\m3b\pacing\<modus>`, je 3000 Frames mit `m3_check`, 120-Hz-Display, Fenster im Vordergrund):

| Modus | Plan | Ø Hz / langsamstes Fenster | Starts im Takt | Auf dem Bildschirm (Refreshes pro Frame) | Present → sichtbar Median / 99 % | `m3_check` |
|---|---|---|---|---|---|---|
| `auto` (Standard) | vblank k = 2 (2970) + free (30, Start) | 60,01 / 58,89 | 99,80 % | 2: 2988 von 2990, 99,97 % nach Plan | 21,3 / 24,9 ms | ALL PASS |
| `vsync` | vblank k = 2 (3000) | 59,99 / 58,93 | 99,93 % | 2: 2991 von 2993, 99,93 % nach Plan | 21,4 / 24,9 ms | ALL PASS |
| `timer` | Timer + Present(0) (3000) | 60,02 / 58,93 | 99,93 % | 2: 2958 von 2978 (1: 9, 4: 8, 5+: 3); ohne Phasenbezug streut das je nach zufälliger Startphase von 1 % bis 37 % Frames mit 1 oder 3 Refreshes (Review-Läufe, Befund 6), mit Kadenzwächter 99,6–99,8 % genau 2 (B47) | 7,3 / 11,6 ms | ALL PASS |
| `auto`, `vsync = 0` | Timer + Present(0, ALLOW_TEARING) | 60,02 / 58,96 | 99,97 % | 2: 2933 von 2956 (komponiertes Fenster: kein echtes Tearing) | 4,4 / 16,6 ms | ALL PASS |
| `off` | ungebremst | 10 380 (Legal-Bild) | – | 365 Presents ohne freien Slot übersprungen | – | nicht anwendbar: 3000 Frames sind nach 0,29 s im Legal-Bild vorbei; das Spiel tickt weiter mit 60,04 Schritten/s (2890 Frames ohne Schritt) |
| `auto`, Tick 50 | tearing-timer (61) → VRR abgelehnt („visible intervals follow the refresh grid“) → display-rate 120 Hz (2908) | 114,77 | 99,73 % (Periode 1/120 s) | 1: 2795 von 2885 | 13,1 / 16,6 ms | M3-1 scheitert wie erwartet (Frame-Nummern ≠ 1/50 s; deshalb `pacing = timer` für 50-Hz-Prüfungen) |
| `timer`, Tick 50 (`run\m3b\pal50`) | Timer (3000) | 50,01 / 49,11 | 100 % | 1: 848, 2: 839, 3: 772, 4: 437 (50 auf 120 Hz ohne VRR: unregelmäßig) | 8,5 / 17,1 ms | ALL PASS |
| `auto`, `frame_clock = realtime` | vblank k = 2 | 60,01 / 58,92 | 99,80 % | wie `auto` | – | ALL PASS; Frames mit 0 / ≥ 2 Schritten: 26 / 31 (B42) |

**Takt nach den Review-Korrekturen** (Phase 5, 2026-10-06: das Fenster lief jetzt mit VRR, **Vermutung:** Independent Flip auf einer Overlay-Ebene, Ursache der Änderung unbekannt; MSVC RelWithDebInfo, je 1800 Frames unter `run\m3b5\p5*`, `auto` aus den `m3_check`-Läufen):

| Lauf | Ergebnis |
|---|---|
| `auto` (Standard) | vblank k = 2 wie in Phase 3, jedes Bild genau 2 Refreshes; Present → sichtbar Median 13,0–14,0 ms statt 21,3 ms (kein DWM-Refresh mehr dazwischen) |
| `timer` 60 Hz, erste Fassung des Kadenzwächters | jedes Present 1 verlängerter Refresh (≈ 16,7 ms); der Wächter verschob 50-mal in 30 s → **59,62 Hz**, 97,22 % der Starts im Takt |
| `timer` 60 Hz, gehärteter Wächter (B47), 3 Läufe | 60,02–60,03 Hz, 99,83–100 % im Takt, 0–1 Verschiebungen, 98,3–99,0 % der bewerteten Dauern „vom Panel gefolgt“ |
| `auto`, Tick 50 | tearing-timer, „VRR confirmed: the display follows the presents (100 %)“ nach 2 s Aufwärmzeit, 50,03 Hz; damit ist T10 im Fenster positiv geprüft |
| `timer`, Tick 50 | 50,03 Hz, 1 Refresh je Frame (statt 1–4 in Phase 3) |
| `auto`, Minimieren/Wiederherstellen | `vblank -> free (no Present)` beim Minimieren, `free -> vblank k=2` im selben Frame wie das Wiederherstellen (vorher 30 Frames, 0,48 s eingefrorenes Bild); 780 übersprungene Presents = 780 minimierte Frames |
| `--set=render.pacing= timer`, `frame_rate = 0` + `timer`, `vsync = 0` + `pacing = vsync` | Plan `timer` (vorher still `auto`); je eine GFX-Warnung bei den wirkungslosen Kombinationen |

Offene Punkte nach M3b (Stand Phase 5):
- Auf dem Messrechner nicht getestet (pacing60.md T9/T10/T12): andere Bildwiederholraten (60, 59,94, 144, 165 Hz), zweiter Monitor, Hotplug, Display aus, gesperrte Sitzung, VRR und Tearing im randlosen Vollbild. Der VRR-Pfad im Fenster ist seit 2026-10-06 positiv geprüft (Tick 50, `auto`: „VRR confirmed“), weil der Messrechner seitdem im Fenster VRR zeigt. Die Logik dafür ist kopflos getestet (`tests\pacer_tests.cpp`).
- **59,94 Hz** (Review-Befund 14): `ClassifyRefresh` wählt bei 60000/1001 Hz `vblank k = 1`; der Port zeigt dann 59,94 fps, also knapp unter der Vorgabe. Die Lockstep-Uhr holt die Drift in Doppelschritten auf: ein Frame mit 2 Schritten alle 996 Frames (16,6 s), der erste nach 748 Frames (Simulation von `LockstepClock::Advance`, QPF 10 MHz). Alternativen aus pacing60.md 3.3 sind nicht umgesetzt: `tick_lock = display` (Spiel 0,1 % langsamer, ruckfrei) oder ein VRR-Versuch mit exakt 60,000 Hz bei Tearing-Unterstützung. Entscheidung nach einer Messung an einer 59,94-Hz-Anzeige; dasselbe gilt für 119,88 Hz (k = 2).
- **Vollbild und VRR** (Review-Befund 13b): Im randlosen Vollbild (Alt+Enter) sitzt die Swapchain weiter auf der 4:3-Kindansicht (2880×2160 auf 3840×2160). Independent Flip und damit VRR und Tearing setzen vermutlich eine Swapchain voraus, die den ganzen Monitor bedeckt (**Vermutung**; die VRR-Belege in pacing60.md stammen von einer Top-Level-Swapchain über den ganzen Monitor, und auch diese zeigt auf dem Messrechner derzeit nur Intervalle im Refresh-Raster). Abhilfe: im Vollbild die Swapchain auf das Rahmenfenster legen und die Pillarbox-Balken im Present-Pass zeichnen; Test T10 auf einem System mit nachweislich aktivem VRR wiederholen.
- **Latenz Present → sichtbar** (Review-Befund 12, Grundlage für M4): vblank k = 2 im komponierten Fenster Median 24,8–24,9 ms im Intro (Arbeit 0,28 ms), 21,2–21,4 ms im Frontend (Arbeit 3,8 ms, Debug), 22,3 ms RelWithDebInfo, 24,9 ms im randlosen Vollbild; `frame_rate = 120` mit k = 1: 16,5 ms; `timer` 1,7–9,1 ms, `free` 4,6 ms. Die Frame-Arbeit beginnt direkt nach dem Signal des Waitables (Wartezeit im Median 12,8 ms), das Present wartet eine volle Periode, DWM fügt einen Refresh hinzu. In den Phase-5-Prüfläufen lag der Median bei 13,0–14,0 ms (99 %: 16,6 ms), weil das Fenster nun mit VRR im Independent Flip lief (**Vermutung**, siehe Tabelle „Takt nach den Review-Korrekturen“). Ein just-in-time-Framestart (pacing60.md 4.9, Option `latency = low`: nach dem Signal bis vblank − (Arbeit p95 + Abstand) warten) ist nicht umgesetzt und mit der Eingabe in M4 zu bewerten.
- Timer-Pfade ohne ganzzahliges Verhältnis (50 Hz auf 120 Hz, `run\m3b\pal50`) zeigen weiter unregelmäßig 1–4 Refreshes pro Frame; der Kadenzwächter (B47) greift nur bei Refresh = k·Ziel. Eine echte Phasenbindung an den vblank (SyncQPCTime der Frame-Statistik, Deadline in der Mitte zwischen zwei Latch-Zeitpunkten) ist nicht umgesetzt.
- Nicht umgesetzt aus pacing60.md: Kadenz-Modus (abwechselnde Sync-Intervalle), doppelte Presents für k > 4, Neuplanung aus der gemessenen Refresh-Rate (der Schätzer ist seit B47 dafür brauchbar: 120,001–120,002 Hz), `tick_lock = display`.
- Tick-gebundene Stellen (B40) laufen wie in der NTSC-Fassung, darunter die spielrelevante Starrkörper-Dämpfung und der Walker; wer das PAL-Verhalten braucht, setzt `[game] tick_rate = 50`. Messung erst ab M4/M8 (timing60.md 8.13–8.19).
- Synchrones Laden im Gast (Kontextwechsel, Neuladen des Frontends) kostet einzelne Vblanks: Frame-Arbeit bis 20,5 ms (Debug), im Dauerlauf 13 Frames über 1,5 Perioden in 500 s, Ø 59,99 Hz statt 60,00 Hz. Das Spiel holt die Zeit über seinen festen Schritt nach (Kappung 6·dt); die Bildrate selbst bleibt im Takt.
- Nach dem Start laufen die ersten 30–31 Frames „free“ (Fenster bis zum ersten Present unsichtbar, Hysterese 30 Frames), danach vblank k = 2.
- Ältere Konfigurationen unter `run\` (M2-/M3-Agentenläufe) setzen noch `[render] frame_rate = 50` und loggen mit dem neuen Standard eine GFX-Warnung; gepflegt sind `run\m3b\*`, `run\m3final\*` und `run\m2fix\ctest*` (ctest). Die INIs unter `run\m3final\*` setzen jetzt `tick_rate = 50` + `pacing = timer`, ihre M3-Belegprotokolle stammen aber noch vom alten Build; ein neuer `m3_check`-Lauf dort überschreibt die M3-Belege.
- `tests\platform_tests.cpp` enthält noch den manuellen (übersprungenen) Test „present loop at 50 Hz“ auf dem alten `PaceFrame(50)`.

### 8.5 Abweichungen in M3c: Konfigurationsschema, Ablageort und Werkzeug-Modi (Host-CLI)

Stand: M3c Phase 1 (Host-CLI/Schema-Agent), 2026-10-06. Anlass ist der Qt-Launcher (`docs\ROADMAP.md`, M3c): Er baut seine Felder aus dem Schema von `futurama.exe`, prüft das ISO und fragt die Grafikkarte ab, ohne die Logik selbst nachzubauen. Schnittstelle: `src\INTERFACES.md` 4.21. Die Zeilen heißen K1–K6, damit sie nicht mit den Renderer-Zeilen derselben Phase kollidieren.

| # | Abschnitt | Design | Umsetzung in M3c | Begründung / Beleg |
|---|---|---|---|---|
| K1 | 5.3, A13 (Konfiguration) | Bekannte Schlüssel in `kKnownKeys` (`core\config.cpp`) | **Eine Schema-Tabelle** `core\config_schema.cpp`: je Schlüssel Typ, Standardwert, Auswahlwerte mit deutscher und englischer Beschriftung, Wertebereich, Name und Hilfetext für Spieler, Reiter, Reihenfolge und Bedienelement im Launcher, Kennzeichen „erweitert“ und „Entwickler“. Die Prüfung beim Laden, `IsKnownConfigKey`, `ConfigKeyChoices` und `--print-config-schema` lesen dieselbe Tabelle; `kKnownKeys` entfällt | ROADMAP M3c „Keine doppelte Pflege der Einstellungen“. Ein Unit-Test durchsucht `src\host` nach gelesenen Schlüsseln ohne Tabellenzeile und nach Zeilen ohne Leser (`tests\schema_tests.cpp`) |
| K2 | A13 | Typen String/Bool/U32/List/Path/Choice | Neue Typen **float** (`render_scale`, nur `.` als Dezimalzeichen) und **resolution** (`auto` oder `<Breite>x<Höhe>`, je 160–16384). Wertebereiche werden geprüft, wenn das Modul sie nicht selbst begrenzt: `render_scale` 1,0–2,0 und `[kernel] min_guest_stack` 0x10000–0x4000000 (bisher Stufenfehler mit Exitcode 2, jetzt Konfigurationsfehler mit Exitcode 1). `[log] level` ist ein Auswahlschlüssel (`warning` bleibt als verborgener Alias). Bereiche, die das Modul begrenzt (`window_scale`, `fiber_stack`, `timeout`, `frame_rate`), sind nur Vorgaben für den Launcher | Fehlerhafte Werte fallen vor dem Start auf, nicht erst in einer Stufe (wie M2-Review-Befund 10). Die Fehlermeldungen der bisherigen Typen sind unverändert |
| K3 | 1.3, 4.1 | INI neben `futurama.exe` oder `--config`; Portable-Modus `.\userdata` „in der Konfiguration“ | **Ablageort der Spieler-Konfiguration:** Ohne `--config` liest `futurama.exe` `%LOCALAPPDATA%\FuturamaPC\futurama.ini` (userdata `%LOCALAPPDATA%\FuturamaPC`). Eine Datei `portable.txt` neben der exe schaltet den Portable-Modus ein: `<exe-Ordner>\futurama.ini`, userdata = `<exe-Ordner>` (Konfiguration und Benutzerdaten nebeneinander wie im Standardmodus; so liest auch der Launcher den gemeinsamen Vertrag, 4.1 sah `.\userdata` vor). `--config` hat immer Vorrang, der userdata-Standard folgt dann derselben Regel. Eine alte `futurama.ini` neben der exe ohne `portable.txt` wird ignoriert und im Log gewarnt. Logzeile `config <Datei> (explicit\|default\|portable)` | Gemeinsamer Vertrag M3c Punkt 4: Der Launcher schreibt die Konfiguration in einen beschreibbaren Ordner, Programmordner können schreibgeschützt sein. Alle Prüfskripte und ctest-Läufe übergeben `--config` und sind nicht betroffen (`m1_check` 2 Läufe mit `run\m3c-cli\m1` und `m2_check` mit `run\m3c-cli\m2-null`: ALL PASS) |
| K4 | 1.3, 1.10 | `futurama.exe` startet immer das Spiel | **Werkzeug-Modi vor Stufe 1** (`core::cli::RunToolIfRequested` am Anfang von `RunHost`): `--print-config-schema`, `--check-iso` mit `--result`, `--query-gpu`, `--print-paths`. Sie laufen nach den Prüfungen des Starters (TLS-Slots, Platzhalter bei 0x10000) und `LoadLibrary`, aber ohne Konfiguration, Log, Absturzbehandlung und XBE. Ausgabe ist JSON in eine Datei (`-` = stdout), über eine `.tmp`-Datei mit Umbenennen geschrieben; Exitcode 0, 1 (Aufruffehler) oder 2 (ISO nicht unterstützt); nie ein Meldungsfenster | Der Launcher ist ein 64-Bit-Programm und kann den 32-Bit-Host nicht laden. Ein eigener Prozess je Abfrage braucht keine zweite Implementierung von XDVDFS, SHA-1 und Anzeigeabfrage. Gemessen (Debug): Schema 27 ms, ISO-Prüfung 40 ms, GPU-Abfrage 0,27 s |
| K5 | 4.2 Punkt 4 | XDVDFS-Leser nur für den Spielstart | `--check-iso` nutzt denselben Leser (XISO, Redump, XGD2/XGD3) und erkennt die Fassung an der SHA-1 der `default.xbe`: Retail EU `58f76de5…` wird unterstützt, der NTSC-Prototyp vom 17.06.2003 `f443d255…` wird erkannt und abgelehnt, sonst entscheidet der Titel im XBE-Zertifikat zwischen „andere Futurama-Fassung“ (z. B. US) und „kein Futurama“. Spielertexte auf Deutsch und Englisch liegen bei | ROADMAP M3c „Erster Start“. Geprüft am Retail-ISO (XISO-Layout, 1 880 227 840 Byte, Titel „Futurama“, Title-ID 46490002), am Prototyp-`.bin` (MODE1/2048, ebenfalls XISO-Layout) sowie an synthetischen XISO- und Redump-Abbildern (sparse) mit echten und gefälschten XBEs |
| K6 | 2.2, M3 | – | `--query-gpu`: DXGI-Adapter; der dedizierte Speicher kommt aus `D3DKMTQueryAdapterInfo`, weil `DedicatedVideoMemory` als `SIZE_T` im 32-Bit-Prozess bei 4 GB sättigt. Ausgaben mit exakter Bildwiederholrate aus `QueryDisplayConfig` (dieselbe Funktion wie der Takt), Anzeigemodi, wirksame DPI. MSAA-Stufen je Adapter für die Formate des Renderers (B8G8R8A8 und R8G8B8A8 mit Multisample-Resolve, D24S8), Tearing-Unterstützung als VRR-Hinweis | Der Launcher bietet nur Stufen an, die die Karte kann. Messrechner: RTX 5090 mit 32 187 MB, MSAA 1/2/4/8, LG TV 3840×2160 bei 120/1 Hz, 27 Modi, 216 dpi; dazu AMD Radeon, RTX 3050 und Microsoft Basic Render Driver |

Offene Punkte nach M3c Phase 1 (Host-CLI):
- **Monitorwahl:** Die ROADMAP nennt sie, das Schema kennt das Bedienelement `monitor`, aber noch kein Modul liest einen Schlüssel dafür. Vorschlag: `[render] monitor = primary | \\.\DISPLAYn` (Gerätename aus `--query-gpu`), umgesetzt im Fenster-Code (`platform\window.cpp`); dazu genügt eine Tabellenzeile. **Erledigt in der Integration (8.7 I1).**
- **Bildrate im Launcher:** `[render] frame_rate` ist als „erweitert“ markiert (Standard = Logik-Takt). Die Auswahl „60 / 120 (interpoliert) / Monitor (max. 120)“ kommt mit M3d als eigener Schlüssel.
- Launcher-Agent: JSON-Erweiterungen (`developer`, `dynamic_default`, `hidden_choices`, `placeholder`, `config`, `reason_code`/`message`) sind optional. Schlüssel mit `dynamic_default` sollte der Launcher nur schreiben, wenn der Spieler sie ändert.

### 8.6 Abweichungen in M3c: Renderauflösung und Kantenglättung (Renderer)

Stand: M3c Phase 1 (Renderer-Agent), 2026-10-06. Anlass: `docs\ROADMAP.md`, M3c (Auflösung und Anti-Aliasing; Vorgabe des Nutzers: MSAA, kein FXAA, kein TAA). Vertrag: `src\host\gfx\GFX_CONTRACT.md` Abschnitt 12, Schnittstelle `src\INTERFACES.md` 4.22, Prüfung `tools\m3c_check.py`. Die Zeilen heißen G1–G10.

| # | Abschnitt | Design | Umsetzung in M3c | Begründung / Beleg |
|---|---|---|---|---|
| G1 | 2.2, 6 (M10) | Höhere interne Auflösung erst mit dem Vulkan-/D3D12-Backend (M10) | **Schon auf D3D11:** `[render] resolution = auto \| <B>x<H>` (größtes 4:3-Rechteck in der Ansicht bzw. in B×H) und `render_scale` 1,0–2,0. Der Gast sieht weiter 640×480 (Slots 21/22, `ni_extract`, Kamera, Draw-Trace); skaliert werden nur D3D-Viewport und Clear-Rechtecke | Die RHI trägt das ohne Umbau. Klassisch (640×480, Skala 1, MSAA aus, anisotrop aus) ist bitgleich zu M3: Legal, Tech-Notice und Frontend-Bild 1200 gleich dem Build vor M3c, xemu 0,00 / 0,00 / 0,30 / 0,02 |
| G2 | GFX_CONTRACT 9 (B33) | Vertex-Raster 1/16 Pixel | Das Raster bleibt **1/16 Gast-Pixel** (bei 2880×2160 also 0,28 interne Pixel) | Die Eckpunkte bleiben, wo der Original-Rasterizer sie hatte; das hochaufgelöste Bild ist eine überabgetastete Fassung derselben Geometrie und passt verkleinert zu xemu (Frontend 0,98 auf den statischen Pixeln, Grenze 2,0) |
| G3 | GFX_CONTRACT 9 (B31) | Zwei persistente Backbuffer, einfach abgetastet | Mit MSAA sind die **persistenten Backbuffer selbst multisampled** (Tiefenpuffer ebenso); Present löst den aktuellen auf (`ResolveSubresource`) | Ein Frame ohne Draw arbeitet so auf den Samples von vor zwei Presents weiter wie der Xbox-Flip; ein einzelner MSAA-Puffer hätte nur den letzten Frame |
| G4 | ROADMAP M3c | Alpha-to-Coverage für Materialien mit Alpha-Test | Nur bei Alpha-Test **ohne** Blending und nur für Schwellen-Vergleiche (>, >=, <, <=, nicht immer/nie erfüllt); der Shader ersetzt `discard` durch die geschärfte Abdeckung `(alpha − Schwelle) / fwidth(alpha) + 0,5`, Schwelle = Umschaltpunkt des 8-Bit-Tests | Geblendete Ausschnitte haben schon eine weiche Kante. Die 50-%-Linie liegt genau auf dem Schnitt des Alpha-Tests (Unit-Test: gleiche Spalte), die Kante bleibt ein Pixel breit. Frontend: 397–401 Draws mit Alpha-to-Coverage |
| G5 | – | – | **Neu: „anti-aliased nearest“** für Texturen mit Punktfilter, sobald intern größer als 640×480 gerendert wird (alle Intro-Bilder): bilinearer Sampler, Koordinaten auf der Texelmitte außer im letzten Pixel vor einer Texelgrenze | Bei Faktor 4,5 (2880×2160) gäbe reines Nearest abwechselnd 4 und 5 Pixel breite Texel. Legal-Bild verkleinert gegen xemu: 1,28 → 0,73; der Rest liegt am halben Pixelraster von 4,5. Klassisch unberührt |
| G6 | ROADMAP M3c | Supersampling „danach heruntergerechnet“ | Exakter Box-Filter bei ganzzahligem Verhältnis (Skala 2), Mitchell-Netravali (B = C = 1/3, auf das Verhältnis gestreckt) sonst; derselbe Filter verkleinert auch, wenn das Bild größer als das Fenster ist (statt bilinear) | Unit-Tests: Box auf ±½ Stufe genau, Mitchell flach auf Flächen, monoton an Kanten. **Nach dem Review ersetzt durch Catmull-Rom mit Anti-Ringing (8.8 R1)** |
| G7 | Vertrag Punkt 5 | Neue Schlüssel resolution, render_scale, msaa, alpha_to_coverage, anisotropy | Zusätzlich **`[render] screenshot_size = output \| internal`** (Zeile im Schema); Screenshots in Ausgabe- oder Intern-Größe; das PNG schreibt ein Hilfsthread (2880×2160 kostet über 100 ms) | Die Aufnahme bleibt auf dem Renderthread vor Present; der Exit-Reporter „screenshots“ wartet auf offene Dateien |
| G8 | – | Standardwerte = M3-Verhalten | Standard **auto / 1,0 / MSAA 4 / A2C an / anisotrop 16** (gleich den Schema-Zeilen) | Spielerfreundlicher Standard („Standard“-Preset). Prüfkonfigurationen, die xemu vergleichen (`m3_check`), müssen die klassischen Werte setzen; `m3c_check` erzeugt sie selbst |
| G9 | ROADMAP M3c „Prüfung“ | Render-to-Texture mit MSAA richtig auflösen | **Entfällt vorerst:** Render to Texture (Slot 15 mit Textur) ist weiter P2 (gezählte No-ops) und behält die Texturgröße (Skala 1). Linien/Punkte (Slots 52/53, Wireframe) sind P2 und 1 internes Pixel breit | Muss mit P2 nachgezogen werden (Ziel multisampled anlegen, vor dem Sampeln auflösen; Linienbreite mit der Skala) |
| G10 | GFX_CONTRACT 8 (M3b-Takt) | Übersprungenes Present (minimiert, Anzeige aus) rotiert nur die Backbuffer | Present ruft dann zusätzlich `Flush` auf; neu ist außerdem die GPU-Zeitmessung je Frame (Zeitstempel vom ersten Clear/Draw bis zum Ende des Present-Passes, Zeile `d3d11 gpu frame time`) | Ohne Flush wartete die GPU-Arbeit auf das nächste implizite Flush des Treibers: Frame-Arbeit 99 % 15–22 ms (Build vor M3c: 55 ms) statt ≤ 6,2 ms; die Zeitstempel-Abfragen liefen in die Debug-Layer-Warnung #408 |

Offene Punkte nach M3c Phase 1 (Renderer):
- **Nur mit ausgeschalteter Anzeige gemessen:** Bei allen Läufen war die Anzeige des Messrechners aus. Der Takt überspringt dann die Swapchain, der Present-Pass lief im Spiel also nicht (nur im Unit-Test mit Fenster). `m3c_check` (Debug, Debug-Layer an): alle Varianten ALL PASS, 59,93–60,03 Hz, Frame-Arbeit 99 % ≤ 6,2 ms, GPU-Spanne Ø 0,35 ms (klassisch) bis 1,57 ms (5760×4320, 8×). Mit eingeschalteter Anzeige wiederholen (Vordergrund, vblank-Kopplung, Present-Pass in jedem Frame). **Erledigt in der Integration (8.7 I7).**
- **Gefunden und behoben (G10):** Überspringt Present die Swapchain (minimiert, Anzeige aus, kopflos), hat nichts mehr den Befehlspuffer abgeschickt; die GPU-Arbeit wartete auf das nächste implizite Flush des Treibers. Folge: Frame-Arbeit 99 % 15–22 ms in jeder Variante, 55 ms mit dem unveränderten Build vor M3c. Present ruft jetzt in diesem Fall `Flush`; danach 99 % ≤ 6,2 ms.
- **Vollbild:** Die Ansicht bleibt im randlosen Vollbild 4:3 (offener Punkt aus M3b: für Independent Flip/VRR müsste die Swapchain den ganzen Monitor bedecken; der Present-Pass kann die Seitenbalken bereits zeichnen).

### 8.7 Abweichungen in M3c: Integration (Launcher, Monitorwahl, Paket, Messung mit Anzeige)

Stand: M3c Phase 2 (Integration), 2026-10-06. Die drei Teilergebnisse arbeiten zusammen: Der Launcher liest das echte Schema, die echte GPU-Abfrage und die ISO-Prüfung von `futurama.exe` (8.5), das Spiel rendert mit den Schlüsseln des Renderers (8.6), und das Paket `dist\FuturamaPC\` startet das Spiel über die Automatik des Launchers. Schnittstellen: `src\INTERFACES.md` 4.21–4.23. Die Zeilen heißen I1–I8.

| # | Abschnitt | Stand nach Phase 1 | Umsetzung in der Integration | Begründung / Beleg |
|---|---|---|---|---|
| I1 | ROADMAP M3c „Monitorwahl“; offene Punkte in 8.5 und 8.6 | Kein Schlüssel, das Fenster öffnet immer auf dem Hauptbildschirm. Der Launcher kannte das Bedienelement `monitor` schon | **`[render] monitor = primary \| <GDI-Gerätename>`** (z. B. `\\.\DISPLAY2`, wie `--query-gpu` ihn unter `outputs[].device` liefert). `platform::SelectMonitor` sucht den Monitor über `EnumDisplayMonitors`; Fenster, Arbeitsbereich und DPI (`GetDpiForMonitor` statt `GetDpiForSystem`) kommen vom gewählten Monitor. Ein nicht angeschlossener Name fällt mit einer Core-Warnung auf den Hauptbildschirm zurück. Die Zeile `window: …` endet mit `monitor \\.\DISPLAY1 (primary)`. Schemazeile: Typ string mit Bedienelement monitor (Grafik-Reiter, Reihenfolge 5); der Launcher zeigt „Automatisch (Hauptbildschirm: LG TV SSCR2)“ und jeden Ausgang mit Größe und Bildwiederholrate | Unit-Test `platform: [render] monitor selects a monitor by its GDI device name` (jeder angeschlossene Monitor über seinen Namen, Groß-/Kleinschreibung, Leerzeichen, fremde Namen). Der Messrechner hat nur einen Monitor: geprüft mit `\\.\DISPLAY1` und mit einem nicht angeschlossenen Namen (Warnung, Hauptbildschirm). Ein zweiter Monitor ist hier nicht prüfbar |
| I2 | 8.5 K1 (Texte des Schemas) | Spielsprache hieß „Sprache“ (im Launcher neben der Oberflächensprache „Sprache:“). Die ISO-Meldungen des Hosts sagten „Retail-Fassung“, der Launcher „Verkaufsversion Europa“. Die Hilfe zur Fenstergröße nannte „2 = 1280×960“ | „Spielsprache“ / „Game language“; einheitlich „europäische Verkaufsversion“ in `--check-iso` und im Launcher; die Hilfe zur Fenstergröße nennt die Windows-Skalierung (bei 225 % ergibt 2× ein Fenster von 2880×2160 Pixeln, das auf den Bildschirm verkleinert wird) | Nur Texte. Bei 225 % ist das Fenster physisch 2,25-mal so groß, und davon hängt die Renderauflösung „auto“ im Fenster ab (2631×1973 auf dem Messrechner) |
| I3 | Launcher, Reiter „Grafik“ | „Automatisch (nativ, 3840 × 2160)“; die Auflösungsliste enthielt 16:9-Modi des Monitors | „Automatisch“ nennt, was das Spiel rendert: „Vollbild: 2880 × 2160“ (größtes 4:3-Rechteck des Monitors), „Fenster: W × H“ oder, wenn das Fenster verkleinert wird, „Fenstergröße, bis 2880 × 2160“. Die Liste bietet nur 4:3-Größen bis zur Bildschirmhöhe; freie Eingabe bleibt möglich. Die Fenstergröße zeigt „2×“ und die Pixelgröße bei der Skalierung des Monitors (`dpi` aus `--query-gpu`). Alpha-to-Coverage ist ohne MSAA ausgegraut („Wirkt nur mit Kantenglättung (MSAA)“). Alle Beschriftungen eines Reiters stehen in einer Spalte, auch die von Voreinstellung, Bildrate und Ordnerliste. Die Bildrate zeigt den Logik-Takt (50 oder 60 fps) statt fest „60 fps“ | Der Launcher zeigte eine Auflösung an, die das Spiel nie benutzt: Das Bild ist 4:3 mit Seitenbalken (Vertrag Punkt 5). UI-Tests `displayHelpers` und `dependentKeysAndIsoStatus` |
| I4 | Launcher, Reiter „Spiel“ | Nur Spielsprache und Logik-Takt; das ISO stand nur im Reiter „Pfade“ ohne Prüfergebnis | Kasten „Spielabbild“ ganz oben: Ergebnis von `--check-iso` für das eingestellte ISO („✓ Futurama, europäische Verkaufsversion – wird unterstützt“, Format, Größe, Pfad), im Hintergrund bei jeder Änderung neu geprüft, mit „Ändern…“. Der Assistent nutzt dieselben Texte. Zahlen und Größen folgen der Oberflächensprache („1,75 GB“ / „1.75 GB“, Render-Skala „1,00“ / „1.00“) | Ein Spieler sieht beim Öffnen, ob sein Abbild passt. Das Häkchen ist U+2713: U+2714 erschien mit manchen Schriften als farbiges Emoji |
| I5 | Launcher, INI | Neue Schlüssel wurden an der Spalte des Nachbarn ausgerichtet; in vom Launcher angelegten Abschnitten entstand dadurch „msaa         = 8“ unter „render_scale = 1.5“ | Ausgerichtet wird nur, wenn der Abschnitt schon ausgerichtet ist (eine Zeile mit zwei oder mehr Leerzeichen vor `=`), und dann an der Spalte des Abschnitts statt an einem überlangen Nachbarn; sonst `key = value` | Test `setValueAppendsAlignedKey` erweitert. Kommentare, Reihenfolge und Schreibweise bleiben wie bisher erhalten |
| I6 | 8.6 G8 (Standardwerte), Prüfkonfigurationen | Ohne die neuen Schlüssel rendert ein Lauf seit Phase 1 in Qualität (auto, MSAA 4×, anisotrop 16×) und `m3_check` sähe 2631×1973-Screenshots | Integrationskonfigurationen `run\m3c-integ\{m1,m2,m2-null,m3-debug,m3-rel,m3-clang}` setzen die klassischen Werte (`resolution = 640x480`, `render_scale = 1.0`, `msaa = off`, `anisotropy = off`); `m3-rel` zusätzlich `debug_layer = on` (M3-2 verlangt die Debug-Schicht auch im RelWithDebInfo-Build). `m3c_check` mit `run\m3c-integ\m3c-debug` bzw. `m3c-rel` | Die alten Konfigurationen `run\m3b\*` und `run\m3final\*` sind unverändert; für einen M3-Regressionslauf dort müssen die klassischen Werte ergänzt werden |
| I7 | 8.6 offener Punkt „nur mit ausgeschalteter Anzeige gemessen“ | Phase 1 lief nur mit ausgeschalteter Anzeige; der Present-Pass lief im Spiel nie | Während der Integration ging die Anzeige an: Die Endprüfungen mit D3D11 (`m3_check`, `m3c_check`) und die Läufe aus dem Paket liefen mit eingeschalteter Anzeige, die Bilder gingen an die Swapchain. Bei den Paket-Läufen war das Fenster im Vordergrund (99,8–100 % der Bilder); beim ersten Durchgang der Endprüfungen blieb es im Hintergrund, weil Windows einem Hintergrundprozess das Holen in den Vordergrund verwehrt, solange am Rechner gearbeitet wird (Vordergrundsperre); daran scheiterte nur M3b-7. Die Wiederholung mit dem Fenster vorn bestand auch M3b-7. Bildschirmfotos des Desktops während der Läufe (Vollbild 3840×2160): das 4:3-Bild liegt genau bei x = 480…3359, die Seitenbalken sind schwarz, 2D-Elemente und Schrift stehen an derselben Stelle wie in „Klassisch“ und sind scharf | Ergebnisse unten. Ein Lauf mit kurz abgemeldetem Monitor (Ereignis `WinDisc`, Fernseher wechselt den Modus) hatte ein Einzelbild von 1,3 s; das ist eine Störung der Umgebung, kein Fehler im Takt |
| I8 | Paket | – | `python tools\package.py --game-build build\m3c-integ-rel --launcher-build build\m3c-integ-launcher-rel` → `dist\FuturamaPC\` (21 Dateien, 29,6 MB; das Spiel braucht keine MSVC-Laufzeit, der Launcher bringt die x64-Laufzeit mit). Start über `FuturamaLauncher.exe --config=… --set … --save --launch --wait --result=…` (`run\m3c-integ\dist\run_dist.py`) | Exitcode 0 in allen vier Varianten, Ergebnis „Spiel normal beendet“ im Ergebnis-JSON |

Ergebnisse der Integration (2026-10-06, Anzeige an):

| Prüfung | Build / Konfiguration | Ergebnis |
|---|---|---|
| Bauen | MSVC Debug `build\m3c-integ`, RelWithDebInfo `build\m3c-integ-rel`, clang-cl Debug `build\m3c-integ-clang`, alle mit `FUT_WERROR=ON`; Launcher Debug `build\m3c-integ-launcher` und Release `build\m3c-integ-launcher-rel` mit `LAUNCHER_WERROR=ON` | ohne Warnungen |
| `fut_tests` | alle drei Spiel-Builds | 257/257 (ein übersprungener Handtest) |
| Launcher-ctest | Debug und Release | 3/3 (`launcher_tests`, `launcher_ui_tests` mit den neuen Fällen, `launcher_screenshots`) |
| `m1_check` | Debug 3 Läufe, RelWithDebInfo 2 Läufe, `run\m3c-integ\m1` | ALL PASS |
| `m2_check` | Debug `run\m3c-integ\m2` (d3d11, klassisch) und `m2-null` (Null-Renderer) | ALL PASS; 59,998–60,000 Schritte/s, 99,60–99,80 % der Frames genau ein Schritt |
| `m3_check` (klassisch) | Debug 3 Läufe, RelWithDebInfo 1 Lauf (Debug-Schicht an), clang-cl 1 Lauf | Erster Durchgang: alle Kriterien außer M3b-7 bestanden, das Fenster blieb im Hintergrund (Vordergrundsperre, siehe I7). Wiederholung mit dem Fenster vorn (Debug, RelWithDebInfo, clang-cl je 1 Lauf): **ALL PASS**, Vordergrund 99,9–100 %, 99,0 % der Bilder im vblank-Plan, davon 99,76–99,90 % genau 2 Refreshes (120 Hz), 60,01 Hz, Frame-Arbeit 99 % ≤ 4,4 ms (RelWithDebInfo 3,4 ms), Present → sichtbar Median 12,8–13,8 ms; xemu Legal 0,00, Tech-Notice 0,00, Ladebild 0,29–0,30, Frontend 0,01; 0 Debug-Layer-Meldungen |
| `m3c_check` | Debug `run\m3c-integ\m3c-debug`, RelWithDebInfo `m3c-rel` (Fenster im Hintergrund); Debug noch einmal mit dem Fenster vorn | **ALL PASS** in allen drei Durchgängen: 59,94–60,03 Hz, Frame-Arbeit 99 % ≤ 6,3 ms (Debug) bzw. ≤ 4,5 ms (RelWithDebInfo); X1 (verkleinert gegen xemu) Legal 0,73, Tech-Notice 0,18, Ladebild 0,60–0,61, Frontend 0,99; X2 Kantenfehler ohne AA 3,07–3,09, mit MSAA 4× 1,36 (Verhältnis 0,44, Grenze 0,7), harte Stufen 1,5–1,6 % → 0,0 %; GPU-Spanne Ø 1,1–2,0 ms |
| Paket-Läufe über den Launcher | `dist\FuturamaPC`, RelWithDebInfo, je 1300 Frames: Klassisch (Fenster, 640×480), Standard (Vollbild auto 2880×2160, MSAA 4×, A2C, anisotrop 16×), Maximum (Vollbild, Skala 1,5 → 4320×3240, MSAA 8×, Mitchell-Netravali), Fenster (auto 2631×1973, MSAA 4×) | Exitcode 0; 60,02–60,05 Hz; jedes Bild an die Swapchain (bis auf 2 ohne freien Puffer im Fenstermodus); Vordergrund 99,8–100 %; GPU-Spanne Ø 0,20 / 0,35 / 1,04 / 0,32 ms. Verkleinert auf 640×480 weichen Standard/Maximum von Klassisch ab um: Legal 0,70/0,92, Tech-Notice 0,17/0,21, Ladebild 0,38/0,43, Frontend 2,38/2,39 (animierte Szene); kein Pixel über 48 Stufen bei Legal und Tech-Notice. Bildschirmfoto im Vollbild: Bild bei x = 480…3359, Seitenbalken schwarz |
| `[render] monitor` über den Launcher | `--set render.monitor=\\.\DISPLAY1 --save`, danach `futurama.exe` aus dem Paket | Launcher schreibt `monitor = \\.\DISPLAY1`, Spiel meldet `monitor \\.\DISPLAY1 (primary)`; ein ungültiger Wert (`render.msaa=3`) wird vom Launcher mit Exitcode 65 abgelehnt |

Offene Punkte nach M3c:
- **Zweiter Monitor:** `[render] monitor` ist nur mit dem einen Monitor des Messrechners geprüft (Auswahl per Name und Rückfall). Fenster auf einem Monitor mit anderer Skalierung und das Verschieben zwischen Monitoren (WM_DPICHANGED) sind ungeprüft.
- **Vollbild bleibt 4:3:** Die Swapchain hat die Größe der 4:3-Ansicht, die Seitenbalken sind der schwarze Rahmen des Fensters (offener Punkt aus M3b/8.6: für Independent Flip mit VRR müsste die Swapchain den ganzen Monitor bedecken).
- **Render-to-Texture mit MSAA, Linienbreite:** weiter P2 (8.6 G9).
- **Speicher:** keine automatische Begrenzung; „Maximum“ (MSAA 8×, Skala 1,5, 4320×3240 intern) belegt 1,36 GiB Backbuffer, 5760×4320 mit 8× etwa 2,4 GiB. **Erledigt nach dem Review (8.8 R2, R21):** Der Renderer stuft ab, wenn die Puffer nicht angelegt werden können, und die Voreinstellungen richten sich nach dem Grafikspeicher.
- **Launcher-Bedienung von Hand:** Native Datei-Dialoge und der Wechsel des Farbschemas zur Laufzeit sind nur über Automatik, Offscreen-Tests und unsichtbare Screenshots der Windows-Plattform geprüft, nicht durch Klicken am Bildschirm.
- **Bildrate:** Der Launcher zeigt die Bildrate nur an (60 fps beim Logik-Takt 60 Hz); die Auswahl bis 120 Hz kommt mit M3d.

### 8.8 Abweichungen in M3c: Review-Befunde (Phase 4)

Stand: M3c Phase 4 (Review-Fixes), 2026-10-06. Das Review hat 23 Befunde zu Renderer, Launcher, Paket und Texten erhoben; alle sind nachgeprüft und bestätigt. 21 sind behoben, Befund 6 (Latenz) ist auf M3d verschoben, Befund 5 ist dokumentiert und mit einer Laufzeitregel abgesichert. Bericht: [`docs/m3c_report.md`](m3c_report.md). Schnittstellen: `src\INTERFACES.md` 4.22/4.23. Die Zeilen heißen R1–R23 nach der Nummer des Befunds; hier stehen nur die Abweichungen vom bisherigen Stand.

| # | Abschnitt | Stand nach Phase 2 | Umsetzung nach dem Review | Begründung / Beleg |
|---|---|---|---|---|
| R1 | 8.6 G6, Voreinstellungen | Gebrochene Render-Skalen (1,25/1,5/1,75) mit Mitchell-Netravali (B = C = 1/3); „Maximum“ = Skala 1,5 + MSAA 8× | **Catmull-Rom (B = 0, C = ½)** mit Anti-Ringing: Ergebnis auf den Bereich der 2×2 Quellpixel um die Zielpixelmitte begrenzt. **„Maximum“ = Skala 2,0 (exakter 2×2-Box) + MSAA 8× + `linear_aa`**, nach Grafikspeicher abgestuft (R21). Hinweis im Launcher bei Zwischenwerten | Offline gegen die Referenz Skala 2 + 8× (`run\m3c_final\kernel\kernels*.txt`): Kantenfehler bei 1,5 auf dem Legal-Bild 8,76 (Mitchell) → 5,03, Frontend 3,22 → 2,05, ohne Überschwinger; Fenster kleiner als das Bild (2880 → 2631): 7,44 → 6,03. Natives MSAA 8× liegt auf 2D-Szenen gleichauf mit der Referenz (0,07), im Frontend bei 1,89 |
| R2 | 8.6, M3 (Null-Renderer bei D3D11-Fehler) | Scheitert das Anlegen der Backbuffer, läuft das Spiel mit dem Null-Renderer unsichtbar weiter | `GpuRenderer::Create` stuft ab (`gfx::QualityFallbacks`: Skala 1, MSAA halbiert bis aus, zuletzt 640×480 ohne MSAA), ebenso das Nachführen der Größe; die `render:`-Zeile endet mit `reduced from …`. Scheitert auch 640×480, endet das Spiel mit `core::Fatal` (Exitcode 3) | Fall `ladder` (`run\m3c_final\cases`): 16384×12288 mit 8× → `E_OUTOFMEMORY` → 4×, das Spiel läuft sichtbar, Exitcode 0. Unit-Test „quality ladder“ |
| R3 | 8.6 (Resolve im Gamma-Raum) | MSAA-Resolve und Verkleinern mitteln gamma-kodierte Werte wie die Xbox | **Neuer Schlüssel `[render] linear_aa`** (Standard aus): TYPELESS-Ziele mit UNORM-Sichten für das Spiel und sRGB-Sichten für Resolve und Verkleinern; Present 1:1 und Vergrößern bleiben im Gamma-Raum. „Maximum“ schaltet ihn ein | Originaltreue bleibt der Standard. Unit-Test: Schachbrett 2:1 → 188 statt 128, MSAA-Kante mit Abdeckung c → encode(c) (≤ 3 Stufen); `m3c_check`-Varianten `max` und `frac` mit Debug-Layer ohne Meldung |
| R4 | 8.6 (Anisotropie) | Anisotropie auch bei MIN = LINEAR / MAG = POINT | Anisotropie nur, wenn auch die Vergrößerung linear ist | D3D11 vergrößert anisotrop immer linear. Unit-Test; der Nutzen ist in den erreichbaren Szenen nicht messbar (Prüfung mit Freecam, M4b) |
| R5 | 8.6 G4 | Alpha-to-Coverage schreibt die Abdeckung in den Alpha-Kanal des Ziels | Abweichung dokumentiert (GFX_CONTRACT 12.4). Neu: Der erste Draw, dessen Blend den Ziel-Alpha liest (DESTALPHA, INVDESTALPHA, SRCALPHASAT), schaltet Alpha-to-Coverage für den Rest der Sitzung ab (GFX/warn, Zähler in `render totals`) | Im Frontend kommt kein solcher Blend vor (0 Draws). SV_Coverage bräuchte Shader-Modell 4.1; Flimmern unter Bewegung wird in M4/M8 geprüft |
| R6 | M3b (Takt) | – | **Nicht geändert, nach M3d verschoben:** Present→sichtbar schwankt je Sitzung um 1–2 Refreshes (Median 12,9–14,0 ms in `m3_check`, 16,6 ms in den Paket-Läufen) | Gehört zum Takt (Phase des Present zum Vblank, DWM mit 4:3-Kind-Swapchain); M3d legt die Present-Phase vor das Ziel-Vblank und lässt die Swapchain den Monitor bedecken |
| R7, R8 | Launcher, Ergebnisfeld | „Ursache“ = letzte Ausgabezeile (`log file: …`); das Log scrollte waagerecht ans Ende einer langen letzten Zeile | `GameRunResult::cause()`: die Zeile `futurama: …` des Hosts ohne Präfix (und ohne `FATAL: `), nie `log file:`; JSON-Feld `cause`. Das Log beginnt links | UI-Test mit zweizeiliger stderr-Ausgabe; Fall `proto`: Ursache „stage LoadXbe failed …“ |
| R9 | Launcher, Spielstart | Ein erkanntes, nicht unterstütztes Abbild startete ohne Rückfrage; Hinweis „starten möglicherweise nicht“ | „Spielen“ fragt bei einem Prüfergebnis „nicht unterstützt“ oder „kein Xbox-Abbild“ nach (Standard „Abbrechen“, „Anderes Abbild wählen…“, „Trotzdem starten“); Hinweis „startet nicht“, kein doppelter Satz | UI-Test `unsupportedIsoAsksBeforePlay` |
| R10 | Launcher, Pfade | Pfade in Anführungszeichen („Als Pfad kopieren“) wurden übernommen | `CleanPathInput` in Assistent, Pfad-Editoren, `--set` und `SettingsModel::setValue`; ein Pfad mit Anführungszeichen in der INI ist ein Fehler | Fall `quoted`, UI-Test `quotedPathInput` |
| R11 | Launcher, Kommandozeile | Überzählige Positionsargumente wurden ignoriert | Rückgabecode 64 mit Hinweis auf Anführungszeichen | ctest `launcher_cli_positional`, Fall `positional` |
| R12 | Launcher, Fenster | Startgröße fest 1024×800 logisch | Höchstens 90 % der freien Fläche, Fenster unter der Taskleiste wird verschoben | 3484×1925 im Arbeitsbereich 3840×2052 bei simulierten 337,5 % (vorher 3484×2192) |
| R13 | Launcher, Texte | INI-Probleme und Speicherfehler auf Englisch, roher Build-String | Problemkennungen (`IniDocument::ProblemCode`) und Speicherfehler mit Handlungshinweis in der Oberflächensprache, Build „Release-Build vom 06.10.2026, 12:06 Uhr (MSVC …)“ | Unit-Tests |
| R14 | Paket | Keine Lizenztexte zu Qt | `LICENSES\Qt\` mit LGPL-3.0 und GPL-3.0 (FSF-Texte aus `launcher\licenses\`) und einem Hinweis zur Bezugsquelle des Qt-Quelltexts | LGPLv3 Abschnitt 4 |
| R15 | 1.10 (Exitcodes), Launcher | Mehrere Spiele bzw. Launcher mit denselben Daten gleichzeitig | Spiel: benannter Mutex je Benutzerdaten-Ordner vor dem Öffnen des Logs, **neuer Exitcode 6**. Launcher: `QLockFile` je INI, ein zweiter Start holt das erste Fenster nach vorn, Automatik Rückgabecode 70 | Fälle `game_twice` (zweites Spiel 6, Log des ersten vollständig) und `lock` |
| R16, R17 | Texte | Gemischte Anrede und Begriffe | Einheitlich „Sie“, „Zeitquelle der Spiellogik“, „Alt+Eingabetaste“, „Protokoll“, „Spielabbild (ISO)“, „Verkaufsversion“, „Fehlende D3D-Funktionen“, „Zeitlimit“, „Rückgabecode“; die Host-Meldung zu unbekannten Schlüsseln nennt keine Quelldatei mehr; Abschnitt `[game]` heißt „Spiellogik“; „Erneut spielen“ nach einem normalen Ende | Durchsicht der Screenshots `run\m3c_final\shots\` |
| R18 | 8.5 (Bildrate) | `frame_rate` im Reiter Erweitert zeigte 60 statt des wirksamen Logik-Takts | `frame_rate` ist bis M3d eine Entwickleroption; angezeigt wird ohne eigenen Wert der wirksame Takt (`EditorContext::dynamicDefault`) | – |
| R19 | Launcher, Pfade | „Auf Standard zurücksetzen“ leerte auch das Spielabbild; doppelte Ordnerzeilen | Das Spielabbild bleibt; die Ordnerübersicht zeigt nur Nicht-Einstellbares | – |
| R20 | Launcher, INI | Ungültiges UTF-8 wurde im Automatikmodus still ersetzt; neue Schlüssel hinter kaputten Zeilen | Automatik bricht ab (68, Datei unverändert); das GUI sichert vorher `<ini>.bak`; neue Schlüssel nur hinter gültigen Zeilen | Fall `latin1`, Unit-Test `newKeyNotAfterBrokenLine` |
| R21 | Voreinstellungen | Kein Bezug zum Grafikspeicher | Größter Puffer ≤ max(128 MB, min(VRAM/4, 2 GB)), Summe ≤ VRAM/2, sonst nächste Stufe; unbekannter Speicher: kein Supersampling | Unit-Test `presetMemory` (2 GB → 1,0/8×, 4 GB → 2,0/4×) |
| R22 | Bauen | `windeployqt` meldet „Translations will not be available“ | `build_launcher.cmd` filtert genau diese Zeilen, Rückgabecode von `windeployqt` entscheidet | – |
| R23 | Launcher, Stil | Unsichtbare Kontrollkästchen (Fusion dunkel), rahmenlose Knöpfe (windows11 offscreen) | Dunkle Palette mit helleren Eingabeflächen; „windows11“ nur auf Windows 11 mit echtem Fenstersystem, sonst „windowsvista“ bzw. Fusion | Screenshots `off_dark`, `off_light` |

Ergebnisse der Endprüfung (2026-10-06, Anzeige an):

| Prüfung | Ergebnis |
|---|---|
| Bauen | Spiel MSVC Debug, RelWithDebInfo, clang-cl Debug mit `FUT_WERROR=ON`; Launcher Debug/Release mit `LAUNCHER_WERROR=ON`: ohne Warnungen |
| Tests | `fut_tests` 259/259 in allen drei Builds (1 übersprungener Handtest); Launcher-ctest 4/4 in beiden Konfigurationen |
| `m1_check` / `m2_check` | ALL PASS (Debug 3 + RelWithDebInfo 2 Läufe; d3d11 und Null-Renderer) |
| `m3_check` (klassisch) | Erster Durchgang: alles außer M3b-7 (Fenster im Hintergrund, Vordergrundsperre). Wiederholung Debug/RelWithDebInfo/clang-cl: **ALL PASS**, xemu Legal 0,00, Tech-Notice 0,00, Ladebild 0,29–0,30, Frontend 0,01 |
| `m3c_check` (classic, hq, hq_noaa, ref, ss, max, frac) | Debug und RelWithDebInfo **ALL PASS**; X1 Legal 0,73, Tech-Notice 0,18, Ladebild 0,60, Frontend 0,98; X2 Verhältnis 0,46/0,44 |
| Paket über den Launcher | Klassisch, Standard, Maximum, Fenster: Exitcode 0, 59,97–60,05 Hz, Vordergrund 99,8–100 %; GPU-Spanne Ø 0,28 / 1,05 / 1,36 / 0,91 ms |
| Endfälle (`run\m3c_final\cases`) | 7/7 bestanden |

Offene Punkte nach dem Review: Latenz Present→sichtbar (R6, M3d), Render-to-Texture mit MSAA und Linienbreite (8.6 G9, P2), Monitorwahl auf einem zweiten Monitor ungeprüft (8.7), Vollbild bleibt eine 4:3-Swapchain (8.7), Nutzen von Anisotropie und Flimmern von Alpha-to-Coverage unter Bewegung (R4/R5, M4b/M8).

### 8.9 Abweichungen in M3d: Integration und Prüfung (Phase 3)

Stand: M3d Phase 3 (Integration und Prüfung), 2026-10-11. Zusammengeführt sind die Ergebnisse der beiden Agenten: Halbtakt-Uhr, `FrameInterp`, Pläne bis 120 Hz, Korrekturen F1–F4, Just-in-time-Start und Vollbild über den ganzen Monitor (Frame-Clock/Takt: `src\INTERFACES.md` 4.24, `analysis/loader/m3d_frameclock.md`) sowie die Interpolation im Renderer (`src\INTERFACES.md` 4.25, `GFX_CONTRACT.md` 13, `analysis/loader/m3d_interpolation.md`). Neu ist die Prüfung `tools\m3d_check.py` (Kriterien im Kopf der Datei). Messrechner: RTX 5090, LG-Fernseher 3840×2160 bei 120 Hz (225 %); der Fernseher war zu Beginn der Phase aus (GDI-Platzhalter `WinDisc`, 60 Hz) und für alle Anzeige-Messungen unten an. Die Zeilen heißen V1–V14.

| # | Abschnitt | Stand nach den Agenten | Umsetzung / Entscheidung in Phase 3 | Begründung / Beleg |
|---|---|---|---|---|
| V1 | m3d_frameclock.md 4.2 F1, INTERFACES 4.24 | Ein Zwischenframe ohne Schritt, der nichts zeichnet (Legal-Bild und Tech-Notice im Intro), wurde **nicht präsentiert** (`skipSwapChain`); auf den Vblank-Plänen hielt ein Timer das Raster (Start + 1 Refresh) | **Das wiederholte Bild wird präsentiert**, wie die Analyse es vorschlug: `rhi::Device::RepeatPreviousPicture()` (add-only) – Present-Pass und Screenshots lesen den persistenten Backbuffer des vorigen Present, die Rotation bleibt stehen; der Pacer behandelt den Frame wie jeden anderen (Sync-Intervall, Waitable, Just-in-time-Start). `OnScreenTracker::ExtendLastPresent` entfällt. Die Draw-Summary sagt „…, repeated (previous present)“, ein Screenshot zeigt das Bild auf dem Schirm (offener Punkt des Agenten erledigt) | **Grundursache, am 120-Hz-Fernseher gemessen:** Ohne Present lag kein Flip in der Warteschlange; das Waitable des nächsten Present gab sofort nach dessen GPU-Arbeit frei (0,4–1,9 ms statt eines Refreshs), der folgende Frame startete 0,07–0,25 Perioden nach seinem Vorgänger. Folge: Intro mit 146 fps (Intro → Frontend bei Frame 1942 nach 13,27 s), Durchschnitt 134 Hz, nur 76,97 % der vblank-gekoppelten Bilder genau 1 Refresh (`run\m3d_verify\v120`, Stand vor der Korrektur). Danach: alle Intro-Starts im Takt, 99,6–99,8 % genau 1 Refresh; Legal- und Tech-Notice-Paare (Frame mit und ohne Schritt) gleich und = xemu 0,00. Unit-Tests `rhi d3d11: a repeated picture presents the previous buffer again and keeps the rotation (M3d F1)`, `pacer m3d: above the tick rate a frame without a step that drew nothing keeps the previous picture` |
| V2 | M3b B43 (Erkennung „Anzeige hält den Takt“) | Vblank-Wartezeiten über 1,5 Perioden in 3 von 8 Frames → 1 s „free“ | Die ersten 12 Wartezeiten nach dem Wechsel in einen Vblank-Plan werden nicht bewertet (`DisplayLimitDetector::StartWarmup`) | Im Fenster (DWM) dauerten die ersten Wartezeiten nach dem Wechsel vom Timer 16, 29 und 19 ms. Bei 120 fps (Grenze 12,5 ms) schlug die Erkennung bei jedem Start an: 179 Frames (1,5 s) auf dem Timer; bei 60 fps blieb eine einzelne Wartezeit von 33 ms unter 25 ms. Danach: 30 Frames (nur die Hysterese) wie bei 60 fps. Unit-Test im Fall `pacer: display-limit detector` |
| V3 | Aufgabe der Phase 3 („Attract-Modus mit Spielkamera“) | Bewegungsprüfung im Attract-Modus geplant | Der Attract-Modus ist der Film `Video\Attract-PAL.xmv`; mit dem XMV-Stub (bis M6) endet er sofort (M2-Bericht). Ohne Eingabe gibt es **keine Spielszene und keine bewegte Kamera**. Geprüft wird mit dem Einflug des Frontends nach dem Kontextwechsel (13,0–14,9 s bei 120 fps: die Logo-Buchstaben drehen sich an animierten Knoten ein) und einem ruhigen Abschnitt (21,6–22,0 s) | Kamera-Interpolation, Schnittregel, Skins mit vielen Knochen, F2–F4 und Ingame-Mengen bleiben durch Unit-Tests belegt; Messung mit M4/M8 |
| V4 | Prüfung (neu) | Keine Prüfung für M3d | `tools\m3d_check.py`: Varianten i120, d120, f120 (Anzeige), t120 / t120off / t120x (Bewegung mit dem Entwicklerwert `frame_rate = 0x78` auf dem Timer-Plan, unabhängig von der Anzeige), c60, ll60off, c60off (60 fps). Screenshots und Draw-Traces laufen getrennt; t120off (Interpolation aus) ist die Referenz des Bildmaßes | Das Schreiben von `interp_draws.csv`/`interp_nodes.csv` kostet etwa 10 ms je Frame, die Uhr holt dann auf (Zwischenframes fehlen): Ein Lauf mit Trace und Screenshots hatte 139 Neustarts des Zeitplans und 2 brauchbare Bildtripel, getrennt 0 Neustarts und 126 Tripel |
| V5 | D9 (Bildmaß) | – | Verhältnis r = d(F0,F1)/(d(F0,F1)+d(F1,F2)) auf den bewegten Pixeln (0 = altes Bild, 1 = nicht interpoliert): Median 0,3–0,75 | Inhalte im Tick-Raster (Blitze, Oszilloskop-Welle, Einblenden, neu sichtbare Buchstaben) zeigen im Zwischenframe schon S[G] und ziehen r über ½; die Klassifizierung der bewegten Pixel zeigt die Buchstaben „dazwischen“, Blitze und Welle „= S[G]“. Gemessen 0,647 mit, 1,000 ohne Interpolation |
| V6 | M3-3 / D1 (Bildstarts im Takt) | ≥ 95 % der Bildstarts innerhalb ±5 % der Periode | Auf Vblank-Plänen nur berichtet; maßgeblich ist die Bildschirm-Statistik (≥ 99 % genau k Refreshes) | Mit dem Just-in-time-Start bei k = 1 im Fenster schwankt die Freigabe des Waitables um bis zu 0,7 ms; die Starts lagen dann in einem Lauf nur zu 93 % im Takt, während 99,94 % der Bilder genau 1 Refresh zeigten |
| V7 | M3b-7 / Messumgebung | Fenster ≥ 95 % der Frames im Vordergrund | Fenster anderer Programme (eine Chrome-Automatisierung einer anderen Sitzung, das Programm „simpsons [rexglue…]“, Benutzereingaben) nahmen den Vordergrund; nach dem Ende eines Spiels geht er an das zuvor aktive Fenster, das nächste Spiel bleibt im Hintergrund (Vordergrundsperre). Neu `tools\foreground_hold.py`: kleines Fenster „Messung läuft“, `AllowSetForegroundWindow(ASFW_ANY)` solange es vorn ist, startet die Prüfung als Kindprozess; ändert keine Einstellung. `m3d_check` bewertet die Anzeige-Kriterien auf den Vordergrund-Frames (gültig ab 95 % oder 1500 Vordergrund-Frames) | Vordergrund-Protokolle `run\m3d_verify\final\fg_*.txt`; mit dem Halter liefen alle Endprüfungen zu 99,9–100 % im Vordergrund |
| V8 | Prüfung Z2/R2 (Bild bei 60 fps bzw. S[G] unverändert) | Interpolation an/aus bitgleich, „bis ein Aufholschritt den Zustand verschiebt“ | Statische Szenen am selben Frame, animierte innerhalb ±6 (60 fps) bzw. ±8 Frames (120 fps); bei 120 fps darf ein Partner in höchstens 0,1 % der Pixel abweichen | Ein Ladehänger verschiebt das Frontend von Lauf zu Lauf um einige Frames (t120 gegen t120off: −1 bis −3 Frames). Im ruhigen Frontend unterscheiden sich zwei Läufe in bis zu 89 Pixeln (Inhalte nach Echtzeit) |
| V9 | `interp_trace.csv` | Spalte „interpolated“ doppelt (Zähler und Grund `None`) | Grund-Spalten heißen `reason_<name>` | – |
| V10 | Launcher-Testdaten | `sample_schema.json` ohne `[render] interpolation` | Schlüssel ergänzt; UI-Test: mit `interpolation = off` sagt der Hinweis „120 Bilder pro Sekunde.“ ohne Zwischenbilder | `UiTest::dependentKeysAndIsoStatus` |
| V11 | m3d_interpolation.md 7 (Stufe 1b optional, Standard aus) | Interpolation-Agent: Stufe 1b (Hierarchie) **Standard an** | Übernommen | Zwei-Tick-Mittelpunkttest am Einflug: Welt-Interpolation p99 12,5 px, max 108,6 px; Hierarchie p99 0,08 px (Grenze 1 px), max 17,5 px (beschleunigte Objekte; einmal 44 px bei einem Objekt, das aus der Ruhe anläuft: 0 → 0,09 → 2,36 Einheiten in zwei Ticks) |
| V12 | m3d_interpolation.md 6.2 | Zerstörungs-Probe nur bei Bedarf | `probe:NiAVObject::Destroy` ist immer installiert (Hooks entstehen vor der Konfiguration der Bildrate); bei 60 fps ruft sie nur das Original. `[hooks] disable = probe:NiAVObject::Destroy` entfernt sie | Z2: bei 60 fps ohne Wirkung (c60off bitgleich zu c60 und ll60off) |
| V13 | m3d_frameclock.md 4.3 („Standard-Empfehlung display, sobald die Interpolation abgenommen ist“) | `frame_rate` Standard „60“ | **Bleibt „60“** bis zur Abnahme durch Review und Nutzer | Die Anzeige-Messungen mit „120“ und „Bildschirm“ bestehen (unten); die Umstellung ist eine Produktentscheidung |
| V14 | `[render] low_latency` (Standard an) | Just-in-time-Start nur in Unit-Tests | Gemessen; Standard bleibt an | 60 fps auf 120 Hz: Bildstart → sichtbar Median 6,6 ms statt 16,6 ms, Present → sichtbar 3,9 statt 14,0 ms; genau 2 Refreshes 99,49–99,73 % mit gegen 99,87 % ohne (5–8 späte Bilder je 3000) |

Ergebnisse (2026-10-11, Anzeige an, Fenster vorn):

| Prüfung | Build / Lauf | Ergebnis |
|---|---|---|
| Bauen | Spiel MSVC Debug `build\m3d-final`, RelWithDebInfo `build\m3d-final-rel`, clang-cl Debug `build\m3d-final-clang` mit `FUT_WERROR=ON`; Launcher Debug/Release `build\m3d-final-launcher(-rel)` mit `LAUNCHER_WERROR=ON`; `gen_all.py --check` | ohne Warnungen; generierte Dateien aktuell |
| Tests | `fut_tests` alle drei Builds; Launcher-ctest | 309/309 (1 übersprungener Handtest), 1 419 205 Assertions; 4/4 in Debug und Release |
| `m3d_check` (RelWithDebInfo, `run\m3d_verify\base`) | 9 Varianten | **ALL PASS** (`run\m3d_verify\final\checks_m3d_rel.txt`) |
| – i120 / d120 (Fenster) / f120 (randloses Vollbild) | 120 Hz, „120“ bzw. „Bildschirm“ | Plan vblank k = 1 für 98,9–100 % der Vordergrund-Frames, **99,61 / 99,70 / 99,74 % der Bilder genau 1 Refresh**, 119,80–119,90 Hz, Present → sichtbar Median 1,8–1,9 ms; Logik 59,99–60,03 Schritte/s, je 50 % Frames mit 0 und 1 Schritt (2+ ≤ 0,07 %); alpha ½ 50,0 %, 1 49,9 %, sonst 1 Frame; 99,89–99,90 % der bewegten Draws interpoliert (Rest: Sprünge der Schleifen, 74–125 Verschiebungen, 4–8 Drehungen); Interpolation Ø 91–100 µs, p99 211–256 µs je Frame; 668–669 wiederholte Bilder, alle präsentiert; Debug-Layer 0 Meldungen; xemu Legal 0,00, Tech-Notice 0,00, Ladebild 0,30, Frontend 0,01–0,03 in beiden Frames jedes Paars. Vollbild: Swapchain 3840×2160, 4:3-Bild bei x = 480…3359, Balken schwarz |
| – t120 / t120off / t120x | Timer 120 fps, Einflug und ruhiges Frontend | angezeigte Zeit +½ Tick je Frame in 99,89 % der Frames (Rest: 2 Korrekturen der Uhr, nie rückwärts); Transformationen: Frames mit alpha 1 exakt S[G], Welt-Draws exakt der Lerp, 21 419 Hierarchie-Draws gegen eine unabhängige Python-Referenz max. 8,7·10⁻⁷ relativ, alle zwischen ihren Nachbarn; Mittelpunkttest p99 0,08 px; Bildtripel r = 0,647 mit, 1,000 ohne Interpolation; jeder Frame ohne Schritt hat sein Gegenstück ohne Interpolation (141 von 147 bitgleich) |
| – c60 / ll60off / c60off | 60 fps | M3b-Uhr, Interpolation inaktiv, 0 wiederholte Bilder; 99,0 % vblank k = 2, 99,66 % genau 2 Refreshes; Just-in-time siehe V14; c60off bitgleich zu c60 und ll60off in Legal, Tech-Notice, Ladebild und Frontend |
| `m1_check` / `m2_check` | Debug 3 + RelWithDebInfo 2 Läufe; d3d11 und Null-Renderer | ALL PASS; `m2_check` 59,99–60,00 Schritte/s, 99,53–99,80 % der Frames genau ein Schritt |
| `m3_check` (klassisch) | Debug, RelWithDebInfo (Debug-Layer an), clang-cl, je 1 Lauf mit `foreground_hold.py` | **ALL PASS**; xemu Legal 0,00, Tech-Notice 0,00, Ladebild 0,29–0,30, Frontend 0,01; Vordergrund 99,9–100 %, 99,49–99,60 % genau 2 Refreshes (Just-in-time an, V14) |
| `m3c_check` | Debug und RelWithDebInfo | **ALL PASS** (7 Varianten); X1 Legal 0,73, Tech-Notice 0,18, Ladebild 0,60, Frontend 0,99; X2 Verhältnis 0,45/0,48; das randlose Vollbild bedeckt jetzt den Monitor, die 4:3-Ausgabe bleibt 2880×2160 |
| Launcher-Screenshots | Release, Windows-Plattform 225 %, `run\m3d_verify\shots\win120` (Bildrate 120, Vollbild) und `windisplay` (Bildschirm, Fenster) | Grafik-Reiter: „120 fps (interpoliert)“ bzw. „Bildschirm (max. 120)“ mit „Auf diesem Bildschirm (120 Hz): 120 Bilder pro Sekunde mit Zwischenbildern.“; Fenstergröße im Vollbild ausgegraut mit „Gilt nur im Fenster …“; „Geringe Verzögerung“ und „Zwischenbilder berechnen“ im Reiter Erweitert |
| Sichtprüfung der Bildfolgen | `run\m3d_verify\base\m3d_check\t120\sheets`, Vergleich t120/t120off | Die Buchstaben des Logos laufen mit Zwischenständen ein (ohne Interpolation je zwei gleiche Bilder); keine Doppelbilder, kein Springen; Blitze, Welle und das Einblenden springen im 60-Hz-Raster (gewollt, 3.10 der Analyse); Menütexte stehen still (kein 2D/3D-Versatz in den erreichbaren Szenen) |

Offene Punkte nach Phase 3:
- **Ingame nicht erreichbar** (ohne Eingabe; Attract ist ein Film): Kamera-Interpolation und Schnittregel, bildgetriebene Objekte, Skins mit vielen Knochen, Mehrfach-Draws, F2–F4 (Death, Okkluder, Flackern) und die Kosten bei etwa 2000 Draws sind nur durch Unit-Tests bzw. statisch belegt (M4/M8). Die Kamera wird nur im Weltraum interpoliert (Kameras an animierten Knoten in Zwischensequenzen könnten Sehnenfehler zeigen).
- **Andere Bildwiederholraten** (144/165/180/240 Hz, 59,94/119,88 Hz), der Kadenz-Plan und VRR mit dem Tearing-Timer sind nur simuliert und durch Unit-Tests belegt; der Messrechner hat nur den 120-Hz-Fernseher.
- **Datenanimationen im 60-Hz-Raster** (UV, Alpha, Daumenkino, Blitze, Einblenden) laufen in Tick-Frames einen halben Tick vor der interpolierten Geometrie; im Frontend unauffällig, in Levels zu beobachten (M8).
- **Doppel-Render-Test** (m3d_frameclock.md 4.4) und F5 (Schwarzbilder nach Intermission) sind nicht umgesetzt (M4/M8).
- **Just-in-time-Start** kostet bei 60 fps 0,1–0,4 Prozentpunkte genau getroffener Refreshes (V14); bei k = 1 im Fenster bringt er wenig.
- **Standard der Bildrate** bleibt „60“ (V13); die Entscheidung für „Monitor“ steht aus.
- Kleinigkeit: Im Reiter Erweitert stehen „Geringe Verzögerung“ und „Zwischenbilder berechnen“ zwischen technischen Schlüsseln (Reihenfolge 105/112 neben Screenshot-Größe 100 und Grafikschnittstelle 110).

### 8.10 Abweichungen in M3d: Review-Befunde (Phase 5)

Stand: M3d Phase 5 (Review-Fixes), 2026-10-11. Das Review hat 13 Befunde erhoben (Interpolation 1–7, Takt und Uhr 8–11, Launcher 12, Messwerkzeug 13); alle sind nachgeprüft und bestätigt. Zehn sind im Code behoben, drei dokumentiert (2 Kosten, 5 Kamera und Elternkette, 7 Spiegelung). Bericht: [`docs/m3d_report.md`](m3d_report.md) Abschnitt 8. Schnittstellen: `src\INTERFACES.md` 4.24/4.25 („Review fixes“). Die Zeilen heißen P1–P13 nach der Nummer des Befunds.

| # | Abschnitt | Stand nach Phase 3 | Umsetzung nach dem Review | Begründung / Beleg |
|---|---|---|---|---|
| P1 | m3d_interpolation.md 3.4 (Regel R2/R4) | Sprung, wenn Schritt > 2 Radien und > 4 × der vorige Schritt (unbekannt = 0), je gezeichnetem Blatt im Weltraum | Bezug = die größere der letzten beiden bekannten Bewegungen je Tick; unbekannte Vorbewegung (erste Sichtung im Tick davor): nur Schritt > 16 Radien bzw. Drehung > 90°; mit Elternkette entscheidet die Kette (nur ein Knoten, dessen eigene lokale Transformation springt, rastet ein, mit allen Nachfahren gemeinsam) | Review-Lauf `trace120`: Objekt 0x03341160 (r 0,0385) rastete am Umkehrpunkt ein (9 px bei 640×480, ganzer Tick und dann Stillstand). Unit-Tests S10 (Umkehrpunkt, Mitte exakt), S1 (schnelles Objekt im zweiten Tick interpoliert), S2 (Körper und Hand bleiben zusammen, Abstand 0,500) |
| P2 | GFX_CONTRACT 13 (Kosten) | Schätzung 0,05 ms + < 0,1 ms je Tick-Frame bei 2000 Draws | Dokumentiert: gemessen 0,55 µs je Draw mit Hierarchie (Tick-Frames Median 0,196 ms, p99 0,371 ms bei 346 Draws), bei 2000 Draws etwa 1 ms (12 % von 8,33 ms); keine Optimierung in M3d, Kostenprüfung mit etwa 2000 Draws in M8 | Review-Läufe `cost_on`/`cost_flat`, `long120` (40 000 Frames ohne Wachstum von Tabellen und Speicher) |
| P3 | Historie (Wiederaufnahme im Zwischenframe) | Gleiche Adresse und Signatur in derselben Generation = bildgetrieben (120 Frames ohne Interpolation) | Wurde die Adresse seit der früheren Aufnahme zerstört, beginnt der Eintrag neu (keine Bildgetrieben-Markierung); eine Zerstörung im Aufnahme-Frame vor der Aufnahme zählt nicht gegen den neuen Eintrag (`kAfterDestroy`) | Unit-Test S6: 60 von 60 folgenden Tick-Frames interpoliert (vorher 59 bildgetrieben); auch der Fall „zerstört vor dem Tick-Frame“ rastet nur einmal ein |
| P4 | Kamera-Historie | Kein Blick in die Zerstörungsliste | `Camera()` prüft die Liste (`destroyedCheckFrom`, Grund `Destroyed`); die Schnittschwellen 0,5 Einheiten / 20° bleiben, ihre Prüfung mit Freecam und Zwischensequenzen kommt in M4b/M8 | Unit-Test S11: neue Kamera an derselben Adresse wird gezeigt wie gegeben (vorher Mischung z 0,225). Fall S4 (Schnitt unter den Schwellen) bleibt offen |
| P5 | Kamera und Elternkette | – | Dokumentiert: Die Kamera wird nur im Weltraum interpoliert, Objekte entlang der Kette; Geometrie fest am drehenden Kameraknoten verschiebt sich im Bild um 0,1 px (3°/Tick) bis 10 px (20°/Tick, 5 Einheiten Ausleger) bei 640 px | Review-Szenario S9; im erreichbaren Frontend ist die Kamera statisch. Prüfung mit M4b/M8 |
| P6 | Stufe 1b (Cache lesbarer Seiten) | Einmal lesbar = immer lesbar | Neue Release-Epoche `guest::MemoryReleaseEpoch()` (`kernel\mm.cpp` meldet jedes Freigeben/Decommit); der Cache wird bei einer neuen Epoche verworfen | `guest::Read` ist ein `memcpy` ohne SEH. Unit-Test „readable-page cache“; Zähler `readable-page cache reset` in der Exitzeile |
| P7 | Interpolation (Spiegelung) | Gespiegelte Rotation (det ≤ 0) rastet immer ein | Unverändert, dokumentiert; Zähler „mirror“ bleibt der Auslöser für einen Spiegel-Pfad in P2/M8 | Die Disc hat keine gespiegelten Transformationen; negative gleichmäßige Skalierung interpoliert. Review S7: Slerp gegen Double-Referenz max. 0,0001° |
| P8 | m3d_frameclock.md 2.7/3.2 (59,94 Hz) | „120“/„Monitor“ an Bildschirmen knapp unter der Tickrate: Anzeigezeit-Quanten mit Interpolation | Bildrate ≤ Tickrate × 1,002 → N = 1, die M3b-Uhr ohne Interpolation wie „60“ (`pacing::FramesPerTick`); Launcher-Hinweis ebenso | Review-Simulation: bei 50 Hz jedes 5. Bild fast 1 Tick Versatz, bei 59,94 Hz Sprung plus Standbild statt des Doppelschritts, 1 Tick mehr Inhaltsverzögerung. Unit-Tests `FramesPerTick(59,94/59,95/60,02/50) = 1`, Launcher `frameRateEstimate` |
| P9 | `LatencyBudget` (Just-in-time) | Faktor nach 2 s ohne späten Frame halbiert | Rückfall mit Gedächtnis: ein später Frame innerhalb eines Intervalls nach dem Halbieren verdoppelt das Intervall (bis 32 s); nach 2 solchen Fehlschlägen hält der Faktor bis zum nächsten Planwechsel. Die GPU-Zeit wird nicht addiert (ihre Spanne überlappt die CPU-Arbeit) | Review-Simulation: Vorlauf zwischen B und 2B gab 29,4 späte Frames pro Minute. Unit-Test: 10 Minuten mit Vorlauf 5–12 ms höchstens 4 späte Frames, danach keiner |
| P10 | m3d_frameclock.md 2.5 (Aufholen, Kappung) | Aufhol- und Kappungs-Frames `snapAll`, danach ein Standbild | Kein `snapAll` mehr (Merker `kFrameInterpCatchUp`); der Renderer interpoliert über das Paar der letzten k = `pairTicks` Ticks (k ≤ 6) mit Gewicht (k − 1 + alpha)/k | Review-Lauf `s120` (Hänger 25/40 ms, Kappungen): Standbild nach jedem Ereignis. Unit-Test „step pairs“: angezeigt G − ½, G − 1 nach der Kappung, dann + ½ je Frame |
| P11 | Frame-Clock (Eingang) | Uhr vergleicht die Gast-Probe nach den Streaming-Pumpen | In den Zwischenbild-Regimes der Bildstart des Pacers (`FrameAdvance::frameStartQpc`, frisch und ≤ Probe, sonst die Probe); ein Planwechsel (`planSerial`) wird einmal aufgefangen (Versatz bis 1 Tick, nie bei N = 1) | Review: falsches Aufholen bei 5–9 ms Pumpenarbeit im Ladebild und Halt nach „free → vblank“. Unit-Test „plan change absorbed“ |
| P12 | 6 (Launcher-Option) | „60 fps“, „120 fps (interpoliert)“, „Bildschirm (max. 120)“ | Die Beschriftung der Vorgabe: „60“, „120 (interpoliert)“, „Monitor (max. 120)“ (en „120 (interpolated)“, „Monitor (max. 120)“), Hilfetext angepasst | ROADMAP und Abschnitt 6 versprechen diese Texte; UI-Test |
| P13 | `tools\timing_compare.py` | Periode der Dampfwolke aus Stufenübergängen (120 fps: 1,103 s) | Autokorrelation der Helligkeit über die Logzeit | `dur60`/`dur120` des Reviews: 0,999 / 1,003 s |

Ergebnisse der Endprüfung (2026-10-11, **Fernseher aus**: Plan „free“ ohne Present, Anzeige-Kriterien nicht messbar):

| Prüfung | Ergebnis |
|---|---|
| Bauen | Spiel MSVC Debug `build\m3d-fix`, RelWithDebInfo `build\m3d-fix-rel`, clang-cl `build\m3d-fix-clang` mit `FUT_WERROR=ON`; Launcher Debug/Release `build\m3d-fix-launcher(-rel)` mit `LAUNCHER_WERROR=ON`; `gen_all.py --check`: ohne Warnungen, aktuell |
| Tests | `fut_tests` 318/318 in allen drei Builds (1 übersprungener Handtest), 1 421 356 Assertions; Launcher-ctest 4/4 in Debug und Release |
| `m3d_check` (`--display-optional`) | alle anzeigeunabhängigen Kriterien PASS (G1–G3, D2–D9, R1, R2, Z1, Z2); D1, F1, L1, M3b-7 SKIP. Logik 60,00 Schritte/s, alpha ½ × 1795 / 1 × 1790, angezeigte Zeit + ½ in 99,94 %, 99,91 % der bewegten Draws interpoliert, Mittelpunkttest p99 0,079 px, Bildtripel 0,636 / 1,000 |
| `m1_check` / `m2_check` | ALL PASS (Debug 3 + RelWithDebInfo 2 Läufe; d3d11 und Null-Renderer) |
| `m3_check` | Debug, RelWithDebInfo, clang-cl: alles PASS außer M3b-7 (keine Bildschirm-Statistik bei ausgeschalteter Anzeige); xemu 0,00 / 0,00 / 0,30 / 0,01 |
| `m3c_check` | Debug und RelWithDebInfo ALL PASS; X1 0,73 / 0,18 / 0,60 / 0,99 |
| Kosten (t120, 346 Draws) | Tick-Frames Median 194 µs, p99 369 µs; Frames ohne Schritt 39 / 92 µs |
| Paket | `dist\FuturamaPC\` (24 Dateien, 29,8 MB) |

Zusätzlich gefunden und behoben: F1 (voriges Bild behalten) wirkt jetzt auch ohne Present (Anzeige aus, minimiert); vorher zeigte ein Screenshot eines Zwischenframes ohne Zeichnen im Intro Schwarz. `m3d_check` R2 wertet ein Fensteranfangsbild ohne erfasstes Gegenstück nicht.

Offen: Anzeige-Kriterien mit eingeschaltetem 120-Hz-Fernseher wiederholen (neuer Eingang der Uhr auf dem Vblank-Plan, Rückfall des Latenz-Budgets); Ingame-Punkte und Kamera an animierten Knoten (P4/P5) mit M4/M4b/M8; Kosten bei etwa 2000 Draws (P2) mit M8.

### 8.11 Abweichungen in M4: Eingabe, Integration und erster Ingame-Lauf (Phase 3)

Stand: M4 Phase 5 (Review-Befunde behoben, Bericht [`m4_report.md`](m4_report.md)), 2026-10-12; zuvor Phase 3 (Integration). Zusammengeführt sind drei Teile: die Eingabeschicht mit XPP-HLE, Router, XInput- und Tastatur/Maus-Backend (`src\INTERFACES.md` 4.28), der Reiter „Steuerung“ im Launcher (4.26) und das Eingabeskript `[debug] input_script` mit `tools\m4_check.py` (4.27). Analyse: `analysis/loader/m4_input.md`. Zum ersten Mal verlässt das Spiel das Frontend: Ein Skript startet „Neues Spiel“, Level 1-1 (Planet-Express-Hangar) lädt, Fry ist steuerbar. Geprüft ohne Controller und mit ausgeschaltetem Fernseher (GDI-Platzhalter `WinDisc`, 60 Hz). Die Zeilen heißen E1–E20; E17–E20 kamen mit den Review-Befunden (Phase 5) dazu.

| # | Abschnitt | Design / Analyse | Umsetzung in M4 | Begründung / Beleg |
|---|---|---|---|---|
| E1 | E7, Anhang B (7 XPP-Einstiege „Stub (M2), HLE (M4)“), avi.md 4.1 | HLE der 7 XPP-Einstiege | `input\xpp_hle.cpp` ersetzt `xpp_stubs.*` (Hook-Namen `XPP::*` unverändert). `XInitDevices` startet nur die Host-Eingabeschicht, nicht den USB-/MU-Treiberstapel; `XGetDevices(MemoryUnit)` = 0; `XInputOpen` liefert ein Gast-Dummyobjekt je Port (nie NULL für 0..3); `XInputGetState` 0 / 0x48F (Status genullt) / 0x57; `XInputSetState` setzt `dwStatus` **synchron** (0 / 0x48F / 0x57, nie 0x3E5), `hEvent` wird ignoriert | Das Spiel sendet Rumble nur bei `dwStatus != ERROR_IO_PENDING` und setzt den Status nie selbst zurück (m4_input.md 2); ein `XInputOpen` = NULL ließe den Port „verbunden“ ohne Eingabe. Rumble-Lauf: Test-Rumble des Optionsmenüs 65535/65535, Abklingen in 55–56 Frames über 61 Änderungen. Summenzeile heißt jetzt `XDK XPP HLE: …` (vorher „stubs“) |
| E2 | m4_input.md 8.1 (Portmodell) | Ein Spieler: alle Quellen auf Anschluss 1 | Standard `[input] pad_mode = single`: alle XInput-Controller, Tastatur/Maus und das Skript werden auf Anschluss 1 zusammengeführt (Tasten ODER, analoge Tasten Maximum, je Achse die größere Auslenkung); `per_port` legt XInput-Benutzer n auf Anschluss n+1 | Das Spiel kennt nur Spieler 1; ein zweiter Controller soll nicht „Controller anschließen“ auslösen |
| E3 | Xbox-Verhalten (ERRCTRLINGAME) | Ohne Controller pausiert das Spiel mit „Controller anschließen“ | **Tastatur/Maus hält Anschluss 1 immer verbunden.** Abziehen eines echten Pads löst daher keine Pause aus, solange Tastatur/Maus da ist; zum Testen gibt es `InputRouter::SimulateUnplug` (Skriptbefehle `unplug`/`replug`) | Ein PC-Spieler ohne Controller soll spielen können. Der simulierte Abzug führt im Level zur Pause aus der Aufrufstelle 0x2ACDB mit „Bitte Controller wieder am Controller-Anschluss 1 anschließen und mit START weiter.“; START danach schließt die Pause (m4_check M4-6) |
| E4 | – (neu) | – | Physische Quellen zählen nur, solange das Spielfenster vorn ist (`[input] background_input`, Standard aus); nach Fokus-Rückkehr und nach dem Ende einer Umleitung gehen gehaltene Tasten erst nach einmaligem Loslassen an das Spiel. Fokusverlust lässt alle Tasten los und gibt die Maus frei | Keine Eingaben aus anderen Fenstern, keine falschen Druck-Flanken. Das Skript (SourceKind::Script) ist fokusunabhängig, automatische Läufe brauchen kein Vordergrundfenster |
| E5 | m4_input.md 9.2 (Esc auf START, „Esc kontextabhängig“ offen) | Esc = START | `[input] menu_escape` (Standard an): Esc und andere Tastatur/Maus-START-Belegungen wirken im Kontext `Frontend` als BACK (beim Drücken entschieden), ingame als START (Pause). Abschaltbar | In Menüs bestätigt START; Esc soll dort zurückgehen. Frontend-Bildschirme, die ausdrücklich START verlangen (Menücode 6), sind im Export nicht gefunden, aber nicht ausgeschlossen |
| E6 | m4_input.md 8.4 (Maus auf den Stick) | Maus über die Spielkennlinie | Mausbewegung → gewünschte Auslenkung → Rohwert über die Umkehrung der Stick-Kennlinie des Spiels (`input\stick_curve.h`: Totzone 0,4, 0,2 wenn die andere Achse über 0,4 liegt, Verstärkung 1,3); `mouse_sensitivity` 1,0 = 24 Counts je Tick für volle Auslenkung | Ohne Umkehrung würden kleine Bewegungen in der Totzone verschwinden. **Nur Unit-Tests**, keine Laufzeit-Injektion (würde den echten Mauszeiger bewegen) |
| E7 | m4_input.md 6 (Rumble) | Rumble an das Pad des Spielers | `rumble`/`rumble_strength` skalieren; stumm ohne Fokus, auf umgeleiteten und abgezogenen Ports; gesendet nur an die zuletzt bewusst benutzte Quelle mit Motoren (`InputSource::HasMotors`; Taste, Analogtaste ab 30/255 oder Stick über 0,2 – nicht schon ein Zustand ungleich null, Review-Befund 2), nur bei Änderung, nach Wiederverbinden erneut; Motoren aus bei Programmende und Absturz | Das Spiel rumbelt nur seinen aktiven Controller; mehrere Pads auf Anschluss 1 sollen nicht alle vibrieren |
| E8 | INTERFACES 4.26 (Schema) | – | 48 Belegungsschlüssel `pad_<control>` / `kbm_<control>` plus `pad_mode`, `rumble`, `rumble_strength`, `pad_deadzone`, `mouse_sensitivity`, `mouse_capture`, `menu_escape`, `background_input` (56 Zeilen in `[input]`). `ui.order` = 100 + 2i bzw. 101 + 2i (eindeutig je Reiter). Eine ungültige Belegungsliste ist im Host ein **Konfigurationsfehler (Rückgabecode 1)**, im Launcher ein Fehler, der Speichern und „Spielen“ sperrt | Gleiche Regel auf beiden Seiten (Launcher-Test vergleicht Namen und VK-Codes mit `src\host\input\bindings.cpp`). Doppelbelegungen innerhalb eines Geräts sind nur Warnungen (der Host verknüpft sie mit ODER) |
| E9 | Launcher (Reiter „Steuerung“) | – | Mausbewegung wird im Aufnahmedialog über vier Richtungsknöpfe belegt, nicht durch Bewegen der Maus; Tastaturaufnahme greift die Tastatur (Esc/Tab/Enter belegbar), Abbruch per Knopf oder nach 10 s | Vermeidet versehentliche Aufnahmen. Die offscreen-Plattform kennt kein `grabKeyboard`; der echte Tastatur-Griff und die AltGr-Wartezeit unter Windows sind nicht interaktiv geprüft |
| E10 | m4_input.md 8.3 (Abtastung pro Tick), INTERFACES 4.27 | Skript im Takt der `XInputGetState`-Aufrufe | Zusätzlich eine Herzschlag-Probe auf `FUN_0009C140` (Hotplug-Prüfung, Aufruf 0xD25D3, einmal je Tick) zählt die Ticks, treibt das Skript auch bei abgezogenem Port und führt `quit` außerhalb des XPP-Aufrufstapels aus | Ein Prozessende innerhalb von `XInputGetState` blockierte die Exit-Reporter (Deadlock). Neue Beobachter-Probes (rufen das Original): `probe:Game::MainMenuActivate` 0x20150, `probe:Game::LevelReadyTest` 0xC3E20 (nur Aufruf 0x2AB2F), `probe:Game::PauseOpen` 0xA8220, `probe:Game::HotplugCheck` 0x9C140; Prologe mit capstone geprüft, kein Sprungziel in den überschriebenen Bytes |
| E11 | M7 (Spielstände), m4_input.md 4.3 | Speicherdialoge beim ersten Bestätigen nicht ausgeschlossen | **Kein Dialog erscheint:** Mit leerem `partition1` und genug freiem Platz der Host-Platte (`GetDiskFreeSpaceEx`) laufen die Prüfungen a–c ohne sichtbaren Dialog; „Neues Spiel“ startet direkt das Level. Das Skript prüft vor jedem A `no_dialog` und drückt nie blind A (`MEMCARD_047`: A = Dashboard) | In allen Läufen (m4_check Debug/RelWithDebInfo/clang-cl, Erkundungs- und 10-Minuten-Lauf) Frontend → Ingame 19 Frames nach dem ersten A. M7 ist für den Weg in Level 1-1 nicht nötig. Bei zu wenig freiem Platz würde `MEMCARD_047` erscheinen; dann führt B („ohne Speichern weiter“) weiter |
| E12 | M2-Kriterium 7 (dokumentierte Fehlzugriffe) | – | `vfs_known_missing.cpp` um die optionalen Level-Dateien `Levels\*\backdrop.nif`, `backdrop_static.nif`, `camerapathtest.nif` (Existenzprüfung in `FUN_000C5B40`, Zustände 5–7) und `Sounds\Level1\Ambience-PlanetExp\Hum-loop2.wav` (vom Level-Skript `Level1-1.Ful` genannt, auf der Disc nur `hum-loop.wav`) erweitert | Disc-Listen `orig\retail_eu\FutPAL00_files.txt`/`FutPAL03_files.txt`; Unit-Test `vfs: documented misses of level loading (Level1-1)`. Danach 0 `UNDOCUMENTED` im Level-Lauf |
| E13 | Renderer P2 (Abschnitt 7, GFX_CONTRACT 11) | Render to Texture, Partikel, Texture-Effects in P2 | **Ingame zum ersten Mal gebraucht, weiter nicht nachgebildet** (siehe Liste unten). Kein Absturz, keine Falle | Zählungen aus `m4_check` (Level-Lauf, 2220 Frames) und dem 10-Minuten-Lauf `run\m4-integ\roam` (36 339 Frames) |
| E14 | Determinismus (Lockstep-Uhr) | Skript läuft Tick für Tick gleich | Gleiche Skriptzeilen laufen von Lauf zu Lauf einige Ticks versetzt (z. B. `context Frontend` bei Tick 890–892), weil Intro und Laden nach I/O- und QPC-Zeit laufen | Skripte warten auf Bedingungen (`context`, `main_menu`, `level_loaded`, `controllable`), nicht auf absolute Ticks |
| E15 | Hardware | – | **Nicht mit echter Hardware geprüft:** XInput-Eingabe, Hotplug echter Pads, LB/RB- und Trigger-Gefühl, Rumble an einem echten Gerät, echte Mausbewegung, Mausfang in Fenster und Vollbild. Das echte `xinput1_4.dll` ist nur bis „0 von 4 Controllern verbunden“ belegt; Tastatur nur per `SendInput` in das laufende Spiel (Menü, Neues Spiel, Laufen, Springen, Pause) | Kein Controller am Messrechner; alles Übrige über `XInputApi`-Doubles, simulierte Quellen und `SimulatedPadReader` im Launcher |
| E16 | M2 T2, M3-3/C2, M3b-6/M3b-7 (Takt) | ≥ 99 % der Frames genau ein Schritt, ≥ 59,5 Hz im Mittel | Der Fernseher war während der ganzen Phase nicht nur aus, sondern abgemeldet: Windows meldet den GDI-Platzhalter `WinDisc` mit 60 Hz (in M3d Phase 5 noch „120 Hz, display off“). Der Pacer versucht den Vblank-Plan, die Wartezeiten sind zu lang („display-limited“), er fällt auf den Timer zurück (16 Planwechsel je 3000 Frames). Ergebnis 96,3–96,6 % genau ein Schritt, 57,2 Hz im Mittel, langsamstes 1-s-Fenster 39–40 Hz. **Der Build vor M4 (`build\m3d-fix-rel`) liefert unter denselben Bedingungen dieselben Werte** (`m3_check`: 96,45 %, 57,25 Hz; `run\m4-integ\checks\checks_m3_rel_pre_m4.txt`). Ingame (10-Minuten-Lauf): Arbeit je Frame im Mittel 1,29 ms (p99 2,7 ms), 976 von 36 372 Frames länger als 1,5 Perioden | Umgebung (Platzhalter-Anzeige), nicht die Eingabeschicht. Mit dem Null-Renderer (ohne Present) besteht T2. Mit angemeldetem Bildschirm wiederholen; ob der Pacer den `WinDisc`-Fall besser erkennen sollte (sofort Timer statt Vblank-Versuche), ist ein offener Punkt für M3b/M8. **Endprüfung Phase 5** (`run\m4-final\checks`): Das Spiel meldete durchgehend „display off“ und ließ das Present aus; damit bestehen T2 (99,73 %), M3-3, M3b-6 und C2, und nur die Anzeige-Kriterien (M3b-7, `m3d_check` D1/F1/L1) fehlen bzw. sind SKIP |
| E17 | INTERFACES 4.27 (Skript „beim ersten `Connected()` geladen“) | Skript wird beim ersten Abfragen geladen | **Laden in `inp::StartInput`** (`script::PreloadScript`), bevor eine XPP- oder Router-Sperre genommen wird; `XppDevices` fragt den Router vor dem eigenen Mutex ab | Review-Befund 1: Ein Syntaxfehler beendete den Prozess unter `XppDevices::mutex_`, der Exit-Reporter sperrte denselben Mutex → `std::terminate`, 0xC0000409 statt Rückgabecode 1. Jetzt Rückgabecode 1 mit Fehlerzeile (`m4_check` M4-9) |
| E18 | m4_input.md 8.3 (Mausrad = Raste) | Ein Raw-Input-Datensatz = eine Raste | Mausrad-Werte werden je Richtung summiert, eine Raste je 120 Einheiten (`WHEEL_DELTA`), ein Richtungswechsel verwirft den Rest | Review-Befund 3: hochauflösende Räder senden mehrere Teil-Datensätze je Raste und lösten bis zu dreimal aus. Nur per Unit-Test geprüft |
| E19 | m4_input.md 8.4 (Tastatur auf den Stick) | Taste = volle Auslenkung je Achse | Digitale Stick-Richtungen der Tastatur werden auf den Einheitskreis begrenzt (W+D = 0,707 je Achse, Rohwert 23 170), wie ein Pad an seinem runden Anschlag; Pad-Werte unverändert. Eine „Gehen“-Taste gibt es nicht | Review-Befund 6: 1,0/1,0 kann kein echtes Pad liefern; das Spiel begrenzt nur je Achse (`FUN_00057940`). Ob es diagonal schneller lief, wurde nicht gemessen – die Änderung ist vorbeugend |
| E20 | INTERFACES 4.26 (Belegungsliste) | Höchstens 4 Tokens, Duplikate „erlaubt und ignoriert“ | Duplikate zählen nicht gegen die Grenze (vorher Konfigurationsfehler bei 5 × `pad:a`); Leerzeichen um `:` sind auf beiden Seiten erlaubt; `f<n>`/`vk_<xx>` nur mit Ziffern auch im Launcher | Review-Befunde 4 und 11: Host und Launcher akzeptieren dieselbe Sprache; gemeinsame Grenzfälle in beiden Testsuiten |

**Erster Ingame-Lauf (Level 1-1, `run\m4-integ`):** Intro → Frontend nach 15,2 s, Hauptmenü bereit, A auf „Neues Spiel“, Ingame nach 19 Frames, Level geladen nach etwa 7,5 s (Frame 1331), steuerbar bei Frame 1493. Laufen, Kamera drehen, Springen, beide Angriffe, Pause (START/Esc), Kamera-Hinweis „Drücken Sie A, um die Kamera zu überspringen“ an einem Auslöser, Wechsel aufs obere Stockwerk des Hangars. Ein Lauf mit 10 Minuten Zufallseingaben (`roam`, 37 665 Ticks, 169 Tastendrücke) endete ohne Absturz, ohne Warnung, ohne Gast-Ausnahme und ohne Fehlzugriff außer den vier oben dokumentierten. Ein Level-Lauf mit D3D11-Debug-Layer und MSAA 4× (`run\m4-integ\dbglayer`) meldet 0 Fehler und 0 Warnungen. Weiter als Level 1-1 kam kein Lauf (der Levelverlauf ist ohne Kenntnis des Levels nicht skriptbar).

**Sichtbare Lücken im Level (für spätere Meilensteine, kein Absturz):**

- **Render to Texture (P2):** Im Level-Lauf gehen etwa 10 600 von 332 000 Shape-Draws (2220 Frames) in Texturziele (gezählte No-ops, B24); ingame gibt es etwa 8 Szenen je Frame; 2 gerenderte Texturen werden angelegt. **Vermutung:** Dazu gehören die Konturlinien (`NiOutline`) – Figuren wie Fry erscheinen ohne die schwarzen Umrisse des Originals – und Schatten/Effekte.
- **Unterseite des Planet-Express-Schiffs tiefschwarz** (in mehreren Kamerawinkeln). Ursache nicht geklärt; Kandidaten: Multi-Map/Mehrpass-Texturen (nur die Basis-Map wird gezeichnet), Texture-Effects (erfasst, nicht angewendet) oder Beleuchtung ohne Umgebungslicht. Mit P2/M8 gegen xemu vergleichen.
- **Partikel (Slot 52 `RenderPoints`):** etwa 12 000 Aufrufe je Level-Lauf, nicht gezeichnet.
- **5 Texturen ohne Bild** → Standardtextur 8×8; Purge-Slots 58/59/61/64 nicht nachgebildet (generische No-ops), Slots 56/71 generische No-ops wie auf der Xbox.
- **Ton (M5)** und **Filme (M6)** fehlen weiter; Level-Filme (`1-1-0n`) enden sofort.

### 8.12 Abweichungen in M5: Dekoder, 3D-Klang und Hall (Codec/3D/Hall-Agent)

Stand: M5 Phase 1, 2026-10-12. Dateien `src\host\audio\codec_*`, `spatial_*`, `reverb_*` (`src\INTERFACES.md` 4.29, Absatz „Codec, 3D and reverb“); Analyse `analysis/loader/m5_audio.md` 3.6, 3.7, 4.5, 6.4–6.6. Geprüft mit Unit-Tests aus synthetischen Signalen und (nur unter `run\m5-codec\`) mit 13 Spieldateien aller Formatklassen und der `dsstdfx.bin` der Disc. Die Zeilen heißen K1–K11.

| # | Abschnitt | Design / Analyse | Umsetzung in M5 | Begründung / Beleg |
|---|---|---|---|---|
| K1 | m5_audio.md 4.5 | Xbox-ADPCM wie ffmpeg `adpcm_ima_xbox` | Kopf-Sample + 63 Nibbles, Schritt `((2n+1)·step)>>3`; ein Schrittindex > 88 im Kopf wird auf 88 begrenzt (ffmpeg verwirft das Paket); ein unvollständiger letzter Block zählt als mit Nullen aufgefüllt | Bitgleich mit ffmpeg auf zufälligen Blöcken (mono/stereo, Prüfsummen im Test) und auf 13 Spieldateien (ADPCM mono/stereo 22,05/44,1/48 kHz, PCM8, PCM16). Ob der Xbox-Chip genauso rundet, bleibt offen (m5_audio.md 10) |
| K2 | m5_audio.md 2.1 (Frequenz) | `SetFrequencyRatio(f / rate)` | Frequenz über das Xbox-Tonhöhenmodell: `round(4096·log2(f/48000))`, begrenzt auf −0x7FFF..0x1FFF (187,5 Hz..191 967 Hz), dann Verhältnis zur Quellrate; `0` = Quellrate | XAudioCalculatePitch 0x246ED1, FUN_0024C5C7. Abweichung zur reinen Division höchstens 1/8192 Oktave (unhörbar), aber gleiche Grenzen wie die Hardware |
| K3 | m5_audio.md 3.6, 6.5 | Richtung auf der Xbox: volle HRTF + Crosstalk-DSP | **Nicht nachgebildet.** X3DAudio-Amplitudenpanning mit flacher Kurve, auf konstante Leistung skaliert: Quelle genau vorne = Pegel 1 auf dem lautesten Kanal (Stereo 1/1, also 0 dB je Kanal – **6 dB über** einer mittigen 2D-Stimme gleicher Lautstärke, die wegen der 600 mB Headroom bei 0,5/0,5 = −6 dB liegt), jede andere Richtung gleiche Gesamtleistung, aber seit Phase 5 kein Kanal über 1 (Stereo ganz seitlich 1 auf dem nahen Kanal statt +3 dB, 8.15 R3); bei 5.1 ohne den Mittenkanal (8.15 R2). Die Vorne/Hinten-Leistungsaufteilung der XTLK-Bins (FUN_00249AE5) entfällt; `InnerRadius` 0,5 m (2,5 Einheiten bei `DistanceFactor` 0,2) | Ohne DSP-Emulation nicht nachbaubar; Pegel und Entfernung bleiben exakt DSOUND (K7). Klangunterschied in Stereo: harte Links/Rechts-Trennung statt HRTF-Färbung – Abhören durch den Nutzer |
| K4 | m5_audio.md 3.7, 6.5 | Listener-Vektoren unverändert an X3DAudio | Der Host rechnet die Quelle selbst in Hörer-Koordinaten um (rechts = oben × vorne wie FUN_002497AC). `[audio] positional`: **`corrected` (Standard)** dreht nur Vorne/Hinten um (vorne = Blickrichtung), `xbox` behält die Vertauschung des Originals, `off` spielt alle 3D-Geräusche von vorne (Pegel nach Entfernung bleibt) | Das Spiel übergibt Front = −Blickrichtung (0xCBE85); die Seite stimmt, Vorne/Hinten ist vertauscht. Einfaches Negieren des Front-Vektors hätte auch Links/Rechts vertauscht (Händigkeit); Test „the game's camera vectors“ |
| K5 | m5_audio.md 6.4 | LFE bei 5.1, Stereo ohne LFE | So umgesetzt: Spiel-Mixbin 3 der 3D-Stimmen → LFE-Kanal mit `lVolume + min(0, Entfernung)`; Stereo- und Quad-Ausgabe ohne LFE | Vermutung zum Stereo-Downmix der Xbox (m5_audio.md 10) |
| K6 | m5_audio.md 6.6 | XAudio2-Reverb mit `ReverbConvertI3DL2ToNative` | Gast-Werte erst auf die I3DL2-Bereiche begrenzt (NaN → Generic), dann umgerechnet, WetDryMix 100; `RearDelay` 5 ms außer bei 7.1 (die SDK-Voreinstellung 20 ms ist nur für 7.1 gültig). Hall-Submix mit **einem** Eingangskanal (ein I3DL2-Mixbin wie auf der Xbox), Effekt mono→5.1, Stereo per `ReverbOutputMatrix`. Für den Betrieb ohne Gerät läuft derselbe Hall-XAPO direkt über IXAPO (`SoftwareReverb`) | Der Xbox-DSP-Hall selbst ist nicht nachgebildet; sein absoluter Pegel ist unbekannt (m5_audio.md 3.6) → Entwickler-Schlüssel `[audio] trim_3d` (dB, auf Direktanteil, LFE und Hall-Send aller 3D-Stimmen). Die Engine (DSOUND-Core) entscheidet über die tatsächliche Verdrahtung |
| K7 | m5_audio.md 3.6 | Pegel exakt nach DSOUND | Entfernung exakt FUN_00249893, mB **abgeschnitten** (cvttss2si 0x24E2B4), Register `(att·64)/100` in 1/64 dB, höchstens 0xFFF = 63,98 dB: −10000 mB klingt mit −63,98 dB, nicht stumm; 3D-Terme je Bin auf ≤ 0 begrenzt | Korrektur zu `m5_gain_model.out`: dort gerundet (0,9 → −92 mB, 0,8 → −194 mB), DSOUND schneidet ab (−91 / −193 mB). Unterschied ≤ 0,01 dB |
| K8 | avi.md 2.5, Aufgabenstellung („Presets aus dsstdfx.bin“) | Hall-Presets aus `dsstdfx.bin` | Die fünf Umgebungen kommen aus der Tabelle 0x2D6818 im XBE über `SetI3DL2Listener` (gleich den SDK-Presets `XAUDIO2FX_I3DL2_PRESET_*`); `dsstdfx.bin` enthält nur DSP-Code und Zustand von 10 Effekten (Deskriptor bei 0x6110, I3DL2 = Effekt 0 mit 0x5CC60 Byte Verzögerungsspeicher, Crosstalk = Effekt 1). `ParseEffectsImage` prüft das Bild, das das Spiel zur Laufzeit an `DownloadEffectsImage` übergibt | Lader FUN_0024A35F/FUN_0024A0F2; nichts aus der Datei liegt im Repository |
| K9 | `m5_i3dl2.out` | Native Werte der fünf Presets | Konzertsaal: `ReflectionsDelay` 20 statt 19, `ReverbDelay` 29 statt 28 | Das Python-Skript rechnet in double (0,02·1000 = 19,99…), die SDK-Funktion in float (20,0); maßgeblich ist die SDK-Rechnung |
| K10 | – | Doppler | DS3D-Formel in Xbox-Tonhöhen-Einheiten (`DopplerPitch`, Schallgeschwindigkeit 343,5 m/s / `DistanceFactor`), vor der Hardware-Begrenzung addiert | Das Spiel setzt alle Geschwindigkeiten auf 0 (0x2F90BC): immer 0. Die genaue DSOUND-Dopplerrechnung ist nicht nachverfolgt (Vermutung) |
| K11 | – | Kegel (Cone) | Nicht umgesetzt | Kein `SetCone*`-Einstieg wird vom Spiel importiert |


### 8.13 Abweichungen in M5: DSOUND-Kern und Audio-Engine (DSOUND-Kern-Agent)

Stand: M5 Phase 1, 2026-10-12. Dateien `src\host\audio\dsound_*`, `audio_engine.*` (`src\INTERFACES.md` 4.29, Absatz „DSOUND core“); Analyse `analysis/loader/m5_audio.md` 2, 3.3–3.6, 6, 7, 8. Geprüft mit `tests\audio_dsound_tests.cpp` (nur synthetische Signale) und Läufen unter `run\m5-dsound\` (Frontend und Level 1-1 mit `capture_wav`, ohne Gerät und mit XAudio2 bei 2 % Lautstärke). Die Zeilen heißen D1–D12.

| # | Abschnitt | Design / Analyse | Umsetzung in M5 | Begründung / Beleg |
|---|---|---|---|---|
| D1 | 1.1, E10 | Host-Worker dürfen Status-DWORDs schreiben | **Kein** Gastzugriff außerhalb des Gast-Threads: `Process` setzt `E_PENDING` synchron, fertige Pakete schreibt nur `DirectSoundDoWork` (Größe, Status 0, Event oder Callback), `FlushEx`/`Flush`/letztes `Release` schreiben `E_ABORT` im Aufruf selbst. Der Mixer-Thread meldet nur „Puffer k fertig“ in Host-Speicher | Kein Rennen mit `Release` (das Spiel gibt Paketpuffer und Status sofort danach frei, 0xCC107 → 0xCC121); Kosten höchstens ein Tick Rückmeldung bei 3 s (Musik) Warteschlange. Test „Release … never writes afterwards“ (Wächtermuster) |
| D2 | m5_audio.md 6.2 | Voices je (Kanäle, Rate) poolen | Eine Source-Voice je DSOUND-Objekt, ohne Pool | Gemessen in Level 1-1: 87 Voices in 40 s, höchstens 29 gleichzeitig – kein Engpass |
| D3 | m5_audio.md 6.6 | Mastering −6 dB + `FXMasteringLimiter` | Kein −6-dB-Vorhalt; eigener Spitzenbegrenzer bei −0,1 dBFS (sofortiger Angriff, 80 ms Rückkehr, darunter wirkungslos) **hinter** dem Master-Abgriff, danach `[audio] master_volume` – beides im Tap-XAPO der Mastering-Voice | Gemessen: `FXMasteringLimiter` hebt leise Signale um bis zu +25 dB an (Lautheits-Ausgleich) und die eigene Lautstärke der Mastering-Voice wirkt **vor** ihrer Effektkette. So bleiben Pegel exakt DSOUND und `capture_wav` enthält die Mischung vor Begrenzer und Gesamtlautstärke (Prüfung R9 „vor dem Limiter“) |
| D4 | m5_audio.md 6.8 | Software-Mix ohne Gerät „ohne Hall (oder einfacher Hall)“, Rückwechsel „im nächsten DoWork“ | Software-Mixer mit demselben Graphen, 16-Tap-Fenster-Sinc (70 dB Rauschabstand bei 1 kHz) und demselben XAudio2-Hall (`reverb::SoftwareReverb`); Wechsel in beide Richtungen in `DoWork` (`OnCriticalError` bzw. Endpunkt-Prüfung alle 3 s). Nach einem Wechsel beginnen laufende Puffer wieder an ihrer Startposition (überholt: seit der Integration laufen sie an ihrer Abspielposition weiter, 8.14 I2); ändert sich die Kanalzahl, baut DSOUND alle Matrizen neu | Messung ohne Gerät: Frontend-Musik −6,00 dB (L/R ±0,0001), Tempo = Xbox-Tonhöhe (−4597 → 0,999963), lokaler Rauschabstand < 4 kHz 50–54 dB. Test „device lost -> software mixer -> device back“ (Streams laufen weiter) |
| D5 | m5_audio.md 6.1 | Hall-Submix stereo/5.1 | Hall-Submix mit **einem** Eingang (K6 des Hall-Agenten), Hall-Effekt mono→1/6 Kanäle, `ReverbOutputMatrix` auf die Ausgabe; der Hall-Abgriff (Stem „reverb“) liefert den Effektausgang mit 1 oder 6 Kanälen | Gleiche Verdrahtung mit und ohne Gerät (Software-Hall) |
| D6 | m5_audio.md 2.1 (`apply` verzögert) | Verzögerte 3D-Werte wirken erst bei `CommitDeferredSettings` | Eine **startende** 3D-Stimme (`Play`, `Pause(0)`) übernimmt ihre ausstehenden Werte sofort (überholt: seit Phase 5 startet jede Stimme erst beim Commit des Ticks mit allen Werten, 8.15 R1) | Das Spiel setzt Position/Entfernungen verzögert direkt vor `Play` (0xCC6B9..0xCC72E) und committet wenige Mikrosekunden später; die APU spielt so kurz mit den Standardwerten, der 10-ms-Takt hier bis zu 10 ms. Gemessen ohne die Übernahme: Spitze +6,5 dBFS beim Levelstart, mit: −2,1 dBFS |
| D7 | m5_audio.md 5.6 | `FlushEx(…, ASYNC)`: `PLAYING` fällt spätestens im nächsten `DoWork` | Sofort im Aufruf (Pakete `E_ABORT`, Callbacks nach dem DSOUND-Lock) | Einfacher, erlaubt nach E10 (Gast-Thread); XMV wartet nur auf das Fallen von `PLAYING` |
| D8 | m5_audio.md 6.2 | `GetStatus` aus `GetState().BuffersQueued` | Aus dem Host-Zustand und `OnBufferEnd` der aktuellen Übergabe (Generationszähler); das Ende ist ohne Tick-Verzögerung sichtbar, verspätete Meldungen geleerter Übergaben zählen nicht | XAudio2 meldet nach `FlushSourceBuffers` noch `OnBufferEnd` für geleerte Puffer; Falle 3 („sofort 0“) |
| D9 | m5_audio.md 3.6 | Mixbins 2D | Xbox-Bins → Lautsprecher: vorne L/R → FL/FR, Mitte → FC sonst L+R −3 dB, LFE nur mit LFE-Kanal, hinten → BL/BR (sonst SL/SR, sonst vorne), XTLK 6..9 wie 0/1/4/5, Bin 10 → Hall | **Vermutung** (Stereo-Downmix der Xbox nicht verfolgt); das Spiel benutzt für 2D nur die Bins 0 und 1 |
| D10 | m5_audio.md 6.1 | Kategorie aus der Probe, XMV → „Film“ | `probe:Game::SoundVoiceCreateDsObject` (0xCC150, 6 Byte `mov eax,fs:[0]`, einziger Aufrufer 0xCC621, kein Sprung in die gestohlenen Bytes) | Unmarkierte Objekte (XMV, M6) laufen über den Submix „movie“; ihre Lautstärke setzt das Spiel selbst (Kategorie 0), der Submix ist nur Messpunkt |
| D11 | 2.1, M2 | `[audio] dummy_streams` | Ohne Wirkung, wird weiter angenommen (eine Protokollzeile) | Alte INI-Dateien bleiben gültig |
| D12 | m5_audio.md 5 | XMV-Callback-Streams, `Pause(2)`/`SynchPlayback`, `FlushEx`, `Discontinuity` | Umgesetzt und mit synthetischen Streams getestet; im Spiel noch nicht durchlaufen (XMV ist bis M6 ein Stub ohne Tonspuren) | M6 kann den Original-XMV-Pfad direkt benutzen |

### 8.14 Abweichungen in M5: Integration und Prüfung (Phase 3)

Stand: M5 Phase 3, 2026-10-12. Die drei Teile (DSOUND-Kern/Engine, Codec/3D/Hall, Mitschnitt/Werkzeuge/Launcher) ließen sich ohne Konflikte zusammenführen; die Empfehlungen aus Phase 1 (Engine-`SetReverb` über `reverb::ToNative`, Hall im Software-Mixer, die beiden clang-Warnungen in `audio_engine.cpp`) waren beim Integrationsstart schon umgesetzt. Bauten: MSVC Debug `build\m5-integ`, RelWithDebInfo `build\m5-integ-rel`, clang-cl Debug `build\m5-integ-clang`, alle `FUT_WERROR=ON`, ohne Warnungen; Launcher Debug/Release mit `LAUNCHER_WERROR=ON`, ctest 4/4; `fut_tests` 427/427 in allen drei Bauten (dazu die beiden optionalen Fälle mit `FUT_AUDIO_CODEC_REFS`/`FUT_AUDIO_DSSTDFX`: 99/99 Prüfungen). Läufe und Mitschnitte nur unter `run\m5-integ\`. Die Zeilen heißen I1–I9.

| # | Abschnitt | Design / Analyse / Phase 1 | Umsetzung in der Integration | Begründung / Beleg |
|---|---|---|---|---|
| I1 | m5_audio.md 6.4, 8.12 K5 | LFE der 3D-Stimmen = `lVolume + min(0, Entfernung)` ungefiltert | Der LFE-Kanal der Ausgabe läuft vor dem Master-Abgriff durch einen Tiefpass 4. Ordnung (Butterworth, 120 Hz) in der Master-Stufe (XAudio2 und Software-Mixer); Stems bleiben ungefiltert | Die Xbox gibt den LFE-Mixbin nur über den Dolby-Digital-Encoder aus, und AC-3 überträgt LFE nur bis 120 Hz. Gemessen vorher: LFE war im Level-Lauf der lauteste Kanal des Effekt-Stems (−36,8 dBFS gegen −38,6 Mitte), jeder Schritt lag vollbandig auf dem Subwoofer bzw. im Downmix des Fernsehers doppelt. Test „LFE channel is low-passed at 120 Hz“ (40 Hz > −0,5 dB, 120 Hz −3 dB, 1 kHz < −60 dB, Mitte unverändert) |
| I2 | 8.13 D4 | Nach einem Gerätewechsel beginnen laufende Puffer wieder an ihrer Startposition | Der gerade spielende Puffer jeder Stimme läuft an seiner Position weiter (abgespielte Frames seit dem letzten Anhängen/Flush minus die Frames der inzwischen beendeten Puffer; ein schon zu Ende gespielter Einmal-Puffer startet am letzten Frame, damit sein Ende noch gemeldet wird), die wartenden folgen | Vorher wiederholte z. B. die Musik bis zu einem Paket (0,3 s), ein Sprachpuffer von vorn. Gemessen mit `device_test = loss` (6 Wechsel in 146 s): Frontend-Musik nach jedem Wechsel auf ±0,08 ms (±4 Samples) an der richtigen Stelle, nur ein 50-ms-Fenster je Wechsel mit NCC 0,90–0,95. Test „a backend switch continues the playing buffers at their position“ |
| I3 | m5_audio.md 9.2 R10, Aufgabenstellung („ohne Gerät testen“) | Endpunkt in Windows deaktivieren | Entwicklerschlüssel `[audio] device_test = off \| absent \| loss`: `absent` verhält sich wie ein Rechner ohne Ausgabegerät (kein XAudio2, keine Endpunkt-Abfrage), `loss` meldet 20 s nach jedem XAudio2-Start einen Geräteverlust (`XAUDIO2_E_DEVICE_INVALIDATED`), nach der 3-s-Abfrage kommt das Gerät zurück (seit Phase 5 über den Geräte-Thread, 8.15 R6/R7) | Das Abschalten eines Endpunkts ist eine Systemeinstellung, die der Agent nicht ändert; ab `Pump` ist der Weg derselbe wie bei echtem Verlust. Ein **echter** Verlust trat im 33-min-Dauerlauf auf (Fernseher ging bei 1917 s aus, `0x80070490`): Wechsel auf den Software-Mixer, Spiel lief bis zum Skriptende weiter |
| I4 | 8.13 D4 | Kein Ausgabegerät / Geräteverlust als Warnung | Als INFO protokolliert (wie „window: display off“) | Ein fehlendes Gerät ist ein Zustand des Rechners, kein Emulationsfehler; die Prüfungen m2/m3 verlangen ein Protokoll ohne WARN und scheiterten bei ausgeschaltetem Fernseher allein daran |
| I5 | 8.13 D4 | Software-Mixer nach `Flush`: nächster Puffer „direkt hinter der Leseposition“ | Der nächste Puffer beginnt **an** der Leseposition (aufgerundet) | Vorher wurde nach `Stop`/`Flush`/`Play` ein Frame des alten Puffers ausgegeben (im Test sichtbar: 150 statt 0) |
| I6 | 4.30 (Ereignisliste) | Detail `voice= mB= hz= pos= frame=` | Zusätzlich `level=` (zuletzt angewandter Pegel: 3D `lVolume` + DSOUND-Entfernungsterm, ≤ −100 = stumm; 2D `lVolume` − 600 mB) und für 3D `dist= min= max= curve=`; neue Ereignisart `level` (`SoundEventKind::Level3D`) bei Änderung um ≥ 3 dB oder in/aus dem stummen Bereich | Diagnose ohne Ohren: belegt I7 |
| I7 | Phase 1 (Werkzeug-Agent): „viele 3D-Loops im Level werden nicht gefunden“ | – | **Keine Abweichung, kein Fehler.** Alle nicht gefundenen Loops (`SteamLoop*`, `Cable_loop`, `FanSpinning`, `ElecBox-Loop`, `Heart`, `Hermes-IdleMoan_Loop`, `Box-Idle_loop`, Eule) lagen 45–470 Einheiten vom Hörer (der Kamera) entfernt, ihr `MaxDistance` ist 15–50 bei gesetztem Stumm-Flag 0x20000: DSOUND dämpft sie auf das Register-Minimum −63,98 dB. Schritte und Sprünge bei 20 Einheiten (Min 5/Max 80) kommen mit −0,2 dB | Ereignisliste des Level-Laufs; im Dauerlauf werden `ElecBox-Loop` und die Eule hörbar, sobald Fry nahe genug ist (1790 bzw. 1609 `level`-Ereignisse über −60 dB) |
| I8 | Phase 1: „Musik 40 ppm zu langsam“ | – | **Keine Abweichung:** Tempo 0,999959 = Xbox-Tonhöhenquantisierung (22050 Hz → Tonhöhe `round(4096·log2(22050/48000))` = −4597 → 22049,10 Hz), so auf der Xbox; 4,4 ms auf 110 s, unhörbar | 8.12 K2 |
| I9 | `tools\m5_check.py` A7 | – | Im Lauf mit `device_test = loss` meldet A7 einen „verlorenen“ 2-s-Abschnitt: Die Verschiebung um 4 Samples mitten im Abschnitt senkt seine NCC unter 0,8. Feinanalyse (50-ms-Fenster) zeigt die Musik an der richtigen Stelle (I2). Wird als Prüfgranularität hingenommen; ohne künstliche Wechsel besteht A7 | `run\m5-integ\loss\m5_check` |

**Messungen** (XAudio2 auf dem 6-Kanal-Endpunkt des Fernsehers bei `master_volume = 2`, solange er an war; danach Software-Mixer):

- `m5_check` RelWithDebInfo mit Gerät: **ALL PASS** (Frontend und Level, dazu M4-1..M4-9). Frontend-Musik 64/64 Abschnitte, NCC 0,971, Loop-Lücke 103,0 ms (erwartet 102,9), Pegel −6,29/−6,49 dB (Modell −6,0; Messartefakt der Prüfung, mit der Auswertung aus Phase 5 −6,01/−6,03 dB unter 2 kHz, 8.15 R5), 6/6 Menüklänge bei −6,0 ± 0,2 dB, Tonhöhe 1,0000; Level: 16–20 Ereignisklänge gefunden (Schritte, Sprünge 3D bei ≈ 0 dB, Sprache/Menü 2D bei −6 dB), Planet-Express-Musik NCC 0,988, 0 Aussetzer, 0 übersteuerte Samples, Glitches 0, höchstens 30 gleichzeitige Stimmen.
- Ohne Gerät: `[audio] enabled = 0` und `device_test = absent` jeweils **ALL PASS** mit `--device-optional` (Software-Mixer stereo; Musik −6,21/−6,36 dB, also innerhalb 0,15 dB des XAudio2-Laufs, R10; neu ausgewertet −6,00/−6,00 dB unter 2 kHz, 8.15 R5).
- `device_test = loss`: Level **PASS**, Frontend alle Kriterien außer A7 (I9); 6 bzw. 1 Wechsel ohne Absturz, ohne verlorene Pakete.
- Dauerlauf 33 min (Level 1-1, Laufen/Springen/Angreifen/Kamera/Pausenmenü, XAudio2 stereo): Ende über `quit 0`; 10 300 Stimmen erzeugt, 20 am Ende, Spitze 33; 15 963 `Play`, 0 verwaiste Objekte; Musik-Stream 5315 Pakete, 0 Unterläufe, 0 abgewiesen; Glitches 0; Speicherpool stabil (Spitze 18 MB). Mischung vor dem Begrenzer: 869 Samples ≥ 0 dBFS (0,00045 %, Spitze +3,8 dBFS bei vielen gleichzeitigen 3D-Effekten) – dafür ist der Begrenzer da (die Begrenzer-Statistik ging beim Gerätewechsel verloren; seitdem summiert die Engine sie über alle Backends).

**Offen für das Abhören:** Pegelverhältnis 2D (−6 dB) zu 3D (≈ 0 dB nahe dem Hörer) und Hallpegel gegenüber der Xbox (unbekannter APU-Headroom, 8.12 K6), X3DAudio-Panning statt HRTF, `positional = corrected`, Wirkung des LFE-Tiefpasses auf dem Fernseher.

### 8.15 Abweichungen in M5: Review-Befunde (Phase 5)

Stand: M5 Phase 5, 2026-10-12. 17 Befunde aus zwei Reviews (Audio-Treue, Robustheit), jeder zuerst nachgeprüft. Bauten: MSVC Debug `build\m5-final`, RelWithDebInfo `build\m5-final-rel`, clang-cl Debug `build\m5-final-clang` (alle `FUT_WERROR=ON`, 0 Warnungen), Launcher `build\m5-final-launcher(-rel)` (`LAUNCHER_WERROR=ON`, ctest 4/4); `fut_tests` 433/433 in allen drei Bauten. Läufe nur unter `run\m5-final\`. Der Fernseher (LG, 6-Kanal-Endpunkt 0x60F, 48 kHz) war während der ganzen Endprüfung eingeschaltet, alle XAudio2-Kriterien liefen also auf dem echten Gerät. Die Zeilen heißen R1–R17, dazu zwei Nachträge der Endprüfung (R2a, R9a).

| # | Befund (Schwere) | Ergebnis der Prüfung | Umsetzung | Beleg |
|---|---|---|---|---|
| R1 | Einsatz eines Sounds für 10–20 ms mit Standardwerten (mittel) | **Bestätigt.** `SoundVoice::vf11` ruft `Play` (0xCC72E) bzw. `Pause(0)` (0xCC711) vor `SetFrequency`/`SetVolume`/`SetMixBins`/`SetI3DL2Source`, der Commit folgt erst bei 0xCBE95; die Engine-Stimme lief aber sofort | `Play`, `Pause(0)` und die Stream-Erzeugung setzen nur den Host-Zustand (`GetStatus` sofort 1/5 bzw. PLAYING) und `startPending`; die Stimme startet in `ApplyAllDirty` (Commit bzw. `DoWork`) im selben Engine-Batch nach den Werten des Ticks. `SynchPlayback` (XMV) startet weiter sofort. Ersetzt 8.13 D6 | Effektregler 0,25, 62 Starts von `scroll.wav`: alle ab dem ersten 5-ms-Fenster bei −21,01 dB (vorher die ersten 20 ms bei −6,0 dB, also +15 dB). Neuer Unit-Test |
| R2 | 3D-Stimmen auf 5.1 überwiegend im Center (niedrig) | **Bestätigt** (die 3D-Mixbins {6,8,7,9,10,3} haben keinen Center-Bin) | X3DAudio pannt nur noch über die vier Ecken (bzw. Stereo), der Host bildet die Koeffizienten auf die Ausgabekanäle ab; LFE wie bisher aus dem DSOUND-LFE-Pegel | Schritte vorne jetzt FL/FR je ≈ 0 dB, FC −200 dB (vorher FC −0,2 dB, FL/FR −17..−21 dB); siehe R2a |
| R2a | Nachtrag: die erste Umsetzung von R2 legte auf dem Fernseher **jede** 3D-Stimme nach vorne links | Gefunden in der Endprüfung am echten Endpunkt (0x60F, Seiten- statt Rücklautsprecher): X3DAudio 2.9 liefert für nicht standardisierte Masken (0x60B = 0x60F ohne Center, auch 0x603) für **jede** Richtung 1 auf Kanal 0 und 0 sonst (gemessen) | Nur noch Standardmasken an X3DAudio: Quad (FL FR BL BR) mit Abbildung der hinteren Ecken auf BL/BR, sonst SL/SR; Stereo; sonst die Ausgabemaske unverändert. Unit-Test prüft jetzt 0x3F **und** 0x60F; `m5_check` A9 verlangt Klang auf beiden Seiten (vorher bestand A9 mit „alles links“, weil die Spannweite gegen die stummen Fenster der anderen Seite gemessen wurde) | Effekt-Stem vorher: FL und LFE gleich, FR/FC/hinten −200 dB; jetzt Level-Lauf A9: 16 % der lauten Fenster links, 10 % rechts lauter, Schritte vorne FL/FR −0,3..−1,3 / −0,0..−2,2 dB |
| R3 | Seitliche 3D-Quelle in Stereo +3 dB über einem Xbox-Mixbin (niedrig) | **Bestätigt** (Leistungsnormierung auf die Front: seitlich √2 auf dem nahen Kanal) | Skalierung `min(√(P_front/P), 1/Spitze)`: kein Kanal über der Front-Referenz (0 dB); K3 korrigiert (3D vorne 0/0 dB gegenüber 2D −6/−6 dB bei gleichem `lVolume`) | Unit-Test: Stereo-Kreis, naher Kanal in jeder Richtung = Direktpegel, Gesamtleistung ≤ Front |
| R4 | „Bitgleich“ beim ADPCM-Dekoder nur gegen ffmpeg belegt (niedrig) | **Bestätigt** (Formulierung) | Kein Code geändert: `audio_api.h`, `codec_adpcm.h` und `src\INTERFACES.md` sagen jetzt „bitgleich mit ffmpeg `adpcm_ima_xbox`; Rundung der Xbox-Hardware unbelegt (Shift-Add-Form höchstens 28 LSB anders, unter −61 dBFS)“ | `m5_adpcm_check.out` |
| R5 | Musikpegel in A11 um 0,2–0,5 dB nach unten verzerrt (niedrig) | **Bestätigt** (ganzzahliger Versatz, Drift ≈ 4 Samples je 2-s-Abschnitt) | `audio_compare.py`: jeder Abschnitt mit der gemessenen Geschwindigkeit nachgesampelt und auf Bruchteile eines Samples ausgerichtet (`stretch_sinc`, `aligned_gains`); A11 bewertet den Pegel unter 2 kHz mit ±0,1 dB (Menüklänge ±0,3 dB), druckt den Vollbandpegel mit | XAudio2: unter 2 kHz −6,01/−6,03 dB, Vollband −6,13/−6,21 dB (der Rest ist die Höhendämpfung des XAudio2-Resamplers); Software-Mixer −6,00/−6,00 bzw. −6,01/−6,01 dB. Die Zahlen in 8.14 (−6,29/−6,49) waren Messartefakte |
| R6 | Backend-Wechsel synchron im Gast-Thread (mittel) | **Bestätigt** (`XaBackend::Create`, Voice-Abbau und Join in `DoWork`) | Host-Thread „audio device worker“ (E10: kein Gastcode) sucht Endpunkte, erzeugt das neue XAudio2-Backend und baut ersetzte ab; `Pump` übergibt nur noch. Nach gescheitertem Öffnen eines vorhandenen Geräts Abstand 3, 6, 12 … 30 s, eine WARN je Lauf | `device_test = loss`, 6 Wechsel im Frontend: längste Frame-Arbeit 7,8 ms, 0 Frames über einer Pacing-Periode (vorher 30,7 ms und 5 Frames); Level-Lauf gleich wie ohne Wechsel (2 lange Frames beim Laden) |
| R7 | Gerätewechsel nur über `OnCriticalError` und 3-s-Abfrage (mittel) | **Bestätigt** | `IMMNotificationClient` im Geräte-Thread (zählt nur), `Pump` reagiert sofort; neues Windows-Standardgerät → Wechsel, ein zurückkehrendes `[audio] device` wird wieder genommen | Nur der Codepfad und die simulierten Wechsel sind geprüft: Standardgerät umschalten und Headset ab-/anstecken sind Systemeinstellungen bzw. Handgriffe, die der Agent nicht ausführt → Nutzertest (m5_report 9) |
| R8 | `[audio] device` nie mit echter ID geprüft (mittel) | **Teilweise bestätigt**: das ID-Format war ungeprüft, funktioniert aber | Kein Umwandeln nötig: XAudio2 2.9 nimmt die `IMMDevice::GetId`-Zeichenkette an. Die INFO-Zeile nennt jetzt den Namen des tatsächlich geöffneten Geräts (`device from [audio] device '<Name>'`); eine fehlende ID fällt auf das Standardgerät zurück | Unit-Test öffnet jeden aktiven Endpunkt über seine ID („device from [audio] device 'LG TV SSCR2 …'“) und eine unbekannte ID („default device“) |
| R9 | `FramesPlayed` nicht monoton nach `Discontinuity`, Flush-Basis zu früh (niedrig) | **Bestätigt** | Monotoner Zähler je XAudio2-Stimme, Flush-Basis erst beim nächsten `Submit` | siehe R9a |
| R9a | Nachtrag: der neue XAudio2-Test „FramesPlayed stays monotonic over Discontinuity“ schlug fehl (0 statt 4800) | Gemessen: In den Callbacks eines Durchlaufs meldet `GetState` den Stand vom **Anfang** des Durchlaufs, der Rücksprung auf 0 ist ab `OnVoiceProcessingPassEnd` sichtbar | Die Basis wächst in `OnVoiceProcessingPassEnd` um den Wert bei `OnStreamEnd`; Lesen springt nie zurück. Nach einem Stream-Ende höchstens ein Durchlauf (480 Frames, 10 ms) zu wenig | Test 5/5 Wiederholungen grün (4320..4800) |
| R10 | Launcher friert ein, wenn während des Testtons „Geräte neu suchen“ gedrückt wird (niedrig) | **Bestätigt** | Abbruch-Flag im `TonePlayer` (Destruktor stoppt den Ton sofort), Knopf während des Tons gesperrt | `tst_ui` |
| R11 | Texte des Audio-Reiters passen nicht zum Spiel (niedrig) | **Bestätigt** (a–d) | (a) „läuft stumm weiter und schaltet den Ton ein, sobald ein Gerät verfügbar ist“; (b) nicht angeschlossenes Gerät → angezeigt wird das Standardgerät, auf das das Spiel ausweicht; (c) Testton weicht wie das Spiel aus und meldet das; (d) Hinweis „Windows mischt herunter“, wenn mehr Lautsprecher gewählt sind als das Gerät hat | Launcher-Tests, Schema-Hilfe `[audio] device` |
| R12 | Endpunkte mit mehr als 8 Kanälen ohne Limiter/Hall/Mitschnitt (niedrig) | **Plausibel** (nicht reproduzierbar, kein solches Gerät) | Mastering-Voice dann mit 8 Kanälen neu angelegt (XAudio2/WASAPI passen an den Endpunkt an) | Codepfad |
| R13 | Stems allozieren im Echtzeit-Thread (niedrig) | **Bestätigt** | `AudioEngine::TapFormat`; `AttachCapture` deklariert alle Stems vorab | Codepfad; Mitschnitt-Tests (Software-Engine) |
| R14 | „audio capture taps: 0 process calls“ irreführend (niedrig) | **Bestätigt** | Zeile nur noch, wenn ein `CreateTap`-XAPO benutzt wurde | Logs der Endprüfung ohne die Zeile |
| R15 | Software-Mixer rechnet ohne Zuhörer den vollen Mix (niedrig) | **Bestätigt** (3,6 % eines Kerns) | Ohne Tap und ohne Ausgabepuffer nur Positionen, `Pull` und Pufferenden (gleiche Frames, gleiche `OnBufferEnd`-Zeitpunkte); mit `capture_wav` weiter voller Mix | Unit-Test „without a listener only positions advance“; `m4_check` mit `[audio] enabled = 0` (ohne Mitschnitt) siehe m5_report 7 |
| R16 | Limiter im Spiel nie aktiv gewesen (niedrig) | **Bestätigt** (alle Läufe bei 2 % Lautstärke) | Neues Kriterium A12 (Limiter-Statistik gegen Spitze vor dem Begrenzer und `master_volume`); Spiellauf mit 100 % und `trim_3d = 10` | Spitze vor dem Begrenzer +5,16 dBFS, Begrenzer auf 178 286 Frames aktiv, tiefste Absenkung 5,30 dB (erwartet 5,26 ± 0,15), A12 PASS; bei 100 % ohne Trim −2,58 dBFS → „nicht nötig“ |
| R17 | `LogEngineSummary` liest `backend_` ohne Facade-Lock (niedrig) | **Bestätigt** | Zusammenfassung nur unter dem Facade-Lock (`try_lock`, sonst ausgelassen) | Codepfad |

Abgelehnt wurde kein Befund. Ein Lauf der vorigen Phase-5-Sitzung mit 100 % Lautstärke endete einmal mit Rückgabecode −1 ohne Absturzbericht mitten im Level; zwei Wiederholungen mit dem Endstand (100 % sowie 100 % mit `trim_3d = 10`) und alle anderen Läufe liefen ohne Befund durch. Ursache nicht gefunden, vermutlich von außen beendet (die Sitzung brach kurz danach ab).

### 8.16 Abweichungen in M6: Video (XMV-HLE)

Stand: M6, 2026-10-12. Bauten: MSVC RelWithDebInfo `build\m6`, Debug `build\m6-debug` und clang-cl Debug `build\m6-clang` (beide `FUT_WERROR=ON`, 0 Warnungen); `fut_tests` 457/457 in allen drei Bauten. Läufe unter `run\m6\` (Testmodus, `boot`, `level`, `level_original`, `attract`, `m4script`). Bericht: [`m6_report.md`](m6_report.md).

| # | Plan (Abschnitt) | Abweichung | Grund / Beleg |
|---|---|---|---|
| V1 | M6 Umfang, R10, 2.1 XMV-Zeile: XMV-HLE **mit FFmpeg** (avformat `xmv`, avcodec `wmv2`/`adpcm_ima_xbox`), dynamisch gelinkt | **Kein FFmpeg.** Eigener XMV-Demuxer (`video\xmv_demux.*`, Layout [AVI] 3.3 bzw. libavformat/xmv.c: Paketkopf, 4-Byte-Extradaten mit Umsetzung in den WMV2-Sequenzkopf, Wörter little-endian → big-endian, `video_size − 4 × Spuren`), WMV2 über den **in Windows eingebauten „WMVideo Decoder DMO“** (wmvdecod.dll, `DllGetClassObject` + `IMediaObject`, Ausgabe I420), Ton über den M5-ADPCM-Dekoder | Entscheidung der Hauptsitzung (keine Downloads, keine LGPL-DLLs). Machbarkeit vorab mit einem 2-KB-Testprogramm geprüft (45/45 Bilder, Luma bitgleich zu FFmpeg im ersten Bild). Testmodus: 47/47 Dateien, 39 602 Bilder, 0 Fehler; Ton aller 195 Spuren bitgleich mit FFmpeg. Bild **nicht bitgleich** mit FFmpegs wmv2 (Microsofts eigener Decoder rundet anders): Luma 3 % der Bilder bitgleich, PSNR min 61,0 / Mittel 76,1 dB, Chroma min 35,6 / Mittel 65,1 dB (235 Bilder) |
| V2 | M6 Umfang: AVIO über die Paket-Callbacks | Kein AVIO-Kontext: `GamePacketSource` ruft `GetNextPacket` 0xEDBA0, kopiert das Paket sofort und startet mit `ReleasePreviousPacket` 0xEDE60 das Lesen des nächsten; gelesen wird nur aus `GetNextFrame` (Gast-Thread, wie das Original) | Das Paket bleibt nur bis zum zweiten `ReleasePreviousPacket` gültig (Puffertausch 0xEDBA0..0xEDBCC, 0xEE3B3..0xEE3CE). `Create` liest nichts: Open legt die Puffer erst danach an (0xEE395) |
| V3 | [AVI] 3.1: Original-Stream mit 2 Paketen und Callback 0x264775 | Host-DSOUND-Stream mit **32 Paketen ohne Callback**; Abschluss über die Status-DWORDs; `Pause(2)` beim Aktivieren, Start mit `DirectSoundCreate` + `SynchPlayback` + `Release` beim ersten Bild; `DirectSoundDoWork` in jedem `GetNextFrame`; Vorlauf 0,6 s beim Start, danach bis 1,2 s | Mehr Vorlauf gegen Ruckler des Gast-Threads (Laden während der Logos); dieselben DSOUND-Wege wie das Original (`SetVolume`/`Release` des Spiels landen im HLE-Stream) |
| V4 | M6: „Takt strikt nach pts“, [AVI] 3.5 „Audio-Uhr bzw. QPC“ | Uhr = vom Stream gespielte Frames (`snd::StreamFramesPlayed`, neu, additiv) / Rate; Wanduhr (QPC) ohne Tonspur, nach dem Tonende und wenn der Zähler 1 s steht. Ton gilt als zu Ende, wenn alles abgegeben ist und höchstens 100 ms fehlen oder alle Pakete fertig sind | Der Resampler einer 22-kHz-Stimme meldet die letzten ≈ 200 Frames nie als gespielt (fox: 455 976 von 456 192): ohne Toleranz endete fox erst nach der 1-s-Stall-Erkennung mit Warnung (Lauf der Hauptsitzung). Gemessen: Bild höchstens 29 ms nach seinem pts, Tondrift 0,1–0,3 ms über 3–9 s, SCI (25 fps) endet bei 12 279 ms (Datei 12 276 ms) |
| V5 | [AVI] 3.5: Renderer mit YUY2 + `YuvEnable` (Shader oder `DXGI_FORMAT_YUY2`) | YUY2 → BGRA8 **auf der CPU beim Hochladen** (`tex::ConvertYuy2ToBgra`, BT.601 begrenzter Bereich), eine Host-Textur je Shim-Ressource, neu hochgeladen bei neuer Generation (`d3d::NoteResourceWritten`); `YuvEnable` beim Zeichnen wählt BT.601 oder (ANNAHME) Rohkomponenten R = Cr, G = Y, B = Cb | 15–25 Bilder/s à 640×480 kosten unter 1 ms; kein neuer Shader. Der Shim-Speicher bleibt die Wahrheit wie im Original (FUN_00265B0B schreibt per LockRect) |
| V6 | – (nicht vorhergesehen) | **Texel-Adressierung:** Das Filmviereck hat UVs 0..640 × 0..480 (Open 0xEE5A1..0xEE647 aus +0x48/+0x4C); lineare Xbox-Texturen werden auf dem NV2A in Texeln adressiert. Stufen mit einer Filmtextur bekommen vor `ff.Draw` die Skalierung 1/W, 1/H und Clamp | Ohne Skalierung zeigte der erste Lauf eine einfarbige Fläche (Texel 0,0); mit Wrap mischte der bilineare Filter die erste und letzte Spalte (Intro-Bild, linker Rand türkis) |
| V7 | [AVI] 3.2: Ende = Ergebnis 2 | Ergebnis 2, wenn alle Bilder gezeigt sind und die Uhr max(letztes pts + letzter Abstand, Tonende) erreicht hat; Ergebnis 3 bei fehlendem Decoder oder kaputter Datei (der Film endet, das Spiel läuft weiter) | Die Header-Dauer passt nicht immer zu Bild und Ton (2-1-03: 3287 ms, Ton 2969 ms, letztes Bild 2933 ms) |
| V8 | m4_input.md 4.4: Überspringen nur nach 2,5 s bzw. ab dem zweiten Ansehen von 1-1-01/02 | **Nutzerwunsch:** `[game] skip_videos = always \| original` (Standard `always`). Probe-Hook `probe:Game::CutsceneSkipAllowed` auf FUN_000D3120 (einziger Aufruf 0x2ABD4, Prolog `A0 D4 B7 2E 00`): bei `always` immer TRUE, sonst das Ergebnis des Originals | Statt das Cheat-Flag 0x2EB7D4 zu setzen (einziger Leser FUN_000D3120, aber der Cheat 0x1C in FUN_0002C530 würde es zurückschalten). Intro/Logos (`Intro::Update`) und Attract sind schon im Original immer abbrechbar. Belegt: Lauf `level` (1-1-01 beim ersten Ansehen nach 9 s übersprungen, Protokoll „original no … -> skip“, danach steuerbar), Lauf `level_original` („-> keep playing“) |
| V9 | – | `[debug] movies = play \| skip` (Entwickler): `skip` beendet jeden Film sofort wie der M2–M5-Stub; m2/m3/m3c/m3d-Prüfungen laufen mit `--set=debug.movies=skip`. Die Eingabeskripte drücken bis zum Frontend START | Mit echten Filmen dauert der Weg ins Frontend ≈ 50 s länger: Zeitlinien mit festen Bildnummern (Intro → Frontend ≈ 15 s) und `wait context Frontend timeout 60s` liefen sonst ab (Hinweis der Hauptsitzung). `m4_newgame.txt` mit echten Filmen: Exit 0 nach 38 s |
| V10 | M6 Exit 3: „Testmodus dekodiert alle 47 Dateien“ | Werkzeugmodus `futurama.exe --decode-movies=<Abbild\|Ordner> --result=<json> [--movie-dump=<dir>]` (ohne Spiel, 34 s für alle 47); JSON mit pts und CRC-32 je Luma-Ebene und je Tonspur statt Bilddumps; `tools\m6_check.py` vergleicht mit PyAV/FFmpeg und wertet Spielläufe aus (Spur, Sprache, Drift) | Vollständiger Vergleich ohne Gigabytes an Rohbildern; Dumps nur für 5 Bilder je Datei |
| V11 | [AVI] 3.2 Untertitel über `GetPlaybackTimeMs` | Rückgabe = Filmuhr; nicht im Lauf geprüft | Auf der PAL-Disc gibt es keine `Video/*.txt` ([AVI] 3.2) |
| V12 | – | Bekannt: bei 2-facher interner Auflösung ist die oberste Zeile und linke Spalte des Films schwarz (halber Gastpixel, Halbpixel-Regel aller vortransformierten Draws, GFX_CONTRACT 15) | Nicht mit xemu verglichen (Filmszenen sind erst ab M6 vergleichbar) |
| V13 | 8.13 (DSOUND): Unterlauf-Warnung „ran out of packets (dropout)“ | Nach `Discontinuity` (Datenende) meldet der Stream kein Unterlaufen mehr (`endOfData`, gelöscht vom nächsten `Process`) | Jeder Film endete sonst mit einer WARN-Zeile |

---

### 8.17 Abweichungen in G4: Start-Level und Spielfigur

Stand: G4, 2026-10-12. Bauten: MSVC RelWithDebInfo `build\g4-select`, Debug `build\g4-werr` und clang-cl Debug `build\g4-clang` (beide `FUT_WERROR=ON`, 0 Warnungen); `fut_tests` 467/467 (1 übersprungen), `tools\m4_check.py` ALL PASS, Launcher-Tests 4/4 (`build\g4-launcher`). Bericht `docs\g4_report.md`, Analyse `analysis\loader\g4_select.md`.

| # | Plan (Abschnitt) | Abweichung | Grund / Beleg |
|---|---|---|---|
| G4-1 | 3.2 Hook-Installation: jeder registrierte Hook ist an (`defaultOn`) oder per `[hooks] disable` aus und erscheint im Log | **Bedingte Probes** `FUT_PROBE_IF` (`core::HookDesc::condition`): Ist die Bedingung falsch, wird der Hook still übersprungen – keine Logzeile, nicht in der Summe „hooks: N installed, M disabled, K registered“, Abbild unverändert | Nutzerwunsch: Mit `[game] start_level = original` und `play_as = original` darf das Spiel nicht verändert werden und das Hook-Protokoll muss gleich bleiben. Beleg: Hook-Zeilen eines Laufs ohne die Schlüssel und eines Laufs mit beiden auf `original` sind gleich (182 Zeilen, ohne Host-Adressen), mit `play_as = bender` kommen genau `probe:PlayerDef::Ctor`, `probe:Level::FindDef`, `probe:Level::FindCharDef` dazu |
| G4-2 | 6 Meilensteine: kein Eingriff in Spiellogik oder Spielinhalte | `play_as` tauscht die Spielfigur, indem es beim Laden den Def-Namen der Figur umbenennt (PlayerDef-ctor 0x5E580, `FindDef` 0xC3E80, `FindCharDef` 0xC3F20) und Waffen (`Pickup=`/`Equip=`) aus dem anstehenden Startzustand entfernt; Level 1-6, 2-5, 4-1 und 5-3 behalten ihre Figur | Nutzerwunsch Figurenwahl. Als experimentell gekennzeichnet (Launcher-Text, Hilfe); Standard `original` installiert nichts |

### 8.18 Abweichungen in M7: Spielstände

Stand: M7, 2026-10-12. Bauten: MSVC RelWithDebInfo `build\m7`, Debug `build\m7-debug` und clang-cl Debug `build\m7-clang` (beide `FUT_WERROR=ON`, 0 Warnungen); `fut_tests` 467/467 (1 übersprungen) in allen drei Bauten; `tools\m7_check.py` M7-1 bis M7-8 PASS; `tools\m4_check.py` (`run\verify-m5\futurama.ini`) ALL PASS. Bericht [`m7_report.md`](m7_report.md), Analyse [`m7_saves.md`](../analysis/loader/m7_saves.md).

| # | Plan (Abschnitt) | Abweichung | Grund / Beleg |
|---|---|---|---|
| S1 | M7 Exit 1: „Speichern, Neustart und Laden eines Spielstands“ – gemeint im normalen Spielablauf | Der einzige Speicherpunkt ist das Levelende (Breakdown, m7_saves.md 1.1). Die Prüfläufe erreichen ihn mit der Eingabeskript-Aktion **`game level_end <NächstesLevel>`**, die zum nächsten Tick das Levelskript-Native FID 0xA1 (FsChangeLevel, `FUN_000B0520`) aufruft – dasselbe, was ein Level an seinem Ende tut. Alles danach (Breakdown, Speichermenü, Dialoge, Dateien) ist der Code des Spiels | Ein Level durchzuspielen dauert Minuten und ist mit Eingabeskripten nicht sicher; der Nutzer wünscht kurze Läufe. FsGameSave (FID 0xBC) allein scheidet aus: Nach dem Erfolg schreibt der Dialog `[0x2E8E4C]+0x90` (Breakdown) und stürzt ohne Breakdown ab (Lauf `explore2`: Schreibzugriff auf 0x90 bei 0xA4964). Das Durchspielen eines echten Levelendes prüft der Nutzer (Checkliste im Bericht) |
| S2 | 4.2 Punkt 5 / 4.3 `NtQueryDirectoryFile`: Host-Verzeichnis 1:1 | Beschreibbare Host-Verzeichnisse (partition1, Z:) liefern ihre Einträge **nach Erstellzeit** (bei Gleichstand nach Namen), schreibgeschützte (entpackte Disc) weiter nach Namen; jeder Durchlauf ist ein **Schnappschuss** beim (Neu-)Start | Lade- und Speichermenü zeigen die Sicherungen in Aufzählungsreihenfolge (`FUN_000E1270` sortiert nicht). FATX hängt neue Einträge an (älteste zuerst), NTFS sortiert nach den Hash-Namen. Beobachtet: „U-Bahn …“, „M7 Test 01“, „M7 Test 02“ … Der Schnappschuss bleibt stabil, während XAPI die zurückgegebenen Einträge löscht (`FUN_000F8709`). Unit-Tests `tests\save_tests.cpp` |
| S3 | 4.5 Pflichtwerte: `XLaunchNewImageA` Reason 2/`'U'` → Meldung statt Dashboard; bis M7: Prozessende (Exitcode 4) | Der Hook protokolliert die Anfrage (WARN „asks for the Xbox dashboard … back to the game“), zeigt bei `[debug] msgbox` ein Meldungsfenster mit dem Spielstand-Ordner und gibt `ERROR_NOT_SUPPORTED` zurück; alle anderen Anfragen beenden den Prozess weiter | Einziger Aufrufer `FUN_000E4B00` ignoriert die Rückgabe, `FUN_000DC290` schließt den Dialog; das Spiel geht ins Level wie nach „Weiter“, nur bleibt Speichern erlaubt (ein späterer Fehlschlag meldet das Spiel selbst, `MEMCARD_006`). Wichtig, weil im Dialog `MEMCARD_047` „Steuerung“ (Dashboard) **vorausgewählt** ist. Lauf `check/nospace`: Dialog, Rückkehr, Level 1-1 geladen, Exitcode 0 |
| S4 | – (nicht vorhergesehen) | Neuer Entwicklerschlüssel `[saves] free_space_limit` (Byte, 0 = aus): begrenzt den gemeldeten freien Platz von partition1 (`vfs::CreateHostDirDevice(..., freeSpaceLimit)`) | Der Platzdialog `MEMCARD_047` ist sonst nur mit einer vollen Platte erreichbar. Mit 65536 Byte: „8 zusätzliche Blöcke“ (192 KiB nötig) |
| S5 | 5.4 Logging | Sieben Beobachter-Proben auf Spielstand-Code und XAPI (`probe:XAPILIB::XCreateSaveGame`, `…XDeleteSaveGame`, `probe:Save::ReadContainer`, `VerifySignature`, `WriteContainer`, `OpenSaveMenu`, `Dialog`) mit Protokollzeilen „save: …“ und Summe „save summary: …“ | Eingabeskripte warten auf die Dialoge (`wait log "save: dialog MEMCARD_007"`), `m7_check` wertet Lesen, Signatur, Schreiben und Löschen aus. Prologe mit capstone geprüft, kein Sprung in die übernommenen Bytes (Rohsuche) |
| S6 | M7 Exit 4: „Löschen über das Menü entfernt das Verzeichnis“ | Das Spiel hat keinen Menüpunkt „Löschen“ (`HINTS_040` „… mit Y löschen“ ist nirgends referenziert; Y wirkt nicht). Gelöscht wird auf den beiden Wegen des Spiels: „beschädigtes Spiel löschen?“ (`MEMCARD_052` → Ja) und Überschreiben (`MEMCARD_016` → Ja löscht die alte Sicherung vor dem Schreiben). Beide geprüft | m7_saves.md 4.2/4.3; Läufe `check/damaged` und `check/slots`: `XDeleteSaveGame(...) -> 0`, Verzeichnis weg |
| S7 | M7 Exit 1: „… Optionen und Extras“ | Optionen: vollständig. Extras: die Freischalt-Flags bleiben über Neustarts erhalten, die **Bestzeiten nicht** – das ist Verhalten des Originals und bleibt so | `FUN_000F3B90` führt Datei und Speicher mit **vorzeichenbehaftetem** Minimum zusammen (`jge` bei 0xF3C5E); −1 („keine Zeit“) der frischen Tabelle gewinnt (m7_saves.md 3.4). Lauf `check/load`: Flag kept, Bestzeit 1-1 2 s → −1. Ein optionaler Fix wäre ein Hook auf `FUN_000F3B90` |
| S8 | R16 | Zustellzeit gemessen und protokolliert: „I/O APC: save write completion 0x000E01A0 delivered N ms after the write“ (`kernel\io_apc.cpp`, Zeitstempel je APC) | 10,6–16,4 ms in allen Läufen (Poll 0xE0030 einmal pro Frame); keine Änderung der Zustellung nötig |
| S9 | M7 Exit 2: `tools/verify_save.py` prüft | Zusätzlich `--resign` (Signatur mit dem eingestellten Schlüssel neu schreiben), `--json`, Prüfung von `SaveMeta.xbx` und Verzeichnis-Hash; als Modul von `m7_check` benutzt (signierte Testdateien) | Import von Sicherungen mit fremdem Schlüssel (R11) und die 25-Sicherungen-Prüfung (24 signierte Kopien). Gegenprobe Python ↔ XAPI über die XcSHA-Exporte in `tests\save_tests.cpp` |
| S10 | 4.1 `<userdata>` | Prüfwerkzeuge benutzen immer eigene Benutzerdaten (`run\m7\check\main`, `…\nospace`); `m7_check` verweigert Ordner unter `%LOCALAPPDATA%\FuturamaPC` | Nutzerwunsch: eigene Spielstände nie anfassen |

### 8.19 Abweichungen in G5 und G7: Kamera am rechten Stick, Level-Skip-Cheat

Stand: G5/G7, 2026-10-12. Bauten: MSVC RelWithDebInfo `build\g5g7`, Debug `build\g5g7-werr` und clang-cl Debug `build\g5g7-clang` (beide `FUT_WERROR=ON`); `fut_tests` in allen drei Bauten bestanden; `tools\m4_check.py` mit der Konfiguration von `run\verify-m5\futurama.ini` (eigene Benutzerdaten `run\g5g7\verify-m5`) ALL PASS. Bericht [`g5_g7_report.md`](g5_g7_report.md), Analyse [`g5_camera.md`](../analysis/loader/g5_camera.md).

| # | Plan (Abschnitt) | Abweichung | Grund / Beleg |
|---|---|---|---|
| G5-1 | 6 Meilensteine: kein Eingriff in die Spiellogik | Mit `[input] camera_response = precise` ersetzt der Port den Drehwinkel der Folgekamera je Tick (`probe:Camera::FollowUpdate` 0x15990, `probe:NiMatrix3::MakeYRotation` 0x103020 nur für den Aufruf 0x17B2B) und löscht danach den Filterzustand des Spiels (`+0x154`, `[+0xE4]+0xC`). Standard `original`: keine Hooks (`FUT_PROBE_IF`, Hook-Summe 181 wie ohne G5) | Nutzerwunsch „präzisere, reaktionsfreudigere Kamera“. Ersetzt wird nur der Winkel an der Stelle, an der das Spiel ihn anwendet; Kollision, Höhe, Abstand und Nachführen bleiben Code des Spiels (g5_camera.md 4). Messung: 90 % der Drehrate nach 33 statt 483–650 ms, Nachlauf 0,1–1 statt 6–18° |
| G5-2 | 3.1 Hooks auf häufig gerufene Funktionen vermeiden | `MakeYRotation` hat 46 Aufrufer; die Probe filtert über die Rückkehradresse (0x17B30) und reicht alle anderen unverändert durch | Ein Patch des einen `call` (0x17B2B) wäre kein Funktionsanfang und passt nicht zur Hook-Mechanik (Funktionsanfang, Prüfung gegen `xbe_functions.gen.inc`). Kosten: ein Host-Aufruf je `MakeYRotation`, nur mit `precise` |
| G5-3 | 8.11 M4: Eingabe 1:1, Totzone nur `pad_deadzone` | Mit `precise` formt der Host den **rechten Stick der Controller** um (radiale Totzone 0,18, linear; über die Umkehrung der Spielkurve, `inp::SetPadShaper`), außer während der Port umgeleitet ist (Freecam) | Die Totzone 0,4 des Spiels macht kleine Drehungen unmöglich; erlaubt, weil sie auf der Host-Seite liegt. Maus und Eingabeskript unverändert |
| G7-1 | M7 S1: FsChangeLevel nur als Debug-Aktion für Prüfläufe | Der Spieler erreicht dasselbe Native (`FUN_000B0520`) mit der Tastenfolge A A B B A A B B A A im Level (`[cheats] level_skip` (bis G8: `[game] level_skip_cheat`), Standard an). Das Folgelevel steht in einer Tabelle (= Argument von Opcode 0xA1 in jedem `levelN-M.ful`) | Nutzerwunsch. Was das Levelskript **vor** seinem FsChangeLevel tut (Schlussszene, in 5-3 der Abspann `credits.avi`), entfällt; Statistik, Bestzeit, Speichern und Folgelevel sind die des Spiels. Geprüft in 1-1, 1-2, 1-6 (Chickenwalker), 2-1, 2-5, 2-6, 3-1, 4-1 (Ratte), 5-1, 5-3 (→ 1-1) |
| G7-2 | 8.11 M4b: ein Sample-Filter (Freecam) | `HostRouter::AddSampleFilter` (Kette nach dem Freecam-Filter, höchstens 4) und `HostRouter::Diverted` | Vorgabe: Freecam-Filter verketten, nicht ersetzen; G6 kann die Kette ebenso nutzen |

### 8.20 Abweichungen in G6: Ego-Steuerung

Stand: G6, 2026-10-12. Bauten: MSVC RelWithDebInfo `build\g6-fp`, Debug `build\g6-werr` und clang-cl Debug `build\g6-clang` (beide `FUT_WERROR=ON`); `fut_tests` in allen drei Bauten bestanden; `tools\m4_check.py` (`run\verify-m5\futurama.ini`, Option aus) ALL PASS. Bericht [`g6_report.md`](g6_report.md), Analyse [`g6_firstperson.md`](../analysis/loader/g6_firstperson.md).

| # | Plan (Abschnitt) | Abweichung | Grund / Beleg |
|---|---|---|---|
| FP-1 | 6 Meilensteine: kein Eingriff in die Spiellogik | Mit `[input] first_person = on` erweitert der Port die Ego-Ansicht des Spiels (Kameramodus 3) zu einer Steuerung: 6 bedingte Probes (`FUT_PROBE_IF`) auf `FUN_00054DA0` (Ein/Aus, automatisches Verlassen 0x587F8 überspringen), `Player::vf+0x11C` 0x588B0 (automatischer Eintritt), `FUN_00054AE0`/`Player::vf+0x104` 0x50D10 (Laufen/Springen mit gelöschtem Sperrbit `+0xB88`), `FUN_000536D0` (Bewegung relativ zum Blick) und `FUN_00018250` (Blick, Position, near-Ebene). Standard `off`: keine Hooks (Hook-Summe 181) | Nutzerwunsch Egoperspektive. Bewegung, Tempo, Kollision, Animation, Zielen der Waffen und Körperdrehung bleiben Code des Spiels; ersetzt werden nur die Sperren, die Bewegungsrichtung und die Blickglättung (g6_firstperson.md 5, 6) |
| FP-2 | 3.2 Hook-Ziele nur aus `xbe_functions.gen.inc` | Zwei nur über die vtable erreichte Funktionen sind neue Einträge in `tools\gen\extra_function_starts.tsv` (0x50D10 `Player_vfJump`, 0x588B0 `Player_vfViewToggle`), Tabelle neu erzeugt | Beide stehen in allen fünf Spieler-vtables (Unit-Test gegen die XBE); Prologe ganze Befehle, kein Sprung in die übernommenen Bytes |
| FP-3 | 8.11 M4: Maus wirkt als rechter Stick | Solange die Ego-Ansicht läuft, gibt `KeyboardMouseSource` Mausbewegung nicht an die Belegung, der Blick liest die rohen Zählschritte (`SetMouseMotionQuery`, additiv) | Für Zielen mit der Maus ist ein fester Winkel je Zählschritt nötig; über den Stick ginge die Bewegung durch Totzone und Kennlinie des Spiels. Tasten und Stick-Belegungen bleiben |
| FP-4 | – | Mit G5 `camera_response = precise` dekodiert die Ego-Ansicht den umgeformten rechten Stick über die Spielkurve und wendet danach ihre eigene Kennlinie an | G5 und G6 wirken nicht gegeneinander: Die Folgekamera (Modus 0) gehört G5, Modus 3 G6 (Lauf `fp11precise`: 189 Hooks, beide aktiv) |
| FP-5 | 4 Renderer: Frustum wie vom Spiel | In der Ego-Ansicht setzt der Hook das Frustum mit near 0,1 (Minimum der Engine, `FUN_00011960`) statt 0,25 und hält die Kamera mindestens 0,4 Einheiten von Wänden (Kamerastrahl `FUN_00011000`) | Kein Durchblick durch nahe Geometrie; normale Wände bleiben laut Messung ≥ 1,9 Einheiten entfernt |

### 8.21 Abweichungen in G3: Breitbild

Stand: G3, 2026-10-12. Bauten: MSVC RelWithDebInfo `build\g3` (`FUT_WERROR=ON`) und clang-cl Debug `build\g3-clang` (`FUT_WERROR=ON`, 0 Warnungen); `fut_tests` 493/493 (1 übersprungen) in `build\g3`, die Widescreen-, Schema-, Fenster- und AA-Fälle auch im clang-Bau. Bericht [`g3_report.md`](g3_report.md).

| # | Plan (Abschnitt) | Abweichung | Grund / Beleg |
|---|---|---|---|
| W1 | 2.2 / M3c: Der Gast sieht 640×480, der Backbuffer hat 4:3 (`ComputeRenderSizes` passt das Gast-Seitenverhältnis ein) | Mit `[render] aspect` ≠ original hat der Backbuffer das Anzeigeformat (z. B. 1280×720). Der Gast sieht weiter 640×480; jeder Zeichenaufruf und jedes Löschrechteck wird mit einer von vier Platzierungen in den breiten Puffer abgebildet (Stretch/Center/Left/Right, `gfx\widescreen_math.h`). Die Seitenbalken des zentrierten 4:3-Bereichs löscht der Renderer einmal pro Bild schwarz | Nutzerwunsch Breitbild mit korrekt platziertem HUD. Original = keine Änderung (Lauf `run\g3\base43`: 181 Hooks, keine Widescreen-Zeile, Bild gleich der Vorher-Aufnahme) |
| W2 | 6 Meilensteine: Spiellogik unverändert | `probe:NiCamera::Click` (Freecam) verbreitert für die Dauer des Click das Frustum (l, r × k) jeder Backbuffer-Kamera, die Szenenkamera ist (perspektivisch, Achse im Frustum, far ≥ 50·near, Port über die ganze Breite), außer im Kontext „Frontend“, und berechnet das vf39-Ende neu; danach Rückgabe der Spielwerte. Spiellogik, Ton-Hörer und HUD-Projektionen sehen das 4:3-Frustum | Hor+ mit Kulling passend zum breiteren Bild. Ohne Neuberechnung der Ebenen würden Objekte am Rand fehlen |
| W3 | – | Das Hauptmenü (Kontext „Frontend“) bleibt 4:3 mit Balken | Im 16:9-Lauf zeigte Hor+ am linken Rand das nächste Menü-Panel, das das Spiel knapp außerhalb des 4:3-Bildes parkt. Pausenmenü und Statistikbildschirm (Kontext „Ingame“) werden verbreitert, dort ist der Rand Teil des Levels |
| W4 | – | Eine Fläche aus höchstens 8 Ecken einer Hor+-Kamera, die mit der Projektion des Spiels den ganzen Bildschirm bedeckt (Abdunklung des Pausenmenüs, ein Quad mit 137 % Überstand), zeichnet der Renderer mit dieser Projektion, gestreckt | Sonst bliebe sie ab 21:9 (Faktor 1,78) schmaler als das Bild |
| W5 | – | HUD-Verankerung nur im Kontext „Ingame“ und nur für Bildschirm-Polygone (Slot 54), Geometrie der HUD-Kameras (l ≥ 0, b ≥ 0) und Kameras mit Teil-Port (Avatar); Slot-55-Bildschirmtexturen bleiben zentriert | Ladebilder bestehen aus 256-Pixel-Kacheln (Slot 55) und laufen schon im Kontext „Ingame“; Menütexte sind 3D-Geometrie der Menükameras |
| W6 | 3.2 Hooks | Kein neuer Hook; der Click-Probe der Freecam ruft `widescreen::FrustumFactor` | Ein Probe pro Adresse; Freecam-Pose und Hor+ wirken in einem Patch |
| W7 | M3c `[render] trace_frames` | Die Draw-Spur enthält zusätzlich jede Kamera von Slot 47 (Zeiger, Frustum, Port) | Zuordnung der Zeichenaufrufe zu HUD-, Avatar- und Szenenkameras (Abschnitt 3 des Berichts) |

### 8.22 Abweichungen in G2: Texturpakete

Stand: G2, 2026-10-12. Bauten: MSVC RelWithDebInfo `build\g2`, Debug `build\g2-werr` und clang-cl Debug `build\g2-clang` (beide `FUT_WERROR=ON`, 0 Warnungen); `fut_tests` in allen drei Bauten bestanden (davon 9 neue Fälle in `tests\texture_pack_tests.cpp`). Läufe unter `run\g2\` (`dump`, `base`, `pack`, `pack4`; je etwa 34 s, Rückgabe 0). Bericht [`g2_report.md`](g2_report.md), Schnittstelle `src\INTERFACES.md` 4.37.

| # | Plan (Abschnitt) | Abweichung | Grund / Beleg |
|---|---|---|---|
| T1 | ROADMAP „Vorgemerkte Grafik-Features“: stabiler Hash über die Originaldaten | Schema **„FTX1“**: XXH64 (Seed 0) über einen 40-Byte-Kopf (Kennung, NiPixelFormat-Format, bpp, Breite, Höhe, vier Kanalmasken, Bytezahl), die gespeicherten Bytes der **Stufe 0** und bei PAL/PALA die Palette. Mipstufen werden nicht gehasht | Unabhängig davon, was der Port daraus macht (DXT bleibt BC, 16 Bit → BGRA8, `keep16Bit`); gleiche Bilder in verschiedenen NIFs teilen sich eine Ersatzdatei (Lauf `dump`: 737 Uploads, 721 verschiedene Hashes). Festgenagelt durch einen Unit-Test |
| T2 | Ersatzordner „DDS/BC7 mit Mipmaps“ | DDS BC1/BC2/BC3/BC7 (BC7 nur über den DX10-Kopf) sowie BGRA8/RGBA8, dazu **PNG** (Mipmaps erzeugt der Port, alphagewichtete Box). Neues RHI-Format `BC7_UNorm` (am Ende der Tabelle angehängt); sRGB-Varianten werden als UNORM gelesen | PNG ist das, was Spieler und Werkzeuge am einfachsten erzeugen; das Spiel rechnet wie die Xbox gammakodiert. Ohne Feature Level 11 schlägt nur das Anlegen einer BC7-Textur fehl, das Original bleibt (Warnung) |
| T3 | – (nicht vorhergesehen) | Nur **statische** Quelltexturen (`NiSourceTexture +0x38` gesetzt) werden ersetzt bzw. abgespeichert; Filmtexturen (YUY2, Texel-UVs, 8.16 V6), Render-Targets, die 8×8-Standardtextur und Neu-Uploads nach Revisionswechsel nie | Nur bei ihnen bleibt die UV-Bedeutung unabhängig von der Größe (normierte UVs). Das Spiel sieht weiter Originalgröße, -format und -stufenzahl (`NiPCTextureData +0x48/+0x4C/+0x50`) |
| T4 | „Ohne Ersatzordner exakt wie bisher“ | Mit `texture_pack` leer und `texture_pack_dump = 0` läuft kein G2-Code (kein Hash, kein Thread, keine Logzeile) | Lauf `base`: keine „texture pack“-Zeile, 738 Texturen wie vor G2 |
| T5 | – | **Asynchrones Laden:** Der Port lädt zuerst das Original hoch und dekodiert die Ersatzdatei in einem eigenen Thread; der Texturauflöser tauscht sie beim nächsten Zeichnen ein. LRU-Zwischenspeicher 96 MiB dekodierter Bilder | Kein Stocken beim Laden (714 Dateien, 2×: 101–116 ms Dekodierzeit gesamt, höchstens 6 ms je Datei; 4×: 235 ms). Bekannt: In den ersten Bildern nach dem Laden kann noch das Original zu sehen sein |
| T6 | – | **Mipfilter NONE** (das Original tastet nur Stufe 0 ab): Für eine Ersatztextur stellt der Port MIP_LINEAR mit `maxLod = log2(Ersatz/Original)` ein (`texpack::AdjustSampler` in `ff_pipeline.cpp`) | Sonst würde eine 2×/4×-Textur ohne Mipmaps verkleinert und flimmern; mit der Grenze wird sie nie unter die Originalauflösung gefiltert |
| T7 | Werkzeug: „auf Cartoon/Anime trainierte Upscaler, klassische Filter als Rückfall“ | Standard ist ein **klassisches, comicgerechtes Verfahren** (Lücken unter Alpha füllen, leichte kantenerhaltende Glättung gegen DXT-Blockrauschen, Lanczos-3 mit Begrenzung gegen Überschwinger, Kantenstraffung zur dunkleren/helleren Nachbarfarbe, binäres Alpha bei 50 % geschwellt) mit BC1/BC3-Ausgabe; KI-Upscaler nur als Option `--method external` mit einem vom Nutzer selbst installierten Programm | Vorgabe „keine Downloads“; numpy und Pillow waren installiert. Das Verfahren erzeugt keine Fotodetails, hält Flächen flach (mittlere Abweichung der Bildfarben 0,5–0,8 Stufen) |
| T8 | – | Dump-Ordner fest `<Benutzerdaten>\texture_dump` (mit `index.tsv`); `.gitignore` schließt `texture_dump/`, `texture_pack*/` und `*.dds` aus | Abgeleitete Spieldaten dürfen nie ins Repository |

### 8.23 Abweichungen in M8 (Renderer P2): Render to Texture, projizierte Schatten, Partikel, Texturen

Stand: M8/M3e, 2026-10-12. Bauten: MSVC RelWithDebInfo `build\m8`, clang-cl Debug `build\m8-clang` (`FUT_WERROR=ON`); `fut_tests` 491/491. Bericht [`m8_report.md`](m8_report.md), Schnittstelle `src\INTERFACES.md` 4.39. Die Zeilen heißen P2-1 bis P2-8.

| # | Plan (Abschnitt) | Abweichung | Grund / Beleg |
|---|---|---|---|
| P2-1 | 8.6 G9 (Render to Texture mit MSAA, Skala 1) | Jede `NiRenderedTexture` bekommt ein eigenes Farbziel und einen eigenen D24S8-Tiefenpuffer **in der Renderauflösung** (Gastgröße × Faktor des Backbuffers, höchstens 4096) mit den **MSAA-Samples des Backbuffers**; beim Verlassen des Ziels (Slot 15 NULL oder anderes Ziel) wird aufgelöst. Viewport und Clear-Rechtecke werden wie beim Backbuffer skaliert | Der Gast sieht weiter 128×128 (Slots 21/22, Kamera, Port); das Schattenbild wird bei 1280×960 mit 256×256 und 4× MSAA gerendert. Unit-Tests: Clear-Rechteck im Texturziel, Auflösen bei Faktor 2 mit 4× MSAA |
| P2-2 | 2.2 (Purge-Slots 58–62/64 „Lebensdauer der Daten“) | Slots 58 (PurgeGeometryData), 59 (PurgeMaterial), 61 (PurgeScreenTextureRendererData), 62, 64 bleiben **No-ops** | Sie geben nur REN-eigene Rendererdaten am Gastobjekt frei (`+0x30` Geometrie, `+0x58` Material über `FUN_001618A0`, `+0x1C` Bildschirmtextur, Skin-Partition). Der Port legt diese Daten nie an (die Felder bleiben 0), Geometrie geht pro Draw durch den Transient-Ring. Die Renderdaten der Texturen (Slot 33/Datendestruktor) räumen auch die neuen Renderziele ab |
| P2-3 | 6 M8, renderer.md 5.6 (Mehrpass-Pipeline offen) | Die Pipeline baut für projizierte Schatten (`NiTextureEffect` Typ 1, Koordinatenmodus WORLD_PARALLEL) die **Zusatzpässe von vf2 0x164A20 ff.** nach: Schattenstufe (CAMERASPACEPOSITION, COUNT2, Matrix des Handlers 0x162C90), bei Clipping-Flag die Clip-Stufe mit der 2×1-Textur von `FUN_00163A40` (`FUN_00162BB0`), Blend ZERO/SRCCOLOR aus der Passtabelle (+0x2F0), Nebel ab Pass 2 aus. Nur für Geometrie ohne Blending und ohne Alpha-Test | Ohne die Pässe wäre das Render to Texture unsichtbar: das einzige im Level gerenderte Texturziel ist Frys Schattenbild (`FUN_0003B960`, Kamera von oben, weißer Hintergrund, Silhouette grau 0,8). Ergebnis: Schatten unter Fry, im Sprung deutlich (Screenshot im Bericht) |
| P2-4 | – | Nicht geclippte Schatten bekommen immer eigene Pässe; REN legt sie ohne geclippte Schatten in den ersten Pass, wenn dessen erste Stufe eine Basis-Map ist und Platz hat (`FUN_00162670(2, 1, 1)`) | Gleiches Ergebnis für deckende Maps, einfacherer Code; der Schatten des Spiels ist geclippt (+0x170 = 1). Projizierte Lichter, Umgebungs- und Nebel-Maps werden weiter nicht nachgebildet (gezählt: `texture effects not reproduced`) |
| P2-5 | 3 M3d (Interpolation Stufe 3: Partikel) | Partikel (Slot 52) bleiben im **60-Hz-Raster** der Logik; ihre Quads werden mit der Kamera des Bildes ausgerichtet | Vorgabe des Auftrags erlaubt es; die Partikeldaten ändern sich nur pro Tick |
| P2-6 | 6 M8 (Partikel) | Slot 52 baut die Quads wie REN auf der CPU (Ecken c ∓ A, c ∓ B mit A/B = (rechts ± oben) · Radius / Skala · Größe, UVs, Index 0 1 2 0 2 3, `FUN_0015CBF0`), aber in Arena-Streams statt in einem dynamischen VB mit FVF 0x142/0x152. Kein Linien-Pfad (Slot 53 wird nicht aufgerufen) | Gleiche Geometrie und gleicher Zustand (Properties, Pipeline mit einem UV-Satz, Licht). Ob die Partikel im Original sichtbar sind, ist ohne xemu-Vergleich nicht geprüft (in den Testszenen liegen sie außerhalb des Bildes oder verdeckt) |
| P2-7 | m4_report 6.1 („5 Texturen ohne Bild“) | **Keine Änderung nötig:** Die 5 Standardtexturen sind die `NiSourceTexture::Create(NULL)`-Texturen des Movie-Players (`FUN_000EE220`), eine je geöffnetem Film (Lauf: 5 Filme geöffnet = 5 Texturen „without image“). Der Player ersetzt sie sofort durch seine YUY2-Textur (M6) | Das Original nimmt hier ebenfalls die 8×8-Standardtextur (`FUN_0015E010`); alle übrigen 737 Texturen haben ein Bild, 0 Konvertierungsfehler |
| P2-8 | – | Diagnose: Jedes Bild aus `[render] trace_frames` schreibt die Texturziele als `frame_NNNNNN_rtt_K.png` in `[render] screenshot_dir`; die Trace-Zeile nennt die Passzahl | Kein neuer Konfigurationsschlüssel; damit war das Schattenbild nachprüfbar |
| P2-9 | 8.3 B19, GFX_CONTRACT 12.2 (Halbpixel-Versatz in Gastpixeln) | Nachtrag 2026-10-14: Auf **Texturzielen** ist der Versatz `gfx::TargetPixelCenter` – 0,5 Gastpixel nur, wenn das Ziel in Gastgröße ohne MSAA gerendert wird, sonst **0** (Gastpixelkanten fallen auf Gerätepixelkanten). Projektionen und Shader (`FFFrameConstants::pixelCenter`: vortransformierte Quads, 1/16-Raster) nehmen denselben Wert. Backbuffer unverändert 0,5 | Schwarze Linie in der Säure von Level 1-2 (Nutzermeldung): Die Säuretextur ist ein 128×128-Texturziel, das das Spiel mit `NiScreenPolygon`s 0..128 füllt. Mit 0,5 Gastpixeln blieben die erste Gerätezeile und -spalte leer (schwarz), und die gekachelte Textur zeigte die Naht als Linie. Beleg: `m8_report.md`, Nachtrag 2026-10-14 |

| P2-10 | 6 M8, renderer.md 5.6 (Mehrpass-Pipeline) | Nachtrag 2026-10-14 (M3e-Rest, [`m8b_report.md`](m8b_report.md)): Die Standard-Pipeline wird **ganz** nachgebildet – vf2 0x164360 Zweig für Zweig (Dark, Detail, Glow, Gloss, Bump, Decals, projizierte Lichter mit und ohne Ebene, projizierte Schatten, Umgebungs- und Nebel-Map), mit den Stufentabellen und Pass-Alpha-Properties von `FUN_0015A120`, den Platzregeln von `FUN_0015FDD0`/`FUN_00162520`/`FUN_00162670` und dem Effektstufen-Cache von `FUN_00164200` (Neuaufbau nur bei anderem Effect State, `BeginScene` leert ihn). **P2-4 entfällt:** nicht geclippte Schatten landen jetzt wie in REN im ersten Pass, wenn er Platz hat. Die Bedingung „+8 != 0“ vor der Nebel-Map (Passzähler von vf3 aus dem vorigen Draw) ist als `StateShadow::pipelinePassesDrawn` nachgebildet | Messung: statischer Scan aller 753 NIFs (`m8b_multimap_scan.py`) und Zensus aller 22 Level. Sichtbar: die Umgebungs-Map (Sphere-Map, 16×1-Toon-Rampe Weiß/0xEE/0xDD, NEAREST) auf Figuren und Requisiten in jedem Level, die Glow-Maps der Lavalampen in 3-2. Dark-Maps (1-6 Brunnennebel, 4-1 Sumpf) und Detail-Maps (4-1 Sumpf) laufen über denselben Code, wurden aber von keinem Skriptlauf erreicht (Unit-Tests) |
| P2-11 | – | Nicht nachgebildet: die BUMPENVMAP(LUMINANCE)-Verschiebung einer Bump-Stufe (gezählt als `multiMapApproximated`); mehr als 1 + 8 Pässe je Draw (gezählt als `passOverflows`, der letzte Pass wird ersetzt) | Das Spiel hat keine Bump-Map (statischer Scan) und braucht höchstens 3 Pässe (Basis, Schatten, Umgebungs-Map); beide Zähler sind in allen Läufen 0 |
| P2-12 | GFX_CONTRACT 11 (Diagnose) | Neuer Zensus `gfx\multimap_census.h` (Exit-Zeilen `multi-map census: …`, ohne Schlüssel immer an, kostet nur bei Mehrpass-Draws eine Tabellensuche) und `texpack::UploadedHash` für die Texturnamen (G2-Hash) | Grundlage der Messung je Level; „texture effects not reproduced“ zählt jetzt nur noch, was auch REN verwirft (0 in allen Läufen) |

Offene Punkte nach M8: Vergleich der Schatten und Partikel mit xemu; die HUD-Ansicht des Avatars braucht kein Render to Texture (G1-Analyse 6, im Port schon sichtbar). Seit M3e-Rest (P2-10) ist der Mehrpass-Pfad vollständig; offen bleiben nur die nicht benutzte Bump-Verschiebung und ein Sichtvergleich der Dark-/Detail-Maps im Spiel.

### 8.24 Abweichungen in M6b: Filmpakete (KI-hochskalierte Filmbilder)

Stand: M6b, 2026-10-13 (Nutzerwunsch: die Filme „nicht übertrieben“ mit KI hochskalieren, 2× = 1280 × 960, Comic-Stil erhalten). Bau: MSVC RelWithDebInfo `build\video` (`FUT_WERROR=ON`); `fut_tests` 517/517 (8 neue Fälle in `tests\movie_pack_tests.cpp`). Läufe unter `run\video\` des Arbeitsbaums. Bericht [`m6b_report.md`](m6b_report.md), Schnittstelle `src\INTERFACES.md` 4.40.

| # | Plan | Abweichung / Entscheidung | Grund / Beleg |
|---|---|---|---|
| F1 | 8.16 V1: kein FFmpeg im Port | Der **Port** bleibt ohne FFmpeg: die Pakete (H.264 in MP4) dekodiert der in Windows eingebaute Media-Foundation-Decoder (`IMFSourceReader`, YUY2-Ausgabe) in einem eigenen Thread. FFmpeg, NVENC und Real-ESRGAN braucht nur das **Werkzeug** `tools\movie_upscale.py`, das der Nutzer selbst ausführt | Keine Downloads, keine LGPL-DLLs im Paket. H.264 statt HEVC: HEVC braucht unter Windows die Store-Erweiterung |
| F2 | – | **Nur das Bild** kommt aus dem Paket. Die XMV wird weiter vollständig demuxt und dekodiert (Ton aller Sprachen, Audiotakt, Filmende, 640 × 480-Bild in der Shim-Fläche); das n-te gezeigte XMV-Bild wird durch das n-te Bild des Pakets ersetzt, wenn beide dasselbe pts relativ zum ersten Bild haben (± 1 ms) | Takt, Sprache und Überspringen bleiben exakt wie in M6; jeder Fehler (Datei fehlt, falsche Bildzahl, pts-Abweichung, Decoderfehler) fällt ohne Lücke auf das Originalbild zurück. WMV2 kostet 0,2–0,4 ms je Bild |
| F3 | – | Zuordnung Film ↔ Paketdatei über die **CRC-32 der ersten 0x1000 Bytes** der XMV (der Kopf, den das Spiel an `CreateDecoderForPackets` übergibt), nicht über den Dateinamen | Die HLE kennt den Dateinamen nicht; ein Paket von einer anderen Disc passt nie. Bytegleiche Filme (Intro = 1-1-00) teilen sich eine Datei |
| F4 | 8.16 V5/V6 | Die Gast-Textur bleibt 640 × 480 (das Spiel adressiert sie in Texeln); das Paketbild hängt als **Host-Bild** an der Shim-Textur (`d3d::SetHostImage`), der Renderer lädt es in eine Host-Textur seiner Größe und skaliert die Texel-UVs mit `MovieTexelScale` | Das Spiel bleibt unverändert (Viereck, UVs, Fläche); das Bild füllt genau dieselbe Bildschirmfläche. Belegt: Screenshot = Paketbild k mit 47–58 dB (Nachbarbilder 13–24 dB), Versatz 1 Ausgabepixel wie bisher (8.16 V12) |
| F5 | – | KI-Verfahren: **realesr-animevideov3 ×2, gemischt 75 % KI + 25 % Lanczos** (`--ai-mix 0.75`), H.264 NVENC cq 18, ohne B-Bilder, Schlüsselbild alle 30 Bilder, BT.601 begrenzt, variable Bildrate mit den pts der XMV | Vergleich reine KI gegen Mischung an Intro, 1-1-01, 3-1-01, fox: reine KI glättet kleine gemalte Details (3-1-01-Hintergrund), die Mischung behält sie bei sauberen Konturen (wie G2); zeitliches Flimmern in ruhigen Flächen 0,02–0,06 Stufen. Eingebrannte Schrift (Abspann, Trailer, Logos, HUD im Attract-Film) eigens geprüft: sauber lesbar, beim rollenden Abspann kein zusätzliches Flimmern – alle 47 Filme mit 75/25 (`--ai-mix-for` für Ausnahmen, nicht gebraucht) |

### 8.25 Abweichungen in G8: Cheats

Stand: G8, 2026-10-13 (Nutzerwunsch: Unverwundbarkeit, One-Hit-Kill, „Feinde ignorieren mich“). Bauten: MSVC RelWithDebInfo `build\cheats` und clang-cl Debug `build\cheats-clang` (beide `FUT_WERROR=ON`); `fut_tests` 533/533. Läufe unter `run\cheats\` des Arbeitsbaums. Bericht [`g8_cheats_report.md`](g8_cheats_report.md), Analyse [`g8_cheats.md`](../analysis/loader/g8_cheats.md), Schnittstelle `src\INTERFACES.md` 4.41.

| # | Plan | Abweichung / Entscheidung | Grund / Beleg |
|---|---|---|---|
| G8-1 | 6 Meilensteine: kein Eingriff in die Spiellogik | Neuer Abschnitt `[cheats]` (4 Schalter, Standard aus) mit 10 bedingten Proben (`FUT_PROBE_IF`): Spielerschaden 0x51AD0, FsDeductLives 0xC1240, Gegnerschaden 0x354B0/0x47240/0x60080/0x80300/0x86520/0x88530/0x90570, KI-Gehirn 0x2ECB0. Alles aus: keine Hooks (`hooks: 184 installed` wie ohne G8) | Nutzerwunsch; Muster wie G4–G7 |
| G8-2 | – | „Unverwundbar“ verändert die Gesundheit nicht (anders als der eingebaute Gott-Modus, Gesundheit 55555), sondern überspringt den Schaden. Todeszonen (Materialflag 8, Rückkehr 0x38C49) töten weiter, der Lebensabzug des Skripts entfällt | HUD und Spielstand bleiben normal; in Abgründen säße eine überlebende Figur fest. Dieselbe Ausnahme hat der Gott-Modus der Entwickler |
| G8-3 | 3.1 Hooks am Funktionsanfang | Für „Ein Treffer“ sitzen Proben am Eingang von sechs Gegnerklassen, nicht nur auf der Basis 0x354B0; fünf davon kannte Ghidra nicht als Funktionen (`tools\gen\extra_function_starts.tsv`) | Diese Klassen prüfen vor 0x354B0, ob der Treffer tödlich ist, und lösen dann ein Ereignis aus (Vermutung: Beute/Statistik); ein erst in 0x354B0 erhöhter Betrag ginge daran vorbei |
| G8-4 | – | „Ein Treffer“: Bosse (Maximum ≥ 1000) ein Viertel je Treffer; Gesundheit/Maximum 55555 und Maximum ≥ 50000 unverändert | Bosse behalten ihre Phasen; vom Spiel oder Skript unverwundbar gemachte Figuren bleiben es (`maxhealth` aus `chars\*.ucf`) |
| G8-5 | – | „Feinde ignorieren mich“ setzt das KI-Flag 0x2DBAA0 („Feuer einstellen“, sonst nur in Zwischensequenzen) nach jedem Gehirn-Update | Kleinster Eingriff mit dem Verhalten des Spiels selbst; Bosse mit eigener Angriffslogik kämpfen weiter |
| G8-6 | 8.11 M4: Rumble nur vom Spiel | `HostRouter::PulseRumble` für die Rückmeldung der Tastenfolgen (Maximum mit dem Rumble des Spiels, skaliert und stumm wie dieses) | Vorgabe „kurzer Rumble-Puls unter Beachtung von `[input] rumble`“ |
| G8-7 | – | Rückmeldung im Bild über die Farbblitz-Funktion des eingebauten Cheat-Handlers (`FUN_00013130`, grün/rot), kein eigener Hinweistext | Robust (reiner Setter auf der Kamera); Hinweistexte des Spiels sind lokalisierte Text-IDs der Levelskripte |
| G8-8 | – | 2026-10-14: Mondsprung und Sprungweite über eine Probe auf den Physikschritt der Welt (0xAE7E0): Geschwindigkeit der gesteuerten Figur vor dem Schritt setzen bzw. um den Schritt herum ergänzen und wieder abziehen; Fallhöhe +0xB84 läuft beim Mondsprung mit. Teleport ohne Hook im Tick (Strahltest und `SetPosition` des Spiels). Munition: Probe auf `Player::UseAmmo` 0x50560, Fahrzeuge und Aufladung je Tick | Kleinster Eingriff: Schwerkraft, Kollision und Animationen bleiben die des Spiels; Bericht `g8_cheats_report.md` 8, Analyse `g8_cheats.md` 9 |
| G8-9 | 3.1 Hooks am Funktionsanfang; Levelskripte unverändert | 2026-10-14 „Timer einfrieren“ (`[cheats] freeze_timer`): zwei bedingte Proben – das Update der HUD-Zeitanzeige 0xEC6D0 wird übersprungen (Zahl steht, das Ablauf-Flag +0x2F der Sumpf-Uhr in 4-1 fällt nie), und um den Skript-Scheduler 0xBA060 bekommen Threads, die an einer der fünf Countdown-Wartestellen der Levelskripte stehen (2-4, 3-5, 5-1/5-2/5-3; Tabelle `kScriptCountdowns`, Code-Offset und drei Bytecode-Wörter als Signatur), vorher dt auf ihre Wartezeit und danach den genauen Wert zurück. Dazu die Debug-Aktion `game snap_to_node <Knoten>` für Prüfläufe (Native FsPlayerSnapToNode 0xB0A60) | Ein Schalter für alle Countdowns (Nutzerwunsch: „eine Option für alle“); die Skripte und ihr Bytecode bleiben unangetastet, nur die Zeit der betroffenen Threads steht. Andere lange Wartezeiten (Dialoge, Kameras, zeitgesteuerte Plattformen) sind keine Countdowns und bleiben (Analyse `g8_cheats.md` 10.2); Bericht `g8_cheats_report.md` 9 |

## Anhang A: Kernel-Implementierungsplan nach Meilenstein

Slots, Ordinale und Konventionen stehen in `kernel_imports.tsv`. „S“ bezeichnet einen Stub mit Log.

| Menge | Slots (Name) |
|---|---|
| **M1 – Boot-Pfad** (`entry`, `XapiInitProcess`, `_rtinit`, `_cinit`) | 0 KfLowerIrql, 1 KeRaiseIrqlToDpcLevel, 3 RtlInitAnsiString, 6 KeInitializeTimerEx, 7 KeInitializeDpc, 9/10/14 Rtl\*CriticalSection, 11 NtFreeVirtualMemory, 12 NtAllocateVirtualMemory, 13 RtlCompareMemoryUlong, 15 NtQueryVirtualMemory, 16 RtlRaiseException, 17 ExQueryNonVolatileSetting, 22 NtSetInformationFile, 23 KeQuerySystemTime, 24 RtlNtStatusToDosError, 25 NtCreateFile, 26 NtClose, 27–29 XcSHA\*, 34 NtReadFile (synchron), 35 NtWriteFile (synchron), 36 NtQueryInformationFile, 37 NtQueryVolumeInformationFile, 41 NtDeviceIoControlFile (Fehler), 43 NtOpenFile, 48 IoCreateSymbolicLink, 54 MmFreeContiguousMemory, 55 HalReturnToFirmware, 58/59 Mm\*AddressProtect, 60 KeSetTimer (nur merken), 72 XcVerifyPKCS1Signature, 74 HalRegisterShutdownNotification (No-op), 76 PsTerminateSystemThread, 77 PsCreateSystemThreadEx, 84 RtlEqualString, 85/86 XeLoad/UnloadSection, 105 RtlUnwind, 106 KeBugCheck; alle 11 DATA + 2 ADDR (1.5) |
| **M2 – Streaming/Fibers** | 20 NtWaitForSingleObjectEx, 21 KeDelayExecutionThread (alertable), 33 NtWaitForSingleObject, 34/35 asynchron + 39 NtUserIoApcDispatcher, 38 NtFlushBuffersFile, 45 NtQueryFullAttributesFile, 46 NtQueryDirectoryFile, 51 MmPersistContiguousMemory, 52 MmAllocateContiguousMemory, 56 MmAllocateContiguousMemoryEx, 107/108 Mm\*KernelStack, 109 DbgBreakPoint |
| **M7 – Saves** | 42/44 MmFree/AllocateSystemMemory (`CopyFileExA`), 49/50 Nt\*SymbolicLinkObject (`XLaunchNewImageA`, falls der Hook später entfällt) |
| **Defensiv** (laut Analyse nicht erreicht, minimal implementiert) | 4 KeSetEvent, 5 KeWaitForSingleObject (XAPI-Shutdown-Routine), 40 NtFsControlFile (Formatierung, durch den `XMountUtilityDrive`-Hook umgangen), 78 HalInitiateShutdown (nur Log) |
| **S – nur ersetzte Bibliotheken** (37) | 2, 8 (ADDR), 18 (DATA), 19, 57, 61–63, 65–71, 79–83, 88–104 ([KRN] 6) |

## Anhang B: Patch- und Hook-Liste der Phase M1/M2

| Adresse | Ziel | Art | ab |
|---|---|---|---|
| 0x293560–0x29371B | 111 Kernel-Slots | Thunk | M1 |
| 38 Stellen aus `fs_sites.tsv` (`offset != 0x0`) | TlsSlots | fs-Retarget | M1 |
| 0xF641B / 0xF642C | `QueryPerformanceCounter` / `QueryPerformanceFrequency` | Detour (stdcall 1) | M1 |
| 0xF3DE0 | `OutputDebugStringA` → Log | Detour | M1 |
| 0xF7F7C | `XMountUtilityDrive` → `Z:` + TRUE | Detour (stdcall 1) | M1 |
| 0xF82E3 | `XLaunchNewImageA` → Meldung + Ende | Detour | M1 |
| 0xD29B0 | `main` (nur bei `--stop-at=main`) | Detour | M1 |
| 0xF92C0, 0xF97DB, 0xF9783, 0xF8081, 0xA4920, 0xF8FCA | Beobachter-Probes `XapiInitProcess`, `_rtinit`, `_cinit`, `XGetDiskClusterSizeA`, Pool-Init, `XapiBootToDash` (Abschnitt 8, A1) | Probe (Detour + Trampolin) | M1 |
| 0x17094F | `SwitchToFiber` (MASM) | Detour | M2 |
| 0x14C170 | `NiXBoxRenderer::Create` → `NiPCRenderer` (cdecl, 8 Args) | Detour | M2 |
| 0x234440, 0x234460, 0x2370D0, 0x2371A0, 0x2373B0, 0x2373F0, 0x236DC0, 0x2354F0 | D3D-Shim | Detour | M2 |
| übrige D3D-Einstiege mit `ext_call_sites > 0` in `d3d_functions.tsv` | Falle | Detour | M2 |
| 40 DSOUND-Einstiege aus `avi_api_surface.tsv` | Stub (M2), HLE (M5) | Detour | M2 |
| 7 XMV-Einstiege 0x264360, 0x264759, 0x2647C6, 0x264943, 0x264962, 0x264FDF, 0x264677 | Stub (M2), HLE (M6) | Detour | M2 |
| 7 XPP-Einstiege 0x28CFE1, 0x28CFE6, 0x28D008, 0x28CE5C, 0x28CEB2, 0x28CEBE, 0x28CF2A | Stub (M2), HLE (M4) | Detour | M2 |
| – | Frame-Zähler für `--frames` | kein Patch: Zähler im `DisplayFrame`-Slot (16) des eigenen `NiPCRenderer`, den `CApp::Frame` 0xD2630 jeden Frame aufruft ([STA] 3.1) | M2 |

Alle Ziele aus Anhang B sind laut `functions.tsv` Funktionsanfänge mit ≥ 5 Byte, auch die 5-Byte-Thunks 0x28CFE1 und `IDirectSoundStream_*`.
