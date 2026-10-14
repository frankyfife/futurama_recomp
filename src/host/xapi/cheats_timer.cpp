// G8: [cheats] freeze_timer (see cheats_timer.h).
#include "xapi/cheats_timer.h"

#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/log.h"
#include "xapi/cheats.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace xapi::cheats {
namespace {

constexpr std::uint32_t kTraceEvery = 60;       // [debug] trace = game: one line per ~1 s (60 Hz ticks)
constexpr std::uint32_t kMaxThreadWalk = 4096;  // safety limit for the scheduler's thread list
// Code offsets above this are no script position (the largest .ful has 339 584 bytes).
constexpr std::uint32_t kMaxScriptImage = 0x200000;
constexpr std::size_t kMaxHeld = 16;

std::uint32_t ReadU32(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
}
std::uint8_t ReadU8(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 1)) ? guest::Read<std::uint8_t>(p) : 0;
}
std::uint32_t FloatBits(float f) noexcept {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

// Level name of the session ("level4-1"), empty without a level.
std::string LevelName() {
    std::string s;
    const guest::GuestAddr session = ReadU32(kSessionPtr);
    const guest::GuestAddr p = session != 0 ? ReadU32(session + kSessionLevelName) : 0;
    for (std::uint32_t i = 0; p >= 0x10000 && i < 15 && guest::IsReadable(p + i, 1); ++i) {
        const char c = guest::Read<char>(p + i);
        if (c == 0) {
            break;
        }
        s += c;
    }
    return s;
}

bool SameLevel(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool LevelHasSites(std::string_view level) noexcept {
    for (const ScriptCountdownSite& s : kScriptCountdowns) {
        if (SameLevel(level, s.level)) {
            return true;
        }
    }
    return false;
}

// ---- state (guest main thread only: both probes run there) -------------------------------------------
bool g_hudHeldLogged = false;  // "HUD timer held" logged for the current freeze
float g_hudHeldAt = 0.0f;
std::uint32_t g_hudCalls = 0;
std::uint32_t g_schedCalls = 0;

struct HeldThread {
    guest::GuestAddr thread = 0;
    std::uint32_t code = 0;
    float wait = 0.0f;
};
std::array<guest::GuestAddr, kMaxHeld> g_loggedThreads{};  // threads whose hold was logged (current freeze)
std::size_t g_loggedCount = 0;

bool Logged(guest::GuestAddr t) noexcept {
    for (std::size_t i = 0; i < g_loggedCount; ++i) {
        if (g_loggedThreads[i] == t) {
            return true;
        }
    }
    return false;
}

// HUD timer update FUN_000EC6D0 (vtable 0x2A55F8 +0x4), thiscall(timer), plain ret.
std::uint32_t __fastcall HudTimerUpdateProbe(std::uint32_t timer, std::uint32_t /*edx*/) noexcept {
    const bool freeze = Active(Cheat::FreezeTimer);
    bool counting = false;
    float seconds = 0.0f;
    std::uint8_t expired = 0;
    if (timer != 0 && guest::IsReadable(timer, kHudTimerExpired + 1)) {
        counting = HudTimerCounting(guest::Read<std::uint8_t>(timer + kHudTimerRunning) != 0,
                                    guest::Read<std::uint8_t>(timer + kHudTimerPaused) != 0);
        seconds = guest::Read<float>(timer + kHudTimerSeconds);
        expired = guest::Read<std::uint8_t>(timer + kHudTimerExpired);
    }
    try {
        if (counting && core::LogEnabled(core::Channel::Game, core::Level::Trace) &&
            g_hudCalls++ % kTraceEvery == 0) {
            char text[16];
            FormatHudTime(seconds, text);
            FUT_LOGT(Game, "timer trace: HUD timer %s (%.2f s, %s%s)", text, static_cast<double>(seconds),
                     freeze ? "frozen" : "counting",
                     ReadU8(timer + kHudTimerSwamp) != 0 ? ", swamp clock" : "");
        }
        if (!freeze && g_hudHeldLogged) {
            g_hudHeldLogged = false;
            char text[16];
            FormatHudTime(seconds, text);
            FUT_LOGI(Game, "cheat: freeze timer - HUD timer runs again from %s (%.2f s; held at %.2f s)",
                     text, static_cast<double>(seconds), static_cast<double>(g_hudHeldAt));
        }
    } catch (...) {
    }
    if (freeze && counting) {
        const std::uint32_t n = CountPlayerCheat(&Stats::hudTimerTicksHeld);
        if (!g_hudHeldLogged) {
            g_hudHeldLogged = true;
            g_hudHeldAt = seconds;
            CountPlayerCheat(&Stats::countdownsHeld);
            try {
                char text[16];
                FormatHudTime(seconds, text);
                FUT_LOGI(Game, "cheat: freeze timer - HUD timer held at %s (%.2f s, %s; %u. tick)", text,
                         static_cast<double>(seconds),
                         ReadU8(timer + kHudTimerSwamp) != 0 ? "4-1 swamp clock, cannot run out"
                                                             : "level countdown display",
                         n);
            } catch (...) {
            }
        }
        return 0;  // the update only counts down (and sets the expired flag at 0)
    }
    const std::uint32_t r = core::CallGuestThiscall(core::HookTrampoline(kHudTimerUpdate), timer, {});
    if (counting && expired == 0 && ReadU8(timer + kHudTimerExpired) != 0) {
        FUT_LOGI(Game,
                 "cheat: freeze timer off - the HUD timer ran out (expired flag set, script native 0x184)");
    }
    return r;
}

// One pass over the scheduler's threads: the threads of the current level waiting at a countdown site. With
// `hold`, their wait is raised by dt (written back exactly after the scheduler, see RestoreHeld).
std::size_t CollectSites(guest::GuestAddr script, float dt, bool hold, bool trace,
                         std::array<HeldThread, kMaxHeld>& held) {
    const std::string level = LevelName();
    if (level.empty() || !LevelHasSites(level)) {
        return 0;
    }
    const guest::GuestAddr image = ReadU32(script + kScriptImage);
    if (image == 0 || ReadU32(image) != kScriptImageMagic) {
        return 0;
    }
    std::size_t n = 0;
    guest::GuestAddr t = ReadU32(script + kScriptThreadList);
    for (std::uint32_t i = 0; t != 0 && i < kMaxThreadWalk; ++i, t = ReadU32(t + kThreadNext)) {
        if (!guest::IsReadable(t, kThreadNext + 4) || ReadU8(t + kThreadDone) != 0 ||
            ReadU8(t + kThreadError) != 0) {
            continue;
        }
        const std::uint32_t code = ReadU32(t + kThreadCode);
        const std::uint32_t offset = code - image;
        if (code < image || offset < 12 || offset >= kMaxScriptImage || !guest::IsReadable(code - 12, 12)) {
            continue;
        }
        const std::uint32_t words[3] = {guest::Read<std::uint32_t>(code - 12),
                                        guest::Read<std::uint32_t>(code - 8),
                                        guest::Read<std::uint32_t>(code - 4)};
        const ScriptCountdownSite* site = FindCountdownSite(level, offset, words);
        if (site == nullptr) {
            continue;
        }
        const float wait = guest::Read<float>(t + kThreadWait);
        if (trace) {
            FUT_LOGT(Game, "timer trace: %s %s %s - wait %.2f s (%s)", site->shown, site->level,
                     site->function, static_cast<double>(wait), hold ? "frozen" : "counting");
        }
        if (!hold || n >= held.size()) {
            continue;
        }
        const float before = HeldWaitBeforeStep(wait, dt);
        guest::Write<float>(t + kThreadWait, before);
        held[n++] = {t, code, wait};
        CountPlayerCheat(&Stats::scriptWaitTicksHeld);
        if (!Logged(t) && g_loggedCount < g_loggedThreads.size()) {
            g_loggedThreads[g_loggedCount++] = t;
            const std::uint32_t k = CountPlayerCheat(&Stats::countdownsHeld);
            FUT_LOGI(Game, "cheat: freeze timer - %s (%s, %s): %s held with %.2f s left (thread 0x%08X; %u.)",
                     site->shown, site->level, site->function, site->what, static_cast<double>(wait), t, k);
        }
    }
    return n;
}

// Writes the held waits back exactly (only threads still in the list at the same code position).
void RestoreHeld(guest::GuestAddr script, const std::array<HeldThread, kMaxHeld>& held, std::size_t count) {
    guest::GuestAddr t = ReadU32(script + kScriptThreadList);
    for (std::uint32_t i = 0; t != 0 && i < kMaxThreadWalk; ++i, t = ReadU32(t + kThreadNext)) {
        for (std::size_t k = 0; k < count; ++k) {
            if (held[k].thread == t && guest::IsReadable(t, kThreadNext + 4) &&
                ReadU32(t + kThreadCode) == held[k].code) {
                guest::Write<float>(t + kThreadWait, held[k].wait);
            }
        }
    }
}

// Script scheduler FUN_000BA060, thiscall(script, float dt) ret 4: every live thread's wait and timer -= dt,
// threads at <= 0 run.
std::uint32_t __fastcall ScriptRunThreadsProbe(std::uint32_t script, std::uint32_t /*edx*/,
                                               float dt) noexcept {
    const bool freeze = Active(Cheat::FreezeTimer);
    const bool trace = core::LogEnabled(core::Channel::Game, core::Level::Trace) && dt > 0.0f &&
                       g_schedCalls++ % kTraceEvery == 0;
    std::array<HeldThread, kMaxHeld> held{};
    std::size_t count = 0;
    if (script != 0 && ((freeze && dt > 0.0f) || trace)) {
        try {
            count = CollectSites(script, dt, freeze && dt > 0.0f, trace, held);
        } catch (...) {
        }
    }
    if (!freeze && g_loggedCount != 0) {
        try {
            FUT_LOGI(Game, "cheat: freeze timer - %u script countdown(s) run again",
                     static_cast<unsigned>(g_loggedCount));
        } catch (...) {
        }
        g_loggedCount = 0;
    }
    const std::uint32_t r =
        core::CallGuestThiscall(core::HookTrampoline(kScriptRunThreads), script, {FloatBits(dt)});
    if (count != 0) {
        try {
            RestoreHeld(script, held, count);
        } catch (...) {
        }
    }
    return r;
}

}  // namespace

bool TimerHooksWanted(const core::Config& config) noexcept {
    const Settings s = SettingsFromConfig(config);
    return s.freezeTimer || s.ingameToggles;
}

bool HudTimerCounting(bool running, bool paused) noexcept {
    return running && !paused;
}

const ScriptCountdownSite* FindCountdownSite(std::string_view level, std::uint32_t codeOffset,
                                             const std::uint32_t (&words)[3]) noexcept {
    for (const ScriptCountdownSite& s : kScriptCountdowns) {
        if (s.code == codeOffset && SameLevel(level, s.level) && words[0] == s.words[0] &&
            words[1] == s.words[1] && words[2] == s.words[2]) {
            return &s;
        }
    }
    return nullptr;
}

float HeldWaitBeforeStep(float wait, float dt) noexcept {
    if (!(dt > 0.0f) || !std::isfinite(dt) || !std::isfinite(wait)) {
        return wait;
    }
    return wait + dt;
}

void FormatHudTime(float seconds, char (&out)[16]) noexcept {
    int s = 0;
    if (std::isfinite(seconds) && seconds > 0.0f) {
        s = seconds >= 2147483520.0f ? 2147483647
                                     : static_cast<int>(seconds);  // truncated like the game's _ftol
    }
    std::snprintf(out, sizeof(out), "%d:%02d", s / 60, s % 60);
}

void LogTimerSummary() noexcept {
    const Stats s = GetStats();
    FUT_LOGI(Game,
             "cheats summary (timer): freeze timer %s (%u countdowns held, %u HUD timer updates skipped, %u "
             "script "
             "waits held)",
             Active(Cheat::FreezeTimer) ? "on" : "off", s.countdownsHeld, s.hudTimerTicksHeld,
             s.scriptWaitTicksHeld);
}

}  // namespace xapi::cheats

// Installed only with [cheats] freeze_timer = 1 or ingame_toggles = 1 (core::HookDesc::condition). Prologues
// checked with capstone (g8_cheats.md 10.3): whole position-independent instructions, no branch into them.
FUT_PROBE_IF(xapi::cheats::kHudTimerUpdate, &xapi::cheats::HudTimerUpdateProbe, Thiscall,
             "probe:HudTimer::Update", "8A 41 2C 84 C0", 5, &xapi::cheats::TimerHooksWanted);
FUT_PROBE_IF(xapi::cheats::kScriptRunThreads, &xapi::cheats::ScriptRunThreadsProbe, Thiscall,
             "probe:Script::RunThreads", "51 57 8B F9 8B 0D 18 A2 2E 00", 10,
             &xapi::cheats::TimerHooksWanted);
