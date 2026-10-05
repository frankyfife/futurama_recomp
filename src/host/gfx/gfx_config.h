// Settings of the gfx module (M2: NiPCRenderer null renderer, D3D shim and traps), read once from
// the process configuration (core/config.h, keys registered in kKnownKeys of core/config.cpp):
//
//   [debug] frames = N          (--frames=N, design 5.4) end the process with exit code 0 after N
//                               frames displayed by NiPCRenderer::DisplayFrame (slot 16); 0 = no limit
//   [render] frame_rate = 50    DisplayFrame waits for the next 1/N s like D3DDevice_Swap waits for the
//                               vertical blank on the Xbox (Create gets PresentationInterval ONE, the EU
//                               XBE runs in PAL mode: 50 Hz); 0 = unthrottled
//   [render] d3d_traps = abort  abort | log: a hit D3D trap aborts (core::Fatal) or only logs and
//                               returns 0 with the purge of the original
//   [render] load_textures = 1  slot 65 (CreateSourceTextureRendererData) reads a texture that has only
//                               a file name through the game's NiImageConverter, like FUN_0015E010, to
//                               fill the size fields of the renderer data; 0 = size 0 for such textures
// M3 (GFX_CONTRACT.md 8, keys registered in phase 1; the defaults keep the M2 null renderer until the
// integration switches [render] backend to d3d11):
//   [render] backend = null     null | d3d11: null = M2 null renderer (no window, nothing drawn);
//                               d3d11 = window + D3D11 RHI (gfx/rhi/rhi.h)
//   [render] vsync = 1          Present waits for the vertical blank (sync interval 1)
//   [render] debug_layer = auto auto | on | off: D3D11 debug layer (auto = on in Debug builds); a missing
//                               layer is reported (GFX/warn), the device is created without it
//   [render] adapter = hardware hardware | warp (software rasterizer, tests)
//   [render] window_scale = 2   initial client area = Create size (640x480) * N
//   [render] fullscreen = 0     borderless window on the whole monitor
//   [render] screenshot_dir     PNG screenshots (default <userdata>\screenshots)
//   [render] screenshot_frames  list of displayed-frame numbers that get an automatic screenshot
//   [render] trace_frames       list of displayed-frame numbers whose draw calls are logged one by one
//                               (GFX/info; M3 exit criterion 1: every draw of a scene accounted for)
//   [render] validate = 0       phase-2 harness (GFX_CONTRACT.md 10): with backend = null, slots 47-51,
//                               54, 55 and 65 also run ni_extract / texconv on the real guest data and
//                               count the results (exit line "validate: ..."); nothing is drawn
// The section is [render], not [gfx]: tests/test_core.cpp uses [gfx] as its example of an unknown
// section (a known section would turn its unknown key into an error).
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace gfx {

enum class TrapMode : std::uint8_t { Abort, Log };
enum class RenderBackend : std::uint8_t { Null, D3D11 };          // M3
enum class DebugLayerMode : std::uint8_t { Auto, On, Off };       // M3

struct Settings {
    std::uint32_t frameLimit = 0;
    std::uint32_t frameRate = 50;
    TrapMode trapMode = TrapMode::Abort;
    bool loadTextures = true;
    // M3 (GFX_CONTRACT.md 8)
    RenderBackend backend = RenderBackend::Null;
    bool vsync = true;
    DebugLayerMode debugLayer = DebugLayerMode::Auto;
    bool softwareAdapter = false;
    std::uint32_t windowScale = 2;
    bool fullscreen = false;
    std::filesystem::path screenshotDir;          // default <userdata>\screenshots
    std::vector<std::uint32_t> screenshotFrames;  // sorted, unique
    std::vector<std::uint32_t> traceFrames;       // sorted, unique
    bool validate = false;
};

// True if the D3D11 debug layer should be requested for `settings` (Auto: Debug builds only).
bool WantDebugLayer(const Settings& settings) noexcept;

// Values from core::GetConfig(), parsed on first use (thread-safe). An unknown d3d_traps value is
// logged once (GFX/warn) and treated as "abort".
const Settings& GetSettings() noexcept;

// Unit tests: use `settings` instead of the configuration (nullptr restores it). Not thread-safe.
void OverrideSettingsForTest(const Settings* settings) noexcept;

}  // namespace gfx
