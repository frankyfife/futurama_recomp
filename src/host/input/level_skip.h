// G7: level skip cheat ([game] level_skip_cheat, default on; docs/g5_g7_report.md).
//
// The pad sequence A A B B A A B B A A, entered in a level while the player character is controllable, ends the level
// the regular way: the level-script native FsChangeLevel FUN_000B0520 with the level's own successor (the argument
// of opcode 0xA1 in scripts\levelN-M.ful, g4_select.md 2.1/2.2) - the same call the level script makes when the
// player finishes the level: level time and "everything collected" flag into the extras table, state text for the
// next level, statistics screen "Breakdown" with Speichern / Weiter / Wiederholen / Beenden (m7_saves.md 1). The
// call runs at the next tick heartbeat (xapi::RequestLevelEnd, the M7 debug action behind `game level_end`).
//
// Detection on the host side: a filter in the input router's sample chain (HostRouter::AddSampleFilter, after the
// freecam filter) sees the merged pad of each port once per logic tick. A press is a rising edge of the analog A/B
// button (from the game's channel threshold 30/256 on); presses must be separated by a release of that button (A A
// needs A released in between). Any other button press, a pause longer than kGapSeconds or a sequence longer than
// kWindowSeconds starts over; the last ten presses count (A A A B B ... still works). Sticks are ignored. Armed only
// while: Ingame, level loaded, player input enabled (+0x630), no cut scene, no pause, no save dialog, no Breakdown
// screen, port not diverted (freecam). The A/B presses also reach the game (jump/attack), like the game's own cheat
// codes (g4_select.md 2.4).
#pragma once

#include "input/input_api.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace inp::levelskip {

enum class Key : std::uint8_t { A, B };
inline constexpr std::array<Key, 10> kSequence = {Key::A, Key::A, Key::B, Key::B, Key::A,
                                                  Key::A, Key::B, Key::B, Key::A, Key::A};
inline constexpr float kWindowSeconds = 4.0f;  // first to last press
inline constexpr float kGapSeconds = 1.0f;     // between two presses

// Pure detector (unit-tested): one Feed per logic tick of a port.
class SequenceDetector {
public:
    void Configure(std::uint32_t ticksPerSecond) noexcept;
    // True if `state` has a new press of A or B compared with the previous Feed (the caller then checks whether the
    // game is armed).
    bool HasPressEdge(const PadState& state) const noexcept;
    // One tick with the pad state the game gets. `armed` = the cheat may fire now (only evaluated on presses; false
    // clears the presses so far). Returns true on the tick that completes the sequence (the history is cleared).
    bool Feed(const PadState& state, bool armed) noexcept;
    void Reset() noexcept;
    // Presses of the sequence matched so far (the longest suffix of the recent presses that is a prefix of
    // kSequence), 0..9.
    int Progress() const noexcept;

private:
    struct Press {
        Key key;
        std::uint64_t tick;
    };
    std::uint32_t windowTicks_ = 240;
    std::uint32_t gapTicks_ = 60;
    std::uint64_t tick_ = 0;
    bool prevA_ = false;
    bool prevB_ = false;
    std::uint16_t prevButtons_ = 0;
    std::uint8_t prevOther_ = 0;  // analog buttons other than A/B above the threshold (bit per AnalogButton)
    std::array<Press, kSequence.size()> history_{};
    std::size_t count_ = 0;  // presses in history_ (oldest first)
};

// The level the game loads after `level` (case-insensitive folder name "LevelN-M"): the FsChangeLevel argument of
// the level's script. nullptr for unknown names. Level5-3 -> Level1-1 (its script plays the credits first),
// Level3-5 (unreachable) -> Level3-3.
const char* NextLevel(std::string_view level) noexcept;

// True if a cheat sequence may fire on `port` now (shared with the G8 cheat toggles, input/cheat_toggles.h): Ingame,
// level loaded, character controllable (player input on, no cut scene, pause or save dialog), no statistics screen,
// port not diverted (freecam). Otherwise `reason` names the first failed condition (for debug lines).
bool InputCheatArmed(int port, const char*& reason) noexcept;

// [game] level_skip_cheat (bool, default on).
bool EnabledInConfig() noexcept;
// Reads the option and logs it (input start). The filter does nothing while the option is off.
void Configure() noexcept;
// The router's chained sample filter (HostRouter::AddSampleFilter).
void SampleFilter(int port, PadState& merged) noexcept;

}  // namespace inp::levelskip
