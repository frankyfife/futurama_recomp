// Unit tests of G3 widescreen without the game: Hor+ frustum, placement mapping, the classification rules (with the
// draws measured in Level 1-1, the pause menu, a loading screen and the front end, docs/g3_report.md 3), the HUD
// groups (Level 4-1 swamp timer, docs/g3_report.md 7), the [render] aspect parser, the render sizes and the view
// layout of a widescreen display.
#include "doctest.h"

#include "gfx/nipc_gpu.h"
#include "gfx/widescreen.h"
#include "gfx/widescreen_math.h"
#include "platform/window.h"

#include <cmath>

namespace ws = gfx::widescreen;

namespace {

bool Near(double a, double b, double eps = 1e-4) {
    return std::fabs(a - b) <= eps;
}

gfx::ni::NiFrustum Frustum(float l, float r, float t, float b, float n, float f) {
    gfx::ni::NiFrustum x;
    x.left = l;
    x.right = r;
    x.top = t;
    x.bottom = b;
    x.nearPlane = n;
    x.farPlane = f;
    return x;
}

gfx::ni::NiRectF Port(float l, float r, float t, float b) {
    gfx::ni::NiRectF p;
    p.left = l;
    p.right = r;
    p.top = t;
    p.bottom = b;
    return p;
}

ws::DrawFacts Screen(ws::DrawSource source, float x0, float y0, float x1, float y1, bool textured = true,
                     bool level = true) {
    ws::DrawFacts d;
    d.source = source;
    d.camera = ws::DrawFacts::Camera::Widened;  // HUD polygons are drawn with the scene camera's data current
    d.hasBounds = true;
    d.bounds = {x0, y0, x1, y1};
    d.textured = textured;
    d.anchoring = level;
    return d;
}

}  // namespace

TEST_CASE("widescreen: Hor+ factor of the display aspects") {
    CHECK(Near(ws::HorPlusFactor(16, 9), 4.0 / 3.0));
    CHECK(Near(ws::HorPlusFactor(16, 10), 1.2));
    CHECK(Near(ws::HorPlusFactor(64, 27), 16.0 / 9.0));
    CHECK(Near(ws::HorPlusFactor(4, 3), 1.0));
    CHECK(ws::HorPlusFactor(0, 0) == 1.0f);
    CHECK(Near(ws::HorPlusFactor(2560, 1440), 4.0 / 3.0));
}

TEST_CASE("widescreen: Hor+ keeps the vertical field of view and widens the horizontal one") {
    // Level 1-1 scene camera: l -1 r 1 t 0.75 b -0.75 n 0.25 (4:3).
    gfx::ni::NiFrustum f = Frustum(-1.0f, 1.0f, 0.75f, -0.75f, 0.25f, 2000.0f);
    const float k = ws::HorPlusFactor(16, 9);
    ws::WidenFrustum(&f, k);
    CHECK(f.top == 0.75f);
    CHECK(f.bottom == -0.75f);
    CHECK(f.nearPlane == 0.25f);
    CHECK(Near(f.left, -4.0 / 3.0));
    CHECK(Near(f.right, 4.0 / 3.0));
    // The frustum's aspect is now the display's: (r - l) / (t - b) = 16/9.
    CHECK(Near((f.right - f.left) / (f.top - f.bottom), 16.0 / 9.0));
    // Front end camera (1.5:1 frustum): scaled by the same factor, the vertical extent unchanged.
    gfx::ni::NiFrustum fe = Frustum(-0.771f, 0.771f, 0.514f, -0.514f, 0.1f, 1000.0f);
    ws::WidenFrustum(&fe, k);
    CHECK(Near(fe.right, 0.771 * 4.0 / 3.0));
    CHECK(fe.top == 0.514f);
}

TEST_CASE("widescreen: which cameras get Hor+ (measured cameras)") {
    const gfx::ni::NiRectF full = Port(0.0f, 1.0f, 1.0f, 0.0f);
    CHECK(ws::WidenableCamera(Frustum(-1.0f, 1.0f, 0.75f, -0.75f, 0.25f, 2000.0f), full));      // level
    CHECK(ws::WidenableCamera(Frustum(-0.771f, 0.771f, 0.514f, -0.514f, 0.1f, 1000.0f), full));  // front end
    CHECK(ws::WidenableCamera(Frustum(-0.77f, 0.77f, 0.51f, -0.51f, 0.1f, 1000.0f), full));      // pause menu
    CHECK_FALSE(ws::WidenableCamera(Frustum(-0.5f, 0.5f, 0.5f, -0.5f, 1.0f, 2.0f), full));       // helper n 1 f 2
    CHECK_FALSE(ws::WidenableCamera(Frustum(0.0f, 0.1f, 0.1f, 0.0f, 1.0f, 100.0f), full));       // HUD digits
    CHECK_FALSE(ws::WidenableCamera(Frustum(-0.414f, 0.414f, 0.311f, -0.311f, 0.1f, 100.0f),
                                    Port(0.0f, 0.33f, 1.0f, 0.68f)));                            // avatar
    // A letterboxed port over the whole width still counts (cinematic bars).
    CHECK(ws::WidenableCamera(Frustum(-1.0f, 1.0f, 0.6f, -0.6f, 0.25f, 2000.0f), Port(0.0f, 1.0f, 0.9f, 0.1f)));
    CHECK_FALSE(ws::WidenableCamera(Frustum(-1.0f, 1.0f, 0.75f, -0.75f, 0.25f, NAN), full));
}

TEST_CASE("widescreen: guest pixels -> internal pixels per placement (1280x720, 16:9)") {
    const ws::Mapping stretch = ws::MappingOf(ws::Placement::Stretch, 640, 480, 1280, 720);
    CHECK(Near(stretch.scaleX, 2.0));
    CHECK(Near(stretch.scaleY, 1.5));
    CHECK(Near(stretch.offsetX, 0.0));
    const ws::Mapping center = ws::MappingOf(ws::Placement::Center, 640, 480, 1280, 720);
    CHECK(Near(center.scaleX, 1.5));
    CHECK(Near(center.offsetX, 160.0));  // (1280 - 960) / 2
    const ws::Mapping left = ws::MappingOf(ws::Placement::Left, 640, 480, 1280, 720);
    CHECK(Near(left.offsetX, 0.0));
    const ws::Mapping right = ws::MappingOf(ws::Placement::Right, 640, 480, 1280, 720);
    CHECK(Near(right.offsetX, 320.0));
    // At 4:3 every placement is the plain scale (no bars).
    for (ws::Placement p : {ws::Placement::Stretch, ws::Placement::Center, ws::Placement::Left,
                            ws::Placement::Right}) {
        const ws::Mapping m = ws::MappingOf(p, 640, 480, 1280, 960);
        CHECK(Near(m.scaleX, 2.0));
        CHECK(Near(m.offsetX, 0.0));
    }

    gfx::rhi::Viewport v;
    v.x = 0.0f;
    v.y = 0.0f;
    v.width = 640.0f;
    v.height = 480.0f;
    const gfx::rhi::Viewport vc = ws::PlaceViewport(v, center);
    CHECK(vc.x == 160.0f);
    CHECK(vc.width == 960.0f);
    CHECK(vc.height == 720.0f);
    const gfx::rhi::Viewport vs = ws::PlaceViewport(v, stretch);
    CHECK(vs.width == 1280.0f);
    // The avatar port (0..211 x 0..153) anchored left keeps square pixels.
    gfx::rhi::Viewport avatar;
    avatar.width = 211.0f;
    avatar.height = 153.0f;
    const gfx::rhi::Viewport va = ws::PlaceViewport(avatar, left);
    CHECK(Near(va.width / va.height, 211.0 / 153.0));
    gfx::rhi::Rect r;
    r.left = 0;
    r.top = 0;
    r.right = 640;
    r.bottom = 480;
    const gfx::rhi::Rect rr = ws::PlaceRect(r, right);
    CHECK(rr.left == 320);
    CHECK(rr.right == 1280);
    CHECK(rr.bottom == 720);
}

TEST_CASE("widescreen: classification of the measured HUD of Level 1-1") {
    // Screen polygons (slot 54) with the scene camera's data current.
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 36, 49, 143, 156)) == ws::Placement::Left);    // ring
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 56, 161, 104, 209)) == ws::Placement::Left);   // ammo
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 56, 352, 120, 416)) == ws::Placement::Left);   // Nibbler
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 520, 352, 584, 416)) == ws::Placement::Right); // money
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 516, 49, 604, 137)) == ws::Placement::Right);
    // Digits: geometry of the HUD cameras (l 0, r 0.1).
    ws::DrawFacts digits = Screen(ws::DrawSource::Geometry, 540, 416, 564, 443);
    digits.camera = ws::DrawFacts::Camera::FullWidth;
    digits.hudCamera = true;
    CHECK(ws::Classify(digits) == ws::Placement::Right);
    digits.bounds = {59, 416, 117, 443};
    CHECK(ws::Classify(digits) == ws::Placement::Left);
    // A centred HUD text (hint line, level title) stays in the middle.
    digits.bounds = {220, 336, 420, 360};
    CHECK(ws::Classify(digits) == ws::Placement::Center);
    // Avatar camera (port 0..0.33 x top): all its draws by the port, also the outline ribbons around the head.
    ws::DrawFacts head;
    head.source = ws::DrawSource::Geometry;
    head.camera = ws::DrawFacts::Camera::Port;
    head.port = {0, 0, 211, 153};
    head.anchoring = true;
    CHECK(ws::Classify(head) == ws::Placement::Left);
    head.source = ws::DrawSource::ScreenPolygon;
    head.ribbon = true;
    CHECK(ws::Classify(head) == ws::Placement::Left);
    // The scene: Hor+ geometry and its ribbons are stretched.
    ws::DrawFacts scene;
    scene.source = ws::DrawSource::Geometry;
    scene.camera = ws::DrawFacts::Camera::Widened;
    CHECK(ws::Classify(scene) == ws::Placement::Stretch);
    scene.source = ws::DrawSource::ScreenPolygon;
    scene.ribbon = true;
    CHECK(ws::Classify(scene) == ws::Placement::Stretch);
}

TEST_CASE("widescreen: menus, loading screens and fades") {
    // Outside a level nothing is anchored.
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 36, 49, 143, 156, true, false)) ==
          ws::Placement::Center);
    // Loading picture: 256-pixel tiles of slot 55 must stay together, also in the level context.
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenTexture, 0, 0, 256, 256)) == ws::Placement::Center);
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenTexture, 512, 0, 768, 768)) == ws::Placement::Center);
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenTexture, 540, 350, 604, 478)) == ws::Placement::Center);
    // Full-screen fade (front end, untextured screen polygon) covers the whole width; a textured full-screen picture
    // keeps its 4:3 shape.
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 0, 0, 640, 480, false, false)) ==
          ws::Placement::Stretch);
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 0, 0, 640, 480, true)) == ws::Placement::Center);
    // 3D of a camera without Hor+ (the front end) is one centred picture, even a full-screen untextured backdrop.
    ws::DrawFacts frontEnd = Screen(ws::DrawSource::Geometry, -50, -20, 700, 500, false, false);
    frontEnd.camera = ws::DrawFacts::Camera::FullWidth;
    CHECK(ws::Classify(frontEnd) == ws::Placement::Center);
    frontEnd.anchoring = true;
    frontEnd.bounds = {10, 10, 100, 100};
    CHECK(ws::Classify(frontEnd) == ws::Placement::Center);
    // Clears: the full raster -> the whole width; the avatar's depth clear -> its edge.
    ws::DrawFacts clear;
    clear.source = ws::DrawSource::Clear;
    clear.hasBounds = true;
    clear.bounds = {0, 0, 640, 480};
    CHECK(ws::Classify(clear) == ws::Placement::Stretch);
    clear.bounds = {0, 0, 211, 153};
    clear.anchoring = true;
    CHECK(ws::Classify(clear) == ws::Placement::Left);
}

TEST_CASE("widescreen: exceptions table overrides the thirds") {
    const ws::AnchorException table[] = {
        {ws::DrawSource::ScreenPolygon, {0, 400, 640, 480}, ws::Placement::Center}};
    const ws::DrawFacts bar = Screen(ws::DrawSource::ScreenPolygon, 20, 420, 150, 460);
    CHECK(ws::Classify(bar) == ws::Placement::Left);
    CHECK(ws::Classify(bar, table) == ws::Placement::Center);
    CHECK(ws::AnchorExceptions().empty());
}

// HUD groups (2026-10-14): the pieces of the 4-1 swamp timer as measured in run\hud (guest pixels). Anchored one by
// one, the clock and the progress fill went to the left edge, the flag to the right edge and the bar stayed in the
// centre (user report).
namespace {

struct Piece {
    std::uint32_t object;
    ws::GuestRect rect;
};
// Scene camera list of Level 4-1: ring, bar, clock, flag, progress fill, fish counter, Nibbler; helper camera list:
// the two polygons at the left edge.
constexpr Piece kSwamp[] = {
    {0x0332D040u, {36, 49, 143, 156}},   {0x0332C330u, {192, 400, 448, 432}}, {0x0332D050u, {156, 400, 188, 432}},
    {0x0332C7C0u, {452, 384, 484, 448}}, {0x0332CA40u, {192, 400, 212, 432}}, {0x038BCB90u, {520, 352, 584, 416}},
    {0x03A722E0u, {56, 352, 120, 416}},  {0x03327250u, {0, 93, 128, 157}},    {0x033275C0u, {128, 125, 144, 157}}};

ws::HudGroups SwampGroups() {
    ws::HudGroups g;
    for (const Piece& p : kSwamp) {
        CHECK(g.Add(p.object, p.rect));
    }
    g.Build();
    return g;
}

ws::Placement Placed(const ws::HudGroups& g, std::uint32_t object, const ws::GuestRect& r,
                     ws::DrawSource source = ws::DrawSource::ScreenPolygon) {
    ws::DrawFacts d = Screen(source, r.x0, r.y0, r.x1, r.y1);
    if (source == ws::DrawSource::Geometry) {
        d.camera = ws::DrawFacts::Camera::FullWidth;
        d.hudCamera = true;
    }
    d.grouped = g.GroupRect(object, r, &d.group);
    return ws::Classify(d);
}

}  // namespace

TEST_CASE("widescreen: HUD groups keep the 4-1 swamp timer together") {
    const ws::HudGroups g = SwampGroups();
    CHECK(g.PieceCount() == 9);
    CHECK(g.GroupCount() == 4);  // portrait (ring + helper polygons), timer, fish counter, Nibbler counter
    // Before: each piece by its own centre - the timer is torn apart.
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 156, 400, 188, 432)) == ws::Placement::Left);    // clock
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 192, 400, 212, 432)) == ws::Placement::Left);    // fill
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 192, 400, 448, 432)) == ws::Placement::Center);  // bar
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 452, 384, 484, 448)) == ws::Placement::Right);   // flag
    // After: clock, bar, fill and flag form one group (156..484, centre 320) and stay together in the centre.
    ws::GuestRect u;
    REQUIRE(g.GroupRect(0x0332D050u, {156, 400, 188, 432}, &u));
    CHECK(u.x0 == 156.0f);
    CHECK(u.x1 == 484.0f);
    CHECK(u.y0 == 384.0f);
    CHECK(u.y1 == 448.0f);
    for (int i : {1, 2, 3, 4}) {
        CHECK(Placed(g, kSwamp[i].object, kSwamp[i].rect) == ws::Placement::Center);
    }
    // The other elements keep their edges.
    CHECK(Placed(g, kSwamp[0].object, kSwamp[0].rect) == ws::Placement::Left);   // portrait ring
    CHECK(Placed(g, kSwamp[5].object, kSwamp[5].rect) == ws::Placement::Right);  // fish counter
    CHECK(Placed(g, kSwamp[6].object, kSwamp[6].rect) == ws::Placement::Left);   // Nibbler counter
    CHECK(Placed(g, kSwamp[7].object, kSwamp[7].rect) == ws::Placement::Left);   // helper polygons
    CHECK(Placed(g, kSwamp[8].object, kSwamp[8].rect) == ws::Placement::Left);
    // Digits of the HUD cameras (geometry, no piece) join the group they touch.
    CHECK(Placed(g, 0, {265, 370, 375, 405}, ws::DrawSource::Geometry) == ws::Placement::Center);  // "1:36"
    CHECK(Placed(g, 0, {56, 416, 120, 443}, ws::DrawSource::Geometry) == ws::Placement::Left);     // "0/5"
    CHECK(Placed(g, 0, {540, 416, 564, 443}, ws::DrawSource::Geometry) == ws::Placement::Right);   // "5"
    // A text far from every group is placed by itself.
    ws::GuestRect none;
    CHECK_FALSE(g.GroupRect(0, {231, 200, 409, 224}, &none));
}

TEST_CASE("widescreen: HUD groups - the growing progress fill never changes sides") {
    // The fill grows from the bar's left end; alone it would jump from the left edge to the centre at 42 px.
    for (float w = 1.0f; w <= 256.0f; w += 1.0f) {
        ws::HudGroups g;
        g.Add(1, {192, 400, 448, 432});      // bar
        g.Add(2, {156, 400, 188, 432});      // clock
        g.Add(3, {192, 400, 192 + w, 432});  // fill
        g.Add(4, {452, 384, 484, 448});      // flag
        g.Build();
        CHECK(g.GroupCount() == 1);
        CHECK(Placed(g, 3, {192, 400, 192 + w, 432}) == ws::Placement::Center);
        CHECK(Placed(g, 2, {156, 400, 188, 432}) == ws::Placement::Center);
    }
    // A fill of width 0 is no piece; its (empty) draw still finds the bar's group by its rectangle.
    ws::HudGroups g;
    CHECK_FALSE(g.Add(3, {192, 400, 192, 432}));
    g.Add(1, {192, 400, 448, 432});
    g.Build();
    ws::GuestRect u;
    CHECK(g.GroupRect(3, {192, 400, 193, 432}, &u));
}

TEST_CASE("widescreen: HUD groups - gap, banners, fades, capacity") {
    // Pieces up to kHudGroupGap (16 px) apart join, further apart they do not; the distance counts in x and in y.
    ws::HudGroups g;
    g.Add(1, {100, 100, 120, 120});
    g.Add(2, {136, 100, 150, 120});  // 16 px to the right: joins
    g.Add(3, {167, 100, 180, 120});  // 17 px further: separate
    g.Add(4, {100, 137, 120, 150});  // 17 px below 1: separate
    g.Add(5, {100, 136, 110, 140});  // 16 px below 1: joins (and is 0 px away from 4)
    g.Build();
    CHECK(g.GroupCount() == 2);
    CHECK(g.GroupOfPiece(0) == g.GroupOfPiece(1));
    CHECK(g.GroupOfPiece(0) == g.GroupOfPiece(4));
    CHECK(g.GroupOfPiece(0) == g.GroupOfPiece(3));  // through piece 5
    CHECK(g.GroupOfPiece(0) != g.GroupOfPiece(2));
    // Transitive: a chain of close pieces is one group.
    g.Clear();
    for (std::uint32_t i = 0; i < 10; ++i) {
        g.Add(i + 1, {10.0f + 30.0f * static_cast<float>(i), 10, 30.0f + 30.0f * static_cast<float>(i), 30});
    }
    g.Build();
    CHECK(g.GroupCount() == 1);
    // A banner (wider than half the raster) and a full-screen fade are no pieces and pull nothing into the centre.
    g.Clear();
    CHECK_FALSE(g.Add(1, {0, 0, 640, 60}));
    CHECK_FALSE(g.Add(2, {0, 0, 640, 480}));
    CHECK(g.Add(3, {36, 49, 143, 156}));
    CHECK_FALSE(g.Add(3, {36, 49, 143, 156}));  // the same object once
    g.Build();
    CHECK(Placed(g, 3, {36, 49, 143, 156}) == ws::Placement::Left);
    ws::GuestRect u;
    CHECK_FALSE(g.GroupRect(1, {0, 0, 640, 60}, &u));
    CHECK(ws::Classify(Screen(ws::DrawSource::ScreenPolygon, 0, 0, 640, 60)) == ws::Placement::Center);
    // Capacity: at most kMaxPieces; an empty table has no groups.
    g.Clear();
    std::uint32_t added = 0;
    for (std::uint32_t i = 0; i < ws::HudGroups::kMaxPieces + 8; ++i) {
        const float x = static_cast<float>(i % 30) * 20.0f;
        added += g.Add(i + 1, {x, 10, x + 5, 20}) ? 1u : 0u;
    }
    CHECK(added == ws::HudGroups::kMaxPieces);
    g.Clear();
    g.Build();
    CHECK(g.GroupCount() == 0);
    CHECK_FALSE(g.GroupRect(1, {10, 10, 20, 20}, &u));
}

TEST_CASE("widescreen: HUD groups keep the measured Level 1-1 HUD at its edges") {
    ws::HudGroups g;
    g.Add(1, {36, 49, 143, 156});    // ring of the health display
    g.Add(2, {77, 88, 141, 152});    // health bar
    g.Add(3, {516, 49, 604, 137});   // top right
    g.Add(4, {56, 161, 104, 209});   // ammo (5 px below the ring)
    g.Add(5, {520, 352, 584, 416});  // money
    g.Add(6, {56, 352, 120, 416});   // Nibbler
    g.Build();
    CHECK(g.GroupCount() == 4);
    CHECK(Placed(g, 1, {36, 49, 143, 156}) == ws::Placement::Left);
    CHECK(Placed(g, 2, {77, 88, 141, 152}) == ws::Placement::Left);
    CHECK(Placed(g, 3, {516, 49, 604, 137}) == ws::Placement::Right);
    CHECK(Placed(g, 4, {56, 161, 104, 209}) == ws::Placement::Left);
    CHECK(Placed(g, 5, {520, 352, 584, 416}) == ws::Placement::Right);
    CHECK(Placed(g, 6, {56, 352, 120, 416}) == ws::Placement::Left);
    // Grouping changes nothing outside a level, and the exceptions table still decides by the draw's own rectangle.
    ws::DrawFacts d = Screen(ws::DrawSource::ScreenPolygon, 56, 352, 120, 416, true, false);
    d.grouped = g.GroupRect(6, d.bounds, &d.group);
    CHECK(d.grouped);
    CHECK(ws::Classify(d) == ws::Placement::Center);
    const ws::AnchorException table[] = {{ws::DrawSource::ScreenPolygon, {0, 340, 640, 480}, ws::Placement::Right}};
    d.anchoring = true;
    CHECK(ws::Classify(d, table) == ws::Placement::Right);
}

TEST_CASE("widescreen: [render] aspect values") {
    ws::AspectMode m = ws::AspectMode::Original;
    CHECK(ws::ParseAspect("16:9", &m));
    CHECK(m == ws::AspectMode::Wide16x9);
    CHECK(ws::ParseAspect(" 21:9 ", &m));
    CHECK(m == ws::AspectMode::Wide21x9);
    CHECK(ws::ParseAspect("Monitor", &m));
    CHECK(m == ws::AspectMode::Monitor);
    CHECK(ws::ParseAspect("original", &m));
    CHECK(m == ws::AspectMode::Original);
    CHECK_FALSE(ws::ParseAspect("4:3", &m));
    CHECK_FALSE(ws::ParseAspect("", &m));
    std::uint32_t w = 1, h = 1;
    ws::AspectOf(ws::AspectMode::Original, 0, 0, 1920, 1080, &w, &h);
    CHECK((w == 0 && h == 0));
    ws::AspectOf(ws::AspectMode::Wide16x10, 0, 0, 0, 0, &w, &h);
    CHECK((w == 16 && h == 10));
    ws::AspectOf(ws::AspectMode::Wide21x9, 0, 0, 0, 0, &w, &h);
    CHECK((w == 64 && h == 27));
    ws::AspectOf(ws::AspectMode::Monitor, 0, 0, 3440, 1440, &w, &h);
    CHECK((w == 43 && h == 18));
    ws::AspectOf(ws::AspectMode::Monitor, 2560, 1080, 3840, 2160, &w, &h);  // [render] resolution wins
    CHECK((w == 64 && h == 27));
    ws::AspectOf(ws::AspectMode::Monitor, 0, 0, 1920, 1080, &w, &h);
    CHECK((w == 16 && h == 9));
    ws::AspectOf(ws::AspectMode::Monitor, 0, 0, 0, 0, &w, &h);
    CHECK((w == 0 && h == 0));
}

TEST_CASE("widescreen: render sizes and view layout of a widescreen display") {
    // [render] resolution 1280x960 with 16:9: the largest 16:9 area.
    gfx::RenderSizes s = gfx::ComputeRenderSizes(640, 480, 1280, 960, 0, 0, 1.0f, 16, 9);
    CHECK(s.outputWidth == 1280);
    CHECK(s.outputHeight == 720);
    // Auto: the 16:9 view as it is; render scale on top.
    s = gfx::ComputeRenderSizes(640, 480, 0, 0, 1920, 1080, 1.5f, 16, 9);
    CHECK(s.outputWidth == 1920);
    CHECK(s.outputHeight == 1080);
    CHECK(s.internalWidth == 2880);
    // Without an aspect: unchanged 4:3 (pillarbox inside a 16:9 view).
    s = gfx::ComputeRenderSizes(640, 480, 0, 0, 1920, 1080, 1.0f);
    CHECK(s.outputWidth == 1440);
    CHECK(s.outputHeight == 1080);
    // Windowed view: the 16:9 rectangle inside a 4:3 client area; fullscreen covers the monitor.
    platform::ViewRect r = platform::ViewLayout(1280, 960, false, 16, 9);
    CHECK(r.width == 1280);
    CHECK(r.height == 720);
    CHECK(r.y == 120);
    r = platform::ViewLayout(3840, 2160, true, 16, 9);
    CHECK(r.width == 3840);
    r = platform::ViewLayout(1280, 960, false, 0, 0);  // 0 x 0 = 4:3 as before
    CHECK(r.width == 1280);
    CHECK(r.height == 960);
}
