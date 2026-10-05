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
