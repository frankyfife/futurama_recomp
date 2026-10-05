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
// The section is [render], not [gfx]: tests/test_core.cpp uses [gfx] as its example of an unknown
// section (a known section would turn its unknown key into an error).
#pragma once

#include <cstdint>

namespace gfx {

enum class TrapMode : std::uint8_t { Abort, Log };

struct Settings {
    std::uint32_t frameLimit = 0;
    std::uint32_t frameRate = 50;
    TrapMode trapMode = TrapMode::Abort;
    bool loadTextures = true;
};

// Values from core::GetConfig(), parsed on first use (thread-safe). An unknown d3d_traps value is
// logged once (GFX/warn) and treated as "abort".
const Settings& GetSettings() noexcept;

// Unit tests: use `settings` instead of the configuration (nullptr restores it). Not thread-safe.
void OverrideSettingsForTest(const Settings* settings) noexcept;

}  // namespace gfx
