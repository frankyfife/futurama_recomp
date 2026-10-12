// Function detours on the mapped XBE (design 3.1/3.2, E11): 5-byte `E9 rel32` at the function start,
// rest of the function unchanged. A plain hook has no trampoline (a replacement never calls the
// original). Probe hooks (FUT_PROBE, M1 addition, see docs/loader_design.md "Abweichungen in M1")
// log and then call the original through a trampoline built from explicitly listed prologue bytes.
//
// Static registration in any TU (re3 style):
//   // XAPILIB::QueryPerformanceCounter 0xF641B, stdcall 1 arg. Evidence: kernel.md 8.8, design 1.9.
//   BOOL __stdcall QueryPerformanceCounterHle(LARGE_INTEGER* counter) noexcept { ... }
//   FUT_HOOK(guest::kXapiQueryPerformanceCounter, &QueryPerformanceCounterHle, Stdcall,
//            "XAPILIB::QueryPerformanceCounter");
//   // optional original-byte check and default state:
//   FUT_HOOK_EX(guest::kXapiOutputDebugStringA, &OutputDebugStringHle, Stdcall,
//               "XAPILIB::OutputDebugStringA", "8B FF 55 8B EC", true);
//
// StageInstallHooks validates every enabled hook (design 3.2): address is a known function start
// (gen/xbe_functions.gen.inc), function >= 5 bytes, no duplicate address, no overlap with the fs
// patch sites or another hook, the 5 target bytes still equal the XBE file (core::ReadOriginalXbeBytes),
// expected bytes match; then writes the jmp. A hook disabled through
// [hooks] disable = <name|0xADDR|prefix*>,... leaves the original bytes in place.
// Hooks can also be installed at runtime (e.g. the --stop-at hook) with InstallHookNow().
//
// Probe hooks (wrapper hooks of design 3.1 without a length disassembler):
//   // XAPILIB::XapiInitProcess 0xF92C0, no args. Prologue: push ebp; mov ebp,esp; sub esp,0x34.
//   std::uint32_t __cdecl XapiInitProcessProbe() noexcept {
//       log(...); return core::CallGuest(core::HookTrampoline(guest::kXapiInitProcess)); }
//   FUT_PROBE(guest::kXapiInitProcess, &XapiInitProcessProbe, Cdecl, "probe:XAPILIB::XapiInitProcess",
//             "55 8B EC 83 EC 34", 6);
// `prologueHex` must list exactly the first `stolenBytes` (5..16) original bytes; they must be whole,
// position-independent instructions (no relative branch/call) and no branch may target their inside
// (check with capstone before adding a probe). The installer verifies the bytes against the XBE file,
// copies them into an executable trampoline followed by `jmp address+stolenBytes` and then writes the
// usual 5-byte jmp. Only the 5 jmp bytes change in the image.
//
// Conditional probes (G4, FUT_PROBE_IF): `condition(config)` decides at StageInstallHooks whether the probe is
// installed. A probe whose condition is false is skipped silently: no log line, not counted in the "hooks: ..."
// summary, the image stays untouched - a run with the option off logs exactly the hooks of a build without it.
#pragma once

#include "core/config.h"
#include "core/guest.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace core {

enum class HookConv : std::uint8_t { Stdcall, Cdecl, Thiscall, Fastcall, Custom };

struct HookDesc {
    guest::GuestAddr address;  // function start in the XBE
    const void* replacement;   // host code with the same convention and stack purge as the original
    HookConv conv;
    const char* name;           // e.g. "XAPILIB::QueryPerformanceCounter" (used by [hooks] disable)
    const char* expectedBytes;  // optional hex string of the first original bytes ("8B FF 55"), or nullptr
    bool defaultOn;
    const char* file;
    // Probe hooks only: number of original bytes copied into the trampoline (5..16, must equal the
    // length of expectedBytes). 0 = plain detour without trampoline.
    std::uint8_t trampolineBytes = 0;
    // G4: optional install condition (FUT_PROBE_IF); nullptr = always (subject to defaultOn and [hooks] disable).
    bool (*condition)(const Config& config) = nullptr;
};

void RegisterHook(const HookDesc& desc) noexcept;

struct HookRegistrar {
    explicit HookRegistrar(const HookDesc& desc) noexcept { RegisterHook(desc); }
};

// Installs all registered, enabled hooks (HostMain stage). False on any validation error.
bool StageInstallHooks(const Config& config);
// Validates and installs one hook immediately (not added to the static registry's install pass).
bool InstallHookNow(const HookDesc& desc) noexcept;
bool IsHookInstalled(guest::GuestAddr address) noexcept;

struct InstalledHook {
    guest::GuestAddr address;  // patched function start (5 bytes E9 rel32)
    const void* replacement;
    const char* name;
    guest::GuestAddr trampoline = 0;  // probe hooks: stolen bytes + jmp address+trampolineBytes
    std::uint8_t trampolineBytes = 0;
};
// Snapshot of all installed hooks in installation order (self-tests, crash diagnostics).
std::vector<InstalledHook> InstalledHookList();
// Entry of the trampoline of the installed probe hook at `address` ("call the original"), 0 if there
// is no installed probe hook at that address.
guest::GuestAddr HookTrampoline(guest::GuestAddr address) noexcept;
// True if `desc` matches an entry of [hooks] disable (exact name, 0xADDR, or "prefix*").
// Address entries compare by value: "0xF641B", "0x000F641B" (log format 0x%08X) and "0xf641b" match
// the same hook.
bool IsHookDisabled(const HookDesc& desc, const Config& config);
bool HookDisableEntryMatches(const HookDesc& desc, std::string_view entry) noexcept;
// Numeric [hooks] disable entry (strtoul base 0, must start with a digit) -> address.
bool ParseHookAddressEntry(std::string_view entry, guest::GuestAddr& address) noexcept;
// Entries of [hooks] disable that match no registered hook (StageInstallHooks logs a warning each).
std::vector<std::string> UnmatchedHookDisableEntries(const Config& config);

namespace detail {
// Code address of a function pointer or (single-inheritance) member function pointer.
template <class F>
const void* CodeAddress(F f) noexcept {
    static_assert(sizeof(F) == sizeof(void*),
                  "only plain function pointers / single-inheritance member pointers");
    const void* p = nullptr;
    std::memcpy(&p, &f, sizeof(p));
    return p;
}
}  // namespace detail

}  // namespace core

#define FUT_HOOK_CAT2(a, b) a##b
#define FUT_HOOK_CAT(a, b) FUT_HOOK_CAT2(a, b)
#define FUT_HOOK_EX(address, replacement, conv, name, expectedHex, defaultOn)                                \
    namespace {                                                                                              \
    const ::core::HookRegistrar FUT_HOOK_CAT(futHookRegistrar_, __LINE__)(::core::HookDesc{                  \
        (address), ::core::detail::CodeAddress(replacement), ::core::HookConv::conv, (name), (expectedHex),  \
        (defaultOn), __FILE__});                                                                             \
    }                                                                                                        \
    static_assert(true, "")
#define FUT_HOOK(address, replacement, conv, name)                                                           \
    FUT_HOOK_EX(address, replacement, conv, name, nullptr, true)
// Probe hook (default on): see the header comment. The replacement calls the original with
// core::CallGuest*(core::HookTrampoline(address), ...).
#define FUT_PROBE(address, replacement, conv, name, prologueHex, stolenBytes)                                \
    namespace {                                                                                              \
    const ::core::HookRegistrar FUT_HOOK_CAT(futHookRegistrar_, __LINE__)(::core::HookDesc{                  \
        (address), ::core::detail::CodeAddress(replacement), ::core::HookConv::conv, (name), (prologueHex),  \
        true, __FILE__, (stolenBytes)});                                                                     \
    }                                                                                                        \
    static_assert((stolenBytes) >= 5 && (stolenBytes) <= 16, "probe: 5..16 stolen bytes")
// G4: probe hook installed only if condition(config) is true (see the header comment).
#define FUT_PROBE_IF(address, replacement, conv, name, prologueHex, stolenBytes, condition)                  \
    namespace {                                                                                              \
    const ::core::HookRegistrar FUT_HOOK_CAT(futHookRegistrar_, __LINE__)(::core::HookDesc{                  \
        (address), ::core::detail::CodeAddress(replacement), ::core::HookConv::conv, (name), (prologueHex),  \
        true, __FILE__, (stolenBytes), (condition)});                                                        \
    }                                                                                                        \
    static_assert((stolenBytes) >= 5 && (stolenBytes) <= 16, "probe: 5..16 stolen bytes")
