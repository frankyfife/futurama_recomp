// M8: pure helpers of ni_extract - slot 52 RenderPoints quads (ni::ExtractPoints) and the texture transforms of the
// projected shadow passes (ni::ProjectedShadowTransform, ni::ShadowClipTransform).
#include "doctest.h"

#include "gfx/ni_extract.h"

using namespace gfx;

namespace {
// Row vector (x, y, z, 1) times a D3D matrix (column c).
float Apply(const Matrix4& m, const Float3& p, int c) {
    return p.x * m.m[0][c] + p.y * m.m[1][c] + p.z * m.m[2][c] + m.m[3][c];
}
}  // namespace

TEST_CASE("extract shadows: the projection matrix maps camera space to the effect's world projection") {
    // Camera at (10, 2, 0), right = -z, up = +y, direction = +x: camera point (x, y, z) = world (10 + z, 2 + y, -x).
    const Float3 inverseView[4] = {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}, {10, 2, 0}};
    // Effect: u = 0.5 * world.x + 0.25, v = -0.5 * world.z + 0.75 (rows = world axes, 16-byte stride).
    const float projection[3][4] = {{0.5f, 0, 0, 0}, {0, 0, 0, 0}, {0, -0.5f, 0, 0}};
    const float translation[3] = {0.25f, 0.75f, 0.0f};
    const Matrix4 m = ni::ProjectedShadowTransform(inverseView, projection, translation);
    const Float3 camPoint{1.0f, 3.0f, 4.0f};  // world (14, 5, -1)
    CHECK(Apply(m, camPoint, 0) == doctest::Approx(0.5f * 14 + 0.25f));
    CHECK(Apply(m, camPoint, 1) == doctest::Approx(-0.5f * -1 + 0.75f));
    CHECK(m.m[0][3] == 0.0f);
    CHECK(m.m[3][3] == 0.0f);
}

TEST_CASE("extract shadows: the clip coordinate is below 0.5 on the plane's positive side") {
    const Float3 inverseView[4] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}};  // camera space = world space
    // Plane normal (0, -1, 0), constant 1: positive side n.p - d > 0 <=> y < -1 (below the caster).
    const Matrix4 m = ni::ShadowClipTransform(inverseView, {0.0f, -1.0f, 0.0f}, 1.0f);
    CHECK(Apply(m, {0.0f, -3.0f, 0.0f}, 0) < 0.5f);   // floor below: black texel, the shadow stays
    CHECK(Apply(m, {0.0f, 0.0f, 0.0f}, 0) >= 0.5f);   // above the plane: white texel, ADD clears the shadow
    CHECK(Apply(m, {0.0f, -1.0f, 0.0f}, 0) == doctest::Approx(0.5f));
    CHECK(Apply(m, {5.0f, -3.0f, 7.0f}, 1) == 0.0f);  // v = 0 (row 0 of the 2 x 1 texture)
}

TEST_CASE("extract particles: quad axes are (right +/- up) * radius / scale") {
    Float3 a, b;
    ni::ParticleAxes({1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 2.0f, 4.0f, &a, &b);
    CHECK(a.x == 0.5f);
    CHECK(a.y == 0.5f);
    CHECK(a.z == 0.0f);
    CHECK(b.x == 0.5f);
    CHECK(b.y == -0.5f);
    CHECK(b.z == 0.0f);
}

TEST_CASE("extract particles: corners c - A, c - B, c + A, c + B scaled by the particle size") {
    const Float3 a{1.0f, 1.0f, 0.0f}, b{1.0f, -1.0f, 0.0f};
    Float3 c[4];
    ni::ParticleCorners({10.0f, 20.0f, 30.0f}, 2.0f, a, b, c);
    CHECK(c[0].x == 8.0f);  // bottom left (right/up camera axes)
    CHECK(c[0].y == 18.0f);
    CHECK(c[1].x == 8.0f);  // top left
    CHECK(c[1].y == 22.0f);
    CHECK(c[2].x == 12.0f);  // top right
    CHECK(c[2].y == 22.0f);
    CHECK(c[3].x == 12.0f);  // bottom right
    CHECK(c[3].y == 18.0f);
    for (const Float3& v : c) {
        CHECK(v.z == 30.0f);
    }
    // Triangles 0 1 2 and 0 2 3 cover the square without overlap: the two diagonals' midpoints coincide.
    CHECK((c[0].x + c[2].x) / 2 == (c[1].x + c[3].x) / 2);
}

TEST_CASE("extract particles: colors round to D3DCOLOR bytes, white without a color array") {
    const Float4 white = ni::ParticleColor(nullptr);
    CHECK(white.x == 1.0f);
    CHECK(white.w == 1.0f);
    const float rgba[4] = {0.5f, 0.0f, 1.0f, 0.25f};
    const Float4 c = ni::ParticleColor(rgba);
    CHECK(c.x == 128.0f / 255.0f);  // 127.5 rounds to even: 128
    CHECK(c.y == 0.0f);
    CHECK(c.z == 1.0f);
    CHECK(c.w == 64.0f / 255.0f);  // 63.75 -> 64
}
