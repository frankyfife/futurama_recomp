// G5: [input] camera_response = original|precise - how the third-person camera follows the right stick
// (analysis/loader/g5_camera.md, docs/g5_g7_report.md).
//
// The game turns its follow camera (camera controller [[0x2DABC4]+0x8C], mode +0x15C = 0, update FUN_00015990,
// called once per tick from FUN_00019C80 with dt) through two cascaded filters in fixed 0.01-s sub-steps:
//   input   u = RX (player +0xB38, game curve) * (|RX| < 0.9 ? 0.25 : 1) * rate (+0x178), 0 below 0.01
//   stage 1 v = clamp((u + v) * 0.91, +-2*rate)                         (camera +0x154)
//   stage 2 w = (v * 0.001 + w) * 0.97                                    ([camera +0xE4] + 0xC)
//   angle  += w * 0.01 per sub-step, clamped to +-0.1 rad per tick -> NiMatrix3::MakeYRotation(-angle) 0x103020
//            (call 0x17B2B), then the collision of the new camera position (rays FUN_00011000/FUN_00011070)
// Stage 2 has a time constant of about 0.33 s: the camera needs about a second to reach its full turn rate and
// glides on after the stick is released; below 0.9 deflection it turns at a quarter of the rate.
//
// precise: two probe hooks (FUT_PROBE_IF, installed only with "precise"; "original" installs nothing):
//   probe:Camera::FollowUpdate  0x15990 (thiscall(camera, float dt) ret 4) - computes the turn of this tick from the
//       game's own stick value (+0xB38, sign flipped by the game's invert flag 0x2D9440) with a linear response, a
//       short smoothing (kPreciseSmoothingSeconds) and kPreciseMaxYawDegPerSecond; afterwards it clears the game's
//       turn filter state (+0x154, [+0xE4]+0xC) so nothing glides on.
//   probe:NiMatrix3::MakeYRotation 0x103020 (thiscall(matrix, float angle) ret 4) - only for the call from the
//       follow camera (return address 0x17B30) while a precise turn is active: replaces the angle. The game's
//       collision of the camera position runs afterwards unchanged.
// Only the turn changes; height (RY), distance, auto-centering when the stick is idle, collision and every other
// camera mode stay the game's. Pad right sticks additionally get a smaller dead zone on the host side
// (inp::PreciseStickOutput, input_system.cpp); mouse look already arrives as the exact game deflection.
//
// Camera trace (tests): input-script command `game camera_trace on|off` logs one line per tick with the follow
// camera's yaw (no hook; read at the tick heartbeat).
#pragma once

#include "core/config.h"
#include "core/guest.h"

#include <cstdint>
#include <string_view>

namespace xapi::camera {

// Guest addresses (retail EU XBE; g5_camera.md).
inline constexpr guest::GuestAddr kFollowUpdate = 0x00015990;        // thiscall(camera, float dt) ret 4
inline constexpr guest::GuestAddr kMakeYRotation = 0x00103020;       // thiscall(NiMatrix3*, float angle) ret 4
inline constexpr guest::GuestAddr kFollowRotationReturn = 0x00017B30;  // after `call 0x103020` at 0x17B2B
inline constexpr guest::GuestAddr kSessionPtr = 0x002DABC4;          // level session
inline constexpr std::uint32_t kSessionCameraOffset = 0x8C;          // camera controller
inline constexpr std::uint32_t kSessionPlayerOffset = 0x94;          // player
inline constexpr std::uint32_t kCameraModeOffset = 0x15C;            // 0 = follow camera (FUN_00015990)
inline constexpr std::uint32_t kCameraPositionOffset = 0x48;         // float[3]
inline constexpr std::uint32_t kCameraLookAtOffset = 0x54;           // float[3] (smoothed target)
inline constexpr std::uint32_t kCameraStateOffset = 0xE4;            // pointer to the state block
inline constexpr std::uint32_t kCameraStateTurnSpeedOffset = 0x0C;   // w (stage 2)
inline constexpr std::uint32_t kCameraTurnVelocityOffset = 0x154;    // v (stage 1)
inline constexpr std::uint32_t kCameraTurnRateOffset = 0x178;        // rate (scales u and the clamp of v)
inline constexpr std::uint32_t kCameraHeightRateOffset = 0x180;      // RY -> height offset +0x150
inline constexpr std::uint32_t kPlayerInputEnabledOffset = 0x630;    // u8
inline constexpr std::uint32_t kPlayerCameraStickXOffset = 0xB38;    // RX after the game's curve (action 8)
inline constexpr std::uint32_t kPlayerCameraStickYOffset = 0xB3C;    // RY after the game's curve
inline constexpr guest::GuestAddr kInvertTurnVar = 0x002D9440;       // u8, negates RX in FUN_00015990

enum class Response : std::uint8_t { Original, Precise };

bool ParseResponse(std::string_view text, Response& out) noexcept;  // original|precise, case-insensitive
const char* ResponseName(Response value) noexcept;
// [input] camera_response (warning and "original" for an invalid value).
Response ResponseFromConfig(const core::Config& config) noexcept;

// ---- precise response (pure, unit-tested) ---------------------------------------------------------------------
inline constexpr float kPreciseMaxYawDegPerSecond = 200.0f;  // full deflection
inline constexpr float kPreciseSmoothingSeconds = 0.02f;     // first-order time constant of the turn rate
inline constexpr float kPreciseStopRadPerSecond = 0.002f;    // below this (and no input) the game takes over again

// Turn rate filter of the precise mode: target = deflection * max rate, first-order smoothing per tick.
struct PreciseTurn {
    float rate = 0.0f;  // rad/s, positive = the game's direction for a positive stick value
    // Advances one tick and returns the turn angle of the tick in rad (0 when idle). `deflection` is the game's
    // stick value -1..1 (already sign-corrected), dt the tick length in s.
    float Step(float deflection, float dt) noexcept;
    bool Active() const noexcept { return rate != 0.0f; }
};

// Steady-state turn rate of the original filter chain for a constant stick value `deflection` (game curve
// output 0..1) and the camera's rate parameter (+0x178), in rad/s at `subStepsPerSecond` (60 Hz ticks: 120).
float OriginalSteadyTurnRate(float deflection, float rateParam, float subStepsPerSecond) noexcept;

// ---- camera trace -------------------------------------------------------------------------------------------
struct CameraSample {
    bool valid = false;
    std::uint32_t mode = 0;
    float yawDeg = 0.0f;  // direction look-at -> camera in the ground plane (atan2(dx, dz))
    float position[3] = {};
    float turnVelocity = 0.0f;  // +0x154
    float turnSpeed = 0.0f;     // [+0xE4]+0xC
    float rateParam = 0.0f;     // +0x178
    float heightRateParam = 0.0f;  // +0x180
    float stickX = 0.0f;        // player +0xB38
    bool controllable = false;
};
bool ReadCameraSample(CameraSample& out) noexcept;
void SetTrace(bool on) noexcept;
// Once per logic tick (input-script tick heartbeat): logs "camera trace: ..." while the trace is on.
void TraceTick() noexcept;
// Logs the active mode once (input start).
void LogConfiguration() noexcept;

}  // namespace xapi::camera
