// GENERATED FILE - DO NOT EDIT.
// Generator: tools/gen/gen_kernel_table.py (run with --check to verify).
// Sources:   analysis/loader/kernel_imports.tsv
//
// Logging stubs for every FUNC slot that has no registered implementation (design 1.5, 5.4).
// Each stub has exactly the guest calling convention and stack purge of its slot, takes DWORD
// parameters, logs "UNIMPLEMENTED Ord <n> <Name>(args) ret=<caller>" once per return address and
// returns STATUS_NOT_IMPLEMENTED (0xC0000002). DATA slots point to a zeroed, writable placeholder
// buffer; ADDR slots to a routine that aborts when called (its convention is unknown).
#include "kernel/kernel_api.h"

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace krnl::gen {
namespace {

using detail::ReportUnimplemented;
using detail::ReportAddrPlaceholderCall;
using detail::kDataPlaceholderSize;

// slot 0, Ord 161: fastcall cl=NewIrql. IRQL-Emulation: thread-lokale IRQL; beim Absenken unter DISPATCH_LEVEL globalen Dispatcher-Lock freigeben und anstehende DPCs ausfuehren. XAPI-Nutzer: Shutdown-Routine sub_0f9c7f; Rest DSOUND/XPP
std::uint32_t __fastcall Stub_KfLowerIrql(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(0, args, 1, _ReturnAddress());
}

// slot 1, Ord 129: stdcall ohne Args, alte IRQL in al (22/24 Sites lesen al). Globalen rekursiven Dispatcher-Lock nehmen, IRQL=2 merken
std::uint32_t __stdcall Stub_KeRaiseIrqlToDpcLevel(void) noexcept {
    return ReportUnimplemented(1, nullptr, 0, _ReturnAddress());
}

// slot 2, Ord 65: Nur XPP MU_Init (Memory-Unit-Treiber). Bei XInput/XPP-HLE Stub (STATUS_SUCCESS, Dummy-DEVICE_OBJECT)
std::uint32_t __stdcall Stub_IoCreateDevice(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6) noexcept {
    const std::uint32_t args[6] = {a1, a2, a3, a4, a5, a6};
    return ReportUnimplemented(2, args, 6, _ReturnAddress());
}

// slot 3, Ord 289: ANSI_STRING initialisieren (ntdll-identisch)
std::uint32_t __stdcall Stub_RtlInitAnsiString(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(3, args, 2, _ReturnAddress());
}

// slot 4, Ord 145: KEVENT liegt im Gastspeicher. SignalState setzen, Host-Event (Map) signalisieren, alten Zustand zurueck. XAPI sub_0f9c63 (IDE-Shutdown-Callback), sonst D3D/XPP
std::uint32_t __stdcall Stub_KeSetEvent(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(4, args, 3, _ReturnAddress());
}

// slot 5, Ord 159: Generisches Warten auf Gast-Dispatcher-Objekte (KEVENT/KTIMER), Timeout PLARGE_INTEGER (negativ = relativ, 100 ns). Host: Objekt->Host-Handle, WaitForSingleObject. XAPI sub_0f9c7f, D3D BlockUntilVerticalBlank/BlockOnTime, XPP
std::uint32_t __stdcall Stub_KeWaitForSingleObject(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5) noexcept {
    const std::uint32_t args[5] = {a1, a2, a3, a4, a5};
    return ReportUnimplemented(5, args, 5, _ReturnAddress());
}

// slot 6, Ord 113: KTIMER im Gastspeicher initialisieren (Header, SignalState=0) und Host-Timer anlegen (Map Gastadresse->Host-Objekt). XAPI: AutoPowerDown-Timer 0x310360
std::uint32_t __stdcall Stub_KeInitializeTimerEx(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(6, args, 2, _ReturnAddress());
}

// slot 7, Ord 107: KDPC im Gastspeicher fuellen (Routine, Context); DPC-Queue im Host, Abarbeitung in DPC-Thread unter Dispatcher-Lock. XAPI FUN_000f9b1a: DPC 0xf9b08 (AutoPowerDown -> HalInitiateShutdown)
std::uint32_t __stdcall Stub_KeInitializeDpc(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(7, args, 3, _ReturnAddress());
}

// slot 8, Ord 74: Nur als Funktionsadresse: XPP FUN_0028d254 kopiert sie in 10 Dispatch-Slots ab 0x28c004. Adresse einer Dummy-Routine (STATUS_INVALID_DEVICE_REQUEST)
void __cdecl AddrPlaceholder_IoInvalidDeviceRequest() noexcept { ReportAddrPlaceholderCall(8, _ReturnAddress()); }

// slot 9, Ord 294: Gegenstueck zu RtlEnterCriticalSection (108 Sites, davon 96 DSOUND)
std::uint32_t __stdcall Stub_RtlLeaveCriticalSection(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(9, args, 1, _ReturnAddress());
}

// slot 10, Ord 277: Xbox-RTL_CRITICAL_SECTION im Gastspeicher (eingebettetes KEVENT, Layout != Win32). Eigene Implementierung auf den Gastfeldern oder Map Gast-CS -> Host-CRITICAL_SECTION. XAPI-Heap, LIBC-Locks, DSOUND
std::uint32_t __stdcall Stub_RtlEnterCriticalSection(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(10, args, 1, _ReturnAddress());
}

// slot 11, Ord 199: 3 Args: BaseAddress*, RegionSize*, FreeType (0x8000/0x4000). VirtualFree
std::uint32_t __stdcall Stub_NtFreeVirtualMemory(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(11, args, 3, _ReturnAddress());
}

// slot 12, Ord 184: 5 Args (kein ProcessHandle): BaseAddress*, ZeroBits, RegionSize*, AllocationType, Protect (=4 an allen 11 Sites). VirtualAlloc, In/Out-Parameter zurueckschreiben; Xbox-only-Flags maskieren. Nutzer: XAPI-Heap
std::uint32_t __stdcall Stub_NtAllocateVirtualMemory(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5) noexcept {
    const std::uint32_t args[5] = {a1, a2, a3, a4, a5};
    return ReportUnimplemented(12, args, 5, _ReturnAddress());
}

// slot 13, Ord 269: Selbst implementieren / ntdll
std::uint32_t __stdcall Stub_RtlCompareMemoryUlong(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(13, args, 3, _ReturnAddress());
}

// slot 14, Ord 291: Gast-Struktur initialisieren / Host-Objekt anlegen (XAPI-Heap, LIBC)
std::uint32_t __stdcall Stub_RtlInitializeCriticalSection(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(14, args, 1, _ReturnAddress());
}

// slot 15, Ord 217: 2 Args: BaseAddress, MEMORY_BASIC_INFORMATION*. VirtualQuery
std::uint32_t __stdcall Stub_NtQueryVirtualMemory(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(15, args, 2, _ReturnAddress());
}

// slot 16, Ord 302: EXCEPTION_RECORD* -> ntdll!RtlRaiseException; setzt voraus, dass fs:[0] die Windows-SEH-Kette ist (Spielcode nutzt fs:[0] 5054x im MSVC-Format)
std::uint32_t __stdcall Stub_RtlRaiseException(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(16, args, 1, _ReturnAddress());
}

// slot 17, Ord 24: 5 Args; Indizes aus Binary: 7 (XGetLanguage), 0x104 (XGetGameRegion), 9 (XGetAudioFlags), 0xA (XGetParentalControlSetting), XQueryValue reicht durch; Review: ueber XQueryValue zusaetzlich 0x11 (FUN_000f9b1a, Bit 0 = AutoPowerDown an) und 0xFFFF (FUN_000f6ffa, ganzes EEPROM 0x100 Byte, nur wenn XboxHDKey komplett 0 ist). Konfigurationstabelle (Sprache, Region EU/PAL, Audio, Kindersicherung); 0xA muss 0 liefern (oder Fehler), sonst XapiBootToDash in XapiInitProcess (cert GameRatings = 0, Vergleich 0x0F93E8). Bedeutung laut XDK (Vermutung)
std::uint32_t __stdcall Stub_ExQueryNonVolatileSetting(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5) noexcept {
    const std::uint32_t args[5] = {a1, a2, a3, a4, a5};
    return ReportUnimplemented(17, args, 5, _ReturnAddress());
}

// slot 18, Ord 16: POBJECT_TYPE; nur XPP: push [slot] als ObjectType fuer ObReferenceObjectByHandle. Adresse eines Dummy-Objekts eintragen
alignas(16) unsigned char Placeholder_ExEventObjectType[kDataPlaceholderSize];

// slot 19, Ord 225: Nur DSOUND (Notify-Events). Bei DSOUND-HLE entbehrlich, sonst SetEvent auf uebersetztem Handle
std::uint32_t __stdcall Stub_NtSetEvent(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(19, args, 2, _ReturnAddress());
}

// slot 20, Ord 234: XAPI WaitForSingleObjectEx (Handle, WaitMode=1, Alertable, Timeout*) -> WaitForSingleObjectEx mit Handle-Uebersetzung, STATUS_* <-> WAIT_* abbilden
std::uint32_t __stdcall Stub_NtWaitForSingleObjectEx(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4) noexcept {
    const std::uint32_t args[4] = {a1, a2, a3, a4};
    return ReportUnimplemented(20, args, 4, _ReturnAddress());
}

// slot 21, Ord 99: XAPI SleepEx: (WaitMode=1, Alertable, Interval*). SleepEx mit Umrechnung 100 ns -> ms (alertable fuer APCs von ReadFileEx/WriteFileEx); STATUS_USER_APC bei APC
std::uint32_t __stdcall Stub_KeDelayExecutionThread(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(21, args, 3, _ReturnAddress());
}

// slot 22, Ord 226: Klassen 4 (Basic), 0xD (Disposition), 0xE (Position), 0x13 (Allocation), 0x14 (EndOfFile) -> NT-identisch, ntdll weiterreichen
std::uint32_t __stdcall Stub_NtSetInformationFile(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5) noexcept {
    const std::uint32_t args[5] = {a1, a2, a3, a4, a5};
    return ReportUnimplemented(22, args, 5, _ReturnAddress());
}

// slot 23, Ord 128: GetSystemTimeAsFileTime (gleiche Epoche/Einheit)
std::uint32_t __stdcall Stub_KeQuerySystemTime(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(23, args, 1, _ReturnAddress());
}

// slot 24, Ord 301: ntdll!RtlNtStatusToDosError
std::uint32_t __stdcall Stub_RtlNtStatusToDosError(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(24, args, 1, _ReturnAddress());
}

// slot 25, Ord 190: 9 Args (ohne EaBuffer/EaLength), OBJECT_ATTRIBUTES mit ANSI_STRING. Pfaduebersetzung (\Device\CdRom0, \Device\Harddisk0\PartitionN, \??\D: usw. ueber eigene Symlink-Tabelle) -> CreateFileW/ntdll; Xbox-Pfade case-insensitive. XAPI + D3D-Moviecache
std::uint32_t __stdcall Stub_NtCreateFile(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6, std::uint32_t a7, std::uint32_t a8, std::uint32_t a9) noexcept {
    const std::uint32_t args[9] = {a1, a2, a3, a4, a5, a6, a7, a8, a9};
    return ReportUnimplemented(25, args, 9, _ReturnAddress());
}

// slot 26, Ord 187: CloseHandle ueber Gast-Handle-Tabelle (Dateien, Threads, Symlinks)
std::uint32_t __stdcall Stub_NtClose(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(26, args, 1, _ReturnAddress());
}

// slot 27, Ord 336: SHA-1 update (ctx, data, len)
std::uint32_t __stdcall Stub_XcSHAUpdate(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(27, args, 3, _ReturnAddress());
}

// slot 28, Ord 335: SHA-1 init auf Gast-Kontextpuffer (eigene Implementierung). Nutzer: XAPI-Signaturcode (XCalculateSignature*, vom Spiel FUN_000e0340/FUN_000e03b0 genutzt) und FUN_000f902f
std::uint32_t __stdcall Stub_XcSHAInit(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(28, args, 1, _ReturnAddress());
}

// slot 29, Ord 337: SHA-1 final (ctx, digest[20])
std::uint32_t __stdcall Stub_XcSHAFinal(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(29, args, 2, _ReturnAddress());
}

// slot 30, Ord 323: 16 Byte, XAPI FUN_000f6ffa (Signatur-Schluesselableitung). Konstanter Dummy-Schluessel, muss != 0 sein (Review: bei 16 Nullbytes liest FUN_000f6ffa das EEPROM per XQueryValue(0xFFFF)); eigene Saves konsistent, Original-Saves braeuchten echte Schluessel
alignas(16) unsigned char Placeholder_XboxHDKey[kDataPlaceholderSize];

// slot 31, Ord 325: 16 Byte, Adresse in XCalculateSignatureBeginEx gespeichert. Dummy-Schluessel
alignas(16) unsigned char Placeholder_XboxSignatureKey[kDataPlaceholderSize];

// slot 32, Ord 354: 16 x 16 Byte, add eax,[slot] -> &Keys[i] (XCalculateSignatureBeginEx). Dummy-Tabelle
alignas(16) unsigned char Placeholder_XboxAlternateSignatureKeys[kDataPlaceholderSize];

// slot 33, Ord 233: XAPI ReadFile/WriteFile warten bei STATUS_PENDING auf das Dateihandle (Alertable=0, Timeout=NULL) -> WaitForSingleObject
std::uint32_t __stdcall Stub_NtWaitForSingleObject(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(33, args, 3, _ReturnAddress());
}

// slot 34, Ord 219: 8 Args (Review: NT hat 9, Xbox ohne Key; 8 Pushes z.B. ReadFileEx 0x0F7630). Handle-Uebersetzung -> ntdll/ReadFile; ist ApcRoutine == NtUserIoApcDispatcher-Marker (ReadFileEx), Completion-Routine (ApcContext) per QueueUserAPC im Aufrufer-Thread zustellen
std::uint32_t __stdcall Stub_NtReadFile(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6, std::uint32_t a7, std::uint32_t a8) noexcept {
    const std::uint32_t args[8] = {a1, a2, a3, a4, a5, a6, a7, a8};
    return ReportUnimplemented(34, args, 8, _ReturnAddress());
}

// slot 35, Ord 236: 8 Args (NT: 9, ohne Key); wie NtReadFile (WriteFileEx-APC)
std::uint32_t __stdcall Stub_NtWriteFile(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6, std::uint32_t a7, std::uint32_t a8) noexcept {
    const std::uint32_t args[8] = {a1, a2, a3, a4, a5, a6, a7, a8};
    return ReportUnimplemented(35, args, 8, _ReturnAddress());
}

// slot 36, Ord 211: Klassen 0x22 (NetworkOpen, 0x38) und 0xE (Position, 8) -> NT-identisch, ntdll weiterreichen
std::uint32_t __stdcall Stub_NtQueryInformationFile(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5) noexcept {
    const std::uint32_t args[5] = {a1, a2, a3, a4, a5};
    return ReportUnimplemented(36, args, 5, _ReturnAddress());
}

// slot 37, Ord 218: Klasse 3 (FileFsSizeInformation, 0x18) an allen 4 Sites -> GetDiskFreeSpaceEx, Werte ggf. faken
std::uint32_t __stdcall Stub_NtQueryVolumeInformationFile(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5) noexcept {
    const std::uint32_t args[5] = {a1, a2, a3, a4, a5};
    return ReportUnimplemented(37, args, 5, _ReturnAddress());
}

// slot 38, Ord 198: XAPI FlushFileBuffers -> FlushFileBuffers
std::uint32_t __stdcall Stub_NtFlushBuffersFile(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(38, args, 2, _ReturnAddress());
}

// slot 39, Ord 232: Nur Adresse: ReadFileEx/WriteFileEx pushen [slot] als ApcRoutine. Spiel nutzt beide (FUN_0013f660, FUN_000e0b00). Marker-Adresse eintragen, die eigenes NtReadFile/NtWriteFile erkennt
void __cdecl AddrPlaceholder_NtUserIoApcDispatcher() noexcept { ReportAddrPlaceholderCall(39, _ReturnAddress()); }

// slot 40, Ord 200: Nur XAPI FUN_000f946d (Utility-Partition formatieren): FSCTL 0x90020 (Vermutung: DISMOUNT_VOLUME). STATUS_SUCCESS
std::uint32_t __stdcall Stub_NtFsControlFile(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6, std::uint32_t a7, std::uint32_t a8, std::uint32_t a9, std::uint32_t a10) noexcept {
    const std::uint32_t args[10] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10};
    return ReportUnimplemented(40, args, 10, _ReturnAddress());
}

// slot 41, Ord 196: XAPI FUN_000f90e6: IOCTL 0x4D014 (Vermutung: SCSI_PASS_THROUGH_DIRECT an DVD), FUN_000f946d: 0x70000/0x74004 (Vermutung: Geometry/PartitionInfo). Fake-Antworten oder Fehlerstatus (Ergebnis von FUN_000f90e6 wird in XapiInitProcess ignoriert, 0x0F931E)
std::uint32_t __stdcall Stub_NtDeviceIoControlFile(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6, std::uint32_t a7, std::uint32_t a8, std::uint32_t a9, std::uint32_t a10) noexcept {
    const std::uint32_t args[10] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10};
    return ReportUnimplemented(41, args, 10, _ReturnAddress());
}

// slot 42, Ord 172: 2 Args (Base, Size). VirtualFree
std::uint32_t __stdcall Stub_MmFreeSystemMemory(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(42, args, 2, _ReturnAddress());
}

// slot 43, Ord 202: 6 Args wie NT; gleiche Pfaduebersetzung wie NtCreateFile
std::uint32_t __stdcall Stub_NtOpenFile(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6) noexcept {
    const std::uint32_t args[6] = {a1, a2, a3, a4, a5, a6};
    return ReportUnimplemented(43, args, 6, _ReturnAddress());
}

// slot 44, Ord 167: 2 Args (Size, Protect=4). XAPI FUN_000f786b (Kopierroutine). VirtualAlloc
std::uint32_t __stdcall Stub_MmAllocateSystemMemory(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(44, args, 2, _ReturnAddress());
}

// slot 45, Ord 210: 2 Args -> GetFileAttributesEx/ntdll (FILE_NETWORK_OPEN_INFORMATION) mit Pfaduebersetzung
std::uint32_t __stdcall Stub_NtQueryFullAttributesFile(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(45, args, 2, _ReturnAddress());
}

// slot 46, Ord 207: 10 Args, Klasse 1 (FileDirectoryInformation), FileMask als ANSI_STRING. FindFirstFile/NextFile -> Xbox-Struktur mit ANSI-Namen (XFindFirstSaveGame u.a.)
std::uint32_t __stdcall Stub_NtQueryDirectoryFile(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6, std::uint32_t a7, std::uint32_t a8, std::uint32_t a9, std::uint32_t a10) noexcept {
    const std::uint32_t args[10] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10};
    return ReportUnimplemented(46, args, 10, _ReturnAddress());
}

// slot 47, Ord 40: DWORD, gelesen in XapiSelectCachePartition. Anzahl Cache-Partitionen bereitstellen (Vermutung: 3), Z: auf Host-Verzeichnis
alignas(16) unsigned char Placeholder_HalDiskCachePartitionCount[kDataPlaceholderSize];

// slot 48, Ord 67: XAPI (XapiInitProcess, XMountUtilityDrive, XapiMapLetterToDirectory): Laufwerksbuchstabe -> Geraetepfad in eigene Symlink-Tabelle
std::uint32_t __stdcall Stub_IoCreateSymbolicLink(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(48, args, 2, _ReturnAddress());
}

// slot 49, Ord 215: Nur XLaunchNewImageA (Ziel von D:). Aus Symlink-Tabelle beantworten
std::uint32_t __stdcall Stub_NtQuerySymbolicLinkObject(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(49, args, 3, _ReturnAddress());
}

// slot 50, Ord 203: Nur XLaunchNewImageA. Pseudo-Handle auf Eintrag der Symlink-Tabelle
std::uint32_t __stdcall Stub_NtOpenSymbolicLinkObject(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(50, args, 2, _ReturnAddress());
}

// slot 51, Ord 178: No-op (nur fuer Reboot/Imagewechsel). XAPI FUN_000f81bb, D3D
std::uint32_t __stdcall Stub_MmPersistContiguousMemory(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(51, args, 3, _ReturnAddress());
}

// slot 52, Ord 165: 1 Arg -> VirtualAlloc (RW, Seitengranular), Groesse merken. XAPI FUN_000f81bb (LaunchDataPage), XPP
std::uint32_t __stdcall Stub_MmAllocateContiguousMemory(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(52, args, 1, _ReturnAddress());
}

// slot 53, Ord 164: Zeigervariable (PLAUNCH_DATA_PAGE*): XAPI liest/schreibt *slot (FUN_000f81bb setzt neue Seite, XapiInitProcess prueft und nullt). Beschreibbares DWORD = NULL
alignas(16) unsigned char Placeholder_LaunchDataPage[kDataPlaceholderSize];

// slot 54, Ord 171: 1 Arg -> VirtualFree. Auch Spielcode: ueber XAPILIB::XPhysicalFree (6-Byte-Stub 0xf8463, FUN_000ee080) und D3D-Stub 0x2417b0 (sub_161180, vtable 0x2aaf08)
std::uint32_t __stdcall Stub_MmFreeContiguousMemory(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(54, args, 1, _ReturnAddress());
}

// slot 55, Ord 49: XapiBootToDash (Arg 4), XLaunchNewImage-Pfad FUN_000f829c (Arg 2): Prozess beenden bzw. Spiel neu starten. Review: Arg 4 nur, wenn die eigene TitleID 0xFFFE0000 (Dashboard) ist (0x0F8FD8); fuer Futurama (TitleID 0x46490002) laeuft XapiBootToDash ueber XLaunchNewImageA -> FUN_000f829c -> Arg 2. Das Spiel ruft XLaunchNewImageA selbst auf (FUN_000e4b00)
std::uint32_t __stdcall Stub_HalReturnToFirmware(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(55, args, 1, _ReturnAddress());
}

// slot 56, Ord 166: 5 Args (Size, Lowest, Highest, Alignment, Protect). D3D: Highest 0x7FFFFFF, Alignment bis 0x4000, Protect 0x404 = RW+WRITECOMBINE (eine D3D-Site 0x239D37: Protect 4); XPhysicalAlloc 0xF8449 reicht Aufruferwerte durch (alle Aufrufer: PhysicalAddress -1 -> Lowest 0/Highest -1; Spiel FUN_000ee220 Protect 4). VirtualAlloc (64-KiB-Granularitaet deckt Alignment), WRITECOMBINE ignorieren. Spiel nutzt es ueber XAPILIB::XPhysicalAlloc
std::uint32_t __stdcall Stub_MmAllocateContiguousMemoryEx(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5) noexcept {
    const std::uint32_t args[5] = {a1, a2, a3, a4, a5};
    return ReportUnimplemented(56, args, 5, _ReturnAddress());
}

// slot 57, Ord 180: Groesse aus eigener Allokationstabelle (Fallback VirtualQuery). Nur D3D/DSOUND
std::uint32_t __stdcall Stub_MmQueryAllocationSize(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(57, args, 1, _ReturnAddress());
}

// slot 58, Ord 182: 3 Args -> VirtualProtect. XAPI FUN_000f9804 nur bei bestimmten Kernel-Builds, XPP FUN_0028eaa7 (Protect 0x20)
std::uint32_t __stdcall Stub_MmSetAddressProtect(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(58, args, 3, _ReturnAddress());
}

// slot 59, Ord 179: VirtualQuery().Protect. XAPI FUN_000f9804 (Kernel-Patch, siehe XboxKrnlVersion), XPP
std::uint32_t __stdcall Stub_MmQueryAddressProtect(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(59, args, 1, _ReturnAddress());
}

// slot 60, Ord 149: Stack: Timer, DueTime (LARGE_INTEGER = 2 DWORDs), Dpc. Host-Timer-Queue; bei Ablauf SignalState=1 und DPC einreihen; Rueckgabe: war schon aktiv. XAPI XAutoPowerDownResetTimer
std::uint32_t __stdcall Stub_KeSetTimer(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4) noexcept {
    const std::uint32_t args[4] = {a1, a2, a3, a4};
    return ReportUnimplemented(60, args, 4, _ReturnAddress());
}

// slot 61, Ord 97: Host-Timer stoppen, Rueckgabe ob aktiv war. Nur DSOUND/XPP
std::uint32_t __stdcall Stub_KeCancelTimer(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(61, args, 1, _ReturnAddress());
}

// slot 62, Ord 250: fastcall ecx=Object, einzige Verwendung Tail-Jump in XPP FUN_0028d656. Stub no-op
std::uint32_t __fastcall Stub_ObfDereferenceObject(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(62, args, 1, _ReturnAddress());
}

// slot 63, Ord 246: Nur XPP FUN_0028d9bf (Event-Handle -> KEVENT*). Bei XPP-HLE Stub
std::uint32_t __stdcall Stub_ObReferenceObjectByHandle(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(63, args, 3, _ReturnAddress());
}

// slot 64, Ord 322: Struktur: Byte +0 (Bits 0x1/0x2/0x8 getestet), +1 Bit 0x2 (D3D), Byte +5 == 0xA1 (XPP); FUN_000f9804 liest DWORD +0. Retail-Werte bereitstellen (Flags=0, Vermutung)
alignas(16) unsigned char Placeholder_XboxHardwareInfo[kDataPlaceholderSize];

// slot 65, Ord 83: Nur XPP (MU-Treiber). Stub
std::uint32_t __stdcall Stub_IoStartPacket(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(65, args, 3, _ReturnAddress());
}

// slot 66, Ord 87: fastcall ecx=Irp, dl=PriorityBoost; nur XPP. Stub
std::uint32_t __fastcall Stub_IofCompleteRequest(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(66, args, 2, _ReturnAddress());
}

// slot 67, Ord 81: Nur XPP. Stub
std::uint32_t __stdcall Stub_IoStartNextPacket(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(67, args, 1, _ReturnAddress());
}

// slot 68, Ord 17: Nur DSOUND/XPP. HeapFree
std::uint32_t __stdcall Stub_ExFreePool(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(68, args, 1, _ReturnAddress());
}

// slot 69, Ord 15: Nur DSOUND-Pool/XPP. HeapAlloc
std::uint32_t __stdcall Stub_ExAllocatePoolWithTag(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(69, args, 2, _ReturnAddress());
}

// slot 70, Ord 359: Nur XPP. Stub
std::uint32_t __stdcall Stub_IoMarkIrpMustComplete(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(70, args, 1, _ReturnAddress());
}

// slot 71, Ord 358: Nur XPP. FALSE
std::uint32_t __stdcall Stub_HalIsResetOrShutdownPending(void) noexcept {
    return ReportUnimplemented(71, nullptr, 0, _ReturnAddress());
}

// slot 72, Ord 344: Nur XAPI FUN_000f902f (mit XePublicKeyData), aufgerufen aus FUN_000f90e6 (DVD-Pruefung); dessen Ergebnis ignoriert XapiInitProcess (0x0F931C test eax,eax; 0x0F931E jmp). TRUE zurueckgeben
std::uint32_t __stdcall Stub_XcVerifyPKCS1Signature(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(72, args, 3, _ReturnAddress());
}

// slot 73, Ord 355: RSA-Public-Key, nur als Argument an XcVerifyPKCS1Signature (FUN_000f902f). Dummy-Puffer
alignas(16) unsigned char Placeholder_XePublicKeyData[kDataPlaceholderSize];

// slot 74, Ord 47: No-op oder Liste fuehren. XAPI registriert 0x2d7530 (Routine sub_0f9c7f), sonst D3D/DSOUND/XPP
std::uint32_t __stdcall Stub_HalRegisterShutdownNotification(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(74, args, 2, _ReturnAddress());
}

// slot 75, Ord 324: {USHORT Major, Minor, Build, Qfe}: FUN_000f9804 liest Build (+4) und patcht bei 0xF68/0xFC2/0xFC7/0x12D1-0x12D2 Kernelcode bei 0x800299A0..0x800398CD (Review: Adresse haengt von Build und XboxHardwareInfo.Flags Bit 1 ab; 0xFC2 mit Bit 1 gesetzt -> kein Patch). Build ausserhalb dieser Werte waehlen -> kein Patch
alignas(16) unsigned char Placeholder_XboxKrnlVersion[kDataPlaceholderSize];

// slot 76, Ord 258: 1 Arg, Ende von XapiThreadStartup (sub_0f99da), danach int3. ExitThread + emulierte Thread-Strukturen freigeben
std::uint32_t __stdcall Stub_PsTerminateSystemThread(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(76, args, 1, _ReturnAddress());
}

// slot 77, Ord 255: 10 Args, einzige Site XAPI CreateThread (StartRoutine 0xf99da = XapiThreadStartup). CreateThread mit Trampolin: pro Thread KPCR/KTHREAD-Ersatz und TLS (0x20 Byte Zero-Fill laut TLS-Directory) bereitstellen, fs:-Zugriffe bedienen, Handle/ThreadId liefern
std::uint32_t __stdcall Stub_PsCreateSystemThreadEx(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6, std::uint32_t a7, std::uint32_t a8, std::uint32_t a9, std::uint32_t a10) noexcept {
    const std::uint32_t args[10] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10};
    return ReportUnimplemented(77, args, 10, _ReturnAddress());
}

// slot 78, Ord 360: 0 Args, nur aus DPC sub_0f9b08 (AutoPowerDown, wenn Flag [0x310388] != 0; Flag = Setting 0x11 Bit 0, Timer 6 h relativ). Loggen und ignorieren
std::uint32_t __stdcall Stub_HalInitiateShutdown(void) noexcept {
    return ReportUnimplemented(78, nullptr, 0, _ReturnAddress());
}

// slot 79, Ord 44: Nur D3D/DSOUND/XPP (Level 3/5/1/6 an den Sites). Stub: Vektor 0, *Irql=0
std::uint32_t __stdcall Stub_HalGetInterruptVector(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(79, args, 2, _ReturnAddress());
}

// slot 80, Ord 160: fastcall cl=NewIrql, Rueckgabe alte IRQL in al; wie KeRaiseIrqlToDpcLevel. Nur DSOUND/XPP -> bei HLE Stub
std::uint32_t __fastcall Stub_KfRaiseIrql(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(80, args, 1, _ReturnAddress());
}

// slot 81, Ord 98: Nur D3D/DSOUND/XPP. Stub: TRUE zurueckgeben, Interrupt feuert nie
std::uint32_t __stdcall Stub_KeConnectInterrupt(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(81, args, 1, _ReturnAddress());
}

// slot 82, Ord 109: Nur D3D/DSOUND/XPP (GPU-, APU-, USB-Interrupt). Stub: KINTERRUPT nur nullen
std::uint32_t __stdcall Stub_KeInitializeInterrupt(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6, std::uint32_t a7) noexcept {
    const std::uint32_t args[7] = {a1, a2, a3, a4, a5, a6, a7};
    return ReportUnimplemented(82, args, 7, _ReturnAddress());
}

// slot 83, Ord 151: Mikrosekunden-Busy-Wait (QueryPerformanceCounter). Nur DSOUND/XPP
std::uint32_t __stdcall Stub_KeStallExecutionProcessor(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(83, args, 1, _ReturnAddress());
}

// slot 84, Ord 279: ntdll!RtlEqualString (ANSI_STRING, CaseInSensitive)
std::uint32_t __stdcall Stub_RtlEqualString(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(84, args, 3, _ReturnAddress());
}

// slot 85, Ord 327: Arg = Zeiger auf XBE-Section-Header (XLoadSectionByHandle). Alle Sektionen ausser $$XTIMAGE/.XTLID sind PRELOAD. Review: XapiInitProcess -> FUN_000f8f75 -> XapiMapLetterToDirectory laedt $$XTIMAGE (String 0x2a5eb8, XLoadSectionByHandle 0x0F8E74) und schreibt TitleImage.xbx -> Rohdaten von $$XTIMAGE muessen an VA 0x3178C0 liegen (Loader mappt sie vorab oder XeLoadSection kopiert sie); dann Referenzzaehler erhoehen, STATUS_SUCCESS
std::uint32_t __stdcall Stub_XeLoadSection(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(85, args, 1, _ReturnAddress());
}

// slot 86, Ord 328: Referenzzaehler senken, STATUS_SUCCESS
std::uint32_t __stdcall Stub_XeUnloadSection(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(86, args, 1, _ReturnAddress());
}

// slot 87, Ord 357: Struktur: XAPI schreibt +0x10/+0x14 (Callbacks 0xf9c7c/0xf9c63), liest/nullt +0x20. Beschreibbarer Nullpuffer (>= 0x24 Byte) -> Shutdown-Routine wartet nicht
alignas(16) unsigned char Placeholder_IdexChannelObject[kDataPlaceholderSize];

// slot 88, Ord 173: Nur DSOUND/XPP (DMA). Stub: Identitaet
std::uint32_t __stdcall Stub_MmGetPhysicalAddress(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(88, args, 1, _ReturnAddress());
}

// slot 89, Ord 175: Nur DSOUND/XPP. No-op
std::uint32_t __stdcall Stub_MmLockUnlockBufferPages(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(89, args, 3, _ReturnAddress());
}

// slot 90, Ord 119: In Host-DPC-Queue einreihen (SystemArgument1/2 merken), TRUE wenn neu. Nur D3D/DSOUND/XPP (Interrupt-DPCs) -> bei HLE Stub
std::uint32_t __stdcall Stub_KeInsertQueueDpc(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(90, args, 3, _ReturnAddress());
}

// slot 91, Ord 176: Nur XPP. No-op
std::uint32_t __stdcall Stub_MmLockUnlockPhysicalPage(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(91, args, 2, _ReturnAddress());
}

// slot 92, Ord 1: Nur D3D. NULL
std::uint32_t __stdcall Stub_AvGetSavedDataAddress(void) noexcept {
    return ReportUnimplemented(92, nullptr, 0, _ReturnAddress());
}

// slot 93, Ord 2: Nur D3D (Optionen 6, 9, 0xB, 0xE, 0xF an den Sites). No-op, *Result=0
std::uint32_t __stdcall Stub_AvSendTVEncoderOption(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4) noexcept {
    const std::uint32_t args[4] = {a1, a2, a3, a4};
    return ReportUnimplemented(93, args, 4, _ReturnAddress());
}

// slot 94, Ord 3: Nur D3D. No-op (Videomodus macht der Host-Renderer)
std::uint32_t __stdcall Stub_AvSetDisplayMode(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6) noexcept {
    const std::uint32_t args[6] = {a1, a2, a3, a4, a5, a6};
    return ReportUnimplemented(94, args, 6, _ReturnAddress());
}

// slot 95, Ord 4: Nur D3D. No-op
std::uint32_t __stdcall Stub_AvSetSavedDataAddress(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(95, args, 1, _ReturnAddress());
}

// slot 96, Ord 46: Nur D3D CMiniport_InitHardware. Stub (Puffer nullen)
std::uint32_t __stdcall Stub_HalReadWritePCISpace(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6) noexcept {
    const std::uint32_t args[6] = {a1, a2, a3, a4, a5, a6};
    return ReportUnimplemented(96, args, 6, _ReturnAddress());
}

// slot 97, Ord 100: Nur D3D/DSOUND (Review: keine XPP-Site). Stub: TRUE
std::uint32_t __stdcall Stub_KeDisconnectInterrupt(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(97, args, 1, _ReturnAddress());
}

// slot 98, Ord 8: cdecl/varargs, einzige Site D3D FUN_0023e600. vsnprintf + OutputDebugStringA
std::uint32_t __cdecl Stub_DbgPrint(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(98, args, 1, _ReturnAddress());
}

// slot 99, Ord 168: 2 Args, nur D3D FUN_0023e7a7. Bei D3D-HLE entbehrlich; sonst Fake-Adresse
std::uint32_t __stdcall Stub_MmClaimGpuInstanceMemory(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(99, args, 2, _ReturnAddress());
}

// slot 100, Ord 142: Nur DSOUND. STATUS_SUCCESS, no-op
std::uint32_t __stdcall Stub_KeSaveFloatingPointState(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(100, args, 1, _ReturnAddress());
}

// slot 101, Ord 139: Nur DSOUND. STATUS_SUCCESS, no-op
std::uint32_t __stdcall Stub_KeRestoreFloatingPointState(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(101, args, 1, _ReturnAddress());
}

// slot 102, Ord 23: Nur DSOUND. HeapSize
std::uint32_t __stdcall Stub_ExQueryPoolBlockSize(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(102, args, 1, _ReturnAddress());
}

// slot 103, Ord 137: Aus Host-DPC-Queue entfernen. Nur DSOUND (Review: keine XPP-Site) -> Stub
std::uint32_t __stdcall Stub_KeRemoveQueueDpc(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(103, args, 1, _ReturnAddress());
}

// slot 104, Ord 153: Nur DSOUND. Stub: SynchronizeRoutine(Context) direkt unter Dispatcher-Lock aufrufen
std::uint32_t __stdcall Stub_KeSynchronizeExecution(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) noexcept {
    const std::uint32_t args[3] = {a1, a2, a3};
    return ReportUnimplemented(104, args, 3, _ReturnAddress());
}

// slot 105, Ord 312: 4 Args, LIBC __global_unwind2/_UnwindNestedFrames -> ntdll!RtlUnwind (fs:[0]-Kette wie oben)
std::uint32_t __stdcall Stub_RtlUnwind(std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4) noexcept {
    const std::uint32_t args[4] = {a1, a2, a3, a4};
    return ReportUnimplemented(105, args, 4, _ReturnAddress());
}

// slot 106, Ord 95: LIBC/XAPI-Fatalpfade (Codes 0, 0xA, 0xC0000144). Loggen + Abbruch
std::uint32_t __stdcall Stub_KeBugCheck(std::uint32_t a1) noexcept {
    const std::uint32_t args[1] = {a1};
    return ReportUnimplemented(106, args, 1, _ReturnAddress());
}

// slot 107, Ord 169: XAPI CreateFiber (Spiel FUN_0013f250 ruft CreateFiber). Stack per VirtualAlloc, Rueckgabe Obergrenze; besser Fiber-API (Create/Switch/Delete/ConvertThreadToFiber) per HLE auf Win32-Fibers
std::uint32_t __stdcall Stub_MmCreateKernelStack(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(107, args, 2, _ReturnAddress());
}

// slot 108, Ord 170: XAPI DeleteFiber. VirtualFree des Stacks (entfaellt bei Fiber-HLE)
std::uint32_t __stdcall Stub_MmDeleteKernelStack(std::uint32_t a1, std::uint32_t a2) noexcept {
    const std::uint32_t args[2] = {a1, a2};
    return ReportUnimplemented(108, args, 2, _ReturnAddress());
}

// slot 109, Ord 5: 0 Args, Tail-Jump aus XAPILIB::DebugBreak. No-op oder __debugbreak
std::uint32_t __stdcall Stub_DbgBreakPoint(void) noexcept {
    return ReportUnimplemented(109, nullptr, 0, _ReturnAddress());
}

// slot 110, Ord 356: DWORD; XAPILIB::XGetAVPack gibt *slot zurueck (AV-Pack). Konfigurierbarer Wert (Konstanten laut XDK, Vermutung)
alignas(16) unsigned char Placeholder_HalBootSMCVideoMode[kDataPlaceholderSize];

}  // namespace

std::uintptr_t StubAddress(std::uint32_t slot) noexcept {
    switch (slot) {
    case 0: return reinterpret_cast<std::uintptr_t>(&Stub_KfLowerIrql);
    case 1: return reinterpret_cast<std::uintptr_t>(&Stub_KeRaiseIrqlToDpcLevel);
    case 2: return reinterpret_cast<std::uintptr_t>(&Stub_IoCreateDevice);
    case 3: return reinterpret_cast<std::uintptr_t>(&Stub_RtlInitAnsiString);
    case 4: return reinterpret_cast<std::uintptr_t>(&Stub_KeSetEvent);
    case 5: return reinterpret_cast<std::uintptr_t>(&Stub_KeWaitForSingleObject);
    case 6: return reinterpret_cast<std::uintptr_t>(&Stub_KeInitializeTimerEx);
    case 7: return reinterpret_cast<std::uintptr_t>(&Stub_KeInitializeDpc);
    case 8: return reinterpret_cast<std::uintptr_t>(&AddrPlaceholder_IoInvalidDeviceRequest);
    case 9: return reinterpret_cast<std::uintptr_t>(&Stub_RtlLeaveCriticalSection);
    case 10: return reinterpret_cast<std::uintptr_t>(&Stub_RtlEnterCriticalSection);
    case 11: return reinterpret_cast<std::uintptr_t>(&Stub_NtFreeVirtualMemory);
    case 12: return reinterpret_cast<std::uintptr_t>(&Stub_NtAllocateVirtualMemory);
    case 13: return reinterpret_cast<std::uintptr_t>(&Stub_RtlCompareMemoryUlong);
    case 14: return reinterpret_cast<std::uintptr_t>(&Stub_RtlInitializeCriticalSection);
    case 15: return reinterpret_cast<std::uintptr_t>(&Stub_NtQueryVirtualMemory);
    case 16: return reinterpret_cast<std::uintptr_t>(&Stub_RtlRaiseException);
    case 17: return reinterpret_cast<std::uintptr_t>(&Stub_ExQueryNonVolatileSetting);
    case 18: return reinterpret_cast<std::uintptr_t>(&Placeholder_ExEventObjectType[0]);
    case 19: return reinterpret_cast<std::uintptr_t>(&Stub_NtSetEvent);
    case 20: return reinterpret_cast<std::uintptr_t>(&Stub_NtWaitForSingleObjectEx);
    case 21: return reinterpret_cast<std::uintptr_t>(&Stub_KeDelayExecutionThread);
    case 22: return reinterpret_cast<std::uintptr_t>(&Stub_NtSetInformationFile);
    case 23: return reinterpret_cast<std::uintptr_t>(&Stub_KeQuerySystemTime);
    case 24: return reinterpret_cast<std::uintptr_t>(&Stub_RtlNtStatusToDosError);
    case 25: return reinterpret_cast<std::uintptr_t>(&Stub_NtCreateFile);
    case 26: return reinterpret_cast<std::uintptr_t>(&Stub_NtClose);
    case 27: return reinterpret_cast<std::uintptr_t>(&Stub_XcSHAUpdate);
    case 28: return reinterpret_cast<std::uintptr_t>(&Stub_XcSHAInit);
    case 29: return reinterpret_cast<std::uintptr_t>(&Stub_XcSHAFinal);
    case 30: return reinterpret_cast<std::uintptr_t>(&Placeholder_XboxHDKey[0]);
    case 31: return reinterpret_cast<std::uintptr_t>(&Placeholder_XboxSignatureKey[0]);
    case 32: return reinterpret_cast<std::uintptr_t>(&Placeholder_XboxAlternateSignatureKeys[0]);
    case 33: return reinterpret_cast<std::uintptr_t>(&Stub_NtWaitForSingleObject);
    case 34: return reinterpret_cast<std::uintptr_t>(&Stub_NtReadFile);
    case 35: return reinterpret_cast<std::uintptr_t>(&Stub_NtWriteFile);
    case 36: return reinterpret_cast<std::uintptr_t>(&Stub_NtQueryInformationFile);
    case 37: return reinterpret_cast<std::uintptr_t>(&Stub_NtQueryVolumeInformationFile);
    case 38: return reinterpret_cast<std::uintptr_t>(&Stub_NtFlushBuffersFile);
    case 39: return reinterpret_cast<std::uintptr_t>(&AddrPlaceholder_NtUserIoApcDispatcher);
    case 40: return reinterpret_cast<std::uintptr_t>(&Stub_NtFsControlFile);
    case 41: return reinterpret_cast<std::uintptr_t>(&Stub_NtDeviceIoControlFile);
    case 42: return reinterpret_cast<std::uintptr_t>(&Stub_MmFreeSystemMemory);
    case 43: return reinterpret_cast<std::uintptr_t>(&Stub_NtOpenFile);
    case 44: return reinterpret_cast<std::uintptr_t>(&Stub_MmAllocateSystemMemory);
    case 45: return reinterpret_cast<std::uintptr_t>(&Stub_NtQueryFullAttributesFile);
    case 46: return reinterpret_cast<std::uintptr_t>(&Stub_NtQueryDirectoryFile);
    case 47: return reinterpret_cast<std::uintptr_t>(&Placeholder_HalDiskCachePartitionCount[0]);
    case 48: return reinterpret_cast<std::uintptr_t>(&Stub_IoCreateSymbolicLink);
    case 49: return reinterpret_cast<std::uintptr_t>(&Stub_NtQuerySymbolicLinkObject);
    case 50: return reinterpret_cast<std::uintptr_t>(&Stub_NtOpenSymbolicLinkObject);
    case 51: return reinterpret_cast<std::uintptr_t>(&Stub_MmPersistContiguousMemory);
    case 52: return reinterpret_cast<std::uintptr_t>(&Stub_MmAllocateContiguousMemory);
    case 53: return reinterpret_cast<std::uintptr_t>(&Placeholder_LaunchDataPage[0]);
    case 54: return reinterpret_cast<std::uintptr_t>(&Stub_MmFreeContiguousMemory);
    case 55: return reinterpret_cast<std::uintptr_t>(&Stub_HalReturnToFirmware);
    case 56: return reinterpret_cast<std::uintptr_t>(&Stub_MmAllocateContiguousMemoryEx);
    case 57: return reinterpret_cast<std::uintptr_t>(&Stub_MmQueryAllocationSize);
    case 58: return reinterpret_cast<std::uintptr_t>(&Stub_MmSetAddressProtect);
    case 59: return reinterpret_cast<std::uintptr_t>(&Stub_MmQueryAddressProtect);
    case 60: return reinterpret_cast<std::uintptr_t>(&Stub_KeSetTimer);
    case 61: return reinterpret_cast<std::uintptr_t>(&Stub_KeCancelTimer);
    case 62: return reinterpret_cast<std::uintptr_t>(&Stub_ObfDereferenceObject);
    case 63: return reinterpret_cast<std::uintptr_t>(&Stub_ObReferenceObjectByHandle);
    case 64: return reinterpret_cast<std::uintptr_t>(&Placeholder_XboxHardwareInfo[0]);
    case 65: return reinterpret_cast<std::uintptr_t>(&Stub_IoStartPacket);
    case 66: return reinterpret_cast<std::uintptr_t>(&Stub_IofCompleteRequest);
    case 67: return reinterpret_cast<std::uintptr_t>(&Stub_IoStartNextPacket);
    case 68: return reinterpret_cast<std::uintptr_t>(&Stub_ExFreePool);
    case 69: return reinterpret_cast<std::uintptr_t>(&Stub_ExAllocatePoolWithTag);
    case 70: return reinterpret_cast<std::uintptr_t>(&Stub_IoMarkIrpMustComplete);
    case 71: return reinterpret_cast<std::uintptr_t>(&Stub_HalIsResetOrShutdownPending);
    case 72: return reinterpret_cast<std::uintptr_t>(&Stub_XcVerifyPKCS1Signature);
    case 73: return reinterpret_cast<std::uintptr_t>(&Placeholder_XePublicKeyData[0]);
    case 74: return reinterpret_cast<std::uintptr_t>(&Stub_HalRegisterShutdownNotification);
    case 75: return reinterpret_cast<std::uintptr_t>(&Placeholder_XboxKrnlVersion[0]);
    case 76: return reinterpret_cast<std::uintptr_t>(&Stub_PsTerminateSystemThread);
    case 77: return reinterpret_cast<std::uintptr_t>(&Stub_PsCreateSystemThreadEx);
    case 78: return reinterpret_cast<std::uintptr_t>(&Stub_HalInitiateShutdown);
    case 79: return reinterpret_cast<std::uintptr_t>(&Stub_HalGetInterruptVector);
    case 80: return reinterpret_cast<std::uintptr_t>(&Stub_KfRaiseIrql);
    case 81: return reinterpret_cast<std::uintptr_t>(&Stub_KeConnectInterrupt);
    case 82: return reinterpret_cast<std::uintptr_t>(&Stub_KeInitializeInterrupt);
    case 83: return reinterpret_cast<std::uintptr_t>(&Stub_KeStallExecutionProcessor);
    case 84: return reinterpret_cast<std::uintptr_t>(&Stub_RtlEqualString);
    case 85: return reinterpret_cast<std::uintptr_t>(&Stub_XeLoadSection);
    case 86: return reinterpret_cast<std::uintptr_t>(&Stub_XeUnloadSection);
    case 87: return reinterpret_cast<std::uintptr_t>(&Placeholder_IdexChannelObject[0]);
    case 88: return reinterpret_cast<std::uintptr_t>(&Stub_MmGetPhysicalAddress);
    case 89: return reinterpret_cast<std::uintptr_t>(&Stub_MmLockUnlockBufferPages);
    case 90: return reinterpret_cast<std::uintptr_t>(&Stub_KeInsertQueueDpc);
    case 91: return reinterpret_cast<std::uintptr_t>(&Stub_MmLockUnlockPhysicalPage);
    case 92: return reinterpret_cast<std::uintptr_t>(&Stub_AvGetSavedDataAddress);
    case 93: return reinterpret_cast<std::uintptr_t>(&Stub_AvSendTVEncoderOption);
    case 94: return reinterpret_cast<std::uintptr_t>(&Stub_AvSetDisplayMode);
    case 95: return reinterpret_cast<std::uintptr_t>(&Stub_AvSetSavedDataAddress);
    case 96: return reinterpret_cast<std::uintptr_t>(&Stub_HalReadWritePCISpace);
    case 97: return reinterpret_cast<std::uintptr_t>(&Stub_KeDisconnectInterrupt);
    case 98: return reinterpret_cast<std::uintptr_t>(&Stub_DbgPrint);
    case 99: return reinterpret_cast<std::uintptr_t>(&Stub_MmClaimGpuInstanceMemory);
    case 100: return reinterpret_cast<std::uintptr_t>(&Stub_KeSaveFloatingPointState);
    case 101: return reinterpret_cast<std::uintptr_t>(&Stub_KeRestoreFloatingPointState);
    case 102: return reinterpret_cast<std::uintptr_t>(&Stub_ExQueryPoolBlockSize);
    case 103: return reinterpret_cast<std::uintptr_t>(&Stub_KeRemoveQueueDpc);
    case 104: return reinterpret_cast<std::uintptr_t>(&Stub_KeSynchronizeExecution);
    case 105: return reinterpret_cast<std::uintptr_t>(&Stub_RtlUnwind);
    case 106: return reinterpret_cast<std::uintptr_t>(&Stub_KeBugCheck);
    case 107: return reinterpret_cast<std::uintptr_t>(&Stub_MmCreateKernelStack);
    case 108: return reinterpret_cast<std::uintptr_t>(&Stub_MmDeleteKernelStack);
    case 109: return reinterpret_cast<std::uintptr_t>(&Stub_DbgBreakPoint);
    case 110: return reinterpret_cast<std::uintptr_t>(&Placeholder_HalBootSMCVideoMode[0]);
    default: return 0;
    }
}

}  // namespace krnl::gen
