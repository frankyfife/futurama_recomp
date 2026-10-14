// G8 (2026-10-14): [cheats] freeze_timer - holds every gameplay countdown of the game
// (analysis/loader/g8_cheats.md section 10, docs/g8_cheats_report.md section 9). Default off, no hook.
//
// The game has two kinds of countdowns (survey of all 23 level scripts and the HUD, g8_cheats.md 10.1):
//
// 1. The HUD timer "timer" (class vtable 0x2A55F8, 0x54 bytes, built for every level by the HUD setup
//    FUN_000C1260; 4-1 gets the swamp variant FUN_000ECB70(1): bar HUD\swamp_bar.nif, clock
//    swamp_cl.nif, flag swamp_fl.nif). The level scripts drive it with the HUD message native 0x17C
//    ("timer", command, float): "set" (+0x28 = seconds, clears the expired flag), "add", "play"
//    (+0x2C = 1, +0x2D = 0), "pause" (+0x2D = 1), "stop" (+0x2C = 0). Its update FUN_000EC6D0
//    (vtable +0x4, thiscall, plain ret) counts +0x28 down by the tick dt [0x2D6748] while it is running
//    and not paused, stops at 0 and - swamp variant only - sets the expired flag +0x2F, which the script
//    native 0x184 (FUN_000B09E0) returns. The display (vtable +0x8, FUN_000EC920) prints "%d:%02d".
//    - 4-1 Bogad Swamp (Level4-1): 2:00 per stretch ("aStartTimer": set 120, play; set to 120 again at
//      the checkpoint beams, "actGetTime"); "aRestart" polls native 0x184 and, when the clock has run
//      out, fades out, plays 4-2-01.avi and restarts the level (native 0xA0).
//    - 2-4 Junkyard 1 (Level2-3) and 3-5 Inner Temple (Level3-4) show it next to their own script
//      countdowns (2.).
//    probe:HudTimer::Update (0xEC6D0): with the cheat on and the timer running, the update is skipped -
//    the time and the display stand still, the expired flag is never set. "set"/"add" still apply.
//
// 2. Script countdowns: the level script thread waits (opcode 0x18: thread +0x20 = seconds; the
//    scheduler FUN_000BA060 subtracts the tick dt from +0x20 of every thread and runs it at <= 0) and
//    then does the "time is up" part. Table kScriptCountdowns: the wait sites (offset of the thread's
//    code pointer +0x34 in the loaded .ful image after the wait; the push and wait words in front of it
//    as a signature).
//    - 2-4 (Level2-3) "@@action@@aEnablePowerConsole": the power console opens the gate for 60 s (HUD
//      1:00, "Timer_ticks_loop"), Wait(60), then the gate closes again.
//    - 3-5 (Level3-4) "RunNibbler" (room 9): 55 s in steps of Wait(1) (HUD 0:55), then "NibblerB1" is
//      switched off and a door is locked as soon as the player is in the room trigger.
//    - 5-1, 5-2, 5-3 (Level5-x) "Start" of the puzzle timer: ten "time_light" lamps, one every
//      m_Timeout seconds (4.5 s unless SetTimeout), then "Puzzle_TimedOut" and the trap door opens.
//    probe:Script::RunThreads (0xBA060, thiscall(script, float dt) ret 4): before the scheduler runs, a
//    thread of the current level that waits at one of these sites gets dt added to its wait (so the
//    scheduler's subtraction leaves it unchanged); afterwards the exact value is written back. The
//    thread stays parked, the rest of the script runs normally.
//
// Everything else that waits for a long time in the scripts is no countdown (dialogue/hint delays,
// camera sequences, timed platforms and doors, the 5-x fan cycle; g8_cheats.md 10.2) and is not touched.
#pragma once

#include "core/config.h"
#include "core/guest.h"

#include <cstdint>
#include <string_view>

namespace xapi::cheats {

// ---- guest addresses (retail EU XBE; g8_cheats.md 10) -----------------------------------------------
inline constexpr guest::GuestAddr kHudTimerVtable = 0x002A55F8;   // HUD element "timer" (FUN_000ECB70)
inline constexpr guest::GuestAddr kHudTimerUpdate = 0x000EC6D0;   // vtable +0x4, thiscall(timer), plain ret
inline constexpr guest::GuestAddr kHudTimerCommand = 0x000EC720;  // +0x10, thiscall(cmd, float) ret 8
inline constexpr std::uint32_t kHudTimerSeconds = 0x28;           // float, counts down
inline constexpr std::uint32_t kHudTimerRunning = 0x2C;           // u8 ("play"; "stop" clears)
inline constexpr std::uint32_t kHudTimerPaused = 0x2D;            // u8 ("pause")
inline constexpr std::uint32_t kHudTimerSwamp = 0x2E;             // u8: 4-1 variant (bar, clock, flag)
inline constexpr std::uint32_t kHudTimerExpired = 0x2F;  // u8: set at 0 (swamp variant), script native 0x184
inline constexpr guest::GuestAddr kTickDeltaVar = 0x002D6748;  // float: the logic tick dt

inline constexpr guest::GuestAddr kScriptRunThreads = 0x000BA060;  // thiscall(script, float dt) ret 4
inline constexpr std::uint32_t kScriptThreadList = 0x10;           // first thread
inline constexpr std::uint32_t kScriptImage = 0x14;                // the loaded .ful file ("L2iM")
inline constexpr std::uint32_t kThreadTimer = 0x1C;  // float: the script's timer (opcodes 0x02/0x03)
inline constexpr std::uint32_t kThreadWait = 0x20;   // float: Wait() seconds left
inline constexpr std::uint32_t kThreadError = 0x24;  // u8
inline constexpr std::uint32_t kThreadDone = 0x25;   // u8
inline constexpr std::uint32_t kThreadCode = 0x34;   // code pointer (after the wait opcode)
inline constexpr std::uint32_t kThreadNext = 0x3C;
inline constexpr std::uint32_t kSessionLevelName = 0x0C;  // session [0x2DABC4]: char* level name ("level4-1")
inline constexpr std::uint32_t kScriptImageMagic = 0x4D69324C;  // "L2iM"
inline constexpr std::uint32_t kScriptOpWait = 0x18;            // opcode = top byte of the word

// ---- the script countdowns ---------------------------------------------------------------------------
struct ScriptCountdownSite {
    const char* level;       // level folder as in the session ("Level2-3"; compared case-insensitively)
    const char* shown;       // level number in the game ("2-4")
    const char* function;    // script function (.dbg)
    std::uint32_t code;      // .ful offset of the waiting thread's code pointer (after the wait opcode)
    std::uint32_t words[3];  // the words in front of it: push (seconds), operand, wait
    const char* what;
};
// clang-format off
inline constexpr ScriptCountdownSite kScriptCountdowns[] = {
    {"Level2-3", "2-4", "@@action@@aEnablePowerConsole", 0x0000FFF8u, {0x08010000u, 0x42700000u, 0x18010000u},
     "power console gate (Wait 60 s, then the gate closes)"},
    {"Level3-4", "3-5", "RunNibbler", 0x0000F2E0u, {0x08016C00u, 0x3F800000u, 0x18016C00u},
     "room 9 countdown (55 x Wait 1 s, then a door locks)"},
    {"Level5-1", "5-1", "Start", 0x00012FB8u, {0x05010601u, 0x00000028u, 0x18010600u},
     "puzzle time lamps (10 x Wait m_Timeout, then the trap door opens)"},
    {"Level5-2", "5-2", "Start", 0x0000E5B4u, {0x05010601u, 0x00000028u, 0x18010600u},
     "puzzle time lamps (10 x Wait m_Timeout, then the trap door opens)"},
    {"Level5-3", "5-3", "Start", 0x00015F90u, {0x05010601u, 0x00000028u, 0x18010600u},
     "puzzle time lamps (10 x Wait m_Timeout, then the trap door opens)"},
};
// clang-format on

// ---- options -----------------------------------------------------------------------------------------
// Hook condition (FUT_PROBE_IF): freeze_timer or ingame_toggles.
bool TimerHooksWanted(const core::Config& config) noexcept;

// ---- pure decisions (unit-tested) --------------------------------------------------------------------
// The HUD timer counts down in its update (running and not paused).
bool HudTimerCounting(bool running, bool paused) noexcept;
// The countdown site of `level` whose code offset is `codeOffset` and whose three words in front of it match
// `words` (the loaded script); nullptr if none.
const ScriptCountdownSite* FindCountdownSite(std::string_view level, std::uint32_t codeOffset,
                                             const std::uint32_t (&words)[3]) noexcept;
// The wait value to write before the scheduler subtracts dt so that the thread keeps `wait` (dt <= 0 or not
// finite: unchanged).
float HeldWaitBeforeStep(float wait, float dt) noexcept;
// "m:ss" like the game's display ("%d:%02d" of the truncated seconds); negative or not finite -> 0:00.
void FormatHudTime(float seconds, char (&out)[16]) noexcept;

// ---- run time ----------------------------------------------------------------------------------------
// Exit summary line "cheats summary (timer): ..." (called by xapi::cheats::LogSummary).
void LogTimerSummary() noexcept;

}  // namespace xapi::cheats
