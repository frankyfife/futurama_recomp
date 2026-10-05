// Small math types shared by the M3 graphics modules (GFX_CONTRACT.md 3): ni_extract (guest NetImmerse
// data -> DrawItem/CameraDesc), ff_pipeline (D3D8 fixed-function emulation) and the RHI users.
//
// Conventions (verified against the original NiXBoxRenderer, GFX_CONTRACT.md 4):
//   - Matrices are D3D style: row vectors, v' = v * M, stored row-major (m[row][col]), translation in
//     row 3. HLSL declares them `row_major` (gfx/shaders/ff_shared.h) and multiplies mul(v, M).
//   - NetImmerse world transforms become D3D world matrices exactly like FUN_001489A0 does it:
//     rows 0..2 = scale * NiMatrix3 rows (the 3x4 storage of NiTransform, 16-byte row stride),
//     row 3 = translation (NiTransformToWorld below).
//   - Clip space is D3D's: x, y in [-1, 1] (y up), z in [0, 1], left-handed view space (x right, y up,
//     z forward = camera direction).
// Shared skeleton header (phase 1): add-only, changes are logged in INTERFACES.md section 8.
#pragma once

#include <cmath>
#include <cstdint>

namespace gfx {

struct Float2 {
    float x = 0.0f, y = 0.0f;
};
struct Float3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
struct Float4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

// 4x4 matrix, row-major storage, row-vector convention (see above). Zero-initialized by default.
struct Matrix4 {
    float m[4][4] = {};
};
static_assert(sizeof(Float3) == 12 && sizeof(Float4) == 16 && sizeof(Matrix4) == 64);

inline constexpr Matrix4 IdentityMatrix() noexcept {
    Matrix4 r;
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
    return r;
}

// a * b (apply a first, then b - D3D row-vector order: world * view * projection).
inline Matrix4 Multiply(const Matrix4& a, const Matrix4& b) noexcept {
    Matrix4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        }
    }
    return r;
}

// Point (w = 1) through the upper 4x3 part: p * M (no perspective divide).
inline Float3 TransformPoint(const Float3& p, const Matrix4& t) noexcept {
    return {p.x * t.m[0][0] + p.y * t.m[1][0] + p.z * t.m[2][0] + t.m[3][0],
            p.x * t.m[0][1] + p.y * t.m[1][1] + p.z * t.m[2][1] + t.m[3][1],
            p.x * t.m[0][2] + p.y * t.m[1][2] + p.z * t.m[2][2] + t.m[3][2]};
}

// Direction (w = 0) through the upper 3x3 part.
inline Float3 TransformVector(const Float3& v, const Matrix4& t) noexcept {
    return {v.x * t.m[0][0] + v.y * t.m[1][0] + v.z * t.m[2][0],
            v.x * t.m[0][1] + v.y * t.m[1][1] + v.z * t.m[2][1],
            v.x * t.m[0][2] + v.y * t.m[1][2] + v.z * t.m[2][2]};
}

inline Float4 TransformFloat4(const Float4& v, const Matrix4& t) noexcept {
    return {v.x * t.m[0][0] + v.y * t.m[1][0] + v.z * t.m[2][0] + v.w * t.m[3][0],
            v.x * t.m[0][1] + v.y * t.m[1][1] + v.z * t.m[2][1] + v.w * t.m[3][1],
            v.x * t.m[0][2] + v.y * t.m[1][2] + v.z * t.m[2][2] + v.w * t.m[3][2],
            v.x * t.m[0][3] + v.y * t.m[1][3] + v.z * t.m[2][3] + v.w * t.m[3][3]};
}

inline Matrix4 Transpose(const Matrix4& a) noexcept {
    Matrix4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a.m[j][i];
        }
    }
    return r;
}

// General 4x4 inverse (cofactors). Returns false (and identity) for a singular matrix.
inline bool Inverse(const Matrix4& a, Matrix4* out) noexcept {
    const float* s = &a.m[0][0];
    float inv[16];
    inv[0] = s[5] * s[10] * s[15] - s[5] * s[11] * s[14] - s[9] * s[6] * s[15] + s[9] * s[7] * s[14] +
             s[13] * s[6] * s[11] - s[13] * s[7] * s[10];
    inv[4] = -s[4] * s[10] * s[15] + s[4] * s[11] * s[14] + s[8] * s[6] * s[15] - s[8] * s[7] * s[14] -
             s[12] * s[6] * s[11] + s[12] * s[7] * s[10];
    inv[8] = s[4] * s[9] * s[15] - s[4] * s[11] * s[13] - s[8] * s[5] * s[15] + s[8] * s[7] * s[13] +
             s[12] * s[5] * s[11] - s[12] * s[7] * s[9];
    inv[12] = -s[4] * s[9] * s[14] + s[4] * s[10] * s[13] + s[8] * s[5] * s[14] - s[8] * s[6] * s[13] -
              s[12] * s[5] * s[10] + s[12] * s[6] * s[9];
    inv[1] = -s[1] * s[10] * s[15] + s[1] * s[11] * s[14] + s[9] * s[2] * s[15] - s[9] * s[3] * s[14] -
             s[13] * s[2] * s[11] + s[13] * s[3] * s[10];
    inv[5] = s[0] * s[10] * s[15] - s[0] * s[11] * s[14] - s[8] * s[2] * s[15] + s[8] * s[3] * s[14] +
             s[12] * s[2] * s[11] - s[12] * s[3] * s[10];
    inv[9] = -s[0] * s[9] * s[15] + s[0] * s[11] * s[13] + s[8] * s[1] * s[15] - s[8] * s[3] * s[13] -
             s[12] * s[1] * s[11] + s[12] * s[3] * s[9];
    inv[13] = s[0] * s[9] * s[14] - s[0] * s[10] * s[13] - s[8] * s[1] * s[14] + s[8] * s[2] * s[13] +
              s[12] * s[1] * s[10] - s[12] * s[2] * s[9];
    inv[2] = s[1] * s[6] * s[15] - s[1] * s[7] * s[14] - s[5] * s[2] * s[15] + s[5] * s[3] * s[14] +
             s[13] * s[2] * s[7] - s[13] * s[3] * s[6];
    inv[6] = -s[0] * s[6] * s[15] + s[0] * s[7] * s[14] + s[4] * s[2] * s[15] - s[4] * s[3] * s[14] -
             s[12] * s[2] * s[7] + s[12] * s[3] * s[6];
    inv[10] = s[0] * s[5] * s[15] - s[0] * s[7] * s[13] - s[4] * s[1] * s[15] + s[4] * s[3] * s[13] +
              s[12] * s[1] * s[7] - s[12] * s[3] * s[5];
    inv[14] = -s[0] * s[5] * s[14] + s[0] * s[6] * s[13] + s[4] * s[1] * s[14] - s[4] * s[2] * s[13] -
              s[12] * s[1] * s[6] + s[12] * s[2] * s[5];
    inv[3] = -s[1] * s[6] * s[11] + s[1] * s[7] * s[10] + s[5] * s[2] * s[11] - s[5] * s[3] * s[10] -
             s[9] * s[2] * s[7] + s[9] * s[3] * s[6];
    inv[7] = s[0] * s[6] * s[11] - s[0] * s[7] * s[10] - s[4] * s[2] * s[11] + s[4] * s[3] * s[10] +
             s[8] * s[2] * s[7] - s[8] * s[3] * s[6];
    inv[11] = -s[0] * s[5] * s[11] + s[0] * s[7] * s[9] + s[4] * s[1] * s[11] - s[4] * s[3] * s[9] -
              s[8] * s[1] * s[7] + s[8] * s[3] * s[5];
    inv[15] = s[0] * s[5] * s[10] - s[0] * s[6] * s[9] - s[4] * s[1] * s[10] + s[4] * s[2] * s[9] +
              s[8] * s[1] * s[6] - s[8] * s[2] * s[5];
    const float det = s[0] * inv[0] + s[1] * inv[4] + s[2] * inv[8] + s[3] * inv[12];
    if (det == 0.0f || !std::isfinite(det)) {
        *out = IdentityMatrix();
        return false;
    }
    const float invDet = 1.0f / det;
    float* d = &out->m[0][0];
    for (int i = 0; i < 16; ++i) {
        d[i] = inv[i] * invDet;
    }
    return true;
}

inline float Dot(const Float3& a, const Float3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Float3 Normalize(const Float3& v) noexcept {
    const float len = std::sqrt(Dot(v, v));
    return len > 0.0f ? Float3{v.x / len, v.y / len, v.z / len} : v;
}

// NiTransform (guest layout gfx/ni_guest_layouts.h: float rotate[3][4], translate[3], scale; 0x40 bytes)
// -> D3D world matrix, exactly as FUN_001489A0 builds it for D3DDevice_SetTransform(6 = WORLD):
//   m[i][j] = rotate[i][j] * scale (i, j < 3), m[3] = (translate, 1), column 3 = (0, 0, 0, 1).
// Evidence: FUN_001489A0 (param_2[0..2], [4..6], [8..10] * param_2[0xF]; [0xC..0xE] translation) and the
// CPU copy in NiTriShape::vf33 0x1166D0 (out.x = t.x + m[0]*v.x + m[4]*v.y + m[8]*v.z with m scaled).
inline Matrix4 NiTransformToWorld(const float rotate3x4[12], const float translate[3], float scale) noexcept {
    Matrix4 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = rotate3x4[i * 4 + j] * scale;
        }
    }
    r.m[3][0] = translate[0];
    r.m[3][1] = translate[1];
    r.m[3][2] = translate[2];
    r.m[3][3] = 1.0f;
    return r;
}

}  // namespace gfx
