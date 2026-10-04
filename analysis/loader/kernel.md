# xboxkrnl-Imports von `default.xbe` (Retail EU) – Analyse für den Host-Loader

Stand: 2026-10-04 · Ziel-Binary: `orig\retail_eu\default.xbe` · maschinenlesbar: `kernel_imports.tsv` (gleiche Spalten wie die Volltabelle unten, plus Details)

## 1. Kurzfassung

- Die Kernel-Thunk-Tabelle liegt bei **0x293560** (XBE-Header-Feld 0x158 = 0x5B4475D6, XOR Retail-Key 0x5B6D40B6 → 0x293560). Sie hat **111 Einträge** (alle `0x80000000|Ordinal`), Terminator 0 bei 0x29371C.
- Alle 111 Ordinal→Name-Zuordnungen wurden gegen Ghidra geprüft: **108 bestätigt** (Callgraph-`EXTERNAL`-Namen, Namen im dekompilierten Code, Namen der Thunk-Stubs), **3 ohne Ghidra-Beleg**, weil die referenzierende Stelle in Code liegt, den Ghidra nicht als Funktion angelegt hat (PsTerminateSystemThread, HalInitiateShutdown, IdexChannelObject). Diese drei sind semantisch stimmig (Abschnitt 3). **Kein Widerspruch.** Nebenbei: Ghidras `EXTERNAL:<idx>` ist bei allen 87 aufgerufenen Imports gleich Slot+1.
- Arten: **98 aufgerufene Funktionen**, **2 Funktionen, deren Adresse nur weitergegeben wird** (IoInvalidDeviceRequest, NtUserIoApcDispatcher), **11 Datenexporte** (Abschnitt 5).
- Aufrufkonventionen aus dem Binary: **93 stdcall**, **4 fastcall** (KfLowerIrql, KfRaiseIrql, IofCompleteRequest, ObfDereferenceObject), **1 cdecl** (DbgPrint). Alle 93 stdcall-Purges sind stack-konsistent; 85 davon legt der Stack-Solver eindeutig fest, 8 nur über Push-Zählung (Abschnitt 4).
- **630 Aufrufstellen** (550 `call [slot]`, 49 `call reg` nach `mov reg,[slot]`, 26 über 6-Byte-Stubs `jmp [slot]`, 5 Tail-Jumps). Verteilung: XAPI 232, DSOUND 165, XPP 153, D3D 59, LIBC 18, Spielcode 3, **XMV 0**.
- Spielcode und NetImmerse rufen den Kernel (fast) nie direkt auf. Ausnahmen: zwei Aufrufer von MmFreeContiguousMemory (FUN_000ee080 über den Stub XAPILIB::XPhysicalFree, sub_161180 über den D3D-Stub 0x2417b0).
- **37 Imports** werden nur von D3D/DSOUND/XPP verwendet. Ersetzt man diese Bibliotheken per HLE, reichen dafür Stubs. **74 Imports** brauchen echte Host-Implementierungen (XAPI, LIBC, Spielcode). Davon sind 51 trivial, 21 mittel und 2 schwer (NtCreateFile mit Pfadübersetzung, PsCreateSystemThreadEx mit KPCR/TLS-Emulation).
- Wichtige Begleitbefunde für den Loader (Abschnitt 8):
  - Der Code greift direkt auf das KPCR zu (fs:[0x20/0x24/0x28/0x58], insgesamt 27 Stellen).
  - TLS läuft über fs:[4] mit negativem `_tls_index`.
  - XAPI patcht je nach `XboxKrnlVersion.Build` Kernelcode bei 0x800299A0–0x800398CD (Adresse hängt zusätzlich von `XboxHardwareInfo.Flags` Bit 1 ab; Review-Korrektur, vorher „0x8003xxxx“).
  - XapiInitProcess lädt beim Boot die nicht-PRELOAD-Sektion `$$XTIMAGE` per XeLoadSection und bootet ins Dashboard, wenn die Kindersicherung (ExQueryNonVolatileSetting 0xA) ≠ 0 ist (Review-Ergänzung, Abschnitt 7 und „Verifikation“).
  - Das Spiel nutzt Fibers und ReadFileEx/WriteFileEx (APCs).
  - Der XBE-Header muss bei 0x10000 gemappt sein.
  - QueryPerformanceCounter/QueryPerformanceFrequency in XAPI sind `rdtsc` bzw. eine fest verdrahtete Frequenz von 733 MHz.

## 2. Methode und Skripte (reproduzierbar)

Alle Skripte liegen in `analysis\loader\scripts\`. Reihenfolge: `python kernel_analyze.py && python kernel_report.py`.

| Skript | Zweck |
|---|---|
| `kernel_common.py` | Pfade, XBE-Zugriff, Funktionsliste, Kandidaten-Tabelle Ordinal→Name (xboxkrnl-Exporttabelle 1..366). Die Tabelle wird gegen Ghidra verifiziert und nicht ungeprüft übernommen. |
| `kernel_analyze.py` | Thunk-Tabelle dekodieren. Alle Code-Referenzen auf Slots finden: Byte-Scan der Code-Sektionen, dann capstone-Dekodierung; nur Operanden `[disp32]` ohne Basis-/Indexregister zählen. Stubs `jmp [slot]` und alle `call`/`jmp rel32` darauf finden. Für jede enthaltende Funktion einen CFG-Walk mit symbolischem Stackmodell durchführen (push/pop, `sub/add esp`, ebp-Frames, `__SEH_prolog`/`__SEH_epilog`, `ret N` der Callees). Pro Aufrufstelle werden erfasst: Argument-Pushes, Cleanup danach, ecx/edx gesetzt, eax/al danach gelesen, Quell-Instruktionen der Argumente. Danach bestimmt ein **Purge-Solver** die Stack-Bereinigung jedes Imports per Koordinatenabstieg über 0..0x30 Bytes. Er minimiert Stack-Inkonsistenzen (Tiefe ≠ 0 bei `ret`/Tail-Jump, widersprüchliche Tiefen an CFG-Joins, negative Tiefe). Code ohne Ghidra-Funktion erhält Pseudo-Einstiege (`sub_xxxxxx`). Ausgabe: `scripts\kernel_raw.json`. |
| `kernel_report.py` + `kernel_host.py` | Ghidra-Abgleich (`scripts\kernel_crosscheck.txt`), Modul-Zuordnung der Aufrufer, Konventionsentscheidung, Host-Plan (handgeschrieben in `kernel_host.py`). Schreibt `kernel_imports.tsv` und `scripts\kernel_tables.md`. |
| `kernel_show_site.py <Name\|0xAdresse>` | Belegt einzelne Aufrufstellen mit Disassembly (Evidenz-Helfer). |
| `kernel_fs_usage.py` | Zählt fs:-Segmentzugriffe (KPCR) im Code. |
| `kernel_rdtsc_scan.py` | Findet `rdtsc` und die 733-MHz-Konstante (Zeitbasis). |

Modul-Zuordnung der Aufrufer:
- nach Sektion: D3D, DSOUND, XMV, XPP;
- in `.text`: 0xF3DE0–0xFA5A0 und 0x1707A0–0x170ABA = XAPI (XbSymbolDatabase-Namen `XAPILIB::*` liegen dort), 0x1668CC–0x171000 = LIBC (FID-Namen `_strncpy`, `__global_unwind2` …);
- alles übrige in `.text` = GAME (Spiel/NetImmerse).

Die Grenzen der .text-Bereiche sind aus der Lage benannter Funktionen abgeleitet. Für einzelne `FUN_`-Funktionen ist das eine Vermutung.

## 3. Thunk-Tabelle und Namensabgleich

- Dekodierung: `kernel_common.read_thunks()` liest ab 0x293560, bis ein Wert 0 kommt; Ordinal = `value & 0x7FFFFFFF`.
- Abgleich (`kernel_crosscheck.txt`) pro Slot:
  - (a) Callgraph-Eintrag `EXTERNAL:<Slot+1>` mit gleichem Namen;
  - (b) Name (bzw. `<Name>_exref`) im dekompilierten Text jeder Ghidra-Funktion, die den Slot referenziert;
  - (c) Name des Stubs in `functions.tsv`.
- Ergebnis: 108 × OK, 0 × Widerspruch, 3 × keine Ghidra-Referenz:
  - **PsTerminateSystemThread** (Slot 76, Ord 258): einzige Stelle 0x0F9A6B in `sub_0f99da`, der Thread-Startroutine. CreateThread übergibt 0xF99DA als letztes Argument an PsCreateSystemThreadEx (Push bei 0x0F9A84). Danach ruft sie `call [ebp+8]` (Thread-Routine) auf, pusht den Rückgabewert (`push [ebp-0x28]`), ruft `call [0x293690]` und es folgt `int3`. Das passt zu einem nicht zurückkehrenden `PsTerminateSystemThread(ExitStatus)`.
  - **HalInitiateShutdown** (Slot 78, Ord 360): Stelle 0x0F9B11 in `sub_0f9b08` (`cmp [0x310388],0; je; call [0x293698]; ret 0x10`) ohne Argumente. 0xF9B08 ist die DPC-Routine, die FUN_000f9b1a per KeInitializeDpc an den Timer 0x310360 (AutoPowerDown) bindet. Das passt zu einem Auto-Power-Down.
  - **IdexChannelObject** (Slot 87, Ord 357): Datenzugriffe `[eax+0x20]`, `[esi+0x10]`, `[esi+0x14]` in sub_0f9c63/sub_0f9c7f. Das ist ein Datenobjekt und kein Funktionsaufruf, was zu einem Datenexport passt.
- Verworfene Byte-Treffer (3): zwei indizierte Tabellenzugriffe `mov reg,[esi*4+0x293608]` (0x235831 in `D3D8::D3DDevice_SetRenderStateNotInline`, 0x14791D in FUN_001478c0; beide Pfade nur für 0x5C ≤ Index < 0x88, weil vorher `cmp esi,0x5c; jge` bei 0x235805/0x1478EF steht, effektive Adressen also 0x293778–0x293824, hinter dem Terminator 0x29371C – Review-Präzisierung). Ghidra zeigt sie als `(&MmFreeSystemMemory_exref)[i]`, es sind aber **keine Kernelzugriffe**. Dazu ein Zufallstreffer in einem `call rel32` (0x23BCC6).

### Bemerkenswert: nicht importiert

Diese in der Aufgabenstellung genannten Exporte stehen **nicht** in der Thunk-Tabelle und müssen daher nicht implementiert werden:
- KeTickCount, KeQueryPerformanceCounter/Frequency: XAPI QueryPerformanceCounter 0xF641B ist `rdtsc`, QueryPerformanceFrequency 0xF642C liefert die Konstante 0x2BB5C755 = 733 333 333 Hz, siehe 8.8.
- XeImageFileName, XboxEEPROMKey, HalReadSMBusValue, HalWriteSMBusValue, MmMapIoSpace, IoDismountVolume.
- XcHMAC: FUN_000f6f26/FUN_000f6f88 rufen XcSHAInit/XcSHAUpdate mit 0x40- bzw. 0x54-Byte-Blöcken auf. Das ist das HMAC-Muster (Vermutung); XAPI baut HMAC also selbst.
- NtCreateEvent/KeInitializeEvent: kein Import, die XAPI-Event-API ist also offenbar nicht eingebunden.
- PsCreateSystemThread (nur die Ex-Variante), KiUnlockDispatcherDatabase, ObfReferenceObject.

## 4. Aufrufkonventionen (Evidenz aus dem Binary)

- **stdcall (93):** Bei keiner Aufrufstelle folgt `add esp,N`, die Callee räumt also auf. Die Argumentzahl ist Purge/4.
  - Der Purge-Solver testet für jeden Import 0..0x30 Bytes Purge in allen Funktionen, die ihn aufrufen. Mit dem gewählten Wert gibt es überall 0 Inkonsistenzen.
  - Beispiel RtlLeaveCriticalSection: Purge 4 → 0 Inkonsistenzen, jeder andere Wert ≥ 111. Beispiel NtCreateFile: Purge 36 → 0, sonst ≥ 15.
  - Unabhängig davon stimmt die Zahl der Argument-Pushes (`pend` in der TSV) bei allen Sites überein. Ausnahme: eine RtlUnwind-Site in `__global_unwind2`, wo `push ebp` um den Call herum gespart wird.
  - Nur über Push-Zählung bestimmt (der Solver hat dort keinen Constraint, meist eine einzige Site in einer Frame-Pointer-Funktion): NtFlushBuffersFile (2), MmAllocateSystemMemory (2), NtQueryFullAttributesFile (2), ExFreePool (1), PsTerminateSystemThread (1), MmClaimGpuInstanceMemory (2), RtlUnwind (4), DbgBreakPoint (0, nur Tail-Jump aus `XAPILIB::DebugBreak`, daher Vermutung).
  - Beispiel 0x0F6B09 in `XAPILIB::XCreateSaveGame`: 9 Pushes, dann `ff 15 c4 35 29 00` = `call [NtCreateFile]`, danach direkt `and byte ptr [esi],0` ohne Stack-Cleanup.
  - Gegenüber Windows NT abweichende Xbox-Signaturen, die damit belegt sind:
    - NtCreateFile hat 9 statt 11 Argumente;
    - NtAllocateVirtualMemory hat 5, NtFreeVirtualMemory 3 und NtQueryVirtualMemory 2 Argumente (jeweils ohne ProcessHandle);
    - PsCreateSystemThreadEx hat 10 Argumente;
    - NtReadFile/NtWriteFile haben 8 statt 9 Argumente (kein `Key`; 8 Pushes in ReadFileEx/WriteFileEx vor 0x0F7630/0x0F7681 – Review-Ergänzung);
    - NtQueryDirectoryFile hat 10 Argumente (NT: 11);
    - KeSetTimer belegt 16 Bytes, weil DueTime ein LARGE_INTEGER (2 DWORDs) ist.
- **fastcall (4):** Purge 0, keine Stack-Argumente, und vor jedem Call wird ecx (bzw. edx) gesetzt.
  - KfLowerIrql: ecx gesetzt an 28/28 Sites, z. B. 0x0F9C8C `8a c8 mov cl,al` vor `call [0x293560]`.
  - KfRaiseIrql: 2/2, z. B. 0x290AFA `mov cl,[eax+0x3102cc]`; das Ergebnis in al wird gelesen.
  - IofCompleteRequest: ecx 8/9, edx 9/9, z. B. 0x28F132 `32 d2 xor dl,dl; 8b c8 mov ecx,eax`.
  - ObfDereferenceObject: einzige Stelle ist der Tail-Jump 0x28D67A `jmp [0x293658]` nach `mov ecx,edi`.
  - Der Host muss diese vier als `__fastcall` implementieren. MSVC-x86-`__fastcall` passt dazu (ecx, edx, Rest auf dem Stack, Callee räumt auf).
- **cdecl (1):** DbgPrint. Einzige Site 0x23E627 in D3D FUN_0023e600 mit 1 Push, danach `add esp,0x118`. Das sind 0x104 lokaler Puffer + 0x10 (vier Argumente des vorherigen `_vsnprintf`-artigen Calls 0x16853A) + 4 (Argument von DbgPrint). Der Caller räumt also auf.
- **Rückgabewerte:** z. B. KeRaiseIrqlToDpcLevel – al wird an 22/24 Sites gelesen (`mov cl,al` usw.). Die Spalte `ret_used` der TSV zeigt, wie oft eax/al nach dem Call gelesen oder zurückgegeben wird.
- **ABI-Folgen für den Host:**
  - Der Code legt Importzeiger in callee-saved Registern ab und ruft sie mehrfach auf (49 Sites `mov edi,[slot] … call edi`). Host-Funktionen müssen also ebx/esi/edi/ebp erhalten, wie es das Standard-MSVC-ABI tut.
  - Alle Kernelaufrufe laufen über die 111 Slots, inklusive der 10 Stubs. Es genügt daher, die Slots zu befüllen: bei Funktionen mit Host-Funktionszeigern, bei Datenexporten mit Zeigern auf Host-Datenobjekte.

## 5. Datenexporte und adressierte Funktionen (werden nicht aufgerufen)

| Slot | Name | Zugriff im Binary (Beleg) | Host |
|---:|---|---|---|
| 18 | ExEventObjectType | 0x28DA06 `push [0x2935a8]` → ObjectType-Argument von ObReferenceObjectByHandle (XPP) | Dummy-Objekt |
| 30 | XboxHDKey | 0x0F7004 `mov esi,[0x2935d8]`, Bytezugriffe in FUN_000f6ffa (Signatur-Schlüsselableitung); sind alle 16 Byte 0 (`or al,[ecx+esi]`-Schleife 0x0F700F–0x0F7016), holt FUN_000f6ffa stattdessen per XQueryValue(0xFFFF) das ganze EEPROM (0x0F7030/0x0F7035) | 16-Byte-Dummy, **≠ 0** (Review-Ergänzung) |
| 31 | XboxSignatureKey | 0x0F70CE `mov eax,[0x2935dc]; mov [edi+4],eax` (XCalculateSignatureBeginEx) | 16-Byte-Dummy |
| 32 | XboxAlternateSignatureKeys | 0x0F70C6 `add eax,[0x2935e0]` (= &Keys[i], 16 Byte je Eintrag laut Decomp `+ uVar2*0x10`) | 16×16-Byte-Dummy |
| 47 | HalDiskCachePartitionCount | 0x0F7DD4/0x0F7E2B `mov eax,[0x29361c]; mov edx,[eax]` (XapiSelectCachePartition) | DWORD (Vermutung: 3) |
| 53 | LaunchDataPage | Zeigervariable: FUN_000f81bb liest `cmp [eax],0` und schreibt `mov [ecx],eax`; XapiInitProcess liest und nullt `and [eax],0` (0x0F945E) | beschreibbares DWORD = NULL |
| 64 | XboxHardwareInfo | Byte +0: `test [eax],8` (XapiInitProcess 0x0F9342, FUN_000f9b1a), `test [eax],1` (XPP); +1: `test [edx+1],2` (D3D 0x23C84C); +5: `cmp [eax+5],0xA1` (XPP 0x28C8FB); FUN_000f9804 liest DWORD +0 | Struktur mit Retail-Werten (Bit-Bedeutungen laut XDK: Vermutung) |
| 73 | XePublicKeyData | 0x0F90BE `push [0x293684]` → 2. Argument von XcVerifyPKCS1Signature | Dummy |
| 75 | XboxKrnlVersion | 0x0F9811 `movzx ecx,word [ecx+4]` = Build, Vergleich mit 0xF68/0xFC2/0xFC7/0x12D1..0x12D2 (Abschnitt 8.2) | Build außerhalb dieser Werte |
| 87 | IdexChannelObject | sub_0f9c7f schreibt `[esi+0x10]=0xF9C7C`, `[esi+0x14]=0xF9C63` und liest `[esi+0x20]`; sub_0f9c63 setzt `[eax+0x20]=0` | Null-Puffer ≥ 0x24 Byte |
| 110 | HalBootSMCVideoMode | XAPILIB::XGetAVPack 0x170AA5: `mov eax,[0x293718]; mov eax,[eax]; ret` | konfigurierbares DWORD |
| 8 | IoInvalidDeviceRequest *(Funktion)* | XPP FUN_0028d254: `mov eax,[0x293580]`, dann 10× `mov [0x28c004..],eax` (Dispatch-Tabelle eines DRIVER_OBJECT) | Dummy-Routine |
| 39 | NtUserIoApcDispatcher *(Funktion)* | 0x0F7625/0x0F7676 `push [0x2935fc]` = ApcRoutine-Argument von NtReadFile/NtWriteFile in ReadFileEx/WriteFileEx | Marker-Adresse, die das Host-NtReadFile/NtWriteFile erkennt |

## 6. Wer braucht was

| Modul | Sites | Importe (nur dieses/ersetzte Module) |
|---|---:|---|
| XAPI (.text) | 232 | 46 Importe werden ausschließlich von XAPI genutzt (Datei-I/O, Threads, Signaturen, Settings, Launch/Reboot) |
| DSOUND | 165 | Locks (96 der 108 RtlLeaveCriticalSection-Sites), Timer/DPC/Interrupt (APU), Pool, Mm*Lock/PhysicalAddress |
| XPP (USB/XInput/MU-Treiber) | 153 | IRQL, Timer, DPC, Interrupt, IRP-Funktionen, IoCreateDevice |
| D3D | 59 | Contiguous Memory, Av*, Interrupt/DPC (GPU), HalReadWritePCISpace, MmClaimGpuInstanceMemory, Movie-Cache-Datei-I/O (FUN_00241c40, Strings 0x243800 „Unable to re-open movie cache file…“) |
| LIBC | 18 | RtlEnter/Leave/InitializeCriticalSection, RtlUnwind, KeBugCheck |
| GAME | 3 | MmFreeContiguousMemory (über die Stubs XPhysicalFree 0xF8463 und 0x2417B0) |
| XMV | 0 | – |

**Nur von D3D/DSOUND/XPP genutzt (37, bei HLE-Ersatz reicht ein Stub):** IoCreateDevice, IoInvalidDeviceRequest, ExEventObjectType, NtSetEvent, MmQueryAllocationSize, KeCancelTimer, ObfDereferenceObject, ObReferenceObjectByHandle, IoStartPacket, IofCompleteRequest, IoStartNextPacket, ExFreePool, ExAllocatePoolWithTag, IoMarkIrpMustComplete, HalIsResetOrShutdownPending, HalGetInterruptVector, KfRaiseIrql, KeConnectInterrupt, KeInitializeInterrupt, KeStallExecutionProcessor, MmGetPhysicalAddress, MmLockUnlockBufferPages, KeInsertQueueDpc, MmLockUnlockPhysicalPage, AvGetSavedDataAddress, AvSendTVEncoderOption, AvSetDisplayMode, AvSetSavedDataAddress, HalReadWritePCISpace, KeDisconnectInterrupt, DbgPrint, MmClaimGpuInstanceMemory, KeSaveFloatingPointState, KeRestoreFloatingPointState, ExQueryPoolBlockSize, KeRemoveQueueDpc, KeSynchronizeExecution.

Hinweis: XInput liegt in der XPP-Sektion (XInputOpen/XInputGetState/XGetDevices/XInitDevices, `XAPILIB::*`-Namen ab 0x28C09C). Ersetzt man XInitDevices und die XInput-API per HLE, bleibt der gesamte USB-Treiberstapel ungenutzt.

## 7. Host-Implementierung nach Gruppen (Priorität für den ersten Boot)

Boot-Pfad laut Binary:
1. `entry` 0xF84DD berechnet die TLS-Größe nach [0x310394] und `_tls_index` und ruft dann `CreateThread(…, 0xF8469, …)` auf, was zu **PsCreateSystemThreadEx** führt.
2. Thread 0xF8469 ruft FUN_000f9804 auf (XboxHardwareInfo, XboxKrnlVersion, ggf. Mm*AddressProtect).
3. Danach folgt `XapiInitProcess` mit HalRegisterShutdownNotification, IoCreateSymbolicLink, LaunchDataPage, MmFreeContiguousMemory, XboxHardwareInfo und FUN_000f9b1a. FUN_000f9b1a ruft KeInitializeTimerEx und KeInitializeDpc auf. XapiInitProcess ruft außerdem XMountUtilityDrive auf, das auf NtOpenFile/NtQueryVolumeInformationFile/IoCreateSymbolicLink und XapiSelectCachePartition zugreift (HalDiskCachePartitionCount, NtRead/WriteFile).
   - Review-Ergänzung (Disassembly 0x0F92C0–0x0F946D, InitFlags [0x10124] = 5, Zertifikat-GameRatings = 0): XapiInitProcess ruft in dieser Reihenfolge HalRegisterShutdownNotification(0x2D7530, 1), FUN_000f9b1a (AutoPowerDown, liest Setting 0x11), FUN_000f4ea8 (XAPI-Heap, NtAllocateVirtualMemory), FUN_000f90e6 (DVD-Prüfung: NtOpenFile, NtDeviceIoControlFile 0x4D014, XcSHA*/XcVerifyPKCS1Signature – **Ergebnis wird ignoriert**: `test eax,eax; jmp` bei 0x0F931C/0x0F931E), FUN_000f92af (NtOpenFile/NtQueryVolumeInformationFile auf `\Device\Harddisk0\partition1\`, Fehler → XapiBootToDash), IoCreateSymbolicLink(`\??\D:` → `\Device\CdRom0`, nur wenn XboxHardwareInfo.Flags Bit 3 = 0), FUN_000f8f75 (T:/U: → `\Device\Harddisk0\partition1\TDATA\%08lx` bzw. `UDATA\…` per XapiMapLetterToDirectory; schreibt TitleMeta.xbx/TitleImage.xbx und lädt dafür `$$XTIMAGE` per XeLoadSection), XMountUtilityDrive, XGetParentalControlSetting (ExQueryNonVolatileSetting 0xA; Ergebnis > GameRatings 0 → XapiBootToDash bei 0x0F93E8–0x0F9404). Für den ersten Boot sind also auch XeLoadSection/XeUnloadSection, NtCreateFile/NtWriteFile/NtSetInformationFile/NtQueryInformationFile und KeQuerySystemTime nötig.
4. Danach folgen `_rtinit`/`_cinit` und `FUN_000d29b0(0,0,0)` (cdecl, 0x0F84C5; Vermutung: Spiel-`main`), anschließend `XapiBootToDash(1,1,0)`.

Gruppen:

1. **Speicher (trivial):**
   - NtAllocate/Free/QueryVirtualMemory → VirtualAlloc/VirtualFree/VirtualQuery. Protect ist an allen 11 Sites 4 (PAGE_READWRITE).
   - MmAllocateContiguousMemory(Ex) → VirtualAlloc. D3D übergibt Protect 0x404 und Highest 0x7FFFFFF; WRITECOMBINE und Adressgrenzen werden ignoriert. Die Größe wird für MmQueryAllocationSize gemerkt.
   - MmFreeContiguousMemory → VirtualFree. MmPersistContiguousMemory → no-op.
   - MmAllocate/FreeSystemMemory → VirtualAlloc/VirtualFree.
   - Mm(Set|Query)AddressProtect → VirtualProtect/VirtualQuery.
   - MmCreate/DeleteKernelStack nur für XAPI-Fibers, siehe 8.4.
2. **Synchronisation (mittel):**
   - RTL_CRITICAL_SECTION hat ein Xbox-Layout mit eingebettetem KEVENT im Gastspeicher. Eigene Implementierung auf den Gastfeldern oder eine Map Gastadresse → Host-CRITICAL_SECTION.
   - KEVENT/KTIMER/KDPC im Gastspeicher mit Host-Objekten (Map) spiegeln: KeSetEvent, KeWaitForSingleObject, KeInitializeTimerEx, KeSetTimer, KeInitializeDpc.
   - Eine globale „Dispatcher-Lock“-Emulation für die IRQL (KeRaiseIrqlToDpcLevel/KfLowerIrql) plus einen DPC-Thread vorsehen.
   - Waits: NtWaitForSingleObject(Ex) → WaitForSingleObject(Ex); KeDelayExecutionThread → SleepEx. Umrechnung: negative Werte = relativ, Einheit 100 ns.
3. **Datei-I/O (mittel/schwer):**
   - NtCreateFile/NtOpenFile mit Pfadübersetzung über eine eigene Symlink-Tabelle, die IoCreateSymbolicLink/NtOpenSymbolicLinkObject/NtQuerySymbolicLinkObject füllen bzw. beantworten. Strings sind ANSI_STRING.
   - Die verwendeten Informationsklassen sind NT-kompatibel und können an ntdll durchgereicht werden:
     - Query 0x22/0xE;
     - Set 4/0xD/0xE/0x13/0x14;
     - Volume 3;
     - Directory 1 (Dateiname ANSI, umsetzen!).
   - NtRead/WriteFile: APC-Zustellung für ReadFileEx/WriteFileEx (8.5).
   - NtFsControlFile (0x90020) und NtDeviceIoControlFile (0x4D014, 0x70000, 0x74004) kommen nur in XAPI-Routinen (FUN_000f946d aus XMountUtilityDrive, FUN_000f90e6 aus XapiInitProcess; Zweck „Utility-Partition formatieren“ bzw. „DVD-Abfrage“ = Vermutung) vor → Fake-Antworten.
4. **Threads (schwer):** PsCreateSystemThreadEx (Argumente bei 0x0F9AAB: ThreadHandle*, 0, StackSize, TlsDataSize = [0x310394], ThreadId*, Ctx1, Ctx2, CreateSuspended, 0, SystemRoutine 0xF99DA) → CreateThread mit Trampolin, das KPCR/KTHREAD-Ersatz und TLS bereitstellt (8.1). PsTerminateSystemThread → ExitThread.
5. **Settings, Signaturen, Sonstiges (trivial):**
   - ExQueryNonVolatileSetting: Indizes 7/0x104/9/0xA an den Sites → Konfigurationstabelle. Über XQueryValue kommen zusätzlich 0x11 (FUN_000f9b1a, Bit 0 = AutoPowerDown) und 0xFFFF (FUN_000f6ffa, nur bei XboxHDKey = 0) an (Review-Ergänzung). **0xA (Kindersicherung) muss 0 liefern** (oder einen Fehler, dann liefert XGetParentalControlSetting 0), sonst bootet XapiInitProcess ins Dashboard.
   - XcSHAInit/Update/Final: eigenes SHA-1.
   - XcVerifyPKCS1Signature → TRUE (unkritisch: das Ergebnis der DVD-Prüfung FUN_000f90e6 wird in XapiInitProcess ignoriert).
   - Datenexporte siehe Abschnitt 5.
   - XeLoad/UnloadSection: alle Sektionen außer `$$XTIMAGE`/`.XTLID` (INSERTED_FILE) sind PRELOAD. `$$XTIMAGE` wird aber beim Boot tatsächlich geladen (XapiMapLetterToDirectory, String 0x2A5EB8, XLoadSectionByHandle 0x0F8E74) und als TitleImage.xbx geschrieben. Der Loader muss deshalb auch die Rohdaten von `$$XTIMAGE` an VA 0x3178C0 bereitstellen (vorab mappen oder in XeLoadSection kopieren); danach zählen die beiden Funktionen nur Referenzen (Review-Korrektur).
   - HalReturnToFirmware (Argumente 2 bzw. 4) → Prozessende/Neustart. Argument 4 kommt nur vor, wenn die eigene TitleID 0xFFFE0000 ist (0x0F8FD8). Für dieses Spiel läuft auch XapiBootToDash über XLaunchNewImageA → FUN_000f829c → Argument 2 (Review). HalInitiateShutdown → ignorieren.
   - KeBugCheck → abbrechen. RtlUnwind/RtlRaiseException → ntdll (setzt fs:[0] = Windows-SEH-Kette voraus).
6. **Nur ersetzte Libraries:** Liste in Abschnitt 6 → Stubs mit plausiblen Rückgabewerten (Connect = TRUE, AvGetSavedDataAddress = NULL, MmGetPhysicalAddress = Identität …).

Optional lassen sich die XAPI-Funktionen selbst auf API-Ebene ersetzen (CreateFile/ReadFile/…, CreateThread, Fiber-API, XCreateSaveGame/XFind*SaveGame, XCalculateSignature*). Dann entfallen die meisten der 46 nur von XAPI genutzten Kernel-Imports. Für Spielcode bleiben fast nur noch Locks, Speicher, RtlUnwind und KeBugCheck übrig.

## 8. Begleitbefunde für den Loader (außerhalb der Thunk-Tabelle, aber zwingend)

### 8.1 Direkte KPCR-Zugriffe über fs (`kernel_fs_usage.py`)

| Zugriff | Anzahl | Wo (Beispiele) | Bedeutung |
|---|---:|---|---|
| fs:[0] | 5054 (Review: eigene Zählung 4850 + 318 = 5168, siehe „Verifikation“) | .text, überwiegend Spiel-/Engine-Code (MSVC-SEH-Prologe `mov fs:[0],esp`) | SEH-Kette, kompatibel mit Windows-TEB |
| fs:[4] | 11 | GetLastError/SetLastError, sub_0f8469, SwitchToFiber, ConvertThreadToFiber, Fiber-Startroutine 0x170880, LIBC FUN_001692d0/FUN_00169467/0x169383, Spiel FUN_0013f250/FUN_0013f590 | TLS-Basis, siehe unten |
| fs:[0x20] | 8 | XAPI-Heap, sub_0f8469, UnhandledExceptionFilter, FUN_000f829c, D3D; **alle 8** lauten `mov eax,fs:[0x20]; mov eax,[eax+0x250]; test eax,eax` (Review) | KPCR.Prcb (Vermutung); Host: Block mit DWORD +0x250 = 0 genügt |
| fs:[0x24] | 8 | GetLastError (`cmp al,2`), CreateThread, LIBC FUN_001692d0 und 0x16935E (KeBugCheck(0xA) wenn ≥ 2), DSOUND | IRQL (Vermutung, konsistent mit DISPATCH_LEVEL = 2); Host: Wert < 2 |
| fs:[0x28] | 9 | GetCurrentThreadId (`[fs:[0x28]+0x12c]`), XapiThreadStartup/CreateThread/LIBC (`[+0x28]` = TlsData), ConvertThreadToFiber (`[+0x1c]`, Review-Ergänzung) | aktueller KTHREAD (Vermutung); benutzte Offsets +0x1C, +0x28, +0x12C |
| fs:[0x58] | 2 | DSOUND FUN_00246d7a/FUN_00246d9f | (unbekannt, nur DSOUND) |

- Unter einem Windows-TEB liegen an 0x20/0x24/0x28 andere Felder. Die 27 Zugriffe außerhalb von fs:[0]/fs:[4] sind zu patchen bzw. umzuleiten; DSOUND/D3D entfallen bei HLE.
- **TLS:** `entry` berechnet die TLS-Größe:
  - TlsDataSize = (SizeOfZeroFill 0x20 + RawData 0 + 0xF) & ~0xF + 4 = 0x24 → [0x310394];
  - `_tls_index` = TlsDataSize / (−4) = −9 → [0x2F3D18] (0xF850F–0xF852F, `idiv` durch −4).
  - Zugriffe lauten `mov eax,[0x2f3d18]; mov ecx,fs:[4]; mov eax,[ecx+eax*4]`, gehen also **unterhalb** von NT_TIB.StackBase.
  - Unter Windows ist fs:[4] die echte Stack-Obergrenze. Die 11 fs:[4]-Stellen sind daher zu patchen, z. B. auf einen eigenen TlsSlots-Eintrag `fs:[0xE10+4n]` bei gleicher Befehlslänge. Vermutung, ungetestet.
- TLS-Directory 0x2A5D2C: RawData 0..0, AddressOfIndex 0x2F3D18, SizeOfZeroFill 0x20, keine Callbacks.

### 8.2 XAPI patcht Kernelcode abhängig von XboxKrnlVersion

FUN_000f9804 ist der erste Call des Hauptthreads (0x0F8469):
- Gelesen werden `XboxKrnlVersion.Build` (+4) und Bit 1 von `XboxHardwareInfo.Flags`.
- Für Build 0xF68, 0xFC2, 0xFC7 und 0x12D1–0x12D2 ergibt sich eine Adresse im Kernel. Review-Korrektur (nachgerechnet aus `neg al; sbb eax,eax; and eax,M; add eax,B` bei 0x0F9844–0x0F9878): Flags-Bit 1 = 0 → 0x8002ABBA (0xF68), 0x8002AB88 (0xFC2), 0x8002AB98 (0xFC7), 0x800299A0 (0x12D1/0x12D2); Bit 1 = 1 → 0x80039749, kein Patch (0xFC2), 0x800398CD, 0x8003838F. Die frühere Angabe „0x8003xxxx“ deckte nur den Fall Bit 1 = 1 ab.
- Enthält das DWORD dort 0x20C5048D, wird es mit MmQueryAddressProtect/MmSetAddressProtect beschreibbar gemacht und durch 0x28C5048D ersetzt.

**Der Host muss eine Build-Nummer außerhalb dieser Werte melden.** Sonst schreibt das Spiel an eine Adresse ≥ 0x80000000.

### 8.3 XBE-Header muss bei 0x10000 gemappt sein

- `entry` liest [0x10118] (Certificate-Adresse) und [0x10108] (SizeOfHeaders) und korrigiert ggf. die Zertifikatsgröße (Schreibzugriff).
- CreateFiber liest [0x10130] (PeStackCommit = 0x10000) als Default-Stackgröße.

### 8.4 Fibers im Spielcode

- FUN_0013f250 ruft `XAPILIB::CreateFiber` auf (Callgraph).
- CreateFiber nutzt MmCreateKernelStack. Die Startroutine ist 0x17086B: Fiber-Routine, danach KeBugCheck(0).
- SwitchToFiber (0x17094F) tauscht esp und fs:[0] und verwendet fs:[4]/TLS.
- Empfehlung: die Fiber-API (CreateFiber, DeleteFiber, SwitchToFiber, ConvertThreadToFiber) per HLE auf Win32-Fibers legen.

### 8.5 Asynchrones Datei-I/O mit Completion-Routinen

- Das Spiel ruft `ReadFileEx` (FUN_0013f660) und `WriteFileEx` (FUN_000e0b00) auf.
- XAPI übergibt dann `NtUserIoApcDispatcher` als ApcRoutine und die Completion-Routine als ApcContext an NtReadFile/NtWriteFile (Push-Quellen bei 0x0F7630).
- Der Host muss die Completion-Routine als User-APC im aufrufenden Thread zustellen. Die Zustellung erfolgt bei alertable SleepEx/WaitForSingleObjectEx; dazu KeDelayExecutionThread bzw. NtWaitForSingleObjectEx mit Alertable-Flag richtig umsetzen.

### 8.6 AutoPowerDown

- FUN_000f9b1a (aus XapiInitProcess) initialisiert den Timer 0x310360 und die DPC 0x310340 mit der Routine 0xF9B08.
- XAutoPowerDownResetTimer setzt den Timer neu (0x0F9B01).
- Feuert er und ist [0x310388] ≠ 0, ruft die DPC HalInitiateShutdown auf.
- Review-Ergänzung: [0x310388] = Bit 0 von XQueryValue(0x11) (FUN_000f9b1a, 0x0F9B65–0x0F9B76; der Block wird übersprungen, wenn XboxHardwareInfo.Flags Bit 3 gesetzt ist). DueTime bei 0x0F9AED–0x0F9AFB ist 0xFFFFFFCD_B5659000 = −216 000 000 000 × 100 ns, also 6 Stunden relativ. Liefert der Host für Setting 0x11 Bit 0 = 0, ist AutoPowerDown ohnehin aus.
- Eine treue Timer/DPC-Emulation würde das Spiel also beenden können. HalInitiateShutdown sollte deshalb nur protokolliert werden.

### 8.7 Shutdown-Routine und IDE-Kanal

- XapiInitProcess registriert per HalRegisterShutdownNotification die Struktur 0x2D7530. Deren erstes DWORD ist der Zeiger auf sub_0f9c7f (Datenreferenz 0x2D7530 → 0xF9C7F).
- Diese Routine wartet über IdexChannelObject/KeWaitForSingleObject auf einen ruhigen IDE-Kanal.
- Wird HalRegisterShutdownNotification als no-op implementiert, läuft sie nie.

### 8.8 Zeitbasis: rdtsc mit fest verdrahteten 733 MHz

- `XAPILIB::QueryPerformanceCounter` (0xF641B) ist `rdtsc`.
- `XAPILIB::QueryPerformanceFrequency` (0xF642C) schreibt die Konstante 0x2BB5C755 (= 733 333 333, Xbox-CPU-Takt). Die Konstante kommt im ganzen Image nur bei 0xF6436 vor (`kernel_rdtsc_scan.py`).
- Weitere `rdtsc` gibt es nur in D3D (FUN_0023b780). Review-Korrektur: Der frühere Treffer „XMV (sub_2742c0)“ ist kein Code. 0x2742C0 ist eine Datentabelle, die FUN_00266b18 bei 0x266F8F per `mov dword ptr [ebp-0x4c],0x2742c0` als Zeiger lädt. Die Bytes `0f 31` bei 0x274509 liegen mitten in dieser Tabelle (`verify_kernel_fs_rdtsc.py`, `verify_kernel_xrefs.py`).
- Auf dem PC stimmt das Verhältnis nicht. Beide XAPI-Funktionen per HLE auf Win32 QueryPerformanceCounter/QueryPerformanceFrequency umleiten (trivial, aber zwingend). Das hat mit der Thunk-Tabelle nichts zu tun.

## 9. Volltabelle

Spalten:
- Slot = Index in der Thunk-Tabelle; Thunk = Slot-Adresse; Ord = Ordinal (dezimal, hex);
- Art: FUNC (aufgerufen) / DATA (Datenexport: dereferenziert, indiziert oder dessen Adresse weitergegeben) / ADDR (Funktionsexport, dessen Adresse nur weitergegeben wird). Review-Korrektur: Vorher standen ExEventObjectType, XboxHDKey, XboxSignatureKey, XboxAlternateSignatureKeys und XePublicKeyData als ADDR in Tabelle und TSV, im Widerspruch zu „11 Datenexporte“ in Abschnitt 1. `kernel_report.py` ist angepasst, TSV und Tabelle sind neu erzeugt;
- Konv. und Args aus dem Binary (stdcall: Purge/4; fastcall: Register);
- Sites = Aufrufstellen inkl. Stub-Calls und Tail-Jumps;
- Aufrufer = Modul(Anzahl Funktionen);
- „nur ersetzte Libs“ = alle Aufrufer in D3D/DSOUND/XMV/XPP.

Vollständige Aufruferlisten, Purge-Evidenz, Ghidra-Abgleich und eine Beleg-Instruktion je Import stehen in `kernel_imports.tsv`. Die Tabelle unten ist aus `scripts\kernel_tables.md` eingefügt (`kernel_report.py`).

| Slot | Thunk | Ord | Name | Art | Konv. | Args | Sites | Aufrufer (Modul: Anzahl Fkt.) | nur ersetzte Libs | Host-Plan | Aufwand |
|---:|---|---:|---|---|---|---|---:|---|---|---|---|
| 0 | 0x293560 | 161 (0x0a1) | KfLowerIrql | FUNC | fastcall | 1 (ecx) | 28 | DSOUND(1), XAPI(1), XPP(22) | nein | fastcall cl=NewIrql. IRQL-Emulation: thread-lokale IRQL; beim Absenken unter DISPATCH_LEVEL globalen Dispatcher-Lock freigeben und anstehende DPCs ausfuehren. XAPI-Nutzer: Shutdown-Routine sub_0f9c7f; Rest DSOUND/XPP | mittel |
| 1 | 0x293564 | 129 (0x081) | KeRaiseIrqlToDpcLevel | FUNC | stdcall | 0 | 24 | XAPI(1), XPP(21) | nein | stdcall ohne Args, alte IRQL in al (22/24 Sites lesen al). Globalen rekursiven Dispatcher-Lock nehmen, IRQL=2 merken | mittel |
| 2 | 0x293568 | 65 (0x041) | IoCreateDevice | FUNC | stdcall | 6 | 1 | XPP(1) | ja | Nur XPP MU_Init (Memory-Unit-Treiber). Bei XInput/XPP-HLE Stub (STATUS_SUCCESS, Dummy-DEVICE_OBJECT) | trivial |
| 3 | 0x29356c | 289 (0x121) | RtlInitAnsiString | FUNC | stdcall | 2 | 22 | XAPI(17), XPP(1) | nein | ANSI_STRING initialisieren (ntdll-identisch) | trivial |
| 4 | 0x293570 | 145 (0x091) | KeSetEvent | FUNC | stdcall | 3 | 7 | D3D(2), XAPI(1), XPP(4) | nein | KEVENT liegt im Gastspeicher. SignalState setzen, Host-Event (Map) signalisieren, alten Zustand zurueck. XAPI sub_0f9c63 (IDE-Shutdown-Callback), sonst D3D/XPP | mittel |
| 5 | 0x293574 | 159 (0x09f) | KeWaitForSingleObject | FUNC | stdcall | 5 | 7 | D3D(2), XAPI(1), XPP(3) | nein | Generisches Warten auf Gast-Dispatcher-Objekte (KEVENT/KTIMER), Timeout PLARGE_INTEGER (negativ = relativ, 100 ns). Host: Objekt->Host-Handle, WaitForSingleObject. XAPI sub_0f9c7f, D3D BlockUntilVerticalBlank/BlockOnTime, XPP | mittel |
| 6 | 0x293578 | 113 (0x071) | KeInitializeTimerEx | FUNC | stdcall | 2 | 6 | DSOUND(1), XAPI(1), XPP(4) | nein | KTIMER im Gastspeicher initialisieren (Header, SignalState=0) und Host-Timer anlegen (Map Gastadresse->Host-Objekt). XAPI: AutoPowerDown-Timer 0x310360 | mittel |
| 7 | 0x29357c | 107 (0x06b) | KeInitializeDpc | FUNC | stdcall | 3 | 10 | D3D(1), DSOUND(2), XAPI(1), XPP(5) | nein | KDPC im Gastspeicher fuellen (Routine, Context); DPC-Queue im Host, Abarbeitung in DPC-Thread unter Dispatcher-Lock. XAPI FUN_000f9b1a: DPC 0xf9b08 (AutoPowerDown -> HalInitiateShutdown) | mittel |
| 8 | 0x293580 | 74 (0x04a) | IoInvalidDeviceRequest | ADDR |  |  | 0 | XPP(1) | ja | Nur als Funktionsadresse: XPP FUN_0028d254 kopiert sie in 10 Dispatch-Slots ab 0x28c004. Adresse einer Dummy-Routine (STATUS_INVALID_DEVICE_REQUEST) | trivial |
| 9 | 0x293584 | 294 (0x126) | RtlLeaveCriticalSection | FUNC | stdcall | 1 | 108 | DSOUND(51), LIBC(5), XAPI(7) | nein | Gegenstueck zu RtlEnterCriticalSection (108 Sites, davon 96 DSOUND) | mittel |
| 10 | 0x293588 | 277 (0x115) | RtlEnterCriticalSection | FUNC | stdcall | 1 | 12 | DSOUND(1), LIBC(6), XAPI(5) | nein | Xbox-RTL_CRITICAL_SECTION im Gastspeicher (eingebettetes KEVENT, Layout != Win32). Eigene Implementierung auf den Gastfeldern oder Map Gast-CS -> Host-CRITICAL_SECTION. XAPI-Heap, LIBC-Locks, DSOUND | mittel |
| 11 | 0x29358c | 199 (0x0c7) | NtFreeVirtualMemory | FUNC | stdcall | 3 | 6 | XAPI(6) | nein | 3 Args: BaseAddress*, RegionSize*, FreeType (0x8000/0x4000). VirtualFree | trivial |
| 12 | 0x293590 | 184 (0x0b8) | NtAllocateVirtualMemory | FUNC | stdcall | 5 | 11 | XAPI(6) | nein | 5 Args (kein ProcessHandle): BaseAddress*, ZeroBits, RegionSize*, AllocationType, Protect (=4 an allen 11 Sites). VirtualAlloc, In/Out-Parameter zurueckschreiben; Xbox-only-Flags maskieren. Nutzer: XAPI-Heap | trivial |
| 13 | 0x293594 | 269 (0x10d) | RtlCompareMemoryUlong | FUNC | stdcall | 3 | 7 | XAPI(3) | nein | Selbst implementieren / ntdll | trivial |
| 14 | 0x293598 | 291 (0x123) | RtlInitializeCriticalSection | FUNC | stdcall | 1 | 2 | LIBC(1), XAPI(1) | nein | Gast-Struktur initialisieren / Host-Objekt anlegen (XAPI-Heap, LIBC) | mittel |
| 15 | 0x29359c | 217 (0x0d9) | NtQueryVirtualMemory | FUNC | stdcall | 2 | 2 | XAPI(1) | nein | 2 Args: BaseAddress, MEMORY_BASIC_INFORMATION*. VirtualQuery | trivial |
| 16 | 0x2935a0 | 302 (0x12e) | RtlRaiseException | FUNC | stdcall | 1 | 3 | XAPI(3) | nein | EXCEPTION_RECORD* -> ntdll!RtlRaiseException; setzt voraus, dass fs:[0] die Windows-SEH-Kette ist (Spielcode nutzt fs:[0] 5054x im MSVC-Format) | mittel |
| 17 | 0x2935a4 | 24 (0x018) | ExQueryNonVolatileSetting | FUNC | stdcall | 5 | 5 | XAPI(5) | nein | 5 Args; Indizes aus Binary: 7 (XGetLanguage), 0x104 (XGetGameRegion), 9 (XGetAudioFlags), 0xA (XGetParentalControlSetting), XQueryValue reicht durch; Review: ueber XQueryValue zusaetzlich 0x11 (FUN_000f9b1a, Bit 0 = AutoPowerDown an) und 0xFFFF (FUN_000f6ffa, ganzes EEPROM 0x100 Byte, nur wenn XboxHDKey komplett 0 ist). Konfigurationstabelle (Sprache, Region EU/PAL, Audio, Kindersicherung); 0xA muss 0 liefern (oder Fehler), sonst XapiBootToDash in XapiInitProcess (cert GameRatings = 0, Vergleich 0x0F93E8). Bedeutung laut XDK (Vermutung) | trivial |
| 18 | 0x2935a8 | 16 (0x010) | ExEventObjectType | DATA |  |  | 0 | XPP(1) | ja | POBJECT_TYPE; nur XPP: push [slot] als ObjectType fuer ObReferenceObjectByHandle. Adresse eines Dummy-Objekts eintragen | trivial |
| 19 | 0x2935ac | 225 (0x0e1) | NtSetEvent | FUNC | stdcall | 2 | 3 | DSOUND(3) | ja | Nur DSOUND (Notify-Events). Bei DSOUND-HLE entbehrlich, sonst SetEvent auf uebersetztem Handle | trivial |
| 20 | 0x2935b0 | 234 (0x0ea) | NtWaitForSingleObjectEx | FUNC | stdcall | 4 | 1 | XAPI(1) | nein | XAPI WaitForSingleObjectEx (Handle, WaitMode=1, Alertable, Timeout*) -> WaitForSingleObjectEx mit Handle-Uebersetzung, STATUS_* <-> WAIT_* abbilden | mittel |
| 21 | 0x2935b4 | 99 (0x063) | KeDelayExecutionThread | FUNC | stdcall | 3 | 3 | XAPI(1), XPP(2) | nein | XAPI SleepEx: (WaitMode=1, Alertable, Interval*). SleepEx mit Umrechnung 100 ns -> ms (alertable fuer APCs von ReadFileEx/WriteFileEx); STATUS_USER_APC bei APC | trivial |
| 22 | 0x2935b8 | 226 (0x0e2) | NtSetInformationFile | FUNC | stdcall | 5 | 16 | D3D(1), XAPI(9) | nein | Klassen 4 (Basic), 0xD (Disposition), 0xE (Position), 0x13 (Allocation), 0x14 (EndOfFile) -> NT-identisch, ntdll weiterreichen | mittel |
| 23 | 0x2935bc | 128 (0x080) | KeQuerySystemTime | FUNC | stdcall | 1 | 9 | DSOUND(4), XAPI(3), XPP(2) | nein | GetSystemTimeAsFileTime (gleiche Epoche/Einheit) | trivial |
| 24 | 0x2935c0 | 301 (0x12d) | RtlNtStatusToDosError | FUNC | stdcall | 1 | 5 | XAPI(5) | nein | ntdll!RtlNtStatusToDosError | trivial |
| 25 | 0x2935c4 | 190 (0x0be) | NtCreateFile | FUNC | stdcall | 9 | 10 | D3D(1), XAPI(6) | nein | 9 Args (ohne EaBuffer/EaLength), OBJECT_ATTRIBUTES mit ANSI_STRING. Pfaduebersetzung (\Device\CdRom0, \Device\Harddisk0\PartitionN, \??\D: usw. ueber eigene Symlink-Tabelle) -> CreateFileW/ntdll; Xbox-Pfade case-insensitive. XAPI + D3D-Moviecache | schwer |
| 26 | 0x2935c8 | 187 (0x0bb) | NtClose | FUNC | stdcall | 1 | 29 | D3D(1), XAPI(18) | nein | CloseHandle ueber Gast-Handle-Tabelle (Dateien, Threads, Symlinks) | trivial |
| 27 | 0x2935cc | 336 (0x150) | XcSHAUpdate | FUNC | stdcall | 3 | 6 | XAPI(5) | nein | SHA-1 update (ctx, data, len) | trivial |
| 28 | 0x2935d0 | 335 (0x14f) | XcSHAInit | FUNC | stdcall | 1 | 3 | XAPI(3) | nein | SHA-1 init auf Gast-Kontextpuffer (eigene Implementierung). Nutzer: XAPI-Signaturcode (XCalculateSignature*, vom Spiel FUN_000e0340/FUN_000e03b0 genutzt) und FUN_000f902f | trivial |
| 29 | 0x2935d4 | 337 (0x151) | XcSHAFinal | FUNC | stdcall | 2 | 3 | XAPI(2) | nein | SHA-1 final (ctx, digest[20]) | trivial |
| 30 | 0x2935d8 | 323 (0x143) | XboxHDKey | DATA |  |  | 0 | XAPI(1) | nein | 16 Byte, XAPI FUN_000f6ffa (Signatur-Schluesselableitung). Konstanter Dummy-Schluessel, muss != 0 sein (Review: bei 16 Nullbytes liest FUN_000f6ffa das EEPROM per XQueryValue(0xFFFF)); eigene Saves konsistent, Original-Saves braeuchten echte Schluessel | trivial |
| 31 | 0x2935dc | 325 (0x145) | XboxSignatureKey | DATA |  |  | 0 | XAPI(1) | nein | 16 Byte, Adresse in XCalculateSignatureBeginEx gespeichert. Dummy-Schluessel | trivial |
| 32 | 0x2935e0 | 354 (0x162) | XboxAlternateSignatureKeys | DATA |  |  | 0 | XAPI(1) | nein | 16 x 16 Byte, add eax,[slot] -> &Keys[i] (XCalculateSignatureBeginEx). Dummy-Tabelle | trivial |
| 33 | 0x2935e4 | 233 (0x0e9) | NtWaitForSingleObject | FUNC | stdcall | 3 | 2 | XAPI(2) | nein | XAPI ReadFile/WriteFile warten bei STATUS_PENDING auf das Dateihandle (Alertable=0, Timeout=NULL) -> WaitForSingleObject | trivial |
| 34 | 0x2935e8 | 219 (0x0db) | NtReadFile | FUNC | stdcall | 8 | 6 | D3D(1), XAPI(4) | nein | 8 Args (Review: NT hat 9, Xbox ohne Key; 8 Pushes z.B. ReadFileEx 0x0F7630). Handle-Uebersetzung -> ntdll/ReadFile; ist ApcRoutine == NtUserIoApcDispatcher-Marker (ReadFileEx), Completion-Routine (ApcContext) per QueueUserAPC im Aufrufer-Thread zustellen | mittel |
| 35 | 0x2935ec | 236 (0x0ec) | NtWriteFile | FUNC | stdcall | 8 | 12 | D3D(1), XAPI(7) | nein | 8 Args (NT: 9, ohne Key); wie NtReadFile (WriteFileEx-APC) | mittel |
| 36 | 0x2935f0 | 211 (0x0d3) | NtQueryInformationFile | FUNC | stdcall | 5 | 9 | XAPI(7) | nein | Klassen 0x22 (NetworkOpen, 0x38) und 0xE (Position, 8) -> NT-identisch, ntdll weiterreichen | mittel |
| 37 | 0x2935f4 | 218 (0x0da) | NtQueryVolumeInformationFile | FUNC | stdcall | 5 | 4 | XAPI(4) | nein | Klasse 3 (FileFsSizeInformation, 0x18) an allen 4 Sites -> GetDiskFreeSpaceEx, Werte ggf. faken | trivial |
| 38 | 0x2935f8 | 198 (0x0c6) | NtFlushBuffersFile | FUNC | stdcall | 2 | 1 | XAPI(1) | nein | XAPI FlushFileBuffers -> FlushFileBuffers | trivial |
| 39 | 0x2935fc | 232 (0x0e8) | NtUserIoApcDispatcher | ADDR |  |  | 0 | XAPI(2) | nein | Nur Adresse: ReadFileEx/WriteFileEx pushen [slot] als ApcRoutine. Spiel nutzt beide (FUN_0013f660, FUN_000e0b00). Marker-Adresse eintragen, die eigenes NtReadFile/NtWriteFile erkennt | mittel |
| 40 | 0x293600 | 200 (0x0c8) | NtFsControlFile | FUNC | stdcall | 10 | 1 | XAPI(1) | nein | Nur XAPI FUN_000f946d (Utility-Partition formatieren): FSCTL 0x90020 (Vermutung: DISMOUNT_VOLUME). STATUS_SUCCESS | trivial |
| 41 | 0x293604 | 196 (0x0c4) | NtDeviceIoControlFile | FUNC | stdcall | 10 | 4 | XAPI(2) | nein | XAPI FUN_000f90e6: IOCTL 0x4D014 (Vermutung: SCSI_PASS_THROUGH_DIRECT an DVD), FUN_000f946d: 0x70000/0x74004 (Vermutung: Geometry/PartitionInfo). Fake-Antworten oder Fehlerstatus (Ergebnis von FUN_000f90e6 wird in XapiInitProcess ignoriert, 0x0F931E) | mittel |
| 42 | 0x293608 | 172 (0x0ac) | MmFreeSystemMemory | FUNC | stdcall | 2 | 3 | XAPI(1) | nein | 2 Args (Base, Size). VirtualFree | trivial |
| 43 | 0x29360c | 202 (0x0ca) | NtOpenFile | FUNC | stdcall | 6 | 13 | XAPI(13) | nein | 6 Args wie NT; gleiche Pfaduebersetzung wie NtCreateFile | mittel |
| 44 | 0x293610 | 167 (0x0a7) | MmAllocateSystemMemory | FUNC | stdcall | 2 | 1 | XAPI(1) | nein | 2 Args (Size, Protect=4). XAPI FUN_000f786b (Kopierroutine). VirtualAlloc | trivial |
| 45 | 0x293614 | 210 (0x0d2) | NtQueryFullAttributesFile | FUNC | stdcall | 2 | 1 | XAPI(1) | nein | 2 Args -> GetFileAttributesEx/ntdll (FILE_NETWORK_OPEN_INFORMATION) mit Pfaduebersetzung | trivial |
| 46 | 0x293618 | 207 (0x0cf) | NtQueryDirectoryFile | FUNC | stdcall | 10 | 3 | XAPI(3) | nein | 10 Args, Klasse 1 (FileDirectoryInformation), FileMask als ANSI_STRING. FindFirstFile/NextFile -> Xbox-Struktur mit ANSI-Namen (XFindFirstSaveGame u.a.) | mittel |
| 47 | 0x29361c | 40 (0x028) | HalDiskCachePartitionCount | DATA |  |  | 0 | XAPI(1) | nein | DWORD, gelesen in XapiSelectCachePartition. Anzahl Cache-Partitionen bereitstellen (Vermutung: 3), Z: auf Host-Verzeichnis | trivial |
| 48 | 0x293620 | 67 (0x043) | IoCreateSymbolicLink | FUNC | stdcall | 2 | 3 | XAPI(3) | nein | XAPI (XapiInitProcess, XMountUtilityDrive, XapiMapLetterToDirectory): Laufwerksbuchstabe -> Geraetepfad in eigene Symlink-Tabelle | trivial |
| 49 | 0x293624 | 215 (0x0d7) | NtQuerySymbolicLinkObject | FUNC | stdcall | 3 | 1 | XAPI(1) | nein | Nur XLaunchNewImageA (Ziel von D:). Aus Symlink-Tabelle beantworten | trivial |
| 50 | 0x293628 | 203 (0x0cb) | NtOpenSymbolicLinkObject | FUNC | stdcall | 2 | 1 | XAPI(1) | nein | Nur XLaunchNewImageA. Pseudo-Handle auf Eintrag der Symlink-Tabelle | trivial |
| 51 | 0x29362c | 178 (0x0b2) | MmPersistContiguousMemory | FUNC | stdcall | 3 | 2 | D3D(1), XAPI(1) | nein | No-op (nur fuer Reboot/Imagewechsel). XAPI FUN_000f81bb, D3D | trivial |
| 52 | 0x293630 | 165 (0x0a5) | MmAllocateContiguousMemory | FUNC | stdcall | 1 | 2 | XAPI(1), XPP(1) | nein | 1 Arg -> VirtualAlloc (RW, Seitengranular), Groesse merken. XAPI FUN_000f81bb (LaunchDataPage), XPP | trivial |
| 53 | 0x293634 | 164 (0x0a4) | LaunchDataPage | DATA |  |  | 0 | XAPI(2) | nein | Zeigervariable (PLAUNCH_DATA_PAGE*): XAPI liest/schreibt *slot (FUN_000f81bb setzt neue Seite, XapiInitProcess prueft und nullt). Beschreibbares DWORD = NULL | trivial |
| 54 | 0x293638 | 171 (0x0ab) | MmFreeContiguousMemory | FUNC | stdcall | 1 | 17 | D3D(6), DSOUND(1), GAME(2), XAPI(2) | nein | 1 Arg -> VirtualFree. Auch Spielcode: ueber XAPILIB::XPhysicalFree (6-Byte-Stub 0xf8463, FUN_000ee080) und D3D-Stub 0x2417b0 (sub_161180, vtable 0x2aaf08) | trivial |
| 55 | 0x29363c | 49 (0x031) | HalReturnToFirmware | FUNC | stdcall | 1 | 2 | XAPI(2) | nein | XapiBootToDash (Arg 4), XLaunchNewImage-Pfad FUN_000f829c (Arg 2): Prozess beenden bzw. Spiel neu starten. Review: Arg 4 nur, wenn die eigene TitleID 0xFFFE0000 (Dashboard) ist (0x0F8FD8); fuer Futurama (TitleID 0x46490002) laeuft XapiBootToDash ueber XLaunchNewImageA -> FUN_000f829c -> Arg 2. Das Spiel ruft XLaunchNewImageA selbst auf (FUN_000e4b00) | trivial |
| 56 | 0x293640 | 166 (0x0a6) | MmAllocateContiguousMemoryEx | FUNC | stdcall | 5 | 12 | D3D(8), DSOUND(1), XAPI(1) | nein | 5 Args (Size, Lowest, Highest, Alignment, Protect). D3D: Highest 0x7FFFFFF, Alignment bis 0x4000, Protect 0x404 = RW+WRITECOMBINE (eine D3D-Site 0x239D37: Protect 4); XPhysicalAlloc 0xF8449 reicht Aufruferwerte durch (alle Aufrufer: PhysicalAddress -1 -> Lowest 0/Highest -1; Spiel FUN_000ee220 Protect 4). VirtualAlloc (64-KiB-Granularitaet deckt Alignment), WRITECOMBINE ignorieren. Spiel nutzt es ueber XAPILIB::XPhysicalAlloc | trivial |
| 57 | 0x293644 | 180 (0x0b4) | MmQueryAllocationSize | FUNC | stdcall | 1 | 3 | D3D(1), DSOUND(2) | ja | Groesse aus eigener Allokationstabelle (Fallback VirtualQuery). Nur D3D/DSOUND | trivial |
| 58 | 0x293648 | 182 (0x0b6) | MmSetAddressProtect | FUNC | stdcall | 3 | 4 | XAPI(1), XPP(1) | nein | 3 Args -> VirtualProtect. XAPI FUN_000f9804 nur bei bestimmten Kernel-Builds, XPP FUN_0028eaa7 (Protect 0x20) | trivial |
| 59 | 0x29364c | 179 (0x0b3) | MmQueryAddressProtect | FUNC | stdcall | 1 | 2 | XAPI(1), XPP(1) | nein | VirtualQuery().Protect. XAPI FUN_000f9804 (Kernel-Patch, siehe XboxKrnlVersion), XPP | trivial |
| 60 | 0x293650 | 149 (0x095) | KeSetTimer | FUNC | stdcall | 4 | 17 | DSOUND(3), XAPI(1), XPP(13) | nein | Stack: Timer, DueTime (LARGE_INTEGER = 2 DWORDs), Dpc. Host-Timer-Queue; bei Ablauf SignalState=1 und DPC einreihen; Rueckgabe: war schon aktiv. XAPI XAutoPowerDownResetTimer | mittel |
| 61 | 0x293654 | 97 (0x061) | KeCancelTimer | FUNC | stdcall | 1 | 22 | DSOUND(2), XPP(20) | ja | Host-Timer stoppen, Rueckgabe ob aktiv war. Nur DSOUND/XPP | mittel |
| 62 | 0x293658 | 250 (0x0fa) | ObfDereferenceObject | FUNC | fastcall | 1 (ecx) | 1 | XPP(1) | ja | fastcall ecx=Object, einzige Verwendung Tail-Jump in XPP FUN_0028d656. Stub no-op | trivial |
| 63 | 0x29365c | 246 (0x0f6) | ObReferenceObjectByHandle | FUNC | stdcall | 3 | 1 | XPP(1) | ja | Nur XPP FUN_0028d9bf (Event-Handle -> KEVENT*). Bei XPP-HLE Stub | trivial |
| 64 | 0x293660 | 322 (0x142) | XboxHardwareInfo | DATA |  |  | 0 | D3D(1), XAPI(3), XPP(3) | nein | Struktur: Byte +0 (Bits 0x1/0x2/0x8 getestet), +1 Bit 0x2 (D3D), Byte +5 == 0xA1 (XPP); FUN_000f9804 liest DWORD +0. Retail-Werte bereitstellen (Flags=0, Vermutung) | trivial |
| 65 | 0x293664 | 83 (0x053) | IoStartPacket | FUNC | stdcall | 3 | 1 | XPP(1) | ja | Nur XPP (MU-Treiber). Stub | trivial |
| 66 | 0x293668 | 87 (0x057) | IofCompleteRequest | FUNC | fastcall | 2 (ecx,edx) | 9 | XPP(9) | ja | fastcall ecx=Irp, dl=PriorityBoost; nur XPP. Stub | trivial |
| 67 | 0x29366c | 81 (0x051) | IoStartNextPacket | FUNC | stdcall | 1 | 8 | XPP(8) | ja | Nur XPP. Stub | trivial |
| 68 | 0x293670 | 17 (0x011) | ExFreePool | FUNC | stdcall | 1 | 2 | DSOUND(1), XPP(1) | ja | Nur DSOUND/XPP. HeapFree | trivial |
| 69 | 0x293674 | 15 (0x00f) | ExAllocatePoolWithTag | FUNC | stdcall | 2 | 3 | DSOUND(1), XPP(2) | ja | Nur DSOUND-Pool/XPP. HeapAlloc | trivial |
| 70 | 0x293678 | 359 (0x167) | IoMarkIrpMustComplete | FUNC | stdcall | 1 | 1 | XPP(1) | ja | Nur XPP. Stub | trivial |
| 71 | 0x29367c | 358 (0x166) | HalIsResetOrShutdownPending | FUNC | stdcall | 0 | 1 | XPP(1) | ja | Nur XPP. FALSE | trivial |
| 72 | 0x293680 | 344 (0x158) | XcVerifyPKCS1Signature | FUNC | stdcall | 3 | 1 | XAPI(1) | nein | Nur XAPI FUN_000f902f (mit XePublicKeyData), aufgerufen aus FUN_000f90e6 (DVD-Pruefung); dessen Ergebnis ignoriert XapiInitProcess (0x0F931C test eax,eax; 0x0F931E jmp). TRUE zurueckgeben | trivial |
| 73 | 0x293684 | 355 (0x163) | XePublicKeyData | DATA |  |  | 0 | XAPI(1) | nein | RSA-Public-Key, nur als Argument an XcVerifyPKCS1Signature (FUN_000f902f). Dummy-Puffer | trivial |
| 74 | 0x293688 | 47 (0x02f) | HalRegisterShutdownNotification | FUNC | stdcall | 2 | 8 | D3D(2), DSOUND(4), XAPI(1), XPP(1) | nein | No-op oder Liste fuehren. XAPI registriert 0x2d7530 (Routine sub_0f9c7f), sonst D3D/DSOUND/XPP | trivial |
| 75 | 0x29368c | 324 (0x144) | XboxKrnlVersion | DATA |  |  | 0 | XAPI(1) | nein | {USHORT Major, Minor, Build, Qfe}: FUN_000f9804 liest Build (+4) und patcht bei 0xF68/0xFC2/0xFC7/0x12D1-0x12D2 Kernelcode bei 0x800299A0..0x800398CD (Review: Adresse haengt von Build und XboxHardwareInfo.Flags Bit 1 ab; 0xFC2 mit Bit 1 gesetzt -> kein Patch). Build ausserhalb dieser Werte waehlen -> kein Patch | trivial |
| 76 | 0x293690 | 258 (0x102) | PsTerminateSystemThread | FUNC | stdcall | 1 | 1 | XAPI(1) | nein | 1 Arg, Ende von XapiThreadStartup (sub_0f99da), danach int3. ExitThread + emulierte Thread-Strukturen freigeben | trivial |
| 77 | 0x293694 | 255 (0x0ff) | PsCreateSystemThreadEx | FUNC | stdcall | 10 | 1 | XAPI(1) | nein | 10 Args, einzige Site XAPI CreateThread (StartRoutine 0xf99da = XapiThreadStartup). CreateThread mit Trampolin: pro Thread KPCR/KTHREAD-Ersatz und TLS (0x20 Byte Zero-Fill laut TLS-Directory) bereitstellen, fs:-Zugriffe bedienen, Handle/ThreadId liefern | schwer |
| 78 | 0x293698 | 360 (0x168) | HalInitiateShutdown | FUNC | stdcall | 0 | 1 | XAPI(1) | nein | 0 Args, nur aus DPC sub_0f9b08 (AutoPowerDown, wenn Flag [0x310388] != 0; Flag = Setting 0x11 Bit 0, Timer 6 h relativ). Loggen und ignorieren | trivial |
| 79 | 0x29369c | 44 (0x02c) | HalGetInterruptVector | FUNC | stdcall | 2 | 4 | D3D(1), DSOUND(2), XPP(1) | ja | Nur D3D/DSOUND/XPP (Level 3/5/1/6 an den Sites). Stub: Vektor 0, *Irql=0 | trivial |
| 80 | 0x2936a0 | 160 (0x0a0) | KfRaiseIrql | FUNC | fastcall | 1 (ecx) | 2 | DSOUND(1), XPP(1) | ja | fastcall cl=NewIrql, Rueckgabe alte IRQL in al; wie KeRaiseIrqlToDpcLevel. Nur DSOUND/XPP -> bei HLE Stub | trivial |
| 81 | 0x2936a4 | 98 (0x062) | KeConnectInterrupt | FUNC | stdcall | 1 | 4 | D3D(1), DSOUND(2), XPP(1) | ja | Nur D3D/DSOUND/XPP. Stub: TRUE zurueckgeben, Interrupt feuert nie | trivial |
| 82 | 0x2936a8 | 109 (0x06d) | KeInitializeInterrupt | FUNC | stdcall | 7 | 4 | D3D(1), DSOUND(2), XPP(1) | ja | Nur D3D/DSOUND/XPP (GPU-, APU-, USB-Interrupt). Stub: KINTERRUPT nur nullen | trivial |
| 83 | 0x2936ac | 151 (0x097) | KeStallExecutionProcessor | FUNC | stdcall | 1 | 7 | DSOUND(5), XPP(1) | ja | Mikrosekunden-Busy-Wait (QueryPerformanceCounter). Nur DSOUND/XPP | trivial |
| 84 | 0x2936b0 | 279 (0x117) | RtlEqualString | FUNC | stdcall | 3 | 1 | XAPI(1) | nein | ntdll!RtlEqualString (ANSI_STRING, CaseInSensitive) | trivial |
| 85 | 0x2936b4 | 327 (0x147) | XeLoadSection | FUNC | stdcall | 1 | 1 | XAPI(1) | nein | Arg = Zeiger auf XBE-Section-Header (XLoadSectionByHandle). Alle Sektionen ausser $$XTIMAGE/.XTLID sind PRELOAD. Review: XapiInitProcess -> FUN_000f8f75 -> XapiMapLetterToDirectory laedt $$XTIMAGE (String 0x2a5eb8, XLoadSectionByHandle 0x0F8E74) und schreibt TitleImage.xbx -> Rohdaten von $$XTIMAGE muessen an VA 0x3178C0 liegen (Loader mappt sie vorab oder XeLoadSection kopiert sie); dann Referenzzaehler erhoehen, STATUS_SUCCESS | trivial |
| 86 | 0x2936b8 | 328 (0x148) | XeUnloadSection | FUNC | stdcall | 1 | 1 | XAPI(1) | nein | Referenzzaehler senken, STATUS_SUCCESS | trivial |
| 87 | 0x2936bc | 357 (0x165) | IdexChannelObject | DATA |  |  | 0 | XAPI(2) | nein | Struktur: XAPI schreibt +0x10/+0x14 (Callbacks 0xf9c7c/0xf9c63), liest/nullt +0x20. Beschreibbarer Nullpuffer (>= 0x24 Byte) -> Shutdown-Routine wartet nicht | trivial |
| 88 | 0x2936c0 | 173 (0x0ad) | MmGetPhysicalAddress | FUNC | stdcall | 1 | 10 | DSOUND(5), XPP(3) | ja | Nur DSOUND/XPP (DMA). Stub: Identitaet | trivial |
| 89 | 0x2936c4 | 175 (0x0af) | MmLockUnlockBufferPages | FUNC | stdcall | 3 | 11 | DSOUND(6), XPP(4) | ja | Nur DSOUND/XPP. No-op | trivial |
| 90 | 0x2936c8 | 119 (0x077) | KeInsertQueueDpc | FUNC | stdcall | 3 | 5 | D3D(1), DSOUND(3), XPP(1) | ja | In Host-DPC-Queue einreihen (SystemArgument1/2 merken), TRUE wenn neu. Nur D3D/DSOUND/XPP (Interrupt-DPCs) -> bei HLE Stub | mittel |
| 91 | 0x2936cc | 176 (0x0b0) | MmLockUnlockPhysicalPage | FUNC | stdcall | 2 | 4 | XPP(2) | ja | Nur XPP. No-op | trivial |
| 92 | 0x2936d0 | 1 (0x001) | AvGetSavedDataAddress | FUNC | stdcall | 0 | 3 | D3D(3) | ja | Nur D3D. NULL | trivial |
| 93 | 0x2936d4 | 2 (0x002) | AvSendTVEncoderOption | FUNC | stdcall | 4 | 8 | D3D(6) | ja | Nur D3D (Optionen 6, 9, 0xB, 0xE, 0xF an den Sites). No-op, *Result=0 | trivial |
| 94 | 0x2936d8 | 3 (0x003) | AvSetDisplayMode | FUNC | stdcall | 6 | 1 | D3D(1) | ja | Nur D3D. No-op (Videomodus macht der Host-Renderer) | trivial |
| 95 | 0x2936dc | 4 (0x004) | AvSetSavedDataAddress | FUNC | stdcall | 1 | 1 | D3D(1) | ja | Nur D3D. No-op | trivial |
| 96 | 0x2936e0 | 46 (0x02e) | HalReadWritePCISpace | FUNC | stdcall | 6 | 2 | D3D(1) | ja | Nur D3D CMiniport_InitHardware. Stub (Puffer nullen) | trivial |
| 97 | 0x2936e4 | 100 (0x064) | KeDisconnectInterrupt | FUNC | stdcall | 1 | 3 | D3D(1), DSOUND(2) | ja | Nur D3D/DSOUND (Review: keine XPP-Site). Stub: TRUE | trivial |
| 98 | 0x2936e8 | 8 (0x008) | DbgPrint | FUNC | cdecl | var (1 Push an der Site) | 1 | D3D(1) | ja | cdecl/varargs, einzige Site D3D FUN_0023e600. vsnprintf + OutputDebugStringA | trivial |
| 99 | 0x2936ec | 168 (0x0a8) | MmClaimGpuInstanceMemory | FUNC | stdcall | 2 | 1 | D3D(1) | ja | 2 Args, nur D3D FUN_0023e7a7. Bei D3D-HLE entbehrlich; sonst Fake-Adresse | trivial |
| 100 | 0x2936f0 | 142 (0x08e) | KeSaveFloatingPointState | FUNC | stdcall | 1 | 1 | DSOUND(1) | ja | Nur DSOUND. STATUS_SUCCESS, no-op | trivial |
| 101 | 0x2936f4 | 139 (0x08b) | KeRestoreFloatingPointState | FUNC | stdcall | 1 | 1 | DSOUND(1) | ja | Nur DSOUND. STATUS_SUCCESS, no-op | trivial |
| 102 | 0x2936f8 | 23 (0x017) | ExQueryPoolBlockSize | FUNC | stdcall | 1 | 2 | DSOUND(2) | ja | Nur DSOUND. HeapSize | trivial |
| 103 | 0x2936fc | 137 (0x089) | KeRemoveQueueDpc | FUNC | stdcall | 1 | 4 | DSOUND(3) | ja | Aus Host-DPC-Queue entfernen. Nur DSOUND (Review: keine XPP-Site) -> Stub | trivial |
| 104 | 0x293700 | 153 (0x099) | KeSynchronizeExecution | FUNC | stdcall | 3 | 2 | DSOUND(2) | ja | Nur DSOUND. Stub: SynchronizeRoutine(Context) direkt unter Dispatcher-Lock aufrufen | trivial |
| 105 | 0x293704 | 312 (0x138) | RtlUnwind | FUNC | stdcall | 4 | 2 | LIBC(2) | nein | 4 Args, LIBC __global_unwind2/_UnwindNestedFrames -> ntdll!RtlUnwind (fs:[0]-Kette wie oben) | trivial |
| 106 | 0x293708 | 95 (0x05f) | KeBugCheck | FUNC | stdcall | 1 | 5 | LIBC(4), XAPI(1) | nein | LIBC/XAPI-Fatalpfade (Codes 0, 0xA, 0xC0000144). Loggen + Abbruch | trivial |
| 107 | 0x29370c | 169 (0x0a9) | MmCreateKernelStack | FUNC | stdcall | 2 | 1 | XAPI(1) | nein | XAPI CreateFiber (Spiel FUN_0013f250 ruft CreateFiber). Stack per VirtualAlloc, Rueckgabe Obergrenze; besser Fiber-API (Create/Switch/Delete/ConvertThreadToFiber) per HLE auf Win32-Fibers | mittel |
| 108 | 0x293710 | 170 (0x0aa) | MmDeleteKernelStack | FUNC | stdcall | 2 | 1 | XAPI(1) | nein | XAPI DeleteFiber. VirtualFree des Stacks (entfaellt bei Fiber-HLE) | trivial |
| 109 | 0x293714 | 5 (0x005) | DbgBreakPoint | FUNC | stdcall | 0 | 1 | XAPI(1) | nein | 0 Args, Tail-Jump aus XAPILIB::DebugBreak. No-op oder __debugbreak | trivial |
| 110 | 0x293718 | 356 (0x164) | HalBootSMCVideoMode | DATA |  |  | 0 | XAPI(1) | nein | DWORD; XAPILIB::XGetAVPack gibt *slot zurueck (AV-Pack). Konfigurierbarer Wert (Konstanten laut XDK, Vermutung) | trivial |

## 10. Offene Punkte

- **Bedeutung einzelner Konstanten:** IOCTL/FSCTL-Codes, HalReturnToFirmware-Routinen 2/4, AV-Pack-Werte, XboxHardwareInfo-Bits, ExQueryNonVolatileSetting-Indizes. Die Werte selbst sind belegt, ihre Namen stammen aus XDK-Wissen und sind als Vermutung markiert.
- **Größe der SHA-1-Kontextpuffer:** Die Puffer, die XAPI an XcSHAInit übergibt, sind noch nicht vermessen. Der Host-State muss hineinpassen.
- **Layout der Gast-Strukturen** RTL_CRITICAL_SECTION, KEVENT, KTIMER und KDPC: Die Offsets für eine Implementierung auf den Gastfeldern sind noch aus den XAPI/LIBC-Zugriffen abzuleiten.
- **fs:-Patching vs. eigenes Segment:**
  - Option 1: die 27 + 11 Stellen patchen und den Windows-TEB behalten (SEH an fs:[0] ist kompatibel).
  - Option 2: ein eigenes FS-Segment per LDT setzen (Cxbx-Stil).
  - Das muss der Loader entscheiden; beide Varianten sind ungetestet.
- **Abdeckung:** Rund 28 Kernel-referenzierende Codestücke liegen außerhalb von Ghidra-Funktionen und wurden über Pseudo-Einstiege (`sub_xxxxxx`) analysiert. Ein erneuter Ghidra-Lauf mit diesen Einstiegen würde die Abdeckung des Decomp verbessern. Er ist nicht Teil dieser Analyse, weil das Ghidra-Projekt nicht geöffnet werden durfte.

## Verifikation

Unabhängiges Review vom 2026-10-04. Alle Prüfungen sind mit eigenen Skripten `scripts\verify_kernel_*.py` reproduzierbar. Diese Skripte verwenden nur `tools\xbe.py`, capstone und die Ghidra-Export-TSVs; `kernel_common.py` und `kernel_analyze.py` des Erstautors importieren sie nicht. Einzige Ausnahme ist der letzte Vergleichsblock in `verify_kernel_ordtable.py`.

Als zusätzliche unabhängige Quelle für die Ordinal→Name-Zuordnung diente die Tabelle `kernelExports[]` aus dem Quellarchiv der installierten Ghidra-Erweiterung ghidra-xbe (`%APPDATA%\ghidra\…\ghidra-xbe-src.zip`). Das ZIP wurde nur gelesen; Ghidra und `D:\FuturamaDecomp\ghidra` wurden nicht geöffnet.

### Skripte

| Skript | Prüft |
|---|---|
| `verify_kernel_common.py` | Hilfen: XBE-Header roh, Thunk-Tabelle über Header-Feld 0x158, Funktionsliste, Modulgrenzen wie im Bericht |
| `verify_kernel_thunks.py` | Header-XOR, 111 Einträge/Terminator, TSV-Slot/Ordinal, Ghidra-`EXTERNAL`-Index, Stub-Namen, `_exref`, „nicht importiert“, Sektionsflags |
| `verify_kernel_ordtable.py` | Ordinal→Name gegen ghidra-xbe `kernelExports[]` (auch für die 3 Slots ohne Ghidra-Codebeleg) |
| `verify_kernel_sites.py` | eigene Enumeration aller Slot-Referenzen: Byte-Scan aller Sektionen und Dekodierung, Stubs, Stub-Calls, lineares `mov reg,[slot]`→`call reg` (schreibt `verify_kernel_sites.json`) |
| `verify_kernel_regflow.py` | CFG-Registerverfolgung für `call reg`, auch hinter Verzweigungen |
| `verify_kernel_stack.py` | eigenes CFG-Stacktiefenmodell (inkl. `__SEH_prolog`/`__SEH_epilog`, `ret N` interner Callees); Purge 0..0x30 je Import |
| `verify_kernel_callconv.py`, `verify_kernel_fastcall.py`, `verify_kernel_args.py` | Push-/Cleanup-Heuristik, ecx/edx vor fastcall-Sites, Argument-Pushes je Site |
| `verify_kernel_modules.py` | Modulgrenzen, 37/74-Aufteilung, TSV-Spalten, Host-Plan-Text gegen tatsächliche Aufrufer |
| `verify_kernel_fs_rdtsc.py` | fs:-Zugriffe, `rdtsc`, 733-MHz-Konstante |
| `verify_kernel_highaddr.py` | direkte Operanden im Kernel-Adressraum 0x8xxxxxxx |
| `verify_kernel_dump.py`, `verify_kernel_regtrace.py`, `verify_kernel_xrefs.py` | Evidenz-Helfer: Disassembly, Register-Trace, Code- und Datenreferenzen |

### Key Facts 1–18

| # | Ergebnis | Eigene Evidenz |
|---:|---|---|
| 1 | bestätigt | hdr[0x158] = 0x5B4475D6 ^ 0x5B6D40B6 = 0x293560. 111 Einträge, alle mit Bit 31, 111 verschiedene Ordinals (1..360), Wert 0 bei 0x29371C. |
| 2 | bestätigt, verstärkt | 87 `EXTERNAL`-Indizes, alle = Slot+1. 9 Ghidra-Stubnamen stimmen, alle 13 `_exref`-Namen stehen in der TSV, 108 Namen haben einen Ghidra-Beleg. Zusätzlich stimmen **alle 111** Namen mit ghidra-xbe `kernelExports[]` überein, auch Ord 258/360/357. `kernel_common.XBOXKRNL_EXPORTS` ist für 1..366 identisch. |
| 3 | bestätigt | KfLowerIrql: ecx an 28/28 Sites (eine davon ist der Tail-Jump 0x2907B9). IofCompleteRequest: ecx 8/9 (0x28EC33 ohne), `xor dl,dl` 9/9. KfRaiseIrql 2/2. ObfDereferenceObject `mov ecx,edi; jmp [0x293658]`. DbgPrint: `add esp,0x118` nach 0x23E627; nach keinem anderen Import-Call folgt `add esp`. |
| 4 | bestätigt | NtCreateFile 9 Pushes (0x0F6B09: 0x22, 3, 0, 4, &, &IoStatus, &ObjAttr, 0xC0100000, &Handle). PsCreateSystemThreadEx 10 Pushes (0x0F9A84–0x0F9AAA). KeSetTimer 4 DWORDs (0x0F9AED: Dpc 0x310340, DueTime High/Low, Timer 0x310360). NtAllocateVirtualMemory: 5 Argument-Pushes an 11/11 Sites; bei 0x0F3E5C kommen 4 Register-Saves aus dem Prolog hinzu. Eigenes Stackmodell: 80 der 97 Imports mit Purge-Wert sind eindeutig, 6 ohne Constraint, 5 Gleichstand, 2 ohne Ghidra-Funktion. In 4 Fällen ist mein gröberes Modell „schlechter“ (NtOpen/NtQuerySymbolicLinkObject, MmPersistContiguousMemory, MmAllocateContiguousMemory; alle in XLaunchNewImageA bzw. FUN_000f81bb). Das Disassembly zeigt dort 2/3/3/1 Argument-Pushes, wie in der TSV. Alle nicht eindeutigen Fälle wurden per Disassembly gegengezählt (u. a. 0x0F75AA, 0x0F7894, 0x0F7C41, 0x0F504D, 0x23E7F0, 0x166949, 0x166C67, 0x0F9A6B, 0x0F9B11). |
| 5 | bestätigt | Eigene Zählung: 549 + 1 (0x291A65, `ss`-Präfix-Mehrdeutigkeit) = 550 `call [slot]`; 49 `call reg` (40 linear und 9 hinter Verzweigungen, CFG-Verfolgung findet genau 49); 26 Stub-Calls; 5 Tail-Jumps (4 `jmp [slot]` und 1 `jmp` auf Stub 0x170AB4). Summe 630. Nach Modul: XAPI 232, DSOUND 165, XPP 153, D3D 59, LIBC 18, GAME 3. Der Byte-Scan findet Slot-Adressen nur in .text/DSOUND/XPP/D3D, nicht in XMV/DOLBY/.data. |
| 6 | bestätigt | Aus der TSV nachgerechnet: 37/74 und 51/21/2. Die Liste in Abschnitt 6 ist identisch mit der TSV-Menge. |
| 7 | bestätigt | GAME-Sites 0x0EE14A und 0x0EE157 (FUN_000ee080 → 0xF8463) sowie 0x161259 (sub_161180 → 0x2417B0). 0x161180 steht als Zeiger bei 0x2AAF08 (.rdata). |
| 8 | bestätigt | Ordinals 156, 126, 127, 321, 45, 50, 177, 326, 340, 189 und 90 sind nicht importiert, ebenso 108, 254, 163 und 251. |
| 9 | bestätigt, Adresse korrigiert | Builds und Patchwert stimmen. Die Zieladresse ist je nach Flags-Bit 1 0x8002xxxx oder 0x8003xxxx (Abschnitt 8.2). |
| 10 | bestätigt | TLS-Directory {0, 0, 0x2F3D18, 0, 0x20, 0}; Rechnung 0x0F84F5–0x0F852F; Zugriffsmuster in GetLastError 0x0F6E66 ff. |
| 11 | bestätigt, präzisiert | fs:[4]/[0x20]/[0x24]/[0x28]/[0x58] = 11/8/8/9/2: in Ghidra-Funktionen 8/6/7/6/2, außerhalb 3/2/1/3/0. Alle fs:[0x20]-Sites lesen nur DWORD +0x250. KTHREAD-Offsets sind +0x1C, +0x28 und +0x12C. fs:[0]: eigene Zählung 4850 in Ghidra-Funktionen plus 318 außerhalb (Byte-Scan) statt 5054; die Methoden unterscheiden sich, die Größenordnung reicht. |
| 12 | bestätigt | Callgraph FUN_0013f660 → ReadFileEx, FUN_000e0b00 → WriteFileEx. NtReadFile(Handle, 0, [NtUserIoApcDispatcher], Completion-Routine, lpOverlapped, Buffer, Len, &Offset). |
| 13 | bestätigt | 0x1708BC `mov eax,[0x10130]`, Mindestgröße 0x3000, seitenaligned, MmCreateKernelStack(Size, 0) bei 0x1708DA. |
| 14 | bestätigt, ergänzt | Push-Quellen 7/0x104/9/0xA. Über XQueryValue zusätzlich 0x11 (0x0F9B65) und 0xFFFF (0x0F7030). |
| 15 | bestätigt | 0x0F641F `rdtsc`. 0x0F6434 `c7 00 55 c7 b5 2b`. Die Konstante kommt in der Datei nur einmal vor (Offset 0xE6436). |
| 16 | bestätigt, ergänzt | KeInitializeDpc(0x310340, 0xF9B08, 0) bei 0x0F9B2C, KeInitializeTimerEx(0x310360, 0). Die DPC-Routine endet mit `ret 0x10` (4 DPC-Argumente). DueTime 6 h, Freigabe über Setting 0x11. |
| 17 | bestätigt, präzisiert | Beide Zugriffe nur für 0x5C ≤ esi < 0x88, effektive Adressen also 0x293778–0x293824, hinter dem Terminator. |
| 18 | Flags bestätigt, Folgerung korrigiert | .text 0x16; D3D/DSOUND/XMV/XPP/.data 0x07; .rdata 0x06; DOLBY 0x26; `$$XTIMAGE`/`.XTLID` 0x38. `$$XTIMAGE` wird aber beim Boot per XeLoadSection geladen (Abschnitt 7). |

### Weitere geprüfte Aussagen (Auswahl)

- **Abschnitt 5, alle 13 Zeilen** am Disassembly geprüft: 0x28DA06, 0x0F7004, 0x0F70C6/0x0F70CE (Schleife über 16 AlternateTitleIds ab Zertifikat+0x5C), 0x0F7DD4/0x0F7E2B, 0x0F9409/0x0F9459, 0x0F933D/0x0F9B3F/0x23C846/0x28C8F6/0x28C9E8/0x28E5AF, 0x0F90BE, 0x0F980B, 0x0F9C63/0x0F9C80, 0x170AA5, 0x28D254 (10 Stores), 0x0F7625/0x0F7676.
- **RtlUnwind-Ausnahme** (`push ebp` um den Call, 0x166C5A/0x166C6C): bestätigt.
- **NtAllocateVirtualMemory Protect = 4** an 11/11 Sites: bestätigt.
- **Informationsklassen:** Query 0x22/0xE (Länge 0x38/8), Set 4/0xD/0xE/0x13/0x14, Volume 3 (0x18) an 4/4 Sites, Directory 1: bestätigt.
- **Konstanten an Sites:** HalGetInterruptVector-Level 3/5/6/1, AvSendTVEncoderOption 6/9/0xB/0xE/0xF, KeBugCheck 0xA/0xA/0xC0000144/0/0, NtDeviceIoControlFile 0x4D014 (2×, FUN_000f90e6)/0x70000/0x74004, NtFsControlFile 0x90020: bestätigt.
- **PsCreateSystemThreadEx-Argumentreihenfolge** (Abschnitt 7): bestätigt.
- **PsTerminateSystemThread:** 0x0F9A71 ist `int3`. HalRegisterShutdownNotification(0x2D7530, 1). Die Struktur 0x2D7530 = {0xF9C7F, 0x80000000, 0, 0}, sub_0f9c7f endet mit `ret 4`. KeWait/KeSetEvent arbeiten auf dem Gast-KEVENT 0x2D7540. Alles bestätigt.
- **Modulgrenzen:** Alle 72 `XAPILIB::*`-Namen in .text liegen im XAPI-Bereich; kein `Ni*`/`::vf`-Name liegt im XAPI- oder LIBC-Bereich. 46 Imports werden nur von XAPI genutzt: bestätigt.
- **Abschnitt 8.3:** entry liest [0x10118] und [0x10108] und schreibt ggf. [Zertifikat]; [0x10130] = 0x10000. Bestätigt.
- **Kernel-Adressraum:** Außer FUN_000f9804 greift kein XAPI/LIBC/GAME-Code direkt auf 0x8xxxxxxx zu. 0x80100000 kommt in XAPI nur als Access-Mask-Konstante vor (`verify_kernel_highaddr.py`).
- **0x23BCC6** ist ein Zufallstreffer in `e8 35 29 00 00` (call 0x23E600): bestätigt.

### Korrigiert (im Text, in der Tabelle und in der TSV)

1. **Spalte Art:** ExEventObjectType, XboxHDKey, XboxSignatureKey, XboxAlternateSignatureKeys und XePublicKeyData standen als ADDR, sind aber Datenexporte. Jetzt gilt DATA 11 / ADDR 2 / FUNC 98, passend zu Abschnitt 1. `kernel_report.py` ist korrigiert (`FUNC_ADDR_ONLY`), `kernel_imports.tsv`, `scripts\kernel_tables.md` und `scripts\kernel_stats.json` sind neu erzeugt. Der Diff gegen den Stand vor dem Review enthält nur die beabsichtigten Änderungen.
2. **Kernel-Patchadresse** (Kurzfassung, 8.2, TSV XboxKrnlVersion): „0x8003xxxx“ → 0x800299A0..0x800398CD, abhängig von Build und Flags-Bit 1.
3. **8.8:** „XMV (sub_2742c0) rdtsc“ ist ein Datentreffer und kein Code.
4. **XeLoadSection/XeUnloadSection** (Abschnitt 7, TSV): `$$XTIMAGE` wird beim Boot geladen, die Rohdaten müssen also bereitstehen.
5. **NtReadFile/NtWriteFile:** „8 Args wie NT“ ist falsch, NT hat 9 Argumente (mit `Key`). Gilt für TSV/Tabelle und Abschnitt 4.
6. **Host-Plan KeDisconnectInterrupt** („D3D/DSOUND/XPP“ → D3D/DSOUND) und **KeRemoveQueueDpc** („DSOUND/XPP“ → DSOUND): Es gibt keine XPP-Site.
7. **Abschnitt 3:** Index-Untergrenze 0x5C ergänzt; damit ist die Aussage „kein Kernelzugriff“ belegt und nicht nur behauptet.
8. **HalReturnToFirmware:** Argument 4 kommt nur bei TitleID 0xFFFE0000 vor; für dieses Spiel ist es Argument 2.
9. **Form:** Listenumbruch in Abschnitt 3 und Pfad `scripts\kernel_tables.md` in Abschnitt 9.

### Vom Bericht übersehen (jetzt ergänzt)

- **Kindersicherung:** XapiInitProcess vergleicht XGetParentalControlSetting (Setting 0xA) mit dem Zertifikatfeld GameRatings, das hier 0 ist (0x0F93E8). Ist das Setting größer, folgt XapiBootToDash. Der Host muss für 0xA den Wert 0 liefern.
- **DVD-Prüfung wird ignoriert:** FUN_000f90e6 (SCSI-IOCTL plus PKCS1-Signatur) wird aufgerufen, ihr Ergebnis verworfen (0x0F931C/0x0F931E). Der Host muss dort nur absturzfrei antworten.
- **Weitere Setting-Indizes:** 0x11 (AutoPowerDown) und 0xFFFF (ganzes EEPROM; nur wenn XboxHDKey = 0). XboxHDKey muss deshalb ≠ 0 sein.
- **Boot-Pfad von XapiInitProcess** vollständig: Heap, `\Device\Harddisk0\partition1\`-Prüfung, D:/T:/U:-Symlinks (`TDATA\46490002`, `UDATA\…`), TitleMeta.xbx/TitleImage.xbx, XMountUtilityDrive (InitFlags 5). Dafür sind beim ersten Start auch Datei-Schreibfunktionen und XeLoadSection nötig.
- **KPCR-Minimalinhalt:** fs:[0x20]→Block mit +0x250 = 0; fs:[0x24] < 2, sonst KeBugCheck(0xA) in LIBC (0x1692DE/0x16936C); fs:[0x28]→KTHREAD mit +0x1C, +0x28 (TlsData; LIBC fällt bei 0 auf die globale 0x310734 zurück) und +0x12C.
- **AutoPowerDown-Parameter:** 6 h relativ, aktiv nur bei Setting 0x11 Bit 0.
- **XPhysicalAlloc:** Alle drei Aufrufer (Spiel FUN_000ee220, XMemAlloc, D3D FUN_00241c40) übergeben PhysicalAddress = −1. Eine feste physische Adresse wird nie angefordert.
- **SwitchToFiber** ändert fs:[4] nicht (0x17094F–0x17098B). Der TLS-Anker bleibt pro Thread gleich; das passt zur vorgeschlagenen Fiber-HLE.

### Unsicher / nicht abschließend geprüft

- Ob XapiMapLetterToDirectory `$$XTIMAGE` bei jedem Boot lädt oder nur, wenn TitleImage.xbx fehlt. Die Verzweigungen in 0x0F8C41–0x0F8F75 wurden nicht vollständig verfolgt.
- Die Bedeutung der KPCR/KTHREAD-Offsets (+0x250, +0x1C, fs:[0x58]) und aller XDK-Konstanten bleibt Vermutung.
- Die fs:[0]-Gesamtzahl hängt von der Methode ab (5054 bzw. 5168).
- Die 4 „schlechter“-Fälle meines Stackmodells sind laut Push-Zählung Modellartefakte; ihre Ursache im Modell wurde nicht weiter untersucht.
- `kernel_raw.json` (Purge-Solver des Erstautors) wurde nicht neu erzeugt; die Konventionen wurden unabhängig davon nachgeprüft (s. o.).
