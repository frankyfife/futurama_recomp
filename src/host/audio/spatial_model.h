// DSOUND 3D level model and listener geometry (M5 3D agent; m5_audio.md 2.1, 3.6, 3.7, 6.4, 6.5).
//
// Pure math without XAudio2, shared by the Spatializer (spatial_x3d.cpp) and the unit tests. Everything the
// game hears from a 3D voice on the Xbox, except the direction itself, is computed here exactly as DSOUND
// does it:
//   distance  FUN_00249893: gain 1 up to MinDistance; with a rolloff curve of N points linear in AMPLITUDE
//   between 1
//             (at Min) and curve[i] at Min + (i+1)*(Max-Min)/N; beyond Max -10000 mB if
//             DSBCAPS_MUTE3DATMAXDISTANCE (0x20000) else clamped to Max; without a curve DS3D's 1/(1 +
//             rolloff*(d/Min - 1)). gain -> mB with FUN_0024974A / FUN_002496CC: (int)(float)(2000 * log10
//             g), truncated (cvttss2si at 0x24E2B4).
//   per bin   FUN_0024C3CA: attenuation = -(lVolume - headroom + mixbinVolume) - 3D terms, every 3D term sum
//             clamped to <= 0 mB, register (att * 64) / 100 in 1/64 dB clamped to 0xFFF (63.98 dB).
//             3D terms: bins 0..5 (LFE 3 included) distance + cone; XTLK bins 6..9 distance + cone + I3DL2
//             lDirect
//             (+ the front/back split, replaced by the direction matrix); I3DL2 bin 10 distance + cone +
//             lRoom.
//   3D voices have no headroom (0x248A63). The game never sets a cone (no SetCone* entry is imported) and
//   never a velocity (doppler without effect, m5_audio.md 3.7).
//
// Listener geometry: DSOUND forms right = top x front (FUN_002497AC) from the vectors the game passes, and
// the game passes front = -view direction, top = camera up (0xCBE85). That keeps left/right correct but swaps
// front and back (m5_audio.md 3.7). The Spatializer therefore never hands the game's front vector to X3DAudio
// directly; it moves the emitter into listener space (x = right, y = up, z = front) itself:
//   PositionalMode::Xbox       front = the game's front (sources in front of the camera are "behind", as on
//   the Xbox) PositionalMode::Corrected  front = -the game's front = view direction, right unchanged
//   (left/right stay correct) PositionalMode::Off        no direction: every 3D voice is placed straight
//   ahead; levels stay exact
#pragma once

#include "audio/audio_api.h"

#include <cstdint>

namespace snd::spatial {

enum class PositionalMode : std::uint8_t { Corrected, Xbox, Off };
// "corrected" | "xbox" | "off" (case-insensitive); false for anything else.
bool ParsePositionalMode(const char* text, PositionalMode& out) noexcept;
const char* PositionalModeName(PositionalMode mode) noexcept;

inline constexpr std::uint32_t kBufferFlag3D = 0x10;  // DSBCAPS_CTRL3D (0x248A63, FUN_0024C3CA)
inline constexpr std::uint32_t kBufferFlagMuteAtMaxDistance = 0x20000;  // FUN_00249893 test byte [flags+2], 2

// ---- levels
// ------------------------------------------------------------------------------------------------------
// FUN_0024974A: amplitude gain -> mB (g <= 0 -> -10000, g >= 1 -> 0, else (int)(float)(2000 * log10 g)).
std::int32_t GainToMillibel(float gain) noexcept;
// FUN_002496CC: DS3D default rolloff, x = rolloff * (d / Min - 1) -> (int)(float)(-2000 * log10(1 + x)); x <
// 0 -> 0.
std::int32_t DefaultRolloffMillibel(float x) noexcept;
// FUN_00249893 for one emitter: distance attenuation in mB (0 .. -10000).
std::int32_t DistanceMillibel(float distance, const Emitter3D& emitter, float listenerRolloffFactor) noexcept;
// The amplitude curve behind DistanceMillibel (before the mB truncation); used for the X3DAudio equivalence
// test.
float DistanceGainExact(float distance, const Emitter3D& emitter, float listenerRolloffFactor) noexcept;
// FUN_0024C3CA register: total level (lVolume - headroom + mixbin volume + 3D terms) -> linear gain, 1/64-dB
// steps, at most 63.98 dB attenuation (so -10000 mB plays at -63.98 dB, like the hardware register).
float HwMixbinGain(std::int32_t totalMillibel) noexcept;
// Clamp of a 3D term sum (FUN_0024C3CA `if (-1 < x) x = 0`).
inline std::int32_t ClampTerm(std::int64_t millibel) noexcept {
    return millibel > 0 ? 0 : (millibel < -1000000 ? -1000000 : static_cast<std::int32_t>(millibel));
}

// Levels of one 3D voice in mB (before the hardware register), all without direction.
struct Levels3D {
    std::int32_t distance = 0;  // DistanceMillibel
    std::int32_t direct = 0;    // lVolume + min(0, distance + lDirect)   (XTLK bins 6..9)
    std::int32_t reverb = 0;    // lVolume + min(0, distance + lRoom)     (I3DL2 bin 10)
    std::int32_t lfe = 0;       // lVolume + min(0, distance)             (bin 3)
};
Levels3D ComputeLevels3D(const Listener3D& listener, const Emitter3D& emitter) noexcept;

// ---- X3DAudio form of the rolloff curve (documentation / tests; the Spatializer uses DistanceMillibel)
// ------------ Points (normalized distance, gain) with CurveDistanceScaler = MaxDistance: (0,1) (Min/Max,1)
// then
// ((Min + (i+1)*(Max-Min)/N) / Max, curve[i]). Equivalent to the DSOUND curve up to the mB truncation,
// including the mute beyond Max when the last point is 0 (X3DAudio holds the last point). Returns the point
// count (0 if the emitter has no curve or Max <= Min).
inline constexpr std::uint32_t kMaxX3dCurvePoints = kMaxRolloffPoints + 2;
struct CurvePoint {
    float distance = 0;
    float gain = 0;
};
std::uint32_t BuildX3dDistanceCurve(const Emitter3D& emitter,
                                    CurvePoint (&points)[kMaxX3dCurvePoints]) noexcept;

// ---- geometry
// ----------------------------------------------------------------------------------------------------
inline Vec3 Sub(const Vec3& a, const Vec3& b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline float Dot(const Vec3& a, const Vec3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Length(const Vec3& v) noexcept;

// Orthonormal listener basis as DSOUND sees it: front = normalize(game front), top = top orthogonalized
// against front, right = top x front. Degenerate input (zero or parallel vectors) falls back to front (0,0,1)
// / top (0,1,0).
struct ListenerBasis {
    Vec3 right{1, 0, 0};
    Vec3 up{0, 1, 0};
    Vec3 front{0, 0, 1};
};
ListenerBasis MakeListenerBasis(const Listener3D& listener) noexcept;
// Emitter position in listener space for the mode (x right, y up, z front; X3DAudio's default listener
// frame).
Vec3 ToListenerSpace(const Vec3& worldPosition, const Listener3D& listener, const ListenerBasis& basis,
                     PositionalMode mode) noexcept;

// DS3D doppler as a pitch offset in Xbox pitch units (codec_format.h): ratio = (c + df * vL.u) / (c + df *
// vS.u), u = unit vector listener -> emitter, c = 343.5 m/s / distanceFactor (world units per second), the
// projected speeds clamped below c. 0 when both velocities are zero (always the case in this game) or the
// factor is 0. ASSUMPTION: DSOUND's doppler evaluation (CMcpx 3D code) was not traced; the game never makes
// it audible.
std::int32_t DopplerPitch(const Listener3D& listener, const Emitter3D& emitter) noexcept;

}  // namespace snd::spatial
