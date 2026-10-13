// Scripted input ([debug] input_script; see input_script.h for the format). Parser and player are plain C++ (unit
// tests: tests\input_script_tests.cpp); the host part at the end reads the game state behind the conditions,
// installs four observer probes and registers the script as an inp::InputSource.
#include "input/input_script.h"

#include "core/config.h"
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "platform/window.h"
#include "xapi/game_probes.h"
#include "xapi/camera_response.h"
#include "xapi/save_probes.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>

namespace inp::script {
namespace {

// ---- lexer ------------------------------------------------------------------------------------------------

struct Token {
    std::string text;
    bool quoted = false;
};

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view Trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// Splits a line into tokens; `#` outside quotes starts a comment; "..." is one token (no escapes).
bool Tokenize(std::string_view line, std::vector<Token>& out, std::string_view& code, std::string* error) {
    out.clear();
    std::size_t i = 0;
    std::size_t end = line.size();
    bool inQuote = false;
    for (std::size_t k = 0; k < line.size(); ++k) {
        if (line[k] == '"') {
            inQuote = !inQuote;
        } else if (line[k] == '#' && !inQuote) {
            end = k;
            break;
        }
    }
    if (inQuote) {
        *error = "missing closing quote";
        return false;
    }
    code = Trim(line.substr(0, end));
    while (i < end) {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        Token t;
        if (c == '"') {
            const std::size_t close = line.find('"', i + 1);
            t.text = std::string(line.substr(i + 1, close - i - 1));
            t.quoted = true;
            i = close + 1;
        } else {
            const std::size_t start = i;
            while (i < end && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '"') {
                ++i;
            }
            t.text = std::string(line.substr(start, i - start));
        }
        out.push_back(std::move(t));
    }
    return true;
}

bool ParseDouble(std::string_view s, double& out) noexcept {
    if (s.empty()) {
        return false;
    }
    const char* first = s.data();
    if (*first == '+') {
        ++first;
    }
    const auto r = std::from_chars(first, s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size() && std::isfinite(out);
}

bool ParseUnsigned(std::string_view s, std::uint64_t& out) noexcept {
    if (s.empty()) {
        return false;
    }
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

// "<n>t" ticks, "<n>f" frames, "<x>s" game seconds (-> ticks), "<x>ms".
bool LooksLikeTime(std::string_view s) noexcept {
    if (s.empty() || !std::isdigit(static_cast<unsigned char>(s.front()))) {
        return false;
    }
    const char last = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
    return last == 't' || last == 'f' || last == 's';
}

bool ParseTime(std::string_view s, std::uint32_t tickRate, Time& out, std::string* error) {
    const std::string low = Lower(s);
    std::string_view v(low);
    std::uint64_t n = 0;
    if (v.size() > 2 && v.substr(v.size() - 2) == "ms") {
        double ms = 0;
        if (!ParseDouble(v.substr(0, v.size() - 2), ms) || ms < 0) {
            *error = "invalid time \"" + std::string(s) + "\"";
            return false;
        }
        out = {static_cast<std::uint64_t>(std::ceil(ms * tickRate / 1000.0 - 1e-9)), Unit::Ticks};
        return true;
    }
    if (v.size() < 2) {
        *error = "invalid time \"" + std::string(s) + "\" (use <n>t, <n>f, <x>s or <x>ms)";
        return false;
    }
    const char unit = v.back();
    const std::string_view num = v.substr(0, v.size() - 1);
    if (unit == 't' || unit == 'f') {
        if (!ParseUnsigned(num, n)) {
            *error = "invalid time \"" + std::string(s) + "\" (ticks and frames are whole numbers)";
            return false;
        }
        out = {n, unit == 't' ? Unit::Ticks : Unit::Frames};
        return true;
    }
    if (unit == 's') {
        double sec = 0;
        if (!ParseDouble(num, sec) || sec < 0 || sec > 1e7) {
            *error = "invalid time \"" + std::string(s) + "\"";
            return false;
        }
        out = {static_cast<std::uint64_t>(std::ceil(sec * tickRate - 1e-9)), Unit::Ticks};
        return true;
    }
    *error = "invalid time \"" + std::string(s) + "\" (use <n>t, <n>f, <x>s or <x>ms)";
    return false;
}

struct ControlName {
    const char* name;
    ControlKind kind;
    std::uint16_t button;
    std::uint8_t index;
};

constexpr ControlName kControls[] = {
    {"a", ControlKind::Analog, 0, kAnalogA},
    {"b", ControlKind::Analog, 0, kAnalogB},
    {"x", ControlKind::Analog, 0, kAnalogX},
    {"y", ControlKind::Analog, 0, kAnalogY},
    {"black", ControlKind::Analog, 0, kAnalogBlack},
    {"white", ControlKind::Analog, 0, kAnalogWhite},
    {"lt", ControlKind::Analog, 0, kAnalogLeftTrigger},
    {"rt", ControlKind::Analog, 0, kAnalogRightTrigger},
    {"start", ControlKind::Button, kButtonStart, 0},
    {"back", ControlKind::Button, kButtonBack, 0},
    {"up", ControlKind::Button, kButtonDpadUp, 0},
    {"down", ControlKind::Button, kButtonDpadDown, 0},
    {"left", ControlKind::Button, kButtonDpadLeft, 0},
    {"right", ControlKind::Button, kButtonDpadRight, 0},
    {"dpad_up", ControlKind::Button, kButtonDpadUp, 0},
    {"dpad_down", ControlKind::Button, kButtonDpadDown, 0},
    {"dpad_left", ControlKind::Button, kButtonDpadLeft, 0},
    {"dpad_right", ControlKind::Button, kButtonDpadRight, 0},
    {"ls", ControlKind::Button, kButtonLeftThumb, 0},
    {"rs", ControlKind::Button, kButtonRightThumb, 0},
    {"lsclick", ControlKind::Button, kButtonLeftThumb, 0},
    {"rsclick", ControlKind::Button, kButtonRightThumb, 0},
    {"ls_click", ControlKind::Button, kButtonLeftThumb, 0},
    {"rs_click", ControlKind::Button, kButtonRightThumb, 0},
    {"lx", ControlKind::Axis, 0, kAxisLX},
    {"ly", ControlKind::Axis, 0, kAxisLY},
    {"rx", ControlKind::Axis, 0, kAxisRX},
    {"ry", ControlKind::Axis, 0, kAxisRY},
};

struct CondName {
    const char* name;
    CondKind kind;
    bool negate;
    bool argument;
};

constexpr CondName kConditions[] = {
    {"true", CondKind::True, false, false},
    {"false", CondKind::True, true, false},
    {"context", CondKind::Context, false, true},
    {"log", CondKind::Log, false, true},
    {"main_menu", CondKind::MainMenu, false, false},
    {"dialog", CondKind::Dialog, false, false},
    {"no_dialog", CondKind::Dialog, true, false},
    {"level_loaded", CondKind::LevelLoaded, false, false},
    {"cutscene", CondKind::Cutscene, false, false},
    {"no_cutscene", CondKind::Cutscene, true, false},
    {"paused", CondKind::Paused, false, false},
    {"controllable", CondKind::Controllable, false, false},
    {"active_pad", CondKind::ActivePad, false, false},
    {"rumble", CondKind::Rumble, false, false},
};

const char* CondKindName(CondKind kind) noexcept {
    switch (kind) {
    case CondKind::True:
        return "true";
    case CondKind::Context:
        return "context";
    case CondKind::Log:
        return "log";
    case CondKind::MainMenu:
        return "main_menu";
    case CondKind::Dialog:
        return "dialog";
    case CondKind::LevelLoaded:
        return "level_loaded";
    case CondKind::Cutscene:
        return "cutscene";
    case CondKind::Paused:
        return "paused";
    case CondKind::Controllable:
        return "controllable";
    case CondKind::ActivePad:
        return "active_pad";
    case CondKind::Rumble:
        return "rumble";
    }
    return "?";
}

// Parses tokens [first, last) as a condition.
bool ParseCondition(const std::vector<Token>& t, std::size_t first, std::size_t last, Program& program,
                    Condition& out, std::string* error) {
    bool negate = false;
    while (first < last && !t[first].quoted && IEquals(t[first].text, "not")) {
        negate = !negate;
        ++first;
    }
    if (first >= last) {
        *error = "condition expected";
        return false;
    }
    const std::string name = Lower(t[first].text);
    for (const CondName& c : kConditions) {
        if (name != c.name || t[first].quoted) {
            continue;
        }
        out = Condition{};
        out.kind = c.kind;
        out.negate = negate != c.negate;
        const std::size_t argc = last - first - 1;
        if (c.argument) {
            if (argc != 1) {
                *error = std::string("condition \"") + c.name + "\" takes one argument";
                return false;
            }
            out.argument = t[first + 1].text;
            if (out.argument.empty()) {
                *error = std::string("condition \"") + c.name + "\" needs a non-empty argument";
                return false;
            }
            if (c.kind == CondKind::Log) {
                const auto it = std::find(program.logPatterns.begin(), program.logPatterns.end(), out.argument);
                out.logPattern = static_cast<int>(it - program.logPatterns.begin());
                if (it == program.logPatterns.end()) {
                    program.logPatterns.push_back(out.argument);
                }
            }
        } else if (argc != 0) {
            *error = std::string("condition \"") + c.name + "\" takes no argument";
            return false;
        }
        return true;
    }
    *error = "unknown condition \"" + t[first].text + "\"";
    return false;
}

// Controls tokens [first, last); `allowTime` takes a trailing time.
bool ParseControls(const std::vector<Token>& t, std::size_t first, std::size_t last, std::uint32_t tickRate,
                   Instruction& ins, bool allowTime, std::string* error, bool release = false) {
    if (allowTime && last > first && !t[last - 1].quoted && LooksLikeTime(t[last - 1].text)) {
        if (!ParseTime(t[last - 1].text, tickRate, ins.time, error)) {
            return false;
        }
        if (ins.time.unit != Unit::Ticks) {
            *error = "a hold time is given in ticks or seconds, not frames";
            return false;
        }
        if (ins.time.value == 0) {
            *error = "a hold time must be at least one tick";
            return false;
        }
        ins.hasTime = true;
        --last;
    }
    for (std::size_t i = first; i < last; ++i) {
        ControlValue v;
        // `release LY`: an axis needs no value there.
        const bool bareAxis = release && t[i].text.find('=') == std::string::npos;
        if (t[i].quoted || !ParseControl(bareAxis ? t[i].text + "=0" : t[i].text, v, error)) {
            if (t[i].quoted) {
                *error = "control expected, found a quoted text";
            }
            return false;
        }
        ins.controls.push_back(std::move(v));
    }
    return true;
}

// Extracts "timeout <time>" from tokens [first, last) (anywhere); `last` shrinks accordingly.
bool TakeTimeout(std::vector<Token>& t, std::size_t first, std::size_t& last, std::uint32_t tickRate,
                 Instruction& ins, std::string* error) {
    for (std::size_t i = first; i < last; ++i) {
        if (!t[i].quoted && IEquals(t[i].text, "timeout")) {
            if (i + 1 >= last) {
                *error = "timeout needs a time";
                return false;
            }
            if (!ParseTime(t[i + 1].text, tickRate, ins.time, error)) {
                return false;
            }
            ins.hasTime = true;
            t.erase(t.begin() + static_cast<std::ptrdiff_t>(i), t.begin() + static_cast<std::ptrdiff_t>(i + 2));
            last -= 2;
            return true;
        }
    }
    return true;
}

std::string TextArgument(const std::vector<Token>& t, std::string_view code, std::size_t keywordLength) {
    if (t.size() == 2 && t[1].quoted) {
        return t[1].text;
    }
    return std::string(Trim(code.substr(std::min(code.size(), keywordLength))));
}

}  // namespace

bool ParseControl(std::string_view token, ControlValue& out, std::string* error) {
    std::string dummy;
    std::string* err = error != nullptr ? error : &dummy;
    const std::size_t eq = token.find('=');
    const std::string name = Lower(token.substr(0, eq));
    const ControlName* found = nullptr;
    for (const ControlName& c : kControls) {
        if (name == c.name) {
            found = &c;
            break;
        }
    }
    if (found == nullptr) {
        *err = "unknown control \"" + std::string(token.substr(0, eq)) + "\"";
        return false;
    }
    out = ControlValue{};
    out.kind = found->kind;
    out.button = found->button;
    out.index = found->index;
    out.name = std::string(token);
    double v = 1.0;
    if (eq != std::string_view::npos) {
        if (!ParseDouble(token.substr(eq + 1), v)) {
            *err = "invalid value in \"" + std::string(token) + "\"";
            return false;
        }
    } else if (found->kind == ControlKind::Axis) {
        *err = "axis \"" + std::string(token) + "\" needs a value (" + name + "=-1..1)";
        return false;
    }
    switch (found->kind) {
    case ControlKind::Button:
        if (v != 0.0 && v != 1.0) {
            *err = "button \"" + name + "\" is digital (=0 or =1)";
            return false;
        }
        out.value = v != 0.0 ? 1 : 0;
        break;
    case ControlKind::Analog:
        if (v < 0.0 || v > 1.0) {
            *err = "analog button \"" + name + "\" takes 0..1";
            return false;
        }
        out.value = static_cast<std::int32_t>(std::lround(v * 255.0));
        break;
    case ControlKind::Axis:
        if (v < -1.0 || v > 1.0) {
            *err = "axis \"" + name + "\" takes -1..1";
            return false;
        }
        out.value = static_cast<std::int32_t>(std::lround(v * 32767.0));
        break;
    }
    return true;
}

void ApplyControl(PadState& pad, const ControlValue& control, bool pressed) noexcept {
    switch (control.kind) {
    case ControlKind::Button:
        if (pressed && control.value != 0) {
            pad.buttons = static_cast<std::uint16_t>(pad.buttons | control.button);
        } else {
            pad.buttons = static_cast<std::uint16_t>(pad.buttons & ~control.button);
        }
        break;
    case ControlKind::Analog:
        pad.analog[control.index] = pressed ? static_cast<std::uint8_t>(std::clamp(control.value, 0, 255)) : 0;
        break;
    case ControlKind::Axis:
        pad.thumb[control.index] =
            pressed ? static_cast<std::int16_t>(std::clamp(control.value, -32768, 32767)) : std::int16_t{0};
        break;
    }
}

bool Parse(std::string_view text, std::uint32_t tickRate, Program& out, std::string* error) {
    std::string dummy;
    std::string* err = error != nullptr ? error : &dummy;
    out = Program{};
    if (tickRate == 0) {
        tickRate = 60;
    }
    std::vector<std::size_t> blocks;  // open loop/repeat instructions
    int lineNo = 0;
    std::size_t pos = 0;
    // A UTF-8 byte order mark at the start is ignored.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        pos = 3;
    }
    auto fail = [&](const std::string& message) {
        *err = "line " + std::to_string(lineNo) + ": " + message;
        return false;
    };
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            nl = text.size();
        }
        const std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        std::vector<Token> t;
        std::string_view code;
        std::string message;
        if (!Tokenize(line, t, code, &message)) {
            return fail(message);
        }
        if (t.empty()) {
            continue;
        }
        if (t[0].quoted) {
            return fail("command expected");
        }
        Instruction ins;
        ins.line = lineNo;
        ins.source = std::string(code);
        const std::string cmd = Lower(t[0].text);
        std::size_t last = t.size();
        if (cmd == "press") {
            ins.op = Op::Press;
            if (!ParseControls(t, 1, last, tickRate, ins, true, &message)) {
                return fail(message);
            }
            if (!ins.hasTime) {
                ins.time = {kDefaultPressTicks, Unit::Ticks};
                ins.hasTime = true;
            }
            if (ins.controls.empty()) {
                return fail("press needs at least one control");
            }
        } else if (cmd == "hold" || cmd == "set") {
            ins.op = Op::Hold;
            if (!ParseControls(t, 1, last, tickRate, ins, cmd == "hold", &message)) {
                return fail(message);
            }
            if (ins.controls.empty()) {
                return fail(cmd + " needs at least one control");
            }
        } else if (cmd == "release") {
            ins.op = Op::Release;
            if (!(last == 2 && !t[1].quoted && IEquals(t[1].text, "all"))) {
                if (!ParseControls(t, 1, last, tickRate, ins, false, &message, true)) {
                    return fail(message);
                }
            }
        } else if (cmd == "wait") {
            if (last == 2 && !t[1].quoted && LooksLikeTime(t[1].text)) {
                ins.op = Op::Wait;
                if (!ParseTime(t[1].text, tickRate, ins.time, &message)) {
                    return fail(message);
                }
                ins.hasTime = true;
            } else {
                ins.op = Op::WaitCond;
                if (!TakeTimeout(t, 1, last, tickRate, ins, &message) ||
                    !ParseCondition(t, 1, last, out, ins.condition, &message)) {
                    return fail(message);
                }
                if (!ins.hasTime) {
                    ins.time = {static_cast<std::uint64_t>(kDefaultWaitTimeoutSeconds) * tickRate, Unit::Ticks};
                    ins.hasTime = true;
                }
            }
        } else if (cmd == "at") {
            ins.op = Op::At;
            if (last != 2 || !ParseTime(t[1].text, tickRate, ins.time, &message)) {
                return fail(message.empty() ? "at needs one time" : message);
            }
            ins.hasTime = true;
        } else if (cmd == "expect") {
            ins.op = Op::Expect;
            if (!ParseCondition(t, 1, last, out, ins.condition, &message)) {
                return fail(message);
            }
        } else if (cmd == "loop") {
            ins.op = Op::Loop;
            if (!TakeTimeout(t, 1, last, tickRate, ins, &message)) {
                return fail(message);
            }
            if (last < 3 || t[1].quoted || !IEquals(t[1].text, "until")) {
                return fail("loop syntax: loop [timeout <time>] until <condition>");
            }
            if (!ParseCondition(t, 2, last, out, ins.condition, &message)) {
                return fail(message);
            }
            blocks.push_back(out.code.size());
        } else if (cmd == "repeat") {
            ins.op = Op::Repeat;
            if (last != 2 || !ParseUnsigned(t[1].text, ins.count)) {
                return fail("repeat needs a count");
            }
            blocks.push_back(out.code.size());
        } else if (cmd == "end") {
            ins.op = Op::End;
            if (last != 1) {
                return fail("end takes no argument");
            }
            if (blocks.empty()) {
                return fail("end without loop or repeat");
            }
            ins.jump = static_cast<int>(blocks.back());
            out.code[blocks.back()].jump = static_cast<int>(out.code.size());
            blocks.pop_back();
        } else if (cmd == "log") {
            ins.op = Op::Log;
            ins.text = TextArgument(t, code, 3);
            if (ins.text.empty()) {
                return fail("log needs a text");
            }
        } else if (cmd == "fail") {
            ins.op = Op::Fail;
            ins.text = TextArgument(t, code, 4);
            if (ins.text.empty()) {
                ins.text = "fail";
            }
        } else if (cmd == "screenshot") {
            ins.op = Op::Screenshot;
            if (last != 2 || t[1].text.empty()) {
                return fail("screenshot needs one name");
            }
            for (const char c : t[1].text) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') {
                    return fail("screenshot names use letters, digits, '_', '-' and '.'");
                }
            }
            ins.text = t[1].text;
        } else if (cmd == "unplug" || cmd == "replug") {
            ins.op = cmd == "unplug" ? Op::Unplug : Op::Replug;
            if (last != 1) {
                return fail(cmd + " takes no argument (it acts on the script's port)");
            }
        } else if (cmd == "game") {
            // (M7) debug actions on the game: `level_end <NextLevel>` ends the level like the level scripts
            // (native FsChangeLevel -> Breakdown screen with "Speichern").
            // (G5) `camera_trace on|off` logs the follow camera's yaw once per tick.
            // `state` logs the player's state text (weapons, ammo, lives) at the next tick.
            ins.op = Op::Game;
            const std::string action = last >= 2 ? Lower(t[1].text) : std::string();
            if (action == "camera_trace" && last == 3 && (Lower(t[2].text) == "on" || Lower(t[2].text) == "off")) {
                ins.text = "camera_trace " + Lower(t[2].text);
            } else if (action == "level_end" && last == 3 && !t[2].text.empty() && t[2].text.size() <= 8) {
                ins.text = "level_end " + t[2].text;
            } else if (action == "state" && last == 2) {
                ins.text = "state";
            } else {
                return fail("game takes one action: level_end <NextLevel> (a level name of 1..8 characters), "
                            "camera_trace on|off or state");
            }
        } else if (cmd == "quit") {
            ins.op = Op::Quit;
            std::uint64_t code64 = 0;
            if (last > 2 || (last == 2 && (!ParseUnsigned(t[1].text, code64) || code64 > 255))) {
                return fail("quit takes an exit code 0..255");
            }
            ins.code = static_cast<int>(code64);
        } else {
            return fail("unknown command \"" + t[0].text + "\"");
        }
        out.code.push_back(std::move(ins));
    }
    if (!blocks.empty()) {
        lineNo = out.code[blocks.back()].line;
        return fail("loop/repeat without end");
    }
    return true;
}

// ---- player -----------------------------------------------------------------------------------------------

Player::Player(Program program, Environment& env)
    : program_(std::move(program)), env_(env), consumed_(program_.logPatterns.size(), 0) {}

int Player::Line() const noexcept {
    return pc_ < program_.code.size() ? program_.code[pc_].line : 0;
}

std::string Player::Now() noexcept {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "tick %llu, frame %llu", static_cast<unsigned long long>(tick_),
                  static_cast<unsigned long long>(env_.Frames()));
    return buf;
}

void Player::Info(const std::string& text) noexcept {
    try {
        env_.Message(false, "input script: line " + std::to_string(Line()) + ": " + text + " (" + Now() + ")");
    } catch (...) {
    }
}

void Player::Fail(const std::string& reason) noexcept {
    try {
        failure_ = "line " + std::to_string(Line()) + ": " + reason;
        env_.Message(true, "input script: FAILED at " + failure_ + " (" + Now() + ")");
        env_.Screenshot("fail");
    } catch (...) {
    }
    state_ = PlayerState::Failing;
    pending_ = Pending{};
    pad_ = PadState{};
    failQuitFrame_ = env_.Frames() + 2;
    failTick_ = tick_;
}

Time Player::Absolute(const Time& relative) noexcept {
    if (relative.unit == Unit::Frames) {
        return {env_.Frames() + relative.value, Unit::Frames};
    }
    return {tick_ + relative.value, Unit::Ticks};
}

bool Player::Reached(const Time& absolute) noexcept {
    return absolute.unit == Unit::Frames ? env_.Frames() >= absolute.value : tick_ >= absolute.value;
}

bool Player::Evaluate(const Condition& c, bool consume) noexcept {
    bool value = false;
    switch (c.kind) {
    case CondKind::True:
        value = true;
        break;
    case CondKind::Log: {
        if (c.logPattern < 0 || static_cast<std::size_t>(c.logPattern) >= consumed_.size()) {
            return c.negate;
        }
        const std::uint64_t matches = env_.LogMatches(c.logPattern);
        value = matches > consumed_[static_cast<std::size_t>(c.logPattern)];
        if (value && consume && !c.negate) {
            consumed_[static_cast<std::size_t>(c.logPattern)] = matches;
        }
        break;
    }
    default:
        value = env_.Test(c);
        break;
    }
    return value != c.negate;
}

std::string Player::Describe(const Condition& c) const {
    std::string s = c.negate ? "not " : "";
    s += CondKindName(c.kind);
    if (!c.argument.empty()) {
        s += c.kind == CondKind::Log ? " \"" + c.argument + "\"" : " " + c.argument;
    }
    return s;
}

bool Player::Ready() noexcept {
    const Instruction& ins = program_.code[pc_];
    if (pending_.kind == WaitKind::Condition) {
        if (Evaluate(ins.condition, true)) {
            Info("condition \"" + Describe(ins.condition) + "\" reached");
            pending_ = Pending{};
            ++pc_;
            return true;
        }
        if (pending_.hasDeadline && Reached(pending_.deadline)) {
            Fail("timeout waiting for \"" + Describe(ins.condition) + "\"");
        }
        return false;
    }
    if (!Reached(pending_.until)) {
        return false;
    }
    if (ins.op == Op::Press && pending_.phase == 0) {
        for (const ControlValue& c : ins.controls) {
            ApplyControl(pad_, c, false);
        }
        pending_.until = {tick_ + 1, Unit::Ticks};  // one released tick before the next instruction
        pending_.phase = 1;
        return false;
    }
    if (ins.op == Op::Hold) {
        for (const ControlValue& c : ins.controls) {
            ApplyControl(pad_, c, false);
        }
    }
    pending_ = Pending{};
    ++pc_;
    return true;
}

bool Player::Execute() noexcept {
    const Instruction& ins = program_.code[pc_];
    switch (ins.op) {
    case Op::Press:
        for (const ControlValue& c : ins.controls) {
            ApplyControl(pad_, c, true);
        }
        ++presses_;
        Info(ins.source);
        pending_.kind = WaitKind::Until;
        pending_.until = Absolute(ins.time);
        return true;
    case Op::Hold:
        for (const ControlValue& c : ins.controls) {
            ApplyControl(pad_, c, true);
        }
        Info(ins.source);
        if (ins.hasTime) {
            pending_.kind = WaitKind::Until;
            pending_.until = Absolute(ins.time);
        } else {
            ++pc_;
        }
        return true;
    case Op::Release:
        if (ins.controls.empty()) {
            pad_ = PadState{};
        } else {
            for (const ControlValue& c : ins.controls) {
                ApplyControl(pad_, c, false);
            }
        }
        Info(ins.source);
        ++pc_;
        return true;
    case Op::Wait:
        pending_.kind = WaitKind::Until;
        pending_.until = Absolute(ins.time);
        return true;
    case Op::At:
        pending_.kind = WaitKind::Until;
        pending_.until = ins.time.unit == Unit::Frames ? Time{startFrame_ + ins.time.value, Unit::Frames}
                                                       : Time{ins.time.value, Unit::Ticks};
        return true;
    case Op::WaitCond:
        pending_.kind = WaitKind::Condition;
        pending_.hasDeadline = ins.hasTime;
        pending_.deadline = Absolute(ins.time);
        Info(ins.source);
        return true;
    case Op::Expect:
        if (!Evaluate(ins.condition, true)) {
            Fail("expected \"" + Describe(ins.condition) + "\"");
            return false;
        }
        Info(ins.source + " - ok");
        ++pc_;
        return true;
    case Op::Loop: {
        auto it = loopDeadlines_.find(pc_);
        if (it == loopDeadlines_.end()) {
            const Time deadline = ins.hasTime ? Absolute(ins.time) : Time{std::numeric_limits<std::uint64_t>::max()};
            it = loopDeadlines_.emplace(pc_, deadline).first;
            Info(ins.source);
        }
        if (Evaluate(ins.condition, true)) {
            Info("loop condition \"" + Describe(ins.condition) + "\" reached");
            loopDeadlines_.erase(it);
            pc_ = static_cast<std::size_t>(ins.jump) + 1;
            return true;
        }
        if (ins.hasTime && Reached(it->second)) {
            loopDeadlines_.erase(it);
            Fail("loop timeout waiting for \"" + Describe(ins.condition) + "\"");
            return false;
        }
        ++pc_;
        return true;
    }
    case Op::Repeat: {
        auto it = repeatsLeft_.find(pc_);
        if (it == repeatsLeft_.end()) {
            it = repeatsLeft_.emplace(pc_, ins.count).first;
        }
        if (it->second == 0) {
            repeatsLeft_.erase(it);
            pc_ = static_cast<std::size_t>(ins.jump) + 1;
            return true;
        }
        --it->second;
        ++pc_;
        return true;
    }
    case Op::End:
        pc_ = static_cast<std::size_t>(ins.jump);
        return true;
    case Op::Log:
        Info("log: " + ins.text);
        ++pc_;
        return true;
    case Op::Screenshot:
        Info("screenshot \"" + ins.text + "\"");
        env_.Screenshot(ins.text);
        pending_.kind = WaitKind::Until;
        pending_.until = {env_.Frames() + 2, Unit::Frames};  // the hotkey is consumed by the next DisplayFrame
        return true;
    case Op::Quit: {
        Info("quit " + std::to_string(ins.code));
        state_ = PlayerState::Quit;
        pad_ = PadState{};
        const int code = ins.code;
        env_.Quit(code, "input script quit");
        return false;
    }
    case Op::Fail:
        Fail(ins.text);
        return false;
    case Op::Unplug:
    case Op::Replug:
        Info(ins.source);
        if (!env_.Unplug(ins.op == Op::Unplug)) {
            Fail(ins.source + ": the input router does not support a simulated unplug");
            return false;
        }
        ++pc_;
        return true;
    case Op::Game:
        Info(ins.source);
        if (!env_.GameAction(ins.text)) {
            Fail(ins.source + ": the game cannot run this action now");
            return false;
        }
        ++pc_;
        return true;
    }
    return false;
}

PadState Player::Step() noexcept {
    tick_ = ticks_++;
    if (!started_) {
        started_ = true;
        startFrame_ = env_.Frames();
        try {
            env_.Message(false, "input script: started (" + std::to_string(program_.code.size()) +
                                    " instructions, " + Now() + ")");
        } catch (...) {
        }
    }
    if (state_ == PlayerState::Failing) {
        // Two frames for the failure screenshot (at most kFailQuitTicks ticks if no frame is displayed).
        if (env_.Frames() >= failQuitFrame_ || tick_ >= failTick_ + kFailQuitTicks) {
            state_ = PlayerState::Quit;
            env_.Quit(kExitInputScriptFailed, "input script failed");
        }
        return pad_;
    }
    if (state_ != PlayerState::Running) {
        return pad_;
    }
    int budget = kMaxInstructionsPerTick;
    while (state_ == PlayerState::Running) {
        if (pending_.kind != WaitKind::None) {
            if (!Ready()) {
                break;
            }
            continue;
        }
        if (pc_ >= program_.code.size()) {
            state_ = PlayerState::Finished;
            pad_ = PadState{};
            try {
                env_.Message(false, "input script: finished (" + Now() + ")");
            } catch (...) {
            }
            break;
        }
        if (--budget < 0) {
            Fail("more than " + std::to_string(kMaxInstructionsPerTick) +
                 " instructions in one tick (loop or repeat without a wait?)");
            break;
        }
        if (!Execute()) {
            break;
        }
    }
    return pad_;
}

// ---- host integration -------------------------------------------------------------------------------------

namespace {

// Observer probes (installed always; they only record pointers/results and call the original).
std::atomic<std::uint32_t> g_mainMenu{0};   // FUN_00020150 `this`
std::atomic<std::uint32_t> g_pause{0};      // FUN_000A8220 `this`
std::atomic<int> g_levelReady{-1};          // last FUN_000C3E20 result at 0x2AB2F (-1 = not called yet)
std::atomic<std::uint64_t> g_heartbeats{0};  // FUN_0009C140 calls from the tick (CApp vf10 0xD25D3)

std::uint32_t ReadU32(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
}
std::uint8_t ReadU8(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 1)) ? guest::Read<std::uint8_t>(p) : 0;
}

std::string CurrentContextName() {
    const guest::GuestAddr app = ReadU32(guest::kGameGamePtr);
    const guest::GuestAddr ctx = app != 0 ? ReadU32(app + guest::kCAppCurrentContextOffset) : 0;
    const guest::GuestAddr name = ctx != 0 ? ReadU32(ctx + guest::kGameContextNameOffset) : 0;
    std::string out;
    for (std::uint32_t i = 0; name != 0 && i < 32; ++i) {
        if ((i == 0 || ((name + i) & 0xFFFu) == 0) && !guest::IsReadable(name + i, 1)) {
            break;
        }
        const char c = guest::Read<char>(name + i);
        if (c == 0) {
            break;
        }
        out += c;
    }
    return out;
}

bool InContext(std::string_view name) {
    return IEquals(CurrentContextName(), name);
}

bool DialogOpen() noexcept {
    const guest::GuestAddr app = ReadU32(guest::kGameAppPtr);
    const guest::GuestAddr ui = app != 0 ? ReadU32(app + guest::kCAppSaveUiOffset) : 0;
    return ui != 0 && ReadU8(ui + guest::kSaveUiDialogOpenOffset) != 0;
}

bool CutsceneActive() noexcept {
    const guest::GuestAddr m = ReadU32(guest::kGameCutscenePtr);
    return m != 0 && ReadU8(m + guest::kCutsceneActiveOffset) != 0;
}

bool PauseOpen() noexcept {
    const guest::GuestAddr p = g_pause.load();
    return p != 0 && ReadU8(p + guest::kPauseOpenOffset) != 0;
}

bool PlayerInputEnabled() noexcept {
    const guest::GuestAddr level = ReadU32(guest::kGameLevelPtr);
    const guest::GuestAddr player = level != 0 ? ReadU32(level + guest::kLevelPlayerOffset) : 0;
    return player != 0 && ReadU8(player + guest::kPlayerInputEnabledOffset) != 0;
}

// Log patterns of the loaded script, counted by the log observer (any thread).
struct Patterns {
    std::vector<std::string> texts;
    std::unique_ptr<std::atomic<std::uint64_t>[]> counts;
};
std::atomic<Patterns*> g_patterns{nullptr};

void ScriptLogObserver(core::Channel, core::Level, const char* line, std::size_t size) noexcept {
    Patterns* p = g_patterns.load(std::memory_order_acquire);
    if (p == nullptr) {
        return;
    }
    const std::string_view text(line, size);
    if (text.find("input script:") != std::string_view::npos) {
        return;  // the script's own lines echo its conditions
    }
    for (std::size_t i = 0; i < p->texts.size(); ++i) {
        if (text.find(p->texts[i]) != std::string_view::npos) {
            p->counts[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

class HostEnvironment final : public Environment {
public:
    int port = 0;

    bool Test(const Condition& c) noexcept override {
        try {
            switch (c.kind) {
            case CondKind::Context:
                return InContext(c.argument);
            case CondKind::MainMenu: {
                // Visible and ready for a confirm: no input delay, no camera move, no countdown running.
                const guest::GuestAddr menu = g_mainMenu.load();
                if (menu == 0 || ReadU8(menu + guest::kMainMenuVisibleOffset) == 0 || !InContext("Frontend")) {
                    return false;
                }
                const guest::GuestAddr camera = ReadU32(menu + guest::kMainMenuCameraOffset);
                return ReadU32(menu + guest::kMainMenuDelayOffset) == 0 &&
                       static_cast<std::int32_t>(ReadU32(menu + guest::kMainMenuCountdownOffset)) <= 0 &&
                       (camera == 0 || ReadU8(camera + guest::kMenuCameraMovingOffset) == 0);
            }
            case CondKind::Dialog:
                return DialogOpen();
            case CondKind::LevelLoaded:
                return g_levelReady.load() == 1 && InContext("Ingame");
            case CondKind::Cutscene:
                return CutsceneActive();
            case CondKind::Paused:
                return PauseOpen() && InContext("Ingame");
            case CondKind::Controllable:
                return g_levelReady.load() == 1 && InContext("Ingame") && PlayerInputEnabled() &&
                       !CutsceneActive() && !PauseOpen() && !DialogOpen();
            case CondKind::ActivePad: {
                const guest::GuestAddr pad = ReadU32(guest::kGameActiveControllerVar);
                return pad != 0 && ReadU32(pad + guest::kControllerPortOffset) == static_cast<std::uint32_t>(port);
            }
            case CondKind::Rumble:
                return rumble.left != 0 || rumble.right != 0;
            case CondKind::True:
            case CondKind::Log:
                break;
            }
        } catch (...) {
        }
        return false;
    }
    std::uint64_t LogMatches(int pattern) noexcept override {
        Patterns* p = g_patterns.load(std::memory_order_acquire);
        if (p == nullptr || pattern < 0 || static_cast<std::size_t>(pattern) >= p->texts.size()) {
            return 0;
        }
        return p->counts[static_cast<std::size_t>(pattern)].load(std::memory_order_relaxed);
    }
    std::uint64_t Frames() noexcept override {
        return xapi::GetGameObserverStats().frames;
    }
    void Message(bool error, const std::string& line) noexcept override {
        if (error) {
            FUT_LOGE(Inp, "%s", line.c_str());
        } else {
            FUT_LOGI(Inp, "%s", line.c_str());
        }
    }
    void Screenshot(const std::string& name) noexcept override {
        ++screenshots;
        FUT_LOGI(Inp, "input script: screenshot \"%s\" requested (frame %llu)", name.c_str(),
                 static_cast<unsigned long long>(Frames()));
        platform::PostHotkeys(platform::kHotkeyScreenshot);
    }
    // Deferred: the player runs under the source's lock and inside the router's sample (XInputGetState); the exit
    // reporters (script summary, router, XPP statistics) take those locks, so the process ends at the next tick
    // heartbeat (OnHeartbeat).
    void Quit(int code, const std::string& reason) noexcept override {
        quitCode = code;
        try {
            quitReason = reason;
        } catch (...) {
        }
        quitPending = true;
    }
    void QuitIfRequested() noexcept {
        if (!quitPending) {
            return;
        }
        FUT_LOGI(Inp, "input script: ending the process with exit code %d (%s)", quitCode, quitReason.c_str());
        core::ExitHostProcess(quitCode, quitReason.c_str());
    }
    // (M7) Deferred to the next tick heartbeat (xapi::RunPendingGameActions): the player runs inside the router's
    // sample (XInputGetState) under the source's lock.
    bool GameAction(const std::string& action) noexcept override {
        static constexpr std::string_view kLevelEnd = "level_end ";
        static constexpr std::string_view kCameraTrace = "camera_trace ";
        if (action.rfind(kCameraTrace, 0) == 0) {
            xapi::camera::SetTrace(action.substr(kCameraTrace.size()) == "on");  // (G5)
            return true;
        }
        if (action == "state") {
            return xapi::RequestStateDump();
        }
        return action.rfind(kLevelEnd, 0) == 0 && xapi::RequestLevelEnd(action.c_str() + kLevelEnd.size());
    }
    bool Unplug(bool unplugged) noexcept override {
        InputRouter* router = GetRouter();
        if (router == nullptr) {
            return false;
        }
        router->SimulateUnplug(port, unplugged);
        portUnplugged = unplugged;
        return true;
    }

    std::uint32_t screenshots = 0;
    bool portUnplugged = false;  // the script holds its port unplugged (the heartbeat steps the player)
    bool quitPending = false;
    RumbleState rumble;  // last request of the game for the script's port (SetRumble)
    int quitCode = 0;
    std::string quitReason;
};

class ScriptSourceImpl final : public InputSource {
public:
    const char* Name() const noexcept override {
        return "input script";
    }
    SourceKind Kind() const noexcept override {
        return SourceKind::Script;
    }
    int DefaultPort() const noexcept override {
        return 0;
    }
    bool Connected() const noexcept override {
        return const_cast<ScriptSourceImpl*>(this)->Ensure();
    }
    bool Sample(int port, std::uint64_t /*tick*/, PadState& out) noexcept override {
        if (!Ensure() || port != env_.port) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // One player step per logic tick: the hotplug heartbeat numbers the ticks; a tick in which the
            // heartbeat already stepped (unplugged port, or the port came back in this tick) returns that tick's
            // pad. Without heartbeats (probe disabled) every sample is a tick.
            const std::uint64_t beat = g_heartbeats.load(std::memory_order_acquire);
            if (beat == 0 || beat != lastStepBeat_) {
                StepLocked(beat);
            }
            out = lastPad_;
        }
        if (g_heartbeats.load(std::memory_order_acquire) == 0) {
            env_.QuitIfRequested();  // no heartbeat probe: end here (inside XInputGetState)
        }
        return true;
    }
    // probe:Game::HotplugCheck, once per logic tick (guest main thread). While the script holds its port unplugged
    // the game does not call XInputGetState for it, so the heartbeat runs the script (its pad goes nowhere).
    // A `quit` (or the end after a failure) requested in the previous tick ends the process here, at the start of
    // the next tick and outside the XPP entry points: the exit reporters of the input layer take the locks that the
    // XInputGetState call stack holds.
    void OnHeartbeat() noexcept {
        const std::uint64_t beat = g_heartbeats.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (!loaded_.load(std::memory_order_acquire)) {
            return;
        }
        env_.QuitIfRequested();
        if (!env_.portUnplugged) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (beat != lastStepBeat_) {
                StepLocked(beat);
            }
        }
        env_.QuitIfRequested();
    }
    void SetRumble(int port, const RumbleState& rumble) noexcept override {
        if (port != env_.port || rumble == lastRumble_) {
            return;
        }
        lastRumble_ = rumble;
        env_.rumble = rumble;
        ++rumbleChanges_;
        maxLeft_ = std::max(maxLeft_, rumble.left);
        maxRight_ = std::max(maxRight_, rumble.right);
        if (rumbleChanges_ <= 64) {
            FUT_LOGI(Inp, "input script: rumble port %d left %u right %u (frame %llu)", port + 1, rumble.left,
                     rumble.right, static_cast<unsigned long long>(env_.Frames()));
        }
    }

    void StepLocked(std::uint64_t beat) noexcept {
        lastStepBeat_ = beat;
        const PadState pad = player_->Step();
        if (pad != lastPad_) {
            ++padChanges_;
            lastPad_ = pad;
        }
    }

    ScriptStats Stats() noexcept {
        ScriptStats s;
        if (!loaded_.load()) {
            return s;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        s.loaded = true;
        s.ticks = player_->Ticks();
        s.presses = player_->Presses();
        s.line = player_->Line();
        s.state = player_->State();
        return s;
    }

    void Summary() noexcept {
        if (!loaded_.load()) {
            return;
        }
        const ScriptStats s = Stats();
        static constexpr const char* kStates[] = {"running", "finished", "failing", "quit"};
        FUT_LOGI(Inp,
                 "input script summary: %s at line %d, %llu ticks sampled, %llu presses, %llu pad changes, "
                 "%u screenshots, rumble %llu changes (max left %u, right %u)",
                 kStates[static_cast<int>(s.state)], s.line, static_cast<unsigned long long>(s.ticks),
                 static_cast<unsigned long long>(s.presses), static_cast<unsigned long long>(padChanges_),
                 env_.screenshots, static_cast<unsigned long long>(rumbleChanges_), maxLeft_, maxRight_);
    }

private:
    bool Ensure() noexcept {
        if (checked_.load(std::memory_order_acquire)) {
            return loaded_.load(std::memory_order_acquire);
        }
        std::lock_guard<std::mutex> lock(loadMutex_);
        if (checked_.load()) {
            return loaded_.load();
        }
        Load();
        checked_.store(true, std::memory_order_release);
        return loaded_.load();
    }

    void Load() noexcept {
        try {
            const core::Config& cfg = core::GetConfig();
            if (cfg.GetString("debug", "input_script").empty()) {
                return;
            }
            const std::filesystem::path path = cfg.GetPath("debug", "input_script");
            const std::uint32_t tickRate = cfg.GetU32("game", "tick_rate", 60);
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                FUT_LOGE(Inp, "input script: cannot open %s", path.string().c_str());
                core::ExitHostProcess(core::kExitStartupError, "input script not found");
            }
            std::ostringstream text;
            text << f.rdbuf();
            Program program;
            std::string error;
            if (!Parse(text.str(), tickRate, program, &error)) {
                FUT_LOGE(Inp, "input script: %s: %s", path.string().c_str(), error.c_str());
                core::ExitHostProcess(core::kExitStartupError, "input script syntax error");
            }
            auto patterns = std::make_unique<Patterns>();
            patterns->texts = program.logPatterns;
            patterns->counts = std::make_unique<std::atomic<std::uint64_t>[]>(patterns->texts.size() + 1);
            const std::size_t instructions = program.code.size();
            const std::size_t patternCount = patterns->texts.size();
            g_patterns.store(patterns.release(), std::memory_order_release);  // lives until the process ends
            if (patternCount != 0 && !core::AddLogObserver(&ScriptLogObserver)) {
                FUT_LOGE(Inp, "input script: no free log observer slot - log conditions never match");
            }
            player_ = std::make_unique<Player>(std::move(program), env_);
            core::AddExitReporter("input script", []() noexcept { Instance().Summary(); });
            FUT_LOGI(Inp, "input script: loaded %s: %zu instructions, %zu log patterns, tick rate %u Hz, port %d",
                     path.string().c_str(), instructions, patternCount, tickRate, env_.port + 1);
            loaded_.store(true, std::memory_order_release);
        } catch (...) {
            FUT_LOGE(Inp, "input script: loading failed (exception)");
        }
    }

public:
    static ScriptSourceImpl& Instance() noexcept {
        static ScriptSourceImpl source;
        return source;
    }

private:
    std::mutex loadMutex_;
    std::mutex mutex_;
    std::atomic<bool> checked_{false};
    std::atomic<bool> loaded_{false};
    HostEnvironment env_;
    std::unique_ptr<Player> player_;
    PadState lastPad_;
    std::uint64_t lastStepBeat_ = ~std::uint64_t{0};
    RumbleState lastRumble_;
    std::uint64_t padChanges_ = 0;
    std::uint64_t rumbleChanges_ = 0;
    std::uint16_t maxLeft_ = 0;
    std::uint16_t maxRight_ = 0;
};

const bool g_registered = RegisterSource(&ScriptSourceImpl::Instance());

guest::GuestAddr Trampoline(guest::GuestAddr address) noexcept {
    const guest::GuestAddr t = core::HookTrampoline(address);
    if (t == 0) {
        core::Fatal("probe at 0x%08X runs without an installed trampoline", address);
    }
    return t;
}

}  // namespace

InputSource& ScriptSource() noexcept {
    (void)g_registered;
    return ScriptSourceImpl::Instance();
}

void PreloadScript() noexcept {
    (void)ScriptSourceImpl::Instance().Connected();
}

ScriptStats GetScriptStats() noexcept {
    return ScriptSourceImpl::Instance().Stats();
}

bool GameControllable() noexcept {
    try {
        return g_levelReady.load() == 1 && InContext("Ingame") && PlayerInputEnabled() && !CutsceneActive() &&
               !PauseOpen() && !DialogOpen();
    } catch (...) {
        return false;
    }
}

// FUN_0009C140 controller hotplug check (cdecl, no args, plain ret 0x9C214; guest.h kGameHotplugCheck): called
// once per logic tick by CApp vf10 (return 0xD25D8) - the script's tick heartbeat (see OnHeartbeat); the call from
// the disc error loop (0xF3004) is not a tick.
std::uint32_t __cdecl HotplugCheckProbe() noexcept {
    if (guest::Addr(_ReturnAddress()) == guest::kGameHotplugCheckTickReturn) {
        xapi::RunPendingGameActions();  // (M7) `game level_end` of the previous tick
        xapi::camera::TraceTick();      // (G5) `game camera_trace on`
        ScriptSourceImpl::Instance().OnHeartbeat();
    }
    return core::CallGuest(Trampoline(guest::kGameHotplugCheck));
}

// FUN_00020150 main menu activation (vtable 0x2940D4 [8], thiscall, no stack args; guest.h
// kGameMainMenuActivate): records the menu object for the main_menu condition.
std::uint32_t __fastcall MainMenuActivateProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    const std::uint32_t eax = core::CallGuestThiscall(Trampoline(guest::kGameMainMenuActivate), self, {});
    if (g_mainMenu.exchange(self) != self) {
        FUT_LOGI(Inp, "game: main menu active (menu 0x%08X, entry %d)", self,
                 static_cast<int>(ReadU32(self + guest::kMainMenuSelectionOffset)));
    }
    return eax;
}

// FUN_000C3E20 level-ready test (thiscall ecx = level object, al = result; guest.h kGameLevelReadyTest): the
// result at the session update's call 0x2AB2F is the level_loaded condition.
std::uint32_t __fastcall LevelReadyProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    const std::uint32_t eax = core::CallGuestThiscall(Trampoline(guest::kGameLevelReadyTest), self, {});
    if (guest::Addr(_ReturnAddress()) == guest::kGameLevelReadyTestReturn) {
        const int ready = (eax & 0xFF) != 0 ? 1 : 0;
        if (g_levelReady.exchange(ready) != ready) {
            FUT_LOGI(Inp, "game: level %s (level object 0x%08X, frame %llu)", ready ? "loaded" : "loading", self,
                     static_cast<unsigned long long>(xapi::GetGameObserverStats().frames));
        }
    }
    return eax;
}

// FUN_000A8220 pause open (thiscall ecx = pause object; guest.h kGamePauseOpen): records the pause object for the
// paused condition (pause+0x24).
std::uint32_t __fastcall PauseOpenProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    const std::uint32_t eax = core::CallGuestThiscall(Trampoline(guest::kGamePauseOpen), self, {});
    g_pause.store(self);
    FUT_LOGI(Inp, "game: pause opened (pause object 0x%08X, called from 0x%08X)", self,
             guest::Addr(_ReturnAddress()));
    return eax;
}

}  // namespace inp::script

FUT_PROBE(guest::kGameHotplugCheck, &inp::script::HotplugCheckProbe, Cdecl, "probe:Game::HotplugCheck",
          "83 EC 08 55 8D 44 24 04", 8);
FUT_PROBE(guest::kGameMainMenuActivate, &inp::script::MainMenuActivateProbe, Thiscall, "probe:Game::MainMenuActivate",
          "56 8B F1 C6 46 04 01", 7);
FUT_PROBE(guest::kGameLevelReadyTest, &inp::script::LevelReadyProbe, Thiscall, "probe:Game::LevelReadyTest",
          "57 8B F9 8B 47 58", 6);
FUT_PROBE(guest::kGamePauseOpen, &inp::script::PauseOpenProbe, Thiscall, "probe:Game::PauseOpen",
          "53 56 8B F1 8B 4E 14", 7);
