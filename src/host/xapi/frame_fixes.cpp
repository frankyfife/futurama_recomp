// M3d: fixes for game code that runs per frame instead of per tick (see frame_fixes.h for the evidence).
//
// Addresses (orig/retail_eu/default.xbe; capstone listings in analysis/loader/m3d_frameclock.md 1.3 and the scan in
// tests/frameclock_tests.cpp): none of the stolen prologue bytes is a branch target or the target of a raw pointer
// (capstone scan of all code sections, M3d), all three are function starts of gen/xbe_functions.gen.inc.
#include "xapi/frame_fixes.h"

#include "core/guest.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "xapi/game_timing.h"

#include <atomic>

namespace xapi {
namespace {

// Death context vf3 (Death::Render, slot 3 of the vtable its constructor installs; m3d_frameclock.md 1.1):
//   push esi; mov esi,ecx; mov ecx,[esi+0x3C]; test ecx,ecx; je 0x2BF2B; mov edx,[esi+0x20]; mov eax,[ecx];
//   push edx; call [eax+4]; test eax,eax; jne 0x2BF2B; mov byte [esi+0x40],1; pop esi; ret
constexpr guest::GuestAddr kDeathRender = 0x0002BF10;
constexpr std::uint32_t kDeathUpdateSecondsOffset = 0x20;  // float, Death::Update 0x2C1EB fstp [esi+0x20]
// FUN_000CD340: occluder fade (sub esp,0x3C; push esi; mov esi,ecx; ... ret 0xCD5E0), called from FUN_000CE2B0.
constexpr guest::GuestAddr kOccluderFade = 0x000CD340;
// FUN_00045EF0: push esi; mov esi,ecx; call _rand 0x1668CC; and eax,0x80000001; jns; dec; or eax,-2; inc;
// mov [esi+0x24],eax; mov byte [esi+0xA8],1; pop esi; ret (32 bytes).
constexpr guest::GuestAddr kRandomFlicker = 0x00045EF0;
constexpr guest::GuestAddr kCrtRand = 0x001668CC;  // _rand: cdecl, no args, eax = 0..0x7FFF (per-thread LCG)
constexpr std::uint32_t kFlickerValueOffset = 0x24;
constexpr std::uint32_t kFlickerFlagOffset = 0xA8;

std::atomic<std::uint64_t> g_deathCleared{0};
std::atomic<std::uint64_t> g_fadeCalls{0};
std::atomic<std::uint64_t> g_fadeSkipped{0};
std::atomic<std::uint64_t> g_flickerCalls{0};
std::atomic<std::uint64_t> g_flickerKept{0};
std::atomic<bool> g_reporter{false};

void RegisterReporter() noexcept {
    if (!g_reporter.exchange(true)) {
        core::AddExitReporter("frame fixes", &LogFrameFixSummary);
    }
}

guest::GuestAddr Trampoline(guest::GuestAddr address) noexcept {
    const guest::GuestAddr t = core::HookTrampoline(address);
    if (t == 0) {
        core::Fatal("probe at 0x%08X runs without an installed trampoline", address);
    }
    return t;
}

// F2. Thiscall without stack arguments (declared __fastcall with an unused edx: same layout and purge).
std::uint32_t __fastcall DeathRenderProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    const std::uint32_t eax = core::CallGuestThiscall(Trampoline(kDeathRender), self, {});
    if (SubTickFramesActive() && guest::IsReadable(self + kDeathUpdateSecondsOffset, 4)) {
        guest::Write<float>(self + kDeathUpdateSecondsOffset, 0.0f);
        g_deathCleared.fetch_add(1);
        RegisterReporter();
    }
    return eax;
}

// F3.
std::uint32_t __fastcall OccluderFadeProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    g_fadeCalls.fetch_add(1);
    if (RenderingFrameWithoutStep()) {
        g_fadeSkipped.fetch_add(1);
        RegisterReporter();
        return 0;  // void function
    }
    return core::CallGuestThiscall(Trampoline(kOccluderFade), self, {});
}

// F4 (replacement).
std::uint32_t __fastcall RandomFlickerHook(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    g_flickerCalls.fetch_add(1);
    std::uint32_t value = 0;
    if (RenderingFrameWithoutStep()) {
        value = guest::IsReadable(self + kFlickerValueOffset, 4) ? guest::Read<std::uint32_t>(self + kFlickerValueOffset)
                                                                : 0u;
        g_flickerKept.fetch_add(1);
        RegisterReporter();
    } else {
        value = FlickerValue(core::CallGuest(kCrtRand));
        guest::Write<std::uint32_t>(self + kFlickerValueOffset, value);
    }
    guest::Write<std::uint8_t>(self + kFlickerFlagOffset, 1);
    return value;  // eax of the original (the stored value)
}

}  // namespace

std::uint32_t FlickerValue(std::uint32_t randResult) noexcept {
    std::uint32_t v = randResult & 0x80000001u;
    if (static_cast<std::int32_t>(v) < 0) {
        v = ((v - 1u) | 0xFFFFFFFEu) + 1u;
    }
    return v;
}

FrameFixStats GetFrameFixStats() noexcept {
    FrameFixStats s;
    s.deathTimeCleared = g_deathCleared.load();
    s.occluderFadeCalls = g_fadeCalls.load();
    s.occluderFadeSkipped = g_fadeSkipped.load();
    s.flickerCalls = g_flickerCalls.load();
    s.flickerKept = g_flickerKept.load();
    return s;
}

void LogFrameFixSummary() noexcept {
    const FrameFixStats s = GetFrameFixStats();
    using ull = unsigned long long;
    FUT_LOGI(Game, "frame fixes (frames between ticks): death screen time added once x %llu; occluder fades %llu "
             "calls, %llu skipped without a step; flicker %llu calls, %llu kept without a step (no rand)",
             static_cast<ull>(s.deathTimeCleared), static_cast<ull>(s.occluderFadeCalls),
             static_cast<ull>(s.occluderFadeSkipped), static_cast<ull>(s.flickerCalls), static_cast<ull>(s.flickerKept));
}

}  // namespace xapi

FUT_PROBE(0x0002BF10, &xapi::DeathRenderProbe, Thiscall, "probe:Death::Render", "56 8B F1 8B 4E 3C", 6);
FUT_PROBE(0x000CD340, &xapi::OccluderFadeProbe, Thiscall, "probe:Session::OccluderFade", "83 EC 3C 56 8B F1", 6);
FUT_HOOK_EX(0x00045EF0, &xapi::RandomFlickerHook, Thiscall, "Session::RandomFlicker", "56 8B F1 E8 D4 09 12 00",
            true);
