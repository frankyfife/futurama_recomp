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

`XAPILIB::QueryPerformanceCounter` (0xF641B, `rdtsc`) und `QueryPerformanceFrequency` (0xF642C, Konstante 733 333 333) werden auf Win32-QPC/QPF umgeleitet (stdcall, 1 Argument, Rückgabe TRUE). Das Spiel berechnet `1/QPF` einmalig, ein konsistentes Paar genügt ([CPU] 3, [STA] 3.3). Der feste Zeitschritt bleibt: dt = 0,02 s, weil die PAL-Archive 50 Hz festlegen ([STA] 3.4). `KeQuerySystemTime` → `GetSystemTimeAsFileTime`. Später kann die QPC-Umleitung im Replay-Modus virtuelle Zeit liefern (M9).

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
  - `XLaunchNewImageA` mit Reason 2/`'U'` (zu wenig Platz) → Meldung statt Dashboard.

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
- Present mit VSync, optionaler 50-Hz-Limiter; Screenshot-Taste.

**Exit-Kriterien:**
1. Splash (`NI_Splash_PAL.nif`), Legal-Bild und Frontend-Menü erscheinen vollständig; Sichtvergleich mit xemu-Screenshots derselben Szenen.
2. Kein D3D-Debug-Layer-Fehler.
3. `--frames=3000` ohne Absturz, Bildrate ≥ 50.

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

**Umfang:** zweites RHI-Backend (Vulkan bevorzugt, D3D12 optional), native Auflösung und Widescreen (Projektion aus `NiFrustum` anpassen, [REN] 7), MSAA, optional Interpolation gegen das 50-Hz-Ruckeln.

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
| R9 | 50-Hz-Simulation auf 60-Hz-Monitoren (keine Interpolation, [STA] 3.2) | Ruckeln | 50-Hz-Limiter als Option (M3); Interpolation erst nach der Dekompilierung von `Game::Frame` (M10) |
| R10 | FFmpeg für x86: Verfügbarkeit, LGPL | M6 verzögert | vcpkg `x86-windows` früh testen (vor M5); Fallback: eigener XMV-Demuxer ([AVI] 3.3) + nur libavcodec |
| R11 | Ableitung von `XboxSignatureKey` unbelegt ([FIL] 4.3) | Xbox-Saves nicht importierbar | Modus `fixed` als Default; `xbox`-Modus mit einem echten Save prüfen (M7) |
| R12 | Ghidra-Lücken und fehlende Funktionsstarts | Hooks und Callgraph unvollständig | `extra_function_starts.tsv` pflegen; Ghidra-Neuanalyse nur in einem eigenen, exklusiven Schritt (Projektsperre, keine parallelen Agenten) |
| R13 | Nicht standardisierte Registerkonventionen im Spielcode (MSVC 7 /GL, Vermutung) | falsche Ersatz-Signatur | vor jeder Dekompilierung die Aufrufstellen auf Registerargumente prüfen; MASM-Adapter (3.3) |
| R14 | Widersprüche zwischen Berichten: Grafikgrenze [D3D]↔[REN]; Fibers [CPU] „ersetzen“ ↔ [AVI] „Original lassen“; Header-Schreibzugriff [CPU] 5 ↔ [STA] 9.10 | – | entschieden in E6, E9 und 1.4.5 (Header RW, der Zweig wird nicht genommen) |
| R15 | [PRB] hat keinen unabhängigen Verifikationsabschnitt | Launcher-Annahmen falsch | M1 wiederholt alle [PRB]-Tests automatisch (Selbsttest) |
| R16 | Häufigkeit des Save-APC-Polls 0xE0030 zur Laufzeit ([FIL] 6) | verzögertes Speichern | in M7 Zustellzeit messen; notfalls Zustellung auch beim nächsten `SleepEx(0,TRUE)` |
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

## 8. Abweichungen in M1 und M2

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
| B16 | 6 M2 (Kriterien 5, 7) | – | Selbsttests laufen mit `core::InSelfTest()`. Ihre Fiber-Wechsel zählen getrennt (`fiber summary: N switches (G game, S self-test; …)`, je eine Zeile `fiber: first SwitchToFiber (self-test|game)`), ihre Öffnungen tragen im VFS-Trace `[self-test]`. `m2_check.py` verlangt Fiber-Wechsel des Spiels | Review-Befunde 5 und 8b: `GuestSehOnFiberStack` lieferte schon vor `entry` zwei Wechsel und die einzige „first SwitchToFiber“-Zeile; ein Lauf ohne einen Fiber-Wechsel des Spiels hätte Kriterium 5 bestanden |
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

---

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
