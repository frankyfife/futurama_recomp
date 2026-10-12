// G6: [input] first_person = off|on - playable first-person control (analysis/loader/g6_firstperson.md,
// docs/g6_report.md). Experimental.
//
// The game already has a first-person view: R3 (player vtable +0x11C, 0x588B0) calls FUN_00054DA0(player, 1), which
// switches the camera controller [[0x2DABC4]+0x8C] to mode 3 (FUN_00018050: camera at the eye, player model hidden
// with the APP_CULLED bit of [player+0x564]) and sets player +0xB88. Mode 3 is updated by FUN_00018250 (RS turns the
// view, smoothed, pitch +-1.5 rad); FUN_00057940 turns the body to the view yaw every tick and Fry's weapons fire along
// the view (FUN_00084E60, ray to 1000 units). But the view is a stand-still aim mode: the movement vfunc +0x100
// (FUN_00054AE0) and the jump vfunc +0x104 (0x50D10) do nothing while +0xB88 is set, the left stick also turns the
// view, and FUN_00057940 leaves the view as soon as the player is in the air or attacks.
//
// "on" extends that mode with six conditional probe hooks (FUT_PROBE_IF; "off" installs nothing):
//   probe:Player::SetFirstPerson   0x54DA0  thiscall(player, char on) ret 4: remembers the wish of the player (R3 on/off,
//       return addresses 0x58946/0x58956) and skips the automatic exit of FUN_00057940 (return 0x587F8: in the air,
//       attack animation, hit, targeting flags) unless the player is dead or +0x7A0 is set. Other exits (respawn, being
//       grabbed, scripted cameras) run; the view comes back by itself afterwards.
//   probe:Player::vfViewToggle     0x588B0  thiscall(player, float, float) ret 8 (vtable-only, extra function start):
//       before the original, enters the first-person view by itself while it is wanted and the game's own R3
//       conditions hold (same checks as 0x588B0), the follow camera (mode 0) is active, R3 is not pressed, the level
//       is not one of the vehicle levels (xapi::select::IsPlayAsBlockedLevel) and a short pause after a game exit
//       (kReenterTicks) has passed.
//   probe:Player::Move             0x54AE0  thiscall(player, float* velocity, float* rotation) ret 8: runs the
//       original with +0xB88 cleared, so the player walks in the view.
//   probe:Player::MoveDirection    0x536D0  thiscall(player, float* out, float turn, float* rotation) ret 0xC: only
//       inside that call: the move intent from the left stick relative to the view (forward = row 0, right = row 2 of
//       the camera rotation +0x70, flattened) is returned directly - strafing without turning the body first.
//   probe:Player::vfJump           0x50D10  thiscall(player) ret 0 (vtable-only, extra function start): original with
//       +0xB88 cleared (jumping in the view).
//   probe:Camera::FirstPersonUpdate 0x18250 thiscall(camera, float dt) ret 4: replaces the mode-3 update: position as
//       the original (player position + eye height [[player+0x10]+0xA0]), view from our own yaw/pitch: right stick
//       (raw values of the controller object [player+0x7B8] +0x124/+0x12C, own dead zone and curve, LookSpeed) and the
//       mouse (raw counts of the keyboard/mouse source, not mapped to the stick meanwhile). Matrix like the game's:
//       Rz(pitch) * Ry(yaw), rows forward/up/right. The game's option "Ziel umkehren" (0x2E790A) inverts as before.
// The game's own R3 view thus becomes the playable view: R3 (keyboard V, middle mouse button) switches between first
// and third person. Cut scenes and scripted cameras use their own camera modes and are not touched.
#pragma once

#include "core/config.h"
#include "core/guest.h"

#include <cmath>
#include <cstdint>
#include <string_view>

namespace xapi::fp {

// ---- guest addresses (retail EU XBE; g6_firstperson.md) -------------------------------------------------------
inline constexpr guest::GuestAddr kSetFirstPerson = 0x00054DA0;        // thiscall(player, char on) ret 4
inline constexpr guest::GuestAddr kPlayerMove = 0x00054AE0;            // vtable +0x100, thiscall ret 8
inline constexpr guest::GuestAddr kMoveDirection = 0x000536D0;         // thiscall(player, out, turn, rot) ret 0xC
inline constexpr guest::GuestAddr kPlayerJump = 0x00050D10;            // vtable +0x104, thiscall ret 0
inline constexpr guest::GuestAddr kCameraFirstPersonUpdate = 0x00018250;  // camera mode 3, thiscall(cam, dt) ret 4
inline constexpr guest::GuestAddr kPlayerViewToggle = 0x000588B0;      // vtable +0x11C, thiscall ret 8
inline constexpr guest::GuestAddr kAutoExitReturn = 0x000587F8;        // FUN_00057940: call 0x54DA0 at 0x587F3
inline constexpr guest::GuestAddr kToggleOnReturn = 0x00058946;        // 0x588B0: R3 enters (call at 0x58941)
inline constexpr guest::GuestAddr kToggleOffReturn = 0x00058956;       // 0x588B0: R3 leaves (call at 0x58951)
inline constexpr guest::GuestAddr kSessionPtr = 0x002DABC4;            // level session (+0xC level name)
inline constexpr std::uint32_t kSessionCamera = 0x8C;
inline constexpr std::uint32_t kSessionPlayer = 0x94;
inline constexpr guest::GuestAddr kInvertAimVar = 0x002E790A;          // u8 option "Ziel umkehren" (FUN_0005E020)
inline constexpr std::uint32_t kSessionHud = 0x118;                   // HUD list, FUN_000E59D0(show) on it
inline constexpr guest::GuestAddr kHudShow = 0x000E59D0;               // thiscall(hud list, int show) ret 4
inline constexpr guest::GuestAddr kVisorSet = 0x00045ED0;              // thiscall(visor, char on) ret 4

// Player (class 0x29A1E8 and its characters).
inline constexpr std::uint32_t kPlayerDef = 0x10;              // def; +0xA0 eye height
inline constexpr std::uint32_t kDefEyeHeight = 0xA0;
inline constexpr std::uint32_t kPlayerMoveX = 0x4D8;           // left stick X after the game curve (+ D-pad)
inline constexpr std::uint32_t kPlayerMoveY = 0x4DC;           // left stick Y
inline constexpr std::uint32_t kPlayerRotation = 0x570;        // NiMatrix3 (3 rows of 4 floats), vtable +0x38
inline constexpr std::uint32_t kPlayerAimPoint = 0x5D8;        // float[3], Fry's shot target (FUN_00084E60, trace)
inline constexpr std::uint32_t kPlayerInputEnabled = 0x630;    // u8
inline constexpr std::uint32_t kPlayerRiding = 0x7A0;          // != 0 blocks the view (0x588B0)
inline constexpr std::uint32_t kPlayerVisor = 0x7B4;           // Bender's visor overlay object (FUN_00045ED0)
inline constexpr std::uint32_t kPlayerCharacterType = 0x774;   // 1 = Fry (FUN_00054DA0 keeps his HUD)
inline constexpr std::uint32_t kCharacterTypeFry = 1;
inline constexpr std::uint32_t kPlayerController = 0x7B8;      // active controller object
inline constexpr std::uint32_t kPlayerMoveIntent = 0xB1C;      // float[3] written by FUN_000536D0
inline constexpr std::uint32_t kPlayerFirstPerson = 0xB88;     // u8
inline constexpr std::uint32_t kPlayerStateB8E = 0xB8E;        // u8, blocks the view (0x588B0)
inline constexpr std::uint32_t kPlayerPadBits = 0xBA0;         // u16, bit 0x20 = view toggle held (FUN_00057940)
inline constexpr std::uint16_t kPadBitViewToggle = 0x20;
inline constexpr std::uint32_t kVfGetPosition = 0x34;          // thiscall(player, float* out) -> out
inline constexpr std::uint32_t kVfOnGround = 0x70;
inline constexpr std::uint32_t kVfIsDead = 0xA8;
inline constexpr std::uint32_t kVfBusyA = 0x12C;               // animation states (0x50010)
inline constexpr std::uint32_t kVfStateA = 0x138;              // +0xB8C/+0xB91/+0xB92
inline constexpr std::uint32_t kVfStateB = 0x13C;              // +0xB8D/+0xB8F/+0xB90
inline constexpr std::uint32_t kVfBusyB = 0x140;               // animation states (0x501A0)

// Camera controller.
inline constexpr std::uint32_t kCamTarget = 0x44;
inline constexpr std::uint32_t kCamPosition = 0x48;            // float[3], dirty byte +0xA1
inline constexpr std::uint32_t kCamPositionDirty = 0xA1;
inline constexpr std::uint32_t kCamRotation = 0x70;            // 3 rows of 4 floats, dirty byte +0xA3
inline constexpr std::uint32_t kCamRotationDirty = 0xA3;
inline constexpr std::uint32_t kCamNiCamera = 0x30;            // NiCamera (local rotation +0x30, translate +0x60)
inline constexpr std::uint32_t kCamFrustumDirty = 0xA0;        // u8, FUN_00012210 then sets the frustum (near 0.25)
inline constexpr std::uint32_t kCamTanHalfH = 0xA4;            // tan(fov / 2), FUN_000110E0
inline constexpr std::uint32_t kCamTanHalfV = 0xA8;            // 0.75 * tan(fov / 2)
inline constexpr std::uint32_t kCamFovDeg = 0xAC;             // horizontal field of view in degrees (90, 45 zoomed)
inline constexpr std::uint32_t kCamFar = 0xB4;
inline constexpr std::uint32_t kCamMode = 0x15C;               // 0 follow, 3 first person
inline constexpr std::uint32_t kCamModeFollow = 0;
inline constexpr std::uint32_t kCamModeFirstPerson = 3;

inline constexpr guest::GuestAddr kNiCameraSetFrustum = 0x00011960;  // thiscall(NiCamera, float frustum[6]) ret 4
// Camera collision ray (cdecl: origin xyz, direction xyz, float* distance in/out) -> hit; collision mask 0x60
// (FUN_000F9CF0) around FUN_000FB6F0, as the follow camera uses it.
inline constexpr guest::GuestAddr kCameraRay = 0x00011000;

// Controller object (0x19C bytes, m4_input.md 3.1): stick s at +0x104 + 0x18*s {thrX, thrY, X, prevX, Y, prevY}.
inline constexpr std::uint32_t kPadRightStickX = 0x124;        // raw / 32768
inline constexpr std::uint32_t kPadRightStickY = 0x12C;        // +Y = up

// ---- option ---------------------------------------------------------------------------------------------------
enum class Mode : std::uint8_t { Off, On };
bool ParseMode(std::string_view text, Mode& out) noexcept;  // off|on (also 0/1, false/true), case-insensitive
const char* ModeName(Mode value) noexcept;
Mode ModeFromConfig(const core::Config& config) noexcept;  // warning and "off" for an invalid value
// FUT_PROBE_IF condition: [input] first_person = on.
bool HooksWanted(const core::Config& config) noexcept;

// ---- tuning --------------------------------------------------------------------------------------------------
inline constexpr float kDefaultLookSpeedDeg = 180.0f;     // [input] first_person_look_speed, full deflection
inline constexpr float kMouseDegPerCount = 0.08f;         // x [input] mouse_sensitivity
inline constexpr float kStickDeadzone = 0.15f;            // radial, raw stick
inline constexpr float kStickFull = 0.95f;                // full deflection from here
inline constexpr float kStickExponent = 1.6f;             // fine aiming near the centre
inline constexpr float kPitchLimit = 1.5f;                // rad, as the game's mode 3 (FUN_00018250)
inline constexpr std::uint32_t kReenterTicks = 15;        // pause after the game left the view by itself
inline constexpr float kNearPlane = 0.1f;                 // engine minimum (FUN_00011960), the game uses 0.25
inline constexpr float kWallProbe = 1.0f;                 // ray length around the eye (ground plane)
inline constexpr float kWallClearance = 0.4f;             // the camera keeps this distance from walls

// ---- pure math (unit-tested) ---------------------------------------------------------------------------------
// The game's view angles: rotation rows Rz(pitch) * Ry(yaw) (FUN_00103060 x FUN_00103020 combined by FUN_00013B30),
// so row 0 (forward) = (cos p cos y, -sin p, cos p sin y), row 1 (up) = (sin p cos y, cos p, sin p sin y),
// row 2 (right) = (-sin y, 0, cos y). Positive yaw turns right, positive pitch looks down. Extraction as FUN_00103720:
// yaw = atan2(r0.z, r0.x), pitch = -asin(r0.y).
struct View {
    float yaw = 0.0f;
    float pitch = 0.0f;
};
View ViewFromForward(float fx, float fy, float fz) noexcept;
// 12 floats (3 rows of 4, 4th column 0) for the camera controller +0x70.
void ViewRotation(const View& view, float out[12]) noexcept;
float ClampPitch(float pitch) noexcept;
float WrapYaw(float yaw) noexcept;  // -pi..pi

// Right-stick deflection for looking: raw stick (x, y) -> (ox, oy) in -1..1. `gameEncoded`: the values were shaped for
// the game's curve by [input] camera_response = precise (G5: raw = game-curve inverse of a linear response) - then
// the game curve decodes them; otherwise a radial dead zone kStickDeadzone..kStickFull. Both then get the response
// curve |o|^kStickExponent (direction kept).
void LookDeflection(float x, float y, bool gameEncoded, float& ox, float& oy) noexcept;

// Move intent relative to the view: left stick (x right, y forward; the game's curve output) and the view rows
// forward (row 0) and right (row 2) flattened onto the ground. Returns false (out = 0) without input or a
// degenerate view. out = unit direction * magnitude (magnitude = |x*right + y*forward|, not clamped - the caller
// FUN_00054AE0 normalizes and caps it at 1 like the original intent).
bool MoveIntent(float x, float y, const float forward[3], const float right[3], float out[3]) noexcept;

// Distance the camera moves away from a wall hit at `hit` (ray length kWallProbe): kWallClearance - hit, 0 beyond.
inline float WallPush(float hit) noexcept {
    return (hit >= 0.0f && hit < kWallClearance) ? kWallClearance - hit : 0.0f;
}

// ---- runtime -------------------------------------------------------------------------------------------------
// True while the first-person view of the extension is active (camera mode 3 updated by the hook within the last
// 150 ms); the keyboard/mouse source then leaves the mouse motion to the view (KeyboardMouseSource query).
bool MouseLookActive() noexcept;

}  // namespace xapi::fp
