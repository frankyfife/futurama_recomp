// Scripted input for automated runs: [debug] input_script = <file> (M4 test automation; src\INTERFACES.md 4.27,
// format reference tools\input_scripts\README.md, analysis\loader\m4_input.md 10).
//
// The script is a small text file played as an inp::InputSource ("input script", SourceKind::Script) on logical
// port 0 (player 1, the game calls it "port 1"). The router samples it once per logic tick inside the game's
// XInputGetState, so the script's time base is the tick. Within a scripted phase the lockstep frame clock ([game]
// frame_clock) makes the play deterministic tick for tick, but absolute ticks drift by a few (measured: up to 18)
// across the intro and loading, which follow I/O and QPC time; synchronise on conditions (wait context/main_menu/
// level_loaded/controllable), not on absolute times (loader_design.md 8.11 E14). Seconds are game seconds
// (ticks / [game] tick_rate), frames are CApp::Frame calls.
//
//   # comment                               (also after a command, outside quotes)
//   press A [3t]                            hold A for n ticks (default 3), then at least 1 tick released
//   press A B 2t                            several controls at once
//   hold LY=1.0 120t                        hold for a time, then release (no extra released tick)
//   hold RT                                 hold until `release RT` / `release all`
//   set RX=-0.5                             = hold without a time (value 0 releases)
//   release LY | release all
//   wait 30t | wait 2s | wait 10f           ticks / game seconds / frames
//   at 600t                                 wait until this time since the script started
//   wait <condition> [timeout 20s]          default timeout 60 s; a timeout fails the script
//   expect <condition>                      fails the script if the condition is false now
//   loop [timeout 300s] until <condition>   ... end   (condition tested before every pass)
//   repeat 5                                ... end
//   log "text" | screenshot <name> | quit [code] | fail "text"
//   unplug | replug                         simulated removal / re-insertion of the script's port
//                                           (InputRouter::SimulateUnplug; in-game the game pauses, ERRCTRLINGAME)
//   game level_end Level1-2                 (M7) end the level like the level scripts (native FsChangeLevel: the
//                                           Breakdown screen with "Speichern"; xapi::RequestLevelEnd, next tick)
//   game camera_trace on|off                (G5) one log line per tick with the follow camera's yaw
//                                           (xapi::camera::TraceTick at the tick heartbeat)
//
// Controls: A B X Y BLACK WHITE LT RT (analog 0..1, digital = 1), START BACK UP DOWN LEFT RIGHT (D-pad;
// DPAD_UP.. too) LS RS (stick clicks; LSCLICK/RSCLICK too), axes LX LY RX RY (-1..1, +Y = up; raw value
// round(v * 32767)).
// Conditions (prefix `not` negates): context <Name> | log "<text>" (a log line containing the text, not yet
// consumed by an earlier log condition of the script) | main_menu | dialog | no_dialog | level_loaded | cutscene |
// paused | controllable | active_pad (the game's active controller is the script's port) | rumble (the game's last
// XInputSetState for the port has a motor above 0) | true | false.
//
// A missing file or a syntax error ends the start with exit code 1 (kExitStartupError) and an error line with the
// line number (PreloadScript). Failure at run time (timeout, expect, fail): an error line "input script: FAILED
// ...", a screenshot "fail", then the process ends with exit code kExitInputScriptFailed after two more frames.
#pragma once

#include "input/input_api.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace inp::script {

// Exit code of a failed script (core\host_main.h lists 0..6; INTERFACES.md section 1).
inline constexpr int kExitInputScriptFailed = 7;
inline constexpr std::uint64_t kDefaultPressTicks = 3;
inline constexpr std::uint32_t kDefaultWaitTimeoutSeconds = 60;
// Instructions executed within one tick before the player gives up (a loop without a wait).
inline constexpr int kMaxInstructionsPerTick = 10000;
// Ticks after a failure before the process ends even if no frame was displayed for the screenshot.
inline constexpr std::uint64_t kFailQuitTicks = 600;

enum class Unit : std::uint8_t { Ticks, Frames };

// A duration or point in time; seconds are converted to ticks when the script is parsed.
struct Time {
    std::uint64_t value = 0;
    Unit unit = Unit::Ticks;
    bool operator==(const Time&) const = default;
};

enum class ControlKind : std::uint8_t { Button, Analog, Axis };

// One control with the value a press/hold applies (Button: on/off; Analog 0..255; Axis -32768..32767).
struct ControlValue {
    ControlKind kind = ControlKind::Button;
    std::uint16_t button = 0;  // kButton* bit (Button)
    std::uint8_t index = 0;    // AnalogButton / StickAxis
    std::int32_t value = 0;
    std::string name;          // as written (log lines)
};

enum class CondKind : std::uint8_t {
    True,
    Context,
    Log,
    MainMenu,
    Dialog,
    LevelLoaded,
    Cutscene,
    Paused,
    Controllable,
    ActivePad,
    Rumble,  // the game's last rumble request for the script's port has a motor above 0
};

struct Condition {
    CondKind kind = CondKind::True;
    bool negate = false;
    std::string argument;  // Context: name; Log: text
    int logPattern = -1;   // Log: index into Program::logPatterns
};

enum class Op : std::uint8_t {
    Press,       // controls, time (ticks), gap tick
    Hold,        // controls, optional time (hasTime)
    Release,     // controls (empty = all)
    Wait,        // time
    At,          // time (absolute since the start)
    WaitCond,    // condition, timeout
    Expect,      // condition
    Loop,        // condition, timeout (hasTime), jump = index of the matching end
    Repeat,      // count, jump = index of the matching end
    End,         // jump = index of the loop/repeat
    Log,         // text
    Screenshot,  // text = name
    Quit,        // code
    Fail,        // text
    Unplug,      // simulated removal of the script's port (InputRouter::SimulateUnplug)
    Replug,      // ... and re-insertion
    Game,        // (M7) debug action on the game, text = action ("level_end Level1-2")
};

struct Instruction {
    Op op = Op::Wait;
    int line = 0;
    std::string text;  // source line (trimmed, without comment) for Log/Screenshot/Fail: the argument
    std::vector<ControlValue> controls;
    Time time;
    bool hasTime = false;
    Condition condition;
    std::uint64_t count = 0;
    int code = 0;
    int jump = -1;
    std::string source;  // the trimmed source line (log lines)
};

struct Program {
    std::vector<Instruction> code;
    std::vector<std::string> logPatterns;  // distinct texts of the log conditions
};

// Parses a script. `tickRate` converts seconds to ticks. False + "line N: message" on the first error.
bool Parse(std::string_view text, std::uint32_t tickRate, Program& out, std::string* error);

// Parses one control assignment ("A", "LT=0.5", "LY=-1.0"). False + message on error.
bool ParseControl(std::string_view token, ControlValue& out, std::string* error);

// Writes `value` into `pad` (Button: set/clear the bit; Analog/Axis: the value).
void ApplyControl(PadState& pad, const ControlValue& control, bool pressed) noexcept;

// What the host provides to the player (the game state behind the conditions, frames, screenshots, the end).
class Environment {
public:
    virtual ~Environment() = default;
    // Current value of a condition kind other than Log/True (Context: `argument` = the name to compare).
    virtual bool Test(const Condition& condition) noexcept = 0;
    // Number of log lines that matched logPatterns[pattern] since the script was loaded.
    virtual std::uint64_t LogMatches(int pattern) noexcept = 0;
    // CApp::Frame calls so far (displayed frames).
    virtual std::uint64_t Frames() noexcept = 0;
    // A line for the log (level: true = error, else info). Every line starts with "input script: ".
    virtual void Message(bool error, const std::string& line) noexcept = 0;
    // Requests a screenshot of the next displayed frame (the host logs where it is written).
    virtual void Screenshot(const std::string& name) noexcept = 0;
    // Ends the process (the host never returns from this; test environments do).
    virtual void Quit(int code, const std::string& reason) noexcept = 0;
    // Simulated unplug (true) / replug (false) of the script's port; false if not supported (no router).
    virtual bool Unplug(bool unplugged) noexcept {
        (void)unplugged;
        return false;
    }
    // (M7) Debug action on the game (`game level_end Level1-2`); false if unknown or not possible now.
    virtual bool GameAction(const std::string& action) noexcept {
        (void)action;
        return false;
    }
};

enum class PlayerState : std::uint8_t { Running, Finished, Failing, Quit };

// Executes a Program one tick at a time. Not thread-safe (the router calls it from the guest main thread).
class Player {
public:
    Player(Program program, Environment& env);

    // One logic tick: runs the script until it waits, returns the pad of this tick.
    PadState Step() noexcept;

    PlayerState State() const noexcept {
        return state_;
    }
    std::uint64_t Ticks() const noexcept {
        return ticks_;
    }
    int Line() const noexcept;  // source line of the current instruction (0 at the end)
    const std::string& FailureReason() const noexcept {
        return failure_;
    }
    std::uint64_t Presses() const noexcept {
        return presses_;
    }

    // Evaluates a condition now (log conditions consume their matches when `consume`).
    bool Evaluate(const Condition& condition, bool consume) noexcept;
    std::string Describe(const Condition& condition) const;

private:
    enum class WaitKind : std::uint8_t { None, Until, Condition };
    struct Pending {
        WaitKind kind = WaitKind::None;
        Time until;               // Until: absolute tick/frame
        bool hasDeadline = false;
        Time deadline;            // Condition: absolute tick/frame
        int phase = 0;            // Press: 0 = held, 1 = gap; Screenshot / Hold with time: 0
    };

    bool Ready() noexcept;  // the pending wait of the current instruction is over (may fail the script)
    // Executes code_[pc_]; returns false when the instruction waits (pending_ set).
    bool Execute() noexcept;
    void Fail(const std::string& reason) noexcept;
    void Info(const std::string& text) noexcept;
    Time Absolute(const Time& relative) noexcept;
    bool Reached(const Time& absolute) noexcept;
    std::string Now() noexcept;

    Program program_;
    Environment& env_;
    PlayerState state_ = PlayerState::Running;
    std::size_t pc_ = 0;
    Pending pending_;
    PadState pad_;
    std::uint64_t ticks_ = 0;   // Step calls so far
    std::uint64_t tick_ = 0;    // index of the tick being executed
    std::uint64_t startFrame_ = 0;
    bool started_ = false;
    std::uint64_t failQuitFrame_ = 0;
    std::uint64_t failTick_ = 0;
    std::uint64_t presses_ = 0;
    std::string failure_;
    std::vector<std::uint64_t> consumed_;              // per log pattern
    std::map<std::size_t, Time> loopDeadlines_;        // active loops (pc of the loop instruction)
    std::map<std::size_t, std::uint64_t> repeatsLeft_;  // active repeats
};

// ---- host integration (input_script.cpp) ----------------------------------------------------------------

// The InputSource of [debug] input_script (registered at static initialisation; inactive without the key).
// Loads the script on first use; a parse error ends the process with exit code 1 (configuration error).
InputSource& ScriptSource() noexcept;
// Loads and parses [debug] input_script now (StartInput, before any XPP or router lock is taken). A missing file or
// a syntax error ends the process here with kExitStartupError (1) and an error line; without this call the first
// Connected() of the source loads the script lazily.
void PreloadScript() noexcept;

struct ScriptStats {
    bool loaded = false;
    std::uint64_t ticks = 0;
    std::uint64_t presses = 0;
    int line = 0;
    PlayerState state = PlayerState::Running;
};
ScriptStats GetScriptStats() noexcept;

// G7: the condition `controllable` without a script (Ingame, level loaded, player input enabled, no cut scene, no
// pause, no save dialog); uses the always-installed observer probes of this file.
bool GameControllable() noexcept;

}  // namespace inp::script
