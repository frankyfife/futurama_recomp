// M4b freecam: pose math without guest access (unit-tested in tests/freecam_tests.cpp).
//
// The game camera as NiCamera keeps it (NiCamera::vf39 0xFFDA0): world rotation rows at +0x70/+0x80/+0x90 =
// direction / up / right, world translate at +0xA0. The freecam starts from that pose and moves in the same frame:
//   - world up = the coordinate axis closest to the game camera's up vector when the freecam starts (the game's
//     scenes are not rolled, so this is the scene's vertical axis);
//   - handedness = sign of dot(right, cross(dir, up)) of the starting pose, so right = h * cross(dir, up) keeps the
//     game's convention whatever it is;
//   - no roll: up is always the world up projected onto the plane perpendicular to the direction.
#pragma once

#include <algorithm>
#include <cmath>

namespace gfx::freecam {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(Vec3 a) noexcept { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a, Vec3 fallback) noexcept {
    const float l = Length(a);
    return (l > 1.0e-6f && std::isfinite(l)) ? a * (1.0f / l) : fallback;
}
// Rodrigues rotation of `v` about the unit axis `k` by `angle` radians.
inline Vec3 Rotate(Vec3 v, Vec3 k, float angle) noexcept {
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1.0f - c));
}

struct Pose {
    Vec3 position;
    Vec3 direction{1, 0, 0};
    Vec3 up{0, 1, 0};
    Vec3 right{0, 0, 1};
};

// Per-tick control values (already scaled by the caller's sensitivities and the tick length).
struct Motion {
    float forward = 0, strafe = 0, rise = 0;  // world units this tick (strafe > 0 = towards `right`)
    float yaw = 0, pitch = 0;                 // radians this tick (yaw > 0 = turn right, pitch > 0 = look up)
};

class Rig {
public:
    // Takes the game camera's pose as the start (derives world up and handedness from it).
    void Start(const Pose& game) noexcept {
        pose_.position = game.position;
        const Vec3 up = Normalize(game.up, Vec3{0, 0, 1});
        const float ax = std::fabs(up.x), ay = std::fabs(up.y), az = std::fabs(up.z);
        if (az >= ax && az >= ay) {
            worldUp_ = {0, 0, up.z >= 0 ? 1.0f : -1.0f};
        } else if (ay >= ax) {
            worldUp_ = {0, up.y >= 0 ? 1.0f : -1.0f, 0};
        } else {
            worldUp_ = {up.x >= 0 ? 1.0f : -1.0f, 0, 0};
        }
        handed_ = Dot(game.right, Cross(game.direction, game.up)) < 0 ? -1.0f : 1.0f;
        pose_.direction = Normalize(game.direction, Vec3{1, 0, 0});
        Orthonormalize();
    }

    void Step(const Motion& m) noexcept {
        // Yaw about the world up, pitch about the camera's right axis; keep 1 degree away from straight up/down.
        // The rotation sense is picked by its effect (turn towards `right`, tilt towards world up), so it does not
        // depend on the game's handedness.
        if (m.yaw != 0) {
            const Vec3 a = Rotate(pose_.direction, worldUp_, std::fabs(m.yaw));
            const Vec3 b = Rotate(pose_.direction, worldUp_, -std::fabs(m.yaw));
            const bool aRight = Dot(a, pose_.right) >= Dot(b, pose_.right);
            pose_.direction = (m.yaw > 0) == aRight ? a : b;
            Orthonormalize();
        }
        if (m.pitch != 0) {
            constexpr float kMaxElevation = 1.5533430f;  // 89 degrees
            const float s = std::clamp(Dot(pose_.direction, worldUp_), -1.0f, 1.0f);
            const float elevation = std::clamp(std::asin(s) + m.pitch, -kMaxElevation, kMaxElevation);
            const Vec3 horizontal = Normalize(pose_.direction - worldUp_ * s, pose_.up * -s);
            pose_.direction = horizontal * std::cos(elevation) + worldUp_ * std::sin(elevation);
            Orthonormalize();
        }
        pose_.position = pose_.position + pose_.direction * m.forward + pose_.right * m.strafe + worldUp_ * m.rise;
    }

    const Pose& Current() const noexcept { return pose_; }
    Vec3 WorldUp() const noexcept { return worldUp_; }
    float Handedness() const noexcept { return handed_; }

private:
    void Orthonormalize() noexcept {
        Vec3 d = Normalize(pose_.direction, Vec3{1, 0, 0});
        if (std::fabs(Dot(d, worldUp_)) > 0.9999f) {  // looking straight along the up axis: tilt slightly
            d = Normalize(d + Cross(worldUp_, Vec3{worldUp_.y, worldUp_.z, worldUp_.x}) * 0.02f, d);
        }
        pose_.direction = d;
        pose_.up = Normalize(worldUp_ - d * Dot(worldUp_, d), worldUp_);
        pose_.right = Normalize(Cross(d, pose_.up) * handed_, Vec3{0, 0, 1});
    }

    Pose pose_;
    Vec3 worldUp_{0, 0, 1};
    float handed_ = 1.0f;
};

// Stick value -32768..32767 -> -1..1 with a radial-style dead zone and a quadratic response for fine control.
inline float StickAxis(int raw, float deadzone) noexcept {
    float v = std::clamp(static_cast<float>(raw) / 32767.0f, -1.0f, 1.0f);
    const float a = std::fabs(v);
    if (a <= deadzone) {
        return 0.0f;
    }
    const float t = (a - deadzone) / (1.0f - deadzone);
    return (v < 0 ? -1.0f : 1.0f) * t * t;
}

}  // namespace gfx::freecam
