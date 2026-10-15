// G8 part 2: player cheats (see cheats_player.h).
#include "xapi/cheats_player.h"

#include "core/config_schema.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/log.h"
#include "xapi/cheats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace xapi::cheats {
namespace {

constexpr std::uint32_t kDetailLines = 40;  // per kind of event, then every kDetailEvery-th
constexpr std::uint32_t kDetailEvery = 100;
constexpr std::uint32_t kTraceLines = 20000;  // [debug] trace = game: physics lines per run

bool Detail(std::uint32_t n) noexcept {
    return n <= kDetailLines || n % kDetailEvery == 0;
}

std::uint32_t ReadU32(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
}
float ReadF32(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<float>(p) : 0.0f;
}
std::int32_t ReadS32(guest::GuestAddr p) noexcept {
    return static_cast<std::int32_t>(ReadU32(p));
}
std::uint8_t ReadU8(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 1)) ? guest::Read<std::uint8_t>(p) : 0;
}
Vec3f ReadVec(guest::GuestAddr p) noexcept {
    return {ReadF32(p), ReadF32(p + 4), ReadF32(p + 8)};
}

double NowSeconds() noexcept {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

guest::GuestAddr ControlledPlayer() noexcept {
    const guest::GuestAddr session = ReadU32(kSessionPtr);
    return session != 0 ? ReadU32(session + kSessionPlayer) : 0;
}

// The player's physics body, if it looks valid (readable up to the grounded flag, collision object readable).
guest::GuestAddr PlayerBody(guest::GuestAddr player) noexcept {
    const guest::GuestAddr body = player != 0 ? ReadU32(player + kPlayerBody) : 0;
    if (body == 0 || !guest::IsReadable(body, kBodyGrounded + 1)) {
        return 0;
    }
    const guest::GuestAddr coll = ReadU32(body + kBodyCollision);
    return coll != 0 && guest::IsReadable(coll + kCollisionPosition, 12) ? body : 0;
}

// Feet position of the player: body centre - the centre height (FUN_00035440, player classes).
Vec3f PlayerFeet(guest::GuestAddr player, guest::GuestAddr body) noexcept {
    Vec3f p = ReadVec(ReadU32(body + kBodyCollision) + kCollisionPosition);
    p.y -= ReadF32(player + kPlayerCentreHeight);
    return p;
}

bool InVehicle(guest::GuestAddr player) noexcept {
    return ReadU32(player + kPlayerCannon) != 0 || ReadU32(player + kPlayerRat) != 0 ||
           ReadU32(player + kPlayerWalker) != 0;
}

// ---- moon jump / jump distance (physics step probe; guest main thread)
// ------------------------------------------
std::array<std::atomic<bool>, 4> g_moonHeld{};

struct JumpTrack {
    bool active = false;
    JumpKind kind = JumpKind::None;
    Vec3f takeoff;           // feet at the take-off
    float elapsed = 0.0f;    // seconds since the take-off
    bool phaseOver = false;  // animation jump: gravity back (the animation's flight is over)
    bool boosting = false;
    bool running = false;  // horizontal speed above kTakeoffMinRunSpeed at the take-off
    JumpBoost boost;
    float metres = 0.0f;  // [cheats] jump_distance at the take-off
};

struct PhysicsState {
    guest::GuestAddr player = 0;
    bool lifting = false;  // the moon jump buttons were held at the last step
    bool moonAir = false;  // since a lift, until the character is on the ground again
    float moonTopY = -1.0e30f;
    StepSample last;  // the previous step of this character
    JumpTrack jump;
    std::uint32_t jumps = 0;
    std::uint32_t traceLines = 0;
};
PhysicsState g_phys;
// Measured flight phase of a running jump's animation per player type (index kPlayerType 0..4; 0 = not
// measured).
std::array<float, 5> g_animJumpSeconds{};

float AnimJumpSeconds(std::int32_t type) noexcept {
    const bool known = type >= 0 && type < static_cast<std::int32_t>(g_animJumpSeconds.size());
    const float measured = known ? g_animJumpSeconds[static_cast<std::size_t>(type)] : 0.0f;
    return measured > 0.0f ? measured : AnimJumpDefaultSeconds(type);
}

bool MoonHeldAnyPort() noexcept {
    for (const std::atomic<bool>& h : g_moonHeld) {
        if (h.load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

const char* KindName(JumpKind k) noexcept {
    return k == JumpKind::Animation ? "animation" : (k == JumpKind::Physics ? "physics" : "-");
}

struct StepContext {
    guest::GuestAddr player = 0;
    guest::GuestAddr body = 0;
    bool held = false;
    bool boosted = false;
    float dt = 0.0f;
    float gravityY = 0.0f;
    Vec3f before;
};

void StartJump(const StepContext& c, JumpKind kind, Vec3f v) noexcept {
    JumpTrack& j = g_phys.jump;
    j = JumpTrack{};
    j.active = true;
    j.kind = kind;
    j.takeoff = c.before;
    const std::int32_t type = ReadS32(c.player + kPlayerType);
    // The rat rises with -130 and falls with -180 (FUN_000530C0, set per tick from the vertical velocity: at
    // the take-off it is still -180); the others use one gravity.
    const float gUp = type == 4 ? kRatGravityUp : c.gravityY;
    const float gDown = type == 4 ? kRatGravityDown : c.gravityY;
    const float flight =
        kind == JumpKind::Animation ? AnimJumpSeconds(type) : JumpFlightTime(v.y, gUp, gDown);
    j.running = v.x * v.x + v.z * v.z > kTakeoffMinRunSpeed * kTakeoffMinRunSpeed;
    j.metres = ActiveSettings().jumpDistanceMetres;
    if (j.metres > 0.0f) {
        j.boost = PlanJumpBoostFlight(MetresToUnits(j.metres), v.x, v.z, flight);
        j.boosting = j.boost.active;
    }
    const std::uint32_t n = ++g_phys.jumps;
    if (j.boosting) {
        CountPlayerCheat(&Stats::boostedJumps);
    }
    if (Detail(n)) {
        FUT_LOGI(
            Game,
            "cheat: jump %u (%s, type %d) - take-off at %.1f %.1f %.1f, velocity %.1f %.1f %.1f, gravity "
            "%.0f/%.0f; flight %.2f s%s; extra %.1f m = %.1f units -> %.1f %.1f units/s%s",
            n, KindName(kind), type, static_cast<double>(c.before.x), static_cast<double>(c.before.y),
            static_cast<double>(c.before.z), static_cast<double>(v.x), static_cast<double>(v.y),
            static_cast<double>(v.z), static_cast<double>(gUp), static_cast<double>(gDown),
            static_cast<double>(flight), kind == JumpKind::Animation ? " (animation, last measured)" : "",
            static_cast<double>(j.metres), static_cast<double>(MetresToUnits(j.metres)),
            static_cast<double>(j.boost.ex), static_cast<double>(j.boost.ez),
            j.metres > 0.0f && !j.boosting ? " (standing jump: no extra distance)" : "");
    }
}

StepContext BeforeStep(guest::GuestAddr world) noexcept {
    StepContext c;
    const guest::GuestAddr player = ControlledPlayer();
    const guest::GuestAddr body = PlayerBody(player);
    if (body == 0) {
        g_phys.player = 0;
        g_phys.lifting = false;
        g_phys.moonAir = false;
        g_phys.last = StepSample{};
        g_phys.jump = JumpTrack{};
        return c;
    }
    if (player != g_phys.player) {
        g_phys.player = player;
        g_phys.lifting = false;
        g_phys.moonAir = false;
        g_phys.last = StepSample{};
        g_phys.jump = JumpTrack{};
    }
    c.player = player;
    c.body = body;
    c.dt = ReadF32(world + kPhysicsWorldTime);
    c.gravityY = ReadF32(body + kBodyGravity + 4);
    c.before = PlayerFeet(player, body);
    const bool controllable = ReadU8(player + kPlayerInputOn) != 0 && ReadU32(player + kPlayerGrabbedBy) == 0;

    // Moon jump: the rat (4-1) counts (its body is the rodent player's); the Chicken Walker and the cannon
    // own Fry's and Bender's position.
    const bool vehicle = ReadU32(player + kPlayerWalker) != 0 || ReadU32(player + kPlayerCannon) != 0;
    c.held = Active(Cheat::MoonJump) && MoonHeldAnyPort() && controllable && !vehicle;
    if (c.held) {
        const float vy = ReadF32(body + kBodyVelocity + 4);
        guest::Write<float>(body + kBodyVelocity + 4, MoonJumpVelocity(vy));
        CountPlayerCheat(&Stats::moonJumpTicks);
        if (!g_phys.lifting) {
            const std::uint32_t n = CountPlayerCheat(&Stats::moonJumps);
            if (Detail(n)) {
                FUT_LOGI(
                    Game,
                    "cheat: moon jump - lifting (type %d, feet %.1f %.1f %.1f, vertical speed %.1f -> %.1f; "
                    "%u.)",
                    ReadS32(player + kPlayerType), static_cast<double>(c.before.x),
                    static_cast<double>(c.before.y), static_cast<double>(c.before.z), static_cast<double>(vy),
                    static_cast<double>(MoonJumpVelocity(vy)), n);
            }
        }
        g_phys.moonAir = true;
        g_phys.jump.boosting = false;  // a lift is no jump to stretch
    }
    g_phys.lifting = c.held;

    // Jumps: take-off, then the extra distance ([cheats] jump_distance) while the jump flies.
    const Vec3f v = ReadVec(body + kBodyVelocity);
    if (!g_phys.jump.active && !c.held && !g_phys.moonAir && controllable) {
        const JumpKind kind = DetectTakeoff(g_phys.last, ReadU8(body + kBodyGrounded) != 0,
                                            ReadU8(player + kPlayerJumpRequest) != 0, c.gravityY, v.y);
        if (kind != JumpKind::None) {
            StartJump(c, kind, v);
        }
    }
    const JumpTrack& j = g_phys.jump;
    if (j.active && j.boosting && !c.held) {
        const float k = ReadU8(body + kBodyDamped) != 0 ? kBodyDampingFactor : 1.0f;
        guest::Write<float>(body + kBodyVelocity, BoostBeforeStep(v.x, j.boost.ex, k));
        guest::Write<float>(body + kBodyVelocity + 8, BoostBeforeStep(v.z, j.boost.ez, k));
        c.boosted = true;
    }
    return c;
}

void AfterStep(const StepContext& c) noexcept {
    if (c.body == 0) {
        return;
    }
    const bool grounded = ReadU8(c.body + kBodyGrounded) != 0;
    const Vec3f feet = PlayerFeet(c.player, c.body);
    JumpTrack& j = g_phys.jump;
    if (c.boosted) {
        float vx = ReadF32(c.body + kBodyVelocity);
        float vz = ReadF32(c.body + kBodyVelocity + 8);
        BoostAfterStep(vx, vz, j.boost.ex, j.boost.ez);
        guest::Write<float>(c.body + kBodyVelocity, vx);
        guest::Write<float>(c.body + kBodyVelocity + 8, vz);
    }
    if (j.active) {
        j.elapsed += c.dt;
        const std::int32_t type = ReadS32(c.player + kPlayerType);
        if (j.kind == JumpKind::Animation && !j.phaseOver && c.gravityY != 0.0f) {
            // The animation's flight is over (gravity back): its length is the flight time of this type's
            // jumps.
            j.phaseOver = true;
            j.boosting = false;
            const float phase = j.elapsed - c.dt;
            if (j.running && phase >= kAnimJumpMinSeconds && phase <= kAnimJumpMaxSeconds && type >= 0 &&
                type < static_cast<std::int32_t>(g_animJumpSeconds.size())) {
                g_animJumpSeconds[static_cast<std::size_t>(type)] = phase;
            }
        }
        if (j.boosting) {
            const Vec3f v = ReadVec(c.body + kBodyVelocity);
            const bool stopped =
                v.x * v.x + v.z * v.z < kTakeoffMinRunSpeed * kTakeoffMinRunSpeed;  // ledge, wall
            if (j.elapsed > j.boost.maxSeconds || stopped) {
                j.boosting = false;
            }
        }
        const bool landed =
            grounded && j.elapsed > c.dt * 1.5f && (j.kind == JumpKind::Physics || j.phaseOver);
        if (landed || j.elapsed > kJumpTrackMaxSeconds) {
            const float dx = feet.x - j.takeoff.x;
            const float dz = feet.z - j.takeoff.z;
            const double d = std::sqrt(static_cast<double>(dx * dx + dz * dz));
            if (Detail(g_phys.jumps)) {
                FUT_LOGI(Game,
                         "cheat: jump %u (%s, type %d) - %s after %.2f s at %.1f %.1f %.1f: %.1f units = "
                         "%.2f m from "
                         "the take-off (extra %.1f m; height %+.1f; jump animation of this type %.2f s)",
                         g_phys.jumps, KindName(j.kind), type, landed ? "landed" : "not landed",
                         static_cast<double>(j.elapsed), static_cast<double>(feet.x),
                         static_cast<double>(feet.y), static_cast<double>(feet.z), d,
                         d / static_cast<double>(kUnitsPerMetre), static_cast<double>(j.metres),
                         static_cast<double>(feet.y - j.takeoff.y),
                         static_cast<double>(AnimJumpSeconds(type)));
            }
            j = JumpTrack{};
        }
    }
    if (g_phys.moonAir) {
        // Fall damage counts from +0xB84 (FUN_0005A700: > 40/100 units lethal); it follows the character
        // until the ground, so the landing after a lift is a landing from the last step's height.
        guest::Write<float>(c.player + kPlayerFallStartY, feet.y);
        g_phys.moonTopY = std::max(g_phys.moonTopY, feet.y);
        if (!c.held && grounded) {
            g_phys.moonAir = false;
            const std::uint32_t n = CountPlayerCheat(&Stats::fallsProtected);
            if (Detail(n)) {
                FUT_LOGI(Game,
                         "cheat: moon jump - landed at %.1f %.1f %.1f (highest %.1f), no fall damage (%u.)",
                         static_cast<double>(feet.x), static_cast<double>(feet.y),
                         static_cast<double>(feet.z), static_cast<double>(g_phys.moonTopY), n);
            }
            g_phys.moonTopY = -1.0e30f;
        }
    }
    g_phys.last = StepSample{true, grounded, ReadU8(c.player + kPlayerJumpRequest) != 0, c.gravityY};
    // [debug] trace = game: one line per step while the physics hook runs (evidence for moon jump / jump
    // distance).
    if (core::LogEnabled(core::Channel::Game, core::Level::Trace) && g_phys.traceLines < kTraceLines) {
        ++g_phys.traceLines;
        const Vec3f v = ReadVec(c.body + kBodyVelocity);
        const guest::GuestAddr coll = ReadU32(c.body + kBodyCollision);
        FUT_LOGT(
            Game,
            "physics trace: dt %.4f feet %.2f %.2f %.2f v %.2f %.2f %.2f g %.1f ground %u damped %u held %u "
            "boost %u moonAir %u jump %s fallStart %.2f jumpReq %u contacts 0x%X centre %.2f dims %.2f %.2f "
            "%.2f "
            "type %d",
            static_cast<double>(c.dt), static_cast<double>(feet.x), static_cast<double>(feet.y),
            static_cast<double>(feet.z), static_cast<double>(v.x), static_cast<double>(v.y),
            static_cast<double>(v.z), static_cast<double>(c.gravityY), grounded ? 1u : 0u,
            ReadU8(c.body + kBodyDamped), c.held ? 1u : 0u, c.boosted ? 1u : 0u, g_phys.moonAir ? 1u : 0u,
            KindName(j.kind), static_cast<double>(ReadF32(c.player + kPlayerFallStartY)),
            ReadU8(c.player + kPlayerJumpRequest), ReadU32(coll + 0x9C),
            static_cast<double>(ReadF32(c.player + kPlayerCentreHeight)),
            static_cast<double>(ReadF32(coll + 0x90)), static_cast<double>(ReadF32(coll + 0x94)),
            static_cast<double>(ReadF32(coll + 0x98)), ReadS32(c.player + kPlayerType));
    }
}

// Physics world step FUN_000AE7E0 (vtable 0x2A1ECC +0xC), thiscall(world), plain ret: integrates every body
// once with the accumulated tick time.
std::uint32_t __fastcall PhysicsStepProbe(std::uint32_t world, std::uint32_t /*edx*/) noexcept {
    StepContext c;
    try {
        c = BeforeStep(world);
    } catch (...) {
    }
    const std::uint32_t r = core::CallGuestThiscall(core::HookTrampoline(kPhysicsStep), world, {});
    try {
        AfterStep(c);
    } catch (...) {
    }
    return r;
}

// ---- infinite ammo
// ----------------------------------------------------------------------------------------------

// Player::UseAmmo FUN_00050560 (player vtables +0x90), thiscall(player, int slot, int amount) ret 8.
std::uint32_t __fastcall UseAmmoProbe(std::uint32_t player, std::uint32_t /*edx*/, std::uint32_t slot,
                                      std::uint32_t amount) noexcept {
    if (Active(Cheat::InfiniteAmmo) && static_cast<std::int32_t>(amount) > 0) {
        const std::uint32_t n = CountPlayerCheat(&Stats::ammoKept);
        if (Detail(n)) {
            FUT_LOGI(Game, "cheat: infinite ammo - shot with slot %u kept (%d left, %d used; %u.)", slot,
                     slot < static_cast<std::uint32_t>(kAmmoSlots) ? ReadS32(player + kPlayerAmmo + slot * 4)
                                                                   : -1,
                     static_cast<std::int32_t>(amount), n);
        }
        return 0;
    }
    return core::CallGuestThiscall(core::HookTrampoline(kPlayerUseAmmo), player, {slot, amount});
}

// Player::SetAmmo FUN_00051D50, thiscall(player, int slot, int value) ret 8: the value and the HUD.
constexpr guest::GuestAddr kPlayerSetAmmo = 0x00051D50;

struct AmmoState {
    bool wasActive = false;
    guest::GuestAddr walker = 0;
    CounterFloor rockets;
    guest::GuestAddr cannon = 0;
    int cannonLast = -1;
    guest::GuestAddr powerOwner = 0;
    CounterFloor power;
};
AmmoState g_ammo;

void KeptCounter(const char* what, double from, double to) noexcept {
    const std::uint32_t n = CountPlayerCheat(&Stats::countersKept);
    if (Detail(n)) {
        FUT_LOGI(Game, "cheat: infinite ammo - %s %.1f -> %.1f (%u.)", what, from, to, n);
    }
}

void AmmoTick(guest::GuestAddr player) noexcept {
    const bool active = Active(Cheat::InfiniteAmmo);
    if (!active || player == 0) {
        g_ammo = AmmoState{};
        return;
    }
    if (!g_ammo.wasActive) {
        g_ammo = AmmoState{};
        g_ammo.wasActive = true;
    }
    // Fry: refill an empty slot of the equipped weapon (the count no longer goes down, see UseAmmoProbe).
    const guest::GuestAddr weapon = ReadU32(player + kPlayerEquipped);
    const std::int32_t slot = weapon != 0 ? ReadS32(weapon + kWeaponAmmoSlot) : -1;
    if (slot >= 0 && slot < kAmmoSlots) {
        const std::int32_t have = ReadS32(player + kPlayerAmmo + static_cast<std::uint32_t>(slot) * 4);
        const std::int32_t max = ReadS32(kAmmoMaxTable + static_cast<std::uint32_t>(slot) * 4);
        if (have <= 0 && max > 0) {
            core::CallGuestThiscall(kPlayerSetAmmo, player,
                                    {static_cast<std::uint32_t>(slot), static_cast<std::uint32_t>(max)});
            const std::uint32_t n = CountPlayerCheat(&Stats::ammoRefills);
            FUT_LOGI(Game, "cheat: infinite ammo - empty slot %d of the equipped weapon refilled to %d (%u.)",
                     slot, max, n);
        }
    }
    // Chicken Walker (1-6): laser energy full, rockets not lowered.
    const guest::GuestAddr walker = ReadU32(player + kPlayerWalker);
    if (walker != g_ammo.walker) {
        g_ammo.walker = walker;
        g_ammo.rockets.Reset();
    }
    if (walker != 0 && guest::IsReadable(walker + kWalkerEnergy, 4)) {
        const float energy = guest::Read<float>(walker + kWalkerEnergy);
        if (energy < kWalkerEnergyMax) {
            guest::Write<float>(walker + kWalkerEnergy, kWalkerEnergyMax);
            if (energy < kWalkerEnergyMax - 20.0f) {  // a shot (25), not the regeneration of the last tick
                KeptCounter("Chicken Walker laser energy", energy, kWalkerEnergyMax);
            }
        }
        const std::int32_t rockets = guest::Read<std::int32_t>(walker + kWalkerRockets);
        const auto keep = static_cast<std::int32_t>(g_ammo.rockets.Keep(static_cast<float>(rockets)));
        if (keep != rockets) {
            guest::Write<std::int32_t>(walker + kWalkerRockets, keep);
            KeptCounter("Chicken Walker rockets", rockets, keep);
        }
    }
    // Cannon (2-5): shots not lowered by firing.
    const guest::GuestAddr cannon = ReadU32(player + kPlayerCannon);
    if (cannon != g_ammo.cannon) {
        g_ammo.cannon = cannon;
        g_ammo.cannonLast = cannon != 0 ? ReadS32(cannon + kCannonShots) : -1;
    }
    if (cannon != 0 && guest::IsReadable(cannon + kCannonShots, 4)) {
        const std::int32_t shots = guest::Read<std::int32_t>(cannon + kCannonShots);
        const int keep = KeepCannonShots(shots, g_ammo.cannonLast, ReadF32(cannon + kCannonCooldown));
        if (keep != shots) {
            guest::Write<std::int32_t>(cannon + kCannonShots, keep);
            KeptCounter("cannon shots", shots, keep);
        }
        g_ammo.cannonLast = keep;
    }
    // Bender's and Leela's charge attacks: power not lowered.
    if (player != g_ammo.powerOwner) {
        g_ammo.powerOwner = player;
        g_ammo.power.Reset();
    }
    const float power = ReadF32(player + kPlayerPower);
    const float keepPower = g_ammo.power.Keep(power);
    if (keepPower != power) {
        guest::Write<float>(player + kPlayerPower, keepPower);
        KeptCounter("charge power", power, keepPower);
    }
}

// ---- teleport
// ---------------------------------------------------------------------------------------------------
std::mutex g_teleportLock;
bool g_teleportPending = false;
Vec3f g_teleportCamera;

void Refuse(const char* why, Vec3f cam) noexcept {
    const std::uint32_t n = CountPlayerCheat(&Stats::teleportsRefused);
    FUT_LOGI(Game, "cheat: teleport to the freecam %.1f %.1f %.1f refused - %s (%u.)",
             static_cast<double>(cam.x), static_cast<double>(cam.y), static_cast<double>(cam.z), why, n);
    QueueFlash(false);
}

void TeleportTick(guest::GuestAddr player) noexcept {
    Vec3f cam;
    {
        std::lock_guard<std::mutex> lock(g_teleportLock);
        if (!g_teleportPending) {
            return;
        }
        g_teleportPending = false;
        cam = g_teleportCamera;
    }
    const guest::GuestAddr body = PlayerBody(player);
    if (body == 0) {
        Refuse("no player character", cam);
        return;
    }
    if (InVehicle(player)) {
        Refuse("the character is in a vehicle (Chicken Walker, cannon or rat)", cam);
        return;
    }
    if (ReadU32(player + kPlayerGrabbedBy) != 0) {
        Refuse("the character is being held", cam);
        return;
    }
    // The game's ray test straight down from the freecam (as the rat's respawn FUN_00061A70 does).
    alignas(16) float origin[3] = {cam.x, cam.y, cam.z};
    alignas(16) float dir[3] = {0.0f, -1.0f, 0.0f};
    alignas(16) float dist[1] = {kTeleportRayLength};
    const std::uint32_t hit =
        core::CallGuest(kRayTest, {guest::Addr(origin), guest::Addr(dir), guest::Addr(dist), 1u, 3u});
    FUT_LOGD(
        Game,
        "cheat: teleport ray from %.2f %.2f %.2f: result 0x%08X distance %.2f origin after %.2f %.2f %.2f",
        static_cast<double>(cam.x), static_cast<double>(cam.y), static_cast<double>(cam.z), hit,
        static_cast<double>(dist[0]), static_cast<double>(origin[0]), static_cast<double>(origin[1]),
        static_cast<double>(origin[2]));
    Vec3f feet;
    if (!TeleportFeet(cam, hit != 0, dist[0], feet)) {
        Refuse("no ground below the freecam (ray 300 m)", cam);
        return;
    }
    const Vec3f from = PlayerFeet(player, body);
    alignas(16) float target[3] = {feet.x, feet.y, feet.z};
    // Actor vf+0x1C SetPosition (FUN_000353B0: + centre height, body vf+4 -> body +0x34 and the collision
    // object).
    core::CallGuestThiscall(ReadU32(ReadU32(player) + kActorSetPosition), player, {guest::Addr(target)});
    const guest::GuestAddr coll = ReadU32(body + kBodyCollision);
    core::CallGuestThiscall(ReadU32(ReadU32(coll) + kCollisionReset), coll,
                            {});  // contact flags (FUN_00057660)
    for (std::uint32_t i = 0; i < 3; ++i) {
        guest::Write<float>(body + kBodyVelocity + i * 4, 0.0f);
        guest::Write<float>(player + kPlayerVelocityCopy + i * 4, 0.0f);
    }
    guest::Write<float>(player + kPlayerFallStartY, feet.y);
    const Vec3f now = PlayerFeet(player, body);
    const std::uint32_t n = CountPlayerCheat(&Stats::teleports);
    FUT_LOGI(Game,
             "cheat: teleport to the freecam - from %.1f %.1f %.1f to %.1f %.1f %.1f (freecam %.1f %.1f "
             "%.1f, ground "
             "%.1f below; now %.1f %.1f %.1f) (%u.)",
             static_cast<double>(from.x), static_cast<double>(from.y), static_cast<double>(from.z),
             static_cast<double>(feet.x), static_cast<double>(feet.y), static_cast<double>(feet.z),
             static_cast<double>(cam.x), static_cast<double>(cam.y), static_cast<double>(cam.z),
             static_cast<double>(dist[0]), static_cast<double>(now.x), static_cast<double>(now.y),
             static_cast<double>(now.z), n);
    QueueFlash(true);
}

// ---- fall guard (invulnerable)
// ----------------------------------------------------------------------------
FallGuard g_fallGuard;
guest::GuestAddr g_fallPlayer = 0;

void FallGuardTick(guest::GuestAddr player) noexcept {
    const guest::GuestAddr body = PlayerBody(player);
    if (!Active(Cheat::Invulnerable) || body == 0) {
        g_fallGuard.Reset();
        g_fallPlayer = 0;
        return;
    }
    if (player != g_fallPlayer) {
        g_fallGuard.Reset();
        g_fallPlayer = player;
    }
    // The height the player update measures the fall with: Actor vf+0x34 GetPosition (0x5AA45).
    alignas(16) float pos[3] = {};
    core::CallGuestThiscall(ReadU32(ReadU32(player) + kActorGetPosition), player, {guest::Addr(pos)});
    const float fallStart = ReadF32(player + kPlayerFallStartY);
    const bool grounded = ReadU8(body + kBodyGrounded) != 0;
    const auto groundBelow = [&pos]() noexcept {
        alignas(16) float origin[3] = {pos[0], pos[1] + 1.0f, pos[2]};
        alignas(16) float dir[3] = {0.0f, -1.0f, 0.0f};
        alignas(16) float dist[1] = {kTeleportRayLength};
        return core::CallGuest(kRayTest, {guest::Addr(origin), guest::Addr(dir), guest::Addr(dist), 1u, 3u}) != 0;
    };
    const FallGuardDecision d = g_fallGuard.Step(fallStart, pos[1], grounded, NowSeconds(), groundBelow);
    if (d.landed && d.drop > kFallHurtHeight) {
        const std::uint32_t n = CountPlayerCheat(&Stats::fallsCaught);
        if (Detail(n)) {
            FUT_LOGI(Game,
                     "cheat: invulnerable - fall of %.1f units (%.1f m; the game: %s) landed normally at %.1f %.1f "
                     "%.1f (%u.)",
                     static_cast<double>(d.drop), static_cast<double>(d.drop / kUnitsPerMetre),
                     d.drop > kFallLandingDeathHeight ? "fatal" : "25 damage", static_cast<double>(pos[0]),
                     static_cast<double>(pos[1]), static_cast<double>(pos[2]), n);
        }
    }
    if (d.abyss) {
        const std::uint32_t n = CountPlayerCheat(&Stats::abyssFalls);
        FUT_LOGI(Game,
                 "cheat: invulnerable - fall of %.1f units at %.1f %.1f %.1f with no ground below (ray %.0f units) or "
                 "falling for more than %.0f s: the game's fall death runs; respawn at the checkpoint, no life is "
                 "lost (%u.)",
                 static_cast<double>(g_fallGuard.RealStart() - pos[1]), static_cast<double>(pos[0]),
                 static_cast<double>(pos[1]), static_cast<double>(pos[2]), static_cast<double>(kTeleportRayLength),
                 kFallAbyssSeconds, n);
    }
    if (d.write) {
        guest::Write<float>(player + kPlayerFallStartY, d.fallStart);
    }
}

}  // namespace

// ---- options
// ----------------------------------------------------------------------------------------------------

float JumpDistanceFromConfig(const core::Config& config) noexcept {
    try {
        const std::string v =
            config.GetString("cheats", "jump_distance", core::ConfigKeyDefault("cheats", "jump_distance"));
        char* end = nullptr;
        const float f = std::strtof(v.c_str(), &end);
        if (end == v.c_str() || !std::isfinite(f)) {
            return 0.0f;
        }
        return std::clamp(f, 0.0f, kJumpDistanceMaxMetres);
    } catch (...) {
        return 0.0f;
    }
}

bool PhysicsHooksWanted(const core::Config& config) noexcept {
    const Settings s = SettingsFromConfig(config);
    return s.moonJump || s.jumpDistanceMetres > 0.0f || s.ingameToggles;
}

bool AmmoHooksWanted(const core::Config& config) noexcept {
    const Settings s = SettingsFromConfig(config);
    return s.infiniteAmmo || s.ingameToggles;
}

// ---- pure decisions
// ---------------------------------------------------------------------------------------------

float MoonJumpVelocity(float vy) noexcept {
    return vy >= kMoonJumpSpeed ? vy : kMoonJumpSpeed;  // NaN -> the lift speed
}

float JumpFlightTime(float vy0, float gUp, float gDown) noexcept {
    if (!(vy0 > 0.0f) || !(gUp < 0.0f) || !(gDown < 0.0f) || !std::isfinite(vy0)) {
        return 0.0f;
    }
    const float up = vy0 / -gUp;                     // to the top
    const float height = vy0 * vy0 / (-2.0f * gUp);  // above the take-off
    const float down = std::sqrt(2.0f * height / -gDown);
    return up + down;
}

JumpBoost PlanJumpBoostFlight(float extraUnits, float vx, float vz, float flightTime) noexcept {
    JumpBoost b;
    if (!(extraUnits > 0.0f) || !(flightTime > 0.0f) || !std::isfinite(flightTime)) {
        return b;
    }
    const float run = std::sqrt(vx * vx + vz * vz);
    if (!(run > kTakeoffMinRunSpeed) || !std::isfinite(run)) {
        return b;  // a standing jump stays where it is
    }
    const float speed = extraUnits / flightTime;
    b.active = true;
    b.ex = vx / run * speed;
    b.ez = vz / run * speed;
    b.flightTime = flightTime;
    b.maxSeconds = flightTime * kBoostMaxFlightFactor;
    return b;
}

JumpBoost PlanJumpBoost(float extraUnits, float vx, float vy, float vz, float gUp, float gDown) noexcept {
    if (!(vy > kTakeoffMinSpeed)) {
        return {};
    }
    return PlanJumpBoostFlight(extraUnits, vx, vz, JumpFlightTime(vy, gUp, gDown));
}

float AnimJumpDefaultSeconds(std::int32_t type) noexcept {
    switch (type) {
    case 2:
        return kAnimJumpSecondsLeela;
    case 3:
        return kAnimJumpSecondsBender;
    default:
        return kAnimJumpSeconds;  // Fry (1) and the base class
    }
}

JumpKind DetectTakeoff(const StepSample& previous, bool groundedNow, bool jumpRequestNow, float gravityNow,
                       float vyNow) noexcept {
    // Animation jump: the request was set in the previous step on the ground, now the jump animation runs
    // (the player update sets the gravity to 0).
    if (previous.valid && previous.grounded && previous.jumpRequest && previous.gravityY != 0.0f &&
        gravityNow == 0.0f) {
        return JumpKind::Animation;
    }
    // Physics jump: on the ground after the previous step, the request set and an upward velocity under
    // gravity (not a knock-back).
    if (groundedNow && jumpRequestNow && gravityNow < 0.0f && vyNow > kTakeoffMinSpeed) {
        return JumpKind::Physics;
    }
    return JumpKind::None;
}

float BoostBeforeStep(float v, float e, float k) noexcept {
    return k > 0.0f ? v + e / k : v + e;
}

void BoostAfterStep(float& vx, float& vz, float ex, float ez) noexcept {
    const float e2 = ex * ex + ez * ez;
    if (!(e2 > 0.0f)) {
        return;
    }
    // Along the boost direction: take e off, but not below zero; the perpendicular part stays.
    const float along = (vx * ex + vz * ez) / std::sqrt(e2);  // component along the boost (units/s)
    const float e = std::sqrt(e2);
    const float newAlong = along >= e ? along - e : (along > 0.0f ? 0.0f : along);
    const float d = (newAlong - along) / e;
    vx += ex * d;
    vz += ez * d;
}

bool TeleportFeet(Vec3f camera, bool hit, float hitDistance, Vec3f& feet) noexcept {
    if (!hit || !(hitDistance >= 0.0f) || !(hitDistance <= kTeleportRayLength) || !std::isfinite(camera.x) ||
        !std::isfinite(camera.y) || !std::isfinite(camera.z)) {
        return false;
    }
    feet = {camera.x, camera.y - hitDistance + kTeleportGroundLift, camera.z};
    return true;
}

float CounterFloor::Keep(float now) noexcept {
    if (!valid_ || now > floor_ || !std::isfinite(floor_)) {
        valid_ = std::isfinite(now);
        floor_ = now;
        return now;
    }
    return floor_;
}

int KeepCannonShots(int now, int last, float cooldown) noexcept {
    if (now < 0 || last < 0 || now >= last) {
        return now;  // unlimited, unknown or more (script SetAmmo, recharge)
    }
    // Lowered: firing sets the cooldown to 0.3 s (FUN_00032FB0) - put the shot back; otherwise the script
    // lowered it.
    return cooldown > 0.2f ? last : now;
}

// ---- run time
// ---------------------------------------------------------------------------------------------------

void SetMoonJumpHeld(int port, bool held) noexcept {
    if (port >= 0 && port < static_cast<int>(g_moonHeld.size())) {
        g_moonHeld[static_cast<std::size_t>(port)].store(held, std::memory_order_release);
    }
}

bool RequestTeleport(float x, float y, float z) noexcept {
    if (!Active(Cheat::TeleportFreecam)) {
        FUT_LOGD(Game, "cheat: teleport to the freecam - off ([cheats] teleport_freecam)");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_teleportLock);
        g_teleportPending = true;
        g_teleportCamera = {x, y, z};
    }
    FUT_LOGI(Game, "cheat: teleport to the freecam requested (freecam at %.1f %.1f %.1f)",
             static_cast<double>(x), static_cast<double>(y), static_cast<double>(z));
    return true;
}

void PlayerCheatsTick() noexcept {
    try {
        const guest::GuestAddr player = ControlledPlayer();
        TeleportTick(player);
        FallGuardTick(player);  // after a teleport: its new fall start height counts
        AmmoTick(player);
    } catch (...) {
    }
}

void LogPlayerCheatsSummary() noexcept {
    const Stats s = GetStats();
    FUT_LOGI(Game,
             "cheats summary (player): teleport to the freecam %s (%u done, %u refused), moon jump %s (%u "
             "lifts, %u "
             "steps, %u landings without fall damage), jump distance +%.1f m (%u jumps), infinite ammo %s "
             "(%u shots "
             "kept, %u refills, %u counters put back)",
             Active(Cheat::TeleportFreecam) ? "on" : "off", s.teleports, s.teleportsRefused,
             Active(Cheat::MoonJump) ? "on" : "off", s.moonJumps, s.moonJumpTicks, s.fallsProtected,
             static_cast<double>(ActiveSettings().jumpDistanceMetres), s.boostedJumps,
             Active(Cheat::InfiniteAmmo) ? "on" : "off", s.ammoKept, s.ammoRefills, s.countersKept);
}

}  // namespace xapi::cheats

// Installed only when the cheat is on at start or [cheats] ingame_toggles = 1 (core::HookDesc::condition).
// Prologues checked with capstone (g8_cheats.md 9.6): whole position-independent instructions, no branch into
// them.
FUT_PROBE_IF(xapi::cheats::kPhysicsStep, &xapi::cheats::PhysicsStepProbe, Thiscall, "probe:Physics::Step",
             "83 EC 64 53 56", 5, &xapi::cheats::PhysicsHooksWanted);
FUT_PROBE_IF(xapi::cheats::kPlayerUseAmmo, &xapi::cheats::UseAmmoProbe, Thiscall, "probe:Player::UseAmmo",
             "8B 44 24 04 8B 54 24 08", 8, &xapi::cheats::AmmoHooksWanted);
