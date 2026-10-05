// Settings of the gfx module (see gfx_config.h).
#include "gfx/gfx_config.h"

#include "core/config.h"
#include "core/log.h"

#include <mutex>
#include <string>

namespace gfx {
namespace {

const Settings* g_override = nullptr;

Settings Load() {
    const core::Config& cfg = core::GetConfig();
    Settings s;
    s.frameLimit = cfg.GetU32("debug", "frames", 0);
    s.frameRate = cfg.GetU32("render", "frame_rate", 50);
    s.loadTextures = cfg.GetBool("render", "load_textures", true);
    std::string mode = cfg.GetString("render", "d3d_traps", "abort");
    for (char& c : mode) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (mode == "log") {
        s.trapMode = TrapMode::Log;
    } else if (mode != "abort") {
        FUT_LOGW(Gfx, "[render] d3d_traps = '%s' is neither 'abort' nor 'log' - using 'abort'", mode.c_str());
    }
    return s;
}

}  // namespace

const Settings& GetSettings() noexcept {
    if (g_override != nullptr) {
        return *g_override;
    }
    static Settings settings;
    static std::once_flag once;
    try {
        std::call_once(once, [] { settings = Load(); });
    } catch (...) {
        // Defaults stay in place (std::string allocation failure while reading the configuration).
    }
    return settings;
}

void OverrideSettingsForTest(const Settings* settings) noexcept {
    g_override = settings;
}

}  // namespace gfx
