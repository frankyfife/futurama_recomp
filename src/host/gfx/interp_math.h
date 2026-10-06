// Renderer-level interpolation (M3d) - pure math, header-only (analysis/loader/m3d_interpolation.md 3.3-3.7,
// 6.2). Owned by the interpolation agent; tests/interp_math_tests.cpp.
//
// Conventions (GFX_CONTRACT.md 3, gfx/ni_guest_layouts.h): an ni::NiTransform maps a point v to
// s * v * R + t (row vectors); R = rotate[0..2][0..2], the rows are the images of the x/y/z axes, column 3 is
// padding. World rotations of the game are orthonormal (error <= 8.9e-7 measured) and never mirrored (0 of
// 1.4 million draws, 0 of 105 233 local transforms on the disc), so a rotation interpolates as a quaternion
// slerp without a polar-decomposition path; anything else snaps (SnapReason::Mirror / NotOrthonormal).
//
// The quaternion round trip matrix -> quaternion -> matrix is not bit-exact (2.4e-7). Callers therefore pass
// the guest data through unchanged when nothing has to be computed: alpha >= 1 - kAlphaOneEpsilon (AlphaOne)
// and bitwise equal transforms (Static) never touch the matrix.
#pragma once

#include "gfx/ni_guest_layouts.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gfx::interp {

// Why a draw (or a camera) shows the current tick state instead of an interpolated one. None = interpolated.
enum class SnapReason : std::uint8_t {
    None,            // interpolated between S[G-1] and S[G]
    Disabled,        // interpolation off or not active in this frame (pass-through; statistics only)
    AlphaOne,        // alpha >= 1 - kAlphaOneEpsilon: the guest data as is (zero-step frames at 120 Hz)
    Static,          // previous and current transform bitwise equal: the guest data as is
    FirstSight,      // no capture of this key in tick G-1
    GenerationGap,   // the last capture is older than G-1 (culled, invisible, catch-up of >= 2 ticks)
    Identity,        // signature changed (vtable, data, skin instance, property state): another object
    Destroyed,       // NiAVObject::~NiAVObject ran on this address since the previous capture
    Translation,     // rule R2: step > 2 bound radii and > 4x the previous step (+ 0.01 r)
    Rotation,        // rule R4: angle > 45 deg and > 4x the previous angle + 1 deg
    Scale,           // scale ratio outside [1/2, 2] or a sign change of the scale
    Mirror,          // determinant <= 0 (never measured; no quaternion exists)
    NotOrthonormal,  // orthonormality error > 1e-3 (never measured)
    FrameDriven,     // the key changed within one tick generation (animated with g_frameDelta in Render)
    SnapAll,         // the frame clock's snapAll (context switch, catch-up, cap, clock reset)
    CameraCut,       // camera rule of 3.7 (jump, sudden turn, zoom, near/far/port change)
    BoneCount,       // skinned draw: bone count changed or above kMaxBones (skin keeps the tick pose)
    kCount
};
inline constexpr std::size_t kSnapReasonCount = static_cast<std::size_t>(SnapReason::kCount);

inline const char* SnapReasonName(SnapReason reason) noexcept {
    switch (reason) {
    case SnapReason::None:
        return "interpolated";
    case SnapReason::Disabled:
        return "disabled";
    case SnapReason::AlphaOne:
        return "alpha one";
    case SnapReason::Static:
        return "static";
    case SnapReason::FirstSight:
        return "first sight";
    case SnapReason::GenerationGap:
        return "generation gap";
    case SnapReason::Identity:
        return "identity";
    case SnapReason::Destroyed:
        return "destroyed";
    case SnapReason::Translation:
        return "translation";
    case SnapReason::Rotation:
        return "rotation";
    case SnapReason::Scale:
        return "scale";
    case SnapReason::Mirror:
        return "mirror";
    case SnapReason::NotOrthonormal:
        return "not orthonormal";
    case SnapReason::FrameDriven:
        return "frame-driven";
    case SnapReason::SnapAll:
        return "snap-all";
    case SnapReason::CameraCut:
        return "camera cut";
    case SnapReason::BoneCount:
        return "bone count";
    case SnapReason::kCount:
        break;
    }
    return "?";
}

// Reasons that are a real "snap" (the key had history but shows the tick state); Disabled, AlphaOne and Static
// are pass-throughs without a decision.
inline constexpr bool IsSnap(SnapReason reason) noexcept {
    return reason != SnapReason::None && reason != SnapReason::Disabled && reason != SnapReason::AlphaOne &&
           reason != SnapReason::Static;
}

// alpha above 1 - kAlphaOneEpsilon is treated as exactly 1 (6.1: the clock's 0.999 of "align B").
inline constexpr float kAlphaOneEpsilon = 1e-3f;

// Thresholds of 3.4 (objects) and 3.7 (cameras). Constants, unit-tested; validated on the frontend only - the
// in-game values are checked with M4/M4b/M8 (no configuration keys).
struct SnapLimits {
    float stepRadii = 2.0f;           // R2: step > stepRadii * r ...
    float stepGrowth = 4.0f;          //     ... and step > stepGrowth * previous step + stepEpsilonRadii * r
    float stepEpsilonRadii = 0.01f;
    float angleDeg = 45.0f;           // R4: angle > angleDeg ...
    float angleGrowth = 4.0f;         //     ... and angle > angleGrowth * previous angle + angleEpsilonDeg
    float angleEpsilonDeg = 1.0f;
    float scaleRatio = 2.0f;          // scale ratio per tick outside [1 / scaleRatio, scaleRatio]
    float orthoTolerance = 1e-3f;     // max |row_i . row_j - delta_ij|
    float minRadius = 1e-3f;          // radius floor (degenerate bounds)
    float cameraStep = 0.5f;          // camera: step > 4x previous + cameraStepEpsilon AND > cameraStep units
    float cameraStepEpsilon = 0.05f;
    float cameraAngleDeg = 20.0f;     // camera: angle > 4x previous + cameraAngleEpsilonDeg AND > cameraAngleDeg
    float cameraAngleEpsilonDeg = 2.0f;
    float cameraGrowth = 4.0f;
    float frustumRatio = 1.25f;       // camera: a frustum tangent changes by more than this factor (zoom cut)
    float particleGrowth = 4.0f;      // particles: a particle moves more than 4x its previous step (+ minStep)
    std::uint32_t frameDrivenHoldFrames = 120;
};

// ---- vectors and quaternions --------------------------------------------------------------------------------

struct Quat {
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
};

inline float Dot(const Quat& a, const Quat& b) noexcept {
    return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Quat Normalized(Quat q) noexcept {
    const float n2 = Dot(q, q);
    if (!(n2 > 0.0f)) {
        return Quat{};
    }
    const float inv = 1.0f / std::sqrt(n2);
    q.w *= inv;
    q.x *= inv;
    q.y *= inv;
    q.z *= inv;
    return q;
}

// Quaternion of the rotation whose matrix A has A[i][j] = rotate[i][j] (Shepperd: the branch of the largest of
// the trace and the diagonal elements). Any consistent matrix <-> quaternion mapping works for slerp; this one is
// the standard mapping of A (RowsFromQuat is its exact inverse up to rounding).
inline Quat QuatFromRows(const float rotate[3][4]) noexcept {
    const float m00 = rotate[0][0], m01 = rotate[0][1], m02 = rotate[0][2];
    const float m10 = rotate[1][0], m11 = rotate[1][1], m12 = rotate[1][2];
    const float m20 = rotate[2][0], m21 = rotate[2][1], m22 = rotate[2][2];
    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;  // 4w
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(std::max(1.0f + m00 - m11 - m22, 0.0f)) * 2.0f;  // 4x
        if (!(s > 0.0f)) {
            return Quat{};
        }
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(std::max(1.0f + m11 - m00 - m22, 0.0f)) * 2.0f;  // 4y
        if (!(s > 0.0f)) {
            return Quat{};
        }
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(std::max(1.0f + m22 - m00 - m11, 0.0f)) * 2.0f;  // 4z
        if (!(s > 0.0f)) {
            return Quat{};
        }
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return Normalized(q);
}

// Rows of the rotation of a unit quaternion (inverse of QuatFromRows); column 3 = 0.
inline void RowsFromQuat(const Quat& q, float rotate[3][4]) noexcept {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    rotate[0][0] = 1.0f - 2.0f * (yy + zz);
    rotate[0][1] = 2.0f * (xy - wz);
    rotate[0][2] = 2.0f * (xz + wy);
    rotate[0][3] = 0.0f;
    rotate[1][0] = 2.0f * (xy + wz);
    rotate[1][1] = 1.0f - 2.0f * (xx + zz);
    rotate[1][2] = 2.0f * (yz - wx);
    rotate[1][3] = 0.0f;
    rotate[2][0] = 2.0f * (xz - wy);
    rotate[2][1] = 2.0f * (yz + wx);
    rotate[2][2] = 1.0f - 2.0f * (xx + yy);
    rotate[2][3] = 0.0f;
}

// q flipped to the hemisphere of `reference` (q and -q are the same rotation; slerp takes the short way).
inline Quat AlignedTo(const Quat& reference, Quat q) noexcept {
    if (Dot(reference, q) < 0.0f) {
        q.w = -q.w;
        q.x = -q.x;
        q.y = -q.y;
        q.z = -q.z;
    }
    return q;
}

// Rotation angle between two unit quaternions in degrees (0..180): 2 atan2(|v(conj(a) b)|, |w(conj(a) b)|),
// accurate for small angles too.
inline float AngleDegBetween(const Quat& a, const Quat& b) noexcept {
    const float w = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    const float x = a.w * b.x - a.x * b.w - a.y * b.z + a.z * b.y;
    const float y = a.w * b.y + a.x * b.z - a.y * b.w - a.z * b.x;
    const float z = a.w * b.z - a.x * b.y + a.y * b.x - a.z * b.w;
    const float v = std::sqrt(x * x + y * y + z * z);
    return 2.0f * std::atan2(v, std::fabs(w)) * (180.0f / 3.14159265358979f);
}

// Spherical linear interpolation along the shortest arc (b is aligned to a), nlerp when the quaternions are
// nearly parallel (cos > 0.9995); the result is normalized. t = 0 -> a, t = 1 -> b (up to sign).
inline Quat Slerp(const Quat& a, Quat b, float t) noexcept {
    b = AlignedTo(a, b);
    const float d = std::min(Dot(a, b), 1.0f);
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        const float theta = std::acos(d);
        const float s = std::sin(theta);
        wa = std::sin((1.0f - t) * theta) / s;
        wb = std::sin(t * theta) / s;
    }
    Quat r;
    r.w = wa * a.w + wb * b.w;
    r.x = wa * a.x + wb * b.x;
    r.y = wa * a.y + wb * b.y;
    r.z = wa * a.z + wb * b.z;
    return Normalized(r);
}

// ---- matrix checks --------------------------------------------------------------------------------------------

inline float Determinant3(const float r[3][4]) noexcept {
    return r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
           r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
}

// max |row_i . row_j - delta_ij| over the 3x3 part.
inline float OrthonormalError(const float r[3][4]) noexcept {
    float e = 0.0f;
    for (int i = 0; i < 3; ++i) {
        for (int j = i; j < 3; ++j) {
            const float d = r[i][0] * r[j][0] + r[i][1] * r[j][1] + r[i][2] * r[j][2];
            e = std::max(e, std::fabs(d - (i == j ? 1.0f : 0.0f)));
        }
    }
    return e;
}

// Angle between two rotation matrices in degrees (via their quaternions).
inline float AngleDeg(const float a[3][4], const float b[3][4]) noexcept {
    return AngleDegBetween(QuatFromRows(a), QuatFromRows(b));
}

inline bool BitwiseEqual(const ni::NiTransform& a, const ni::NiTransform& b) noexcept {
    return std::memcmp(&a, &b, sizeof(ni::NiTransform)) == 0;
}

inline ni::NiTransform IdentityTransform() noexcept {
    ni::NiTransform t{};
    t.rotate[0][0] = 1.0f;
    t.rotate[1][1] = 1.0f;
    t.rotate[2][2] = 1.0f;
    t.scale = 1.0f;
    return t;
}

// ---- NetImmerse transform algebra --------------------------------------------------------------------------

// s * v * R + t (NiTriShape::vf33 0x1166D0: out.x = t.x + s * (m[0] x + m[4] y + m[8] z)).
inline void TransformPoint(const ni::NiTransform& t, const float in[3], float out[3]) noexcept {
    const float x = in[0], y = in[1], z = in[2];
    for (int c = 0; c < 3; ++c) {
        out[c] = t.translate[c] + t.scale * (x * t.rotate[0][c] + y * t.rotate[1][c] + z * t.rotate[2][c]);
    }
}

// FUN_0014D880 (NiTransform::Invert): R' = R^T (FUN_00103BD0, padding 0), s' = 1 / s, t' = -t R^T / s.
inline ni::NiTransform Inverse(const ni::NiTransform& t) noexcept {
    ni::NiTransform r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.rotate[i][j] = t.rotate[j][i];
        }
        r.rotate[i][3] = 0.0f;
    }
    const float inv = 1.0f / t.scale;
    r.scale = inv;
    const float nx = -t.translate[0], ny = -t.translate[1], nz = -t.translate[2];
    r.translate[0] = (nx * r.rotate[0][0] + ny * r.rotate[1][0] + nz * r.rotate[2][0]) * inv;
    r.translate[1] = (nx * r.rotate[0][1] + ny * r.rotate[1][1] + nz * r.rotate[2][1]) * inv;
    r.translate[2] = (nx * r.rotate[0][2] + ny * r.rotate[1][2] + nz * r.rotate[2][2]) * inv;
    return r;
}

// FUN_0010DFC0 (thiscall: this = a, result = a * b): the transform that applies b first, then a -
// Compose(a, b)(v) = a(b(v)). Rotation = b.R * a.R (FUN_00013B30: out row i = b.row_i * a), scale = a.s * b.s,
// translation = a.s * (b.t * a.R) + a.t.
inline ni::NiTransform Compose(const ni::NiTransform& a, const ni::NiTransform& b) noexcept {
    ni::NiTransform r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.rotate[i][j] = b.rotate[i][0] * a.rotate[0][j] + b.rotate[i][1] * a.rotate[1][j] +
                             b.rotate[i][2] * a.rotate[2][j];
        }
        r.rotate[i][3] = 0.0f;
    }
    r.scale = a.scale * b.scale;
    for (int c = 0; c < 3; ++c) {
        r.translate[c] = (b.translate[0] * a.rotate[0][c] + b.translate[1] * a.rotate[1][c] +
                          b.translate[2] * a.rotate[2][c]) *
                             a.scale +
                         a.translate[c];
    }
    return r;
}

// ---- rigid interpolation -----------------------------------------------------------------------------------

// What AnalyzeMotion found out about a pair of transforms.
struct MotionInfo {
    float step = 0.0f;      // |t1 - t0| (world units)
    float angleDeg = 0.0f;  // rotation angle between R0 and R1
    Quat q0, q1;            // q1 aligned to q0 (valid when check == None)
    SnapReason check = SnapReason::None;  // Scale / Mirror / NotOrthonormal: the pair cannot be interpolated
};

inline MotionInfo AnalyzeMotion(const ni::NiTransform& prev, const ni::NiTransform& cur,
                                const SnapLimits& limits = {}) noexcept {
    MotionInfo m;
    const float dx = cur.translate[0] - prev.translate[0];
    const float dy = cur.translate[1] - prev.translate[1];
    const float dz = cur.translate[2] - prev.translate[2];
    m.step = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(Determinant3(prev.rotate) > 0.0f) || !(Determinant3(cur.rotate) > 0.0f)) {
        m.check = SnapReason::Mirror;
        return m;
    }
    if (!(OrthonormalError(prev.rotate) <= limits.orthoTolerance) ||
        !(OrthonormalError(cur.rotate) <= limits.orthoTolerance)) {
        m.check = SnapReason::NotOrthonormal;
        return m;
    }
    const float ratio = prev.scale != 0.0f ? cur.scale / prev.scale : 0.0f;
    if (!(ratio > 0.0f) || ratio > limits.scaleRatio || ratio < 1.0f / limits.scaleRatio) {
        m.check = SnapReason::Scale;
    }
    m.q0 = QuatFromRows(prev.rotate);
    m.q1 = AlignedTo(m.q0, QuatFromRows(cur.rotate));
    m.angleDeg = AngleDegBetween(m.q0, m.q1);
    if (!std::isfinite(m.step) || !std::isfinite(m.angleDeg)) {
        m.check = SnapReason::NotOrthonormal;
    }
    return m;
}

// Interpolated transform at t from an analyzed pair (check == None): translation and scale linear, rotation slerp.
inline void BlendTransforms(const ni::NiTransform& prev, const ni::NiTransform& cur, const MotionInfo& motion, float t,
                            ni::NiTransform* out) noexcept {
    RowsFromQuat(Slerp(motion.q0, motion.q1, t), out->rotate);
    for (int c = 0; c < 3; ++c) {
        out->translate[c] = prev.translate[c] + (cur.translate[c] - prev.translate[c]) * t;
    }
    out->scale = prev.scale + (cur.scale - prev.scale) * t;
}

// Rigid interpolation including the fast paths and the checks of 3.3 (no motion snap rule - see MotionSnap).
// Returns true with *out = the result (AlphaOne / Static: *out = cur, the caller passes the guest data through) or
// false with the snap reason (Mirror, NotOrthonormal, Scale; *out = cur).
inline bool InterpolateTransform(const ni::NiTransform& prev, const ni::NiTransform& cur, float t,
                                 ni::NiTransform* out, SnapReason* reason, const SnapLimits& limits = {}) noexcept {
    SnapReason r = SnapReason::None;
    bool ok = true;
    if (!(t < 1.0f - kAlphaOneEpsilon)) {
        r = SnapReason::AlphaOne;
        *out = cur;
    } else if (BitwiseEqual(prev, cur)) {
        r = SnapReason::Static;
        *out = cur;
    } else {
        const MotionInfo m = AnalyzeMotion(prev, cur, limits);
        if (m.check != SnapReason::None) {
            r = m.check;
            *out = cur;
            ok = false;
        } else {
            BlendTransforms(prev, cur, m, std::max(t, 0.0f), out);
        }
    }
    if (reason != nullptr) {
        *reason = r;
    }
    return ok;
}

// Motion snap rule of 3.4 for one tick: R2 (translation) or R4 (rotation). `previousStep`/`previousAngleDeg` =
// the motion of the tick before (0 when unknown), `radius` = the world bound radius.
inline SnapReason MotionSnap(float step, float angleDeg, float radius, float previousStep, float previousAngleDeg,
                             const SnapLimits& limits = {}) noexcept {
    const float r = std::max(radius, limits.minRadius);
    if (step > limits.stepRadii * r && step > limits.stepGrowth * previousStep + limits.stepEpsilonRadii * r) {
        return SnapReason::Translation;
    }
    if (angleDeg > limits.angleDeg && angleDeg > limits.angleGrowth * previousAngleDeg + limits.angleEpsilonDeg) {
        return SnapReason::Rotation;
    }
    return SnapReason::None;
}

// ---- camera ------------------------------------------------------------------------------------------------------

// The camera as SetCameraData (slot 47) receives it: location, the rows dir/up/right of the world rotation
// (NiCamera::vf39 0xFFDA0 copies +0x70/+0x80/+0x90 to +0x110/+0x11C/+0x128), frustum and port.
struct CameraState {
    float location[3] = {};
    float direction[3] = {1.0f, 0.0f, 0.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};
    float right[3] = {0.0f, 0.0f, 1.0f};
    ni::NiFrustum frustum{};
    ni::NiRectF port{};
};

inline bool BitwiseEqual(const CameraState& a, const CameraState& b) noexcept {
    return std::memcmp(&a, &b, sizeof(CameraState)) == 0;
}

inline void CameraRows(const CameraState& c, float rows[3][4]) noexcept {
    for (int i = 0; i < 3; ++i) {
        rows[0][i] = c.direction[i];
        rows[1][i] = c.up[i];
        rows[2][i] = c.right[i];
    }
    rows[0][3] = rows[1][3] = rows[2][3] = 0.0f;
}

struct CameraMotion {
    float step = 0.0f;
    float angleDeg = 0.0f;
    Quat q0, q1;  // q1 aligned to q0
    SnapReason check = SnapReason::None;  // Mirror / NotOrthonormal / CameraCut (frustum, near/far, port)
};

// Step, angle and the "always a cut" checks of 3.7 (frustum tangent ratio, near/far/port changed, invalid rows).
inline CameraMotion AnalyzeCamera(const CameraState& prev, const CameraState& cur, const SnapLimits& limits = {}) noexcept {
    CameraMotion m;
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = cur.location[i] - prev.location[i];
        d2 += d * d;
    }
    m.step = std::sqrt(d2);
    float r0[3][4], r1[3][4];
    CameraRows(prev, r0);
    CameraRows(cur, r1);
    if (!(Determinant3(r0) > 0.0f) || !(Determinant3(r1) > 0.0f)) {
        m.check = SnapReason::Mirror;
        return m;
    }
    if (!(OrthonormalError(r0) <= limits.orthoTolerance) || !(OrthonormalError(r1) <= limits.orthoTolerance)) {
        m.check = SnapReason::NotOrthonormal;
        return m;
    }
    m.q0 = QuatFromRows(r0);
    m.q1 = AlignedTo(m.q0, QuatFromRows(r1));
    m.angleDeg = AngleDegBetween(m.q0, m.q1);
    if (std::memcmp(&prev.port, &cur.port, sizeof(ni::NiRectF)) != 0 ||
        std::memcmp(&prev.frustum.nearPlane, &cur.frustum.nearPlane, sizeof(float)) != 0 ||
        std::memcmp(&prev.frustum.farPlane, &cur.frustum.farPlane, sizeof(float)) != 0) {
        m.check = SnapReason::CameraCut;
        return m;
    }
    const float tangents0[4] = {prev.frustum.left, prev.frustum.right, prev.frustum.top, prev.frustum.bottom};
    const float tangents1[4] = {cur.frustum.left, cur.frustum.right, cur.frustum.top, cur.frustum.bottom};
    for (int i = 0; i < 4; ++i) {
        const float a = tangents0[i], b = tangents1[i];
        if (a == b) {
            continue;
        }
        // A sign change or a tangent at 0 on one side only is a cut; otherwise compare the ratio.
        if (!(a * b > 0.0f)) {
            m.check = SnapReason::CameraCut;
            return m;
        }
        const float ratio = std::fabs(b / a);
        if (ratio > limits.frustumRatio || ratio < 1.0f / limits.frustumRatio) {
            m.check = SnapReason::CameraCut;
            return m;
        }
    }
    if (!std::isfinite(m.step) || !std::isfinite(m.angleDeg)) {
        m.check = SnapReason::NotOrthonormal;
    }
    return m;
}

// Cut rule of 3.7: (step > 4x previous + 0.05 AND > 0.5) OR (angle > 4x previous + 2 deg AND > 20 deg).
inline SnapReason CameraCutRule(float step, float angleDeg, float previousStep, float previousAngleDeg,
                                const SnapLimits& limits = {}) noexcept {
    if (step > limits.cameraGrowth * previousStep + limits.cameraStepEpsilon && step > limits.cameraStep) {
        return SnapReason::CameraCut;
    }
    if (angleDeg > limits.cameraGrowth * previousAngleDeg + limits.cameraAngleEpsilonDeg &&
        angleDeg > limits.cameraAngleDeg) {
        return SnapReason::CameraCut;
    }
    return SnapReason::None;
}

// Interpolated camera (check == None): location linear, rotation slerp of the rows (dir, up, right), frustum
// tangents l/r/t/b linear; near/far and the port of the current tick (they only change at cuts and drive the
// screen projection of slot 54).
inline void BlendCameras(const CameraState& prev, const CameraState& cur, const CameraMotion& motion, float t,
                         CameraState* out) noexcept {
    for (int i = 0; i < 3; ++i) {
        out->location[i] = prev.location[i] + (cur.location[i] - prev.location[i]) * t;
    }
    float rows[3][4];
    RowsFromQuat(Slerp(motion.q0, motion.q1, t), rows);
    for (int i = 0; i < 3; ++i) {
        out->direction[i] = rows[0][i];
        out->up[i] = rows[1][i];
        out->right[i] = rows[2][i];
    }
    out->frustum = cur.frustum;
    out->frustum.left = prev.frustum.left + (cur.frustum.left - prev.frustum.left) * t;
    out->frustum.right = prev.frustum.right + (cur.frustum.right - prev.frustum.right) * t;
    out->frustum.top = prev.frustum.top + (cur.frustum.top - prev.frustum.top) * t;
    out->frustum.bottom = prev.frustum.bottom + (cur.frustum.bottom - prev.frustum.bottom) * t;
    out->port = cur.port;
}

// ---- particles (3.10; P2) -----------------------------------------------------------------------------------------
//
// The disc has no particle systems (no NiParticleSystemController, analysis 2.2) and slot 52 is not drawn by the
// port yet (P2); the single-point NiParticles of FUN_0006DC60 interpolate rigidly through their obj+0x70. For
// particle arrays with stable indices this helper interpolates each point linearly; it snaps the whole system when
// the count differs or one particle moves more than particleGrowth x its previous step + minStep (a respawned
// index). `before` = positions of tick G-2 (nullptr = unknown: only the count is checked). Returns false (out =
// cur) on a snap. Arrays hold 3 floats per point.
inline bool InterpolatePoints(const float* before, const float* prev, const float* cur, std::uint32_t prevCount,
                              std::uint32_t curCount, float t, float minStep, float* out,
                              const SnapLimits& limits = {}) noexcept {
    const std::size_t n = static_cast<std::size_t>(curCount) * 3;
    bool ok = prevCount == curCount && prev != nullptr && cur != nullptr;
    if (ok && before != nullptr) {
        for (std::uint32_t i = 0; i < curCount && ok; ++i) {
            float s1 = 0.0f, s0 = 0.0f;
            for (int c = 0; c < 3; ++c) {
                const float d1 = cur[i * 3 + c] - prev[i * 3 + c];
                const float d0 = prev[i * 3 + c] - before[i * 3 + c];
                s1 += d1 * d1;
                s0 += d0 * d0;
            }
            ok = std::sqrt(s1) <= limits.particleGrowth * std::sqrt(s0) + minStep;
        }
    }
    if (!ok || !(t < 1.0f - kAlphaOneEpsilon)) {
        if (cur != nullptr && out != cur) {
            std::memcpy(out, cur, n * sizeof(float));
        }
        return ok;
    }
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = prev[i] + (cur[i] - prev[i]) * t;
    }
    return true;
}

}  // namespace gfx::interp
