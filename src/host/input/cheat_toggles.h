// G8: in-game cheat toggles ([cheats] ingame_toggles, default off; docs/g8_cheats_report.md).
//
// Three D-pad sequences of eight presses switch the cheats of xapi/cheats.h on and off while the character is
// controllable (same conditions as the G7 level skip, inp::levelskip::InputCheatArmed):
//   invulnerable   UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT
//   one-hit kill   LEFT RIGHT LEFT RIGHT UP DOWN UP DOWN
//   enemies ignore DOWN UP DOWN UP RIGHT LEFT RIGHT LEFT
// In a level the D-pad only walks (it is added to the left stick, m4_input.md 5), so a sequence costs a few steps;
// none of them occurs in the G7 sequence (A/B) or in the game's own cheat codes (LT held + face buttons). A press is a
// rising edge of one D-pad direction; two directions pressed in the same tick (diagonal), any other button or analog
// button press, a pause longer than kGapSeconds or a sequence longer than kWindowSeconds start over. Sticks are
// ignored. The last eight presses count. Feedback: a rumble pulse on the port (HostRouter::PulseRumble, scaled and
// muted like the game's rumble by [input] rumble / rumble_strength; long = on, short = off), the game's colour flash
// (green = on, red = off) and the log line "cheat: <name> ON|OFF ...". The presses reach the game.
#pragma once

#include "input/input_api.h"

#include <array>
#include <cstdint>

namespace inp::cheattoggles {

enum class Dir : std::uint8_t { Up, Down, Left, Right };
using Sequence = std::array<Dir, 8>;
inline constexpr Sequence kInvulnerableSequence = {Dir::Up,   Dir::Up,    Dir::Down, Dir::Down,
                                                   Dir::Left, Dir::Right, Dir::Left, Dir::Right};
inline constexpr Sequence kOneHitKillSequence = {Dir::Left, Dir::Right, Dir::Left, Dir::Right,
                                                 Dir::Up,   Dir::Down,  Dir::Up,   Dir::Down};
inline constexpr Sequence kEnemiesIgnoreSequence = {Dir::Down,  Dir::Up,   Dir::Down,  Dir::Up,
                                                    Dir::Right, Dir::Left, Dir::Right, Dir::Left};
// Index = xapi::cheats::Cheat.
inline constexpr std::array<Sequence, 3> kSequences = {kInvulnerableSequence, kOneHitKillSequence,
                                                       kEnemiesIgnoreSequence};
inline constexpr float kWindowSeconds = 5.0f;  // first to last press
inline constexpr float kGapSeconds = 1.0f;     // between two presses

// "UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT"
const char* SequenceText(int index) noexcept;

// Pure detector (unit-tested): one Feed per logic tick of a port.
class DpadSequenceDetector {
public:
    void Configure(std::uint32_t ticksPerSecond) noexcept;
    // True if `state` has a new D-pad press compared with the previous Feed (the caller then checks whether the game
    // is armed).
    bool HasPressEdge(const PadState& state) const noexcept;
    // One tick with the pad state the game gets. `armed` = a sequence may fire now (only evaluated on presses; false
    // clears the presses so far). Returns the index of the completed sequence (0..2, the history is cleared) or -1.
    int Feed(const PadState& state, bool armed) noexcept;
    void Reset() noexcept;

private:
    struct Press {
        Dir dir;
        std::uint64_t tick;
    };
    std::uint32_t windowTicks_ = 300;
    std::uint32_t gapTicks_ = 60;
    std::uint64_t tick_ = 0;
    std::uint16_t prevDpad_ = 0;
    std::uint16_t prevOtherButtons_ = 0;
    std::uint8_t prevAnalog_ = 0;  // analog buttons above the threshold (bit per AnalogButton)
    std::array<Press, 8> history_{};
    std::size_t count_ = 0;  // presses in history_ (oldest first)
};

// Rumble pulse of a toggle: on = 0.4 s, off = 0.15 s (both motors at half speed before [input] rumble_strength).
inline constexpr float kPulseOnSeconds = 0.4f;
inline constexpr float kPulseOffSeconds = 0.15f;
inline constexpr std::uint16_t kPulseMotor = 0x8000;

// Reads [cheats] ingame_toggles (via xapi::cheats::ActiveSettings) and logs it (input start).
void Configure() noexcept;
// The router's chained sample filter (HostRouter::AddSampleFilter).
void SampleFilter(int port, PadState& merged) noexcept;

}  // namespace inp::cheattoggles
