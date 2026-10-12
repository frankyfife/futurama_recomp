// M4b freecam: a freely movable camera for screenshots, inspection and debugging (docs/ROADMAP.md "M4b").
//
// - Toggle: gamepad chord L3+R3 ([freecam] toggle_pad, held back from the game by inp::ChordFilter) or a key
//   ([freecam] toggle_key, default F6). The port the chord came from (the keyboard's port for the key) is diverted
//   to the freecam (inp::InputRouter::Divert): the game sees a connected, neutral pad meanwhile.
// - Camera: probe hook on NiCamera::Click 0x100C50 (fastcall, ecx = camera). For the camera that renders to the back
//   buffer the world pose (+0x70/+0x80/+0x90 rows, +0xA0 translate) and the frustum are replaced for the duration of
//   the call, the derived data of NiCamera::vf39 0xFFDA0 (rows copy +0x110.., culling planes, view data 0xFF3A0) is
//   recomputed, so culling and rendering both use the freecam; afterwards the game's values are restored, the game
//   logic (and the 3D sound listener) keeps its own camera. Works wherever the game clicks a camera: front end,
//   levels, engine cut scenes. Render-to-texture cameras (avatar portrait) stay untouched.
// - Pose updates once per logic tick (inside the diverted port's XInputGetState); M3d interpolates it like the game
//   camera at higher frame rates.
#pragma once

#include "input/input_api.h"

#include <cstdint>

namespace gfx::freecam {

// The router's sample filter (inp::HostRouter::SetSampleFilter): chord/key detection and toggling.
void SampleFilter(int port, inp::PadState& merged) noexcept;

// Reads [freecam] (once, at input start).
void Configure() noexcept;

bool Active() noexcept;
// True while the freecam is active with the HUD hidden (Y / H).
bool HudHidden() noexcept;
// True while the freecam is active with back faces shown ([freecam] back_faces, toggle B): the pipeline then
// drops the normal back-face cull, so level walls seen from outside are drawn instead of the void.
bool ShowBackFaces() noexcept;
// Renderer slot 47 SetCameraData: `location` = camera +0xA0 (NiCamera::Click passes it; other callers too).
void NoteCameraData(std::uint32_t location) noexcept;
// Renderer draw slots (geometry 48-51, screen polygon/texture 54/55): true = skip this draw. With the HUD hidden,
// back-buffer geometry drawn with the camera data of another camera than the scene camera (HUD icons, avatar
// portrait - their cameras set their own data, also in the middle of the scene camera's Click) and every back-buffer
// screen polygon/texture (numbers, text) is overlay. Render-to-texture draws are never skipped.
bool SuppressOverlayDraw(std::uint32_t renderTarget, bool geometry) noexcept;
// Toggles from the host (tests, automation). port = the port to divert.
void SetActive(bool on, int port) noexcept;

}  // namespace gfx::freecam
