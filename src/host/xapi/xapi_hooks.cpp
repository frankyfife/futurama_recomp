// XAPI replacement functions (design E5, 2.1, Anhang B). Owned by the core agent.
// Every hook carries the original prologue bytes (FUT_HOOK_EX expected bytes, checked against the
// disassembly of the retail EU XBE) in addition to the generic check against the XBE file.
#include "xapi/xapi_hooks.h"

#include "core/config.h"
#include "core/guest.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "vfs/vfs.h"

#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace xapi {
namespace {

// Copies a NUL-terminated guest string (at most `max` bytes, stops at unreadable memory).
std::string ReadGuestCString(const char* text, std::size_t max) {
    std::string out;
    if (text == nullptr) {
        return out;
    }
    guest::GuestAddr p = guest::Addr(text);
    while (out.size() < max) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!guest::IsReadable(p, 1) || VirtualQuery(guest::Ptr<const void>(p), &mbi, sizeof(mbi)) == 0) {
            break;
        }
        const std::uint64_t regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const std::size_t avail =
            static_cast<std::size_t>(std::min<std::uint64_t>(regionEnd - p, max - out.size()));
        const char* s = guest::Ptr<const char>(p);
        const std::size_t n = strnlen(s, avail);
        out.append(s, n);
        if (n < avail) {
            break;  // terminator found
        }
        p += static_cast<guest::GuestAddr>(n);
    }
    return out;
}

// ASSUMPTION: XDK names of the LD_LAUNCH_DASHBOARD reasons/errors (startup.md 3.5 marks them as
// "Vermutung nach XDK-Konvention"); only used for log text.
const char* DashboardReasonName(std::uint32_t reason) {
    switch (reason) {
    case 0:
        return "main menu";
    case 1:
        return "error";
    case 2:
        return "memory (free space)";
    case 3:
        return "settings";
    case 4:
        return "music";
    default:
        return "?";
    }
}

const char* DashboardErrorName(std::uint32_t code) {
    switch (code) {
    case 1:
        return "invalid XBE / XAPI init failure (also used after main returns)";
    case 2:
        return "invalid hard disk (partition1 cluster size, TDATA/UDATA)";
    case 3:
        return "XBE region";
    case 4:
        return "parental control";
    case 5:
        return "XBE media type";
    default:
        return "?";
    }
}

}  // namespace

// XAPILIB::QueryPerformanceCounter 0xF641B - stdcall, 1 arg (LARGE_INTEGER*), returns TRUE (ret 4).
// Original: rdtsc against a hard-wired 733 333 333 Hz frequency. Evidence: disassembly 0xF641B
// (mov ecx,[esp+4]; rdtsc; mov [ecx],eax; mov [ecx+4],edx; xor eax,eax; inc eax; ret 4),
// cpu.md 3, kernel.md 8.8, startup.md 3.3, design 1.9.
BOOL __stdcall QueryPerformanceCounterHle(LARGE_INTEGER* counter) noexcept {
    if (counter != nullptr) {
        ::QueryPerformanceCounter(counter);
    }
    return TRUE;
}
FUT_HOOK_EX(guest::kXapiQueryPerformanceCounter, &QueryPerformanceCounterHle, Stdcall,
            "XAPILIB::QueryPerformanceCounter", "8B 4C 24 04 0F 31", true);

// XAPILIB::QueryPerformanceFrequency 0xF642C - stdcall, 1 arg (LARGE_INTEGER*), returns TRUE (ret 4).
// Original: constant 0x2BB5C755. The game computes 1/QPF once (0xD2BB6), so a consistent Win32 pair is
// enough. Evidence: disassembly 0xF642C (mov eax,[esp+4]; and [eax+4],0; mov [eax],0x2BB5C755; ret 4),
// cpu.md 3, design 1.9.
BOOL __stdcall QueryPerformanceFrequencyHle(LARGE_INTEGER* frequency) noexcept {
    if (frequency != nullptr) {
        ::QueryPerformanceFrequency(frequency);
    }
    return TRUE;
}
FUT_HOOK_EX(guest::kXapiQueryPerformanceFrequency, &QueryPerformanceFrequencyHle, Stdcall,
            "XAPILIB::QueryPerformanceFrequency", "8B 44 24 04 83 60 04 00", true);

// XAPILIB::OutputDebugStringA 0xF3DE0 - stdcall, 1 arg (LPCSTR), void (ret 4).
// Original: builds an ANSI_STRING and executes int 0x2d + int3 (debug service), which would raise a
// breakpoint exception under Windows. Evidence: disassembly 0xF3DE0..0xF3E0F, cpu.md 3, design 1.11.
// Callers: FUN_000edc70, FUN_0016eb2b (cpu.md 3).
void __stdcall OutputDebugStringAHle(const char* text) noexcept {
    try {
        std::string s = ReadGuestCString(text, 4096);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        FUT_LOGI(Xapi, "OutputDebugString: %s", s.c_str());
    }
    FUT_HLE_CATCH("XAPILIB::OutputDebugStringA")
}
FUT_HOOK_EX(guest::kXapiOutputDebugStringA, &OutputDebugStringAHle, Stdcall, "XAPILIB::OutputDebugStringA",
            "55 8B EC 51 51 8B 45 08", true);

// XAPILIB::XMountUtilityDrive 0xF7F7C - stdcall, 1 arg (BOOL fFormatClean), returns BOOL (ret 4).
// Original: selects a cache partition through \Device\Harddisk0\partition0 (XapiSelectCachePartition
// 0xF7D51), formats it with FATX if needed (0xF946D) and links \??\Z: to
// \Device\Harddisk0\Partition<n>. Replacement: Z: -> host directory <userdata>\cache\Z, TRUE.
// Only caller: XapiInitProcess 0xF93CC with InitFlags & 2 (= 0 for this XBE); the result is tested
// as a full DWORD (0xF93D1 test eax,eax). The game itself never uses Z: (files.md 1.1).
// Evidence: decomp 0xF7F7C, disassembly ret 4 at 0xF807E, files.md 1.1/5.1 item 5, design 2.1/4.1.
BOOL __stdcall XMountUtilityDriveHle(BOOL formatClean) noexcept {
    try {
        const core::Config& cfg = core::GetConfig();
        const std::filesystem::path dir = cfg.CacheDirectory() / "Z";
        std::error_code ec;
        if (formatClean) {
            // ASSUMPTION: "format clean" = start with an empty utility drive (FATX format on the Xbox).
            std::filesystem::remove_all(dir, ec);
        }
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            FUT_LOGW(Xapi, "XMountUtilityDrive: cannot create %s: %s",
                     core::WideToUtf8(dir.wstring()).c_str(), ec.message().c_str());
        }
        bool deviceOk = vfs::FindDevice(kUtilityDriveDevice) != nullptr;
        if (!deviceOk) {
            std::shared_ptr<vfs::Device> device = vfs::CreateHostDirDevice(dir, false);
            deviceOk = device != nullptr && vfs::AddDevice(kUtilityDriveDevice, device);
        }
        const NTSTATUS link = deviceOk ? vfs::CreateSymbolicLink(kUtilityDriveLink, kUtilityDriveDevice)
                                       : STATUS_OBJECT_NAME_NOT_FOUND;
        if (!deviceOk || (link < 0 && link != STATUS_OBJECT_NAME_COLLISION)) {
            // Not fatal: no game code uses Z: (files.md 1.1); XapiInitProcess only needs TRUE.
            FUT_LOGW(Xapi, "XMountUtilityDrive(formatClean=%d): Z: not available (device %s, link 0x%08lX)",
                     formatClean ? 1 : 0, deviceOk ? "ok" : "failed", static_cast<unsigned long>(link));
        } else {
            FUT_LOGI(Xapi, "XMountUtilityDrive(formatClean=%d): %s -> %s -> %s", formatClean ? 1 : 0,
                     kUtilityDriveLink, kUtilityDriveDevice, core::WideToUtf8(dir.wstring()).c_str());
        }
        return TRUE;
    }
    FUT_HLE_CATCH("XAPILIB::XMountUtilityDrive")
}
FUT_HOOK_EX(guest::kXapiXMountUtilityDrive, &XMountUtilityDriveHle, Stdcall, "XAPILIB::XMountUtilityDrive",
            "55 8B EC 81 EC 14 01 00 00", true);

// XAPILIB::XLaunchNewImageA 0xF82E3 - stdcall, 2 args (LPCSTR lpTitlePath, PLAUNCH_DATA pLaunchData),
// returns DWORD (ret 8). On the Xbox it never returns (HalReturnToFirmware); callers rely on that
// (XapiBootToDash returns with ret 0xC, startup.md 3.5). Callers: XapiBootToDash 0xF8FCA (boot
// errors and after main returns) and FUN_000E4B00 ({2, 0, 'U', blocks}: not enough space on U:).
// Replacement: log the request (decoded LD_LAUNCH_DASHBOARD) and end the process with
// core::kExitGuestShutdown. Evidence: decomp 0xF82E3, disassembly ret 8 at 0xF841D, startup.md 3.5,
// files.md 4.4, design 1.10 "Ende" and 4.5 (Pflichtwerte, last item).
DWORD __stdcall XLaunchNewImageAHle(const char* titlePath, const std::uint32_t* launchData) noexcept {
    try {
        const std::string path =
            titlePath != nullptr ? ReadGuestCString(titlePath, 260) : std::string("(dashboard)");
        std::uint32_t ld[4] = {};
        const bool haveData = launchData != nullptr && guest::IsReadable(guest::Addr(launchData), sizeof(ld));
        if (haveData) {
            std::memcpy(ld, launchData, sizeof(ld));
        }
        const void* ret = _ReturnAddress();
        const std::string caller = guest::Symbolize(guest::Addr(ret));
        if (titlePath == nullptr && haveData && ld[0] == 2 && ld[2] == 'U') {
            FUT_LOGE(Xapi,
                     "the game reports not enough free space on U: (%u blocks needed) and wants the memory "
                     "dashboard (XLaunchNewImageA from %s)",
                     ld[3], caller.c_str());
            if (core::GetConfig().msgbox) {
                MessageBoxA(nullptr, "Not enough free space for saved games (drive U:).", "Futurama",
                            MB_OK | MB_ICONWARNING);
            }
        } else if (titlePath == nullptr && haveData && ld[0] == 1) {
            FUT_LOGE(
                Xapi,
                "guest requested the dashboard: reason 1 (%s), code %u (%s), parameter %u - XLaunchNewImageA "
                "from %s",
                DashboardReasonName(ld[0]), ld[2], DashboardErrorName(ld[2]), ld[3], caller.c_str());
        } else {
            FUT_LOGW(Xapi, "guest XLaunchNewImageA(%s, %s{%u, %u, %u, %u}) from %s - %s", path.c_str(),
                     haveData ? "" : "no data ", ld[0], ld[1], ld[2], ld[3], caller.c_str(),
                     haveData ? DashboardReasonName(ld[0]) : "-");
        }
        core::ExitHostProcess(core::kExitGuestShutdown, "guest XLaunchNewImageA");
    }
    FUT_HLE_CATCH("XAPILIB::XLaunchNewImageA")
}
FUT_HOOK_EX(guest::kXapiXLaunchNewImageA, &XLaunchNewImageAHle, Stdcall, "XAPILIB::XLaunchNewImageA",
            "55 8B EC 81 EC 84 03 00 00", true);

}  // namespace xapi
