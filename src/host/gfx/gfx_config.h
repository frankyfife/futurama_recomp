// Settings of the gfx module (M2: NiPCRenderer null renderer, D3D shim and traps), read once from
// the process configuration (core/config.h, keys registered in kKnownKeys of core/config.cpp):
//
//   [debug] frames = N          (--frames=N, design 5.4) end the process with exit code 0 after N
//                               frames displayed by NiPCRenderer::DisplayFrame (slot 16); 0 = no limit
//   [render] frame_rate = 60    game frames per second the pacer aims at (platform/frame_pacer.h). M3b: the
//                               default is [game] tick_rate (60 since M3b, xapi/game_timing.h; M2/M3 had a
//                               fixed 50 = PAL); 0 = unthrottled (= [render] pacing = off) ONLY with
//                               pacing = auto - with vsync/timer the pacer keeps the tick rate (GFX/warn,
//                               review M3b finding 11). A value other
//                               than the tick rate is allowed for experiments and logged as a warning (the
//                               fixed-step loop then takes 0 or 2 steps in some frames)
//                               M3d: also "120" (up to 120 frames per second, interpolated) and "display" (the
//                               refresh, at most 120): Settings::frameRateMode (platform::pacing::FrameRateMode;
//                               frameRate is then the cap 120). "60" and other numbers keep the M3b meaning
//   [render] low_latency = on   (M3d) just-in-time frame start on the vblank plans (platform/frame_pacer.h)
//   [render] pacing = auto      (M3b) auto | vsync | timer | off (platform/pacer_policy.h Mode)
//   [render] pacing_trace       (M3b) CSV file with one line per frame, default off
//   [game] tick_rate = 60       (timing agent's key, read here as the default frame rate)
//   [render] d3d_traps = abort  abort | log: a hit D3D trap aborts (core::Fatal) or only logs and
//                               returns 0 with the purge of the original
//   [render] load_textures = 1  slot 65 (CreateSourceTextureRendererData) reads a texture that has only
//                               a file name through the game's NiImageConverter, like FUN_0015E010, to
//                               fill the size fields of the renderer data; 0 = size 0 for such textures
// M3 (GFX_CONTRACT.md 8, keys registered in phase 1; the M3 integration made d3d11 the default):
//   [render] backend = d3d11    d3d11 | null: d3d11 = window + D3D11 RHI (gfx/rhi/rhi.h, gfx/nipc_gpu.h);
//                               null = M2 null renderer (no window, nothing drawn; M1/M2 regression)
//   [render] vsync = 1          Present waits for the vertical blank (M3b: the pacer's vblank-coupled plans;
//                               0 = [render] pacing = auto behaves like timer and timer/off may tear;
//                               no effect with pacing = vsync, which logs GFX/warn)
//   [render] debug_layer = auto auto | on | off: D3D11 debug layer (auto = on in Debug builds); a missing
//                               layer is reported (GFX/warn), the device is created without it
//   [render] adapter = hardware hardware | warp (software rasterizer, tests)
//   [render] window_scale = 2   initial client area = Create size (640x480) * N
//   [render] fullscreen = 0     borderless window on the whole monitor
//   [render] monitor = primary  M3c: primary | <GDI device name> (\\.\DISPLAY2 as --query-gpu lists it): the
//                               monitor the window opens on (platform::WindowOptions::monitor); not connected
//                               -> primary monitor + Core warning
//   [render] scaling = linear   linear | point | integer: how Present maps the 640x480 back buffer onto the
//                               window (rhi::PresentScaling; M3 review finding 11)
//   [render] screenshot_dir     PNG screenshots (default <userdata>\screenshots)
//   [render] screenshot_frames  list of displayed-frame numbers that get an automatic screenshot
//   [render] trace_frames       list of displayed-frame numbers whose draw calls are logged one by one
//                               (GFX/info; M3 exit criterion 1: every draw of a scene accounted for)
//   [render] validate = 0       phase-2 harness (GFX_CONTRACT.md 10): with backend = null, slots 47-51,
//                               54, 55 and 65 also run ni_extract / texconv on the real guest data and
//                               count the results (exit line "validate: ..."); nothing is drawn
// M3c (render resolution and anti-aliasing; GFX_CONTRACT.md 12, contract item 5 of the M3c workflow):
//   [render] resolution = auto  auto | <W>x<H>: the 4:3 game image is rendered at the largest 4:3 size inside
//                               the window's view (auto) or inside W x H (e.g. 1920x1080 -> 1440x1080,
//                               640x480 = the original size); W, H 160..16384. The guest keeps
//                               seeing 640x480 (slots 21/22); viewports, clear rectangles, pre-transformed
//                               vertices and the 1/16-pixel snap are scaled from 640x480 to it
//   [render] render_scale = 1.0 1.0..2.0 (UI steps of 0.25): supersampling on top of the resolution (the back
//                               buffers are resolution x scale, downsampled with an exact box filter for 2.0
//                               and Catmull-Rom with anti-ringing for fractional factors)
//   [render] msaa = 4           off | 2 | 4 | 8: multisampled back buffers + depth, resolved before the
//                               downsample and Present; clamped to the device's support (GFX/info line)
//   [render] alpha_to_coverage = on  on | off: alpha-tested materials use alpha-to-coverage with alpha
//                               sharpening (only with MSAA; ff_pipeline.h RenderOptions)
//   [render] anisotropy = 16    off | 2 | 4 | 8 | 16: anisotropic filtering of mip-mapped 3D textures
//   [render] linear_aa = off    on | off (review M3c finding 3): the MSAA resolve and the supersampling downsample
//                               average linear light instead of gamma-encoded values (the Xbox and the default
//                               average gamma); rendering and blending stay unchanged (rhi DeviceDesc::
//                               linearResolve). No effect without MSAA and render_scale 1
//   [render] screenshot_size = output  internal | output: screenshots of the back buffer at the internal
//                               render size (resolution x render_scale, MSAA resolved) or of the presented
//                               image (resolution, after the downsample)
// G1 (character outlines, analysis/loader/g1_outlines.md, GFX_CONTRACT.md "Konturen"):
//   [render] outline_color = original    original | RRGGBB (hex): colour of the inverted-hull shells
//                               ("QDOutline", technique A) and of the NiOutline ribbons (technique B)
//   [render] outline_opacity = original  original | 0..1: A replaces the material alpha 0.75, B multiplies
//                               the ribbon vertex alpha
//   [render] outline_thickness = 1.0     0..4: factor on the original thickness (A: shell offset per vertex,
//                               B: DAT_002D7AB0 = 0.003 * k); 0 = outlines off
//   All three at their defaults = no outline code runs (bit-identical to the original look).
// "Classic" (bit-identical to M3, the xemu regression): resolution = 640x480, render_scale = 1, msaa = off,
// anisotropy = off (alpha_to_coverage has no effect without MSAA).
// The section is [render], not [gfx]: tests/test_core.cpp uses [gfx] as its example of an unknown
// section (a known section would turn its unknown key into an error).
#pragma once

#include "gfx/rhi/rhi.h"
#include "platform/frame_pacer.h"
#include "platform/pacer_policy.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace core {
struct Config;
}

namespace gfx {

enum class TrapMode : std::uint8_t { Abort, Log };
enum class RenderBackend : std::uint8_t { Null, D3D11 };          // M3
enum class DebugLayerMode : std::uint8_t { Auto, On, Off };       // M3
enum class ScreenshotSize : std::uint8_t { Internal, Output };    // M3c [render] screenshot_size

// M3c defaults of the new [render] keys = the defaults of the configuration schema (core/config_schema.cpp;
// tests/aa_tests.cpp compares them).
inline constexpr float kDefaultRenderScale = 1.0f;
inline constexpr std::uint32_t kDefaultMsaa = 4;            // samples per pixel (1 = off)
inline constexpr bool kDefaultAlphaToCoverage = true;
inline constexpr std::uint32_t kDefaultAnisotropy = 16;     // 1 = off
inline constexpr bool kDefaultLinearAa = false;             // review M3c finding 3: gamma like the Xbox
inline constexpr ScreenshotSize kDefaultScreenshotSize = ScreenshotSize::Output;
// Limits of [render] resolution / render_scale.
inline constexpr std::uint32_t kMaxRenderDimension = 16384;  // D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
inline constexpr float kMinRenderScale = 1.0f;
inline constexpr float kMaxRenderScale = 2.0f;
inline constexpr float kRenderScaleStep = 0.25f;  // launcher step (any value in range is accepted)

// G1 outline options (OutlineSettings::Active() == false at the defaults).
inline constexpr float kDefaultOutlineThickness = 1.0f;
inline constexpr float kMaxOutlineThickness = 2.5f;  // above ~2 the shells show through (nose, eyes)
struct OutlineSettings {
    bool hasColor = false;                      // [render] outline_color != original
    float color[3] = {0.0f, 0.0f, 0.0f};        // 0..1 (RRGGBB / 255)
    bool hasOpacity = false;                    // [render] outline_opacity != original
    float opacity = 0.75f;
    float thickness = kDefaultOutlineThickness; // [render] outline_thickness
    bool Active() const noexcept { return hasColor || hasOpacity || thickness != kDefaultOutlineThickness; }
};
// "original" (case-insensitive) -> *has = false; "RRGGBB" (6 hex digits, optional leading '#') -> *has = true and
// rgb / 255. Blanks around the value are ignored. The schema (core/config_schema.cpp) accepts the same values.
bool ParseOutlineColor(std::string_view text, bool* has, float rgb[3]) noexcept;
// "original" -> *has = false; a decimal number 0..1 -> *has = true.
bool ParseOutlineOpacity(std::string_view text, bool* has, float* opacity) noexcept;
// A decimal number 0..kMaxOutlineThickness.
bool ParseOutlineThickness(std::string_view text, float* thickness) noexcept;

// M3c parsers = the syntax of the new keys (values as in the INI; blanks ignored; case-insensitive). They use
// the configuration schema's value parsers (core/config_schema.h), so the INI validation and the renderer agree.
// "auto" -> 0 x 0; "<W>x<H>" (also "X", blanks around the numbers) with W, H in
// core::kConfigResolutionMin..kConfigResolutionMax (160..16384).
bool ParseRenderResolution(std::string_view text, std::uint32_t* width, std::uint32_t* height) noexcept;
// A decimal number ("1", "1.5", '.' only) from kMinRenderScale to kMaxRenderScale.
bool ParseRenderScale(std::string_view text, float* scale) noexcept;
// off | 2 | 4 | 8 -> samples per pixel 1 / 2 / 4 / 8.
bool ParseMsaa(std::string_view text, std::uint32_t* samples) noexcept;
// off | 2 | 4 | 8 | 16 -> maximum anisotropy 1 / 2 / 4 / 8 / 16.
bool ParseAnisotropy(std::string_view text, std::uint32_t* level) noexcept;
// M3d [render] frame_rate: "120" -> Max120 with the cap 120, "display" -> Display with the cap 120 (case-insensitive),
// any other unsigned number (decimal or 0x hex; "60", "0", developer values) -> Fixed with that rate. The schema
// (core/config_schema.cpp) accepts the same values.
bool ParseFrameRate(std::string_view text, std::uint32_t* hz, platform::pacing::FrameRateMode* mode) noexcept;

struct Settings {
    std::uint32_t frameLimit = 0;
    std::uint32_t frameRate = 60;  // M3b: default = tickRate (was 50)
    TrapMode trapMode = TrapMode::Abort;
    bool loadTextures = true;
    // M3 (GFX_CONTRACT.md 8)
    RenderBackend backend = RenderBackend::D3D11;  // default since the M3 integration
    bool vsync = true;
    DebugLayerMode debugLayer = DebugLayerMode::Auto;
    bool softwareAdapter = false;
    std::uint32_t windowScale = 2;
    bool fullscreen = false;
    std::string monitor;  // M3c [render] monitor: GDI device name, empty = primary (platform/window.h)
    rhi::PresentScaling scaling = rhi::PresentScaling::Linear;
    std::filesystem::path screenshotDir;          // default <userdata>\screenshots
    std::vector<std::uint32_t> screenshotFrames;  // sorted, unique
    std::vector<std::uint32_t> traceFrames;       // sorted, unique
    bool validate = false;
    // M3b (display-coupled pacing, platform/frame_pacer.h)
    std::uint32_t tickRate = 60;                                        // [game] tick_rate
    platform::pacing::Mode pacing = platform::pacing::Mode::Auto;       // [render] pacing
    std::filesystem::path pacingTrace;                                  // [render] pacing_trace (empty = off)
    // M3c (render resolution and anti-aliasing, see the header comment)
    std::uint32_t resolutionWidth = 0;                       // [render] resolution: 0 x 0 = auto
    std::uint32_t resolutionHeight = 0;
    float renderScale = kDefaultRenderScale;                 // [render] render_scale
    std::uint32_t msaa = kDefaultMsaa;                       // [render] msaa (requested samples, 1 = off)
    bool alphaToCoverage = kDefaultAlphaToCoverage;          // [render] alpha_to_coverage
    std::uint32_t anisotropy = kDefaultAnisotropy;           // [render] anisotropy (1 = off)
    ScreenshotSize screenshotSize = kDefaultScreenshotSize;  // [render] screenshot_size
    bool linearAa = kDefaultLinearAa;                        // [render] linear_aa (review M3c finding 3)
    // M3d (frame rates up to 120 Hz, platform/frame_pacer.h)
    platform::pacing::FrameRateMode frameRateMode = platform::pacing::FrameRateMode::Fixed;  // [render] frame_rate
    bool lowLatency = true;                                  // [render] low_latency
    // G1 (character outlines)
    OutlineSettings outline;                                 // [render] outline_color/opacity/thickness
};

// Parses the settings from `cfg` (GetSettings does this once for core::GetConfig(); tests). May throw
// std::bad_alloc; logs the warnings described above.
Settings SettingsFromConfig(const core::Config& cfg);

// The pacer's view of `settings` (frame_rate 0 = pacing off when pacing is auto, else the tick rate; tracePath
// points into settings.pacingTrace).
platform::PacingConfig PacingConfigOf(const Settings& settings) noexcept;

// True if the D3D11 debug layer should be requested for `settings` (Auto: Debug builds only).
bool WantDebugLayer(const Settings& settings) noexcept;

// Values from core::GetConfig(), parsed on first use (thread-safe). An unknown d3d_traps value is
// logged once (GFX/warn) and treated as "abort".
const Settings& GetSettings() noexcept;

// Unit tests: use `settings` instead of the configuration (nullptr restores it). Not thread-safe.
void OverrideSettingsForTest(const Settings* settings) noexcept;

}  // namespace gfx
