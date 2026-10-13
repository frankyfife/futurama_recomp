// G7: level skip cheat (see level_skip.h).
#include "input/level_skip.h"

#include "core/config.h"
#include "core/guest.h"
#include "core/log.h"
#include "input/input_script.h"
#include "input/input_system.h"
#include "xapi/game_timing.h"
#include "xapi/save_probes.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

namespace inp::levelskip {
namespace {

constexpr std::uint16_t kOtherButtons = kButtonMask;  // every digital button (D-pad, START, BACK, stick clicks)

// Successor of each level = the only level name in scripts\levelN-M.ful (FutPAL01.img), the argument of opcode 0xA1
// FsChangeLevel (g4_select.md 2.1; checked again for G7: g5_g7_report.md 3.1).
struct NextLevelRow {
    const char* level;
    const char* next;
};
constexpr NextLevelRow kNextLevels[] = {
    {"Level1-1", "Level1-2"}, {"Level1-2", "Level1-3"}, {"Level1-3", "Level1-4"}, {"Level1-4", "Level1-5"},
    {"Level1-5", "Level1-7"}, {"Level1-7", "Level1-6"}, {"Level1-6", "Level2-1"}, {"Level2-1", "Level2-6"},
    {"Level2-6", "Level2-2"}, {"Level2-2", "Level2-3"}, {"Level2-3", "Level2-4"}, {"Level2-4", "Level2-5"},
    {"Level2-5", "Level3-1"}, {"Level3-1", "Level3-2"}, {"Level3-2", "Level3-3"}, {"Level3-3", "Level3-6"},
    {"Level3-6", "Level3-4"}, {"Level3-4", "Level4-1"}, {"Level3-5", "Level3-3"}, {"Level4-1", "Level5-1"},
    {"Level5-1", "Level5-2"}, {"Level5-2", "Level5-3"}, {"Level5-3", "Level1-1"},
};

// Guest state (retail EU; guest.h kGameLevelPtr / kGameBreakdownPtr, m7_saves.md 1).
constexpr guest::GuestAddr kLevelPtr = 0x002DABC4;  // +0xC char* level name
constexpr std::uint32_t kLevelNameOffset = 0x0C;
constexpr guest::GuestAddr kBreakdownPtr = 0x002E8E4C;  // Breakdown screen; byte +1 = open
constexpr std::uint32_t kBreakdownOpenOffset = 1;

char Lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::uint32_t ReadU32(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
}
std::uint8_t ReadU8(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 1)) ? guest::Read<std::uint8_t>(p) : 0;
}

std::string CurrentLevelName() {
    const guest::GuestAddr level = ReadU32(kLevelPtr);
    const guest::GuestAddr name = level != 0 ? ReadU32(level + kLevelNameOffset) : 0;
    std::string s;
    for (std::uint32_t i = 0; name != 0 && i < 16; ++i) {
        if (!guest::IsReadable(name + i, 1)) {
            break;
        }
        const char c = guest::Read<char>(name + i);
        if (c == 0) {
            break;
        }
        s += c;
    }
    return s;
}

bool BreakdownOpen() noexcept {
    const guest::GuestAddr b = ReadU32(kBreakdownPtr);
    return b != 0 && ReadU8(b + kBreakdownOpenOffset) != 0;
}

std::atomic<bool> g_enabled{false};
std::array<SequenceDetector, kPortCount> g_detectors{};  // router lock (one sample at a time)
std::uint32_t g_configuredRate = 0;
std::uint32_t g_triggers = 0;

bool Armed(int port) noexcept {
    const char* reason = nullptr;
    if (!InputCheatArmed(port, reason)) {
        FUT_LOGD(Game, "cheat: press on port %d not counted for the level skip - %s", port + 1, reason);
        return false;
    }
    return true;
}

void Trigger(int port) noexcept {
    try {
        const std::string level = CurrentLevelName();
        const char* next = NextLevel(level);
        if (next == nullptr) {
            FUT_LOGW(Game, "cheat: level skip (A A B B A A B B A A, port %d) in an unknown level \"%s\" - ignored",
                     port + 1, level.c_str());
            return;
        }
        if (!xapi::RequestLevelEnd(next, /*guaranteeStartWeapons=*/true)) {
            FUT_LOGW(Game, "cheat: level skip in %s not possible now (a level end is already pending)", level.c_str());
            return;
        }
        ++g_triggers;
        FUT_LOGI(Game,
                 "cheat: level skip (A A B B A A B B A A, port %d) in %s - level end via the script native "
                 "FsChangeLevel(\"%s\") like the level script (%u. skip of this session)",
                 port + 1, level.c_str(), next, g_triggers);
    } catch (...) {
    }
}

}  // namespace

void SequenceDetector::Configure(std::uint32_t ticksPerSecond) noexcept {
    const float rate = static_cast<float>(std::max<std::uint32_t>(ticksPerSecond, 1));
    windowTicks_ = static_cast<std::uint32_t>(std::lround(kWindowSeconds * rate));
    gapTicks_ = static_cast<std::uint32_t>(std::lround(kGapSeconds * rate));
}

bool SequenceDetector::HasPressEdge(const PadState& state) const noexcept {
    const bool a = state.analog[kAnalogA] >= kAnalogPressThreshold;
    const bool b = state.analog[kAnalogB] >= kAnalogPressThreshold;
    return (a && !prevA_) || (b && !prevB_);
}

void SequenceDetector::Reset() noexcept {
    count_ = 0;
}

bool SequenceDetector::Feed(const PadState& state, bool armed) noexcept {
    const std::uint64_t now = tick_++;
    const bool a = state.analog[kAnalogA] >= kAnalogPressThreshold;
    const bool b = state.analog[kAnalogB] >= kAnalogPressThreshold;
    std::uint8_t other = 0;
    for (int i = 0; i < kAnalogCount; ++i) {
        if (i != kAnalogA && i != kAnalogB && state.analog[i] >= kAnalogPressThreshold) {
            other = static_cast<std::uint8_t>(other | (1u << i));
        }
    }
    const std::uint16_t buttons = static_cast<std::uint16_t>(state.buttons & kOtherButtons);
    const bool newA = a && !prevA_;
    const bool newB = b && !prevB_;
    const bool otherPress = (buttons & ~prevButtons_) != 0 || (other & ~prevOther_) != 0;
    prevA_ = a;
    prevB_ = b;
    prevButtons_ = buttons;
    prevOther_ = other;

    if (otherPress || (newA && newB)) {
        count_ = 0;  // another control, or A and B together
        return false;
    }
    if (!newA && !newB) {
        return false;
    }
    if (!armed) {
        count_ = 0;
        return false;
    }
    if (count_ > 0 && now - history_[count_ - 1].tick > gapTicks_) {
        count_ = 0;  // pause too long: this press starts over
    }
    if (count_ == history_.size()) {
        std::copy(history_.begin() + 1, history_.end(), history_.begin());
        --count_;
    }
    history_[count_++] = {newA ? Key::A : Key::B, now};
    if (count_ < kSequence.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kSequence.size(); ++i) {
        if (history_[i].key != kSequence[i]) {
            return false;
        }
    }
    if (history_[count_ - 1].tick - history_[0].tick > windowTicks_) {
        return false;
    }
    count_ = 0;
    return true;
}

int SequenceDetector::Progress() const noexcept {
    for (std::size_t len = std::min(count_, kSequence.size() - 1); len > 0; --len) {
        bool match = true;
        for (std::size_t i = 0; i < len && match; ++i) {
            match = history_[count_ - len + i].key == kSequence[i];
        }
        if (match) {
            return static_cast<int>(len);
        }
    }
    return 0;
}

bool InputCheatArmed(int port, const char*& reason) noexcept {
    reason = nullptr;
    if (GlobalRouter().Diverted(port)) {
        reason = "port diverted (freecam)";
    } else if (ReadU32(kLevelPtr) == 0) {
        reason = "no level";
    } else if (BreakdownOpen()) {
        reason = "statistics screen open";
    } else if (!script::GameControllable()) {
        reason = "character not controllable (menu, cut scene, pause, loading)";
    }
    return reason == nullptr;
}

const char* NextLevel(std::string_view level) noexcept {
    for (const NextLevelRow& r : kNextLevels) {
        if (IEquals(level, r.level)) {
            return r.next;
        }
    }
    return nullptr;
}

bool EnabledInConfig() noexcept {
    try {
        const core::Config& cfg = core::GetConfig();
        if (!cfg.Has("cheats", "level_skip") && cfg.Has("game", "level_skip_cheat")) {
            return cfg.GetBool("game", "level_skip_cheat", true);  // before G8: [game] level_skip_cheat
        }
        return cfg.GetBool("cheats", "level_skip", true);
    } catch (...) {
        return true;
    }
}

void Configure() noexcept {
    const bool on = EnabledInConfig();
    g_enabled.store(on, std::memory_order_release);
    FUT_LOGI(Game, "cheat: level skip A A B B A A B B A A %s", on ? "on (in a level, within 4 s)" : "off");
}

void SampleFilter(int port, PadState& merged) noexcept {
    if (!g_enabled.load(std::memory_order_acquire) || port < 0 || port >= kPortCount) {
        return;
    }
    const std::uint32_t rate = xapi::ActiveTickRate();
    if (rate != g_configuredRate) {
        g_configuredRate = rate;
        for (SequenceDetector& d : g_detectors) {
            d.Configure(rate);
        }
    }
    SequenceDetector& d = g_detectors[static_cast<std::size_t>(port)];
    const bool armed = d.HasPressEdge(merged) ? Armed(port) : true;
    if (d.Feed(merged, armed)) {
        Trigger(port);
    }
}

}  // namespace inp::levelskip
