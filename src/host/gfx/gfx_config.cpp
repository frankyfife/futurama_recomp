// Settings of the gfx module (see gfx_config.h).
#include "gfx/gfx_config.h"

#include "core/config.h"
#include "core/log.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>

namespace gfx {
namespace {

const Settings* g_override = nullptr;

std::string Lower(std::string v) {
    for (char& c : v) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return v;
}

// [render] <key> = list of displayed-frame numbers (>= 1), sorted and unique.
std::vector<std::uint32_t> FrameList(const core::Config& cfg, const char* key) {
    std::vector<std::uint32_t> frames;
    for (const std::string& item : cfg.GetList("render", key)) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(item.c_str(), &end, 0);
        if (end == item.c_str() || *end != 0 || v == 0) {
            FUT_LOGW(Gfx, "[render] %s: '%s' is not a frame number >= 1 - ignored", key, item.c_str());
            continue;
        }
        frames.push_back(static_cast<std::uint32_t>(v));
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

Settings Load() {
    const core::Config& cfg = core::GetConfig();
    Settings s;
    s.frameLimit = cfg.GetU32("debug", "frames", 0);
    s.frameRate = cfg.GetU32("render", "frame_rate", 50);
    s.loadTextures = cfg.GetBool("render", "load_textures", true);
    const std::string mode = Lower(cfg.GetString("render", "d3d_traps", "abort"));
    if (mode == "log") {
        s.trapMode = TrapMode::Log;
    } else if (mode != "abort") {
        FUT_LOGW(Gfx, "[render] d3d_traps = '%s' is neither 'abort' nor 'log' - using 'abort'", mode.c_str());
    }
    // M3 keys (choice values are validated by the configuration loader, core/config.cpp kKnownKeys).
    s.backend = Lower(cfg.GetString("render", "backend", "null")) == "d3d11" ? RenderBackend::D3D11
                                                                              : RenderBackend::Null;
    s.vsync = cfg.GetBool("render", "vsync", true);
    const std::string layer = Lower(cfg.GetString("render", "debug_layer", "auto"));
    s.debugLayer = layer == "on" ? DebugLayerMode::On : layer == "off" ? DebugLayerMode::Off : DebugLayerMode::Auto;
    s.softwareAdapter = Lower(cfg.GetString("render", "adapter", "hardware")) == "warp";
    s.windowScale = cfg.GetU32("render", "window_scale", 2);
    if (s.windowScale == 0 || s.windowScale > 8) {
        FUT_LOGW(Gfx, "[render] window_scale = %u is outside 1..8 - using 2", s.windowScale);
        s.windowScale = 2;
    }
    s.fullscreen = cfg.GetBool("render", "fullscreen", false);
    s.screenshotDir = cfg.GetPath("render", "screenshot_dir", cfg.userdata / "screenshots");
    s.screenshotFrames = FrameList(cfg, "screenshot_frames");
    s.traceFrames = FrameList(cfg, "trace_frames");
    s.validate = cfg.GetBool("render", "validate", false);
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

bool WantDebugLayer(const Settings& settings) noexcept {
    switch (settings.debugLayer) {
    case DebugLayerMode::On:
        return true;
    case DebugLayerMode::Off:
        return false;
    case DebugLayerMode::Auto:
        break;
    }
#ifdef _DEBUG
    return true;
#else
    return false;
#endif
}

}  // namespace gfx
