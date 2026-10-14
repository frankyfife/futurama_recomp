// G8 (part 2, 2026-10-14): player cheats - [cheats] teleport_freecam, moon_jump, jump_distance, infinite_ammo
// (analysis/loader/g8_cheats.md section 9, docs/g8_cheats_report.md). Default: all off, no hook installed.
//
// Physics (retail EU XBE): every character has a physics body at +0x5C (class vtable 0x2A1FB8, ctor
// FUN_000AF040):
//   +0x0C gravity vector (players -95 per s^2 on Y, set every tick by the player update FUN_000530C0; the rat
//   -130 rising / -180 falling), +0x18 damping flag (horizontal velocity x 0.5 per step), +0x28 velocity (Y =
//   up), +0x40 collision object (+0x14 position of the body centre, +0x9C contact flags, vf+4 = reset
//   contacts), +0x45 "on the ground" after the last step. The physics world (vtable 0x2A1ECC) integrates all
//   bodies once per logic tick: vf+0xC = FUN_000AE7E0 calls each body's vf+0x14 = FUN_000AF170 (ground clamp,
//   damping, v += g dt, position += v dt, collision) with the accumulated tick time (world +0x24).
//
// Moon jump (probe:Physics::Step 0xAE7E0): while LB + B (jump) are held, the controlled character's vertical
//   velocity is raised to kMoonJumpSpeed right before the physics step - it rises, gravity is unchanged, so
//   releasing falls normally. Works for every player class including Zoidberg's rat (4-1: body of the rodent
//   player, which the rat control FUN_00061A70 moves along its track; its own jump adds 56 to body +0x2C the
//   same way). The landing after a moon jump does not hurt: the player's fall start height +0xB84 (fall
//   damage: > 40/100 units lethal, FUN_0005A700) follows the character until it touches the ground again.
//
// Jump distance (same probe). Two kinds of jumps (measured, g8_cheats.md 9.3):
//   - animation jumps (Fry, Bender, Leela): the jump request +0xB89 (Player vf+0x104 = 0x50D10, B) starts the
//   jump
//     animation; while it runs the player update sets the body's gravity to 0 and the velocity follows the
//     animation (Fry: 29 steps = 0.48 s, then gravity -95 again for the last bit of the fall). Take-off =
//     gravity 0 after a step on the ground with the request set.
//   - physics jumps (the rat in 4-1, FUN_00061A70: +56 vertical and the request; rodent vf+0x104: +50):
//   take-off =
//     on the ground after the previous step, the request set and a vertical velocity above kTakeoffMinSpeed;
//     the flight time follows from that speed and the gravity (rat: -130 rising, -180 falling).
//   A running jump (horizontal speed above kTakeoffMinRunSpeed) gets an extra horizontal velocity e = extra /
//   T along its horizontal direction for its flight time T (animation jumps: the last measured animation
//   phase of a running jump of this character type, AnimJumpDefaultSeconds before the first one), so it lands
//   `extra` further. The game damps the horizontal velocity by its factor k (0.5 with the damping flag) at
//   the start of each step: the probe adds e / k before the step and takes e off afterwards, so the step
//   moves e * dt further and the game's own velocity stays untouched. Height and flight time stay the game's.
//   Every jump is logged with its distance
//   ("cheat: jump ...").
//
// Teleport (freecam RB / T, executed at the next logic tick): the freecam position, dropped onto the ground
// below it
//   (the game's ray test FUN_000FB6F0 straight down, like the rat's respawn), then the game's own placement:
//   Actor vf+0x1C SetPosition (feet -> body centre, body vf+4 + collision object), collision contacts reset
//   (vf+4), body velocity and the player's velocity copy +0x4E4 zero, fall start height +0xB84 = the new feet
//   height - the parts of the game's respawn FUN_00057910/FUN_00057660 that place the character, without its
//   health refill and weapon reset. Not in vehicles (Chicken Walker 1-6 +0x7B0, cannon 2-5 +0x7A8, rat 4-1
//   +0x7AC: the vehicle code owns the position) and not while grabbed (+0x7A0).
//
// Infinite ammo: Fry's weapons use Player::UseAmmo vf+0x90 = FUN_00050560 (slot, amount: +0xAD0[slot] -=
// amount),
//   probe:Player::UseAmmo skips it - the count and the HUD (player update compares +0xAD0 with its copy
//   +0xAE4) stay. An empty slot of the equipped weapon is refilled to the game's maximum (0x29A1B4:
//   500/200/300/400/0). Per tick (no hook): Chicken Walker (1-6, [player+0x7B0]) laser energy +0xE4 kept at
//   500 (25 per shot, regains 100/s) and rockets +0xC4 not below the highest count seen; cannon (2-5,
//   [player+0x7A8]) shots +0x90 not lowered by firing (-1 = unlimited, the script sets the count with
//   SetAmmo); Bender's and Leela's charge "Power" +0xAF8 (1.0 per special attack, max 4.0) not lowered.
#pragma once

#include "core/config.h"
#include "core/guest.h"

#include <cstdint>

namespace xapi::cheats {

// ---- guest addresses (retail EU XBE; g8_cheats.md 9)
// ------------------------------------------------------------
inline constexpr guest::GuestAddr kPhysicsStep =
    0x000AE7E0;  // physics world vf+0xC, thiscall(world), plain ret
inline constexpr guest::GuestAddr kPlayerUseAmmo =
    0x00050560;  // player vtables +0x90, thiscall(slot, n) ret 8
inline constexpr guest::GuestAddr kRayTest =
    0x000FB6F0;  // cdecl(float* origin, float* dir, float* dist, 1, 3) -> hit
inline constexpr std::uint32_t kPhysicsWorldTime = 0x24;  // float: accumulated tick time of the next step
inline constexpr std::uint32_t kActorSetPosition =
    0x1C;                                             // vtable slot: thiscall(float feet[3]) ret 4 (0x353B0)
inline constexpr std::uint32_t kPlayerBody = 0x5C;    // physics body
inline constexpr std::uint32_t kBodyGravity = 0x0C;   // float[3]
inline constexpr std::uint32_t kBodyDamped = 0x18;    // u8: horizontal velocity x kBodyDampingFactor per step
inline constexpr std::uint32_t kBodyVelocity = 0x28;  // float[3], Y = up
inline constexpr std::uint32_t kBodyCollision = 0x40;      // collision object
inline constexpr std::uint32_t kBodyGrounded = 0x45;       // u8: on the ground after the last step
inline constexpr std::uint32_t kCollisionPosition = 0x14;  // float[3]: body centre
inline constexpr std::uint32_t kCollisionReset = 0x04;  // vtable slot: thiscall() (contact flags +0x9C = 0)
inline constexpr float kBodyDampingFactor = 0.5f;       // 0x293A00
inline constexpr std::uint32_t kPlayerCentreHeight =
    0x4D0;  // float: feet -> body centre (SetPosition 0x353B0)
inline constexpr std::uint32_t kPlayerVelocityCopy = 0x4E4;  // float[3] (FUN_00057660)
inline constexpr std::uint32_t kPlayerInputOn = 0x630;       // u8: controllable
inline constexpr std::uint32_t kPlayerType = 0x774;          // 1 Fry, 2 Leela, 3 Bender, 4 rodent
inline constexpr std::uint32_t kPlayerGrabbedBy = 0x7A0;
inline constexpr std::uint32_t kPlayerCannon = 0x7A8;  // 2-5
inline constexpr std::uint32_t kPlayerRat = 0x7AC;     // 4-1
inline constexpr std::uint32_t kPlayerWalker = 0x7B0;  // 1-6
inline constexpr std::uint32_t kPlayerAmmo = 0xAD0;    // s32[5]
inline constexpr int kAmmoSlots = 5;
inline constexpr guest::GuestAddr kAmmoMaxTable = 0x0029A1B4;  // s32[5]
inline constexpr std::uint32_t kPlayerEquipped = 0x528;        // weapon; +0x74 = ammo slot (-1 = none)
inline constexpr std::uint32_t kWeaponAmmoSlot = 0x74;
inline constexpr std::uint32_t kPlayerPower = 0xAF8;       // float, charge attacks (max 4.0)
inline constexpr std::uint32_t kPlayerFallStartY = 0xB84;  // float: feet height where the fall started
inline constexpr std::uint32_t kPlayerJumpRequest =
    0xB89;                                             // u8: jump requested (vf+0x104), taken by the update
inline constexpr std::uint32_t kWalkerRockets = 0xC4;  // s32, max 99
inline constexpr std::uint32_t kWalkerEnergy = 0xE4;   // float, max 500
inline constexpr float kWalkerEnergyMax = 500.0f;
inline constexpr std::uint32_t kCannonShots = 0x90;     // s32, -1 = unlimited
inline constexpr std::uint32_t kCannonCooldown = 0x8C;  // float: 0.3 s after a shot

// ---- tuning
// ----------------------------------------------------------------------------------------------------- Game
// units per metre (g8_cheats.md 9.2): Fry's collision ellipsoid is 9.6 units high (radii 1.6/4.8/1.6, centre
// 4.8 above the feet) for a man of about 1.75 m; his run (25 units/s = 4.5 m/s), the gravity (95 units/s^2 =
// 17 m/s^2) and the lethal fall (100 units = 18 m) fit that scale.
inline constexpr float kUnitsPerMetre = 5.5f;
inline constexpr float kMoonJumpSpeed = 40.0f;    // units/s upwards while held (the game's jump: 50, rat 56)
inline constexpr float kTakeoffMinSpeed = 20.0f;  // vertical speed of a jump's first step
inline constexpr float kTakeoffMinRunSpeed =
    5.0f;  // horizontal speed: below it a jump is "standing", no boost
inline constexpr float kBoostMaxFlightFactor = 1.5f;  // boost at most this many planned flight times
inline constexpr float kRatGravityUp = -130.0f;       // FUN_000530C0, type 4: vertical velocity > 0
inline constexpr float kRatGravityDown = -180.0f;     // vertical velocity <= 0
// Flight phase of the running jump animation (gravity 0), measured 2026-10-14 at 60 Hz: Fry 0.48 s (29
// steps), Leela 0.58 s, Bender 0.45 s (standing jumps: about 0.52 s). Used until a running jump of the type
// was measured.
inline constexpr float kAnimJumpSeconds = 0.48f;
inline constexpr float kAnimJumpSecondsLeela = 0.58f;
inline constexpr float kAnimJumpSecondsBender = 0.45f;
inline constexpr float kAnimJumpMinSeconds =
    0.2f;  // measured animation phases outside this range are ignored
inline constexpr float kAnimJumpMaxSeconds = 2.0f;
inline constexpr float kJumpTrackMaxSeconds = 5.0f;   // a jump not landed after this is dropped
inline constexpr float kTeleportRayLength = 3000.0f;  // units (300 m) searched below the freecam
inline constexpr float kTeleportGroundLift = 0.5f;    // feet this far above the ground hit
inline constexpr float kJumpDistanceMaxMetres = 2.0f;

// ---- options
// ----------------------------------------------------------------------------------------------------
// [cheats] jump_distance in metres (0 = off), clamped to 0..kJumpDistanceMaxMetres.
float JumpDistanceFromConfig(const core::Config& config) noexcept;
// Hook conditions (FUT_PROBE_IF).
bool PhysicsHooksWanted(
    const core::Config& config) noexcept;                   // moon_jump, jump_distance > 0 or ingame_toggles
bool AmmoHooksWanted(const core::Config& config) noexcept;  // infinite_ammo or ingame_toggles

// ---- pure decisions (unit-tested)
// -------------------------------------------------------------------------------
struct Vec3f {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline constexpr float MetresToUnits(float metres) noexcept {
    return metres * kUnitsPerMetre;
}
// Moon jump: the vertical velocity before the physics step while the buttons are held.
float MoonJumpVelocity(float vy) noexcept;
// Flight time of a jump with vertical take-off speed vy0 (> 0) that lands at its take-off height: up with
// gravity gUp, down with gDown (both negative; the rat falls faster than it rises). 0 for invalid input.
float JumpFlightTime(float vy0, float gUp, float gDown) noexcept;

struct JumpBoost {
    bool active = false;
    float ex = 0.0f, ez = 0.0f;  // extra horizontal velocity (units/s)
    float flightTime = 0.0f;     // planned seconds
    float maxSeconds = 0.0f;     // the boost ends at the latest after this (landing lower than the take-off)
};
// The boost of a jump with horizontal take-off velocity (vx, vz) and flight time `flightTime`; extraUnits >
// 0.
JumpBoost PlanJumpBoostFlight(float extraUnits, float vx, float vz, float flightTime) noexcept;
// The same for a physics jump that takes off with velocity (vx, vy, vz) under gravity gUp/gDown.
JumpBoost PlanJumpBoost(float extraUnits, float vx, float vy, float vz, float gUp, float gDown) noexcept;
// The running jump animation's flight phase of a player type (1 Fry, 2 Leela, 3 Bender; others
// kAnimJumpSeconds).
float AnimJumpDefaultSeconds(std::int32_t type) noexcept;
// The kind of take-off in this step (before it), from the body now and the previous step (JumpKind::None =
// none).
enum class JumpKind : std::uint8_t { None, Animation, Physics };
struct StepSample {
    bool valid = false;        // false: no previous step of this character
    bool grounded = false;     // body +0x45 after the step
    bool jumpRequest = false;  // player +0xB89 after the step
    float gravityY = 0.0f;
};
// jumpRequestNow = player +0xB89 before the step (the rat sets it with its jump in FUN_00061A70).
JumpKind DetectTakeoff(const StepSample& previous, bool groundedNow, bool jumpRequestNow, float gravityNow,
                       float vyNow) noexcept;
// Horizontal velocity component before the step (k = the step's damping factor, 0 < k <= 1) ...
float BoostBeforeStep(float v, float e, float k) noexcept;
// ... and the horizontal velocity after it with the boost taken off again; along the boost never past zero (a
// wall stopped the character), across it unchanged.
void BoostAfterStep(float& vx, float& vz, float ex, float ez) noexcept;

// Teleport: the feet position for a freecam at `camera` whose ray straight down hit the ground `hitDistance`
// below it (hit = false: no ground within the ray). False = no target (nothing below).
bool TeleportFeet(Vec3f camera, bool hit, float hitDistance, Vec3f& feet) noexcept;

// Infinite ammo for counters the game lowers inline (walker rockets, cannon shots, charge power): keeps a
// counter from going down. Reset() on a new owner; increases (pickups, script) are taken over.
class CounterFloor {
public:
    // Returns the value the counter should have now (>= the highest value seen since Reset).
    float Keep(float now) noexcept;
    void Reset() noexcept { valid_ = false; }

private:
    bool valid_ = false;
    float floor_ = 0.0f;
};
// Cannon shots: -1 (unlimited) stays; a count lowered by firing (just fired: cooldown > 0.2) goes back up; a
// count the script lowers (no shot) is taken over.
int KeepCannonShots(int now, int last, float cooldown) noexcept;

// ---- run time
// --------------------------------------------------------------------------------------------------- Input
// side (input/cheat_toggles.cpp): moon jump buttons held on `port` (0..3) this tick.
void SetMoonJumpHeld(int port, bool held) noexcept;
// Freecam (gfx/freecam.cpp): teleport the player to the freecam position at the next tick. False (and a log
// line) if the cheat is off.
bool RequestTeleport(float x, float y, float z) noexcept;
// Logic tick heartbeat (OnTick in cheats.cpp): teleport, vehicle/charge counters.
void PlayerCheatsTick() noexcept;
// Settings/summary helpers for cheats.cpp.
void LogPlayerCheatsSummary() noexcept;

}  // namespace xapi::cheats
