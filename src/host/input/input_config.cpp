// [input] settings reader (see input_config.h).
#include "input/input_config.h"

#include "core/config.h"
#include "core/config_schema.h"

#include <algorithm>

namespace inp {
namespace {

std::uint32_t ReadPercent(const core::Config& cfg, const char* key, std::uint32_t max,
                          std::vector<std::string>& warnings) {
    const std::string def(core::ConfigKeyDefault("input", key));
    std::uint32_t defValue = 0;
    core::ParseConfigU32(def, defValue);
    const std::string text = cfg.GetString("input", key, def);
    std::uint32_t v = defValue;
    if (!core::ParseConfigU32(text, v)) {
        warnings.push_back("[input] " + std::string(key) + " = '" + text + "' is not a number - using " + def);
        v = defValue;
    }
    return std::min(v, max);
}

}  // namespace

const char* PadModeName(PadMode mode) noexcept {
    return mode == PadMode::PerPort ? "per_port" : "single";
}

InputSettings ReadInputSettings(const core::Config& cfg, std::vector<std::string>& warnings) {
    InputSettings s;
    for (int i = 0; i < kControlCount; ++i) {
        const ControlInfo& c = Info(static_cast<Control>(i));
        for (const BindDevice device : {BindDevice::Pad, BindDevice::KeyboardMouse}) {
            const char* key = device == BindDevice::Pad ? c.padKey : c.kbmKey;
            if (!cfg.Has("input", key)) {
                continue;  // default
            }
            const std::string text = cfg.GetString("input", key);
            BindingList list;
            std::string error;
            if (ParseBindingList(text, device, list, error)) {
                s.bindings.Of(device, c.control) = list;
            } else {
                warnings.push_back("[input] " + std::string(key) + " = '" + text + "': " + error + " - using the default");
            }
        }
    }
    s.padDeadzone = ReadPercent(cfg, "pad_deadzone", 50, warnings);
    s.rumbleStrength = ReadPercent(cfg, "rumble_strength", 100, warnings);
    {
        const std::string def(core::ConfigKeyDefault("input", "mouse_sensitivity"));
        const std::string text = cfg.GetString("input", "mouse_sensitivity", def);
        double d = 1.0;
        if (!core::ParseConfigFloat(text, d) || d <= 0.0) {
            warnings.push_back("[input] mouse_sensitivity = '" + text + "' is invalid - using " + def);
            core::ParseConfigFloat(def, d);
        }
        s.mouseSensitivity = static_cast<float>(std::clamp(d, 0.1, 10.0));
    }
    s.rumble = cfg.GetBool("input", "rumble", true);
    s.mouseCapture = cfg.GetBool("input", "mouse_capture", true);
    s.backgroundInput = cfg.GetBool("input", "background_input", false);
    s.menuEscape = cfg.GetBool("input", "menu_escape", true);
    const std::string mode = cfg.GetString("input", "pad_mode", core::ConfigKeyDefault("input", "pad_mode"));
    if (core::MatchesConfigChoice(mode, "per_port")) {
        s.padMode = PadMode::PerPort;
    } else if (!mode.empty() && !core::MatchesConfigChoice(mode, "single")) {
        warnings.push_back("[input] pad_mode = '" + mode + "' is invalid - using single");
    }
    return s;
}

}  // namespace inp
