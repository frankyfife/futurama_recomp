// [game] skip_videos (see movie_skip.h).
#include "video/movie_skip.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/log.h"

#include <intrin.h>

#include <atomic>
#include <string>

namespace vid {
namespace {

std::atomic<std::uint32_t> g_asked{0}, g_originalNo{0}, g_forced{0};
std::atomic<int> g_mode{-1};  // -1 = not read yet

bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        x = (x >= 'A' && x <= 'Z') ? static_cast<char>(x - 'A' + 'a') : x;
        y = (y >= 'A' && y <= 'Z') ? static_cast<char>(y - 'A' + 'a') : y;
        if (x != y) {
            return false;
        }
    }
    return true;
}

SkipVideos Mode() noexcept {
    int m = g_mode.load(std::memory_order_relaxed);
    if (m < 0) {
        m = static_cast<int>(SkipVideosFromConfig());
        g_mode.store(m, std::memory_order_relaxed);
        FUT_LOGI(Vid, "movies: [game] skip_videos = %s (cut scenes %s)", SkipVideosName(static_cast<SkipVideos>(m)),
                 m == static_cast<int>(SkipVideos::Always) ? "always skippable with A/START"
                                                           : "skippable as in the original");
    }
    return static_cast<SkipVideos>(m);
}

// FUN_000D3120 cut-scene skip condition (thiscall ecx = cut-scene manager, al = result; guest.h
// kGameCutsceneSkipAllowed). The original runs through the trampoline (its timer calls have no side effects).
std::uint32_t __fastcall CutsceneSkipAllowedProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    const std::uint32_t eax =
        core::CallGuestThiscall(core::HookTrampoline(guest::kGameCutsceneSkipAllowed), self, {});
    const bool original = (eax & 0xFF) != 0;
    const SkipVideos mode = Mode();
    const bool allowed = CutsceneSkipAllowed(mode, original);
    g_asked.fetch_add(1, std::memory_order_relaxed);
    if (!original) {
        g_originalNo.fetch_add(1, std::memory_order_relaxed);
    }
    if (allowed && !original) {
        g_forced.fetch_add(1, std::memory_order_relaxed);
    }
    FUT_LOGI(Vid, "movies: cut-scene skip asked (manager 0x%08X, from 0x%08X): original %s -> %s", self,
             guest::Addr(_ReturnAddress()), original ? "yes" : "no (first viewing or < 2.5 s)",
             allowed ? "skip" : "keep playing");
    return allowed ? ((eax & ~0xFFu) | 1u) : (eax & ~0xFFu);
}

}  // namespace

bool ParseSkipVideos(std::string_view text, SkipVideos& out) noexcept {
    if (IEquals(text, "always")) {
        out = SkipVideos::Always;
        return true;
    }
    if (IEquals(text, "original")) {
        out = SkipVideos::Original;
        return true;
    }
    return false;
}

const char* SkipVideosName(SkipVideos mode) noexcept {
    return mode == SkipVideos::Always ? "always" : "original";
}

SkipVideos SkipVideosFromConfig() noexcept {
    try {
        const std::string text =
            core::GetConfig().GetString("game", "skip_videos", core::ConfigKeyDefault("game", "skip_videos"));
        SkipVideos mode = SkipVideos::Always;
        if (!text.empty() && !ParseSkipVideos(text, mode)) {
            FUT_LOGW(Vid, "movies: invalid [game] skip_videos '%s' (expected always or original) - using always",
                     text.c_str());
            mode = SkipVideos::Always;
        }
        return mode;
    } catch (...) {
        return SkipVideos::Always;
    }
}

SkipStats GetSkipStats() noexcept {
    return SkipStats{g_asked.load(), g_originalNo.load(), g_forced.load()};
}

}  // namespace vid

FUT_PROBE(guest::kGameCutsceneSkipAllowed, &vid::CutsceneSkipAllowedProbe, Thiscall, "probe:Game::CutsceneSkipAllowed",
          "A0 D4 B7 2E 00", 5);
