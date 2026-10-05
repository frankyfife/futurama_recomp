// Phase-2 validation harness of the M3 graphics modules (GFX_CONTRACT.md 10): with [render] validate = 1
// the null renderer (backend = null) also runs ni_extract on every SetCameraData / draw slot and texconv on
// every new source texture, on the real guest data of a game run, and counts the results. Nothing is
// drawn and no guest memory is written. The extract and texconv agents use it to check their code against
// the frontend scene before the D3D11 integration exists:
//   futurama.exe --config=run\<name>\futurama.ini --set=render.validate=1 [--set=render.trace_frames=900]
// Log: first result != Ok/Empty per draw kind and status (GFX/info), every draw of a trace frame
// ("validate frame N: ..."), and at the end (exit reporter "gfx") the summary lines "validate: ...".
// All entry points run on the guest main thread (slot handlers) and are noexcept.
// Skeleton-owned (M3 phase 1); the integration (phase 3) owns it together with nipc_renderer.cpp.
#pragma once

#include "gfx/ni_extract.h"

#include <cstdint>

namespace gfx::validate {

struct TargetInfo {
    std::uint32_t width = 640;
    std::uint32_t height = 480;
    bool leftRightSwap = false;
};

// Slot 47 arguments (guest pointers).
void OnSetCameraData(std::uint32_t location, std::uint32_t direction, std::uint32_t up, std::uint32_t right,
                     std::uint32_t frustum, std::uint32_t port, const TargetInfo& target) noexcept;
// Slots 48-51: data, skin (0 for 49/50), transform, bound + renderer +0x0C/+0x10.
void OnGeometry(ni::DrawKind kind, std::uint32_t data, std::uint32_t skin, std::uint32_t transform,
                std::uint32_t bound, std::uint32_t propertyState, std::uint32_t effectState,
                const TargetInfo& target) noexcept;
// Slots 54/55.
void OnScreenPolygon(std::uint32_t polygon, const TargetInfo& target) noexcept;
void OnScreenTexture(std::uint32_t screenTexture, const TargetInfo& target) noexcept;
// Slot 65: the NiPixelData the source order of FUN_0015E010 selected.
void OnSourceTexture(std::uint32_t pixelData) noexcept;
// Slot 16 on the back buffer: `displayed` = frames displayed so far (after this one).
void OnFrameDisplayed(std::uint32_t displayed) noexcept;
// Summary (GFX/info), called by LogRendererStatistics when [render] validate = 1.
void LogSummary() noexcept;

}  // namespace gfx::validate
