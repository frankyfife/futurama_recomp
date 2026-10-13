// Unit tests of G5 (xapi/camera_response.h: [input] camera_response, the precise turn filter, the original filter
// model, the right-stick curve, the guest code the probes rely on - retail XBE %FUT_TEST_XBE% or
// orig\retail_eu\default.xbe, skipped without it) and G7 (input/level_skip.h: [cheats] level_skip, the sequence
// detector, the successor table; the router's filter chain).
#include "doctest.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "input/host_router.h"
#include "input/input_script.h"
#include "input/level_skip.h"
#include "input/stick_curve.h"
#include "xapi/camera_response.h"
#include "xapi/game_select.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cam = xapi::camera;
namespace skip = inp::levelskip;

namespace {

constexpr float kRadToDeg = 180.0f / 3.14159265f;

std::optional<std::vector<std::uint8_t>> LoadRetailXbe() {
    wchar_t buf[1024];
    std::filesystem::path p = L"D:\\FuturamaDecomp\\orig\\retail_eu\\default.xbe";
    const DWORD n = GetEnvironmentVariableW(L"FUT_TEST_XBE", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        p = buf;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(f), {});
    if (bytes.size() < 0x200 || std::memcmp(bytes.data(), "XBEH", 4) != 0) {
        return std::nullopt;
    }
    return bytes;
}

// VA -> bytes through the section headers (tools/xbe.py).
std::vector<std::uint8_t> At(const std::vector<std::uint8_t>& x, std::uint32_t va, std::size_t n) {
    auto u32 = [&](std::size_t o) {
        std::uint32_t v = 0;
        std::memcpy(&v, x.data() + o, 4);
        return v;
    };
    const std::uint32_t base = u32(0x104);
    const std::uint32_t count = u32(0x11C);
    const std::uint32_t headers = u32(0x120);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t o = headers - base + i * 0x38;
        const std::uint32_t sva = u32(o + 4), raw = u32(o + 12), rsize = u32(o + 16);
        if (va >= sva && va + n <= sva + rsize) {
            const std::size_t off = raw + (va - sva);
            return std::vector<std::uint8_t>(x.begin() + static_cast<std::ptrdiff_t>(off),
                                             x.begin() + static_cast<std::ptrdiff_t>(off + n));
        }
    }
    return {};
}

float F32(const std::vector<std::uint8_t>& x, std::uint32_t va) {
    const auto b = At(x, va, 4);
    float f = 0.0f;
    if (b.size() == 4) {
        std::memcpy(&f, b.data(), 4);
    }
    return f;
}

inp::PadState Buttons(std::uint8_t a, std::uint8_t b, std::uint16_t digital = 0, std::uint8_t x = 0) {
    inp::PadState s;
    s.analog[inp::kAnalogA] = a;
    s.analog[inp::kAnalogB] = b;
    s.analog[inp::kAnalogX] = x;
    s.buttons = digital;
    return s;
}

// Feeds a press of A or B: `held` ticks pressed, `gap` ticks released. Returns true if a tick completed the sequence.
bool Press(skip::SequenceDetector& d, char key, int held = 3, int gap = 4, bool armed = true) {
    bool done = false;
    for (int i = 0; i < held; ++i) {
        done = d.Feed(Buttons(key == 'A' ? 255 : 0, key == 'B' ? 255 : 0), armed) || done;
    }
    for (int i = 0; i < gap; ++i) {
        done = d.Feed(Buttons(0, 0), armed) || done;
    }
    return done;
}

bool Sequence(skip::SequenceDetector& d, const char* keys, int held = 3, int gap = 4) {
    bool done = false;
    for (const char* k = keys; *k != 0; ++k) {
        if (*k != ' ') {
            done = Press(d, *k, held, gap) || done;
        }
    }
    return done;
}

}  // namespace

// ---- G5 ---------------------------------------------------------------------------------------------------------

TEST_CASE("G5: [input] camera_response parsing and schema row") {
    cam::Response r = cam::Response::Original;
    CHECK(cam::ParseResponse("precise", r));
    CHECK(r == cam::Response::Precise);
    CHECK(cam::ParseResponse("ORIGINAL", r));
    CHECK(r == cam::Response::Original);
    CHECK_FALSE(cam::ParseResponse("fast", r));
    CHECK_FALSE(cam::ParseResponse("", r));
    CHECK(std::string(cam::ResponseName(cam::Response::Precise)) == "precise");
    CHECK(std::string(cam::ResponseName(cam::Response::Original)) == "original");

    CHECK(core::ConfigKeyDefault("input", "camera_response") == "original");
    CHECK(std::string(core::ConfigKeyChoices("input", "camera_response")) == "original|precise");
    const core::ConfigKeyDef* key = core::FindConfigKey("input", "camera_response");
    REQUIRE(key != nullptr);
    CHECK(key->tab == core::ConfigUiTab::Controls);

    core::Config cfg;
    CHECK(cam::ResponseFromConfig(cfg) == cam::Response::Original);
    cfg.values["input.camera_response"] = "Precise";
    CHECK(cam::ResponseFromConfig(cfg) == cam::Response::Precise);
    cfg.values["input.camera_response"] = "nonsense";
    CHECK(cam::ResponseFromConfig(cfg) == cam::Response::Original);
}

TEST_CASE("G5: precise turn - proportional, reaches the rate within a few ticks, stops on release") {
    const float dt = 1.0f / 60.0f;
    const float full = cam::kPreciseMaxYawDegPerSecond;
    cam::PreciseTurn t;
    CHECK_FALSE(t.Active());
    CHECK(t.Step(0.0f, dt) == 0.0f);
    CHECK_FALSE(t.Active());
    // step to full deflection: > 50 % after one tick, > 90 % after three
    t.Step(1.0f, dt);
    CHECK(t.rate * kRadToDeg > 0.5f * full);
    t.Step(1.0f, dt);
    t.Step(1.0f, dt);
    CHECK(t.rate * kRadToDeg > 0.9f * full);
    for (int i = 0; i < 30; ++i) {
        t.Step(1.0f, dt);
    }
    CHECK(t.rate * kRadToDeg == doctest::Approx(full).epsilon(0.001));
    const float angle = t.Step(1.0f, dt);
    CHECK(angle * kRadToDeg == doctest::Approx(full * dt).epsilon(0.001));
    // release: below 1 % after 6 ticks, exactly idle (the game takes over) soon after
    for (int i = 0; i < 6; ++i) {
        t.Step(0.0f, dt);
    }
    CHECK(std::fabs(t.rate * kRadToDeg) < 0.01f * full);
    for (int i = 0; i < 30; ++i) {
        t.Step(0.0f, dt);
    }
    CHECK_FALSE(t.Active());
    // proportional and signed
    for (int i = 0; i < 40; ++i) {
        t.Step(-0.5f, dt);
    }
    CHECK(t.rate * kRadToDeg == doctest::Approx(-0.5f * full).epsilon(0.001));
    // clamped, robust against garbage
    for (int i = 0; i < 40; ++i) {
        t.Step(3.0f, dt);
    }
    CHECK(t.rate * kRadToDeg == doctest::Approx(full).epsilon(0.001));
    CHECK(t.Step(NAN, dt) <= full * dt / kRadToDeg);
    CHECK(t.Step(1.0f, 0.0f) == 0.0f);
}

TEST_CASE("G5: model of the original filter chain matches the measured rates") {
    // Level 1-1: rate parameter +0x178 = 35, 2 sub-steps per tick at 60 Hz (g5_camera.md 2.2/4).
    const float fullDeg = cam::OriginalSteadyTurnRate(1.0f, 35.0f, 120.0f) * kRadToDeg;
    CHECK(fullDeg == doctest::Approx(155.6f).epsilon(0.01));
    // the stick value 0.433 (RX = 0.6 through the game's curve): measured 82.5-84.7 deg/s
    const float partDeg = cam::OriginalSteadyTurnRate(0.433f, 35.0f, 120.0f) * kRadToDeg;
    CHECK(partDeg == doctest::Approx(85.2f).epsilon(0.02));
    // the quarter rate below 0.9 saturates at the clamp of stage 1 from about 0.79 on
    CHECK(cam::OriginalSteadyTurnRate(0.8f, 35.0f, 120.0f) == doctest::Approx(cam::OriginalSteadyTurnRate(1.0f, 35.0f,
                                                                                                         120.0f)));
    CHECK(cam::OriginalSteadyTurnRate(0.001f, 35.0f, 120.0f) == 0.0f);  // u below 0.01
    // precise at full deflection is faster than the original's steady rate, and about equal at the middle
    CHECK(cam::kPreciseMaxYawDegPerSecond > fullDeg);
    CHECK(0.5f * cam::kPreciseMaxYawDegPerSecond ==
          doctest::Approx(cam::OriginalSteadyTurnRate(0.5f, 35.0f, 120.0f) * kRadToDeg).epsilon(0.03));
}

TEST_CASE("G5: precise right stick - small radial dead zone, linear, encoded for the game's curve") {
    float ox = 0.0f, oy = 0.0f;
    inp::PreciseStickOutput(0.1f, 0.1f, ox, oy);
    CHECK(ox == 0.0f);
    CHECK(oy == 0.0f);
    inp::PreciseStickOutput(1.0f, 0.0f, ox, oy);
    CHECK(ox == doctest::Approx(1.0f));
    CHECK(oy == 0.0f);
    inp::PreciseStickOutput(-0.565f, 0.0f, ox, oy);
    CHECK(ox == doctest::Approx(-0.5f).epsilon(0.01));
    inp::PreciseStickOutput(0.6f, 0.8f, ox, oy);  // magnitude 1, direction kept
    CHECK(ox == doctest::Approx(0.6f));
    CHECK(oy == doctest::Approx(0.8f));

    // through the thumbs: the game's own curve reproduces the precise deflection (the game's dead zone 0.4 is gone)
    for (const float x : {0.25f, 0.4f, 0.565f, 0.8f, -0.3f, -0.9f}) {
        for (const float y : {0.0f, 0.1f, -0.5f}) {
            CAPTURE(x);
            CAPTURE(y);
            std::int16_t tx = inp::ThumbFromUnit(x), ty = inp::ThumbFromUnit(y);
            float wantX = 0.0f, wantY = 0.0f;
            inp::PreciseStickOutput(inp::UnitFromThumb(tx), inp::UnitFromThumb(ty), wantX, wantY);
            inp::PreciseStickToGameThumbs(tx, ty);
            const float gx = inp::GameStickOutput(inp::UnitFromThumb(tx), inp::UnitFromThumb(ty));
            const float gy = inp::GameStickOutput(inp::UnitFromThumb(ty), inp::UnitFromThumb(tx));
            CHECK(gx == doctest::Approx(wantX).epsilon(0.002));
            CHECK(gy == doctest::Approx(wantY).epsilon(0.002));
        }
    }
    std::int16_t tx = 3000, ty = -2000;  // inside the dead zone: neutral
    inp::PreciseStickToGameThumbs(tx, ty);
    CHECK(tx == 0);
    CHECK(ty == 0);
}

TEST_CASE("G5: the guest code of the probes in the retail XBE") {
    const auto xbe = LoadRetailXbe();
    if (!xbe) {
        MESSAGE("skipped: retail XBE not found (FUT_TEST_XBE)");
        return;
    }
    const auto& x = *xbe;
    // prologues of both probes
    CHECK(At(x, cam::kFollowUpdate, 6) == std::vector<std::uint8_t>{0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0});
    CHECK(At(x, cam::kMakeYRotation, 6) == std::vector<std::uint8_t>{0xD9, 0x44, 0x24, 0x04, 0x33, 0xC0});
    // the follow camera's rotation call: call 0x103020 at 0x17B2B, returns to 0x17B30
    const auto call = At(x, 0x17B2B, 5);
    REQUIRE(call.size() == 5);
    CHECK(call[0] == 0xE8);
    std::int32_t rel = 0;
    std::memcpy(&rel, call.data() + 1, 4);
    CHECK(static_cast<std::uint32_t>(0x17B2B + 5 + rel) == cam::kMakeYRotation);
    CHECK(0x17B2B + 5 == cam::kFollowRotationReturn);
    // the camera mode dispatcher FUN_00019C80 calls the follow camera (case 0) at 0x19CCB
    const auto dispatch = At(x, 0x19CCB, 5);
    REQUIRE(dispatch.size() == 5);
    std::memcpy(&rel, dispatch.data() + 1, 4);
    CHECK(static_cast<std::uint32_t>(0x19CCB + 5 + rel) == cam::kFollowUpdate);
    // the filter constants (FUN_00015990 0x17608..0x17961)
    CHECK(F32(x, 0x293B60) == 0.91f);   // stage 1
    CHECK(F32(x, 0x293B58) == 0.97f);   // stage 2
    CHECK(F32(x, 0x293A24) == 0.001f);  // v -> w
    CHECK(F32(x, 0x293AB0) == 0.25f);   // quarter rate below
    CHECK(F32(x, 0x293B68) == 0.9f);    // ... this stick value
    CHECK(F32(x, 0x293B5C) == -2.0f);   // clamp of stage 1 (-2 * rate)
    CHECK(F32(x, 0x293A10) == 0.1f);    // per-tick clamp of the angle
    // the stick values the probe reads: fld [ecx+0xB38] at 0x1760C, invert flag mov al,[0x2D9440] at 0x17629
    CHECK(At(x, 0x1760C, 6) == std::vector<std::uint8_t>{0xD9, 0x81, 0x38, 0x0B, 0x00, 0x00});
    CHECK(At(x, 0x17629, 5) == std::vector<std::uint8_t>{0xA0, 0x40, 0x94, 0x2D, 0x00});
    // the state the probe clears: fstp [esi+0x154] (stage 1) at 0x17862, fstp [eax+0xC] (stage 2) at 0x178FA
    CHECK(At(x, 0x17862, 6) == std::vector<std::uint8_t>{0xD9, 0x9E, 0x54, 0x01, 0x00, 0x00});
    CHECK(At(x, 0x178FA, 3) == std::vector<std::uint8_t>{0xD9, 0x58, 0x0C});
}

TEST_CASE("G5: input script command game camera_trace on|off") {
    using namespace inp::script;
    Program p;
    std::string error;
    REQUIRE(Parse("game camera_trace on\ngame CAMERA_TRACE Off\n", 60, p, &error));
    REQUIRE(p.code.size() == 2);
    CHECK(p.code[0].op == Op::Game);
    CHECK(p.code[0].text == "camera_trace on");
    CHECK(p.code[1].text == "camera_trace off");
    for (const char* bad : {"game camera_trace\n", "game camera_trace maybe\n", "game camera_trace on now\n"}) {
        Program q;
        std::string e;
        CHECK_FALSE(Parse(bad, 60, q, &e));
    }
}

// ---- G7 ---------------------------------------------------------------------------------------------------------

TEST_CASE("G7: [cheats] level_skip schema row (formerly [game] level_skip_cheat)") {
    CHECK(core::ConfigKeyDefault("cheats", "level_skip") == "1");
    const core::ConfigKeyDef* key = core::FindConfigKey("cheats", "level_skip");
    REQUIRE(key != nullptr);
    CHECK(key->type == core::ConfigKeyType::Bool);
    CHECK(key->tab == core::ConfigUiTab::Game);
    CHECK(core::FindConfigKey("game", "level_skip_cheat") == nullptr);  // old name: only an accepted alias
}

TEST_CASE("G7: successor table = FsChangeLevel arguments of the level scripts, one chain through all levels") {
    CHECK(std::string(skip::NextLevel("Level1-1")) == "Level1-2");
    CHECK(std::string(skip::NextLevel("level1-5")) == "Level1-7");
    CHECK(std::string(skip::NextLevel("LEVEL1-7")) == "Level1-6");
    CHECK(std::string(skip::NextLevel("Level1-6")) == "Level2-1");
    CHECK(std::string(skip::NextLevel("Level2-1")) == "Level2-6");
    CHECK(std::string(skip::NextLevel("Level2-6")) == "Level2-2");
    CHECK(std::string(skip::NextLevel("Level3-3")) == "Level3-6");
    CHECK(std::string(skip::NextLevel("Level3-6")) == "Level3-4");
    CHECK(std::string(skip::NextLevel("Level3-5")) == "Level3-3");
    CHECK(std::string(skip::NextLevel("Level5-3")) == "Level1-1");
    CHECK(skip::NextLevel("Level9-9") == nullptr);
    CHECK(skip::NextLevel("") == nullptr);

    // The chain from Level1-1 follows the game order of G4's level table and returns to Level1-1.
    std::string level = "Level1-1";
    std::set<std::string> seen;
    for (const xapi::select::StartLevel& l : xapi::select::StartLevels()) {
        CHECK(level == l.level);
        seen.insert(level);
        const char* next = skip::NextLevel(level);
        REQUIRE(next != nullptr);
        level = next;
    }
    CHECK(level == "Level1-1");
    CHECK(seen.size() == xapi::select::StartLevels().size());
}

TEST_CASE("G7: sequence detector - A A B B A A B B A A with releases") {
    skip::SequenceDetector d;
    d.Configure(60);
    CHECK(Sequence(d, "AABBAABBAA"));
    CHECK(d.Progress() == 0);  // cleared after a hit
    CHECK_FALSE(Sequence(d, "AABBAABBA"));
    CHECK(d.Progress() == 9);
    CHECK(Press(d, 'A'));

    // a held button counts once: A held for the second "A" without a release in between is one press
    skip::SequenceDetector h;
    h.Configure(60);
    CHECK_FALSE(Press(h, 'A', 10, 0));
    CHECK_FALSE(Press(h, 'A', 10, 4));  // still held from before: no new edge
    CHECK(h.Progress() == 1);

    // wrong order: no hit; extra presses before the sequence do not matter (the last ten count)
    skip::SequenceDetector w;
    w.Configure(60);
    CHECK_FALSE(Sequence(w, "ABABABABAB"));
    CHECK(Sequence(w, "A AABBAABBAA"));
    CHECK(Sequence(w, "BBB AABBAABBAA"));
    CHECK(Sequence(w, "AAABBAABBAA"));  // A A A B B ...: the last ten presses are the sequence
}

TEST_CASE("G7: sequence detector - time window, pause, other buttons, A+B, not armed") {
    skip::SequenceDetector d;
    d.Configure(60);
    // whole sequence > 4 s (10 presses every 30 ticks = 4.5 s): no hit
    CHECK_FALSE(Sequence(d, "AABBAABBAA", 3, 27));
    // exactly within: every 26 ticks -> 9 * 26 = 234 ticks < 240
    skip::SequenceDetector ok;
    ok.Configure(60);
    CHECK(Sequence(ok, "AABBAABBAA", 3, 23));

    // a pause > 1 s between two presses starts over
    skip::SequenceDetector p;
    p.Configure(60);
    CHECK_FALSE(Sequence(p, "AABBA"));
    for (int i = 0; i < 61; ++i) {
        p.Feed(Buttons(0, 0), true);
    }
    CHECK_FALSE(Sequence(p, "ABBAA"));
    CHECK(Sequence(p, "AABBAABBAA"));

    // another button (X, a D-pad direction, START) in between starts over
    skip::SequenceDetector o;
    o.Configure(60);
    CHECK_FALSE(Sequence(o, "AABBA"));
    o.Feed(Buttons(0, 0, 0, 255), true);  // X
    o.Feed(Buttons(0, 0), true);
    CHECK_FALSE(Sequence(o, "ABBAA"));
    CHECK(o.Progress() == 2);  // "A A" at the end is a new beginning
    o.Feed(Buttons(0, 0, inp::kButtonDpadUp), true);
    o.Feed(Buttons(0, 0), true);
    CHECK(o.Progress() == 0);
    CHECK(Sequence(o, "AABBAABBAA"));
    // sticks do not matter
    inp::PadState stick = Buttons(0, 0);
    stick.thumb[inp::kAxisLX] = 32767;
    CHECK_FALSE(Sequence(o, "AABBA"));
    o.Feed(stick, true);
    CHECK(Sequence(o, "ABBAA"));

    // A and B together start over
    skip::SequenceDetector ab;
    ab.Configure(60);
    CHECK_FALSE(Sequence(ab, "AABBA"));
    ab.Feed(Buttons(255, 255), true);
    ab.Feed(Buttons(0, 0), true);
    CHECK(ab.Progress() == 0);

    // not armed (menu, cut scene, pause): presses are dropped
    skip::SequenceDetector n;
    n.Configure(60);
    CHECK_FALSE(Sequence(n, "AABBAABBA"));
    CHECK_FALSE(Press(n, 'A', 3, 4, false));
    CHECK(n.Progress() == 0);
    CHECK(n.HasPressEdge(Buttons(255, 0)));
    n.Feed(Buttons(255, 0), true);
    CHECK_FALSE(n.HasPressEdge(Buttons(255, 0)));
    CHECK(n.HasPressEdge(Buttons(255, 40)));  // B from the channel threshold 30 on
    CHECK_FALSE(n.HasPressEdge(Buttons(255, 29)));

    // 50 Hz logic: the window is in seconds
    skip::SequenceDetector pal;
    pal.Configure(50);
    CHECK_FALSE(Sequence(pal, "AABBAABBAA", 3, 22));  // 9 * 25 = 225 ticks = 4.5 s
    CHECK(Sequence(pal, "AABBAABBAA", 3, 18));        // 9 * 21 = 189 ticks = 3.8 s
}

namespace {
int g_calls[3] = {};
std::vector<int> g_order;
void FilterA(int, inp::PadState& s) noexcept {
    ++g_calls[0];
    g_order.push_back(0);
    s.analog[inp::kAnalogY] = 7;
}
void FilterB(int, inp::PadState& s) noexcept {
    ++g_calls[1];
    g_order.push_back(1);
    s.analog[inp::kAnalogX] = static_cast<std::uint8_t>(s.analog[inp::kAnalogY] + 1);  // sees FilterA's change
}
void FilterC(int, inp::PadState&) noexcept {
    ++g_calls[2];
    g_order.push_back(2);
}
class NullSource final : public inp::InputSource {
public:
    const char* Name() const noexcept override { return "null"; }
    inp::SourceKind Kind() const noexcept override { return inp::SourceKind::Script; }
    int DefaultPort() const noexcept override { return 0; }
    bool Connected() const noexcept override { return true; }
    bool Sample(int, std::uint64_t, inp::PadState& out) noexcept override {
        out = {};
        return true;
    }
};
class NullConsumer final : public inp::InputConsumer {
public:
    const char* Name() const noexcept override { return "consumer"; }
    void Consume(int, std::uint64_t, const inp::PadState&) noexcept override {}
};
}  // namespace

TEST_CASE("G7: router - chained sample filters after the main filter, Diverted") {
    NullSource src;
    inp::HostRouter r;
    r.UseSources({&src});
    r.SetSampleFilter(&FilterA);
    CHECK_FALSE(r.AddSampleFilter(nullptr));
    CHECK_FALSE(r.AddSampleFilter(&FilterA));  // already the main filter
    CHECK(r.AddSampleFilter(&FilterB));
    CHECK_FALSE(r.AddSampleFilter(&FilterB));  // no duplicates
    CHECK(r.AddSampleFilter(&FilterC));
    g_order.clear();
    inp::PadState out;
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(g_order == std::vector<int>{0, 1, 2});
    CHECK(out.analog[inp::kAnalogX] == 8);
    // the chain has a fixed size
    inp::HostRouter full;
    int added = 0;
    for (auto f : {&FilterA, &FilterB, &FilterC}) {
        added += full.AddSampleFilter(f) ? 1 : 0;
    }
    CHECK(added == 3);
    CHECK(inp::HostRouter::kMaxChainedFilters == 4);

    NullConsumer consumer;
    CHECK_FALSE(r.Diverted(0));
    r.Divert(0, &consumer);
    CHECK(r.Diverted(0));
    CHECK_FALSE(r.Diverted(1));
    CHECK_FALSE(r.Diverted(-1));
    CHECK_FALSE(r.Diverted(inp::kPortCount));
    g_order.clear();
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(g_order == std::vector<int>{0, 1, 2});  // the filters also run for a diverted port
    r.Divert(0, nullptr);
    CHECK_FALSE(r.Diverted(0));
}
