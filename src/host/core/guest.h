// Guest (XBE) address constants and guest-memory helpers.
//
// RULE (design 5.3): guest addresses appear in code only through the named constants below (or
// FUT_HOOK/FUT_GLOBAL), never as bare numbers. Add new constants here (shared file: append only and
// note it in the Changelog of src/INTERFACES.md), each with its evidence.
//
// Guest memory is ordinary host memory in the same 32-bit address space: a guest address is a host
// pointer. The helpers below only add intent and type safety.
#pragma once

#include "shared/launcher_info.h"
#include "shared/xbox_types.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace guest {

using GuestAddr = std::uint32_t;

// ---------------------------------------------------------------------------------------------
// Image layout (design 1.2, 1.4; XBE header of orig/retail_eu/default.xbe)
// ---------------------------------------------------------------------------------------------
inline constexpr GuestAddr kXbeBase = 0x00010000;           // header page (XBE header + PE polyglot)
inline constexpr std::uint32_t kXbeSizeOfHeaders = 0x968;   // header bytes copied to 0x10000
inline constexpr std::uint32_t kXbeSizeOfImage = 0x30A300;  // header field 0x10C
inline constexpr GuestAddr kXbeImageEnd = kXbeBase + kXbeSizeOfImage;  // 0x31A300
inline constexpr GuestAddr kXbeSectionsBegin = FUT_XBE_SPACE_ADDRESS;  // 0x11000 (.text)
inline constexpr GuestAddr kXbeSectionsEnd = 0x0031A2F8;               // end of .XTLID (last section)
inline constexpr GuestAddr kXbeSpaceEnd = FUT_XBE_SPACE_ADDRESS + FUT_XBE_SPACE_SIZE;  // 0x31B000
// VirtualProtect(0x10000, kXbeProtectSize, ...) (design 1.4 step 3)
inline constexpr std::uint32_t kXbeProtectSize = 0x30B000;
inline constexpr std::uint32_t kXbeSectionCount = 10;
inline constexpr std::uint32_t kXbeFileSize = 2973696;
inline constexpr char kXbeSha1[] = "58f76de5bc739bf016d74c1d7f23e348f65ea4b4";  // design 1.4 step 1
inline constexpr GuestAddr kGuestMemoryLimit = 0x80000000;  // HLE allocations must stay below (design 1.2)

// PE/XBE polyglot (design 1.4 step 5, E2): 'MZ' at 0x10000, e_lfanew at 0x1003C -> NT headers copy
// at 0x10970 (free space 0x10968..0x10FFF). The guest reads the header only from 0x10108 on.
inline constexpr GuestAddr kPolyglotMzAddr = 0x00010000;
inline constexpr GuestAddr kPolyglotLfanewAddr = 0x0001003C;
inline constexpr GuestAddr kPolyglotNtHeaders = 0x00010970;
inline constexpr GuestAddr kPolyglotFreeEnd = 0x00011000;
inline constexpr GuestAddr kFirstGuestReadHeaderAddr = 0x00010108;  // design_header_refs.out

// XBE header fields read by guest code (design 1.4 step 5 evidence)
inline constexpr GuestAddr kHdrSizeOfHeaders = 0x00010108;
inline constexpr GuestAddr kHdrCertificateAddress = 0x00010118;
inline constexpr GuestAddr kHdrInitFlags = 0x00010124;
inline constexpr GuestAddr kHdrPeStackCommit = 0x00010130;  // CreateThread/CreateFiber default stack
inline constexpr GuestAddr kHdrPeHeapReserve = 0x00010134;
inline constexpr GuestAddr kHdrPeHeapCommit = 0x00010138;
inline constexpr GuestAddr kCertificate = 0x00010180;
inline constexpr GuestAddr kSectionHeaders = 0x0001036C;  // 10 x xbe::SectionHeader

// Section layout (design 1.4 step 4 table) - for validation/self-tests.
struct SectionLayout {
    const char* name;
    GuestAddr va;
    std::uint32_t virtualSize;
    std::uint32_t rawSize;
};
inline constexpr SectionLayout kSections[kXbeSectionCount] = {
    {".text", 0x011000, 0x2232C0, 0x2232C0}, {"D3D", 0x2342C0, 0x12A38, 0xF5D0},
    {"DSOUND", 0x246D00, 0x1D654, 0x1D40C},  {"XMV", 0x264360, 0x27B6C, 0x27B5C},
    {"XPP", 0x28BEE0, 0x767C, 0x767C},       {".rdata", 0x293560, 0x2A97C, 0x2A96C},
    {".data", 0x2BDEE0, 0x52858, 0x1B558},   {"DOLBY", 0x310740, 0x7180, 0x716C},
    {"$$XTIMAGE", 0x3178C0, 0x2800, 0x2800}, {".XTLID", 0x31A0C0, 0x238, 0x238},
};

// ---------------------------------------------------------------------------------------------
// Kernel thunk table (kernel.md 1; design 1.5)
// ---------------------------------------------------------------------------------------------
inline constexpr GuestAddr kKernelThunkTable = 0x00293560;
inline constexpr GuestAddr kKernelThunkTerminator = 0x0029371C;
inline constexpr std::uint32_t kKernelThunkCount = 111;
inline constexpr std::uint32_t kKernelThunkOrdinalFlag = 0x80000000;

// ---------------------------------------------------------------------------------------------
// Boot path (startup.md 1, design 1.10)
// ---------------------------------------------------------------------------------------------
inline constexpr GuestAddr kEntryPoint = 0x000F84DD;         // entry (XOR-decoded header field)
inline constexpr GuestAddr kMainThreadRoutine = 0x000F8469;  // StartContext1 of the main thread
inline constexpr GuestAddr kXapiThreadStartup = 0x000F99DA;  // SystemRoutine of PsCreateSystemThreadEx
inline constexpr GuestAddr kXapiCreateThread = 0x000F9A72;
inline constexpr GuestAddr kXapiKernelHotfix = 0x000F9804;  // FUN_000f9804 (XboxKrnlVersion check)
inline constexpr GuestAddr kXapiInitProcess = 0x000F92C0;
inline constexpr GuestAddr kXapiRtInit = 0x000F97DB;  // _rtinit
inline constexpr GuestAddr kXapiCInit = 0x000F9783;   // _cinit
inline constexpr GuestAddr kGameMain = 0x000D29B0;    // main (cdecl, 3 args), --stop-at=main hook
inline constexpr GuestAddr kXapiBootToDash = 0x000F8FCA;
inline constexpr GuestAddr kXapiUnhandledExceptionFilter = 0x000F999E;
inline constexpr GuestAddr kXapiRebootRoutine = 0x000F829C;  // FUN_000f829c -> HalReturnToFirmware

// TLS (cpu.md 2.2, startup.md 6)
inline constexpr GuestAddr kTlsDirectory = 0x002A5D2C;
inline constexpr GuestAddr kTlsIndexVar = 0x002F3D18;       // _tls_index, entry writes -9
inline constexpr GuestAddr kTlsDataSizeVar = 0x00310394;    // TlsDataSize, entry writes 0x24
inline constexpr GuestAddr kCrtGlobalPtdSlot = 0x00310734;  // _getptd fallback when KTHREAD.TlsData == 0

// ---------------------------------------------------------------------------------------------
// XAPI functions (design 2.1, Anhang B; addresses from functions.tsv)
// ---------------------------------------------------------------------------------------------
inline constexpr GuestAddr kXapiQueryPerformanceCounter = 0x000F641B;    // stdcall 1, rdtsc
inline constexpr GuestAddr kXapiQueryPerformanceFrequency = 0x000F642C;  // stdcall 1, 733333333
inline constexpr GuestAddr kXapiOutputDebugStringA = 0x000F3DE0;
inline constexpr GuestAddr kXapiXMountUtilityDrive = 0x000F7F7C;  // stdcall 1
inline constexpr GuestAddr kXapiXLaunchNewImageA = 0x000F82E3;
inline constexpr GuestAddr kXapiSwitchToFiber = 0x0017094F;  // 63 bytes (M2, MASM replacement)
inline constexpr GuestAddr kXapiCreateFiber = 0x001708B0;
inline constexpr GuestAddr kXapiDeleteFiber = 0x0017093C;
inline constexpr GuestAddr kXapiConvertThreadToFiber = 0x0017098E;
inline constexpr GuestAddr kXapiFiberStartRoutine = 0x0017086B;
inline constexpr GuestAddr kXapiIsBadReadPtr = 0x001709C9;  // stdcall 2, self-test (design 1.4.8)
inline constexpr GuestAddr kXapiReadFileEx = 0x000F7603;
inline constexpr GuestAddr kXapiCreateFileA = 0x000F76E0;
// (added by io, M2) WriteFileEx 0xF7654: stdcall 5 (h, buf, len, lpOverlapped, completion), ret 0x14,
// NtWriteFile(h, 0, [slot 39] NtUserIoApcDispatcher, completion, ovl, buf, len, &ovl.Offset) at
// 0xF7676..0xF7681 (files.md 2.2). SleepEx 0xF64BE: stdcall 2 (ms, alertable), ret 8,
// KeDelayExecutionThread(1 = UserMode, alertable, &interval) at 0xF64E3..0xF64E9, returns 0xC0
// (WAIT_IO_COMPLETION) after APCs (startup.md 4.3). Used by the io self-test GuestReadFileExApc.
inline constexpr GuestAddr kXapiWriteFileEx = 0x000F7654;
inline constexpr GuestAddr kXapiSleepEx = 0x000F64BE;
inline constexpr GuestAddr kXapiProcessHeapVar = 0x0031039C;  // XapiProcessHeap

// CRT / SEH (cpu.md 2.1/2.5)
inline constexpr GuestAddr kCrtExceptHandler3 = 0x00168DCC;   // _except_handler3
inline constexpr GuestAddr kCrtCxxFrameHandler = 0x00166977;  // ___CxxFrameHandler

// Data used by XapiInitProcess / AutoPowerDown (kernel.md 8.6/8.7)
inline constexpr GuestAddr kShutdownNotificationRecord = 0x002D7530;
inline constexpr GuestAddr kAutoPowerDownDpc = 0x00310340;
inline constexpr GuestAddr kAutoPowerDownTimer = 0x00310360;
inline constexpr GuestAddr kAutoPowerDownDpcRoutine = 0x000F9B08;

// Game data (startup.md 2.5)
inline constexpr GuestAddr kGameAppPtr = 0x002EB310;  // g_pApp (CApp*)

// Renderer (design 2.2; M2)
inline constexpr GuestAddr kNiXBoxRendererCreate = 0x0014C170;

// --- added by the core agent (phase 2, see Changelog in src/INTERFACES.md) ---------------------
// Code range of the XBE: .text, D3D, DSOUND, XMV, XPP (flags 0x16/0x07; .rdata starts at 0x293560).
// Used by crash diagnostics to recognise guest return addresses (design 1.11).
inline constexpr GuestAddr kXbeCodeBegin = 0x00011000;
inline constexpr GuestAddr kXbeCodeEnd = 0x00293560;
// XAPI functions called by the AdoptedThread self-tests (functions.tsv; fs-patched, cpu.md 2.3).
inline constexpr GuestAddr kXapiGetLastError = 0x000F6E54;        // stdcall 0, reads [TLS+4] via fs:[4]
inline constexpr GuestAddr kXapiSetLastError = 0x000F6E7C;        // stdcall 1, writes [TLS+4] via fs:[4]
inline constexpr GuestAddr kXapiGetCurrentThreadId = 0x000F9AE0;  // stdcall 0, [fs:[0x28]+0x12C]
// MMIO trap reservation (design 1.2 debug option, cpu.md 4): NV2A 0xFD000000, APU 0xFE800000,
// AC97 0xFEC00000, OHCI 0xFED00000 - reserved PAGE_NOACCESS so that a stray access faults clearly.
inline constexpr GuestAddr kMmioTrapBase = 0xFD000000;
inline constexpr std::uint32_t kMmioTrapSize = 0x02000000;  // up to 0xFEFFFFFF

// --- added in phase 3 (integration, M1 boot probes; see Changelog in src/INTERFACES.md) ---------
// XGetDiskClusterSizeA (FUN_000f8081, stdcall 1, ret 4): SectorsPerAllocationUnit * BytesPerSector
// of FileFsSizeInformation, 0 on error (files.md 2.3; name is an assumption there, "V").
inline constexpr GuestAddr kXapiXGetDiskClusterSizeA = 0x000F8081;
// C++ initializer 0x20EFC0 stores XGetDiskClusterSizeA("U:\") here; the next initializers divide by
// it (0x20EFDD idiv) - files.md 5.5 / Verifikation 14, design 4.5.
inline constexpr GuestAddr kGameSaveClusterSizeVar = 0x002EDB0C;
// memory.cpp game pool (startup.md 5.2): object 0x2E7D60 (+0x0C begin, +0x10 end), lazy init
// FUN_000a4920 (thiscall, 1 stack arg = size 0x2600000, ret 4) called from FUN_000a4520 0xA4547.
inline constexpr GuestAddr kGameMemoryPool = 0x002E7D60;
inline constexpr GuestAddr kGameMemoryPoolInit = 0x000A4920;
inline constexpr std::uint32_t kGameMemoryPoolBeginOffset = 0x0C;
inline constexpr std::uint32_t kGameMemoryPoolEndOffset = 0x10;
inline constexpr std::uint32_t kGameMemoryPoolSize = 0x02600000;  // 38 MiB, push at 0xA4530

// --- added by the XDK agent (M2, DSOUND/XMV/XPP stubs; see Changelog in src/INTERFACES.md) ------
// The 54 XDK entry addresses themselves are generated (gen/xdk_entries.gen.inc, audio/xdk_entries.h).
// XPP device type descriptors; the game only passes their addresses (avi.md 4.1/4.3): Gamepad to
// XInitDevices (0x9C2C3), XGetDevices (0x9C31C), XGetDeviceChanges (0x9C150) and XInputOpen
// (0x9C1E4/0x9C37E); MemoryUnit once to XGetDevices (0x9C32D, result discarded).
inline constexpr GuestAddr kXppDeviceTypeGamepad = 0x0028BF10;
inline constexpr GuestAddr kXppDeviceTypeMemoryUnit = 0x0028BFC0;
// CDirectSoundStream vtable (XMediaObject layout AddRef, Release, GetInfo, GetStatus, Process,
// Discontinuity, Flush; constructor 0x248C91 stores it at +0; avi.md 2.2 / Verifikation Runde 2 #3).
inline constexpr GuestAddr kDsoundStreamVtable = 0x002A5F5C;

// --- added by the gfx agent (M2, NiPCRenderer null renderer + D3D shim; see Changelog) ----------
// NiXBoxRenderer (renderer.md 1.1/1.4/2, nixboxrenderer_vtable.tsv): vtable 75 slots, object 0x6D0,
// ms_RTTI (slot 1 GetRTTI 0x14B090 returns it). The 79 D3D trap addresses are generated
// (gen/d3d_traps.gen.inc), the 75 slot rows too (gen/renderer_slots.gen.inc).
inline constexpr GuestAddr kNiXBoxRendererVtable = 0x002AA7C0;
inline constexpr GuestAddr kNiXBoxRendererRtti = 0x0030EE00;
inline constexpr std::uint32_t kNiXBoxRendererSize = 0x6D0;  // push 0x6D0 at 0x14C1B6
// NiRenderer base ctor/dtor (thiscall ecx = this, no stack args, plain ret; disassembly 0x14AF18
// `mov esi,ecx` + 0x14AF1F `call 0x140550` without pushes; 0x1405EF / 0x140746 `ret`). The ctor
// returns this and appends the renderer to the global renderer list; the dtor removes it, releases
// the accumulator at +8 and runs the NiObject dtor (renderer.md 1.2, Verifikation 7).
inline constexpr GuestAddr kNiRendererBaseCtor = 0x00140550;
inline constexpr GuestAddr kNiRendererBaseDtor = 0x001406E0;
// Renderer list object (+0 count, +4 head, +8 tail; node {renderer, next, prev}) used by the purge /
// create broadcasts FUN_00117FC0, FUN_00110BD0, FUN_00111D70, FUN_00140160..FUN_00140250.
inline constexpr GuestAddr kNiRendererList = 0x002D7B18;
// Game operator new / delete of memory.cpp (cdecl: `push 0x6D0; call 0xA4900` at 0x14C1B6, size at
// [esp+4], plain ret; delete 0xA3880 `push esi; call; add esp,4` in NiRenderer::vf00 0x14075F).
inline constexpr GuestAddr kGameOperatorNew = 0x000A4900;
inline constexpr GuestAddr kGameOperatorDelete = 0x000A3880;
// Screen-centering helpers called by NiXBoxRenderer::vf15 (SetRenderTarget): back buffer -> user
// offset FUN_000A1930 (SetScreenSpaceOffset([0x2E7928], [0x2E792C])), texture -> FUN_000A1920
// (offset 0,0). Both cdecl/no arguments, plain ret (renderer.md 7.1.3, d3d.md 4.3).
inline constexpr GuestAddr kGameScreenOffsetUser = 0x000A1930;
inline constexpr GuestAddr kGameScreenOffsetZero = 0x000A1920;
// NiImageConverter singleton (static initializer, initstubs_00230000.c) and its vf2 ReadImageFile
// (thiscall 2 args: path, 0 -> NiPixelData* with refcount 0) as used by FUN_0015E010 0x15E06A.
inline constexpr GuestAddr kNiImageConverterPtr = 0x0030EF64;
// --- added in M3 phase 5 (review fixes; see Changelog in src/INTERFACES.md) ----------------------
// NiPixelData "free the pixel buffer" FUN_001511B0 (thiscall ecx = NiPixelData, no stack args, plain ret
// 0x1511C7): FUN_000A3880([+0x30]), then +0x30 = 0. FUN_0015E010 calls it at 0x15E1DD after the upload
// (M3 review finding 1).
inline constexpr GuestAddr kNiPixelDataFreePixels = 0x001511B0;
// NiLight::ms_RTTI (retail_eu_ni_classes.json); NiXBoxRenderer::vf60 0x146E57 walks the RTTI chain
// (GetRTTI vf1, base RTTI at +8) looking for it (M3 review finding 4).
inline constexpr GuestAddr kNiLightRtti = 0x0030EBAC;
// memory.cpp game pool counters (pool object kGameMemoryPool below): +0x00 bytes in use, +0x04 bytes free
// (FUN_000A4520 0xA4682 adds/subtracts size + 4; FUN_000A3880 the reverse), DAT_002E8D38 peak bytes in
// use (0xA4697), DAT_002E8D35 "pool initialized" (FUN_000A4920 sets it, FUN_000A4520 tests it).
inline constexpr std::uint32_t kGameMemoryPoolUsedOffset = 0x00;
inline constexpr std::uint32_t kGameMemoryPoolFreeOffset = 0x04;
inline constexpr GuestAddr kGameMemoryPoolPeak = 0x002E8D38;
inline constexpr GuestAddr kGameMemoryPoolInitialized = 0x002E8D35;
// D3D8 functions the game calls directly (design 2.2 shim table, d3d.md 3.1/3.2/4.3; ret per
// d3d_functions.tsv). All stdcall.
inline constexpr GuestAddr kD3DDeviceAddRef = 0x00234440;                 // ret 0
inline constexpr GuestAddr kD3DDeviceRelease = 0x00234460;                // ret 0
inline constexpr GuestAddr kD3DDeviceCreateTexture2 = 0x002370D0;         // ret 0x1C
inline constexpr GuestAddr kD3DTextureGetSurfaceLevel2 = 0x002371A0;      // ret 8
inline constexpr GuestAddr kD3DResourceAddRef = 0x002373B0;               // ret 4
inline constexpr GuestAddr kD3DResourceRelease = 0x002373F0;              // ret 4
inline constexpr GuestAddr kD3DDeviceSetRenderStateYuvEnable = 0x00236DC0;  // ret 4
inline constexpr GuestAddr kD3DDeviceSetScreenSpaceOffset = 0x002354F0;   // ret 8 (2 floats)

// --- added by the fiber agent (M2, design 1.7/E9 + self-tests R2; see Changelog) ----------------
// MSVC 7 SEH3 helpers used by every guest __try (startup.md 8): __SEH_prolog 0x167DC8 is called as
// `push localsSize; push scopeTable; call` and builds the frame (record at ebp-0x10, handler
// _except_handler3, trylevel -1, saved esp at ebp-0x18); __SEH_epilog 0x167E03 unlinks it and
// restores ebx/esi/edi/ebp (disassembly 0x167DC8..0x167E13).
inline constexpr GuestAddr kCrtSehProlog = 0x00167DC8;
inline constexpr GuestAddr kCrtSehEpilog = 0x00167E03;
// `eh vector constructor iterator` ??_L 0x166E00: stdcall 5 (ptr, size, count, ctor, dtor), ret 0x14;
// ctor/dtor are thiscall without stack arguments (0x166E22 `mov ecx,esi; call [ebp+0x14]`); the body
// is __try/__finally (scope table 0x2AC230) whose finally 0x166E4A calls __ArrayUnwind 0x166D42 for
// the elements constructed so far when the loop did not complete.
inline constexpr GuestAddr kCrtEhVectorCtorIterator = 0x00166E00;
// CRT strchr 0x166FC0: cdecl (str, ch), reads the string without any SEH frame (0x166FCC..0x166FD8).
inline constexpr GuestAddr kCrtStrchr = 0x00166FC0;

// --- added in M2 phase 2 (integration: game observer probes, xapi/game_probes.cpp; see Changelog) --
// Every prologue was checked with capstone (whole, position-independent instructions; no direct
// branch of a Ghidra function and no raw E8/E9 in the code sections targets the inside of the stolen
// range) and every return with the call sites (docs/loader_design.md section 8, M2).
// .img archives (files.md 3.2/3.3): mount FUN_0013FE70 (cdecl 1: name, plain ret 0x13FFAC, callers
// `add esp,4` at 0xD277C/0xD27AE via the jmp thunk 0x13FFB0; returns al = 1 on success) and lookup
// FUN_0013FE10 (cdecl 3: name, &offset (stream +0x4C), &size (stream +0x40); returns the archive
// stream or 0; only caller NiUDSStream ctor 0x13F3B2). Mounted entries {stream, header} are pointers
// in the array [0x2D7AF0] with count [0x2D7AF8]; [0x2D7AE8] = 0 disables the lookup while mounting.
inline constexpr GuestAddr kGameArchiveMount = 0x0013FE70;
inline constexpr GuestAddr kGameArchiveLookup = 0x0013FE10;
inline constexpr GuestAddr kGameArchiveTableVar = 0x002D7AF0;  // entry** (each entry: +0 stream, +4 header)
inline constexpr GuestAddr kGameArchiveCountVar = 0x002D7AF8;  // u32
inline constexpr GuestAddr kGameArchiveSearchVar = 0x002D7AE8;  // u8, 0 while mounting
// Game::Init FUN_000A26A0 (CApp vf2 base part; thiscall ecx = CApp, no stack args, plain ret 0xA2C1A,
// called `mov ecx,esi; call` at 0xD28F6 at the end of CApp::Init 0xD26A0). Sets fps/dt from
// CApp+0xE8, selects the language again in PAL mode (0xA28DB..0xA2900), creates the contexts and
// enters "Intro", then reads BUILD.TXT into kGameBuildText (<= 0xFF chars; startup.md 2.6 #11/#19).
inline constexpr GuestAddr kGameInit = 0x000A26A0;
// CApp::Frame FUN_000D2630 (CApp vf3; thiscall, no stack args, plain ret 0xD265A/0xD268B, al = FALSE
// ends the main loop in main; startup.md 3.1). Called once per frame.
inline constexpr GuestAddr kCAppFrame = 0x000D2630;
// XAPILIB::XGetLanguage 0xF6440 (no args, plain ret 0xF6475/0xF6479; ExQueryNonVolatileSetting(7),
// values > 9 mapped by the region; its only caller FUN_000D9990 maps 3 german, 4 french, 5 spanish,
// 6 italian, else english - files.md 3.2, startup.md 2.6 #4).
inline constexpr GuestAddr kXapiXGetLanguage = 0x000F6440;
// g_pGame (CApp*, set by the game base ctor FUN_000A2240; startup.md 2.5) and CApp fields.
inline constexpr GuestAddr kGameGamePtr = 0x002E7B84;
inline constexpr std::uint32_t kCAppVideoStandardOffset = 0xE8;  // 0 = PAL, 1 = NTSC (files.md 3.2)
inline constexpr std::uint32_t kCAppContextListOffset = 0xB8;    // first context
inline constexpr std::uint32_t kCAppCurrentContextOffset = 0xBC;
inline constexpr std::uint32_t kGameContextNameOffset = 0x04;  // char* (heap copy, ctor 0x2C346)
inline constexpr std::uint32_t kGameContextNextOffset = 0x08;  // next in the CApp+0xB8 list
// String-table manager singleton (FUN_000D9CF0 creates it, 0x1C bytes) and its current language
// string (+0x18, set by FUN_000D9B20 from FUN_000D9990 = "german"/"french"/... - files.md 3.2).
inline constexpr GuestAddr kGameStringManagerPtr = 0x002EC640;
inline constexpr std::uint32_t kStringManagerLanguageOffset = 0x18;
// Time base written by Game::Init from CApp+0xE8 (startup.md 3.2/3.4): fps (float, 50.0 PAL / 60.0
// NTSC), fixed time step dt (float, 0.02 / 1/60) and the movie name tag ("PAL" / "NTSC", char[8]).
inline constexpr GuestAddr kGameFpsVar = 0x002D6744;
inline constexpr GuestAddr kGameFrameTimeVar = 0x002D6748;
inline constexpr GuestAddr kGameMovieTagVar = 0x002E7B70;
// BUILD.TXT contents (char[0x100]: NiUDSStream("BUILD.TXT") 0xA2B8E, Read(0x2E7A68, 0x100) 0xA2BBA,
// NUL at 0xA2BD2 - files.md 1.2: "1.25").
inline constexpr GuestAddr kGameBuildText = 0x002E7A68;
inline constexpr std::uint32_t kGameBuildTextSize = 0x100;
// Disc error handler (files.md 2.4): DAT_0030E68C is read only at 0x13F1E3 (ReadFileEx completion,
// dwErr != 0) and 0x13F6B2 (FUN_0013F660, ReadFileEx returned FALSE); FUN_000F33E0 sets it through
// FUN_0013FAD0 to the object 0x2D707C (vtable 0x2A59BC). Its vf0 0xF2FD0 (thiscall, 1 stack arg =
// the stream; not a Ghidra function, 0xF2FD0..0xF30A9) draws the disc error screen in an endless
// loop (`jmp 0xF3004` at 0xF30A4, no ret) - M2 exit 5: it must never be called.
inline constexpr GuestAddr kGameDiscErrorHandlerVar = 0x0030E68C;
inline constexpr GuestAddr kGameDiscErrorObject = 0x002D707C;
inline constexpr GuestAddr kGameDiscErrorScreen = 0x000F2FD0;
// --- added in the M2 review fixes (context switch probe, review finding 4) --------------------------
// Game::Frame FUN_000A2C20 (thiscall ecx = CApp, called by CApp::Frame) switches the context inside its
// fixed-step loop when cur->Update() returns FALSE: cur->Exit() (0xA2DEB), FUN_0002A660 (0xA2DF6, eight
// cleared frames), CApp+0xBC = CApp+0xC0 (0xA2E28), CApp+0xC0 = 0 (0xA2E34), FUN_000A48C0 (call 0xA2E40,
// return address 0xA2E45), new cur->Enter() (0xA2EC8) and Update(). Several switches can happen within
// one CApp::Frame (up to 6 steps per frame, startup.md 3.2).
inline constexpr GuestAddr kGameFrame = 0x000A2C20;
// FUN_000A48C0: cdecl, no args, `xor eax,eax; ret` (0xA48D4): FUN_000A3CE0 + FUN_000A3B10 on the game
// memory pool object 0x2E7D60 (kGameMemoryPool). Callers: Game::Frame right after `cur = next`
// (kGameFrameContextSwitchReturn) and the level loader FUN_0002A760. Prologue B9 60 7D 2E 00 =
// mov ecx,0x2E7D60 (capstone; the only raw branch candidate into 0xA48C1..0xA48C4, 0xA4892, lies inside
// the instruction at 0xA4890).
inline constexpr GuestAddr kGamePoolMaintenance = 0x000A48C0;
inline constexpr GuestAddr kGameFrameContextSwitchReturn = 0x000A2E45;
inline constexpr std::uint32_t kCAppNextContextOffset = 0xC0;  // next context (0 after the switch)
// --- added in M3b (game timing: 60 Hz tick with PAL content, xapi/game_timing.cpp; see Changelog) ----
// Time base block of Game::Init (kGameInit) 0xA26BD..0xA276A, analysis/loader/timing60.md 2.1 (capstone,
// scripts/timing_patch.out): `mov eax,[esi+0xE8]; mov ebx,1; cmp eax,ebx; jne 0xA2714` selects the branch
// (CApp+0xE8 = 1 NTSC, else PAL). Both branches load fps and dt with `fld` from the shared .rdata pool and
// leave them on the x87 stack; the common tail stores st0 = dt to kGameDtMinVar (0xA274E), sets
// kGameFixedStepVar = bl = 1 (0xA2754), then `fstp` fps -> kGameFpsVar (0xA275E) and dt -> kGameFrameTimeVar
// (0xA2764). The PAL branch is patched operand by operand to the NTSC values (timing60.md 2.3).
inline constexpr GuestAddr kGameInitVideoStandardBranch = 0x000A26BD;  // 8B 86 E8 00 00 00 .. 75 48 (15 bytes)
inline constexpr GuestAddr kGameInitNtscFpsLoad = 0x000A26CC;          // fld [kRdataFloat60]
inline constexpr GuestAddr kGameInitNtscDtLoad = 0x000A26E3;           // fld [kRdataFloatOneSixtieth]
inline constexpr GuestAddr kGameInitNtscFpsCopyStore = 0x000A26E9;     // mov [kGameFpsCopy2Var], 60.0f
inline constexpr GuestAddr kGameInitNtscDtMaxStore = 0x000A26F3;       // mov [kGameDtMaxVar], 1/60
inline constexpr GuestAddr kGameInitNtscFrameTimeStore = 0x000A26FD;   // mov [kGameVideoFrameTimeVar], 1/30.5
inline constexpr GuestAddr kGameInitPalFpsLoad = 0x000A2714;           // fld [kRdataFloat50]
inline constexpr GuestAddr kGameInitPalFpsCopyStore = 0x000A271A;      // mov [kGameFpsCopy2Var], 50.0f
inline constexpr GuestAddr kGameInitPalFpsCopy1Store = 0x000A2724;     // fst [kGameFpsCopy1Var] (unchanged)
inline constexpr GuestAddr kGameInitPalDtMaxStore = 0x000A272A;        // mov [kGameDtMaxVar], 0.02f
inline constexpr GuestAddr kGameInitPalDtLoad = 0x000A2734;            // fld [kRdataFloat0p02]
inline constexpr GuestAddr kGameInitPalFrameTimeStore = 0x000A273A;    // mov [kGameVideoFrameTimeVar], 1/25.4
inline constexpr GuestAddr kGameInitPalMovieTagStore = 0x000A2744;     // mov [kGameMovieTagVar], "PAL" (kept)
inline constexpr GuestAddr kGameInitTimeBaseTail = 0x000A274E;         // common tail, 0x1C bytes (unchanged)
// Shared .rdata float constants (timing60.md 2.2): 50.0f has 9 code users, 0.02f 13 users, so the operand
// of the PAL `fld` is retargeted to the NTSC branch's own constants, never the constant itself.
inline constexpr GuestAddr kRdataFloat50 = 0x002955AC;          // 0x42480000
inline constexpr GuestAddr kRdataFloat0p02 = 0x002942C0;        // 0x3CA3D70A
inline constexpr GuestAddr kRdataFloat60 = 0x002942CC;          // 0x42700000 (3 users, read only)
inline constexpr GuestAddr kRdataFloatOneSixtieth = 0x0029E390;  // 0x3C888889 (4 users, read only)
// Further time base globals (timing60.md 3): kGameFixedStepVar u8 (1 = fixed step, .data initial value 1;
// read at 0xA2D23); kGameDtMinVar/kGameDtMaxVar clamps of the dead variable-step branch (0xA2D3A..0xA2D66);
// kGameFpsCopy1Var/kGameFpsCopy2Var fps copies and kGameVideoFrameTimeVar (1/25.4 PAL, 1/30.5 NTSC) are only
// written, never read. kGameFpsVar is read only by the pause/input ctor FUN_000E4AA0 (key repeat period
// trunc(fps * 0.25), CApp::Init step 6, i.e. BEFORE Game::Init: it sees the .data initial value).
inline constexpr GuestAddr kGameFixedStepVar = 0x002D674C;
inline constexpr GuestAddr kGameDtMinVar = 0x002E7A58;
inline constexpr GuestAddr kGameDtMaxVar = 0x002E7A5C;
inline constexpr GuestAddr kGameFpsCopy1Var = 0x002E7A60;
inline constexpr GuestAddr kGameVideoFrameTimeVar = 0x002E7B68;
inline constexpr GuestAddr kGameFpsCopy2Var = 0x002E7B6C;
// One call per fixed time step: Game::Frame's step loop calls FUN_0002C890 (thiscall, ecx = [0x2E7B7C], no
// stack args, plain ret, al = 1; per-step pad helper reading the active controller [0x2E6FA4]) exactly once
// per step at 0xA2DD3, after CApp vf10 and before cur->Update(); it has no other caller (callgraph.tsv, no
// raw pointer to it in the image). Prologue 55 8B 2D A4 6F 2E 00 = push ebp; mov ebp,[0x2E6FA4]; no branch
// targets 0x2C891..0x2C896 (capstone scan of all code sections). probe:Game::FixedStep counts the steps.
inline constexpr GuestAddr kGameFixedStepHelper = 0x0002C890;
inline constexpr GuestAddr kGameFrameFixedStepCall = 0x000A2DD3;
// Game timer FUN_000D2B90: cdecl void(int64* out), plain ret 0xD2BE6 (callers `add esp,4`). First call:
// QueryPerformanceFrequency -> kGameTimerFrequencyVar (int64) and 1.0f/QPF -> kGameTimerInverseFrequencyVar
// (float), then *out = QueryPerformanceCounter (XAPILIB 0xF641B). 23 call sites; three in Game::Frame
// (analysis/loader/pacing60.md 1.4, timing60.md 6): 0xA2C6A "now" of the fixed-step accumulator (return
// 0xA2C6F), 0xA2FA1 one-time init of the previous render time 0x2E7D50 (return 0xA2FA6), 0xA2FB6 render branch,
// g_frameDelta [0x2E7B88] = now - 0x2E7D50 (return 0xA2FBB). Game::Frame converts seconds to ticks with
// trunc(QPF * s) (FUN_000D2B50/_ftol) and subtracts ticks(dt) per step. Prologue 83 EC 08 8B 0D 4C B4 2E 00 =
// sub esp,8; mov ecx,[0x2EB44C] (9 bytes, position-independent; no branch targets 0xD2B91..0xD2B98).
inline constexpr GuestAddr kGameTimer = 0x000D2B90;
inline constexpr GuestAddr kGameTimerFrequencyVar = 0x002EB448;
inline constexpr GuestAddr kGameTimerInverseFrequencyVar = 0x002EB450;
inline constexpr GuestAddr kGameFrameClockSampleReturn = 0x000A2C6F;
inline constexpr GuestAddr kGameFrameRenderInitSampleReturn = 0x000A2FA6;
inline constexpr GuestAddr kGameFrameRenderSampleReturn = 0x000A2FBB;

// ---------------------------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------------------------
template <class T = void>
inline T* Ptr(GuestAddr addr) noexcept {
    return reinterpret_cast<T*>(static_cast<std::uintptr_t>(addr));
}
inline GuestAddr Addr(const volatile void* p) noexcept {
    return static_cast<GuestAddr>(reinterpret_cast<std::uintptr_t>(p));
}
template <class T>
inline T Read(GuestAddr addr) noexcept {
    T v;
    std::memcpy(&v, Ptr<const void>(addr), sizeof(T));
    return v;
}
template <class T>
inline void Write(GuestAddr addr, const T& value) noexcept {
    std::memcpy(Ptr<void>(addr), &value, sizeof(T));
}
// Typed pointer to a guest function, e.g. Func<void(__stdcall*)(void*, void*)>(kXapiThreadStartup).
template <class Fn>
inline Fn Func(GuestAddr addr) noexcept {
    return reinterpret_cast<Fn>(static_cast<std::uintptr_t>(addr));
}

inline constexpr bool InXbeImage(GuestAddr addr, std::uint32_t size = 1) noexcept {
    return addr >= kXbeBase && size <= kXbeSpaceEnd - kXbeBase && addr <= kXbeSpaceEnd - size;
}

// True if [addr, addr+size) is committed and readable (VirtualQuery; for logging/diagnostics).
bool IsReadable(GuestAddr addr, std::uint32_t size) noexcept;
// Copies bytes into guest code/data (temporarily making the pages writable if needed) and flushes
// the instruction cache. Returns false if the range is not committed.
bool WriteBytes(GuestAddr addr, const void* data, std::uint32_t size) noexcept;
// Copies a guest ANSI_STRING into a std::string (empty for nullptr / unreadable buffer).
std::string ReadAnsiString(const xbox::ANSI_STRING* s);

// ---------------------------------------------------------------------------------------------
// Generated tables (src/host/gen, tools/gen)
// ---------------------------------------------------------------------------------------------
inline constexpr std::uint8_t kFunctionGhidra = 0;
inline constexpr std::uint8_t kFunctionExtraStart = 1;
inline constexpr std::uint8_t kFunctionSyntheticRange = 2;  // not a function start

struct FunctionInfo {  // row of gen/xbe_functions.gen.inc
    GuestAddr address;
    std::uint32_t size;
    std::uint8_t flags;
    const char* name;
};

struct FsPatchSite {  // row of gen/fs_patch_table.gen.inc (design 1.6)
    GuestAddr address;
    std::uint8_t length;         // 6, 7 or 8
    std::uint8_t original[8];    // original instruction bytes (zero padded)
    std::uint8_t dispOffset;     // position of the disp32 in the instruction (2, 3 or 4)
    std::uint32_t originalDisp;  // 0x04, 0x20, 0x24, 0x28 or 0x58
    std::uint32_t tlsSlot;       // FutTlsSlot: new disp = 0xE10 + 4 * TlsSlotIndex(tlsSlot)
    const char* function;
    const char* library;
};

std::span<const FunctionInfo> Functions() noexcept;    // sorted by address
std::span<const FsPatchSite> FsPatchSites() noexcept;  // 38 entries, sorted by address
// Function whose [address, address+size) contains addr (nullptr if none).
const FunctionInfo* FindFunctionContaining(GuestAddr addr) noexcept;
// Entry whose start is exactly addr (nullptr if none or a synthetic range).
const FunctionInfo* FindFunctionStart(GuestAddr addr) noexcept;
// "0x0013F5D5 FUN_0013f590+0x45" (design 1.11); addresses outside the XBE give "0x12345678".
std::string Symbolize(GuestAddr addr);

}  // namespace guest

// Fixed-address guest global (design 3.4), e.g.  FUT_GLOBAL(CApp*, g_pApp, guest::kGameAppPtr);
#define FUT_GLOBAL(type, name, address) inline type& name = *::guest::Ptr<type>(address)
