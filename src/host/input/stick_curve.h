// The game's stick curve and its inverse (analysis\loader\m4_input.md 3.2 and 8.4).
//
// The controller update 0x9C3D0 stores each stick axis as v = raw / 32768. FUN_0009B3E0 (X) / FUN_0009B480 (Y) turn it
// into the value the players and menus read:
//     dz  = 0.4, or 0.2 when the OTHER axis lies outside +-0.4 (0x3ECCCCCD)
//     out = |v| > dz ? sign(v) * min(1, (|v| - dz) * 1.3 / (1 - dz)) : 0
// A full deflection is reached from |v| = 0.862 (dz 0.4) / 0.815 (dz 0.2).
//
// Mouse look (the mouse drives a stick, m4_input.md 8.4) asks for a deflection `o` the game should see and needs the
// raw value that produces it: raw = sign(o) * (dz + |o| * (1 - dz) / 1.3). Because dz of one axis depends on the
// other axis's raw value, GameStickInverse picks a consistent pair of dead zones (one always exists; for small
// diagonal movements the game's curve itself is asymmetric - one axis must stay inside +-0.4).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace inp {

inline constexpr float kGameStickDeadzone = 0.4f;         // ctor 0x9BF50 (0x3ECCCCCD)
inline constexpr float kGameStickDeadzoneReduced = 0.2f;  // other axis outside +-0.4
inline constexpr float kGameStickGain = 1.3f;
inline constexpr float kGameStickIdle = 0.25f;            // IsIdle 0x9BAC0: |stick| <= 0.25

// Dead zone the game uses for an axis when the other axis has the raw value `other` (-1..1).
inline float GameStickDeadzone(float other) noexcept {
    return std::fabs(other) > kGameStickDeadzone ? kGameStickDeadzoneReduced : kGameStickDeadzone;
}

// The game's curve: raw axis `v` (-1..1) with the other axis `other` -> deflection -1..1.
inline float GameStickOutput(float v, float other) noexcept {
    const float dz = GameStickDeadzone(other);
    const float a = std::fabs(v);
    if (a <= dz) {
        return 0.0f;
    }
    const float o = std::min(1.0f, (a - dz) * kGameStickGain / (1.0f - dz));
    return v < 0.0f ? -o : o;
}

// Raw value (-1..1) that gives deflection `o` (-1..1) with dead zone `dz`; 0 for o = 0.
inline float GameStickRawFor(float o, float dz) noexcept {
    if (o == 0.0f) {
        return 0.0f;
    }
    const float a = std::min(1.0f, std::fabs(o));
    const float raw = std::min(1.0f, dz + a * (1.0f - dz) / kGameStickGain);
    return o < 0.0f ? -raw : raw;
}

// Inverse of the curve for a whole stick: wanted deflections (ox, oy) -> raw values (rx, ry), each -1..1, such that
// GameStickOutput(rx, ry) == ox and GameStickOutput(ry, rx) == oy (up to float rounding).
inline void GameStickInverse(float ox, float oy, float& rx, float& ry) noexcept {
    // Candidate dead-zone pairs; the axis with the larger wish keeps the full dead zone first.
    const bool xFirst = std::fabs(ox) >= std::fabs(oy);
    const float pairs[4][2] = {
        {kGameStickDeadzone, kGameStickDeadzone},
        {kGameStickDeadzoneReduced, kGameStickDeadzoneReduced},
        {xFirst ? kGameStickDeadzone : kGameStickDeadzoneReduced, xFirst ? kGameStickDeadzoneReduced : kGameStickDeadzone},
        {xFirst ? kGameStickDeadzoneReduced : kGameStickDeadzone, xFirst ? kGameStickDeadzone : kGameStickDeadzoneReduced},
    };
    for (const auto& p : pairs) {
        const float x = GameStickRawFor(ox, p[0]);
        const float y = GameStickRawFor(oy, p[1]);
        const bool xOk = ox == 0.0f || GameStickDeadzone(y) == p[0];
        const bool yOk = oy == 0.0f || GameStickDeadzone(x) == p[1];
        if (xOk && yOk) {
            rx = x;
            ry = y;
            return;
        }
    }
    // Not reachable (one pair is always consistent); fall back to the full dead zone.
    rx = GameStickRawFor(ox, kGameStickDeadzone);
    ry = GameStickRawFor(oy, kGameStickDeadzone);
}

// -1..1 -> XINPUT thumb value (the game divides by 32768; rounding up keeps the deflection at least the wish).
inline std::int16_t ThumbFromUnit(float v) noexcept {
    const float c = std::clamp(v, -1.0f, 1.0f);
    const float scaled = c * 32768.0f;
    const long r = static_cast<long>(c < 0.0f ? std::floor(scaled) : std::ceil(scaled));
    return static_cast<std::int16_t>(std::clamp(r, -32768L, 32767L));
}
inline float UnitFromThumb(std::int16_t v) noexcept {
    return static_cast<float>(v) / 32768.0f;
}

}  // namespace inp
