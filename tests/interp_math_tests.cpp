// Unit tests of gfx/interp_math.h (M3d renderer-level interpolation, analysis/loader/m3d_interpolation.md 3.3-3.7
// and 8): quaternions from NetImmerse rotation rows, slerp, the rigid interpolation with its fast paths, the snap
// rules with the measured cases, Inverse/Compose against FUN_0014D880/FUN_0010DFC0, camera cuts and particles.
#include "doctest.h"

#include "gfx/interp_math.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>

namespace {

using namespace gfx;
using namespace gfx::interp;

constexpr float kPi = 3.14159265358979f;

float Rad(float degrees) {
    return degrees * kPi / 180.0f;
}

// Rotation by `angle` radians about the unit axis (x, y, z), as NiTransform rows (standard matrix of the axis-angle
// rotation; any rotation matrix works for these tests).
ni::NiTransform AxisAngle(float x, float y, float z, float angle, float tx = 0.0f, float ty = 0.0f, float tz = 0.0f,
                          float scale = 1.0f) {
    const float n = std::sqrt(x * x + y * y + z * z);
    x /= n;
    y /= n;
    z /= n;
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    ni::NiTransform r{};
    r.rotate[0][0] = t * x * x + c;
    r.rotate[0][1] = t * x * y - s * z;
    r.rotate[0][2] = t * x * z + s * y;
    r.rotate[1][0] = t * x * y + s * z;
    r.rotate[1][1] = t * y * y + c;
    r.rotate[1][2] = t * y * z - s * x;
    r.rotate[2][0] = t * x * z - s * y;
    r.rotate[2][1] = t * y * z + s * x;
    r.rotate[2][2] = t * z * z + c;
    r.translate[0] = tx;
    r.translate[1] = ty;
    r.translate[2] = tz;
    r.scale = scale;
    return r;
}

float MaxRowDifference(const float a[3][4], const float b[3][4]) {
    float e = 0.0f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            e = std::fmax(e, std::fabs(a[i][j] - b[i][j]));
        }
    }
    return e;
}

}  // namespace

TEST_CASE("interp math: quaternion round trip for random rotations and every Shepperd branch") {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float worst = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        const float x = u(rng), y = u(rng), z = u(rng);
        if (x * x + y * y + z * z < 1e-3f) {
            continue;
        }
        const ni::NiTransform t = AxisAngle(x, y, z, u(rng) * kPi);
        float back[3][4];
        RowsFromQuat(QuatFromRows(t.rotate), back);
        worst = std::fmax(worst, MaxRowDifference(t.rotate, back));
        CHECK(back[0][3] == 0.0f);
        CHECK(back[1][3] == 0.0f);
        CHECK(back[2][3] == 0.0f);
    }
    CHECK(worst < 2e-6f);
    // Trace <= 0: the x, y and z branches (180 degrees about each axis and about diagonals).
    for (const auto& axis : {std::array<float, 3>{1, 0, 0}, std::array<float, 3>{0, 1, 0},
                             std::array<float, 3>{0, 0, 1}, std::array<float, 3>{1, 1, 0},
                             std::array<float, 3>{0, 1, 1}, std::array<float, 3>{1, 0.2f, 0.9f}}) {
        for (const float degrees : {180.0f, 179.0f, 150.0f}) {
            const ni::NiTransform t = AxisAngle(axis[0], axis[1], axis[2], Rad(degrees));
            CHECK(t.rotate[0][0] + t.rotate[1][1] + t.rotate[2][2] <= 0.0f);
            float back[3][4];
            const Quat q = QuatFromRows(t.rotate);
            CHECK(std::fabs(Dot(q, q) - 1.0f) < 1e-6f);
            RowsFromQuat(q, back);
            CHECK(MaxRowDifference(t.rotate, back) < 2e-6f);
        }
    }
    // Identity -> (1, 0, 0, 0).
    const Quat id = QuatFromRows(IdentityTransform().rotate);
    CHECK(id.w == doctest::Approx(1.0f));
    CHECK(id.x == doctest::Approx(0.0f));
}

TEST_CASE("interp math: slerp endpoints, shortest path, nlerp continuity, 179 degrees stays on its side") {
    const Quat a = QuatFromRows(AxisAngle(0, 0, 1, Rad(10)).rotate);
    const Quat b = QuatFromRows(AxisAngle(0, 0, 1, Rad(70)).rotate);
    CHECK(AngleDegBetween(Slerp(a, b, 0.0f), a) < 1e-3f);
    CHECK(AngleDegBetween(Slerp(a, b, 1.0f), b) < 1e-3f);
    // Midpoint = 40 degrees about z.
    CHECK(AngleDegBetween(Slerp(a, b, 0.5f), QuatFromRows(AxisAngle(0, 0, 1, Rad(40)).rotate)) < 1e-3f);
    // q and -q are the same rotation: the result does not take the long way.
    const Quat negB{-b.w, -b.x, -b.y, -b.z};
    CHECK(AngleDegBetween(Slerp(a, negB, 0.5f), QuatFromRows(AxisAngle(0, 0, 1, Rad(40)).rotate)) < 1e-3f);
    // Continuity at the nlerp threshold (cos 0.9995 ~ 3.6 degrees apart): angles just below/above interpolate to
    // the true half angle within a tiny error.
    for (const float degrees : {3.5f, 3.6f, 3.7f, 3.8f}) {
        const Quat p = QuatFromRows(IdentityTransform().rotate);
        const Quat q = QuatFromRows(AxisAngle(1, 1, 0, Rad(degrees)).rotate);
        CHECK(AngleDegBetween(Slerp(p, q, 0.5f), p) == doctest::Approx(degrees / 2).epsilon(1e-3));
    }
    // 179 degrees: the middle is 89.5 degrees on the 179-degree side, not 90.5 the other way round.
    const Quat p = QuatFromRows(IdentityTransform().rotate);
    const Quat q = QuatFromRows(AxisAngle(0, 1, 0, Rad(179)).rotate);
    const Quat mid = Slerp(p, q, 0.5f);
    CHECK(AngleDegBetween(mid, QuatFromRows(AxisAngle(0, 1, 0, Rad(89.5f)).rotate)) < 1e-2f);
    CHECK(AngleDegBetween(p, q) == doctest::Approx(179.0f).epsilon(1e-4));
}

TEST_CASE("interp math: InterpolateTransform fast paths are bit-identical and the midpoint is exact") {
    const ni::NiTransform a = AxisAngle(0.3f, 0.8f, 0.1f, 0.7f, 1, 2, 3, 1.5f);
    ni::NiTransform out{};
    SnapReason reason = SnapReason::None;
    // Equal inputs: Static, *out = cur bit for bit (the quaternion round trip would differ by ~1e-7).
    CHECK(InterpolateTransform(a, a, 0.5f, &out, &reason));
    CHECK(reason == SnapReason::Static);
    CHECK(BitwiseEqual(out, a));
    // t = 1 (and 1 - 1e-4): AlphaOne, cur as is.
    const ni::NiTransform b = AxisAngle(0.3f, 0.8f, 0.1f, 0.9f, 3, 2, 1, 1.6f);
    CHECK(InterpolateTransform(a, b, 1.0f, &out, &reason));
    CHECK(reason == SnapReason::AlphaOne);
    CHECK(BitwiseEqual(out, b));
    CHECK(InterpolateTransform(a, b, 0.9999f, &out, &reason));
    CHECK(reason == SnapReason::AlphaOne);
    // Midpoint: translation, scale linear, rotation half way about the common axis.
    CHECK(InterpolateTransform(a, b, 0.5f, &out, &reason));
    CHECK(reason == SnapReason::None);
    CHECK(out.translate[0] == doctest::Approx(2.0f));
    CHECK(out.translate[1] == doctest::Approx(2.0f));
    CHECK(out.translate[2] == doctest::Approx(2.0f));
    CHECK(out.scale == doctest::Approx(1.55f));
    const ni::NiTransform expected = AxisAngle(0.3f, 0.8f, 0.1f, 0.8f);
    CHECK(MaxRowDifference(out.rotate, expected.rotate) < 2e-6f);
    CHECK(OrthonormalError(out.rotate) < 1e-6f);
    // t = 0: the previous state.
    CHECK(InterpolateTransform(a, b, 0.0f, &out, &reason));
    CHECK(MaxRowDifference(out.rotate, a.rotate) < 2e-6f);
    CHECK(out.translate[0] == doctest::Approx(1.0f));
}

TEST_CASE("interp math: matrix checks snap mirrored, non-orthonormal and rescaled transforms") {
    const ni::NiTransform a = AxisAngle(0, 0, 1, 0.2f);
    ni::NiTransform out{};
    SnapReason reason = SnapReason::None;
    // Determinant < 0 (never measured on the disc): no quaternion -> Mirror.
    ni::NiTransform mirrored = AxisAngle(0, 0, 1, 0.3f);
    for (int c = 0; c < 3; ++c) {
        mirrored.rotate[0][c] = -mirrored.rotate[0][c];
    }
    CHECK(Determinant3(mirrored.rotate) < 0.0f);
    CHECK_FALSE(InterpolateTransform(a, mirrored, 0.5f, &out, &reason));
    CHECK(reason == SnapReason::Mirror);
    CHECK(BitwiseEqual(out, mirrored));
    // A sheared matrix.
    ni::NiTransform sheared = AxisAngle(0, 0, 1, 0.3f);
    sheared.rotate[0][1] += 0.01f;
    CHECK(OrthonormalError(sheared.rotate) > 1e-3f);
    CHECK_FALSE(InterpolateTransform(a, sheared, 0.5f, &out, &reason));
    CHECK(reason == SnapReason::NotOrthonormal);
    // Measured world rotations are orthonormal to 8.9e-7: such an error is accepted.
    ni::NiTransform noisy = AxisAngle(0, 0, 1, 0.3f);
    noisy.rotate[1][2] += 9e-7f;
    CHECK(InterpolateTransform(a, noisy, 0.5f, &out, &reason));
    CHECK(reason == SnapReason::None);
    // Scale x2.5 per tick -> Scale; x1.33 (measured maximum) interpolates; a sign change snaps.
    ni::NiTransform big = AxisAngle(0, 0, 1, 0.2f, 0, 0, 0, 2.5f);
    CHECK_FALSE(InterpolateTransform(a, big, 0.5f, &out, &reason));
    CHECK(reason == SnapReason::Scale);
    ni::NiTransform grown = AxisAngle(0, 0, 1, 0.2f, 0, 0, 0, 1.33f);
    CHECK(InterpolateTransform(a, grown, 0.5f, &out, &reason));
    CHECK(out.scale == doctest::Approx(1.165f));
    ni::NiTransform negative = AxisAngle(0, 0, 1, 0.2f, 0, 0, 0, -1.0f);
    CHECK_FALSE(InterpolateTransform(a, negative, 0.5f, &out, &reason));
    CHECK(reason == SnapReason::Scale);
}

TEST_CASE("interp math: snap rule R2/R4 with the measured cases of the analysis (3.4)") {
    const SnapLimits limits;
    // Keyframe loop wrap: 4.6 units after 0.095 at radius 0.19 -> Translation.
    CHECK(MotionSnap(4.6f, 0.0f, 0.19f, 0.095f, 0.0f, limits) == SnapReason::Translation);
    // Fast small object 0x033811D0: 0.17 units per tick (4.2 radii) after 0.16 -> interpolate.
    CHECK(MotionSnap(0.17f, 1.0f, 0.04f, 0.16f, 1.0f, limits) == SnapReason::None);
    // Sustained 23.3 degrees per tick (frontend fly-in) -> interpolate.
    CHECK(MotionSnap(0.96f, 23.3f, 0.5f, 0.96f, 23.3f, limits) == SnapReason::None);
    // Accelerating spin 72 -> 144 degrees per tick (loading screen) -> interpolate (144 < 4 * 72 + 1).
    CHECK(MotionSnap(0.0f, 144.0f, 1.0f, 0.0f, 72.0f, limits) == SnapReason::None);
    // 2 -> 115 degrees (loop wrap rotation) -> Rotation; 179 after 0.75 -> Rotation.
    CHECK(MotionSnap(0.0f, 115.0f, 1.0f, 0.0f, 2.0f, limits) == SnapReason::Rotation);
    CHECK(MotionSnap(0.0f, 179.0f, 1.0f, 0.0f, 0.75f, limits) == SnapReason::Rotation);
    // A jump from rest: 2.18 units after 0 at radius 0.5 -> Translation; small steps never snap.
    CHECK(MotionSnap(2.18f, 0.0f, 0.5f, 0.0f, 0.0f, limits) == SnapReason::Translation);
    CHECK(MotionSnap(0.9f, 0.0f, 0.5f, 0.0f, 0.0f, limits) == SnapReason::None);  // < 2 radii
    // 40 degrees from rest stays below 45 -> interpolate.
    CHECK(MotionSnap(0.0f, 40.0f, 1.0f, 0.0f, 0.0f, limits) == SnapReason::None);
    // Degenerate radius: the floor keeps the growth test meaningful.
    CHECK(MotionSnap(0.5f, 0.0f, 0.0f, 0.4f, 0.0f, limits) == SnapReason::None);
    CHECK(MotionSnap(0.5f, 0.0f, 0.0f, 0.0f, 0.0f, limits) == SnapReason::Translation);
}

TEST_CASE("interp math: AnalyzeMotion step and angle") {
    const ni::NiTransform a = AxisAngle(1, 0, 0, Rad(10), 0, 0, 0);
    const ni::NiTransform b = AxisAngle(1, 0, 0, Rad(33.3f), 3, 4, 0);
    const MotionInfo m = AnalyzeMotion(a, b);
    CHECK(m.check == SnapReason::None);
    CHECK(m.step == doctest::Approx(5.0f));
    CHECK(m.angleDeg == doctest::Approx(23.3f).epsilon(1e-4));
    // Small angles are accurate (atan2 form): 0.75 degrees.
    const MotionInfo small = AnalyzeMotion(a, AxisAngle(1, 0, 0, Rad(10.75f)));
    CHECK(small.angleDeg == doctest::Approx(0.75f).epsilon(1e-3));
}

TEST_CASE("interp math: Inverse and Compose follow FUN_0014D880 / FUN_0010DFC0") {
    const ni::NiTransform a = AxisAngle(0.2f, 0.5f, 0.9f, 1.1f, 1.0f, -2.0f, 0.5f, 2.0f);
    const ni::NiTransform b = AxisAngle(-0.7f, 0.1f, 0.3f, -0.4f, 0.25f, 3.0f, -1.0f, 0.5f);
    const float v[3] = {0.3f, -1.2f, 2.5f};
    // Compose(a, b)(v) = a(b(v)).
    float bv[3], abv[3], composed[3];
    TransformPoint(b, v, bv);
    TransformPoint(a, bv, abv);
    TransformPoint(Compose(a, b), v, composed);
    for (int c = 0; c < 3; ++c) {
        CHECK(composed[c] == doctest::Approx(abv[c]).epsilon(1e-5));
    }
    // Hand-computed per FUN_0010DFC0: translation = (a[0] b.t.x + a[4] b.t.y + a[8] b.t.z) * a.s + a.t.x, scale =
    // a.s * b.s, rotation row 0 = b.row0 * a (FUN_00013B30: a[0] * b[0] + a[4] * b[1] + a[8] * b[2]).
    const ni::NiTransform ab = Compose(a, b);
    const float tx = (a.rotate[0][0] * b.translate[0] + a.rotate[1][0] * b.translate[1] +
                      a.rotate[2][0] * b.translate[2]) * a.scale + a.translate[0];
    CHECK(ab.translate[0] == doctest::Approx(tx));
    CHECK(ab.scale == doctest::Approx(1.0f));
    CHECK(ab.rotate[0][0] == doctest::Approx(a.rotate[0][0] * b.rotate[0][0] + a.rotate[1][0] * b.rotate[0][1] +
                                             a.rotate[2][0] * b.rotate[0][2]));
    CHECK(ab.rotate[0][3] == 0.0f);
    // Inverse per FUN_0014D880: R^T, 1/s, t' = -t R^T / s; Inverse(a)(a(v)) = v and Compose(a, Inverse(a)) = I.
    const ni::NiTransform inv = Inverse(a);
    CHECK(inv.scale == doctest::Approx(0.5f));
    CHECK(inv.rotate[0][1] == a.rotate[1][0]);
    CHECK(inv.translate[0] == doctest::Approx(-(a.translate[0] * a.rotate[0][0] + a.translate[1] * a.rotate[0][1] +
                                                a.translate[2] * a.rotate[0][2]) / a.scale));
    float av[3], back[3];
    TransformPoint(a, v, av);
    TransformPoint(inv, av, back);
    for (int c = 0; c < 3; ++c) {
        CHECK(back[c] == doctest::Approx(v[c]).epsilon(1e-5));
    }
    const ni::NiTransform id = Compose(a, inv);
    CHECK(MaxRowDifference(id.rotate, IdentityTransform().rotate) < 1e-6f);
    CHECK(id.scale == doctest::Approx(1.0f));
    for (int c = 0; c < 3; ++c) {
        CHECK(std::fabs(id.translate[c]) < 1e-5f);
    }
    // The substitute skin path (3.8): RootToSkin * (root^-1 * bone) * boneData equals the original
    // ((RootToSkin * root^-1) * bone) * boneData up to rounding.
    const ni::NiTransform rootToSkin = AxisAngle(0, 1, 0, 0.3f, 0.1f, 0.2f, 0.3f, 1.0f);
    const ni::NiTransform root = AxisAngle(1, 1, 0, 0.8f, 5, 6, 7, 1.2f);
    const ni::NiTransform bone = AxisAngle(0, 0, 1, -0.6f, 5.5f, 6.1f, 7.3f, 1.2f);
    const ni::NiTransform boneData = AxisAngle(1, 0, 1, 0.1f, -0.5f, 0.0f, 0.2f, 1.0f);
    const ni::NiTransform original = Compose(Compose(Compose(rootToSkin, Inverse(root)), bone), boneData);
    const ni::NiTransform substitute =
        Compose(Compose(Compose(rootToSkin, Inverse(IdentityTransform())), Compose(Inverse(root), bone)), boneData);
    CHECK(MaxRowDifference(original.rotate, substitute.rotate) < 1e-5f);
    for (int c = 0; c < 3; ++c) {
        CHECK(original.translate[c] == doctest::Approx(substitute.translate[c]).epsilon(1e-5));
    }
    CHECK(original.scale == doctest::Approx(substitute.scale));
}

TEST_CASE("interp math: camera interpolation and cut rule (3.7)") {
    CameraState a;
    a.frustum = ni::NiFrustum{-0.7714f, 0.7714f, 0.5786f, -0.5786f, 0.1f, 1000.0f};
    a.port = ni::NiRectF{0.0f, 1.0f, 1.0f, 0.0f};
    CameraState b = a;
    b.location[0] = 1.0f;
    // Turn 10 degrees about the up axis: rows dir/up/right of the rotated frame.
    const ni::NiTransform r = AxisAngle(0, 1, 0, Rad(10));
    for (int i = 0; i < 3; ++i) {
        b.direction[i] = r.rotate[0][i];
        b.up[i] = r.rotate[1][i];
        b.right[i] = r.rotate[2][i];
    }
    const CameraMotion m = AnalyzeCamera(a, b);
    CHECK(m.check == SnapReason::None);
    CHECK(m.step == doctest::Approx(1.0f));
    CHECK(m.angleDeg == doctest::Approx(10.0f).epsilon(1e-4));
    CameraState out;
    BlendCameras(a, b, m, 0.5f, &out);
    CHECK(out.location[0] == doctest::Approx(0.5f));
    const ni::NiTransform half = AxisAngle(0, 1, 0, Rad(5));
    for (int i = 0; i < 3; ++i) {
        CHECK(out.direction[i] == doctest::Approx(half.rotate[0][i]).epsilon(1e-5));
        CHECK(out.right[i] == doctest::Approx(half.rotate[2][i]).epsilon(1e-5));
    }
    CHECK(std::memcmp(&out.port, &b.port, sizeof(out.port)) == 0);
    CHECK(out.frustum.nearPlane == b.frustum.nearPlane);
    // Cut rule: a steady 1 unit/tick pan is no cut, a 10-unit jump after 0.1 is; a 30-degree turn after 1 is.
    CHECK(CameraCutRule(1.0f, 10.0f, 1.0f, 10.0f) == SnapReason::None);
    CHECK(CameraCutRule(10.0f, 0.0f, 0.1f, 0.0f) == SnapReason::CameraCut);
    CHECK(CameraCutRule(0.4f, 0.0f, 0.0f, 0.0f) == SnapReason::None);  // below 0.5 units
    CHECK(CameraCutRule(0.0f, 30.0f, 0.0f, 1.0f) == SnapReason::CameraCut);
    CHECK(CameraCutRule(0.0f, 15.0f, 0.0f, 1.0f) == SnapReason::None);  // below 20 degrees
    // Zoom by more than 1.25, near/far or port changes are always cuts.
    CameraState zoom = a;
    zoom.frustum.left *= 0.7f;
    zoom.frustum.right *= 0.7f;
    CHECK(AnalyzeCamera(a, zoom).check == SnapReason::CameraCut);
    CameraState slowZoom = a;
    slowZoom.frustum.left *= 0.9f;
    slowZoom.frustum.right *= 0.9f;
    CHECK(AnalyzeCamera(a, slowZoom).check == SnapReason::None);
    CameraState nearChanged = a;
    nearChanged.frustum.nearPlane = 1.0f;
    CHECK(AnalyzeCamera(a, nearChanged).check == SnapReason::CameraCut);
    CameraState portChanged = a;
    portChanged.port.right = 0.5f;
    CHECK(AnalyzeCamera(a, portChanged).check == SnapReason::CameraCut);
}

TEST_CASE("interp math: particles interpolate with stable indices and snap on respawns or count changes") {
    const float before[6] = {0, 0, 0, 10, 0, 0};
    const float prev[6] = {1, 0, 0, 11, 0, 0};
    const float cur[6] = {2, 0, 0, 12, 0, 0};
    float out[6] = {};
    CHECK(InterpolatePoints(before, prev, cur, 2, 2, 0.5f, 0.01f, out));
    CHECK(out[0] == doctest::Approx(1.5f));
    CHECK(out[3] == doctest::Approx(11.5f));
    // Particle 1 respawns far away: the whole system snaps (out = cur).
    const float respawn[6] = {2, 0, 0, -50, 0, 0};
    CHECK_FALSE(InterpolatePoints(before, prev, respawn, 2, 2, 0.5f, 0.01f, out));
    CHECK(out[3] == -50.0f);
    // Count changed: snap.
    CHECK_FALSE(InterpolatePoints(nullptr, prev, cur, 1, 2, 0.5f, 0.01f, out));
    CHECK(out[0] == 2.0f);
    // Without G-2 only the count is checked.
    CHECK(InterpolatePoints(nullptr, prev, respawn, 2, 2, 0.5f, 0.01f, out));
    CHECK(out[3] == doctest::Approx(-19.5f));
}

TEST_CASE("interp math: snap reason names and the IsSnap classification") {
    for (std::size_t r = 0; r < kSnapReasonCount; ++r) {
        CHECK(std::string(SnapReasonName(static_cast<SnapReason>(r))) != "?");
    }
    CHECK_FALSE(IsSnap(SnapReason::None));
    CHECK_FALSE(IsSnap(SnapReason::Static));
    CHECK_FALSE(IsSnap(SnapReason::AlphaOne));
    CHECK_FALSE(IsSnap(SnapReason::Disabled));
    CHECK(IsSnap(SnapReason::FirstSight));
    CHECK(IsSnap(SnapReason::Translation));
    CHECK(IsSnap(SnapReason::CameraCut));
}
