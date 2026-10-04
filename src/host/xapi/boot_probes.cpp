// Observer probes on the M1 boot path (design 6, M1 exit criteria 2 and 4; FUT_PROBE in core/hooks.h).
//
// Each probe logs, calls the original guest function through its trampoline and returns the
// original eax, so guest behaviour is unchanged. They make the boot sequence visible on log level
// info: XapiInitProcess -> _rtinit -> _cinit (38 MiB game pool, XGetDiskClusterSizeA("U:\")) and a
// possible XapiBootToDash. All are default on; `[hooks] disable = probe:*` switches them off.
//
// Prologue bytes and lengths were checked with capstone on orig/retail_eu/default.xbe: whole,
// position-independent instructions, and no branch in any Ghidra function targets the inside of a
// stolen range (scratch script of the phase 3 integration, documented in docs/loader_design.md
// "Abweichungen in M1").
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/log.h"

#include <intrin.h>

#include <cstdint>
#include <initializer_list>

namespace xapi {
namespace {

// Calls the original function behind the probe at `address` (stdcall or cdecl, DWORD arguments).
std::uint32_t CallOriginal(guest::GuestAddr address,
                           std::initializer_list<std::uint32_t> args = {}) noexcept {
    const guest::GuestAddr trampoline = core::HookTrampoline(address);
    if (trampoline == 0) {
        core::Fatal("probe at 0x%08X runs without an installed trampoline", address);
    }
    return core::CallGuest(trampoline, args);
}

// Copies a short NUL-terminated guest string for log output (stops at unreadable memory).
void CopyGuestString(const char* text, char* out, std::size_t outSize) noexcept {
    std::size_t n = 0;
    if (text != nullptr) {
        const guest::GuestAddr p = guest::Addr(text);
        while (n + 1 < outSize && guest::IsReadable(p + static_cast<guest::GuestAddr>(n), 1) &&
               text[n] != 0) {
            out[n] = text[n];
            ++n;
        }
    }
    out[n] = 0;
}

// XAPILIB::XapiInitProcess 0xF92C0 - no arguments, plain `ret` (0xF9374); called once by the main
// thread stub (0xF846E). Evidence: startup.md 1 and 2.3, design 1.10.
// Prologue 55 8B EC 83 EC 34 = push ebp; mov ebp,esp; sub esp,0x34.
std::uint32_t __cdecl XapiInitProcessProbe() noexcept {
    FUT_LOGI(Xapi, "boot: XapiInitProcess (0x%08X) start", guest::kXapiInitProcess);
    const std::uint32_t eax = CallOriginal(guest::kXapiInitProcess);
    FUT_LOGI(Xapi, "boot: XapiInitProcess done (XapiProcessHeap 0x%08X)",
             guest::Read<std::uint32_t>(guest::kXapiProcessHeapVar));
    return eax;
}

// XAPILIB::_rtinit 0xF97DB - no arguments, plain `ret` (0xF9803); called by the main thread stub
// (0xF84B5). Evidence: startup.md 1, design 1.10.
// Prologue 56 57 B8 E0 DE 2B 00 = push esi; push edi; mov eax,0x2BDEE0.
std::uint32_t __cdecl RtInitProbe() noexcept {
    FUT_LOGI(Xapi, "boot: _rtinit (0x%08X) start", guest::kXapiRtInit);
    const std::uint32_t eax = CallOriginal(guest::kXapiRtInit);
    FUT_LOGI(Xapi, "boot: _rtinit done");
    return eax;
}

// XAPILIB::_cinit 0xF9783 - no arguments, plain `ret` (0xF97DA); called by the main thread stub
// (0xF84BA). Runs the C++ initializer table 0x2BDEF0..0x2D5B30 (startup.md 2.4).
// Prologue A1 FC 85 2D 00 = mov eax,[0x2D85FC] (absolute operand, position independent).
std::uint32_t __cdecl CInitProbe() noexcept {
    FUT_LOGI(Xapi, "boot: _cinit (0x%08X) start", guest::kXapiCInit);
    const std::uint32_t eax = CallOriginal(guest::kXapiCInit);
    FUT_LOGI(Xapi, "boot: _cinit done (eax 0x%X, save cluster size [0x%08X] = 0x%X)", eax,
             guest::kGameSaveClusterSizeVar, guest::Read<std::uint32_t>(guest::kGameSaveClusterSizeVar));
    return eax;
}

// XGetDiskClusterSizeA 0xF8081 (FUN_000f8081) - stdcall 1 (path), `ret 4` (0xF811D).
// Returns SectorsPerAllocationUnit * BytesPerSector or 0 (files.md 2.3). The C++ initializer
// 0x20EFC0 passes "U:\" and later initializers divide by the result (0x20EFDD): it must be 0x4000
// (design 4.5, M1 exit criterion 4).
// Prologue 55 8B EC 83 EC 34 = push ebp; mov ebp,esp; sub esp,0x34.
DWORD __stdcall XGetDiskClusterSizeAProbe(const char* path) noexcept {
    const DWORD result = CallOriginal(guest::kXapiXGetDiskClusterSizeA, {guest::Addr(path)});
    char text[64];
    CopyGuestString(path, text, sizeof(text));
    if (result == 0) {
        FUT_LOGE(Xapi, "XGetDiskClusterSizeA(\"%s\") = 0 - the game divides by this value", text);
    } else if (result != 0x4000) {
        FUT_LOGW(Xapi, "XGetDiskClusterSizeA(\"%s\") = 0x%lX (expected 0x4000, design 4.5)", text, result);
    } else {
        FUT_LOGI(Xapi, "XGetDiskClusterSizeA(\"%s\") = 0x%lX", text, result);
    }
    return result;
}

// memory.cpp pool init FUN_000a4920 - thiscall (ecx = pool object 0x2E7D60), 1 stack argument (size),
// `ret 4` (0xA4951). Called once from FUN_000a4520 (0xA4547) on the first game allocation, which
// happens inside _cinit (startup.md 5.2). Declared __fastcall with an unused edx parameter, which has
// the same register/stack layout and purge as the thiscall original.
// Prologue 56 57 8B 7C 24 0C = push esi; push edi; mov edi,[esp+0xC].
std::uint32_t __fastcall GamePoolInitProbe(std::uint32_t self, std::uint32_t /*edx*/,
                                           std::uint32_t size) noexcept {
    const guest::GuestAddr trampoline = core::HookTrampoline(guest::kGameMemoryPoolInit);
    if (trampoline == 0) {
        core::Fatal("probe at 0x%08X runs without an installed trampoline", guest::kGameMemoryPoolInit);
    }
    const std::uint32_t eax = core::CallGuestThiscall(trampoline, self, {size});
    const std::uint32_t begin = guest::Read<std::uint32_t>(self + guest::kGameMemoryPoolBeginOffset);
    const std::uint32_t end = guest::Read<std::uint32_t>(self + guest::kGameMemoryPoolEndOffset);
    if (begin == 0 || end - begin != size) {
        FUT_LOGE(
            Game,
            "game memory pool (object 0x%08X): allocation of 0x%X bytes failed (begin 0x%08X end 0x%08X)",
            self, size, begin, end);
    } else {
        FUT_LOGI(Game, "game memory pool (memory.cpp, object 0x%08X): 0x%X bytes (%u MiB) at 0x%08X..0x%08X",
                 self, size, size >> 20, begin, end);
    }
    return eax;
}

// XAPILIB::XapiBootToDash 0xF8FCA - stdcall 3 (reason, parameter1, parameter2), `ret 0xC` (0xF9023).
// Builds LD_LAUNCH_DASHBOARD and calls XLaunchNewImageA (hooked: log + exit code 4). Must not occur
// before main in M1 (exit criterion 2). Evidence: startup.md 3.5.
// Prologue 55 8B EC 81 EC 00 0C 00 00 = push ebp; mov ebp,esp; sub esp,0xC00.
std::uint32_t __stdcall XapiBootToDashProbe(std::uint32_t reason, std::uint32_t parameter1,
                                            std::uint32_t parameter2) noexcept {
    const guest::GuestAddr caller = guest::Addr(_ReturnAddress());
    const guest::FunctionInfo* f = guest::FindFunctionContaining(caller);
    FUT_LOGW(Xapi, "boot: XapiBootToDash(reason %u, parameter1 %u, parameter2 %u) called from 0x%08X %s+0x%X",
             reason, parameter1, parameter2, caller, f != nullptr ? f->name : "?",
             f != nullptr ? caller - f->address : 0u);
    return CallOriginal(guest::kXapiBootToDash, {reason, parameter1, parameter2});
}

}  // namespace

FUT_PROBE(guest::kXapiInitProcess, &XapiInitProcessProbe, Cdecl, "probe:XAPILIB::XapiInitProcess",
          "55 8B EC 83 EC 34", 6);
FUT_PROBE(guest::kXapiRtInit, &RtInitProbe, Cdecl, "probe:XAPILIB::_rtinit", "56 57 B8 E0 DE 2B 00", 7);
FUT_PROBE(guest::kXapiCInit, &CInitProbe, Cdecl, "probe:XAPILIB::_cinit", "A1 FC 85 2D 00", 5);
FUT_PROBE(guest::kXapiXGetDiskClusterSizeA, &XGetDiskClusterSizeAProbe, Stdcall,
          "probe:XAPILIB::XGetDiskClusterSizeA", "55 8B EC 83 EC 34", 6);
FUT_PROBE(guest::kGameMemoryPoolInit, &GamePoolInitProbe, Thiscall, "probe:memory.cpp::PoolInit",
          "56 57 8B 7C 24 0C", 6);
FUT_PROBE(guest::kXapiBootToDash, &XapiBootToDashProbe, Stdcall, "probe:XAPILIB::XapiBootToDash",
          "55 8B EC 81 EC 00 0C 00 00", 9);

}  // namespace xapi
