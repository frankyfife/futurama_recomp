// [input] settings (src\INTERFACES.md 4.26; schema rows in core/config_schema.cpp, section "input").
//
//   pad_<control> / kbm_<control>   binding lists (input/bindings.h), defaults = m4_input.md 9
//   pad_deadzone = 0                radial dead zone in percent of the stick range before the game's own 0.4 (0..50)
//   mouse_sensitivity = 1.0         mouse look: 1.0 = kMouseCountsPerFullDeflection counts per tick give a full
//                                   stick deflection (0.1..10.0)
//   rumble = 1, rumble_strength = 100   vibration on/off and strength in percent (scales the game's motor speeds)
//   pad_mode = single               single: every controller + keyboard/mouse on port 1 (one player, m4_input.md 8.1)
//                                   per_port: controller n on port n (keyboard/mouse stays on port 1)
//   mouse_capture = 1               (input agent) a click into the window hides and confines the cursor; mouse
//                                   input counts only while it is captured (released when the window loses focus)
//   background_input = 0            (input agent) controllers also work while the window has no focus
//   menu_escape = 1                 (input agent) a key bound to START acts as BACK in the frontend menus (Esc goes
//                                   back instead of confirming, m4_input.md 9.2)
#pragma once

#include "input/bindings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace core {
struct Config;
}

namespace inp {

enum class PadMode : std::uint8_t { Single, PerPort };

// Mouse look scale: counts of mouse movement within one logic tick that ask for a full stick deflection at
// mouse_sensitivity 1.0 (24 counts per tick = 1440 counts/s at 60 Hz, about 1.8 inch/s with an 800 DPI mouse).
inline constexpr float kMouseCountsPerFullDeflection = 24.0f;

struct InputSettings {
    BindingTable bindings = DefaultBindings();
    std::uint32_t padDeadzone = 0;  // percent 0..50
    float mouseSensitivity = 1.0f;
    bool rumble = true;
    std::uint32_t rumbleStrength = 100;  // percent 0..100
    PadMode padMode = PadMode::Single;
    bool mouseCapture = true;
    bool backgroundInput = false;
    bool menuEscape = true;
};

// Reads the [input] section. Values were validated by core::LoadConfig (schema); anything that still does not parse
// keeps its default and adds a line to `warnings`.
InputSettings ReadInputSettings(const core::Config& cfg, std::vector<std::string>& warnings);
const char* PadModeName(PadMode mode) noexcept;  // "single" | "per_port"

}  // namespace inp
