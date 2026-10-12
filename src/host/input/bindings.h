// Bindings of the [input] section: physical inputs -> Xbox controls (src\INTERFACES.md 4.26, the contract with the
// launcher's "Steuerung" tab; analysis\loader\m4_input.md 8.6 and 9).
//
//   [input] pad_<control> = pad:a                    Xbox controller (XInput)
//           kbm_<control> = key:enter, mouse:left     keyboard and mouse
//
// Controls are Xbox controls, not game actions: the game's action table is fixed (set 1, m4_input.md 5.1) and the
// menus read the 16 channels directly, so free assignment happens here, in the host.
//
// A binding list is a ','-separated list of tokens "<device>:<name>" (blanks around tokens and around ':' ignored,
// case-insensitive, empty = not bound, at most kMaxBindingTokens different tokens, duplicates allowed, ignored and not
// counted). pad_* keys take only pad: tokens, kbm_* keys only key: and mouse: tokens; anything else is a
// configuration error (core::ValidateConfigValue calls ValidatePadBindingValue / ValidateKbmBindingValue, exit code 1).
// The launcher's parser (launcher/src/core/bindings.cpp) accepts the same language.
//
// Token names (canonical = lower case):
//   pad:   a b x y lb rb lt rt start back dpad_up dpad_down dpad_left dpad_right ls_click rs_click
//          lx+ lx- ly+ ly- rx+ rx- ry+ ry-          (half axes, ly+ = left stick up; LB/RB = Xbox WHITE/BLACK)
//   key:   Windows virtual-key codes with US-layout position names: a..z 0..9 f1..f24 space enter escape backspace
//          tab capslock lshift rshift lctrl rctrl lalt ralt lwin rwin menu up down left right insert delete home end
//          pageup pagedown pause scrolllock numlock printscreen num0..num9 num_add num_sub num_mul num_div num_dec
//          minus plus comma period semicolon slash grave lbracket backslash rbracket apostrophe oem102, and vk_<hex>
//          (two hex digits) for any code 0x01..0xFE. enter includes the numpad Enter.
//   mouse: left right middle x1 x2 wheel_up wheel_down move_up move_down move_left move_right
//
// Values (MapPad / MapKeyboardMouse): every token of a control gives a value 0..1 - digital inputs 0 or 1, triggers
// t/255, half axes the deflection in that direction, mouse movement the deflection the game should see (sensitivity,
// see input/stick_curve.h). The control takes the largest value of its tokens. Analog buttons (A B X Y WHITE BLACK
// LT RT) get round(255 * value); digital controls (START BACK D-pad stick clicks) are pressed from 0.5 on; a stick
// axis is (up - down) or (right - left) of its two direction controls. mouse:move_* on a stick direction goes
// through the inverse of the game's stick curve (input/stick_curve.h); on any other control it presses while the
// mouse moves in that direction.
#pragma once

#include "input/input_api.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace inp {

enum class BindDevice : std::uint8_t { Pad, KeyboardMouse };

// The 24 Xbox controls of the contract (key name without the device prefix, in table order).
enum class Control : std::uint8_t {
    A, B, X, Y, White, Black, LeftTrigger, RightTrigger, Start, Back,
    DpadUp, DpadDown, DpadLeft, DpadRight, LeftThumb, RightThumb,
    LsUp, LsDown, LsLeft, LsRight, RsUp, RsDown, RsLeft, RsRight,
    Count
};
inline constexpr int kControlCount = static_cast<int>(Control::Count);
static_assert(kControlCount == 24);

enum class ControlKind : std::uint8_t { AnalogButton, DigitalButton, StickDirection };

struct ControlInfo {
    Control control;
    const char* id;       // "a", "white", "dpad_up", "ls_up", ...
    const char* padKey;   // "pad_a" (config key)
    const char* kbmKey;   // "kbm_a"
    ControlKind kind;
    std::uint8_t target;  // AnalogButton index | kButton* bit | StickAxis*2 + (negative ? 1 : 0)
    const char* padDefault;  // m4_input.md 9.1 (identity)
    const char* kbmDefault;  // m4_input.md 9.2
};
const ControlInfo& Info(Control control) noexcept;
const ControlInfo* FindControl(std::string_view id) noexcept;  // case-insensitive, nullptr if unknown

// ---- tokens ------------------------------------------------------------------------------------------------
enum class TokenKind : std::uint8_t {
    None,
    PadButton,    // code = XInput wButtons bit (XINPUT_GAMEPAD_A 0x1000, ...)
    PadTrigger,   // code 0 = left, 1 = right
    PadHalfAxis,  // code = axis * 2 + (negative ? 1 : 0), axes lx ly rx ry
    Key,          // code = virtual-key code 0x01..0xFE
    MouseButton,  // code 0 left, 1 right, 2 middle, 3 x1, 4 x2
    MouseWheel,   // code 0 up, 1 down
    MouseMove,    // code 0 up, 1 down, 2 left, 3 right
};
struct BindToken {
    TokenKind kind = TokenKind::None;
    std::uint16_t code = 0;
    bool operator==(const BindToken&) const = default;
};
inline constexpr int kMaxBindingTokens = 4;

// XInput wButtons bits (Windows XINPUT_GAMEPAD; the same values as the Xbox for the low byte).
inline constexpr std::uint16_t kXiDpadUp = 0x0001, kXiDpadDown = 0x0002, kXiDpadLeft = 0x0004, kXiDpadRight = 0x0008;
inline constexpr std::uint16_t kXiStart = 0x0010, kXiBack = 0x0020, kXiLeftThumb = 0x0040, kXiRightThumb = 0x0080;
inline constexpr std::uint16_t kXiLeftShoulder = 0x0100, kXiRightShoulder = 0x0200;
inline constexpr std::uint16_t kXiA = 0x1000, kXiB = 0x2000, kXiX = 0x4000, kXiY = 0x8000;

BindDevice TokenDevice(const BindToken& token) noexcept;
// Parses one token (blanks around it ignored, case-insensitive). False if it is not a token of the contract.
bool ParseToken(std::string_view text, BindToken& out) noexcept;
// Canonical lower-case text ("key:enter", "key:vk_e5", "pad:ly+"); "" for TokenKind::None.
std::string FormatToken(const BindToken& token);

struct BindingList {
    std::array<BindToken, kMaxBindingTokens> tokens{};
    std::uint8_t count = 0;
    bool Contains(const BindToken& token) const noexcept;
    bool operator==(const BindingList&) const = default;
};
// Parses a binding list for `device`. Duplicates are dropped. On failure `error` names the first problem
// ("unknown token 'key:foo'", "'pad:a' is not a keyboard/mouse token", "more than 4 tokens").
bool ParseBindingList(std::string_view text, BindDevice device, BindingList& out, std::string& error);
// "key:enter, mouse:left" (canonical, ", "-separated).
std::string FormatBindingList(const BindingList& list);

// Validators of the schema rows [input] pad_* / kbm_* (core/config_schema.cpp ConfigKeyDef::validate).
bool ValidatePadBindingValue(std::string_view value, std::string& error) noexcept;
bool ValidateKbmBindingValue(std::string_view value, std::string& error) noexcept;

// All bindings of both devices.
struct BindingTable {
    std::array<BindingList, kControlCount> pad{};
    std::array<BindingList, kControlCount> kbm{};
    const BindingList& Of(BindDevice device, Control c) const noexcept {
        return device == BindDevice::Pad ? pad[static_cast<int>(c)] : kbm[static_cast<int>(c)];
    }
    BindingList& Of(BindDevice device, Control c) noexcept {
        return device == BindDevice::Pad ? pad[static_cast<int>(c)] : kbm[static_cast<int>(c)];
    }
};
// The defaults of m4_input.md 9 (= the schema defaults).
const BindingTable& DefaultBindings() noexcept;

// ---- evaluation --------------------------------------------------------------------------------------------
// Physical state of one XInput controller (Windows XINPUT_GAMEPAD layout).
struct PhysicalPad {
    std::uint16_t buttons = 0;  // kXi* bits
    std::uint8_t leftTrigger = 0;
    std::uint8_t rightTrigger = 0;
    std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool operator==(const PhysicalPad&) const = default;
};
// Radial dead zone in percent of the stick range (0..90): inside it the stick reads 0, outside it is passed on
// unchanged ([input] pad_deadzone; the game's own dead zone 0.4 follows).
PhysicalPad ApplyStickDeadzone(const PhysicalPad& pad, std::uint32_t percent) noexcept;
// Maps a controller through the pad_* bindings.
PadState MapPad(const PhysicalPad& pad, const BindingTable& bindings) noexcept;

// Keyboard and mouse inputs of one tick (input/kbm_source.h builds it).
struct KbmFrame {
    std::array<bool, 256> keys{};     // virtual-key codes held (or tapped since the last tick)
    std::uint8_t mouseButtons = 0;    // bit n = MouseButton code n
    std::uint8_t wheel = 0;           // bit 0 = wheel_up pressed this tick, bit 1 = wheel_down
    float move[4] = {};               // MouseMove up/down/left/right: game-side stick deflection 0..1 (sensitivity applied)
};
PadState MapKeyboardMouse(const KbmFrame& frame, const BindingTable& bindings) noexcept;

}  // namespace inp
