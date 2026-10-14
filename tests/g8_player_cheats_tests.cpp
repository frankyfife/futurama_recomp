// Unit tests of the G8 player cheats (xapi/cheats_player.h, input/cheat_toggles.h, 2026-10-14): [cheats]
// teleport_freecam, moon_jump, jump_distance, infinite_ammo - schema rows and hook conditions, the pure
// decisions (moon jump speed, jump flight time and boost, take-off detection, teleport target, counters), the
// moon jump buttons, the three new D-pad sequences, and the probe sites and data against the retail XBE
// (%FUT_TEST_XBE% or orig\retail_eu\default.xbe; skipped without it).
#include "doctest.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "input/cheat_toggles.h"
#include "xapi/cheats.h"
#include "xapi/cheats_player.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace ch = xapi::cheats;
namespace ct = inp::cheattoggles;
namespace fs = std::filesystem;

namespace {

struct Xbe {
    std::vector<std::uint8_t> bytes;
    struct Section {
        std::uint32_t va, vsize, raw, rsize;
    };
    std::vector<Section> sections;
    std::uint32_t U32At(std::size_t off) const {
        std::uint32_t v = 0;
        std::memcpy(&v, bytes.data() + off, 4);
        return v;
    }
    std::optional<std::size_t> Offset(std::uint32_t va) const {
        for (const Section& s : sections) {
            if (va >= s.va && va < s.va + s.rsize) {
                return s.raw + (va - s.va);
            }
        }
        return std::nullopt;
    }
    std::vector<std::uint8_t> At(std::uint32_t va, std::size_t n) const {
        const auto o = Offset(va);
        if (!o || *o + n > bytes.size()) {
            return {};
        }
        return {bytes.begin() + static_cast<std::ptrdiff_t>(*o),
                bytes.begin() + static_cast<std::ptrdiff_t>(*o + n)};
    }
    std::uint32_t U32(std::uint32_t va) const {
        const auto o = Offset(va);
        return o ? U32At(*o) : 0;
    }
    float F32(std::uint32_t va) const {
        const std::uint32_t u = U32(va);
        float f = 0.0f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    // Target of the E8/E9 rel32 at `va`.
    std::uint32_t CallTarget(std::uint32_t va) const {
        const auto b = At(va, 5);
        if (b.size() != 5 || (b[0] != 0xE8 && b[0] != 0xE9)) {
            return 0;
        }
        std::int32_t rel = 0;
        std::memcpy(&rel, b.data() + 1, 4);
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(va) + 5 + rel);
    }
};

std::optional<Xbe> LoadXbe() {
    wchar_t buf[1024];
    fs::path p = L"D:\\FuturamaDecomp\\orig\\retail_eu\\default.xbe";
    const DWORD n = GetEnvironmentVariableW(L"FUT_TEST_XBE", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        p = buf;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    Xbe x;
    x.bytes.assign(std::istreambuf_iterator<char>(f), {});
    if (x.bytes.size() < 0x200 || std::memcmp(x.bytes.data(), "XBEH", 4) != 0) {
        return std::nullopt;
    }
    const std::uint32_t base = x.U32At(0x104);
    const std::uint32_t count = x.U32At(0x11C);
    const std::uint32_t headers = x.U32At(0x120);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t o = headers - base + i * 0x38;
        x.sections.push_back({x.U32At(o + 4), x.U32At(o + 8), x.U32At(o + 12), x.U32At(o + 16)});
    }
    return x;
}

std::vector<std::uint8_t> Hex(std::initializer_list<int> v) {
    std::vector<std::uint8_t> out;
    for (int b : v) {
        out.push_back(static_cast<std::uint8_t>(b));
    }
    return out;
}

inp::PadState Dpad(std::uint16_t bits) {
    inp::PadState s;
    s.buttons = bits;
    return s;
}

std::uint16_t Bit(ct::Dir d) {
    switch (d) {
    case ct::Dir::Up:
        return inp::kButtonDpadUp;
    case ct::Dir::Down:
        return inp::kButtonDpadDown;
    case ct::Dir::Left:
        return inp::kButtonDpadLeft;
    default:
        return inp::kButtonDpadRight;
    }
}

int PressAll(ct::DpadSequenceDetector& d, const ct::Sequence& seq) {
    int hit = -1;
    for (ct::Dir dir : seq) {
        for (int t = 0; t < 2; ++t) {
            const int r = d.Feed(Dpad(Bit(dir)), true);
            hit = r >= 0 ? r : hit;
        }
        for (int t = 0; t < 3; ++t) {
            const int r = d.Feed(Dpad(0), true);
            hit = r >= 0 ? r : hit;
        }
    }
    return hit;
}

// The game's step for one horizontal component (FUN_000AF170): damping by k, then position += v dt. The
// player update re-adds the animation's movement every tick, so before each step the velocity is m / k (m
// after the damping). Returns the distance of `steps` steps with or without the cheat's boost e.
double SimulateRun(float m, float k, float e, int steps, float dt) {
    double x = 0.0;
    for (int i = 0; i < steps; ++i) {
        float vin = m / k;
        if (e != 0.0f) {
            vin = ch::BoostBeforeStep(vin, e, k);
        }
        x += static_cast<double>(vin * k) * dt;
    }
    return x;
}

}  // namespace

TEST_CASE("G8 player cheats: [cheats] schema rows, settings and hook conditions") {
    for (const char* key : {"teleport_freecam", "moon_jump", "infinite_ammo"}) {
        const core::ConfigKeyDef* def = core::FindConfigKey("cheats", key);
        REQUIRE(def != nullptr);
        CHECK(def->type == core::ConfigKeyType::Bool);
        CHECK(core::ConfigKeyDefault("cheats", key) == "0");
        CHECK(def->tab == core::ConfigUiTab::Cheats);
        CHECK(def->widget == core::ConfigUiWidget::Check);
    }
    const core::ConfigKeyDef* jd = core::FindConfigKey("cheats", "jump_distance");
    REQUIRE(jd != nullptr);
    CHECK(jd->type == core::ConfigKeyType::Float);
    CHECK(jd->widget == core::ConfigUiWidget::DSpin);
    CHECK(jd->ranged);
    CHECK(jd->min == doctest::Approx(0.0));
    CHECK(jd->max == doctest::Approx(2.0));
    CHECK(jd->step == doctest::Approx(0.1));
    REQUIRE(jd->unit != nullptr);
    CHECK(std::string(jd->unit) == "m");
    CHECK(jd->tab == core::ConfigUiTab::Cheats);
    // The labels name the buttons / sequences.
    CHECK(std::string(core::FindConfigKey("cheats", "moon_jump")->label.de).find("LB + B") !=
          std::string::npos);
    CHECK(std::string(core::FindConfigKey("cheats", "teleport_freecam")->label.de).find("RB / T") !=
          std::string::npos);

    core::Config cfg;
    ch::Settings s = ch::SettingsFromConfig(cfg);
    CHECK_FALSE(s.AnyHook());  // default: no hook
    CHECK(s.jumpDistanceMetres == 0.0f);
    CHECK_FALSE(ch::PhysicsHooksWanted(cfg));
    CHECK_FALSE(ch::AmmoHooksWanted(cfg));

    cfg.values["cheats.moon_jump"] = "1";
    CHECK(ch::PhysicsHooksWanted(cfg));
    CHECK_FALSE(ch::AmmoHooksWanted(cfg));
    s = ch::SettingsFromConfig(cfg);
    CHECK(s.Enabled(ch::Cheat::MoonJump));
    CHECK(s.AnyHook());
    cfg.values["cheats.moon_jump"] = "0";

    cfg.values["cheats.jump_distance"] = "1.5";
    CHECK(ch::PhysicsHooksWanted(cfg));
    CHECK(ch::SettingsFromConfig(cfg).jumpDistanceMetres == doctest::Approx(1.5f));
    cfg.values["cheats.jump_distance"] = "3.0";  // clamped
    CHECK(ch::JumpDistanceFromConfig(cfg) == doctest::Approx(ch::kJumpDistanceMaxMetres));
    cfg.values["cheats.jump_distance"] = "-1";
    CHECK(ch::JumpDistanceFromConfig(cfg) == 0.0f);
    cfg.values["cheats.jump_distance"] = "abc";
    CHECK(ch::JumpDistanceFromConfig(cfg) == 0.0f);
    CHECK_FALSE(ch::PhysicsHooksWanted(cfg));
    cfg.values["cheats.jump_distance"] = "0.0";

    cfg.values["cheats.infinite_ammo"] = "1";
    CHECK(ch::AmmoHooksWanted(cfg));
    CHECK_FALSE(ch::PhysicsHooksWanted(cfg));
    cfg.values["cheats.infinite_ammo"] = "0";

    cfg.values["cheats.teleport_freecam"] = "1";  // no hook of its own (runs at the tick heartbeat)
    CHECK_FALSE(ch::PhysicsHooksWanted(cfg));
    CHECK_FALSE(ch::AmmoHooksWanted(cfg));
    s = ch::SettingsFromConfig(cfg);
    CHECK(s.Enabled(ch::Cheat::TeleportFreecam));
    CHECK(s.AnyHook());
    cfg.values["cheats.teleport_freecam"] = "0";

    cfg.values["cheats.ingame_toggles"] = "1";  // toggles: every hook
    CHECK(ch::PhysicsHooksWanted(cfg));
    CHECK(ch::AmmoHooksWanted(cfg));

    CHECK(std::string(ch::CheatKey(ch::Cheat::TeleportFreecam)) == "teleport_freecam");
    CHECK(std::string(ch::CheatKey(ch::Cheat::MoonJump)) == "moon_jump");
    CHECK(std::string(ch::CheatKey(ch::Cheat::InfiniteAmmo)) == "infinite_ammo");
    CHECK(std::string(ch::CheatLabelDe(ch::Cheat::MoonJump)) == "Mondsprung");
    CHECK(ch::kCheatCount == 6);
}

TEST_CASE("G8 player cheats: moon jump speed and the scale") {
    CHECK(ch::MoonJumpVelocity(0.0f) == doctest::Approx(ch::kMoonJumpSpeed));
    CHECK(ch::MoonJumpVelocity(-80.0f) == doctest::Approx(ch::kMoonJumpSpeed));  // falling: lifted again
    CHECK(ch::MoonJumpVelocity(64.0f) == doctest::Approx(64.0f));                // the jump's own push stays
    CHECK(ch::MoonJumpVelocity(std::nanf("")) == doctest::Approx(ch::kMoonJumpSpeed));
    CHECK(ch::MetresToUnits(2.0f) == doctest::Approx(11.0f));
    CHECK(ch::MetresToUnits(0.0f) == 0.0f);
    // Fry's collision ellipsoid (9.6 units high) for about 1.75 m.
    CHECK(9.6f / ch::kUnitsPerMetre == doctest::Approx(1.75f).epsilon(0.01));
}

TEST_CASE("G8 player cheats: jump flight time, boost plan and the boosted step") {
    // Symmetric jump: 2 vy / g.
    CHECK(ch::JumpFlightTime(50.0f, -95.0f, -95.0f) == doctest::Approx(100.0f / 95.0f));
    // The rat: 56 up with -130, down with -180 (measured 0.88 s with a landing 7 units lower).
    const float rat = ch::JumpFlightTime(56.0f, ch::kRatGravityUp, ch::kRatGravityDown);
    CHECK(rat == doctest::Approx(56.0f / 130.0f + std::sqrt(2.0f * (56.0f * 56.0f / 260.0f) / 180.0f)));
    CHECK(rat == doctest::Approx(0.80f).epsilon(0.01));
    CHECK(ch::JumpFlightTime(0.0f, -95.0f, -95.0f) == 0.0f);
    CHECK(ch::JumpFlightTime(50.0f, 0.0f, -95.0f) == 0.0f);
    CHECK(ch::JumpFlightTime(50.0f, -95.0f, 10.0f) == 0.0f);

    // Running jump along +z at 25 units/s, 0.48 s, +11 units (2 m): 22.9 units/s along the run.
    ch::JumpBoost b = ch::PlanJumpBoostFlight(11.0f, 0.0f, 25.0f, 0.48f);
    REQUIRE(b.active);
    CHECK(b.ex == doctest::Approx(0.0f));
    CHECK(b.ez == doctest::Approx(11.0f / 0.48f));
    CHECK(b.maxSeconds == doctest::Approx(0.48f * ch::kBoostMaxFlightFactor));
    b = ch::PlanJumpBoostFlight(11.0f, 6.0f, -8.0f, 0.5f);  // direction kept
    CHECK(b.ex == doctest::Approx(22.0f * 0.6f));
    CHECK(b.ez == doctest::Approx(-22.0f * 0.8f));
    CHECK_FALSE(ch::PlanJumpBoostFlight(11.0f, 1.0f, 2.0f, 0.5f).active);   // standing jump
    CHECK_FALSE(ch::PlanJumpBoostFlight(0.0f, 0.0f, 25.0f, 0.5f).active);   // off
    CHECK_FALSE(ch::PlanJumpBoostFlight(11.0f, 0.0f, 25.0f, 0.0f).active);  // no flight time
    // Physics jump plan = the flight time from vy and gravity.
    b = ch::PlanJumpBoost(11.0f, 23.2f, 56.0f, 55.3f, ch::kRatGravityUp, ch::kRatGravityDown);
    REQUIRE(b.active);
    CHECK(b.flightTime == doctest::Approx(rat));
    CHECK_FALSE(ch::PlanJumpBoost(11.0f, 23.2f, 10.0f, 55.3f, -130.0f, -180.0f).active);  // no take-off speed

    // Before/after the step with the game's damping (k = 0.5) or without (k = 1): the step moves e * dt
    // further and the velocity afterwards is the game's own.
    for (float k : {0.5f, 1.0f}) {
        const float v0 = 30.0f;
        const float e = 20.0f;
        const float before = ch::BoostBeforeStep(v0, e, k);
        float vx = before * k;  // damping in FUN_000AF170
        float vz = 0.0f;
        CHECK(vx - v0 * k == doctest::Approx(e));  // this step's extra speed
        ch::BoostAfterStep(vx, vz, e, 0.0f);
        CHECK(vx == doctest::Approx(v0 * k));
        CHECK(vz == 0.0f);
    }
    // A wall stopped the character: never pushed backwards; the perpendicular part stays.
    {
        float vx = 0.0f;
        float vz = 7.0f;
        ch::BoostAfterStep(vx, vz, 20.0f, 0.0f);
        CHECK(vx == 0.0f);
        CHECK(vz == doctest::Approx(7.0f));
        vx = 5.0f;  // slower than the boost: the boost part only
        ch::BoostAfterStep(vx, vz, 20.0f, 0.0f);
        CHECK(vx == 0.0f);
        vx = -3.0f;  // moving against the boost: unchanged
        ch::BoostAfterStep(vx, vz, 20.0f, 0.0f);
        CHECK(vx == doctest::Approx(-3.0f));
    }
    // Whole jump, damped (Fry): 29 steps at 60 Hz with +11 units planned over 0.48 s.
    {
        const float dt = 1.0f / 60.0f;
        const double plain = SimulateRun(25.0f, 0.5f, 0.0f, 29, dt);
        const double boosted = SimulateRun(25.0f, 0.5f, 11.0f / (29.0f * dt), 29, dt);
        CHECK(plain == doctest::Approx(25.0 * 29 * dt));
        CHECK(boosted - plain == doctest::Approx(11.0).epsilon(0.001));
    }
}

TEST_CASE("G8 player cheats: take-off detection (animation and physics jumps)") {
    using K = ch::JumpKind;
    const ch::StepSample groundRequest{true, true, true, -95.0f};
    // Fry/Bender/Leela: the request in the previous step on the ground, now the jump animation (gravity 0).
    CHECK(ch::DetectTakeoff(groundRequest, true, false, 0.0f, 0.0f) == K::Animation);
    CHECK(ch::DetectTakeoff(groundRequest, false, false, 0.0f, -13.6f) == K::Animation);
    // No request (ledge, cut scene with gravity 0), not on the ground, gravity already 0, no previous step.
    CHECK(ch::DetectTakeoff({true, true, false, -95.0f}, true, false, 0.0f, 0.0f) == K::None);
    CHECK(ch::DetectTakeoff({true, false, true, -95.0f}, false, false, 0.0f, 0.0f) == K::None);
    CHECK(ch::DetectTakeoff({true, true, true, 0.0f}, true, false, 0.0f, 0.0f) == K::None);
    CHECK(ch::DetectTakeoff({}, true, false, 0.0f, 0.0f) == K::None);
    // The rat: on the ground, request set this step, +56 under gravity.
    const ch::StepSample ground{true, true, false, -180.0f};
    CHECK(ch::DetectTakeoff(ground, true, true, -180.0f, 56.0f) == K::Physics);
    CHECK(ch::DetectTakeoff(ground, true, false, -180.0f, 56.0f) == K::None);  // knock-back, no request
    CHECK(ch::DetectTakeoff(ground, false, true, -180.0f, 56.0f) == K::None);  // already in the air
    CHECK(ch::DetectTakeoff(ground, true, true, -180.0f, 10.0f) == K::None);   // too slow
    // Defaults of the animation's flight phase per type.
    CHECK(ch::AnimJumpDefaultSeconds(1) == doctest::Approx(0.48f));
    CHECK(ch::AnimJumpDefaultSeconds(2) == doctest::Approx(0.58f));
    CHECK(ch::AnimJumpDefaultSeconds(3) == doctest::Approx(0.45f));
    CHECK(ch::AnimJumpDefaultSeconds(0) == doctest::Approx(ch::kAnimJumpSeconds));
    CHECK(ch::AnimJumpDefaultSeconds(-1) == doctest::Approx(ch::kAnimJumpSeconds));
}

TEST_CASE("G8 player cheats: teleport target and the ammo counters") {
    ch::Vec3f feet;
    REQUIRE(ch::TeleportFeet({313.3f, 38.7f, 290.1f}, true, 17.0f, feet));
    CHECK(feet.x == doctest::Approx(313.3f));
    CHECK(feet.y == doctest::Approx(38.7f - 17.0f + ch::kTeleportGroundLift));
    CHECK(feet.z == doctest::Approx(290.1f));
    CHECK_FALSE(ch::TeleportFeet({0.0f, 10.0f, 0.0f}, false, ch::kTeleportRayLength, feet));  // nothing below
    CHECK_FALSE(ch::TeleportFeet({0.0f, 10.0f, 0.0f}, true, ch::kTeleportRayLength + 1.0f, feet));
    CHECK_FALSE(ch::TeleportFeet({0.0f, 10.0f, 0.0f}, true, -1.0f, feet));
    CHECK_FALSE(ch::TeleportFeet({std::nanf(""), 10.0f, 0.0f}, true, 5.0f, feet));

    ch::CounterFloor f;
    CHECK(f.Keep(5.0f) == 5.0f);  // first value
    CHECK(f.Keep(4.0f) == 5.0f);  // a shot: back to 5
    CHECK(f.Keep(7.0f) == 7.0f);  // a pickup: taken over
    CHECK(f.Keep(6.0f) == 7.0f);
    f.Reset();
    CHECK(f.Keep(2.0f) == 2.0f);  // new owner
    CHECK(f.Keep(std::nanf("")) == 2.0f);

    CHECK(ch::KeepCannonShots(-1, -1, 0.3f) == -1);  // unlimited
    CHECK(ch::KeepCannonShots(4, 5, 0.3f) == 5);     // just fired: back
    CHECK(ch::KeepCannonShots(0, 5, 0.0f) == 0);     // the script took the shots
    CHECK(ch::KeepCannonShots(8, 5, 0.3f) == 8);     // recharge / SetAmmo
    CHECK(ch::KeepCannonShots(3, -1, 0.3f) == 3);    // unknown before
}

TEST_CASE("G8 player cheats: moon jump buttons and the new D-pad sequences") {
    // B first, then LB: lift; LB hidden from the game (no weapon switch for Fry) until it is released.
    {
        ct::MoonJumpButtons m;
        auto r = m.Feed(true, false);
        CHECK_FALSE(r.lift);
        CHECK_FALSE(r.hideWhite);
        r = m.Feed(true, true);
        CHECK(r.lift);
        CHECK(r.hideWhite);
        r = m.Feed(false, true);  // B released: no lift, LB stays hidden (it was the moon jump's)
        CHECK_FALSE(r.lift);
        CHECK(r.hideWhite);
        r = m.Feed(false, false);
        CHECK_FALSE(r.hideWhite);
        r = m.Feed(false, true);  // a later LB press alone reaches the game
        CHECK_FALSE(r.lift);
        CHECK_FALSE(r.hideWhite);
    }
    // LB first (the game sees it), then B: lift, LB not hidden.
    {
        ct::MoonJumpButtons m;
        CHECK_FALSE(m.Feed(false, true).lift);
        const auto r = m.Feed(true, true);
        CHECK(r.lift);
        CHECK_FALSE(r.hideWhite);
    }
    // The six sequences are distinct; the new ones fire 3..5.
    for (std::size_t a = 0; a < ct::kSequences.size(); ++a) {
        for (std::size_t b = a + 1; b < ct::kSequences.size(); ++b) {
            CHECK(ct::kSequences[a] != ct::kSequences[b]);
        }
    }
    CHECK(std::string(ct::SequenceText(3)) == "RIGHT RIGHT LEFT LEFT UP UP DOWN DOWN");
    CHECK(std::string(ct::SequenceText(4)) == "UP RIGHT DOWN LEFT UP RIGHT DOWN LEFT");
    CHECK(std::string(ct::SequenceText(5)) == "DOWN DOWN UP UP LEFT LEFT RIGHT RIGHT");
    ct::DpadSequenceDetector d;
    d.Configure(60);
    CHECK(PressAll(d, ct::kTeleportFreecamSequence) == static_cast<int>(ch::Cheat::TeleportFreecam));
    CHECK(PressAll(d, ct::kMoonJumpSequence) == static_cast<int>(ch::Cheat::MoonJump));
    CHECK(PressAll(d, ct::kInfiniteAmmoSequence) == static_cast<int>(ch::Cheat::InfiniteAmmo));
    CHECK(PressAll(d, ct::kInvulnerableSequence) == static_cast<int>(ch::Cheat::Invulnerable));
}

TEST_CASE("G8 player cheats: probe sites and data agree with the retail XBE") {
    const auto xbe = LoadXbe();
    if (!xbe) {
        MESSAGE("skipped: retail XBE not found (FUT_TEST_XBE)");
        return;
    }
    // Probes: whole, position-independent instructions (g8_cheats.md 9.6).
    CHECK(xbe->At(ch::kPhysicsStep, 5) ==
          Hex({0x83, 0xEC, 0x64, 0x53, 0x56}));  // sub esp,0x64; push ebx; push esi
    CHECK(xbe->At(ch::kPlayerUseAmmo, 15) ==
          Hex({0x8B, 0x44, 0x24, 0x04, 0x8B, 0x54, 0x24, 0x08,                // slot, amount
               0x29, 0x94, 0x81, 0xD0, 0x0A, 0x00, 0x00}));                   // sub [+0xAD0]
    CHECK(xbe->At(ch::kPlayerUseAmmo + 0x1C, 3) == Hex({0xC2, 0x08, 0x00}));  // ret 8
    // Physics world vtable 0x2A1ECC: +0x8 accumulates dt into +0x24, +0xC is the step.
    CHECK(xbe->U32(0x002A1ECC + 0xC) == ch::kPhysicsStep);
    CHECK(xbe->U32(0x002A1ECC + 0x8) == 0x000AE7D0u);
    CHECK(xbe->At(0x000AE7D0, 9) == Hex({0xD9, 0x05, 0x48, 0x67, 0x2D, 0x00, 0xD8, 0x41, 0x24}));
    // Body vtable 0x2A1FB8: vf+4 set position, +0x14 integrate, +0x48 grounded (+0x45), +0x50 add velocity
    // (+0x28).
    CHECK(xbe->U32(0x002A1FB8 + 0x04) == 0x000AEC00u);
    CHECK(xbe->U32(0x002A1FB8 + 0x14) == 0x000AF170u);
    CHECK(xbe->U32(0x002A1FB8 + 0x48) == 0x000AF160u);
    CHECK(xbe->U32(0x002A1FB8 + 0x50) == 0x000AF740u);
    CHECK(xbe->At(0x000AF160, 4) == Hex({0x8A, 0x41, static_cast<int>(ch::kBodyGrounded), 0xC3}));
    CHECK(xbe->At(0x000AF740, 7) ==
          Hex({0x8B, 0x44, 0x24, 0x04, 0xD9, 0x41, static_cast<int>(ch::kBodyVelocity)}));
    CHECK(xbe->At(0x000AF234, 3) ==
          Hex({0x38, 0x5E, static_cast<int>(ch::kBodyDamped)}));  // cmp [esi+0x18], bl
    CHECK(xbe->At(0x000AF239, 9) == Hex({0xD9, 0x46, 0x28, 0xD8, 0x0D, 0x00, 0x3A, 0x29, 0x00}));
    CHECK(xbe->F32(0x00293A00) == doctest::Approx(ch::kBodyDampingFactor));
    // Gravity: players -95 (0x2D656C), the rat -130 rising / -180 falling (FUN_000530C0).
    CHECK(xbe->F32(0x002D656C) == doctest::Approx(-95.0f));
    CHECK(xbe->At(0x000532D7, 8) == Hex({0xC7, 0x44, 0x24, 0x14, 0x00, 0x00, 0x02, 0xC3}));  // -130
    CHECK(xbe->At(0x000532E9, 8) == Hex({0xC7, 0x44, 0x24, 0x14, 0x00, 0x00, 0x34, 0xC3}));  // -180
    // Player vtables: +0x1C SetPosition 0x353B0 (+ centre height +0x4D0), +0x90 UseAmmo, +0x104 jump (+0xB89
    // = 1).
    for (std::uint32_t vt : ch::kPlayerVtables) {
        CHECK(xbe->U32(vt + ch::kActorSetPosition) == 0x000353B0u);
        CHECK(xbe->U32(vt + 0x90) == ch::kPlayerUseAmmo);
        CHECK(xbe->U32(vt + 0x104) == 0x00050D10u);
    }
    CHECK(xbe->At(0x000353E3, 6) == Hex({0xD8, 0x81, 0xD0, 0x04, 0x00, 0x00}));        // fadd [ecx+0x4D0]
    CHECK(xbe->At(0x00050E2D, 7) == Hex({0xC6, 0x86, 0x89, 0x0B, 0x00, 0x00, 0x01}));  // jump request
    // The game's respawn placement FUN_00057910: SetPosition (vf+0x1C), fall start +0xB84 = the new height.
    CHECK(xbe->At(0x0005791B, 3) == Hex({0xFF, 0x50, 0x1C}));
    CHECK(xbe->At(0x00057935, 6) == Hex({0x89, 0x8E, 0x84, 0x0B, 0x00, 0x00}));
    // Collision object reset (vf+4 = 0xA9100: contact flags +0x9C = 0).
    CHECK(xbe->U32(0x002A1E68 + ch::kCollisionReset) == 0x000A9100u);
    CHECK(xbe->At(0x000A9108, 6) == Hex({0xC7, 0x86, 0x9C, 0x00, 0x00, 0x00}));
    // Ray test FUN_000FB6F0, called by the rat's respawn (0x620A3).
    CHECK(xbe->CallTarget(0x000620A3) == ch::kRayTest);
    // Ammo: SetAmmo FUN_00051D50 writes +0xAD0[slot]; maxima 500/200/300/400/0.
    CHECK(xbe->At(0x00051D50, 15) ==
          Hex({0x8B, 0x44, 0x24, 0x08, 0x8B, 0x54, 0x24, 0x04, 0x89, 0x84, 0x91, 0xD0, 0x0A, 0x00, 0x00}));
    const std::int32_t maxima[] = {500, 200, 300, 400, 0};
    for (int i = 0; i < ch::kAmmoSlots; ++i) {
        CHECK(static_cast<std::int32_t>(xbe->U32(ch::kAmmoMaxTable + static_cast<std::uint32_t>(i) * 4)) ==
              maxima[i]);
    }
    // Chicken Walker: rockets +0xC4 (FUN_00042700 add, max 99), laser energy +0xE4 capped at 500.
    CHECK(xbe->At(0x00042700, 10) == Hex({0x8B, 0x44, 0x24, 0x04, 0x8B, 0x91, 0xC4, 0x00, 0x00, 0x00}));
    CHECK(xbe->At(0x000457B1, 10) == Hex({0xC7, 0x83, 0xE4, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFA, 0x43}));
    // Cannon: SetAmmo native FUN_00032E00 writes the shots +0x90.
    CHECK(xbe->At(0x00032E07, 6) == Hex({0x89, 0x81, 0x90, 0x00, 0x00, 0x00}));
}
