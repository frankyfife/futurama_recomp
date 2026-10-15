// G3 widescreen ([render] aspect, docs/ROADMAP.md "G3", docs/g3_report.md): the stateful part. The pure math and the
// placement rules are in gfx/widescreen_math.h.
//
//   [render] aspect = original | 16:9 | 16:10 | 21:9 | monitor
//     original  (default) nothing changes: 4:3 back buffer, bars by the present pass, no frustum patch, no placement
//     16:9 / 16:10 / 21:9 (64:27, the 2560x1080 panels)
//     monitor   the aspect of [render] resolution when it is set, else of the monitor the window opens on
//
// With an aspect other than original:
//   - the window's view and the back buffer get the display aspect (platform::WindowOptions::aspectW/H,
//     GpuRenderer::CreateParams::aspectWidth/Height, ComputeRenderSizes),
//   - 3D: Hor+ - NiCamera::Click (the probe of gfx/freecam.cpp) widens the frustum (l, r x k) of every back-buffer
//     camera WidenableCamera() accepts, recomputes the culling planes and view data (as the freecam does), and
//     restores the game's values afterwards; with the freecam both combine (freecam pose and zoom, widened frustum),
//   - every back-buffer draw and clear gets a placement (widescreen_math.h Classify): Hor+ 3D stretched over the width,
//     HUD elements anchored at their edge in a level, the rest centred 4:3; the bars outside the centred area are
//     cleared black at the start of each frame that draws.
#pragma once

#include "gfx/widescreen_math.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace core {
struct Config;
}

namespace gfx::widescreen {

enum class AspectMode : std::uint8_t { Original, Wide16x9, Wide16x10, Wide21x9, Monitor };
// "original" | "16:9" | "16:10" | "21:9" | "monitor" (case-insensitive, blanks ignored). The schema (core/
// config_schema.cpp) accepts the same words.
bool ParseAspect(std::string_view text, AspectMode* mode) noexcept;
const char* AspectModeName(AspectMode mode) noexcept;

// The display aspect of `mode` (pure, tests): fixed ratios; Monitor = resolution W x H if set (non-zero), else the
// monitor's pixel size; 0 x 0 for Original (or Monitor without any size).
void AspectOf(AspectMode mode, std::uint32_t resolutionW, std::uint32_t resolutionH, std::uint32_t monitorW,
              std::uint32_t monitorH, std::uint32_t* aspectW, std::uint32_t* aspectH) noexcept;

struct Display {
    AspectMode mode = AspectMode::Original;
    bool active = false;          // aspect differs from the guest's 4:3
    std::uint32_t aspectW = 0;    // display aspect (0 x 0 = original)
    std::uint32_t aspectH = 0;
    float factor = 1.0f;          // Hor+ factor (HorPlusFactor)
};
// [render] aspect resolved once (thread-safe; [render] resolution and [render] monitor of gfx::GetSettings()).
const Display& GetDisplay() noexcept;

// ---- NiCamera::Click side (gfx/freecam.cpp) ----------------------------------------------------------------------
// Hor+ factor for a back-buffer camera (1 = leave it alone: original aspect or not a scene camera).
float FrustumFactor(std::uint32_t camera) noexcept;
// Around the original Click of a widened camera: slot 47 asks WidenedFactor for the camera whose data it gets.
void BeginWidened(std::uint32_t camera, float factor) noexcept;
void EndWidened(std::uint32_t camera) noexcept;
// k while `camera` is inside its widened Click, else 1.
float WidenedFactor(std::uint32_t camera) noexcept;

// ---- renderer side (gfx/nipc_gpu.cpp) ----------------------------------------------------------------------------
// The exceptions table of Classify (widescreen.cpp kAnchorExceptions).
std::span<const AnchorException> AnchorExceptions() noexcept;
// HUD anchoring allowed: the game is in a level (context "Ingame"). Evaluated once per displayed frame.
bool AnchoringAllowed(std::uint32_t frame) noexcept;
// HUD groups (widescreen_math.h HudGroups, 2026-10-14): the rectangle an anchored HUD draw of `frame` is placed by.
// `camera` != 0: the draw is a screen polygon of that NiCamera; its screen polygon list (+0x164 array, +0x16C size,
// drawn by NiCamera::Click 0x100C50) is read once per camera and frame - all pieces of the frame are known before the
// first of them is drawn. `viewport` = the guest viewport the polygons map to (normalised 0..1, y up). `object` = the
// polygon (0 for geometry: the digits of a HUD camera join the group they touch). Draws before the first list of the
// frame use the groups of the previous frame. False: no group, the draw is anchored by `bounds`.
bool HudGroupOf(std::uint32_t camera, const GuestRect& viewport, std::uint32_t object, const GuestRect& bounds,
                std::uint32_t frame, GuestRect* group) noexcept;
// Statistics and the first classifications of screen-space draws (GFX/info, at most a few dozen lines per level
// entered). `camera` = the NiCamera of the draw (0 = unknown), `transform` = the NiTransform* of a geometry draw
// (its NiAVObject and parents are named in the line; 0 = none).
void NoteDraw(const DrawFacts& facts, Placement placement, std::uint32_t object, std::uint32_t frame,
              std::uint32_t camera = 0, std::uint32_t transform = 0) noexcept;
// A full-screen quad of a Hor+ camera drawn with the unwidened projection (it covers the whole width).
void NoteUnwidenedQuad() noexcept;
// "widescreen: ..." exit line.
void LogSummary() noexcept;

}  // namespace gfx::widescreen
