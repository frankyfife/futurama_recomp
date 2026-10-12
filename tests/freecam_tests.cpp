// Unit tests of the M4b freecam pieces without the game: the L3+R3 chord filter (input/chord_filter.h) and the
// pose rig (gfx/freecam_math.h).
#include "doctest.h"

#include "gfx/freecam_math.h"
#include "input/chord_filter.h"

#include <cmath>
#include <vector>

namespace {

constexpr std::uint16_t kL3 = inp::kButtonLeftThumb;
constexpr std::uint16_t kR3 = inp::kButtonRightThumb;

// Runs the filter over a button sequence; returns what the game sees per tick and the ticks the chord fired.
struct Run {
    std::vector<std::uint16_t> game;
    std::vector<int> fired;
};
Run Feed(inp::ChordFilter& f, const std::vector<std::uint16_t>& buttons) {
    Run r;
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        inp::PadState s;
        s.buttons = buttons[i];
        if (f.Filter(s)) {
            r.fired.push_back(static_cast<int>(i));
        }
        r.game.push_back(s.buttons);
    }
    return r;
}

bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

}  // namespace

TEST_CASE("freecam chord: L3+R3 together fires once and never reaches the game") {
    inp::ChordFilter f;
    const std::uint16_t both = kL3 | kR3;
    const Run r = Feed(f, {0, both, both, both, 0, 0});
    CHECK(r.fired == std::vector<int>{1});
    for (std::uint16_t g : r.game) {
        CHECK(g == 0);
    }
}

TEST_CASE("freecam chord: staggered press within the window still fires, no single click leaks") {
    inp::ChordFilter f;  // window 4 ticks
    const Run r = Feed(f, {kR3, kR3, kR3 | kL3, kR3 | kL3, kR3, 0, 0});
    CHECK(r.fired == std::vector<int>{2});
    for (std::uint16_t g : r.game) {
        CHECK((g & (kL3 | kR3)) == 0);
    }
}

TEST_CASE("freecam chord: a held single click reaches the game after the window") {
    inp::ChordFilter f(kL3 | kR3, 4);
    const Run r = Feed(f, {kR3, kR3, kR3, kR3, kR3, kR3, kR3, 0, 0});
    CHECK(r.fired.empty());
    CHECK(r.game[0] == 0);
    CHECK(r.game[3] == 0);
    CHECK(r.game[4] == kR3);  // window over: passed on
    CHECK(r.game[6] == kR3);
    CHECK(r.game[7] == 0);
}

TEST_CASE("freecam chord: a short tap is delivered as a one-tick press") {
    inp::ChordFilter f;
    const Run r = Feed(f, {kR3, kR3, 0, 0, 0});
    CHECK(r.fired.empty());
    CHECK(r.game[0] == 0);
    CHECK(r.game[1] == 0);
    CHECK(r.game[2] == kR3);  // released inside the window: one tick press
    CHECK(r.game[3] == 0);
}

TEST_CASE("freecam chord: other buttons pass unchanged") {
    inp::ChordFilter f;
    const std::uint16_t start = inp::kButtonStart;
    const Run r = Feed(f, {start, start | kL3 | kR3, start, 0});
    CHECK(r.fired == std::vector<int>{1});
    CHECK(r.game[0] == start);
    CHECK(r.game[1] == start);
    CHECK(r.game[2] == start);
}

TEST_CASE("freecam rig: starts at the game pose and keeps world up, handedness and orthonormality") {
    using namespace gfx::freecam;
    // The front-end camera of the game: looking along -Z, up +Y (run/freecam log 2026-10-12).
    Pose game;
    game.position = {-0.6f, 0.1f, 3.0f};
    game.direction = {0, 0, -1};
    game.up = {0, 1, 0};
    game.right = Cross(game.direction, game.up);
    Rig rig;
    rig.Start(game);
    CHECK(rig.WorldUp().y == 1.0f);
    CHECK(rig.Handedness() == 1.0f);
    CHECK(Near(rig.Current().direction.z, -1.0f));

    Motion m;
    m.yaw = 0.3f;  // turn right
    m.pitch = 0.2f;  // look up
    m.forward = 2.0f;
    rig.Step(m);
    const Pose& p = rig.Current();
    CHECK(Dot(p.direction, game.right) > 0.2f);  // turned towards the old right
    CHECK(p.direction.y > 0.1f);                  // looks up
    CHECK(Near(Length(p.direction), 1.0f));
    CHECK(Near(Length(p.up), 1.0f));
    CHECK(Near(Dot(p.direction, p.up), 0.0f));
    CHECK(Near(Dot(p.direction, p.right), 0.0f));
    CHECK(Near(Dot(p.right, Vec3{0, 1, 0}), 0.0f));  // no roll
    CHECK(Dot(p.right, Cross(p.direction, p.up)) > 0.99f);  // same handedness as the game
    CHECK(Near(Length(p.position - game.position), 2.0f));
}

TEST_CASE("freecam rig: pitch stops short of straight up") {
    using namespace gfx::freecam;
    Pose game;
    game.direction = {1, 0, 0};
    game.up = {0, 0, 1};
    game.right = Cross(game.direction, game.up) * -1.0f;  // left-handed convention
    Rig rig;
    rig.Start(game);
    CHECK(rig.WorldUp().z == 1.0f);
    CHECK(rig.Handedness() == -1.0f);
    Motion m;
    m.pitch = 0.1f;
    for (int i = 0; i < 100; ++i) {
        rig.Step(m);
    }
    CHECK(rig.Current().direction.z < 0.99986f);
    CHECK(rig.Current().direction.z > 0.99f);
    CHECK(Dot(rig.Current().right, Cross(rig.Current().direction, rig.Current().up)) < -0.99f);
}

TEST_CASE("freecam stick axis: dead zone and quadratic response") {
    using gfx::freecam::StickAxis;
    CHECK(StickAxis(0, 0.15f) == 0.0f);
    CHECK(StickAxis(3000, 0.15f) == 0.0f);
    CHECK(Near(StickAxis(32767, 0.15f), 1.0f));
    CHECK(Near(StickAxis(-32768, 0.15f), -1.0f));
    CHECK(StickAxis(16384, 0.15f) < 0.5f);
}
