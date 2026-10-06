// Settings of the gfx module (see gfx_config.h).
#include "gfx/gfx_config.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "core/log.h"

#include <algorithm>
#include <cmath>
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

std::string_view TrimView(std::string_view v) noexcept {
    while (!v.empty() && (v.front() == ' ' || v.front() == '	')) {
        v.remove_prefix(1);
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '	')) {
        v.remove_suffix(1);
    }
    return v;
}

// [render] <key> M3c values: parsed with the gfx parsers; the configuration loader accepted the text, so an
// invalid value here is a GFX/warn and the default.
template <class Parse, class... Out>
void ParseOrWarn(const core::Config& cfg, const char* key, const char* expected, Parse parse, Out*... out) {
    if (!cfg.Has("render", key)) {
        return;
    }
    const std::string text = cfg.GetString("render", key);
    if (!parse(text, out...)) {
        FUT_LOGW(Gfx, "[render] %s = '%s' is not %s - using the default", key, text.c_str(), expected);
    }
}

}  // namespace

// The syntax of the M3c keys is the configuration schema's (core/config_schema.h: the same parsers validate
// the INI), the ranges are the renderer's.
bool ParseRenderResolution(std::string_view text, std::uint32_t* width, std::uint32_t* height) noexcept {
    core::ConfigResolution r;
    if (!core::ParseConfigResolution(text, r)) {
        return false;
    }
    if (width != nullptr) {
        *width = r.isAuto ? 0 : r.width;
    }
    if (height != nullptr) {
        *height = r.isAuto ? 0 : r.height;
    }
    return true;
}

bool ParseRenderScale(std::string_view text, float* scale) noexcept {
    double v = 0.0;
    if (!core::ParseConfigFloat(text, v) || v < kMinRenderScale - 1e-6 || v > kMaxRenderScale + 1e-6) {
        return false;
    }
    if (scale != nullptr) {
        *scale = static_cast<float>(std::clamp(v, static_cast<double>(kMinRenderScale),
                                               static_cast<double>(kMaxRenderScale)));
    }
    return true;
}

bool ParseMsaa(std::string_view text, std::uint32_t* samples) noexcept {
    text = TrimView(text);
    std::uint32_t n = 0;
    if (text.size() == 3 && (text[0] | 0x20) == 'o' && (text[1] | 0x20) == 'f' && (text[2] | 0x20) == 'f') {
        n = 1;
    } else if (!core::ParseConfigU32(text, n) || (n != 2 && n != 4 && n != 8)) {
        return false;
    }
    if (samples != nullptr) {
        *samples = n;
    }
    return true;
}

bool ParseAnisotropy(std::string_view text, std::uint32_t* level) noexcept {
    text = TrimView(text);
    std::uint32_t n = 0;
    if (text.size() == 3 && (text[0] | 0x20) == 'o' && (text[1] | 0x20) == 'f' && (text[2] | 0x20) == 'f') {
        n = 1;
    } else if (!core::ParseConfigU32(text, n) || (n != 2 && n != 4 && n != 8 && n != 16)) {
        return false;
    }
    if (level != nullptr) {
        *level = n;
    }
    return true;
}

bool ParseFrameRate(std::string_view text, std::uint32_t* hz, platform::pacing::FrameRateMode* mode) noexcept {
    text = TrimView(text);
    std::uint32_t rate = 0;
    platform::pacing::FrameRateMode m = platform::pacing::FrameRateMode::Fixed;
    const auto equalsNoCase = [&](std::string_view word) {
        if (text.size() != word.size()) {
            return false;
        }
        for (std::size_t i = 0; i < word.size(); ++i) {
            if ((text[i] | 0x20) != word[i]) {
                return false;
            }
        }
        return true;
    };
    if (equalsNoCase("display")) {
        m = platform::pacing::FrameRateMode::Display;
        rate = static_cast<std::uint32_t>(platform::pacing::kMaxFrameRate);
    } else if (text == "120") {
        m = platform::pacing::FrameRateMode::Max120;
        rate = static_cast<std::uint32_t>(platform::pacing::kMaxFrameRate);
    } else if (!core::ParseConfigU32(text, rate)) {
        return false;
    }
    if (hz != nullptr) {
        *hz = rate;
    }
    if (mode != nullptr) {
        *mode = m;
    }
    return true;
}

Settings SettingsFromConfig(const core::Config& cfg) {
    Settings s;
    s.frameLimit = cfg.GetU32("debug", "frames", 0);
    // M3b: the frame rate follows the game's tick rate ([game] tick_rate, validated 60|50 by the configuration
    // loader; xapi/game_timing.h) unless [render] frame_rate is given; the fixed 50 Hz default is gone.
    s.tickRate = cfg.GetU32("game", "tick_rate", 60);
    if (s.tickRate == 0) {
        s.tickRate = 60;
    }
    s.frameRate = s.tickRate;
    // M3d: "120" / "display" choose the rate from the refresh (interpolated above the tick rate); numbers keep M3b.
    if (cfg.Has("render", "frame_rate")) {
        const std::string text = cfg.GetString("render", "frame_rate");
        if (!ParseFrameRate(text, &s.frameRate, &s.frameRateMode)) {
            FUT_LOGW(Gfx, "[render] frame_rate = '%s' is not 60|120|display or a number - using the tick rate %u",
                     text.c_str(), s.tickRate);
            s.frameRate = s.tickRate;
            s.frameRateMode = platform::pacing::FrameRateMode::Fixed;
        }
    }
    if (s.frameRateMode == platform::pacing::FrameRateMode::Fixed && s.frameRate != 0 && s.frameRate != s.tickRate) {
        FUT_LOGW(Gfx, "[render] frame_rate = %u differs from [game] tick_rate = %u: the game steps %u times per "
                 "second but shows %u frames, so some frames get 0 or 2 steps (remove frame_rate to follow the tick "
                 "rate; M3d: frame_rate = 120 or display shows up to 120 interpolated frames)", s.frameRate, s.tickRate,
                 s.tickRate, s.frameRate);
    }
    s.lowLatency = cfg.GetBool("render", "low_latency", true);
    const std::string pacing = Lower(cfg.GetString("render", "pacing", "auto"));  // validated choice
    if (!platform::pacing::ParseMode(pacing, &s.pacing)) {
        FUT_LOGW(Gfx, "[render] pacing = '%s' is not auto|vsync|timer|off - using auto", pacing.c_str());
        s.pacing = platform::pacing::Mode::Auto;
    }
    s.pacingTrace = cfg.GetPath("render", "pacing_trace", {});
    s.loadTextures = cfg.GetBool("render", "load_textures", true);
    const std::string mode = Lower(cfg.GetString("render", "d3d_traps", "abort"));
    if (mode == "log") {
        s.trapMode = TrapMode::Log;
    } else if (mode != "abort") {
        FUT_LOGW(Gfx, "[render] d3d_traps = '%s' is neither 'abort' nor 'log' - using 'abort'", mode.c_str());
    }
    // M3 keys (choice values are validated by the configuration loader, core/config.cpp kKnownKeys).
    s.backend = Lower(cfg.GetString("render", "backend", "d3d11")) == "null" ? RenderBackend::Null
                                                                             : RenderBackend::D3D11;
    s.vsync = cfg.GetBool("render", "vsync", true);
    // Combinations whose value has no effect (review M3b finding 11; PacingConfigOf).
    if (s.frameRate == 0 && s.pacing != platform::pacing::Mode::Auto && s.pacing != platform::pacing::Mode::Off) {
        FUT_LOGW(Gfx, "[render] frame_rate = 0 (unthrottled) has no effect with [render] pacing = %s: the pacer runs "
                 "at the tick rate %u Hz (use pacing = off for an unthrottled run)",
                 platform::pacing::ModeName(s.pacing), s.tickRate);
    }
    if (!s.vsync && s.pacing == platform::pacing::Mode::Vsync) {
        FUT_LOGW(Gfx, "[render] vsync = 0 has no effect with [render] pacing = vsync (always vblank-coupled; use "
                 "pacing = timer or auto for presents without vsync)");
    }
    const std::string layer = Lower(cfg.GetString("render", "debug_layer", "auto"));
    s.debugLayer = layer == "on" ? DebugLayerMode::On : layer == "off" ? DebugLayerMode::Off : DebugLayerMode::Auto;
    s.softwareAdapter = Lower(cfg.GetString("render", "adapter", "hardware")) == "warp";
    s.windowScale = cfg.GetU32("render", "window_scale", 2);
    if (s.windowScale == 0 || s.windowScale > 8) {
        FUT_LOGW(Gfx, "[render] window_scale = %u is outside 1..8 - using 2", s.windowScale);
        s.windowScale = 2;
    }
    s.fullscreen = cfg.GetBool("render", "fullscreen", false);
    s.monitor = std::string(
        TrimView(cfg.GetString("render", "monitor", core::ConfigKeyDefault("render", "monitor"))));
    if (Lower(s.monitor) == "primary") {
        s.monitor.clear();
    }
    const std::string scaling = Lower(cfg.GetString("render", "scaling", "linear"));
    s.scaling = scaling == "point"     ? rhi::PresentScaling::Point
                : scaling == "integer" ? rhi::PresentScaling::Integer
                                       : rhi::PresentScaling::Linear;
    s.screenshotDir = cfg.GetPath("render", "screenshot_dir", cfg.userdata / "screenshots");
    s.screenshotFrames = FrameList(cfg, "screenshot_frames");
    s.traceFrames = FrameList(cfg, "trace_frames");
    s.validate = cfg.GetBool("render", "validate", false);
    // M3c: render resolution and anti-aliasing (gfx_config.h; invalid values: GFX/warn + default).
    ParseOrWarn(cfg, "resolution", "auto or <W>x<H> (160..16384 each)",
                [](const std::string& t, std::uint32_t* w, std::uint32_t* h) { return ParseRenderResolution(t, w, h); },
                &s.resolutionWidth, &s.resolutionHeight);
    ParseOrWarn(cfg, "render_scale", "a number from 1.0 to 2.0",
                [](const std::string& t, float* v) { return ParseRenderScale(t, v); }, &s.renderScale);
    ParseOrWarn(cfg, "msaa", "off|2|4|8", [](const std::string& t, std::uint32_t* v) { return ParseMsaa(t, v); },
                &s.msaa);
    ParseOrWarn(cfg, "anisotropy", "off|2|4|8|16",
                [](const std::string& t, std::uint32_t* v) { return ParseAnisotropy(t, v); }, &s.anisotropy);
    s.alphaToCoverage = cfg.GetBool("render", "alpha_to_coverage", kDefaultAlphaToCoverage);
    s.linearAa = cfg.GetBool("render", "linear_aa", kDefaultLinearAa);  // review M3c finding 3
    if (cfg.Has("render", "screenshot_size")) {
        const std::string size = Lower(cfg.GetString("render", "screenshot_size"));
        if (size == "internal") {
            s.screenshotSize = ScreenshotSize::Internal;
        } else if (size == "output") {
            s.screenshotSize = ScreenshotSize::Output;
        } else {
            FUT_LOGW(Gfx, "[render] screenshot_size = '%s' is not internal|output - using output", size.c_str());
        }
    }
    return s;
}

namespace {

Settings Load() {
    return SettingsFromConfig(core::GetConfig());
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

platform::PacingConfig PacingConfigOf(const Settings& settings) noexcept {
    platform::PacingConfig c;
    c.mode = settings.frameRate == 0 && settings.pacing == platform::pacing::Mode::Auto ? platform::pacing::Mode::Off
                                                                                        : settings.pacing;
    c.vsync = settings.vsync;
    c.targetHz = static_cast<double>(settings.frameRate != 0 ? settings.frameRate : settings.tickRate);
    c.tickHz = settings.tickRate;
    c.tracePath = settings.pacingTrace.empty() ? nullptr : settings.pacingTrace.c_str();
    c.rateMode = settings.frameRateMode;  // M3d
    c.justInTime = settings.lowLatency;
    return c;
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
