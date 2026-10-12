// Binding lists of the [input] section: parser, formatter, defaults and evaluation (see bindings.h).
#include "input/bindings.h"

#include "input/stick_curve.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace inp {
namespace {

constexpr std::uint8_t Dir(int axis, bool negative) {
    return static_cast<std::uint8_t>(axis * 2 + (negative ? 1 : 0));
}

// The 24 controls (src\INTERFACES.md 4.26), defaults of m4_input.md 9.1 (pad = identity) and 9.2 (keyboard/mouse).
constexpr ControlInfo kControls[kControlCount] = {
    {Control::A, "a", "pad_a", "kbm_a", ControlKind::AnalogButton, kAnalogA, "pad:a", "key:enter, mouse:left"},
    {Control::B, "b", "pad_b", "kbm_b", ControlKind::AnalogButton, kAnalogB, "pad:b", "key:space"},
    {Control::X, "x", "pad_x", "kbm_x", ControlKind::AnalogButton, kAnalogX, "pad:x", "key:f, mouse:right"},
    {Control::Y, "y", "pad_y", "kbm_y", ControlKind::AnalogButton, kAnalogY, "pad:y", "key:e"},
    {Control::White, "white", "pad_white", "kbm_white", ControlKind::AnalogButton, kAnalogWhite, "pad:lb",
     "key:1, mouse:wheel_down"},
    {Control::Black, "black", "pad_black", "kbm_black", ControlKind::AnalogButton, kAnalogBlack, "pad:rb",
     "key:2, mouse:wheel_up"},
    {Control::LeftTrigger, "lt", "pad_lt", "kbm_lt", ControlKind::AnalogButton, kAnalogLeftTrigger, "pad:lt", "key:q"},
    {Control::RightTrigger, "rt", "pad_rt", "kbm_rt", ControlKind::AnalogButton, kAnalogRightTrigger, "pad:rt",
     "key:lshift"},
    {Control::Start, "start", "pad_start", "kbm_start", ControlKind::DigitalButton, 0x10, "pad:start", "key:escape"},
    {Control::Back, "back", "pad_back", "kbm_back", ControlKind::DigitalButton, 0x20, "pad:back", "key:backspace"},
    {Control::DpadUp, "dpad_up", "pad_dpad_up", "kbm_dpad_up", ControlKind::DigitalButton, 0x01, "pad:dpad_up",
     "key:up"},
    {Control::DpadDown, "dpad_down", "pad_dpad_down", "kbm_dpad_down", ControlKind::DigitalButton, 0x02,
     "pad:dpad_down", "key:down"},
    {Control::DpadLeft, "dpad_left", "pad_dpad_left", "kbm_dpad_left", ControlKind::DigitalButton, 0x04,
     "pad:dpad_left", "key:left"},
    {Control::DpadRight, "dpad_right", "pad_dpad_right", "kbm_dpad_right", ControlKind::DigitalButton, 0x08,
     "pad:dpad_right", "key:right"},
    {Control::LeftThumb, "ls_click", "pad_ls_click", "kbm_ls_click", ControlKind::DigitalButton, 0x40, "pad:ls_click",
     "key:c"},
    {Control::RightThumb, "rs_click", "pad_rs_click", "kbm_rs_click", ControlKind::DigitalButton, 0x80, "pad:rs_click",
     "key:v, mouse:middle"},
    {Control::LsUp, "ls_up", "pad_ls_up", "kbm_ls_up", ControlKind::StickDirection, Dir(kAxisLY, false), "pad:ly+",
     "key:w"},
    {Control::LsDown, "ls_down", "pad_ls_down", "kbm_ls_down", ControlKind::StickDirection, Dir(kAxisLY, true),
     "pad:ly-", "key:s"},
    {Control::LsLeft, "ls_left", "pad_ls_left", "kbm_ls_left", ControlKind::StickDirection, Dir(kAxisLX, true),
     "pad:lx-", "key:a"},
    {Control::LsRight, "ls_right", "pad_ls_right", "kbm_ls_right", ControlKind::StickDirection, Dir(kAxisLX, false),
     "pad:lx+", "key:d"},
    {Control::RsUp, "rs_up", "pad_rs_up", "kbm_rs_up", ControlKind::StickDirection, Dir(kAxisRY, false), "pad:ry+",
     "mouse:move_up"},
    {Control::RsDown, "rs_down", "pad_rs_down", "kbm_rs_down", ControlKind::StickDirection, Dir(kAxisRY, true),
     "pad:ry-", "mouse:move_down"},
    {Control::RsLeft, "rs_left", "pad_rs_left", "kbm_rs_left", ControlKind::StickDirection, Dir(kAxisRX, true),
     "pad:rx-", "mouse:move_left"},
    {Control::RsRight, "rs_right", "pad_rs_right", "kbm_rs_right", ControlKind::StickDirection, Dir(kAxisRX, false),
     "pad:rx+", "mouse:move_right"},
};

struct Name {
    const char* name;
    std::uint16_t code;
};

constexpr Name kPadButtons[] = {
    {"a", kXiA}, {"b", kXiB}, {"x", kXiX}, {"y", kXiY}, {"lb", kXiLeftShoulder}, {"rb", kXiRightShoulder},
    {"start", kXiStart}, {"back", kXiBack}, {"dpad_up", kXiDpadUp}, {"dpad_down", kXiDpadDown},
    {"dpad_left", kXiDpadLeft}, {"dpad_right", kXiDpadRight}, {"ls_click", kXiLeftThumb}, {"rs_click", kXiRightThumb},
};
constexpr Name kPadTriggers[] = {{"lt", 0}, {"rt", 1}};
constexpr Name kPadHalfAxes[] = {{"lx+", 0}, {"lx-", 1}, {"ly+", 2}, {"ly-", 3},
                                 {"rx+", 4}, {"rx-", 5}, {"ry+", 6}, {"ry-", 7}};
constexpr Name kMouseButtons[] = {{"left", 0}, {"right", 1}, {"middle", 2}, {"x1", 3}, {"x2", 4}};
constexpr Name kMouseWheel[] = {{"wheel_up", 0}, {"wheel_down", 1}};
constexpr Name kMouseMove[] = {{"move_up", 0}, {"move_down", 1}, {"move_left", 2}, {"move_right", 3}};
// Named virtual keys (letters, digits, F-keys and num0..num9 are generated); VK values of WinUser.h.
constexpr Name kKeys[] = {
    {"space", 0x20}, {"enter", 0x0D}, {"escape", 0x1B}, {"backspace", 0x08}, {"tab", 0x09}, {"capslock", 0x14},
    {"lshift", 0xA0}, {"rshift", 0xA1}, {"lctrl", 0xA2}, {"rctrl", 0xA3}, {"lalt", 0xA4}, {"ralt", 0xA5},
    {"lwin", 0x5B}, {"rwin", 0x5C}, {"menu", 0x5D}, {"up", 0x26}, {"down", 0x28}, {"left", 0x25}, {"right", 0x27},
    {"insert", 0x2D}, {"delete", 0x2E}, {"home", 0x24}, {"end", 0x23}, {"pageup", 0x21}, {"pagedown", 0x22},
    {"pause", 0x13}, {"scrolllock", 0x91}, {"numlock", 0x90}, {"printscreen", 0x2C}, {"num_add", 0x6B},
    {"num_sub", 0x6D}, {"num_mul", 0x6A}, {"num_div", 0x6F}, {"num_dec", 0x6E}, {"minus", 0xBD}, {"plus", 0xBB},
    {"comma", 0xBC}, {"period", 0xBE}, {"semicolon", 0xBA}, {"slash", 0xBF}, {"grave", 0xC0}, {"lbracket", 0xDB},
    {"backslash", 0xDC}, {"rbracket", 0xDD}, {"apostrophe", 0xDE}, {"oem102", 0xE2},
};

char Lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
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
std::string_view Trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}
template <std::size_t N>
bool Lookup(const Name (&table)[N], std::string_view name, std::uint16_t& code) noexcept {
    for (const Name& n : table) {
        if (IEquals(name, n.name)) {
            code = n.code;
            return true;
        }
    }
    return false;
}
template <std::size_t N>
const char* NameOf(const Name (&table)[N], std::uint16_t code) noexcept {
    for (const Name& n : table) {
        if (n.code == code) {
            return n.name;
        }
    }
    return nullptr;
}
int HexDigit(char c) noexcept {
    c = Lower(c);
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}
// Decimal number 1..max after `prefix` ("f12", "num7").
bool ParseNumbered(std::string_view name, std::string_view prefix, int min, int max, int& out) noexcept {
    if (name.size() <= prefix.size() || !IEquals(name.substr(0, prefix.size()), prefix)) {
        return false;
    }
    int v = 0;
    for (char c : name.substr(prefix.size())) {
        if (c < '0' || c > '9' || v > 100) {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    if (name.size() - prefix.size() > 1 && name[prefix.size()] == '0') {
        return false;  // "f01"
    }
    out = v;
    return v >= min && v <= max;
}

bool ParseKeyName(std::string_view name, std::uint16_t& vk) noexcept {
    if (name.size() == 1) {
        const char c = Lower(name[0]);
        if (c >= 'a' && c <= 'z') {
            vk = static_cast<std::uint16_t>('A' + (c - 'a'));
            return true;
        }
        if (c >= '0' && c <= '9') {
            vk = static_cast<std::uint16_t>(c);
            return true;
        }
        return false;
    }
    int n = 0;
    if (ParseNumbered(name, "f", 1, 24, n)) {
        vk = static_cast<std::uint16_t>(0x70 + n - 1);  // VK_F1
        return true;
    }
    if (name.size() == 4 && ParseNumbered(name, "num", 0, 9, n)) {
        vk = static_cast<std::uint16_t>(0x60 + n);  // VK_NUMPAD0
        return true;
    }
    if (name.size() == 5 && IEquals(name.substr(0, 3), "vk_")) {
        const int hi = HexDigit(name[3]);
        const int lo = HexDigit(name[4]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        const int code = hi * 16 + lo;
        if (code < 0x01 || code > 0xFE) {
            return false;
        }
        vk = static_cast<std::uint16_t>(code);
        return true;
    }
    return Lookup(kKeys, name, vk);
}

std::string KeyName(std::uint16_t vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return std::string(1, static_cast<char>('a' + (vk - 'A')));
    }
    if (vk >= '0' && vk <= '9') {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= 0x70 && vk <= 0x87) {
        return "f" + std::to_string(vk - 0x70 + 1);
    }
    if (vk >= 0x60 && vk <= 0x69) {
        return "num" + std::to_string(vk - 0x60);
    }
    if (const char* n = NameOf(kKeys, vk)) {
        return n;
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "vk_%02x", static_cast<unsigned>(vk & 0xFF));
    return buf;
}

// ---- evaluation helpers --------------------------------------------------------------------------------------
float HalfAxis(std::int16_t v, bool negative) noexcept {
    if (negative) {
        return v < 0 ? std::min(1.0f, -static_cast<float>(v) / 32768.0f) : 0.0f;
    }
    return v > 0 ? std::min(1.0f, static_cast<float>(v) / 32767.0f) : 0.0f;
}

float PadTokenValue(const BindToken& t, const PhysicalPad& p) noexcept {
    switch (t.kind) {
    case TokenKind::PadButton:
        return (p.buttons & t.code) != 0 ? 1.0f : 0.0f;
    case TokenKind::PadTrigger:
        return static_cast<float>(t.code == 0 ? p.leftTrigger : p.rightTrigger) / 255.0f;
    case TokenKind::PadHalfAxis: {
        const std::int16_t axes[4] = {p.lx, p.ly, p.rx, p.ry};
        return HalfAxis(axes[(t.code >> 1) & 3], (t.code & 1) != 0);
    }
    default:
        return 0.0f;
    }
}

// Value of a keyboard/mouse token; `mouseMove` receives the movement part separately (stick directions).
float KbmTokenValue(const BindToken& t, const KbmFrame& f, bool forStick, float& mouseMove) noexcept {
    switch (t.kind) {
    case TokenKind::Key:
        return t.code < f.keys.size() && f.keys[t.code] ? 1.0f : 0.0f;
    case TokenKind::MouseButton:
        return (f.mouseButtons & (1u << t.code)) != 0 ? 1.0f : 0.0f;
    case TokenKind::MouseWheel:
        return (f.wheel & (1u << t.code)) != 0 ? 1.0f : 0.0f;
    case TokenKind::MouseMove: {
        const float m = std::clamp(f.move[t.code & 3], 0.0f, 1.0f);
        if (forStick) {
            mouseMove = std::max(mouseMove, m);
            return 0.0f;
        }
        return m > 0.0f ? 1.0f : 0.0f;  // on a button: pressed while the mouse moves that way
    }
    default:
        return 0.0f;
    }
}

std::int16_t ThumbFromDirections(float v) noexcept {
    const float scaled = v >= 0.0f ? v * 32767.0f : v * 32768.0f;
    return static_cast<std::int16_t>(std::lround(std::clamp(scaled, -32768.0f, 32767.0f)));
}

std::uint8_t AnalogFromUnit(float v) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// Per control: value (raw domain) and the mouse-movement part (game-output domain, stick directions only).
struct ControlValues {
    float value[kControlCount] = {};
    float mouse[kControlCount] = {};
};

// `circularGate` (keyboard/mouse): digital stick directions are limited to the unit circle like a real pad on its
// round gate (W+D = 0.707 per axis, not 1.0/1.0; review M4 finding 6). The game clamps each axis separately
// (FUN_00057940) and does not normalise the vector. The pad mapping passes its values through unchanged.
PadState Compose(const ControlValues& cv, bool circularGate) noexcept {
    PadState s;
    float axisDir[kAxisCount][2] = {};    // [axis][negative] raw domain
    float axisMouse[kAxisCount][2] = {};  // [axis][negative] game-output domain
    for (const ControlInfo& c : kControls) {
        const int i = static_cast<int>(c.control);
        const float v = std::clamp(cv.value[i], 0.0f, 1.0f);
        switch (c.kind) {
        case ControlKind::AnalogButton:
            s.analog[c.target] = std::max(s.analog[c.target], AnalogFromUnit(v));
            break;
        case ControlKind::DigitalButton:
            if (v >= 0.5f) {
                s.buttons = static_cast<std::uint16_t>(s.buttons | c.target);
            }
            break;
        case ControlKind::StickDirection:
            axisDir[c.target >> 1][c.target & 1] = v;
            axisMouse[c.target >> 1][c.target & 1] = std::clamp(cv.mouse[i], 0.0f, 1.0f);
            break;
        }
    }
    for (int stick = 0; stick < 2; ++stick) {
        const int ax = stick * 2;
        const int ay = stick * 2 + 1;
        float dx = axisDir[ax][0] - axisDir[ax][1];
        float dy = axisDir[ay][0] - axisDir[ay][1];
        if (circularGate) {
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1.0f) {
                dx /= len;
                dy /= len;
            }
        }
        float mx = 0.0f;
        float my = 0.0f;
        GameStickInverse(axisMouse[ax][0] - axisMouse[ax][1], axisMouse[ay][0] - axisMouse[ay][1], mx, my);
        // Mouse values round up (stick_curve.h ThumbFromUnit); digital and half-axis values use the XInput scale
        // (+32767 / -32768), so the identity pad mapping passes every stick value through unchanged.
        s.thumb[ax] = std::fabs(mx) > std::fabs(dx) ? ThumbFromUnit(mx) : ThumbFromDirections(dx);
        s.thumb[ay] = std::fabs(my) > std::fabs(dy) ? ThumbFromUnit(my) : ThumbFromDirections(dy);
    }
    return s;
}

const BindingTable& BuildDefaults() noexcept {
    static const BindingTable table = [] {
        BindingTable t;
        for (const ControlInfo& c : kControls) {
            std::string error;
            ParseBindingList(c.padDefault, BindDevice::Pad, t.pad[static_cast<int>(c.control)], error);
            ParseBindingList(c.kbmDefault, BindDevice::KeyboardMouse, t.kbm[static_cast<int>(c.control)], error);
        }
        return t;
    }();
    return table;
}

}  // namespace

const ControlInfo& Info(Control control) noexcept {
    const int i = static_cast<int>(control);
    return kControls[i >= 0 && i < kControlCount ? i : 0];
}

const ControlInfo* FindControl(std::string_view id) noexcept {
    for (const ControlInfo& c : kControls) {
        if (IEquals(id, c.id)) {
            return &c;
        }
    }
    return nullptr;
}

BindDevice TokenDevice(const BindToken& token) noexcept {
    switch (token.kind) {
    case TokenKind::PadButton:
    case TokenKind::PadTrigger:
    case TokenKind::PadHalfAxis:
        return BindDevice::Pad;
    default:
        return BindDevice::KeyboardMouse;
    }
}

bool ParseToken(std::string_view text, BindToken& out) noexcept {
    text = Trim(text);
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    const std::string_view device = Trim(text.substr(0, colon));
    const std::string_view name = Trim(text.substr(colon + 1));
    std::uint16_t code = 0;
    if (IEquals(device, "pad")) {
        if (Lookup(kPadButtons, name, code)) {
            out = {TokenKind::PadButton, code};
            return true;
        }
        if (Lookup(kPadTriggers, name, code)) {
            out = {TokenKind::PadTrigger, code};
            return true;
        }
        if (Lookup(kPadHalfAxes, name, code)) {
            out = {TokenKind::PadHalfAxis, code};
            return true;
        }
        return false;
    }
    if (IEquals(device, "key")) {
        if (ParseKeyName(name, code)) {
            out = {TokenKind::Key, code};
            return true;
        }
        return false;
    }
    if (IEquals(device, "mouse")) {
        if (Lookup(kMouseButtons, name, code)) {
            out = {TokenKind::MouseButton, code};
            return true;
        }
        if (Lookup(kMouseWheel, name, code)) {
            out = {TokenKind::MouseWheel, code};
            return true;
        }
        if (Lookup(kMouseMove, name, code)) {
            out = {TokenKind::MouseMove, code};
            return true;
        }
    }
    return false;
}

std::string FormatToken(const BindToken& token) {
    const char* n = nullptr;
    switch (token.kind) {
    case TokenKind::None:
        return {};
    case TokenKind::PadButton:
        n = NameOf(kPadButtons, token.code);
        return n ? std::string("pad:") + n : std::string();
    case TokenKind::PadTrigger:
        n = NameOf(kPadTriggers, token.code);
        return n ? std::string("pad:") + n : std::string();
    case TokenKind::PadHalfAxis:
        n = NameOf(kPadHalfAxes, token.code);
        return n ? std::string("pad:") + n : std::string();
    case TokenKind::Key:
        return "key:" + KeyName(token.code);
    case TokenKind::MouseButton:
        n = NameOf(kMouseButtons, token.code);
        return n ? std::string("mouse:") + n : std::string();
    case TokenKind::MouseWheel:
        n = NameOf(kMouseWheel, token.code);
        return n ? std::string("mouse:") + n : std::string();
    case TokenKind::MouseMove:
        n = NameOf(kMouseMove, token.code);
        return n ? std::string("mouse:") + n : std::string();
    }
    return {};
}

bool BindingList::Contains(const BindToken& token) const noexcept {
    for (std::uint8_t i = 0; i < count; ++i) {
        if (tokens[i] == token) {
            return true;
        }
    }
    return false;
}

bool ParseBindingList(std::string_view text, BindDevice device, BindingList& out, std::string& error) {
    BindingList list;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t comma = text.find(',', pos);
        const std::string_view item =
            Trim(text.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
        pos = comma == std::string_view::npos ? text.size() + 1 : comma + 1;
        if (item.empty()) {
            continue;
        }
        BindToken t;
        if (!ParseToken(item, t)) {
            error = "unknown token '" + std::string(item) + "'";
            return false;
        }
        if (TokenDevice(t) != device) {
            error = "'" + std::string(item) + "' is not a " +
                    (device == BindDevice::Pad ? "controller (pad:)" : "keyboard/mouse (key:, mouse:)") + " token";
            return false;
        }
        if (list.Contains(t)) {
            continue;  // duplicates are ignored and do not count towards the limit
        }
        if (list.count >= kMaxBindingTokens) {
            error = "more than " + std::to_string(kMaxBindingTokens) + " tokens";
            return false;
        }
        list.tokens[list.count++] = t;
    }
    out = list;
    return true;
}

std::string FormatBindingList(const BindingList& list) {
    std::string s;
    for (std::uint8_t i = 0; i < list.count; ++i) {
        s += (s.empty() ? "" : ", ") + FormatToken(list.tokens[i]);
    }
    return s;
}

bool ValidatePadBindingValue(std::string_view value, std::string& error) noexcept {
    try {
        BindingList l;
        return ParseBindingList(value, BindDevice::Pad, l, error);
    } catch (...) {
        return false;
    }
}

bool ValidateKbmBindingValue(std::string_view value, std::string& error) noexcept {
    try {
        BindingList l;
        return ParseBindingList(value, BindDevice::KeyboardMouse, l, error);
    } catch (...) {
        return false;
    }
}

const BindingTable& DefaultBindings() noexcept {
    return BuildDefaults();
}

PhysicalPad ApplyStickDeadzone(const PhysicalPad& pad, std::uint32_t percent) noexcept {
    PhysicalPad p = pad;
    if (percent == 0) {
        return p;
    }
    const float dz = static_cast<float>(std::min<std::uint32_t>(percent, 90)) / 100.0f;
    const auto cut = [dz](std::int16_t& x, std::int16_t& y) {
        const float fx = static_cast<float>(x) / 32768.0f;
        const float fy = static_cast<float>(y) / 32768.0f;
        if (std::sqrt(fx * fx + fy * fy) < dz) {
            x = 0;
            y = 0;
        }
    };
    cut(p.lx, p.ly);
    cut(p.rx, p.ry);
    return p;
}

PadState MapPad(const PhysicalPad& pad, const BindingTable& bindings) noexcept {
    ControlValues cv;
    for (int i = 0; i < kControlCount; ++i) {
        const BindingList& l = bindings.pad[i];
        for (std::uint8_t k = 0; k < l.count; ++k) {
            cv.value[i] = std::max(cv.value[i], PadTokenValue(l.tokens[k], pad));
        }
    }
    return Compose(cv, false);
}

PadState MapKeyboardMouse(const KbmFrame& frame, const BindingTable& bindings) noexcept {
    ControlValues cv;
    for (const ControlInfo& c : kControls) {
        const int i = static_cast<int>(c.control);
        const BindingList& l = bindings.kbm[i];
        const bool forStick = c.kind == ControlKind::StickDirection;
        for (std::uint8_t k = 0; k < l.count; ++k) {
            cv.value[i] = std::max(cv.value[i], KbmTokenValue(l.tokens[k], frame, forStick, cv.mouse[i]));
        }
    }
    return Compose(cv, true);
}

}  // namespace inp
