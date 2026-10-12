// M4b: detects a two-button chord (freecam toggle L3+R3) in the merged pad state of a port before the game sees it.
//
// The game uses R3 on its own (first-person view, analysis\loader\m4_input.md 1), so a chord must not leak a single
// click: a newly pressed chord button is held back for up to `windowTicks` ticks. If the other button follows within
// the window, the chord fires and neither click reaches the game until both are released. If the window expires the
// held button is passed on (the game sees it `windowTicks` late); a tap shorter than the window is passed on as a
// one-tick press when it is released, so the game still registers it.
#pragma once

#include "input/input_api.h"

#include <cstdint>

namespace inp {

class ChordFilter {
public:
    ChordFilter(std::uint16_t chord = kButtonLeftThumb | kButtonRightThumb, int windowTicks = 4) noexcept
        : chord_(chord), window_(windowTicks) {}

    // Filters one tick in place. True on the tick the chord fires.
    bool Filter(PadState& state) noexcept {
        const std::uint16_t held = static_cast<std::uint16_t>(state.buttons & chord_);
        state.buttons = static_cast<std::uint16_t>(state.buttons & ~chord_);
        if (latched_) {  // after a chord: nothing of it until both are released
            if (held == 0) {
                latched_ = false;
            }
            return false;
        }
        if (held == chord_) {
            latched_ = true;
            pending_ = 0;
            passing_ = 0;
            return true;
        }
        std::uint16_t out = static_cast<std::uint16_t>(held & passing_);  // already passed on, still held
        passing_ = static_cast<std::uint16_t>(passing_ & held);
        const std::uint16_t released = static_cast<std::uint16_t>(pending_ & ~held);
        if (released != 0) {  // a short tap: give the game a one-tick press now
            out = static_cast<std::uint16_t>(out | released);
            pending_ = static_cast<std::uint16_t>(pending_ & ~released);
        }
        const std::uint16_t fresh = static_cast<std::uint16_t>(held & ~passing_ & ~pending_);
        if (fresh != 0) {
            if (pending_ == 0) {
                age_ = 0;
            }
            pending_ = static_cast<std::uint16_t>(pending_ | fresh);
        }
        if (pending_ != 0 && ++age_ > window_) {  // window over: pass the held button on
            passing_ = static_cast<std::uint16_t>(passing_ | pending_);
            out = static_cast<std::uint16_t>(out | pending_);
            pending_ = 0;
        }
        state.buttons = static_cast<std::uint16_t>(state.buttons | out);
        return false;
    }

    void Reset() noexcept {
        pending_ = passing_ = 0;
        age_ = 0;
        latched_ = false;
    }

private:
    std::uint16_t chord_;
    int window_;
    std::uint16_t pending_ = 0;  // pressed, held back, waiting for the rest of the chord
    std::uint16_t passing_ = 0;  // given to the game (window expired) until released
    int age_ = 0;
    bool latched_ = false;
};

}  // namespace inp
