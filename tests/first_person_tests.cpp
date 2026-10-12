// Unit tests of G6 (xapi/first_person.h): [input] first_person parsing and schema rows, the view math (the game's
// Rz(pitch) * Ry(yaw) rows and the Euler extraction of FUN_00103720), the right-stick look curve, the move intent
// relative to the view, and the probe sites against the retail XBE (%FUT_TEST_XBE% or orig\retail_eu\default.xbe;
// skipped without it).
#include "doctest.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "input/kbm_source.h"
#include "input/stick_curve.h"
#include "xapi/first_person.h"

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

namespace fp = xapi::fp;
namespace fs = std::filesystem;

namespace {

constexpr float kPi = 3.14159265358979f;

// The game's matrix product FUN_00013B30 (thiscall this = A, out, B): out = B x A (rows of 4 floats).
void GameMul(const float a[12], const float b[12], float out[12]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float v = 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += b[i * 4 + k] * a[k * 4 + j];
            }
            out[i * 4 + j] = v;
        }
        out[i * 4 + 3] = 0.0f;
    }
}
// FUN_00103020 (Y rotation) and FUN_00103060 (Z rotation) as decompiled.
void GameRotY(float angle, float m[12]) {
    const float c = std::cos(angle), s = std::sin(angle);
    const float r[12] = {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0};
    std::memcpy(m, r, sizeof(r));
}
void GameRotZ(float angle, float m[12]) {
    const float c = std::cos(angle), s = std::sin(angle);
    const float r[12] = {c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0};
    std::memcpy(m, r, sizeof(r));
}

struct XbeFile {
    std::vector<std::uint8_t> bytes;
    std::uint32_t base = 0;
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
        return {bytes.begin() + static_cast<std::ptrdiff_t>(*o), bytes.begin() + static_cast<std::ptrdiff_t>(*o + n)};
    }
    std::uint32_t U32(std::uint32_t va) const {
        const auto o = Offset(va);
        return o ? U32At(*o) : 0;
    }
};

std::optional<XbeFile> LoadRetailXbe() {
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
    XbeFile x;
    x.bytes.assign(std::istreambuf_iterator<char>(f), {});
    if (x.bytes.size() < 0x200 || std::memcmp(x.bytes.data(), "XBEH", 4) != 0) {
        return std::nullopt;
    }
    x.base = x.U32At(0x104);
    const std::uint32_t count = x.U32At(0x11C);
    const std::uint32_t headers = x.U32At(0x120);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t o = headers - x.base + i * 0x38;
        x.sections.push_back({x.U32At(o + 4), x.U32At(o + 8), x.U32At(o + 12), x.U32At(o + 16)});
    }
    return x;
}

}  // namespace

TEST_CASE("G6: [input] first_person parsing and schema rows") {
    fp::Mode m = fp::Mode::On;
    CHECK(fp::ParseMode("off", m));
    CHECK(m == fp::Mode::Off);
    CHECK(fp::ParseMode(" ON ", m));
    CHECK(m == fp::Mode::On);
    CHECK(fp::ParseMode("1", m));
    CHECK(m == fp::Mode::On);
    CHECK(fp::ParseMode("", m));
    CHECK(m == fp::Mode::Off);
    CHECK_FALSE(fp::ParseMode("maybe", m));
    CHECK(std::string(fp::ModeName(fp::Mode::On)) == "on");

    CHECK(core::ConfigKeyDefault("input", "first_person") == "off");
    CHECK(std::string(core::ConfigKeyChoices("input", "first_person")) == "off|on");
    CHECK(core::ConfigKeyDefault("input", "first_person_look_speed") == "180");

    core::Config cfg;
    CHECK(fp::ModeFromConfig(cfg) == fp::Mode::Off);
    CHECK_FALSE(fp::HooksWanted(cfg));  // default: no hook is installed
    cfg.values["input.first_person"] = "on";
    CHECK(fp::HooksWanted(cfg));
    cfg.values["input.first_person"] = "nonsense";
    CHECK_FALSE(fp::HooksWanted(cfg));
}

TEST_CASE("G6: view rotation equals the game's Rz(pitch) x Ry(yaw) and round-trips") {
    for (float yawDeg : {-170.0f, -90.0f, -30.0f, 0.0f, 45.0f, 120.0f, 179.0f}) {
        for (float pitchDeg : {-80.0f, -20.0f, 0.0f, 10.0f, 60.0f, 85.0f}) {
            CAPTURE(yawDeg);
            CAPTURE(pitchDeg);
            const fp::View v{yawDeg * kPi / 180.0f, pitchDeg * kPi / 180.0f};
            float mine[12];
            fp::ViewRotation(v, mine);
            float ry[12], rz[12], game[12];
            GameRotY(v.yaw, ry);
            GameRotZ(v.pitch, rz);
            GameMul(ry, rz, game);  // FUN_00018250: this = Ry, param_3 = Rz -> Rz x Ry
            for (int i = 0; i < 12; ++i) {
                CHECK(mine[i] == doctest::Approx(game[i]).epsilon(1e-5));
            }
            // Orthonormal rows, right = forward x up? The game's rows: right = (-sin y, 0, cos y).
            for (int r = 0; r < 3; ++r) {
                const float* a = mine + r * 4;
                CHECK(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] == doctest::Approx(1.0f));
            }
            CHECK(mine[0] * mine[8] + mine[1] * mine[9] + mine[2] * mine[10] == doctest::Approx(0.0f).epsilon(1e-6));
            const fp::View back = fp::ViewFromForward(mine[0], mine[1], mine[2]);
            CHECK(back.yaw == doctest::Approx(v.yaw).epsilon(1e-4));
            CHECK(back.pitch == doctest::Approx(v.pitch).epsilon(1e-4));
        }
    }
    // Positive pitch looks down, positive yaw turns towards the right row.
    float m[12];
    fp::ViewRotation({0.0f, 0.3f}, m);
    CHECK(m[1] < 0.0f);
    float a[12], b[12];
    fp::ViewRotation({0.0f, 0.0f}, a);
    fp::ViewRotation({0.1f, 0.0f}, b);
    CHECK(b[0] * a[8] + b[2] * a[10] > 0.0f);
    CHECK(fp::ClampPitch(2.0f) == doctest::Approx(fp::kPitchLimit));
    CHECK(fp::ClampPitch(-2.0f) == doctest::Approx(-fp::kPitchLimit));
    CHECK(fp::WrapYaw(3.5f * kPi) == doctest::Approx(-0.5f * kPi));
    CHECK(fp::WrapYaw(-1.0f) == doctest::Approx(-1.0f));
    CHECK(fp::WrapYaw(NAN) == 0.0f);
}

TEST_CASE("G6: right-stick look curve") {
    float x = 1.0f, y = 1.0f;
    fp::LookDeflection(0.1f, 0.05f, false, x, y);  // inside the dead zone
    CHECK(x == 0.0f);
    CHECK(y == 0.0f);
    fp::LookDeflection(1.0f, 0.0f, false, x, y);
    CHECK(x == doctest::Approx(1.0f));
    CHECK(y == 0.0f);
    fp::LookDeflection(0.0f, -1.0f, false, x, y);
    CHECK(y == doctest::Approx(-1.0f));
    // Monotonic and finer than linear in the middle.
    float prev = 0.0f;
    for (int i = 16; i <= 100; ++i) {
        fp::LookDeflection(static_cast<float>(i) / 100.0f, 0.0f, false, x, y);
        CHECK(x >= prev);
        prev = x;
    }
    fp::LookDeflection(0.55f, 0.0f, false, x, y);
    CHECK(x > 0.0f);
    CHECK(x < (0.55f - fp::kStickDeadzone) / (fp::kStickFull - fp::kStickDeadzone));
    // Diagonal keeps the direction.
    fp::LookDeflection(0.7f, 0.7f, false, x, y);
    CHECK(x == doctest::Approx(y));
    // G5 precise: the pad sends the game-curve inverse of a linear response; the game curve decodes it.
    float rx = 0.0f, ry = 0.0f;
    inp::GameStickInverse(0.5f, 0.0f, rx, ry);
    fp::LookDeflection(rx, ry, true, x, y);
    CHECK(x == doctest::Approx(std::pow(0.5f, fp::kStickExponent)).epsilon(1e-3));
    CHECK(y == 0.0f);
    fp::LookDeflection(NAN, 0.0f, false, x, y);
    CHECK(x == 0.0f);
}

TEST_CASE("G6: move intent follows the view") {
    float view[12];
    fp::ViewRotation({0.7f, 0.4f}, view);  // looking down a little: the move stays on the ground
    const float* forward = view;
    const float* right = view + 8;
    float out[3];
    REQUIRE(fp::MoveIntent(0.0f, 1.0f, forward, right, out));
    const float fl = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    CHECK(out[0] == doctest::Approx(forward[0] / fl));
    CHECK(out[1] == 0.0f);
    CHECK(out[2] == doctest::Approx(forward[2] / fl));
    REQUIRE(fp::MoveIntent(1.0f, 0.0f, forward, right, out));  // strafe right
    CHECK(out[0] == doctest::Approx(right[0]));
    CHECK(out[2] == doctest::Approx(right[2]));
    REQUIRE(fp::MoveIntent(-0.5f, -0.5f, forward, right, out));  // back-left, magnitude kept
    CHECK(std::sqrt(out[0] * out[0] + out[2] * out[2]) == doctest::Approx(std::sqrt(0.5f)));
    CHECK(out[0] * forward[0] + out[2] * forward[2] < 0.0f);
    CHECK(out[0] * right[0] + out[2] * right[2] < 0.0f);
    CHECK_FALSE(fp::MoveIntent(0.0f, 0.0f, forward, right, out));
    CHECK(out[0] == 0.0f);
    const float up[3] = {0.0f, 1.0f, 0.0f};
    CHECK_FALSE(fp::MoveIntent(0.0f, 1.0f, up, up, out));  // degenerate view
}

TEST_CASE("G6: probe sites and call sites agree with the retail XBE") {
    const auto xbe = LoadRetailXbe();
    if (!xbe) {
        MESSAGE("skipped: retail XBE not found (FUT_TEST_XBE)");
        return;
    }
    const std::vector<std::uint8_t> frame = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    CHECK(xbe->At(fp::kSetFirstPerson, 6) == frame);
    CHECK(xbe->At(fp::kPlayerMove, 6) == frame);
    CHECK(xbe->At(fp::kMoveDirection, 6) == frame);
    CHECK(xbe->At(fp::kCameraFirstPersonUpdate, 6) == frame);
    CHECK(xbe->At(fp::kPlayerJump, 6) == std::vector<std::uint8_t>{0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1});
    CHECK(xbe->At(fp::kPlayerViewToggle, 6) == std::vector<std::uint8_t>{0x83, 0xEC, 0x24, 0x56, 0x8B, 0xF1});
    // The three calls of FUN_00054DA0 whose return addresses the probe tells apart.
    auto callTarget = [&](std::uint32_t site) {
        const auto b = xbe->At(site, 5);
        REQUIRE(b.size() == 5);
        REQUIRE(b[0] == 0xE8);
        std::int32_t rel = 0;
        std::memcpy(&rel, b.data() + 1, 4);
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(site + 5) + rel);
    };
    CHECK(callTarget(fp::kAutoExitReturn - 5) == fp::kSetFirstPerson);
    CHECK(callTarget(fp::kToggleOnReturn - 5) == fp::kSetFirstPerson);
    CHECK(callTarget(fp::kToggleOffReturn - 5) == fp::kSetFirstPerson);
    CHECK(callTarget(0x54C00) == fp::kMoveDirection);            // only caller: FUN_00054AE0
    CHECK(callTarget(0x19CDB) == fp::kCameraFirstPersonUpdate);  // camera dispatcher FUN_00019C80, mode 3
    // Player vtables (base, Bender, Fry, Leela, Rodent): +0x100 move, +0x104 jump, +0x11C view toggle.
    for (std::uint32_t vt : {0x29A1E8u, 0x29D8E0u, 0x29E1E8u, 0x29F0C0u, 0x2A0DB0u}) {
        CAPTURE(vt);
        CHECK(xbe->U32(vt + 0x100) == fp::kPlayerMove);
        CHECK(xbe->U32(vt + 0x104) == fp::kPlayerJump);
        CHECK(xbe->U32(vt + 0x11C) == fp::kPlayerViewToggle);
    }
}

namespace {
bool g_mouseTaken = false;
bool MouseTakenForTest() noexcept {
    return g_mouseTaken;
}
}  // namespace

TEST_CASE("G6: keyboard/mouse - mouse motion goes to the first-person view instead of the right stick") {
    inp::KeyboardMouseSource kbm;
    kbm.Configure(nullptr, 1.0f, false);
    kbm.SetMouseMotionQuery(&MouseTakenForTest);
    inp::PadState s;
    g_mouseTaken = false;
    kbm.MouseMoveEvent(50, 0);
    REQUIRE(kbm.Sample(0, 0, s));
    CHECK(s.thumb[inp::kAxisRX] > 0);  // mapped to the stick as before
    g_mouseTaken = true;
    kbm.MouseMoveEvent(50, -20);
    kbm.KeyEvent('W', true);
    REQUIRE(kbm.Sample(0, 1, s));
    CHECK(s.thumb[inp::kAxisRX] == 0);  // dropped from the pad emulation ...
    CHECK(s.thumb[inp::kAxisRY] == 0);
    CHECK(s.thumb[inp::kAxisLY] == 32767);  // ... keys still work
    const inp::KeyboardMouseSource::RawSnapshot raw = kbm.ReadRaw(true);
    CHECK(raw.dx == 100);  // ... and the raw view sees all of it
    CHECK(raw.dy == -20);
    g_mouseTaken = false;
    REQUIRE(kbm.Sample(0, 2, s));
    CHECK(s.thumb[inp::kAxisRX] == 0);  // the dropped motion does not come back later
    kbm.SetMouseMotionQuery(nullptr);
    CHECK(fp::WallPush(0.1f) == doctest::Approx(fp::kWallClearance - 0.1f));
    CHECK(fp::WallPush(fp::kWallClearance + 0.1f) == 0.0f);
    CHECK(fp::WallPush(-1.0f) == 0.0f);
}
