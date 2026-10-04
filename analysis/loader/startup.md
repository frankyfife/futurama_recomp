# Startsequenz, Hauptschleife, Threads, Speicher (Retail EU)

Ziel-Binary: `orig\retail_eu\default.xbe`. Alle Adressen sind virtuelle Adressen (Basis 0x10000).
Belege: Ghidra-Export (`analysis\retail_eu_export`), Disassembly mit capstone. Die Skripte liegen in
`analysis\loader\scripts\startup_*.py` (Liste am Ende). Funktionsnamen der Form `CApp::…` sind Arbeitsnamen
aus dieser Analyse. Unsichere Deutungen sind als **Vermutung** markiert.

Kernel-Importnamen: Ghidra (ghidra-xbe) benennt die Imports als `EXTERNAL:<n>`. Dabei gilt n = Thunk-Slot + 1:
Slot 12 ist Ordinal 184 `NtAllocateVirtualMemory`, Slot 56 ist Ordinal 166 `MmAllocateContiguousMemoryEx`,
Slot 77 ist Ordinal 255 `PsCreateSystemThreadEx`. Die vollständige Tabelle steht in
`scripts\startup_kimports.out`. Für Datenimporte ohne Aufrufkante ordnet `startup_kdata.py` die `*_exref`-Namen
aus dem Decompilat zu (Ordinal 164 `LaunchDataPage`, 322 `XboxHardwareInfo`, 324 `XboxKrnlVersion`,
232 `NtUserIoApcDispatcher`). Ordinale ohne Ghidra-Namen sind unten ausdrücklich gekennzeichnet.

---

## 0. Kurzfassung für den Loader

| Thema | Ergebnis | Beleg |
|---|---|---|
| Threads | Das Spiel startet **genau einen** Thread, den Hauptthread (0xF8469). `PsCreateSystemThreadEx` wird nur aus `CreateThread` aufgerufen, und `CreateThread` nur aus `entry` (Raw-Scan nach E8/E9 sowie Datenreferenzen). | `startup_callers_raw.py 0xf9a72` liefert nur 0xF8531 |
| Prioritäten | Weder `KeSetBasePriorityThread` (Ord. 143) noch `KeSetPriorityThread` (148) werden importiert. Es gibt also keine Prioritätsänderung. | Importtabelle |
| Nebenläufigkeit | Das Streaming läuft **kooperativ über XAPI-Fibers** auf dem Hauptthread (16-KB-Fiber pro asynchroner Datei). Die I/O ist überlappt (`ReadFileEx` → `NtReadFile` mit APC). Die APCs werden über `SleepEx(0, TRUE)` zugestellt. | 0x13F250, 0x13F590, 0x13F660 |
| Hauptschleife | `main` (0xD29B0) wiederholt `while (app->vf3())`. Der Kern ist ein **Fixed Timestep** mit dt = 0,02 s (PAL, 50 Hz) bzw. 1/60 s (NTSC). Pro Frame laufen höchstens 100 Simulationsschritte (Zähler), wegen der auf 6·dt begrenzten Zeitschuld praktisch aber höchstens 6. Danach wird genau einmal gerendert und `D3DDevice_Swap(0)` aufgerufen. Zusätzliche `Swap`-Aufrufe gibt es außerhalb dieses Pfads, z. B. 8 leere Frames bei jedem Kontextwechsel (`FUN_0002a660`, siehe 3.2). | 0xA2C20, 0xD2630, 0x146460 |
| Zeitbasis | `QueryPerformanceCounter` liest `rdtsc` (0xF641F). `QueryPerformanceFrequency` liefert die **Konstante 733.333.333** (0x2BB5C755). Auf dem PC **müssen** beide Funktionen ersetzt werden, sonst läuft das Spiel mit falscher Geschwindigkeit. | 0xF641B, 0xF642C |
| PAL/NTSC | Die Wahl hängt **nur von den Archivnamen auf der Disc** ab (`sysPAL.img`/`FutPAL%02d.img` gegenüber `sysNTSC.img`/`FutNTSC%02d.img`). Die EU-Disc ergibt damit fest 50 Hz. | 0xD26A0, 0xA26A0 |
| Speicher | Das Spiel nutzt einen eigenen Zwei-Enden-Pool (`memory.cpp`, Objekt 0x2E7D60) von **0x2600000 Byte (38 MiB)**. Er wird beim ersten `new` einmalig über `RtlAllocateHeap(XapiProcessHeap, 0xC, 0x2600020)` geholt, und zwar schon im **ersten ausgeführten C++-Initialisierer** während `_cinit` (Tabelleneintrag 0x2BDEF4; der erste Eintrag 0x2BDEF0 ist NULL). NetImmerse und Spielcode allozieren über `FUN_000a4900`/`FUN_000a48e0` und geben über `FUN_000a3880` frei. | 0xA4520, 0xA4920, 0x17E010 |
| GPU-/Audio-Speicher | Kontiguierlicher Speicher kommt nur aus XDK-Code (D3D, DSOUND, XMV, XPP) und aus dem Movie-Player (2× `XPhysicalAlloc`). Typische Parameter: `MmAllocateContiguousMemoryEx(n, 0, 0x7FFFFFF, align, 0x404)`. | `startup_kcallsites.py 56` |
| TLS | `IMAGE_TLS_DIRECTORY` hat keine Rohdaten, 0x20 Byte Zero-Fill und keine Callbacks; der Index liegt bei 0x2F3D18. `entry` setzt TlsDataSize = 0x24 und `_tls_index` = −9. Zugriffe gehen über **`[fs:[4] − 0x24]`**, also über StackBase. Belegung: +4 LastError, +8 CRT-ptd, +0xC aktueller Fiber, +0x10…0x1F Fiber-Struktur des Hauptthreads. | 0xF84F5, 0xF99DA, `startup_tls.py` |
| KPCR-Abhängigkeiten | XAPI und CRT lesen `fs:[0x20]` (+0x250 Debug-Monitor), `fs:[0x24]` (IRQL), `fs:[0x28]` (KTHREAD: +0x1C, +0x28 TlsData, +0x12C ThreadId) und `fs:[4]`. Unter Windows kollidiert das mit `TEB.ClientId` und `ActiveRpcHandle`. Die betroffenen Funktionen sind **vollständig aufgelistet** (Kapitel 7) und sollten gehookt werden. | `startup_fsscan.out` |
| Exceptions | Es gibt viele SEH/C++-EH-Frames (`fs:[0]` in ≥1419 Funktionen), aber **kein C++-`throw`**. `RaiseException` ruft nur der CRT-FP-Fehlerpfad auf. `terminate` endet in `KeBugCheck(0xC0000144)`. | 0x16AFF0, 0x169D5E |
| Ende | Gibt `main` zurück, folgt `XapiBootToDash(1,1,0)` → `XLaunchNewImageA(NULL, LD_LAUNCH_DASHBOARD)` → `HalReturnToFirmware(2)`. `XapiBootToDash` ist **nicht** `noreturn` (`ret 0xC` bei 0xF9023): Kehrt ein HLE-`HalReturnToFirmware` zurück, laufen `XapiInitProcess` bzw. der Hauptthread einfach weiter. Der Loader muss dort den Prozess beenden. Der einzige weitere Launch ist das Speicher-Dashboard (0xE4B00, Reason 2, Laufwerk 'U'; Auslöser laut XDK-Konvention zu wenig Platz auf U:, **Vermutung**). `_cexit`/`doexit` laufen beim normalen Ende nicht, die 77 `atexit`-Registrierungen werden also nie ausgeführt. | 0xF84D3, 0xF8FCA, 0xF829C |

---

## 1. Aufrufbaum vom Einsprung bis zur Hauptschleife

```
entry 0x000F84DD                                  (Initialthread des Kernels)
├─ Zertifikatsgröße begrenzen: cert->Size = min(cert->Size, SizeOfHeaders-cert+0x10000)
├─ TlsDataSize [0x310394] = ((EndRaw-StartRaw+ZeroFill+0xF)&~0xF)+4 = 0x24
├─ *TLS.AddressOfIndex (0x2F3D18) = -(0x24/4) = -9           (= XAPILIB::_tls_index)
├─ XAPILIB::CreateThread(NULL, 0, 0xF8469, NULL, 0, NULL)     0xF9A72
│    └─ PsCreateSystemThreadEx(&h, 0, KernelStack=[0x10130]=0x10000, TlsDataSize=0x24,
│                              ThreadId, StartCtx1=0xF8469, StartCtx2=NULL, Suspended=0,
│                              Debugger=0, SystemRoutine=XapiThreadStartup 0xF99DA)
├─ bei Fehler: XapiBootToDash(1,1,0)
└─ CloseHandle(h); ret                                       (Initialthread endet)

XapiThreadStartup 0x000F99DA                       (SystemRoutine jedes XAPI-Threads)
├─ blk = KTHREAD(fs:[0x28])->TlsData(+0x28) + 4 ; *(blk-4) = blk
├─ memcpy(blk, TLS raw, 0) ; memset(blk+0, 0, 0x20)
├─ XapiCallThreadNotifyRoutines(TRUE)                        0xF98B5
├─ StartRoutine(param)            ──► 0xF8469
├─ XapiCallThreadNotifyRoutines(FALSE)
├─ __except(UnhandledExceptionFilter 0xF999E)
└─ Ord. 258 (Aufruf bei 0xF9A6B mit einem Argument = Rückgabewert der StartRoutine, danach int3;
                Ghidra ohne Namen; Vermutung: PsTerminateSystemThread(ExitStatus))

Hauptthread 0x000F8469                    (keine Ghidra-Funktion; Stub zwischen XAPI-Funktionen)
├─ FUN_000f9804   XDK-Kernel-Hotfix (schreibt je nach Kernel-Build ein Kernel-Dword; alt/neu unterscheiden sich in 1 Byte)
├─ XAPILIB::XapiInitProcess 0xF92C0             (Kapitel 2.3)
├─ falls KPRCB(fs:[0x20])+0x250 != 0 (Debug-Monitor): TLS-Offset an den Monitor melden
├─ XAPILIB::_rtinit 0xF97DB  → Tabelle 0x2BDEE0..0x2BDEEC: 0x169467 (_mtinit), 0x16EDEA (_ioinit)
├─ XAPILIB::_cinit  0xF9783  → [0x2D85FC]=0x1668A9 (FP-Init), C-Init 0x2D5B34..0x2D5B48,
│                              C++-Init 0x2BDEF0..0x2D5B30 (24.336 Einträge)
├─ FUN_000d29b0(0,0,0)   = main                       (Kapitel 2.5)
├─ XapiBootToDash(1,1,0)                              (nur, wenn main zurückkehrt)
└─ ret 4

main 0x000D29B0
├─ app = new(0x108) CApp   [FUN_000a4900 → Basis-ctor FUN_000a2240, vtable 0x2A3EF8]
├─ g_pApp [0x2EB310] = app
├─ if (app->Init())      vf2 = 0xD26A0
│     while (app->Frame())  vf3 = 0xD2630           ◄── Hauptschleife
├─ app->Shutdown()       vf4 = 0xD2920
├─ app->~CApp(1)         vf0 = 0xD2AF0
└─ return 0
```

Beleg für `main` und die Reihenfolge: Disassembly 0xF8469–0xF84DA:
`call 0xF9804; call 0xF92C0; …; call 0xF97DB; call 0xF9783; push 0;push 0;push 0; call 0xD29B0; add esp,0xC; push 0;push 1;push 1; call 0xF8FCA; xor eax,eax; ret 4`.

---

## 2. Initialisierung im Detail

### 2.1 `entry` (0xF84DD)

Die Disassembly zeigt die Berechnung aus den TLS-Feldern: `mov eax,[0x2A5D3C]` (ZeroFill), `sub eax,[0x2A5D2C]`
(StartRaw), `add` EndRaw, `+0xF`, `and ~0xF`, `+4`, Ergebnis nach `[0x310394]`. Danach folgen `idiv -4` und
`mov [AddressOfIndex], eax`. Ghidra zeigt das konstant gefaltet als `DAT_00310394 = 0x24; _tls_index = 0xfffffff7`.

Der Hauptthread bekommt die Stackgröße aus dem XBE-Header, Feld 0x130 („PE stack commit“), also **64 KiB**.
`CreateThread` setzt bei `dwStackSize == 0` den Wert `mov eax,[0x10130]` ein.

### 2.2 `FUN_000f9804` – Kernel-Hotfix der XDK

Die Funktion liest `XboxKrnlVersion->Build` (Offset +4, Ord. 324) und `XboxHardwareInfo->Flags` (Ord. 322, Bit 1).
Bei den Builds 0xF68/0xFC2/0xFC7/0x12D1–0x12D2 (3944, 4034, 4039, 4817, 4818) patcht sie über
`MmQueryAddressProtect`/`MmSetAddressProtect` ein Kernel-Dword von 0x20C5048D auf 0x28C5048D.
**Für den Loader:** `XboxKrnlVersion.Build` auf einen anderen Wert setzen (z. B. 5838). Dann kehrt die Funktion
ohne Zugriff zurück.

### 2.3 `XAPILIB::XapiInitProcess` (0xF92C0)

Die Reihenfolge ist aus der Disassembly 0xF92C0–0xF9466 übernommen. Ghidra hat die InitFlags (`[0x10124]` = 5)
konstant gefaltet; die tatsächlichen Abfragen stehen in der rechten Spalte.

| # | Schritt | Kernel-/XAPI-Aufrufe | Bedingung / Fehlerfolge |
|---|---|---|---|
| 1 | Shutdown-Notifikation registrieren, Record 0x2D7530 = {Routine 0xF9C7F, Priority 0x80000000} | `HalRegisterShutdownNotification` (Ord. 47) | – |
| 2 | Auto-Power-Down (`FUN_000f9b1a`): DPC 0x310340 (Routine 0xF9B08), Timer 0x310360 | `KeInitializeDpc` (107), `KeInitializeTimerEx` (113), `XQueryValue(0x11)` → `ExQueryNonVolatileSetting` (24), `XAutoPowerDownResetTimer` → `KeSetTimer(timer, -0x324A9A7000 (6 h), dpc)` (149) | `XQueryValue` nur, wenn `!(XboxHardwareInfo->Flags & 8)`. Die DPC ruft nur bei aktivierter Einstellung Ord. 360 auf (Ghidra ohne Namen; Vermutung: `HalInitiateShutdown`). |
| 3 | **XAPI-Prozess-Heap**: `RtlCreateHeap(HEAP_GROWABLE=2, NULL, Reserve=[0x10134]=1 MiB, Commit=[0x10138]=4 KiB, NULL, &params{Length=0x30})` → `[0x31039C]` (Ghidra: `FUN_000f4ea8`; der Name `RtlCreateHeap` und die Flag-Namen sind Vermutung nach der Argumentform) | `FUN_000f4ea8` (Heap-Implementierung in XAPI): `NtAllocateVirtualMemory` (184), `NtQueryVirtualMemory` (217), `RtlInitializeCriticalSection` (291) | NULL → `XapiBootToDash(1,1,0)` (0xF9315 `je 0xF9362`; dort `eax=ecx=ebx=1`, `[ebp-4]=0`) |
| 4 | DVD-Prüfung `FUN_000f90e6`: `NtOpenFile("\Device\CdRom0")`, IOCTL 0x4D014 (SCSI-Passthrough, CDB 0x5A, ggf. 0xAD) | `NtOpenFile` (202), `NtDeviceIoControlFile` (196), `NtClose` (187) | nur bei `cert->AllowedMedia == 2` oder SecurityFlags&1 (hier: AllowedMedia = 2). **Rückgabewert wird ignoriert** (`test eax,eax; jmp`). |
| 5 | Prüfung von partition1: `FUN_000f8ae2("\Device\Harddisk0\partition1\", 0x4000)` mit `NtQueryVolumeInformationFile(FileFsSizeInformation)`; SectorsPerAllocationUnit·BytesPerSector muss **0x4000** ergeben | `NtOpenFile`, `NtQueryVolumeInformationFile` (218) | wenn `!(InitFlags & 8)`. Fehler → `XapiBootToDash(1,2,0)` |
| 6 | `IoCreateSymbolicLink("\??\D:", "\Device\CdRom0")` | Ord. 67 | wenn `!(XboxHardwareInfo->Flags & 8)` (Vermutung: Flag 0x08 = Arcade/Chihiro); `STATUS_OBJECT_NAME_COLLISION` (0xC0000035) ist erlaubt, jeder andere Fehler → `XapiBootToDash(1,1,0)` |
| 7 | `FUN_000f8f75(TitleID 0x46490002, cert+0xC TitleName)`: T: → `\Device\Harddisk0\partition1\TDATA\46490002`, U: → `…\UDATA\46490002`. Dabei werden TitleMeta.xbx, TitleImage.xbx und SaveImage.xbx geschrieben (XAPI kennt die Sektionsnamen `$$XTIMAGE`/`$$XSIMAGE`; dieses XBE enthält nur `$$XTIMAGE`, nicht vorgeladen). | `XapiMapLetterToDirectory` 0xF8C41: `NtCreateFile` (190), `NtWriteFile` (236), `NtSetInformationFile` (226), `NtQueryInformationFile` (211), `KeQuerySystemTime` (128), `XeLoadSection`/`XeUnloadSection` (327/328), `IoCreateSymbolicLink` | wenn `!(InitFlags & 0x18)`. `0xC000007F` → `XapiBootToDash(2,'T',0x10)`, andere Fehler → `(1,2,0)` |
| 8 | `XMountUtilityDrive(InitFlags & 2)` → Z: auf der Cache-Partition | `XapiSelectCachePartition` 0xF7D51 (`NtOpenFile`/`NtReadFile`/`NtWriteFile`), `FUN_000f946d` (Formatierung: `NtFsControlFile`, `NtDeviceIoControlFile`), `IoCreateSymbolicLink` | wenn `!(InitFlags & 8) && (InitFlags & 1)`. Fehler → `XapiBootToDash(1,2,0)` |
| 9 | Jugendschutz: `XGetParentalControlSetting()` = `ExQueryNonVolatileSetting(0xA)` ≤ `cert->GameRatings` (= 0) | Ord. 24 | sonst `XapiBootToDash(1,4,rating)`. **Der Loader muss für Index 0xA den Wert 0 liefern.** |
| 10 | LaunchDataPage auswerten, XAPI-Init-Tabelle 0x2D5B64..0x2D5B68 abarbeiten (einziger Eintrag ist 0) und die Page bei Typ 0xFFFFFFFF freigeben | `LaunchDataPage` (Daten, 164), `MmFreeContiguousMemory` (171) | – |

Die Shutdown-Routine 0xF9C7F hängt Callbacks in das Objekt von Ord. 357 ein (Datenimport ohne Ghidra-Namen) und
wartet mit `KeWaitForSingleObject` (159) auf das Event 0x2D7540. Für den Loader kann
`HalRegisterShutdownNotification` ein No-op sein.

### 2.4 `_rtinit` / `_cinit`

| Tabelle | Inhalt | Beleg |
|---|---|---|
| `_rtinit` 0x2BDEE0–0x2BDEEC | 0x169467 = CRT `_mtinit`: legt die per-Thread-Daten an (0x84 Byte via `FUN_0016eca7`), speichert sie in **TLS +8** und registriert `XRegisterThreadNotifyRoutine(0x310058)`. 0x16EDEA = `_ioinit`: `malloc(0x500)` für 32 lowio-Handles, die ersten drei mit Flag 0xC1. | `startup_getfunc.py 0x169467 0x16edea` |
| `_cinit` Vorlauf `[0x2D85FC]` | 0x1668A9 (FP-Initialisierung, `FUN_00166871` + `FUN_001692a2`) | 0xF9783 |
| C-Init 0x2D5B34–0x2D5B48 | `___onexitinit` 0x166EE2, 0x16C29B (stdio-Tabelle), 0x16DDB0 (Multibyte-Codepage), 0x16ED64 = `SetUnhandledExceptionFilter(0x16ED1E)` (CRT-terminate-Filter) | Decompilat |
| C++-Init 0x2BDEF0–0x2D5B30 | 24.336 Einträge, davon 24.335 ungleich null und alle verschieden (Eintrag 0 bei 0x2BDEF0 ist NULL). 21.791 rufen als viertes Befehlswort direkt `NiStream::RegisterLoader` (0x1087D0) auf. 24.329 liegen in 0x17E010–0x233880, die übrigen 6 sind **XDK-Initialisierer** (siehe unten). | `verify_startup_cinit.py` |

**Wichtig:** Schon der erste ausgeführte C++-Initialisierer (zweiter Tabelleneintrag 0x2BDEF4 → 0x17E010,
`RegisterLoader("NiExtraData")`, Name über `[0x2D77C0]` → 0x2A6A78) ruft `FUN_000a4900` auf und legt damit den
38-MiB-Spielpool an (Kapitel 5.2). Der XAPI-Heap muss also vor `_cinit` funktionieren. Das ist auf dem
Xbox-Original durch Schritt 3 von `XapiInitProcess` gegeben.

**XDK-Initialisierer in der C++-Tabelle** (laufen während `_cinit` nativ, auch wenn D3D/DSOUND/XPP später ersetzt
werden):

| Tabelleneintrag | Funktion | Wirkung |
|---|---|---|
| 0x2D3E68 | XPP 0x28D254 | kopiert den Wert des Kernel-Imports Ord. 74 (Slot 8, `IoInvalidDeviceRequest`, Datenimport ohne Aufruf) nach 0x28C004…0x28C038 (10 Dwords im XPP-Abschnitt, Vermutung: Dispatch-Tabelle eines Treiberobjekts) |
| 0x2D3E6C | D3D 0x235770 | kopiert 4 Dwords von 0x2421B4…0x2421C0 (D3D-Abschnitt) nach 0x2D75F4…0x2D7600 (`.data`) |
| 0x2D3E70…0x2D3E7C | DSOUND 0x24968F, 0x24969A, 0x24F3D4, 0x24F3DF | schreiben je einen Zeiger (0x263AF4…0x263B00) nach 0x2641C8/0x2641C4/0x264244/0x264240 (DSOUND-Abschnitt) |

Der Loader muss daher D3D, DSOUND und XPP beschreibbar mappen (die XBE-Sektionsflags setzen dort ohnehin
Writable, `verify_startup_sections.py`) und für Ord. 74 einen Wert in Slot 8 eintragen (Ghidra benennt ihn
`IoInvalidDeviceRequest_exref`; bei ersetztem XPP genügt ein Dummy, sonst die Adresse einer HLE-Funktion).

### 2.5 `main` und die Anwendungsklasse

| Ebene | ctor | vtable | Bemerkung |
|---|---|---|---|
| Basis 0 | `FUN_001091a0` | 0x2A6A10 | NetImmerse-Klasse aus derselben Übersetzungseinheit wie `NiStream::RegisterLoader`. Sie legt Arrays mit 0x400 Einträgen an (`FUN_00107c60(0x400,0x400)`). **Vermutung:** `NiStream`. |
| Basis 1 | `FUN_000d1f60` | 0x2A3EE0 | setzt +0x20 = 0x1000, +0x24/+0x28 = 0x20 |
| Spielbasis | `FUN_000a2240` | 0x2A1810 | `g_pGame [0x2E7B84] = this`, Kontextliste, `FUN_000f3b60` (0xC0) → +0xA4, `[0x2E7B7C] = FUN_0002c440` (0x38) |
| CApp | in `main` inline | **0x2A3EF8** | Größe 0x108 |

vtable 0x2A3EF8 (CApp) und 0x2A1810 (Basis):

| Slot | CApp | Basis | Bedeutung |
|---|---|---|---|
| 0 | 0xD2AF0 | 0xA2680 | Deleting-Destruktor |
| 1 | 0x107980 | 0x107980 | geerbt von Basis 0 |
| 2 | **0xD26A0** | 0xA26A0 | `Init()` (CApp ruft am Ende die Basis auf) |
| 3 | **0xD2630** | 0xA2C20 | `Frame()` (CApp ruft die Basis auf) |
| 4 | **0xD2920** | 0xA2640 | `Shutdown()` |
| 5 | 0xD2690 | 0x1668B6 `_purecall` (`push 0x19; call 0x1692B4`) | `GetRenderer()` = `return this->+0x104` (in der Basis rein virtuell) |
| 6 | 0xA2380 | 0xA2380 | `PlayMovie(name)`: Name `"%s-%s"` mit Tag `"PAL"`/`"NTSC"` aus 0x2E7B70, Datei `Video\%s.xmv` |
| 7 | 0xA24C0 | 0xA24C0 | Movie-Status (`this->+0xE4`) |
| 8 | 0xA24A0 | 0xA24A0 | `if (vf7()) FUN_000ede90()`: Movie-Frame dekodieren und anzeigen |
| 9 | 0xAED30 | 0xAED30 | leer (`ret 4`) |
| 10 | **0xD25D0** | – | Pro-Schritt-Update: Controller, Sound, Pause |

Wichtige Felder von CApp (aus ctor, Init und Frame):

| Offset | Inhalt |
|---|---|
| +0xA4 | Objekt `FUN_000f3b60` (0xC0) |
| +0xA8 | Objekt `FUN_000a2200` (0x1FC; verwendet `D3DDevice_SetScreenSpaceOffset`) |
| +0xAC | Start-Levelname `"Level1-1"` |
| +0xB8 / +0xBC / +0xC0 | Kontextliste / aktueller Kontext / nächster Kontext |
| +0xC8 | Zeitpunkt des letzten Simulationsschritts (64-Bit-Ticks) |
| +0xD0 / +0xD8 / +0xE0 | FPS-Zähler / FPS-Fensterstart / gemessene FPS (float) |
| +0xE4 | Movie-Player (XMV) |
| +0xE8 | **Videonorm: 0 = PAL, 1 = NTSC** |
| +0xF0 | Sound-Manager (vtable 0x2A3B80) |
| +0xF4 | Objekt `FUN_000e4aa0` (0xA4). Byte +0x0A schaltet das Pause-Overlay (`FUN_000e52d0`). Vermutung: Pausemenü. |
| +0xFC | Objekt `FUN_00098440` (8 Byte) |
| +0x100 | Hint-System (`FUN_000255e0`, `Language/HINTS_XBOX.ldb`) |
| +0x104 | Renderer (`NiPointer<NiXBoxRenderer>`) |

### 2.6 Init-Reihenfolge des Spiels (`CApp::Init` 0xD26A0 → `Game::Init` 0xA26A0)

| # | Adresse (Aufruf) | Subsystem | Details |
|---|---|---|---|
| (0) | 0x17E010 (während `_cinit`) | **Speichermanager** | Lazy-Init des 38-MiB-Pools beim ersten `new` |
| 1 | 0xD26DA `FUN_0014c170(640,480,2,1,0x14000002,0x4B,0,1)` | **Renderer / D3D-Device** | `NiXBoxRenderer::Create` (0x6D0 Byte, ctor `FUN_0014af00`): `D3D_SetPushBufferSize(0x200000, 0x8000)`, `Direct3D_CreateDevice(0, 1, NULL, BehaviorFlags=0x50, &pp, &dev)`. Presentation-Parameter: 640×480, Format 0x1E, 1 Backbuffer, MultiSampleType 0x11 (`FUN_00146180`, kein Bit in 0xF00/0xF0000 gesetzt), SwapEffect 1 (`FUN_00146260`), AutoDepthStencil = 1 mit Format 0x2A, Flags = 1, Refresh = 0, **PresentationInterval = 1**. Die symbolischen D3D-Namen (0x50 = PUREDEVICE\|HARDWARE_VERTEXPROCESSING, Interval 1 = ONE) sind Vermutung nach den d3d8-Headern; die Zuordnung 0 → 0x80000000 im Code stützt sie. |
| 2 | 0xD270A `FUN_0009c2a0` | **Input** | `XInitDevices(1, {g_DeviceType_Gamepad, 4})`, 4 Controller-Objekte (0x19C, `[0x2E6F84]`), `XGetDevices(Gamepad)`, `XGetDevices(MU)`, `XInputOpen` pro Port |
| – | – | Abbruch | Liefert `FUN_0014c170` NULL (kein Device), gibt `Init` false zurück; `main` überspringt dann die Schleife und ruft `Shutdown` auf. |
| 3 | Renderer-vf27 (+0x6C) mit {0,0,0} | Renderer | Hintergrundfarbe (Vermutung) |
| 3a | `FUN_000a4900(8)` → `FUN_00098440` → +0xFC | ? | Objekt +0xFC wird vor dem Einhängen der Archive angelegt |
| 4 | `thunk_FUN_0013fe70("sysPAL.img")`, sonst `"sysNTSC.img"` | **Dateisystem/Archive** | `.img`-Archiv einhängen. PAL → +0xE8 = 0 und Sprachwahl `FUN_000d9990` (`XGetLanguage` = `ExQueryNonVolatileSetting(7)`: 3 german, 4 french, 5 spanish, 6 italian, sonst english) |
| 5 | `FUN_000f33e0` | Fehlermeldungs-UI | `Language/XBOXERROR.ldb`, `HUD\ErrorIcon.nif` |
| 6 | `FUN_000e4aa0` → +0xF4 | Pause-/Statusobjekt | liest fps aus `[0x2D6744]` |
| 7 | Schleife `FutPAL%02d.img` (bis 99), sonst `FutNTSC%02d.img` | **Archive** | setzt +0xE8 erneut (0 = PAL; 1 nur, wenn FutNTSC-Archive gefunden werden; ohne beide bleibt 0) |
| 8 | `FUN_000ccd00` → +0xF0 | **Sound** | `DirectSoundCreate(NULL, &+0x48, NULL)`, `XBOX\dsstdfx.bin`, `DirectSoundUseFullHRTF`, Listener-Parameter (`SetPosition`, `SetRolloffFactor`, `SetDopplerFactor`, `SetDistanceFactor`, `SetI3DL2Listener`) |
| 9 | `FUN_000255e0` → +0x100 | Hints | `Language/HINTS_XBOX.ldb` |
| 10 | `FUN_000e51c0` | Pausemenü | `sounds/menu/Pause_fold_*.wav` |
| 11 | `FUN_000a26a0` (Basis-Init) | Zeitbasis | PAL: fps 50, dt 0,02 (0x3CA3D70A), Tag „PAL“. NTSC: fps 60, dt 1/60 (0x3C888889), Tag „NTSC“. `[0x2D674C]` = 1 schaltet den festen Zeitschritt ein. Bei +0xE8 == 0 (PAL) läuft die Sprachwahl (`FUN_000d9990` usw.) hier ein zweites Mal. |
| 12 | – | Renderer | zweimal `ClearBuffer(NULL,5)` (vf17) und `DisplayFrame` (vf16): zwei schwarze Frames |
| 13 | `FUN_001188a0(1)`, `FUN_00111940(1)`, `NiAlphaAccumulator` (0x44) | NetImmerse | Sorter am Renderer (Renderer+8) |
| 14 | `FUN_0002ca60` (0x10D8) → `[0x2DB79C]` | Profiler | Sektionen „Total“, „Context::Render“, „Sound“. Die Aufrufe landen in leeren Stubs (0xAED30 `ret 4`, 0xB0390 `ret`, 0xA3210 `mov al,1; ret`). |
| 15 | `FUN_000a2200` (0x1FC) → +0xA8 | Display-Einstellungen? | `D3DDevice_SetScreenSpaceOffset` |
| 16 | Kontexte: `"Intro"` 0x2B440, `"Frontend"` 0x22650, `"Attract"` 0x2BA80, `"Ingame"` 0x2A1F0, `"Death"` 0x2BF50 | **Frontend/Spielzustände** | Basis-ctor 0x2C320 hängt jeden Kontext in die Liste +0xB8 ein. Der erste (Intro) wird aktueller Kontext +0xBC. |
| 17 | `FUN_000d76d0` (0xC) → `[0x2EBD0C]` | ? | wird im Frame mit `FUN_000d7630` gezeichnet |
| 18 | `FUN_000d4dc0` → `"Intermission"` (0xD37C0, 0x148) → `[0x2EB7C8]` | Kontext | Event-Handler `"SOUND"` (0xB0390) und `"LIPSYNC"` (0xD2FA0) über `FUN_000d4e50` |
| 19 | `Intro->vf1()` (Enter) | Frontend | Erfolg → Zeitbasis +0xC8/+0xD8 = QPC, `BUILD.TXT` nach 0x2E7A68 (max. 255 Zeichen) |
| später | `Session`-Ladezustandsautomat in 0xC5B40 (11 Zustände, Sprungtabelle 0xC6FB4) | **Skriptsystem** | pro Level: `FUN_000ba170(session+0xA0, "scripts\%s.Ful")`, bei Fehler Meldung „SCRIPT IS NOT RUNNING!“, Start über `FUN_000b9f10`. Geladen wird über mehrere Frames verteilt. |

Kontext-vtable (Basis 0x295380, z. B. Ingame 0x295140, Intermission 0x2A3FC0): [0] Deleting-Destruktor,
[1] `Enter()`→bool, [2] `Update()`→bool (false bedeutet Kontextwechsel), [3] `Render()`, [4] `Exit()`, [5] leer.

---

## 3. Hauptschleife

### 3.1 `CApp::Frame` (0xD2630)

```
renderer->vf17(NULL, [0x2D6740]=4)       NiXBoxRenderer::ClearBuffer 0x146490 (SetViewport, D3DDevice_Clear)
this->vf8()                              Movie: ggf. XMV-Frame dekodieren (0xA24A0 → 0xEDE90 → XMV 0x264962)
if (!Game::Frame()) return false         0xA2C20 (Simulation + Render-Aufruf)
if (pauseObj->byte[0xA]) FUN_000e52d0()  Pause-Overlay
FUN_000f2cf0()                           Debug-Overlay ("%dfps", "x=%d y=%d z=%d")
NiAVObject::vf28(this->+0x100)           Stub 0xA3210
renderer->vf16()                         NiXBoxRenderer::DisplayFrame 0x146460 → D3DDevice_Swap(0)
return true
```

### 3.2 `Game::Frame` (0xA2C20): fester Zeitschritt

Pseudocode nach Decompilat und Disassembly 0xA2D00–0xA3030. Die Hilfsfunktionen:
`FUN_000d2b90` = jetzt (QPC), `FUN_000d2b30` = Differenz, `FUN_000d2b80` = Ticks → Sekunden (× `[0x2EB450]` = 1/QPF),
`FUN_000d2b10`/`FUN_000d2b50` = Sekunden → Ticks.

```
if (cur == NULL) return false;
FUN_000d2370();  FUN_000f2050();  FUN_000f2680();     // Async-Ladewarteschlange, Streaming (pumpen die Fiber)
now = QPC(); elapsed = now - last(+0xC8);
fpsCount(+0xD0)++; if (now-fpsStart(+0xD8) > 1.0 s) { fps(+0xE0) = n/t; ... }
if (sec(elapsed) > 6*dt) elapsed = ticks(6*dt);          // Spiral-of-Death-Bremse
if (!fixedStep [0x2D674C]) { dt = clamp(sec(elapsed), [0x2E7A58], [0x2E7A5C]); fps = 1/dt; }
for (n = 100;; n--) {                                     // mov edi,0x64
    if (sec(elapsed) < dt || n < 1) {                     // --- Render ---
        last = now - elapsed;
        FUN_000a46d0();                                   // Speicherpflege: alle 41 bzw. 201 Aufrufe
        g_frameDelta [0x2E7B88] = sec(QPC() - prevRender); // echte Frame-Dauer (float)
        cur->Render();                                    // vf3
        FUN_0002c9c0();  if ([0x2EBD0C]) FUN_000d7630();
        return true;
    }
    app->vf10();          // 0xD25D0: XGetDeviceChanges/Controller-Poll, Sound->Update (DirectSoundDoWork), Pause
    FUN_0002c890();       // Eingaben an die Kontexte
    if (!cur->Update()) { // vf2
        cur->Exit(); FUN_0002a660(); FUN_00108410(); Sorter freigeben;
        cur = next(+0xC0); if (!cur) return false; next = 0;
        FUN_000a48c0(); neuer NiAlphaAccumulator; if (!cur->Enter()) return false;
        FUN_0009be10(); cur->Update();
    }
    elapsed -= ticks(dt);
}
```

* **Fester Zeitschritt:** `[0x2D674C]` ist im `.data`-Abbild 1 und wird in 0xA26A0 erneut auf 1 gesetzt. dt steht in
  `[0x2D6748]` (280 Referenzen im Code, `startup_dataref.py 0x2d6748`), fps in `[0x2D6744]`.
* Der variable Zweig ist praktisch tot: `[0x2D674C]` hat nur zwei Code-Referenzen (Schreiben 0xA2756, Lesen 0xA2D25),
  und selbst dort würde dt auf `[0x2E7A58]` … `[0x2E7A5C]` geklemmt. Beide setzt 0xA26A0 auf denselben Wert dt
  (`verify_startup_r2_extra.py dt`, Decompilat 0xA26A0).
* **Schrittzahl:** Der Zähler `edi = 100` greift praktisch nie. Die Zeitschuld wird vor der Schleife auf 6·dt gekappt
  (0xA2CE8 `fld [0x2D6748]; fmul [0x29BFF4]` mit 6.0f) und innerhalb der Schleife nicht neu gemessen. Pro Frame laufen
  also höchstens etwa 6 Simulationsschritte (genau 6 oder 5, je nach Rundung bei Ticks↔Sekunden).
* Gerendert wird **ohne Interpolation**: `Render()` bekommt keinen Alpha-Wert. Die Spielzustände ändern sich also
  im 50-Hz-Raster, auch wenn öfter gerendert wird (Vermutung aus dem Fehlen eines Alpha-Parameters).
* Weder in der Schleife noch in `CApp::Frame` steht ein `Sleep`. `Sleep` rufen nur XMV (0x2646D5 in 0x264677) und
  D3D-Code auf (0x2419B5, 0x241BF2, 0x242045; ein Capture-Pfad, den `D3DDevice_Swap` bei 0x24108C über
  `FUN_00241c40` erreicht). `SleepEx(0, TRUE)` rufen der Dateicode (0x13F514, 0x13F59E, 0x13F5F9) und die kleine
  Funktion 0xE0030 (`SleepEx(0,1); return true;`, vtable-Eintrag 0x2A4DE8, Klasse in der Nähe der Strings
  `"U:\"`/`"Futurama Options"`; Vermutung: Spielstand-/Optionsverwaltung) auf. Das Tempo bestimmen also `Swap`
  (VSync) und der feste Zeitschritt.
* **Weitere Präsentationen außerhalb des Render-Pfads:** Beim Kontextwechsel innerhalb der Update-Schleife ruft
  `FUN_0002a660` achtmal `ClearBuffer(NULL,1)` + `DisplayFrame` auf (Hintergrundfarbe aus 0x30E8A0, dazwischen
  `FUN_0013cf90` und Renderer-vf71). Weitere `DisplayFrame`-Aufrufer sind `FUN_000a26a0` (2×), `FUN_000a62b0`
  (Menü), `FUN_000c8e90` und der Movie-Pfad `FUN_000ee080` (Liste in `nixboxrenderer_vtable.tsv`). `DisplayFrame`
  ruft `Swap(0)` nur auf, wenn `[[renderer+0x58]+0x10] == 0` ist (0x146463–0x14646B).

### 3.3 Zeitbasis

| Funktion | Code | Folge |
|---|---|---|
| `XAPILIB::QueryPerformanceCounter` 0xF641B | `rdtsc; mov [ecx],eax; mov [ecx+4],edx; mov eax,1; ret 4` | TSC des 733-MHz-Pentium-III |
| `XAPILIB::QueryPerformanceFrequency` 0xF642C | `mov dword [eax],0x2BB5C755; and dword [eax+4],0; mov eax,1; ret 4` | Konstante 733.333.333 Hz |

Nur `FUN_000d2b90` (Spieltimer), XMV (0x264360, 0x264775, 0x264962, 0x264FDF) und D3D-Code verwenden QPC/QPF
(D3D: QPC bei 0x241A12/0x241A61/0x241B45 und QPF bei 0x241B4E, alle außerhalb von Ghidra-Funktionen, sowie QPF bei
0x241D4E in `FUN_00241c40`, dem Capture-Pfad aus `D3DDevice_Swap`). Direktes `rdtsc` steht außerdem nur in D3D 0x23B780
(`rdtsc; ret`, Aufrufer 0x23B98B und 0x23BD96). Die Bytes `0F 31` bei DSOUND 0x260BDF und XMV 0x274509 liegen in
Datentabellen und sind kein Code (`verify_startup_misc.py timer`).
`KeQueryPerformanceCounter` (126) und `KeTickCount` (156) werden **nicht** importiert.
**Loader:** 0xF641B und 0xF642C durch Win32-`QueryPerformanceCounter`/`-Frequency` ersetzen. Die Spiellogik ist
dann unabhängig von der PC-TSC-Frequenz.

### 3.4 PAL 50 Hz und NTSC 60 Hz

* Die Norm entscheidet allein, welche Archive gefunden werden (CApp+0xE8). `XGetVideoStandard`/`XGetVideoFlags`
  ruft der Spielcode nicht auf (keine Treffer in functions.tsv). Die EU-Disc enthält `SysPAL.img` und
  `FutPAL0x.img`, das Spiel läuft also **immer mit dt = 0,02 s**.
* Die Norm bestimmt fps/dt (0xA26A0), den Movie-Namen (`"%s-%s"` mit „PAL“/„NTSC“), die Sprachwahl
  (nur bei PAL) und die Splash-/Legal-Bilder im Intro-Kontext: Code ohne Ghidra-Funktion bei 0x2B65B/0x2B814 liest
  `[g_pGame 0x2E7B84]+0xE8` und wählt `images\english\NI_Splash_PAL.nif` (0x29526C) bzw. `…NI_Splash_NTSC.nif`
  (0x295248) sowie `images\%s\legal.nif` (0x2952C8) bzw. `images\%s\legal-NTSC.nif` (0x2952AC)
  (`verify_startup_r2_extra.py splash`).
* Absolute Code-Referenzen der Zeit-Globals (`verify_startup_r2_extra.py dt`): dt `[0x2D6748]` 280; fps `[0x2D6744]`
  nur 0xA26A0, 0xA2C20 (variabler Zweig) und 0xE4AA2 (Pause-Objekt); `[0x2E7A58]`/`[0x2E7A5C]` nur 0xA26A0/0xA2C20;
  `[0x2E7A60]`, `[0x2E7B6C]`, `[0x2E7B68]` werden nur in 0xA26A0 geschrieben. Setzt man stattdessen CApp+0xE8 = 1,
  ändern sich auch die Movie- und Splash-Namen, und die Sprachwahl entfällt (Vermutung zur Wirkung, nicht getestet).
* `FullScreen_RefreshRateInHz = 0`: Die Bildwiederholrate legt D3D bzw. der AV-Modus fest. Was bei PAL60 passiert,
  ist ungeprüft. Vermutung: weiterhin 50 Simulationsschritte pro Sekunde, Rendern mit bis zu 60 Hz.
* Idee für den Port (Vermutung): 60 Hz ließen sich über dt/fps (0x2D6748/0x2D6744) erzwingen, weil der Code dt aus
  dem Global liest. Die Wirkung auf Animationen und Skripte ist ungeprüft.

### 3.5 Ende der Schleife

* `Game::Frame` gibt false zurück, wenn kein Kontext aktiv ist, wenn ein Kontext mit `Update()==false` keinen
  Nachfolger hat oder wenn `Enter()` des Nachfolgers fehlschlägt. Ob das im Retail-Spiel je vorkommt, ist ungeprüft.
* `CApp::Shutdown` 0xD2920: Sound-Manager löschen, `Game::Shutdown` 0xA2640 (aktueller Kontext `Exit()`,
  `[0x2EBD0C]` freigeben), Renderer freigeben, +0xFC und +0x100 löschen, `thunk_FUN_0013fe70(0)` (Archive aushängen).
* Danach `XapiBootToDash(1,1,0)` 0xF8FCA: `LD_LAUNCH_DASHBOARD{dwReason=1, dwContext=0, dwParameter1=1, dwParameter2=0}`
  → `XLaunchNewImageA(NULL, &ld)` 0xF82E3 → `FUN_000f829c` → `FUN_000f81bb` (LaunchDataPage per
  `MmAllocateContiguousMemory(0x1000)` und `MmPersistContiguousMemory`) → `HalReturnToFirmware(2)` (Ord. 49,
  Aufruf 0xF82D2). Nur für TitleID 0xFFFE0000 gilt `HalReturnToFirmware(4)` (0xF9028). Die Bedeutungen der Reason-Codes 1 und 2
  (Fehler/Speicher) sind Vermutung nach XDK-Konvention.
* `XapiBootToDash` kehrt zurück, wenn `XLaunchNewImageA` zurückkehrt (0xF9021–0xF9023 `leave; ret 0xC`). Auf der Xbox
  kehrt `HalReturnToFirmware` nie zurück. Die Aufrufer verlassen sich darauf: Nach dem Heap-Fehler in `XapiInitProcess`
  (0xF936B) folgt direkt `ret`, danach würden `_rtinit`/`_cinit` ohne Heap laufen. **Loader:** `HalReturnToFirmware`
  bzw. `XLaunchNewImageA` müssen den Prozess beenden.
* Speicher-Dashboard: `FUN_000dc290` → `FUN_000e4b00(blocks)` ruft `FUN_000f8081("U:\")` auf. Diese Funktion
  liefert die **Clustergröße** (`NtQueryVolumeInformationFile(FileFsSizeInformation)`,
  SectorsPerAllocationUnit·BytesPerSector), nicht den freien Platz, und der Rückgabewert wird verworfen (`eax` wird bei
  0xE4B10 überschrieben). Danach folgt `XLaunchNewImageA(NULL, {2, 0, 'U'(0x55), blocks})`.
* `XLaunchNewImageA` fragt zuerst die Hook-Tabelle 0x2D5B6C…0x2D5B70 ab (einziger Eintrag 0, also wirkungslos).
* `_cexit`/`doexit` werden beim normalen Ende nicht aufgerufen. `doexit` 0x169F1B hat nur den Aufrufer `_exit`
  0x169FC5, und der ist nur über den Funktionszeiger `[0x2D86AC]` (`_aexit_rtn`) aus `_amsg_exit` 0x1692B4 erreichbar.
  Er übergibt quick = 1, die `atexit`-Liste (77 Aufrufe von `_atexit` 0x166F42) wird also nie abgearbeitet.

---

## 4. Threads und Synchronisation

### 4.1 Threads

| Thread | Erzeugung | Einstieg | Stack | Aufgabe |
|---|---|---|---|---|
| Initialthread | Kernel | `entry` 0xF84DD | – | erzeugt den Hauptthread und endet |
| **Hauptthread** | `CreateThread` → `PsCreateSystemThreadEx` (0xF9AAB, einziger Aufruf) | 0xF99DA → 0xF8469 | 0x10000 | **alles**: Init, Spiel, Render, Sound-DoWork, XMV-Dekodierung, Datei-I/O |

Den Beleg dafür liefern `startup_kcallsites.py 77` (einziger Call-Site 0xF9AAB), `startup_callers_raw.py 0xf9a72`
(nur 0xF8531) und `startup_dataref.py` (keine Zeiger auf `CreateThread` in den Daten). `KeSetBasePriorityThread`
und `KeSetPriorityThread` sind nicht importiert.

### 4.2 Fibers: kooperatives Streaming

| Funktion | Adresse | Verhalten (Disassembly) |
|---|---|---|
| `XAPILIB::ConvertThreadToFiber` | 0x17098E | Die Fiber-Struktur des Threads liegt **im TLS-Block bei +0x10**: [+0] param, [+4] = KTHREAD+0x1C, [+8] = 0. TLS+0xC zeigt auf diese Struktur. |
| `XAPILIB::CreateFiber(size, start, param)` | 0x1708B0 | `MmCreateKernelStack(max(size,0x3000) auf Seiten gerundet, FALSE)` (Ord. 169). Struktur am Stack-Top −0x10: [+0] param, [+4] StackBase, [+8] StackLimit, [+0xC] gespeicherter ESP. Der Startframe zeigt auf den Trampolin 0x17086B, die SEH-Kette des neuen Fibers beginnt mit −1. |
| `XAPILIB::SwitchToFiber` | 0x17094F | sichert ebp/esi/edi/ebx und `fs:[0]`, `cur->+0xC = esp`, `esp = new->+0xC`, `TLS+0xC = new`, stellt `fs:[0]` und die Register wieder her. **`fs:[4]`/`fs:[8]` bleiben unverändert.** |
| `XAPILIB::DeleteFiber` | 0x17093C | `MmDeleteKernelStack(fiber->StackBase, fiber->StackLimit)` (Ord. 170) |
| Trampolin | 0x17086B | `start(TLS+0xC->param)` unter `__try`. Kehrt der Start zurück, folgt `KeBugCheck(0)`. |

Nur die Dateiklasse benutzt Fibers:

* `File::File(name, job)` 0x13F250 (vtable 0x2A9E68, Interface 0x2A9E58 bei +8): Pfadpräfix `[0x2D7AD8]` + Name,
  `/` wird zu `\`. Zuerst wird im `.img`-Archiv gesucht (`FUN_0013fe10`), sonst
  `CreateFileA(GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0x68000000)`. Mit gesetztem `job` folgt einmalig
  `ConvertThreadToFiber(0)` (Flag `[0x30E594]`) und `CreateFiber(0x4000, 0x13F1A0, this)`.
* Fiber-Routine 0x13F1A0: `job->vf0(file)`, danach `done(+0x2E) = 1` und `SwitchToFiber(main)`.
* `File::Poll` 0x13F590 wechselt aus dem Hauptfiber in den Lade-Fiber. Läuft der Lade-Fiber selbst, wechselt sie
  zurück zum Hauptfiber und ruft `SleepEx(0, TRUE)` auf. Ist der Job fertig, folgt `DeleteFiber`. Ohne Job ruft
  `Poll` nur `SleepEx(0, TRUE)` auf. Aufrufer (Raw-Scan): `FUN_000d2370` (Async-Warteschlange, jeden Frame),
  `FUN_000f2680`, Sound-Streaming 0xCB6E0/0xCC030/0xCCB80, 0x13F730, `File::Read` 0x13F8F0 sowie 0xCCA56 und
  0xD20A2 (außerhalb von Ghidra-Funktionen).
* Async-Jobs entstehen in `FUN_000d21f0`: `File(req->name, &req->job)`.

### 4.3 Asynchrone I/O mit APCs

`File::Read` 0x13F8F0 arbeitet mit einem Ringpuffer (`FUN_000a48e0(size, 0x40)`). Die Lesegröße wird auf
2-KiB-Sektoren abgerundet, höchstens 128 KiB (`& 0xFFFFF800`, 0x20000 in `FUN_0013f7b0`). Die Anforderung geht
über `FUN_0013f660` an `ReadFileEx(h, buf, n, &ov(+0x18), Completion 0x13F1D0)`. `ReadFileEx` 0xF7603 ruft dann
`NtReadFile(h, NULL, ApcRoutine = [Slot 39] (Ord. 232, NtUserIoApcDispatcher), ApcContext = Completion, IoStatus = ov, …)` auf.
Der Destruktor 0x13F4C0 wartet mit `while (pending) SleepEx(0, TRUE)`.
**Loader:** Slot 39 muss die Adresse einer HLE-Routine enthalten, die `Completion(err, bytes, ov)` aufruft. Das
HLE-`NtReadFile` muss die APC beim nächsten alertable `KeDelayExecutionThread`/`NtWaitForSingleObjectEx` dieses
Threads zustellen.
`SleepEx` 0xF64BE ruft `KeDelayExecutionThread(WaitMode = 1, Alertable, &Interval)` (Ord. 99, 0xF64E3–0xF64E9) auf.
Liefert der Kernel 0x101, wiederholt `SleepEx` den Aufruf, und 0xC0 übersetzt es in den Rückgabewert 0xC0
(`WAIT_IO_COMPLETION`). Ein HLE-`KeDelayExecutionThread` sollte deshalb nach dem Zustellen von APCs 0xC0 liefern
(Statusnamen: Vermutung nach NT-Konvention).

### 4.4 DPCs, Timer und Interrupts (nur XDK-Bibliotheken)

| Modul | Interrupt | DPC / Timer | Beleg |
|---|---|---|---|
| D3D | `D3D8::CMiniport_InitHardware` 0x23C88D: `HalGetInterruptVector`, `KeInitializeInterrupt`, `KeConnectInterrupt`, `KeInitializeDpc` | `KeInsertQueueDpc` 0x23B790, `KeSetEvent` 0x23B980/0x23BD00, `KeWaitForSingleObject` in `BlockUntilVerticalBlank`/`D3D_BlockOnTime` | callgraph |
| DSOUND | 0x24B09E (APU), 0x250AE4 (AC97) | `KeInitializeDpc`/`KeInitializeTimerEx`, `KeSetTimer`/`KeCancelTimer`, `KeInsertQueueDpc`, `KeSynchronizeExecution`, `NtSetEvent`, eigene CS (`DirectSoundEnterCriticalSection`, prüft IRQL über `fs:[0x24]`) | callgraph |
| XPP (USB) | 0x28C932 (OHCI; `HalGetInterruptVector` in 0x28C8F0) | Aufrufstellen im XPP-Abschnitt: 5× `KeInitializeDpc`, 4× `KeInitializeTimerEx`, 13× `KeSetTimer`, 20× `KeCancelTimer`, 21× `KeRaiseIrqlToDpcLevel` (+1 indirekt), 26× `KfLowerIrql`, 4× `KeSetEvent`, 2× `KeWaitForSingleObject` (+1 indirekt), 1× `KeInsertQueueDpc`; außerdem `XAutoPowerDownResetTimer` (0x28D62B/0x28D651) | `verify_startup_r2_xdksync.py` |
| XAPI | – | Auto-Power-Down-Timer (6 h) + DPC 0xF9B08 | 0xF9B1A, 0xF9AED |

Die XDK-Bibliotheken werden ersetzt, ihre DPCs und Interrupts entfallen damit. Den Auto-Power-Down-Timer lässt der
Loader nie feuern (oder Ord. 360 als No-op).

### 4.5 Synchronisationsprimitive im Spielcode

| Primitive | Verwendung |
|---|---|
| Critical Sections (`RtlEnter/LeaveCriticalSection`) | nur XAPI-Heap (0xF52CC, 0xF5A83, 0xF5C77), Thread-Notify-Liste, CRT-Datei-Locks, DSOUND. **Kein** Spielcode. |
| Events | `NtCreateEvent` ist nicht importiert. KEVENTs liegen nur inline in XDK-Code. Synchrones `ReadFile`/`WriteFile` wartet direkt mit `NtWaitForSingleObject` (Ord. 233, 0xF723B/0xF731C). XAPI-`WaitForSingleObject` 0xF6510 rufen nur `GetOverlappedResult` (einziger Aufrufer XMV 0x26471C) und der D3D-Capture-Pfad (0x24194E, 0x241A7A, 0x2420DE) auf, kein Spielcode. |
| APCs | Datei-I/O (Kapitel 4.3) |
| Fibers | Streaming (Kapitel 4.2) |

---

## 5. Speicher

### 5.1 Allokatoren im Überblick

| Allokator | Einstieg | Backend | Nutzer |
|---|---|---|---|
| **Spielpool** (`memory.cpp`) | `FUN_000a4900(size)` (cdecl, 976 aufrufende Ghidra-Funktionen bzw. 2011 Raw-Aufrufstellen; Vermutung: globaler `operator new`), `FUN_000a48e0(size, align)` (aligned), `FUN_000a3880(p)` (cdecl, 949 Ghidra-Aufrufer bzw. 2696 Raw-Aufrufstellen; Vermutung: `operator delete`) | Objekt 0x2E7D60, Kern `FUN_000a4520(size, align, file, line)` (thiscall, `ret 0x10`) | Spielcode, **NetImmerse** (101 `CreateObject`-Funktionen, `NiStream::RegisterLoader`), Dateipuffer |
| XAPI-Prozess-Heap | `RtlAllocateHeap`-Implementierung `FUN_000f52cc(heap, flags, size)`, Heap in `[0x31039C]` | `NtAllocateVirtualMemory` | CRT `malloc` (0x16A083 → `__nh_malloc` → `FUN_0016a030`: `RtlAllocateHeap(heap, 0, (n+0xF)&~0xF)`, 12 Raw-Aufrufstellen, alle CRT-intern), `LocalAlloc` 0xF63E0 (14 Aufrufstellen: 10× D3D, `XFindFirstSaveGame`, `XCalculateSignatureBeginEx`, Formatierung 0xF946D, `XMemAlloc`; kein Spielcode), der 38-MiB-Pool selbst |
| `XMemAlloc` 0x1707E2 | Bit 31 der Attribute: `XPhysicalAlloc`, sonst `LocalAlloc` | – | nur XMV (0x264360) |
| `XPhysicalAlloc` 0xF8420 | `MmAllocateContiguousMemoryEx(n, 0 bzw. addr, -1 bzw. addr+n-1, align, protect)` | Kernel | Movie-Player 0xEE395/0xEE3A4 (2× `(n, -1, 0, PAGE_READWRITE)`), `XMemAlloc`, D3D 0x241EFD |
| Direkt `MmAllocateContiguousMemoryEx` | – | Kernel | D3D (`CreateTexture2`, `CreateVertexBuffer2`, `CreatePalette2`, `InitializeFrameBuffers`, `CreateStandAloneSurface`, `D3D_AllocContiguousMemory`, 0x238B40, 0x239D20), DSOUND 0x2494B0 |
| `MmAllocateContiguousMemory` | – | Kernel | `FUN_000f81bb` (LaunchDataPage, 0x1000), XPP 0x28CBC9 |
| `ExAllocatePoolWithTag` | – | Kernel | DSOUND `DSound_CMemoryManager_PoolAlloc`, XPP 0x28EED6/0x28F2A6 |
| `MmCreateKernelStack` | – | Kernel | Fiber-Stacks (Kapitel 4.2) |

Parameter von `MmAllocateContiguousMemoryEx` an den Aufrufstellen (`startup_kcallsites.py 56`; der Aufrufer räumt
den Stack nicht auf, also stdcall mit 5 Argumenten):

| Aufrufstelle | Größe | Low | High | Alignment | Protect |
|---|---|---|---|---|---|
| `D3DDevice_CreateTexture2` 0x237144 | var. | 0 | 0x7FFFFFF | 0x80 | 0x404 |
| 0x238B57 | `[0x246CAC]` | 0 | 0x7FFFFFF | 0 | 0x404 |
| `CDevice_InitializeFrameBuffers` 0x239A40/0x239B2F/0x239C0C | var. | 0 | 0x7FFFFFF | 0x4000 | 0x404 |
| 0x239D37 | 0x60 | 0 | 0x7FFFFFF | 0 | 4 |
| `CreateVertexBuffer2` 0x23A483, `CreatePalette2` 0x23A56F | var. | 0 | 0x7FFFFFF | 0 | 0x404 |
| `D3D_CreateStandAloneSurface` 0x23CF2A | var. | 0 | 0x7FFFFFF | 0x40 | 0x404 |
| `D3D_AllocContiguousMemory` 0x2417A6 | arg | 0 | 0x7FFFFFF | arg | 0x404 |
| DSOUND 0x2494C1 | arg | 0 | −1 | arg | arg |
| `XPhysicalAlloc` 0xF8449 | arg | 0/addr | −1/addr+n−1 | arg | arg |

Der Protect-Wert 0x404 ist PAGE_READWRITE (4) | 0x400. Dass 0x400 Write-Combine bedeutet, ist Vermutung
(Xbox-Konvention).

### 5.2 Der Spielpool (`C:\Code\Futurama\Futurama\Memory\memory.cpp`)

* **Lazy-Init** beim ersten Aufruf von `FUN_000a4520`, wenn `[0x2E8D35] == 0`:
  `FUN_000a4920(0x2600000)` → `RtlAllocateHeap(XapiProcessHeap, 0xC, 0x2600020)`. Die Flags 0xC sind laut
  Vermutung HEAP_GENERATE_EXCEPTIONS|HEAP_ZERO_MEMORY; die XAPI-Heap-Funktionen rufen `RtlRaiseException`
  tatsächlich auf. Danach `FUN_000a3660`: 134 (0x86) Größenklassen mit je 0xFF-Eintrags-Tabellen (0x3FC Byte),
  zweimal angelegt (memory.cpp Zeilen 0xEDE und 0xDC2), Endmarke 0xF00DBEEF.
* Der erste Aufruf kommt **aus `_cinit`**: C++-Init 0x2BDEF4 → 0x17E010 → `NiStream::RegisterLoader` →
  `FUN_000a4900`.
* Layout von 0x2E7D60: +0x00 belegte Bytes, +0x04 freie Bytes, +0x0C Poolanfang, +0x10 Poolende,
  **+0x14 unterer Cursor (wächst nach oben, `FUN_000a37b0`)**, **+0x18 oberer Cursor (wächst nach unten,
  `FUN_000a3190`)**, +0x1C/+0x20/+0x28/+0x2C Freilisten-Tabellen. Spitzenwert in `[0x2E8D38]`.
* Blockheader: das Dword vor dem Nutzerzeiger. Bits 1–30 = Größe, Bit 31 = frei, Bit 0 = Region (oben/unten).
  Mindestgröße 8, Granularität 16.
* Strategie (`FUN_000a4520`): Größen < 8 werden auf 8 angehoben. Blöcke < 0x123A Byte kommen zuerst aus den
  Freilisten (`FUN_000a4020`), dann vom unteren Cursor (`FUN_000a37b0`). Schlägt beides fehl und ist der Zähler
  `[0x2E8D44]` > 2000, läuft `FUN_000a3b10` (Koaleszieren) und beides wird wiederholt. Als letzte Möglichkeit folgen
  die Freilisten der großen Blöcke (`FUN_000a42a0`). Größere Blöcke kommen aus `FUN_000a42a0`, dann vom oberen Cursor
  (`FUN_000a3190`), bei `[0x2E8D48]` > 20 nach `FUN_000a3ce0` ein zweiter Versuch und zuletzt die Freilisten der kleinen
  Blöcke (`FUN_000a4020`). **Speicher voll:** `FUN_000a39d0` und danach eine **Endlosschleife**.
* **Schreibzugriff vor dem Poolanfang:** Die erste Allokation (0xC90 Byte aus `FUN_000a3660`; `FUN_000a4020` liefert
  0, solange `[+0x1C]` = 0 ist) läuft über `FUN_000a37b0` mit Cursor = Poolanfang. Dabei wird der Blockheader bei
  **Poolanfang − 4** gelesen und geschrieben (0xA3814 `mov eax,[esi-4]` … `mov [esi-4],eax`, bei unausgerichtetem
  Anfang zusätzlich 0xA37F4 `mov [edi-4],eax`). Diese 4 Byte liegen außerhalb des 0x2600020-Byte-Heapblocks. Auf der
  Xbox überschreiben sie den Eintragskopf des nie freigegebenen XAPI-Heapblocks. **Loader:** Den Pool nicht direkt
  aus einem Windows-Heap holen (dessen Blockkopf würde beschädigt; Folgen Vermutung), sondern z. B. per `VirtualAlloc`
  mit mindestens 16 Byte Vorlauf.
* Pflege jeden Render-Durchlauf (`FUN_000a46d0`): Zähler `[0x2E8E48]` > 0x28 → `FUN_000a3ce0` (also alle 41 Aufrufe),
  Zähler `[0x2E8E44]` > 200 → `FUN_000a3b10` (alle 201 Aufrufe).
* Bit 0 des Headers: `FUN_000a3eb0` (free) löscht es für Blöcke im unteren Bereich ([+0xC], [+0x14]) und setzt es
  für Blöcke im oberen Bereich ([+0x18], [+0x10]); Bit 0 = 1 heißt also „obere Region“.
* `new` mit Alignment: `FUN_000a4900` übergibt `[0x2D6750]` = 0x10.

### 5.3 Speicherbudget (Xbox 64 MiB)

| Posten | Größe | Beleg |
|---|---|---|
| XBE-Abbild 0x10000–0x31A300 | 0x30A300 = 3,04 MiB (davon BSS in `.data`: 0x37300) | Sektionstabelle |
| Spielpool | 0x2600000 = **38 MiB** (fest, genullt) | 0xA4530 |
| D3D-Pushbuffer | 0x200000 (2 MiB), Kickoff 0x8000 | `D3D_SetPushBufferSize`, `[0x2D7CEC]`/`[0x2D7CF0]` |
| Hauptthread-Stack | 64 KiB, Fiber-Stacks je 16 KiB | 0xF9A7C, 0x13F473 |
| XAPI-Heap | 1 MiB reserviert, wachsend | Header 0x134/0x138 |
| Rest (Kernel, Framebuffer, Texturen und VBs über D3D, DSOUND, XMV) | etwa 19 MiB | Rechnung, **Vermutung** |

XBE-InitFlags = 5. Bit 0 (MountUtilityDrive) ist im Code belegt (2.3). Bit 2 bedeutet laut XBE-Konvention
„Limit 64 MB“ (Vermutung, wird nur vom Kernel ausgewertet).

### 5.4 Feste Adressen

Der Spielcode enthält **keine festen Hardware- oder Pool-Adressen** (`startup_hwaddr.py`). Absolute Adressen
außerhalb des Abbilds stehen nur in D3D (NV2A 0xFD000000…, 0x80000000), DSOUND (APU 0xFE800000…,
AC97 0xFEC00000…) und XPP (0xFE80200C). Dazu kommen die Nullzeiger-Schreibzugriffe `[0x54]`/`[0x14]` im
OOM-Pfad von `_mtinit` 0x1694A1. Alle Pools werden dynamisch angelegt.

---

## 6. TLS

| Feld (IMAGE_TLS_DIRECTORY @0x2A5D2C) | Wert |
|---|---|
| StartAddressOfRawData / EndAddressOfRawData | 0 / 0 (keine Vorlage) |
| AddressOfIndex | 0x2F3D18 (`.data`-BSS, `XAPILIB::_tls_index`, von `entry` auf −9 gesetzt) |
| AddressOfCallBacks | 0 |
| SizeOfZeroFill | 0x20 |
| Characteristics | 0 |

So funktioniert der Zugriff: `mov eax,[0x2F3D18]; mov ecx,fs:[4]; mov eax,[ecx+eax*4]` ergibt
`[StackBase − 0x24]`, also den Zeiger auf den TLS-Block. Er stimmt, weil der Kernel `KTHREAD->TlsData` ans
Stack-Top legt (TlsDataSize 0x24) und `XapiThreadStartup` `TlsData[0] = TlsData + 4` schreibt.

| Offset im Block | Nutzung | Beleg |
|---|---|---|
| +0x00 | keine Zugriffe gefunden | `startup_fsscan.py` |
| +0x04 | `GetLastError`/`SetLastError` | 0xF6E54, 0xF6E7C |
| +0x08 | CRT per-Thread-Daten (`_ptd`) | `_mtinit` 0x169499, `_getptd` 0x1692D0 |
| +0x0C | aktueller Fiber | 0x17096E, 0x13F44D, 0x13F5DF, 0x17088A |
| +0x10…+0x1F | Fiber-Struktur aus `ConvertThreadToFiber` | 0x1709A1 |

`__declspec(thread)`-Variablen im Spielcode gibt es nicht (keine Rohdaten). Alle Zugriffe über `fs:[4]` sind
bekannt: 11 Stellen, nämlich 8 Ghidra-Funktionen, die zweite `_getptd`-Variante 0x169352 (Zugriff bei 0x169383, keine
Ghidra-Funktion) und die Stubs 0xF8495 und 0x170880 (`verify_startup_fs.py`).

---

## 7. Zugriffe über `fs:` (Xbox-KPCR)

| fs-Offset | Bedeutung (Xbox) | Funktionen | Windows-TEB an dieser Stelle |
|---|---|---|---|
| `fs:[0]` | SEH-Kette | ≥4850 Zugriffe in ≥1419 Funktionen (EH-Prologe) | ExceptionList (**kompatibel**) |
| `fs:[4]` | NT_TIB.StackBase | `GetLastError`, `SetLastError`, `ConvertThreadToFiber`, `SwitchToFiber`, 0x13F250, 0x13F590, 0x1692D0, 0x169467, sowie 0x169383 (in 0x169352), 0xF8495 und 0x170880 außerhalb von Ghidra-Funktionen (11 Stellen) | StackBase (gleiche Bedeutung, aber TLS bei −0x24 ist nötig) |
| `fs:[0x20]` (+0x250) | KPRCB-Zeiger, Debug-Monitor-Hook | Heap 0xF52CC/0xF5A83/0xF5C77, 0xF829C, `UnhandledExceptionFilter` 0xF999E, D3D 0x239D20, 0xF6384, Hauptthread 0xF8473 | ClientId.UniqueProcess (**Konflikt**) |
| `fs:[0x24]` (Byte) | IRQL | `GetLastError`, `SetLastError`, `CreateThread`, `_getptd` 0x1692D0 und 0x16935E (**IRQL ≥ 2 → `KeBugCheck(0xA)`**), DSOUND 0x246D30/0x246E0D/0x24BA46 | ClientId.UniqueThread (**Konflikt**) |
| `fs:[0x28]` | KTHREAD* (+0x1C, +0x28 TlsData, +0x12C ThreadId) | `GetCurrentThreadId` 0xF9AE0, `CreateThread`, `_getptd` 0x1692E6 und 0x169372 (Variante 0x169352), `ConvertThreadToFiber`, `GetLastError`/`SetLastError` (gelesen, ohne Dereferenz), 0xF99DA, 0xF848F | ActiveRpcHandle (meist NULL → **Absturz** bei Dereferenz) |
| `fs:[0x58]` | ? (Vermutung: KPRCB-Feld wie DpcRoutineActive; 0x246D7A prüft es vor `KeSaveFloatingPointState`) | DSOUND 0x246D7A/0x246D9F | – |

**Empfehlung:** Die TEB nicht umschreiben, weil ClientId von Win32 und ntdll benutzt wird (z. B. als
CS-Owner). Stattdessen diese Funktionen hooken bzw. ersetzen: 0xF6E54, 0xF6E7C, 0xF9AE0, 0xF9A72 (`CreateThread`,
wird nicht gebraucht), 0x1692D0 und 0x169352 (`_getptd`-Varianten), 0x169467 (`_mtinit`), 0xF999E, die
Heap-Funktionen (oder `[KPRCB+0x250]` über eine eigene Struktur bereitstellen) und die Fiber-Funktionen
0x1708B0/0x17093C/0x17094F/0x17098E. Den Hauptthread-Stub 0xF8469 **nicht nativ ausführen**, sondern nachbauen:
TLS einrichten, `XapiInitProcess` als HLE, `_rtinit`, `_cinit`, `main`. D3D, DSOUND und XPP werden ohnehin ersetzt.

TLS unter Windows: Den Spiel-Hauptthread auf einem vom Loader angelegten Stack laufen lassen.
`NT_TIB.StackBase`/`StackLimit` auf diesen Stack setzen und bei `StackBase − 0x24` den Zeiger auf einen genullten
0x20-Byte-Block ablegen. Als Alternative lassen sich die 11 `fs:[4]`-Stellen patchen.

Fibers unter Windows:
1. Die Original-XAPI-Fibers ließen sich behalten (nur `MmCreateKernelStack`/`MmDeleteKernelStack` als HLE mit
   committetem Speicher). Dann liegt ESP während eines Fibers außerhalb von TEB StackBase/StackLimit, und
   `RtlDispatchException` würde SEH-Frames auf dem Fiber-Stack verwerfen. Das spielt nur bei Ausnahmen eine Rolle
   (Vermutung, nicht getestet).
2. Sauberer: eigene Fiber-Implementierung, die zusätzlich StackBase/StackLimit umschaltet und an jedes
   Fiber-Stack-Top bei −0x24 denselben TLS-Zeiger schreibt.

---

## 8. Exceptions

* **SEH/C++-EH:** EH-Prologe (`push -1; push handler; mov eax,fs:[0]; …`) mit Funclets, die per
  `jmp ___CxxFrameHandler` (0x166977 → `___InternalCxxFrameHandler` 0x169CBC) weiterleiten. XAPI benutzt
  `__SEH_prolog` 0x167DC8 (`__except`-Blöcke in `XapiThreadStartup`, Fiber-Trampolin und Heap).
* **Kein `throw`:** `XAPILIB::RaiseException` 0xF98ED hat einen einzigen Aufrufer, den CRT-FP-Fehlerpfad 0x16AFF0
  (über 0x16EF85). 0xE06D7363 kommt nur in CRT-EH-Code vor (0x166D84, 0x169550…, 0x16ED27).
  `RtlRaiseException` (Ord. 302) rufen nur der XAPI-Heap (0xF52CC, 0xF5C77) und `RaiseException` auf.
* **Unhandled-Exception-Pfad:** `XapiThreadStartup`/Fiber-Trampolin → `UnhandledExceptionFilter` 0xF999E. Er fragt
  zuerst den Debug-Monitor (`KPRCB+0x250`, Methode +0x14), dann den Filter in `[0x310390]` (gesetzt von
  `SetUnhandledExceptionFilter`; die CRT installiert 0x16ED1E). 0x16ED1E erkennt die C++-Ausnahme
  (0xE06D7363/0x19930520) und ruft `terminate` 0x169D5E auf: Handler `ptd+0x68`, danach
  `KeBugCheck(0xC0000144)` (Ord. 95).
* **Weitere `KeBugCheck`-Aufrufe:** `_getptd` bei IRQL ≥ 2 (0xA; 0x1692DE und 0x16936C), 0x169FBE (0; Ende von
  `doexit` 0x169F1B, erreichbar über `_amsg_exit` 0x1692B4 → `[0x2D86AC]` = `_exit` 0x169FC5, z. B. aus `_purecall`
  0x1668B6), Fiber-Rückkehr 0x1708A9 (0).
  Der Loader sollte `KeBugCheck` als „Fatal Error“ mit Backtrace melden.
* SafeSEH/SEHOP unter Windows: Die Handler liegen im gemappten XBE-Speicher, nicht in einem geladenen Image. Das
  wird erst wichtig, wenn eine Ausnahme tatsächlich verteilt wird (Debugging/Crash). Vermutung, nicht getestet.

---

## 9. Konsequenzen für den Loader (Checkliste)

1. Abbild 0x10000–0x31A300 reservieren, BSS nullen, D3D/DSOUND/XMV/XPP/.data beschreibbar mappen (XDK-Initialisierer
   schreiben in diese Abschnitte, 2.4). Den Hauptthread-Ablauf selbst nachbauen:
   `_tls_index = −9` (wie `entry`), TLS-Block, `XapiInitProcess`-HLE (oder das Original mit HLE-Kernel, siehe
   Anforderungen in 2.3), danach `_rtinit` 0xF97DB, `_cinit` 0xF9783, `main` 0xD29B0.
2. Kernel-HLE für den Start: `XboxKrnlVersion.Build` außerhalb von {3944, 4034, 4039, 4817, 4818}.
   `ExQueryNonVolatileSetting` (alle Aufrufer gehen über den Stub 0xF9CC6, `verify_startup_nvsetting.py`):
   7 = Sprache (`XGetLanguage`), 0x104 = Region (nur, wenn die Sprache > 9 ist), 0xA = 0 (Jugendschutz),
   0x11 = Auto-Power-Down aus (über `XQueryValue`), 9 = Audio-Flags (`XGetAudioFlags`, nur aus DSOUND 0x24857B) und
   0xFFFF = ganzes EEPROM (über `XQueryValue` in `XCalculateSignatureEnd`, nur wenn `XboxHDKey` (Ord. 323) aus 16
   Nullbytes besteht; betrifft Spielstand-Signaturen). Datenimporte mit gültigem Wert belegen, u. a. Ord. 74
   (`IoInvalidDeviceRequest`), weil ein XPP-Initialisierer ihn schon in `_cinit` kopiert (2.4).
   `NtQueryVolumeInformationFile(partition1)` muss eine Clustergröße von 0x4000 melden. Die Pfade
   `\Device\CdRom0`, `\Device\Harddisk0\partition1\TDATA|UDATA` und die Cache-Partition müssen bereitstehen.
   `HalRegisterShutdownNotification`, `KeSetTimer` (Power-Down) und die DPCs können No-ops sein.
3. 0xF641B/0xF642C (QPC/QPF) ersetzen.
4. Die Funktionen mit `fs:`-Abhängigkeit hooken (Kapitel 7).
5. `NtReadFile` mit APC (Ord. 232 als Routinenadresse) sowie alertable `KeDelayExecutionThread` und
   `NtWaitForSingleObjectEx` korrekt umsetzen. Das asynchrone Laden hängt davon ab.
6. `MmCreateKernelStack`/`MmDeleteKernelStack` für die 16-KiB-Fiber-Stacks bereitstellen. Die Fiber-Struktur
   übernimmt StackBase/StackLimit aus dem Rückgabewert (0x1708F5–0x1708FD). Ein HLE darf also größere Stacks liefern.
   Das ist ratsam, weil HLE-Code (z. B. `NtReadFile` → Win32) aus dem Fiber heraus auf diesem Stack läuft
   (Empfehlung; welche Jobs im Fiber laufen, ist nicht vollständig geprüft).
7. Mindestens 38 MiB + 1 MiB für den XAPI-Heap. `RtlAllocateHeap` mit HEAP_ZERO_MEMORY für den Pool. Vor dem Poolanfang
   müssen mindestens 4 Byte frei beschreibbar sein (5.2).
8. Bildrate: Der 50-Hz-Fixed-Step bleibt. VSync bzw. `Present` ersetzt `D3DDevice_Swap`, das Rendern kann
   ungebremst laufen (ohne Interpolation). `Swap` wird auch außerhalb des Render-Pfads aufgerufen (8 Frames je
   Kontextwechsel, Menü, Movie; 3.2).
9. `XLaunchNewImageA`/`HalReturnToFirmware` als „Programm beenden“ abbilden, und zwar ohne Rückkehr
   (`XapiBootToDash` endet mit `ret 0xC`, 3.5).
10. Den XBE-Header (0x10000–0x10968) unverändert an 0x10000 einblenden. XAPI liest ihn direkt: `[0x10108]`
    SizeOfHeaders (`entry`), `[0x10118]` Zertifikat (XapiInitProcess, XapiBootToDash, XLaunchNewImageA, Signaturen,
    Cache-Partition), `[0x10124]` InitFlags, `[0x10130]` Stackgröße (CreateThread, CreateFiber), `[0x10134]/[0x10138]`
    Heap, `[0x1011C]/[0x10120]` und die Sektionsheader 0x1036C…0x10564 (`FUN_000f9b8e` = Sektionssuche nach Namen
    für `XapiMapLetterToDirectory`). `entry` würde cert->Size kürzen, wenn das Zertifikat über die Header hinausreicht
    (hier nicht: 0x1EC < 0x968 − 0x180 = 0x7E8). Mit diesem XBE gibt es also keinen Schreibzugriff auf den Header
    (`verify_startup_r2_hdrrefs.py`, `verify_startup_r2_hdr.py`).

---

## 10. Offene Punkte

* Identität der Basisklasse `FUN_001091a0` (vtable 0x2A6A10; Vermutung: NiStream).
* Ob und wie der Retail-Code die Hauptschleife verlässt (Kontext ohne Nachfolger).
* Genaues Verhalten bei PAL60 (refresh rate 0) und Wirkung eines erzwungenen 60-Hz-Modus.
* Identität der Ordinale 258, 357 und 360 (keine Ghidra-Namen; Vermutungen oben). Der Binärcode passt dazu:
  258 wird mit genau einem Argument aufgerufen und kehrt nicht zurück (0xF9A6B, danach `int3`), 360 wird ohne
  Argument aus der DPC 0xF9B08 aufgerufen, 357 ist ein Datenobjekt mit Feldern +0x10/+0x14 (Callbacks) und +0x20.
* Welche `File::File`-Aufrufer außer `FUN_000d21f0` einen Job (also einen Fiber) übergeben, ist nicht vollständig
  geprüft (35 Aufrufstellen, Argumente oft in Registern). Ebenso offen ist, was die Job-Objekte (`req+8`, `vf0(file)`
  in 0x13F1A0) auf dem 16-KiB-Fiber-Stack tun, ob dort etwa NetImmerse- oder D3D-Objekte entstehen. Das bestimmt, wie
  groß die HLE-Fiber-Stacks sein müssen.
* Ghidra hat viele `.text`-Bereiche nicht als Funktionen erkannt (z. B. 0x1D882, 0x218F0, 0xF8469, 0xF99DA,
  0x13F1D0, 0x9C3D0). Die Raw-Scans (`startup_callers_raw.py`, `startup_fsscan.py --raw`) decken sie ab, die
  Aufrufgraph-Statistik nicht.

---

## Anhang: Skripte (`analysis\loader\scripts`)

| Skript | Zweck |
|---|---|
| `startup_getfunc.py <addr…>` | Decompilat einer Funktion aus dem Export |
| `startup_dis.py <va> [len] [stop]` | capstone-Disassembly mit Funktions- und Thunk-Namen |
| `startup_xref.py [-r] <addr…>` | Callees bzw. Caller und referenzierte Strings |
| `startup_kimports.py` → `startup_kimports.out` | Thunk-Slot → Ordinal → Ghidra-Name, Anzahl der Aufrufer |
| `startup_kdata.py` | Datenimport-Slots ↔ `*_exref`-Namen |
| `startup_kcallsites.py <slot…>` | Argument-Pushes an jedem `call [thunk]` |
| `startup_callers_raw.py <va…>` | E8/E9-Raw-Scan nach direkten Aufrufern, auch außerhalb von Funktionen |
| `startup_dataref.py <va…>` | absolute Adressreferenzen im Code |
| `startup_tls.py` | XBE-Headerfelder, TLS-Directory, abgeleitete TlsDataSize und `_tls_index` |
| `startup_fsscan.py [--raw]` → `startup_fsscan.out` | alle `fs:`-Zugriffe nach Offset und Funktion |
| `startup_hwaddr.py` | absolute Speicheroperanden außerhalb des Abbilds (Hardware-Adressen) |
| `verify_startup_common.py` | Hilfsfunktionen des Reviews (Disassembly, Raw-Aufrufer, Thunk-Slots, Decompilat); unabhängig von `startup_*` |
| `verify_startup_header.py` | XBE-Header, Zertifikat, TLS-Directory, Sektionen, `.data`-Startwerte (dt, fps, Pushbuffer) |
| `verify_startup_threads.py` | importierte Ordinale, `PsCreateSystemThreadEx`-/`CreateThread`-Aufrufer |
| `verify_startup_timer.py` | QPC/QPF-Bytes und Aufrufer, `rdtsc`-Stellen mit Code-/Daten-Prüfung, Spieltimer |
| `verify_startup_cinit.py` | `_rtinit`/`_cinit`-Tabellen, Zählung der `RegisterLoader`-Initialisierer, XDK-Initialisierer |
| `verify_startup_fs.py` | unabhängiger `fs:`-Scan inkl. Code außerhalb von Ghidra-Funktionen |
| `verify_startup_contig.py` | Argumente an allen `MmAllocateContiguousMemory(Ex)`-/`XPhysicalAlloc`-Aufrufen |
| `verify_startup_nvsetting.py` | alle `ExQueryNonVolatileSetting`-Aufrufer mit ValueIndex |
| `verify_startup_counts.py` | Aufruferzahlen (Pool, `malloc`, `LocalAlloc`), dt-Referenzen, Critical-Section-Nutzer |
| `verify_startup_sections.py` | XBE-Sektionsflags |
| `verify_startup_kref.py <ord…>` | alle Referenzen auf einen Kernel-Thunk-Slot |
| `verify_startup_misc.py [abschnitt…]` | Einzelbelege des Reviews (Stub, XapiInitProcess-Pfade, Schleife, Sleep/Wait, XDK-Initialisierer, Dashboard, Exit-Pfad) |
| `verify_startup_r2_common.py` | Hilfsmodul des zweiten Reviews (eigene Thunk-/Disassembly-/Raw-Scan-Funktionen, unabhängig von `startup_*` und `verify_startup_common`) |
| `verify_startup_r2_k1.py` | Thunk-Slots ↔ Ordinale ↔ Ghidra-Namen, Hauptthread-Stub, `PsCreateSystemThreadEx`-/`CreateThread`-Aufrufer, `entry` |
| `verify_startup_r2_hdr.py` | XBE-Header, Zertifikat, TLS-Directory, Sektionsflags, `.data`-Startwerte |
| `verify_startup_r2_timer.py` | QPC/QPF-Bytes und Aufrufer, alle `0F 31`-Vorkommen mit Code-Prüfung, Timer-Hilfen 0xD2B10–0xD2B90 |
| `verify_startup_r2_cinit.py` | `_rtinit`/`_cinit`, C++-Init-Zählung, erster Initialisierer, XDK-Initialisierer |
| `verify_startup_r2_fs.py` | `fs:`-Scan über alle Ghidra-Funktionen plus Rohsuche außerhalb (Treffer mit Register-Operanden außerhalb von Funktionen sind Fehldekodierungen) |
| `verify_startup_r2_mem.py` | `MmAllocateContiguousMemory(Ex)`, `XPhysicalAlloc`, Exit-Pfad, Aufruferzahlen Pool/`malloc`/`LocalAlloc`/`_atexit` |
| `verify_startup_r2_misc.py [abschnitt…]` | `XapiThreadStartup`, NV-Setting-Indizes, `doexit`, Synchronisationsimporte, `LocalAlloc`, vollständige Thunk-Tabelle |
| `verify_startup_r2_xdksync.py` | DPC-/Timer-/IRQL-/Wait-Aufrufstellen je Abschnitt (Tabelle 4.4) |
| `verify_startup_r2_hdrrefs.py` | direkte Lesezugriffe auf den XBE-Header, VBlank-APIs, `DisplayFrame`-Referenzen |
| `verify_startup_r2_extra.py [abschnitt…]` | Belege der Ergänzungen: Zeit-Globals, Splash-Auswahl, Pool-Header vor dem Poolanfang, `XapiBootToDash`-Rückkehr, Present-Parameter, Kontextnamen |

---

## Verifikation

Unabhängiges Review vom 2026-10-04 (zweiter Durchgang). Alle Belege stammen aus eigenen Skripten
(`scripts\verify_startup_r2_*.py`, Ausgaben jeweils als `.out` daneben), die direkt gegen `orig\retail_eu\default.xbe`
und den Ghidra-Export laufen. Die Skripte `startup_*.py` und `verify_startup_*.py` (ohne `_r2_`) der früheren Durchgänge
wurden dafür nicht benutzt. Ordinal-Namen stammen nur aus Ghidra (`callgraph.tsv`, Form `EXTERNAL:<Slot+1>`
`xboxkrnl.exe::<Name>`, z. B. `EXTERNAL:0000004e` = Slot 77 = Ord. 255 `PsCreateSystemThreadEx`). Für Slots ohne
Ghidra-Namen (Ord. 24, 74, 164, 232, 258, 322, 324, 357, 360 …) bleibt der Name Vermutung, sofern er nicht aus
einem `*_exref` des Decompilats stammt. Korrekturen sind oben direkt im Text eingearbeitet. Eigene TSV-Dateien hat
dieser Bericht nicht; `fs_sites.tsv` gehört zum CPU-Bericht und enthält die hier genannten `fs:`-Stellen bereits.

### Kernaussagen (17 von 17 geprüft)

| # | Aussage | Ergebnis | Beleg (eigene Prüfung) |
|---|---|---|---|
| 1 | Reihenfolge im Stub 0xF8469 | bestätigt | `verify_startup_r2_k1.py`: `call 0xF9804; call 0xF92C0; fs:[0x20]+0x250-Prüfung; call 0xF97DB; call 0xF9783; push 0 ×3; call 0xD29B0; add esp,0xC; push 0,1,1; call 0xF8FCA; xor eax,eax; ret 4` |
| 2 | genau ein Thread, keine Prioritätsimporte | bestätigt | Slot 77 (Ord. 255) nur bei 0xF9AAB (`call [0x293694]`, 10 Pushes); `CreateThread` 0xF9A72 nur per E8 von 0xF8531, sonst nur ein Eintrag in `.XTLID` (Bibliotheks-ID-Tabelle, keine Code-Referenz); Ord. 143, 148, 254 nicht in der Thunk-Tabelle |
| 3 | CApp, vtable 0x2A3EF8, g_pApp 0x2EB310 | bestätigt | Decompilat 0xD29B0: `FUN_000a4900(0x108)`, vtable 0x2A3EF8, `DAT_002eb310`; vtable-Dump identisch mit Bericht |
| 4 | fester Zeitschritt, 100 Schritte, 6·dt | bestätigt, präzisiert | 0xA2CE8 `fld [0x2D6748]; fmul [0x29BFF4]` (6.0f), 0xA2D23 `cmp byte [0x2D674C],bl`, 0xA2D84 `mov edi,0x64`; dt = 0x3CA3D70A, fps = 50.0f. Ergänzt: praktisch ≤ 6 Schritte, variabler Zweig tot (3.2) |
| 5 | QPC = `rdtsc`, QPF = 733.333.333 | bestätigt | 0xF641F `0F 31`, 0xF6434 `C7 00 55 C7 B5 2B`; Aufrufer QPC: 0xD2BCD, 0x241A12/0x241A61/0x241B45, XMV 0x26478B/0x264DDE/0x264FFA; QPF: 0xD2BB1, 0x241B4E, 0x241D4E, 0x26440A; Ord. 126/127/156 nicht importiert |
| 6 | PAL/NTSC nur über Archivnamen | bestätigt, ergänzt | Strings 0x2A3F24–0x2A3F50 nur in 0xD26A0; `XGetAVPack` 0x170AA5 nur aus `XGetAudioFlags`; NV-Indizes ohne Videonorm. Ergänzt: Splash-/Legal-Bilder hängen ebenfalls an +0xE8 (3.4) |
| 7 | `DisplayFrame` → `Swap(0)`, Device-Parameter | bestätigt, ergänzt | 0x14646D `push 0; call 0x240FC0`; `FUN_0014c170`: Behavior 0x10\|0x40, PP-Felder `[0x9F]…[0xAB]`; ergänzt MultiSample 0x11, SwapEffect 1 |
| 8 | Pool per `RtlAllocateHeap(heap, 0xC, 0x2600020)` im ersten Initialisierer | bestätigt | 0xA4530 `push 0x2600000`; 0xA4926–0xA4934; `FUN_000f63da` = `mov eax,[0x31039C]; ret`; Tabelle 0x2BDEF0 = 0, 0x2BDEF4 = 0x17E010 → `call 0x1087D0` → `call 0xA4900` (0x1087F1) |
| 9 | Zwei-Enden-Pool, Endlosschleife | bestätigt, präzisiert | Decompilat `FUN_000a4520/37b0/3190/3eb0/4020`; Ablaufbeschreibung korrigiert; neu: Schreibzugriff auf Poolanfang − 4 (5.2) |
| 10 | Fiber pro Async-Datei, `SwitchToFiber` ohne `fs:[4]` | bestätigt | 0x13F468–0x13F473; 0x17094F–0x17098B (nur `fs:[0]` und ESP); `MmCreateKernelStack` nur aus `CreateFiber` 0x1708DA |
| 11 | `ReadFileEx` → `NtReadFile` mit APC aus Slot 39 | bestätigt | 0xF7625 `push [0x2935FC]`, 0xF7630 `call [0x2935E8]` (8 Pushes, `ret 0x14`); 0x13F68C `push 0x13F1D0`; Destruktor 0x13F510–0x13F51E `SleepEx(0,1)`-Schleife |
| 12 | TLS-Directory, TlsDataSize 0x24, Index −9 | bestätigt | `verify_startup_r2_hdr.py`: 0/0/0x2F3D18/0/0x20/0; Rechnung 0xF84F5–0xF852F |
| 13 | KPCR-Zugriffe | bestätigt | `verify_startup_r2_fs.py`: `fs:[4]` 11, `fs:[0x20]` 8 (alle `+0x250`), `fs:[0x24]` 8, `fs:[0x28]` 9, `fs:[0x58]` 2 Stellen, exakt wie Kapitel 7; `fs:[0]` 5140 Stellen in 1420 Funktionen |
| 14 | Voraussetzungen `XapiInitProcess` | bestätigt | `FUN_000f92af` = `FUN_000f8ae2(path, 0x4000)`; Produkt FILE_FS_SIZE_INFORMATION +0x10·+0x14; Fehlerpfade 0xF9362 (1,1,0), 0xF93AD (1,2,0), 0xF939F (2,0x54,0x10), 0xF93FC (1,4,rating), 0xF93D5 (1,2,0). `XGetParentalControlSetting` beginnt bei 0xF9C3E (0xF9C4F ist das `push 0xA`) und liefert 0, wenn die NV-Abfrage fehlschlägt |
| 15 | kein C++-`throw` | bestätigt | `RaiseException` 0xF98ED: einziger Aufruf 0x16B1E5 in `FUN_0016aff0` (FP-Statuscodes 0xC000008E…0xC0000093), dieser nur aus 0x16EF85; 0xE06D7363 nur in CRT-EH (u. a. `__ArrayUnwind`-Filter 0x166D84); `terminate` → `KeBugCheck(0xC0000144)` |
| 16 | `MmAllocateContiguousMemoryEx`-Parameter, `XPhysicalAlloc` im Movie-Player | bestätigt | 12 Aufrufstellen, Werte wie Tabelle 5.1, kein `add esp` nach dem Aufruf; 0xEE38E–0xEE3A4 `XPhysicalAlloc(edi,-1,0,4)` |
| 17 | Programmende | bestätigt, ergänzt | `HalReturnToFirmware`-Aufrufe nur 0xF82D2 (Wert 2) und 0xF9028 (Wert 4); `XLaunchNewImageA` nur aus 0xE4B3A und 0xF901C. Ergänzt: `XapiBootToDash` kehrt zurück (`ret 0xC`) |

### Weitere geprüfte Einzelaussagen (Auswahl, alle bestätigt)

1. Ghidra-Importnamen `EXTERNAL:<n>` mit n = Slot + 1; Slot 12 = Ord. 184, Slot 56 = Ord. 166, Slot 77 = Ord. 255.
2. XBE-Werte: Stack `[0x130]` = 0x10000, Heap 0x100000/0x1000, InitFlags 5, TitleID 0x46490002, AllowedMedia 2,
   GameRatings 0, SizeOfImage 0x30A300, BSS in `.data` 0x37300; Sektionsflags W für D3D/DSOUND/XMV/XPP/`.data`.
3. `XapiThreadStartup` 0xF99DA: TLS-Kopie (0 Byte) + 0x20 Byte Nullen, Notify(TRUE), StartRoutine, Notify(FALSE),
   `call [Slot 76]` (Ord. 258) mit Rückgabewert, `int3`.
4. Hotfix 0xF9804: Builds 0xF68/0xFC2/0xFC7/0x12D1–0x12D2, Dword 0x20C5048D → 0x28C5048D.
5. Auto-Power-Down: `KeSetTimer(0x310360, 0xFFFFFFCD:B5659000 = −216·10⁹ × 100 ns = −6 h, 0x310340)`; DPC 0xF9B08
   ruft Slot 78 (Ord. 360) nur bei `[0x310388]` ≠ 0; `XQueryValue(0x11)` nur bei `!(HardwareInfo & 8)`.
6. Shutdown-Record 0x2D7530 = {0xF9C7F, 0x80000000}; 0xF9C7F setzt Callbacks +0x10/+0x14 im Ord.-357-Objekt und wartet
   bei [+0x20] ≠ 0 mit `KeWaitForSingleObject(0x2D7540, 0, 1, 0, NULL)`.
7. `_rtinit` {0, 0x169467, 0x16EDEA}; C-Init {0, 0x166EE2, 0x16C29B, 0x16DDB0, 0x16ED64}; `[0x2D85FC]` = 0x1668A9;
   C++-Init 24.336 Einträge / 24.335 ungleich null / 21.791 mit `RegisterLoader` als 4. Befehl / 24.329 im Ni-Bereich,
   6 XDK-Initialisierer mit den genannten Schreibzielen.
8. `CApp::Init` 0xD26A0 und `Game::Init` 0xA26A0: Reihenfolge, Objektgrößen (0x7C Sound, 0xA4 Pause, 0xC Hints,
   0x40/0x1D0/0x18/0x1C/0x48 Kontexte), PAL/NTSC-Konstanten, „Ingame“ über String 0x2940CC, Intermission 0xD37C0,
   `BUILD.TXT` → 0x2E7A68 (≤ 0xFF Zeichen).
9. `CApp::Frame` 0xD2630 und `vf10` 0xD25D0 (`FUN_0009c140` → `XGetDeviceChanges` 0x9C15D; Sound-vf1 0xCBCB0 →
   `DirectSoundDoWork`); Profiler- und Hint-Aufrufe landen in Stubs 0xA3210/0xAED30/0xB0390.
10. `FUN_0002a660`: 8× (vf17(0,1) + vf16), dazwischen `FUN_0013cf90` und vf71; weitere vf16-Aufrufer 0xA62B0, 0xC8E90,
    0xEE080 im Decompilat.
11. Fiber-API 0x1708B0/0x17093C/0x17094F/0x17098E und Trampolin 0x17086B (`KeBugCheck(0)` bei 0x1708A9).
12. Synchronisation: `RtlEnterCriticalSection` nur in DSOUND, XAPI-Heap, Thread-Notify und CRT; kein Thunk mit dem Ghidra-Namen
    `NtCreateEvent`/`NtCreateSemaphore` (die Ordinale 189/192 fehlen; Zuordnung nach xboxkrnl-Konvention, Vermutung); `NtWaitForSingleObject` nur in `ReadFile`/`WriteFile`;
    `WaitForSingleObject` nur aus `GetOverlappedResult` (Aufrufer XMV 0x26471C) und D3D 0x24194E/0x241A7A/0x2420DE.
13. `SleepEx`-Aufrufer 0xE0034, 0xF6528, 0x13F514, 0x13F59E, 0x13F5F9; `Sleep`-Aufrufer D3D 0x2419B5/0x241BF2/0x242045,
    XMV 0x2646D5.
14. NV-Indizes am Stub 0xF9CC6: 7 (`XGetLanguage`, bei > 9 Region 0x104), 0x104, 0xA, 9 (`XGetAudioFlags`) und über
    `XQueryValue` 0x11 (0xF9B67) und 0xFFFF (0xF7035).
15. Exit-Kette: `doexit` 0x169F1B nur aus `_exit` 0x169FC5 (quick = 1), `_exit` nur über `[0x2D86AC]` aus `_amsg_exit`
    0x1692B4 (Aufrufer u. a. `_purecall` 0x1668B6, `_getptd` 0x169347); `_atexit` 77 Aufrufstellen.
16. Aufruferzahlen: `FUN_000a4900` 2011 Raw/976 Ghidra, `FUN_000a3880` 2696/949, `malloc` 12/11 (alle CRT),
    `LocalAlloc` 14/14; 101 von 102 `*::CreateObject` rufen `FUN_000a4900` auf.
17. Lade-Automat 0xC5B40: `cmp eax,0xA; ja; jmp [eax*4+0xC6FB4]` (11 Zustände).
18. Tabelle 4.4 (Interrupts): `HalGetInterruptVector`/`KeInitializeInterrupt`/`KeConnectInterrupt` je einmal in D3D
    (`CMiniport_InitHardware`), zweimal in DSOUND (0x24B09E, 0x250AE4) und einmal in XPP.

### Korrekturen dieses Durchgangs

| Stelle | vorher | jetzt | Beleg |
|---|---|---|---|
| 5.2 Strategie | Koaleszieren „läuft vorher“; keine Ausweichpfade | Koaleszieren erst nach einem Fehlschlag, danach zweiter Versuch; als letzte Möglichkeit die Freilisten der jeweils anderen Größenklasse; Mindestgröße 8 | Decompilat `FUN_000a4520` |
| 4.4 XPP | 3/3/11/11/21 Aufrufe (Herkunft Callgraph, nicht reproduzierbar) | Raw-Aufrufstellen 5× `KeInitializeDpc`, 4× `KeInitializeTimerEx`, 13× `KeSetTimer`, 20× `KeCancelTimer`, 21 (+1)× `KeRaiseIrqlToDpcLevel`, 26× `KfLowerIrql`, 4× `KeSetEvent`, 2 (+1)× `KeWaitForSingleObject`; `HalGetInterruptVector` in 0x28C8F0 | `verify_startup_r2_xdksync.py` |
| 3.3 | „QPF bei 0x241B4E und 0x241D4E in `FUN_00241c40`“ | 0x241B4E liegt außerhalb einer Ghidra-Funktion, nur 0x241D4E in `FUN_00241c40` | `verify_startup_r2_timer.py` |
| 3.5 / Verifikation 17 | `HalReturnToFirmware`-„Referenzen“ 0xF82D4/0xF902A | Aufrufbefehle 0xF82D2/0xF9028 (die früheren Werte sind die Operandenadressen) | `verify_startup_r2_mem.py` |
| 2.3 Schritt 3 | `RtlCreateHeap` als Tatsache | Name nach Argumentform, als Vermutung gekennzeichnet (Ghidra: `FUN_000f4ea8`) | Disassembly 0xF92E2–0xF9309 |
| 5.1 `LocalAlloc` | „D3D-Objekte, XAPI-Savegames“ | vollständige Liste der 14 Aufrufstellen, kein Spielcode | `verify_startup_r2_misc.py local` |

Die Korrekturen des ersten Review-Durchgangs (Heap-Fehler → (1,1,0); Flag 0x08; nur `$$XTIMAGE`; 21.791
`RegisterLoader`; erster ausgeführter = zweiter Tabelleneintrag; 41/201 Aufrufe; `SleepEx` in 0xE0030; `rdtsc`-Bytes in
DSOUND/XMV sind Daten; Clustergröße statt freiem Platz in 0xF8081; `WaitForSingleObject`-Aufrufer; 11 `fs:[4]`-Stellen;
Raw- gegenüber Ghidra-Zählungen; Hotfix schreibt ein Dword) wurden erneut geprüft und bleiben gültig.

### Ergänzungen dieses Durchgangs (im Bericht bisher nicht enthalten)

* **`XapiBootToDash` ist nicht `noreturn`** (`ret 0xC` bei 0xF9023). Der Loader muss in `HalReturnToFirmware` bzw.
  `XLaunchNewImageA` den Prozess beenden, sonst laufen `XapiInitProcess` und der Hauptthread nach einem Fehler weiter
  (3.5, Checkliste 9).
* **Pool schreibt 4 Byte vor seinen Anfang** (erste Allokation über `FUN_000a37b0`, 0xA3814/0xA381C). Den Pool also
  nicht direkt aus einem Windows-Heap holen (5.2, Checkliste 7).
* **XBE-Header muss an 0x10000 liegen**: XAPI liest Zertifikat, InitFlags, Stack-/Heapgrößen und die Sektionsheader
  direkt (Checkliste 10).
* **PAL/NTSC wählt auch Splash-/Legal-Bilder** im Intro-Kontext (0x2B65B/0x2B814) (3.4).
* **Variabler Zeitschritt ist toter Code**, und effektiv laufen höchstens etwa 6 Schritte pro Frame (3.2).
* **`SleepEx`-Semantik**: `KeDelayExecutionThread(1, Alertable, …)`; 0x101 → Wiederholung, 0xC0 → `WAIT_IO_COMPLETION`
  (4.3).
* **Fiber-Stacks**: Größe kommt aus dem HLE-`MmCreateKernelStack`, größere Stacks sind transparent (Checkliste 6).
* Device-Parameter MultiSampleType 0x11 und SwapEffect 1 (2.6); `XAutoPowerDownResetTimer` wird auch aus XPP
  (0x28D62B/0x28D651) aufgerufen (4.4).

### Offen bzw. weiterhin unsicher

* Namen der Ordinale 258/357/360 (und 24, 74, 232, 164, 322, 324 dort, wo kein `*_exref` vorliegt): nur durch Aufrufform
  bzw. Datenzugriff gestützt. Die benachbarten Ghidra-Namen (Slot 70 = Ord. 359 `IoMarkIrpMustComplete`, Slot 71 =
  Ord. 358 `HalIsResetOrShutdownPending`) widersprechen der Vermutung `HalInitiateShutdown` für 360 nicht.
* Die Lage von `KTHREAD->TlsData` bei `StackBase − 0x24` folgt nur aus der Konsistenz von Schreiber
  (`XapiThreadStartup`) und Lesern; für den Loader genügt diese Konsistenz.
* Welche Folgen der Schreibzugriff vor dem Poolanfang in einem Windows-Heap hätte, ist nicht getestet (Vermutung).
* Inhalt der Fiber-Jobs und damit der Stackbedarf im Fiber; Identität der Basisklasse `FUN_001091a0` (NiStream);
  Bedeutung von `fs:[0x58]` (DSOUND) und von 0x400 im Protect-Wert; Bedeutung von `[0x2E7B68]` (PAL 0.03937 ≈ 1/25,4,
  NTSC 0.03279 ≈ 1/30,5; nur in 0xA26A0 geschrieben, kein absoluter Leser gefunden).
* Laufzeitverhalten (Ende der Hauptschleife im Retail-Spiel, PAL60) wurde nicht getestet.
