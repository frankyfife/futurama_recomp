// G8: in-game cheat toggles (see cheat_toggles.h).
#include "input/cheat_toggles.h"

#include "core/log.h"
#include "input/input_system.h"
#include "input/level_skip.h"
#include "xapi/cheats.h"
#include "xapi/cheats_player.h"
#include "xapi/game_timing.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace inp::cheattoggles {
namespace {

constexpr std::uint16_t kDpadMask = kButtonDpadUp | kButtonDpadDown | kButtonDpadLeft | kButtonDpadRight;
constexpr std::uint16_t kOtherButtons = static_cast<std::uint16_t>(kButtonMask & ~kDpadMask);

std::uint8_t AnalogBits(const PadState& s) noexcept {
    std::uint8_t bits = 0;
    for (int i = 0; i < kAnalogCount; ++i) {
        if (s.analog[i] >= kAnalogPressThreshold) {
            bits = static_cast<std::uint8_t>(bits | (1u << i));
        }
    }
    return bits;
}

Dir DirOf(std::uint16_t bit) noexcept {
    switch (bit) {
        case kButtonDpadUp:
            return Dir::Up;
        case kButtonDpadDown:
            return Dir::Down;
        case kButtonDpadLeft:
            return Dir::Left;
        default:
            return Dir::Right;
    }
}

std::atomic<bool> g_enabled{false};
std::array<DpadSequenceDetector, kPortCount> g_detectors{};  // router lock (one sample at a time)
std::uint32_t g_configuredRate = 0;

std::atomic<bool> g_moonEnabled{false};
std::array<MoonJumpButtons, kPortCount> g_moonButtons{};  // router lock

}  // namespace

const char* SequenceText(int index) noexcept {
    switch (index) {
        case 0:
            return "UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT";
        case 1:
            return "LEFT RIGHT LEFT RIGHT UP DOWN UP DOWN";
        case 2:
            return "DOWN UP DOWN UP RIGHT LEFT RIGHT LEFT";
        case 3:
            return "RIGHT RIGHT LEFT LEFT UP UP DOWN DOWN";
        case 4:
            return "UP RIGHT DOWN LEFT UP RIGHT DOWN LEFT";
        case 5:
            return "DOWN DOWN UP UP LEFT LEFT RIGHT RIGHT";
        default:
            return "?";
    }
}

void DpadSequenceDetector::Configure(std::uint32_t ticksPerSecond) noexcept {
    const float rate = static_cast<float>(std::max<std::uint32_t>(ticksPerSecond, 1));
    windowTicks_ = static_cast<std::uint32_t>(std::lround(kWindowSeconds * rate));
    gapTicks_ = static_cast<std::uint32_t>(std::lround(kGapSeconds * rate));
}

bool DpadSequenceDetector::HasPressEdge(const PadState& state) const noexcept {
    return ((state.buttons & kDpadMask) & ~prevDpad_) != 0;
}

void DpadSequenceDetector::Reset() noexcept {
    count_ = 0;
}

int DpadSequenceDetector::Feed(const PadState& state, bool armed) noexcept {
    const std::uint64_t now = tick_++;
    const std::uint16_t dpad = static_cast<std::uint16_t>(state.buttons & kDpadMask);
    const std::uint16_t other = static_cast<std::uint16_t>(state.buttons & kOtherButtons);
    const std::uint8_t analog = AnalogBits(state);
    const std::uint16_t newDpad = static_cast<std::uint16_t>(dpad & ~prevDpad_);
    const bool otherPress = (other & ~prevOtherButtons_) != 0 || (analog & ~prevAnalog_) != 0;
    prevDpad_ = dpad;
    prevOtherButtons_ = other;
    prevAnalog_ = analog;

    if (otherPress) {
        count_ = 0;
        return -1;
    }
    if (newDpad == 0) {
        return -1;
    }
    if ((newDpad & (newDpad - 1)) != 0) {
        count_ = 0;  // two directions in the same tick (diagonal)
        return -1;
    }
    if (!armed) {
        count_ = 0;
        return -1;
    }
    if (count_ > 0 && now - history_[count_ - 1].tick > gapTicks_) {
        count_ = 0;  // pause too long: this press starts over
    }
    if (count_ == history_.size()) {
        std::copy(history_.begin() + 1, history_.end(), history_.begin());
        --count_;
    }
    history_[count_++] = {DirOf(newDpad), now};
    if (count_ < history_.size() || history_[count_ - 1].tick - history_[0].tick > windowTicks_) {
        return -1;
    }
    for (std::size_t s = 0; s < kSequences.size(); ++s) {
        bool match = true;
        for (std::size_t i = 0; i < history_.size() && match; ++i) {
            match = history_[i].dir == kSequences[s][i];
        }
        if (match) {
            count_ = 0;
            return static_cast<int>(s);
        }
    }
    return -1;
}

void Configure() noexcept {
    const bool on = xapi::cheats::ActiveSettings().ingameToggles;
    g_enabled.store(on, std::memory_order_release);
    if (on) {
        FUT_LOGI(Game, "cheat: in-game toggles on (D-pad, in a level, 8 presses within %.0f s)",
                 static_cast<double>(kWindowSeconds));
    }
}

void SampleFilter(int port, PadState& merged) noexcept {
    if (!g_enabled.load(std::memory_order_acquire) || port < 0 || port >= kPortCount) {
        return;
    }
    const std::uint32_t rate = xapi::ActiveTickRate();
    if (rate != g_configuredRate) {
        g_configuredRate = rate;
        for (DpadSequenceDetector& d : g_detectors) {
            d.Configure(rate);
        }
    }
    DpadSequenceDetector& d = g_detectors[static_cast<std::size_t>(port)];
    bool armed = true;
    if (d.HasPressEdge(merged)) {
        const char* reason = nullptr;
        armed = levelskip::InputCheatArmed(port, reason);
        if (!armed) {
            FUT_LOGD(Game, "cheat: D-pad press on port %d not counted for the cheat toggles - %s", port + 1, reason);
        }
    }
    const int hit = d.Feed(merged, armed);
    if (hit < 0) {
        return;
    }
    const bool on = xapi::cheats::Toggle(static_cast<xapi::cheats::Cheat>(hit), port, SequenceText(hit));
    const float seconds = on ? kPulseOnSeconds : kPulseOffSeconds;
    GlobalRouter().PulseRumble(port, RumbleState{kPulseMotor, kPulseMotor},
                               static_cast<std::uint32_t>(std::lround(seconds * static_cast<float>(rate))));
}

MoonJumpButtons::Result MoonJumpButtons::Feed(bool jumpHeld, bool whiteHeld) noexcept {
    Result r;
    if (!whiteHeld) {
        whiteHidden_ = false;
    } else if (!prevWhite_ && jumpHeld) {
        whiteHidden_ = true;  // LB pressed while B is held: the moon jump's, not Fry's weapon switch
    }
    prevWhite_ = whiteHeld;
    r.lift = jumpHeld && whiteHeld;
    r.hideWhite = whiteHidden_;
    return r;
}

void ConfigureMoonJump() noexcept {
    const xapi::cheats::Settings& s = xapi::cheats::ActiveSettings();
    const bool on = s.moonJump || s.ingameToggles;
    g_moonEnabled.store(on, std::memory_order_release);
    if (on) {
        FUT_LOGI(Game, "cheat: moon jump buttons LB + B held (keyboard with the default bindings: 1 + Space)%s",
                 s.moonJump ? "" : " - off until toggled (D-pad UP RIGHT DOWN LEFT UP RIGHT DOWN LEFT)");
    }
}

void MoonJumpFilter(int port, PadState& merged) noexcept {
    if (!g_moonEnabled.load(std::memory_order_acquire) || port < 0 || port >= kPortCount) {
        return;
    }
    MoonJumpButtons& b = g_moonButtons[static_cast<std::size_t>(port)];
    const bool jump = merged.analog[kAnalogB] >= kAnalogPressThreshold;
    const bool white = merged.analog[kAnalogWhite] >= kAnalogPressThreshold;
    const MoonJumpButtons::Result r = b.Feed(jump, white);
    const bool active = xapi::cheats::Active(xapi::cheats::Cheat::MoonJump) && !GlobalRouter().Diverted(port);
    xapi::cheats::SetMoonJumpHeld(port, active && r.lift);
    if (active && r.hideWhite) {
        merged.analog[kAnalogWhite] = 0;
    }
}

}  // namespace inp::cheattoggles
