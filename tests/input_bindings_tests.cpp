// Unit tests of the M4 input layer: binding lists, schema rows of [input], mapping and the game's stick curve
// (input/bindings.h, input/stick_curve.h, input/input_config.h; analysis/loader/m4_input.md 3.2, 8.4, 9).
#include "doctest.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "input/bindings.h"
#include "input/input_config.h"
#include "input/stick_curve.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace {

namespace fs = std::filesystem;

inp::BindingList Parse(const char* text, inp::BindDevice device) {
    inp::BindingList l;
    std::string error;
    REQUIRE_MESSAGE(inp::ParseBindingList(text, device, l, error), error);
    return l;
}

bool LoadIni(const std::string& body, core::Config& cfg, std::string& error) {
    const fs::path dir = fs::temp_directory_path() / "fut_input_tests";
    fs::create_directories(dir);
    const fs::path ini = dir / "input.ini";
    std::ofstream(ini, std::ios::binary) << body;
    const std::wstring cmd = L"futurama.exe \"--config=" + ini.wstring() + L"\" --userdata=C:\\fut_input_test";
    return core::LoadConfig(cmd.c_str(), L"C:\\nowhere\\futurama.exe", cfg, error);
}

}  // namespace

TEST_CASE("input bindings: tokens of the contract (src/INTERFACES.md 4.26) parse and format canonically") {
    const char* pad[] = {"pad:a", "pad:b", "pad:x", "pad:y", "pad:lb", "pad:rb", "pad:lt", "pad:rt", "pad:start",
                         "pad:back", "pad:dpad_up", "pad:dpad_down", "pad:dpad_left", "pad:dpad_right",
                         "pad:ls_click", "pad:rs_click", "pad:lx+", "pad:lx-", "pad:ly+", "pad:ly-", "pad:rx+",
                         "pad:rx-", "pad:ry+", "pad:ry-"};
    const char* mouse[] = {"mouse:left", "mouse:right", "mouse:middle", "mouse:x1", "mouse:x2", "mouse:wheel_up",
                           "mouse:wheel_down", "mouse:move_up", "mouse:move_down", "mouse:move_left",
                           "mouse:move_right"};
    const char* keys[] = {"key:space", "key:enter", "key:escape", "key:backspace", "key:tab", "key:capslock",
                          "key:lshift", "key:rshift", "key:lctrl", "key:rctrl", "key:lalt", "key:ralt", "key:lwin",
                          "key:rwin", "key:menu", "key:up", "key:down", "key:left", "key:right", "key:insert",
                          "key:delete", "key:home", "key:end", "key:pageup", "key:pagedown", "key:pause",
                          "key:scrolllock", "key:numlock", "key:printscreen", "key:num_add", "key:num_sub",
                          "key:num_mul", "key:num_div", "key:num_dec", "key:minus", "key:plus", "key:comma",
                          "key:period", "key:semicolon", "key:slash", "key:grave", "key:lbracket", "key:backslash",
                          "key:rbracket", "key:apostrophe", "key:oem102", "key:a", "key:z", "key:0", "key:9",
                          "key:f1", "key:f12", "key:f24", "key:num0", "key:num9", "key:vk_e5"};
    std::set<std::string> seen;
    for (const char* t : pad) {
        CAPTURE(t);
        inp::BindToken tok;
        REQUIRE(inp::ParseToken(t, tok));
        CHECK(inp::TokenDevice(tok) == inp::BindDevice::Pad);
        CHECK(inp::FormatToken(tok) == t);
        CHECK(seen.insert(t).second);
    }
    for (const char* t : mouse) {
        CAPTURE(t);
        inp::BindToken tok;
        REQUIRE(inp::ParseToken(t, tok));
        CHECK(inp::TokenDevice(tok) == inp::BindDevice::KeyboardMouse);
        CHECK(inp::FormatToken(tok) == t);
    }
    for (const char* t : keys) {
        CAPTURE(t);
        inp::BindToken tok;
        REQUIRE(inp::ParseToken(t, tok));
        CHECK(tok.kind == inp::TokenKind::Key);
        CHECK(inp::FormatToken(tok) == t);
    }
    // Virtual-key codes of the contract.
    const struct {
        const char* token;
        std::uint16_t vk;
    } vks[] = {{"key:space", 0x20}, {"key:a", 0x41},      {"key:0", 0x30},     {"key:f1", 0x70},
               {"key:f24", 0x87},   {"key:num0", 0x60},   {"key:lshift", 0xA0}, {"key:ralt", 0xA5},
               {"key:minus", 0xBD}, {"key:plus", 0xBB},   {"key:oem102", 0xE2}, {"key:vk_41", 0x41},
               {"key:menu", 0x5D},  {"key:printscreen", 0x2C}};
    for (const auto& v : vks) {
        CAPTURE(v.token);
        inp::BindToken tok;
        REQUIRE(inp::ParseToken(v.token, tok));
        CHECK(tok.code == v.vk);
    }
    // Case and blanks do not matter; vk_xx of a named key formats with its name.
    inp::BindToken tok;
    REQUIRE(inp::ParseToken("  KEY : Enter ", tok));
    CHECK(inp::FormatToken(tok) == "key:enter");
    REQUIRE(inp::ParseToken("key:VK_41", tok));
    CHECK(inp::FormatToken(tok) == "key:a");
    REQUIRE(inp::ParseToken("Pad:LY+", tok));
    CHECK(inp::FormatToken(tok) == "pad:ly+");
    // Not tokens.
    for (const char* bad : {"", "a", "key:", "key:foo", "key:f0", "key:f25", "key:f01", "key:num10", "key:vk_00",
                            "key:vk_ff", "key:vk_1", "key:vk_zz", "pad:lz+", "pad:white", "mouse:wheel", "joy:a",
                            "pad:a:b", "key:f+1", "key:vk_+1", "key:vk_+f", "key:f 1", "key:vk_ 1", "key:f-1"}) {
        CAPTURE(bad);
        CHECK_FALSE(inp::ParseToken(bad, tok));
    }
    // Blanks around ':' are ignored (the launcher's CanonicalToken accepts the same; review M4 findings 4/11).
    for (const char* good : {"key : a", "key: space", "pad : a", " mouse :left "}) {
        CAPTURE(good);
        CHECK(inp::ParseToken(good, tok));
    }
}

TEST_CASE("input bindings: lists - devices, duplicates, at most 4 tokens, empty = unbound") {
    std::string error;
    inp::BindingList l;
    CHECK(inp::ParseBindingList("", inp::BindDevice::Pad, l, error));
    CHECK(l.count == 0);
    CHECK(inp::ParseBindingList(" , ,", inp::BindDevice::KeyboardMouse, l, error));
    CHECK(l.count == 0);
    l = Parse("key:Enter, mouse:LEFT,key:enter", inp::BindDevice::KeyboardMouse);
    CHECK(l.count == 2);
    CHECK(inp::FormatBindingList(l) == "key:enter, mouse:left");
    CHECK_FALSE(inp::ParseBindingList("pad:a", inp::BindDevice::KeyboardMouse, l, error));
    CHECK(error.find("pad:a") != std::string::npos);
    CHECK_FALSE(inp::ParseBindingList("key:a", inp::BindDevice::Pad, l, error));
    CHECK_FALSE(inp::ParseBindingList("pad:a, pad:nope", inp::BindDevice::Pad, l, error));
    CHECK(error.find("pad:nope") != std::string::npos);
    CHECK(inp::ParseBindingList("pad:a, pad:b, pad:x, pad:y", inp::BindDevice::Pad, l, error));
    CHECK_FALSE(inp::ParseBindingList("pad:a, pad:b, pad:x, pad:y, pad:lb", inp::BindDevice::Pad, l, error));
    CHECK(error.find("more than 4") != std::string::npos);
    // Duplicates do not count towards the limit (bindings.h).
    l = Parse("pad:a, pad:a, pad:a, pad:a, pad:a", inp::BindDevice::Pad);
    CHECK(l.count == 1);
    CHECK(inp::ParseBindingList("pad:a, pad:b, pad:x, pad:y, pad:A", inp::BindDevice::Pad, l, error));
    CHECK(l.count == 4);
    CHECK_FALSE(inp::ValidatePadBindingValue("key:a", error));
    CHECK(inp::ValidateKbmBindingValue("key:a, mouse:move_up", error));
}

TEST_CASE("input bindings: the schema rows of [input] match the controls, defaults and validators") {
    int pad = 0;
    int kbm = 0;
    for (int i = 0; i < inp::kControlCount; ++i) {
        const inp::ControlInfo& c = inp::Info(static_cast<inp::Control>(i));
        CAPTURE(c.id);
        CHECK(inp::FindControl(c.id) == &c);
        const core::ConfigKeyDef* p = core::FindConfigKey("input", c.padKey);
        const core::ConfigKeyDef* k = core::FindConfigKey("input", c.kbmKey);
        REQUIRE(p != nullptr);
        REQUIRE(k != nullptr);
        CHECK(std::string("pad_") + c.id == c.padKey);
        CHECK(std::string("kbm_") + c.id == c.kbmKey);
        CHECK(p->widget == core::ConfigUiWidget::Binding);
        CHECK(k->widget == core::ConfigUiWidget::Binding);
        CHECK(p->tab == core::ConfigUiTab::Controls);
        CHECK(std::string(p->defaultValue) == c.padDefault);
        CHECK(std::string(k->defaultValue) == c.kbmDefault);
        // Defaults are canonical (what the launcher writes) and equal the built-in table.
        CHECK(inp::FormatBindingList(Parse(c.padDefault, inp::BindDevice::Pad)) == c.padDefault);
        CHECK(inp::FormatBindingList(Parse(c.kbmDefault, inp::BindDevice::KeyboardMouse)) == c.kbmDefault);
        CHECK(inp::DefaultBindings().pad[i] == Parse(c.padDefault, inp::BindDevice::Pad));
        CHECK(inp::DefaultBindings().kbm[i] == Parse(c.kbmDefault, inp::BindDevice::KeyboardMouse));
        // Label "<control> – <meaning>" (the launcher splits at the en dash).
        CHECK(std::string(p->label.de).find(" \xE2\x80\x93 ") != std::string::npos);
        CHECK(std::string(p->label.en).find(" \xE2\x80\x93 ") != std::string::npos);
        // pad_x and kbm_x sit next to each other in the tab order.
        CHECK(k->order == p->order + 1);
        std::string error;
        CHECK_FALSE(core::ValidateConfigValue(*p, "key:a", error));
        CHECK(error.find("[input] pad_") != std::string::npos);
        CHECK_FALSE(core::ValidateConfigValue(*k, "pad:a", error));
        CHECK(core::ValidateConfigValue(*k, "", error));
        ++pad;
        ++kbm;
    }
    CHECK(pad == 24);
    CHECK(kbm == 24);
    for (const char* key : {"pad_mode", "rumble", "rumble_strength", "pad_deadzone", "mouse_sensitivity",
                            "mouse_capture", "menu_escape", "background_input"}) {
        CAPTURE(key);
        const core::ConfigKeyDef* d = core::FindConfigKey("input", key);
        REQUIRE(d != nullptr);
        CHECK(d->tab == core::ConfigUiTab::Controls);
    }
    CHECK(std::string(core::ConfigUiTabName(core::ConfigUiTab::Controls)) == "controls");
    CHECK(std::string(core::ConfigUiWidgetName(core::ConfigUiWidget::Binding)) == "binding");
    CHECK(std::string(core::ConfigUiWidgetName(core::ConfigUiWidget::Slider)) == "slider");
    CHECK(std::string(core::FindConfigKey("input", "rumble_strength")->unit) == "%");
}

TEST_CASE("input config: [input] values are read; an invalid binding is a configuration error (exit code 1)") {
    core::Config cfg;
    std::string error;
    REQUIRE_MESSAGE(LoadIni("[input]\npad_a = pad:rt, pad:a\nkbm_b = \nkbm_start = key:f10\npad_mode = per_port\n"
                            "rumble = 0\nrumble_strength = 40\npad_deadzone = 12\nmouse_sensitivity = 2.5\n"
                            "mouse_capture = 0\nmenu_escape = off\nbackground_input = 1\n",
                            cfg, error),
                    error);
    std::vector<std::string> warnings;
    const inp::InputSettings s = inp::ReadInputSettings(cfg, warnings);
    CHECK(warnings.empty());
    CHECK(inp::FormatBindingList(s.bindings.pad[static_cast<int>(inp::Control::A)]) == "pad:rt, pad:a");
    CHECK(s.bindings.kbm[static_cast<int>(inp::Control::B)].count == 0);  // present and empty = not bound
    CHECK(inp::FormatBindingList(s.bindings.kbm[static_cast<int>(inp::Control::Start)]) == "key:f10");
    CHECK(s.bindings.kbm[static_cast<int>(inp::Control::A)] == inp::DefaultBindings().kbm[0]);  // missing = default
    CHECK(s.padMode == inp::PadMode::PerPort);
    CHECK_FALSE(s.rumble);
    CHECK(s.rumbleStrength == 40);
    CHECK(s.padDeadzone == 12);
    CHECK(s.mouseSensitivity == doctest::Approx(2.5));
    CHECK_FALSE(s.mouseCapture);
    CHECK_FALSE(s.menuEscape);
    CHECK(s.backgroundInput);

    core::Config defaults;
    REQUIRE(LoadIni("; nothing\n", defaults, error));
    const inp::InputSettings d = inp::ReadInputSettings(defaults, warnings);
    CHECK(d.padMode == inp::PadMode::Single);
    CHECK(d.rumble);
    CHECK(d.rumbleStrength == 100);
    CHECK(d.padDeadzone == 0);
    CHECK(d.mouseSensitivity == doctest::Approx(1.0));
    CHECK(d.mouseCapture);
    CHECK(d.menuEscape);
    CHECK_FALSE(d.backgroundInput);

    core::Config bad;
    CHECK_FALSE(LoadIni("[input]\nkbm_a = pad:a\n", bad, error));
    CHECK(error.find("kbm_a") != std::string::npos);
    CHECK_FALSE(LoadIni("[input]\npad_a = pad:a, pad:b, pad:x, pad:y, pad:lb\n", bad, error));
    CHECK_FALSE(LoadIni("[input]\nrumble_strength = 101\n", bad, error));
    CHECK_FALSE(LoadIni("[input]\npad_mode = two\n", bad, error));
    CHECK_FALSE(LoadIni("[input]\npad_x = pad:nothing\n", bad, error));
}

TEST_CASE("input mapping: the identity pad mapping passes the controller through (LB = WHITE, RB = BLACK)") {
    inp::PhysicalPad p;
    p.buttons = inp::kXiA | inp::kXiY | inp::kXiLeftShoulder | inp::kXiStart | inp::kXiDpadLeft | inp::kXiRightThumb;
    p.leftTrigger = 17;
    p.rightTrigger = 255;
    p.lx = -32768;
    p.ly = 32767;
    p.rx = 1234;
    p.ry = -5;
    const inp::PadState s = inp::MapPad(p, inp::DefaultBindings());
    CHECK(s.analog[inp::kAnalogA] == 255);
    CHECK(s.analog[inp::kAnalogB] == 0);
    CHECK(s.analog[inp::kAnalogY] == 255);
    CHECK(s.analog[inp::kAnalogWhite] == 255);
    CHECK(s.analog[inp::kAnalogBlack] == 0);
    CHECK(s.analog[inp::kAnalogLeftTrigger] == 17);  // analog trigger -> analog LT
    CHECK(s.analog[inp::kAnalogRightTrigger] == 255);
    CHECK(s.buttons == (inp::kButtonStart | inp::kButtonDpadLeft | inp::kButtonRightThumb));
    CHECK(s.thumb[inp::kAxisLX] == -32768);
    CHECK(s.thumb[inp::kAxisLY] == 32767);
    CHECK(s.thumb[inp::kAxisRX] == 1234);
    CHECK(s.thumb[inp::kAxisRY] == -5);
    // Every stick value survives the half-axis split exactly.
    for (int v = -32768; v <= 32767; v += 97) {
        inp::PhysicalPad q;
        q.ly = static_cast<std::int16_t>(v);
        CHECK(inp::MapPad(q, inp::DefaultBindings()).thumb[inp::kAxisLY] == v);
    }
    CHECK(inp::MapPad(inp::PhysicalPad{}, inp::DefaultBindings()).Neutral());
}

TEST_CASE("input mapping: free assignment - analog/digital rules, half axes on buttons, swapped sticks") {
    inp::BindingTable b = inp::DefaultBindings();
    b.pad[static_cast<int>(inp::Control::Start)] = Parse("pad:rt", inp::BindDevice::Pad);  // analog -> digital
    b.pad[static_cast<int>(inp::Control::A)] = Parse("pad:ry-", inp::BindDevice::Pad);     // half axis -> analog
    b.pad[static_cast<int>(inp::Control::LsUp)] = Parse("pad:ry+", inp::BindDevice::Pad);  // sticks swapped
    b.pad[static_cast<int>(inp::Control::LsDown)] = Parse("pad:ry-", inp::BindDevice::Pad);
    b.pad[static_cast<int>(inp::Control::RsUp)] = Parse("pad:y", inp::BindDevice::Pad);    // button -> full axis
    inp::PhysicalPad p;
    p.rightTrigger = 127;  // below half: START not pressed
    p.ry = -16384;         // half down
    inp::PadState s = inp::MapPad(p, b);
    CHECK((s.buttons & inp::kButtonStart) == 0);
    CHECK(s.analog[inp::kAnalogRightTrigger] == 127);
    CHECK(s.analog[inp::kAnalogA] == 128);  // round(0.5 * 255)
    CHECK(s.thumb[inp::kAxisLY] == -16384);
    p.rightTrigger = 128;
    p.buttons = inp::kXiY;
    p.ry = 0;  // a stick axis is up - down of its two direction controls
    s = inp::MapPad(p, b);
    CHECK((s.buttons & inp::kButtonStart) != 0);
    CHECK(s.thumb[inp::kAxisRY] == 32767);
    CHECK(s.analog[inp::kAnalogY] == 255);  // pad:y still also on Y
}

TEST_CASE("input mapping: radial dead zone [input] pad_deadzone") {
    inp::PhysicalPad p;
    p.lx = 3000;
    p.ly = 3000;  // radius 0.13
    p.rx = 10000;
    CHECK(inp::ApplyStickDeadzone(p, 0) == p);
    const inp::PhysicalPad d = inp::ApplyStickDeadzone(p, 20);
    CHECK(d.lx == 0);
    CHECK(d.ly == 0);
    CHECK(d.rx == 10000);  // radius 0.305 > 0.2: unchanged, not rescaled
}

TEST_CASE("input mapping: keyboard and mouse defaults of m4_input.md 9.2") {
    inp::KbmFrame f;
    f.keys[0x0D] = true;  // enter -> A
    f.keys['W'] = true;   // ls up
    f.keys['A'] = true;   // ls left
    f.keys['D'] = true;   // ls right: cancels left
    f.keys[0xA0] = true;  // lshift -> RT
    f.keys[0x1B] = true;  // escape -> START
    f.mouseButtons = 1u << 1;  // right -> X
    f.wheel = 1;               // wheel_up -> BLACK
    const inp::PadState s = inp::MapKeyboardMouse(f, inp::DefaultBindings());
    CHECK(s.analog[inp::kAnalogA] == 255);
    CHECK(s.analog[inp::kAnalogX] == 255);
    CHECK(s.analog[inp::kAnalogRightTrigger] == 255);
    CHECK(s.analog[inp::kAnalogBlack] == 255);
    CHECK(s.analog[inp::kAnalogWhite] == 0);
    CHECK(s.buttons == inp::kButtonStart);
    CHECK(s.thumb[inp::kAxisLY] == 32767);
    CHECK(s.thumb[inp::kAxisLX] == 0);
    CHECK(s.thumb[inp::kAxisRX] == 0);
    // Arrow keys = D-pad; space = B.
    inp::KbmFrame g;
    g.keys[0x26] = true;
    g.keys[0x20] = true;
    const inp::PadState t = inp::MapKeyboardMouse(g, inp::DefaultBindings());
    CHECK(t.buttons == inp::kButtonDpadUp);
    CHECK(t.analog[inp::kAnalogB] == 255);
}

TEST_CASE("input mapping: keyboard diagonals stay on the unit circle like a pad on its round gate") {
    // Review M4 finding 6: W+D gave 32767/32767 (length 1.41), which no real pad can produce.
    inp::KbmFrame f;
    f.keys['W'] = true;
    f.keys['D'] = true;
    const inp::PadState s = inp::MapKeyboardMouse(f, inp::DefaultBindings());
    CHECK(s.thumb[inp::kAxisLX] == 23170);  // round(0.70711 * 32767)
    CHECK(s.thumb[inp::kAxisLY] == 23170);
    // Straight stays at full deflection; the pad mapping is not changed (identity pass-through).
    f.keys['D'] = false;
    CHECK(inp::MapKeyboardMouse(f, inp::DefaultBindings()).thumb[inp::kAxisLY] == 32767);
    inp::PhysicalPad p;
    p.lx = 32767;
    p.ly = 32767;
    const inp::PadState q = inp::MapPad(p, inp::DefaultBindings());
    CHECK(q.thumb[inp::kAxisLX] == 32767);
    CHECK(q.thumb[inp::kAxisLY] == 32767);
}

TEST_CASE("input mapping: mouse movement goes through the inverse game curve; on a button it presses") {
    inp::KbmFrame f;
    f.move[3] = 0.5f;  // right: wish 0.5
    f.move[0] = 0.0f;
    inp::PadState s = inp::MapKeyboardMouse(f, inp::DefaultBindings());
    const float rx = inp::UnitFromThumb(s.thumb[inp::kAxisRX]);
    const float ry = inp::UnitFromThumb(s.thumb[inp::kAxisRY]);
    CHECK(ry == 0.0f);
    CHECK(inp::GameStickOutput(rx, ry) == doctest::Approx(0.5f).epsilon(0.001));
    // Up and left at once.
    f = {};
    f.move[0] = 0.8f;
    f.move[2] = 0.3f;
    s = inp::MapKeyboardMouse(f, inp::DefaultBindings());
    const float x = inp::UnitFromThumb(s.thumb[inp::kAxisRX]);
    const float y = inp::UnitFromThumb(s.thumb[inp::kAxisRY]);
    CHECK(inp::GameStickOutput(x, y) == doctest::Approx(-0.3f).epsilon(0.001));
    CHECK(inp::GameStickOutput(y, x) == doctest::Approx(0.8f).epsilon(0.001));
    // Larger than 1 saturates at the full deflection.
    f = {};
    f.move[1] = 7.0f;
    s = inp::MapKeyboardMouse(f, inp::DefaultBindings());
    CHECK(inp::GameStickOutput(inp::UnitFromThumb(s.thumb[inp::kAxisRY]), 0.0f) == doctest::Approx(-1.0f));
    // On a button: any movement presses.
    inp::BindingTable b = inp::DefaultBindings();
    b.kbm[static_cast<int>(inp::Control::Back)] = Parse("mouse:move_left", inp::BindDevice::KeyboardMouse);
    f = {};
    f.move[2] = 0.01f;
    CHECK((inp::MapKeyboardMouse(f, b).buttons & inp::kButtonBack) != 0);
}

TEST_CASE("input stick curve: forward curve of FUN_0009B3E0 and the inverse for every wish") {
    // Forward: dead zone 0.4, 0.2 when the other axis is outside +-0.4; full from 0.862 / 0.815.
    CHECK(inp::GameStickOutput(0.4f, 0.0f) == 0.0f);
    CHECK(inp::GameStickOutput(0.39f, 0.5f) > 0.0f);
    CHECK(inp::GameStickOutput(0.87f, 0.0f) == doctest::Approx(1.0f));
    CHECK(inp::GameStickOutput(0.82f, 0.9f) == doctest::Approx(1.0f));
    CHECK(inp::GameStickOutput(-0.7f, 0.0f) == doctest::Approx(-(0.3f * 1.3f / 0.6f)));
    // Inverse: forward(inverse(o)) == o on a grid, including small diagonals where the curve is asymmetric.
    for (int i = -20; i <= 20; ++i) {
        for (int j = -20; j <= 20; ++j) {
            const float ox = static_cast<float>(i) / 20.0f;
            const float oy = static_cast<float>(j) / 20.0f;
            float rx = 0.0f;
            float ry = 0.0f;
            inp::GameStickInverse(ox, oy, rx, ry);
            CAPTURE(ox);
            CAPTURE(oy);
            CHECK(inp::GameStickOutput(rx, ry) == doctest::Approx(ox).epsilon(1e-4));
            CHECK(inp::GameStickOutput(ry, rx) == doctest::Approx(oy).epsilon(1e-4));
            // Through the int16 thumb values the deflection is at least the wish (rounded away from 0).
            const float qx = inp::UnitFromThumb(inp::ThumbFromUnit(rx));
            const float qy = inp::UnitFromThumb(inp::ThumbFromUnit(ry));
            CHECK(std::fabs(inp::GameStickOutput(qx, qy) - ox) < 0.001f);
            CHECK(std::fabs(inp::GameStickOutput(qy, qx) - oy) < 0.001f);
        }
    }
    CHECK(inp::ThumbFromUnit(1.0f) == 32767);
    CHECK(inp::ThumbFromUnit(-1.0f) == -32768);
    CHECK(inp::ThumbFromUnit(0.0f) == 0);
}
