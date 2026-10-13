// Unit tests of G3 widescreen without the game: Hor+ frustum, placement mapping, the classification rules (with the
// draws measured in Level 1-1, the pause menu, a loading screen and the front end, docs/g3_report.md 3), the
// [render] aspect parser, the render sizes and the view layout of a widescreen display.
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
