// DSOUND 3D level model and listener geometry (see spatial_model.h).
#include "audio/spatial_model.h"

#include "audio/codec_format.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace snd::spatial {
namespace {

bool EqualsNoCase(const char* a, const char* b) noexcept {
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return *a == *b;
}

Vec3 Scale(const Vec3& v, float s) noexcept {
    return {v.x * s, v.y * s, v.z * s};
}

bool Normalize(Vec3& v) noexcept {
    const float len = Length(v);
    if (!(len > 1.0e-6f) || !std::isfinite(len)) {
        return false;
    }
    v = Scale(v, 1.0f / len);
    return true;
}

}  // namespace

bool ParsePositionalMode(const char* text, PositionalMode& out) noexcept {
    if (text == nullptr) {
        return false;
    }
    // Blanks around the value are tolerated like everywhere in the configuration.
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    char word[16] = {};
    std::size_t n = 0;
    while (text[n] != '\0' && text[n] != ' ' && text[n] != '\t' && n + 1 < sizeof(word)) {
        word[n] = text[n];
        ++n;
    }
    for (const char* rest = text + n; *rest != '\0'; ++rest) {
        if (*rest != ' ' && *rest != '\t') {
            return false;
        }
    }
    if (EqualsNoCase(word, "corrected")) {
        out = PositionalMode::Corrected;
    } else if (EqualsNoCase(word, "xbox")) {
        out = PositionalMode::Xbox;
    } else if (EqualsNoCase(word, "off")) {
        out = PositionalMode::Off;
    } else {
        return false;
    }
    return true;
}

const char* PositionalModeName(PositionalMode mode) noexcept {
    switch (mode) {
    case PositionalMode::Corrected:
        return "corrected";
    case PositionalMode::Xbox:
        return "xbox";
    case PositionalMode::Off:
        return "off";
    }
    return "?";
}

// ---- levels
// ------------------------------------------------------------------------------------------------------

std::int32_t GainToMillibel(float gain) noexcept {
    if (!(gain > 0.0f)) {
        return kMillibelMin;  // 0xFFFFD8F0
    }
    if (gain >= 1.0f) {
        return 0;
    }
    // 0.3010299956639812 * log2(g) * 2000 in x87 extended precision, stored as float32, then cvttss2si.
    const float mb = static_cast<float>(0.3010299956639812 * std::log2(static_cast<double>(gain)) * 2000.0);
    return static_cast<std::int32_t>(mb);
}

std::int32_t DefaultRolloffMillibel(float x) noexcept {
    if (x < 0.0f) {
        return 0;
    }
    if (!std::isfinite(x)) {
        return kMillibelMin;
    }
    const float mb =
        static_cast<float>(0.3010299956639812 * std::log2(static_cast<double>(x) + 1.0) * -2000.0);
    if (mb < static_cast<float>(kMillibelMin)) {
        return kMillibelMin;  // the guest would truncate any float; the register clamps at 63.98 dB anyway
    }
    return static_cast<std::int32_t>(mb);
}

namespace {

// Shared evaluation of FUN_00249893. Returns true with `mb` set for the early-out cases (inside Min, muted),
// else fills `gain` (curve) or `defaultX` (no curve).
struct DistanceEval {
    bool done = false;
    std::int32_t mb = 0;
    bool useCurve = false;
    float gain = 1.0f;
    float defaultX = 0.0f;
};

DistanceEval EvaluateDistance(float distance, const Emitter3D& emitter,
                              float listenerRolloffFactor) noexcept {
    DistanceEval r;
    const float minD = emitter.minDistance;
    const float maxD = emitter.maxDistance;
    float d = distance;
    if (!(d > minD)) {  // fcom + test ah, 0x41: d <= Min (or unordered) -> 0 mB
        r.done = true;
        return r;
    }
    if ((emitter.bufferFlags & kBufferFlagMuteAtMaxDistance) != 0 && !(d < maxD)) {
        r.done = true;
        r.mb = kMillibelMin;
        return r;
    }
    if (maxD < d) {
        d = maxD;
    }
    const std::uint32_t count = std::min(emitter.rolloffCount, kMaxRolloffPoints);
    if (count == 0) {
        // ASSUMPTION: the per-source factor at +0x44 of FUN_00249893 (multiplied with the listener's rolloff
        // factor) is 1 - the game always sets a curve, so this branch never runs for it.
        if (!(minD > 0.0f)) {
            r.done = true;
            r.mb = d > 0.0f ? kMillibelMin : 0;
            return r;
        }
        r.defaultX = (d / minD - 1.0f) * listenerRolloffFactor;
        return r;
    }
    const float span = (maxD - minD) / static_cast<float>(count);
    if (!(span > 0.0f)) {
        r.done =
            true;  // Max <= Min: d was clamped to Max <= Min, no attenuation (the guest would divide by 0)
        return r;
    }
    const double offset = static_cast<double>(d) - static_cast<double>(minD);
    const double x = offset / static_cast<double>(span);
    std::uint32_t idx = x <= 0.0 ? 0u : static_cast<std::uint32_t>(x);  // _ftol truncates
    if (idx >= count) {
        idx = count - 1;
    }
    double lo = 1.0;
    double hi = emitter.rolloff[0];
    double t = offset;
    if (idx != 0) {
        lo = emitter.rolloff[idx - 1];
        hi = emitter.rolloff[idx];
        t = offset - static_cast<double>(idx) * static_cast<double>(span);
    }
    r.useCurve = true;
    r.gain = static_cast<float>((hi - lo) * (1.0 / static_cast<double>(span)) * t + lo);
    return r;
}

}  // namespace

std::int32_t DistanceMillibel(float distance, const Emitter3D& emitter,
                              float listenerRolloffFactor) noexcept {
    const DistanceEval r = EvaluateDistance(distance, emitter, listenerRolloffFactor);
    if (r.done) {
        return r.mb;
    }
    return r.useCurve ? GainToMillibel(r.gain) : DefaultRolloffMillibel(r.defaultX);
}

float DistanceGainExact(float distance, const Emitter3D& emitter, float listenerRolloffFactor) noexcept {
    const DistanceEval r = EvaluateDistance(distance, emitter, listenerRolloffFactor);
    if (r.done) {
        return r.mb <= kMillibelMin ? 0.0f : 1.0f;
    }
    if (r.useCurve) {
        return std::clamp(r.gain, 0.0f, 1.0f);
    }
    return r.defaultX < 0.0f ? 1.0f : 1.0f / (1.0f + r.defaultX);
}

float HwMixbinGain(std::int32_t totalMillibel) noexcept {
    // FUN_0024C3CA: (att << 6) / 100 as unsigned, clamped to 0xFFF. A positive total (amplification) cannot
    // occur with the game's values (volume, mixbin volumes and 3D terms are all <= 0); it is treated as 0 dB
    // here.
    const std::int64_t att = totalMillibel >= 0 ? 0 : -static_cast<std::int64_t>(totalMillibel);
    const std::int64_t reg = std::min<std::int64_t>((att << 6) / 100, 0xFFF);
    return static_cast<float>(std::pow(10.0, -static_cast<double>(reg) / (64.0 * 20.0)));
}

Levels3D ComputeLevels3D(const Listener3D& listener, const Emitter3D& emitter) noexcept {
    Levels3D l;
    const float distance = Length(Sub(emitter.position, listener.position));
    l.distance = DistanceMillibel(distance, emitter, listener.rolloffFactor);
    const std::int64_t volume = emitter.volumeMillibel;  // 3D voices: no headroom (0x248A63)
    l.direct = static_cast<std::int32_t>(std::max<std::int64_t>(
        volume + ClampTerm(static_cast<std::int64_t>(l.distance) + emitter.i3dl2.direct), -1000000));
    l.reverb = static_cast<std::int32_t>(std::max<std::int64_t>(
        volume + ClampTerm(static_cast<std::int64_t>(l.distance) + emitter.i3dl2.room), -1000000));
    l.lfe = static_cast<std::int32_t>(std::max<std::int64_t>(volume + ClampTerm(l.distance), -1000000));
    return l;
}

std::uint32_t BuildX3dDistanceCurve(const Emitter3D& emitter,
                                    CurvePoint (&points)[kMaxX3dCurvePoints]) noexcept {
    const std::uint32_t count = std::min(emitter.rolloffCount, kMaxRolloffPoints);
    const float minD = emitter.minDistance;
    const float maxD = emitter.maxDistance;
    if (count == 0 || !(maxD > minD) || !(minD >= 0.0f)) {
        return 0;
    }
    std::uint32_t n = 0;
    points[n++] = {0.0f, 1.0f};
    points[n++] = {minD / maxD, 1.0f};
    for (std::uint32_t i = 0; i < count; ++i) {
        const float at = minD + static_cast<float>(i + 1) * (maxD - minD) / static_cast<float>(count);
        points[n++] = {std::min(1.0f, at / maxD), std::clamp(emitter.rolloff[i], 0.0f, 1.0f)};
    }
    if (minD == 0.0f) {
        // X3DAudio needs strictly increasing distances: drop the duplicate 0 point.
        for (std::uint32_t i = 1; i < n; ++i) {
            points[i - 1] = points[i];
        }
        --n;
    }
    return n;
}

// ---- geometry
// ----------------------------------------------------------------------------------------------------

float Length(const Vec3& v) noexcept {
    return std::sqrt(Dot(v, v));
}

ListenerBasis MakeListenerBasis(const Listener3D& listener) noexcept {
    ListenerBasis b;
    Vec3 front = listener.front;
    if (!Normalize(front)) {
        front = {0, 0, 1};
    }
    Vec3 up = Sub(listener.top, Scale(front, Dot(listener.top, front)));
    if (!Normalize(up)) {
        // Top missing or parallel to front: any perpendicular vector (world up if possible).
        const Vec3 helper = std::fabs(front.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
        up = Sub(helper, Scale(front, Dot(helper, front)));
        if (!Normalize(up)) {
            up = {0, 1, 0};
        }
    }
    b.front = front;
    b.up = up;
    b.right = Cross(up, front);  // FUN_002497AC: right = top x front
    return b;
}

Vec3 ToListenerSpace(const Vec3& worldPosition, const Listener3D& listener, const ListenerBasis& basis,
                     PositionalMode mode) noexcept {
    const Vec3 rel = Sub(worldPosition, listener.position);
    switch (mode) {
    case PositionalMode::Xbox:
        return {Dot(rel, basis.right), Dot(rel, basis.up), Dot(rel, basis.front)};
    case PositionalMode::Corrected:
        return {Dot(rel, basis.right), Dot(rel, basis.up), -Dot(rel, basis.front)};
    case PositionalMode::Off:
        break;
    }
    return {0.0f, 0.0f, Length(rel)};
}

std::int32_t DopplerPitch(const Listener3D& listener, const Emitter3D& emitter) noexcept {
    const float factor = listener.dopplerFactor;
    if (!(factor > 0.0f) || !(listener.distanceFactor > 0.0f)) {
        return 0;
    }
    if (Dot(listener.velocity, listener.velocity) == 0.0f &&
        Dot(emitter.velocity, emitter.velocity) == 0.0f) {
        return 0;
    }
    Vec3 u = Sub(emitter.position, listener.position);
    if (!Normalize(u)) {
        return 0;
    }
    const double c = 343.5 / static_cast<double>(listener.distanceFactor);
    const double limit = c * 0.999 / static_cast<double>(factor);
    const double vl = std::clamp(static_cast<double>(Dot(listener.velocity, u)), -limit, limit);
    const double vs = std::clamp(static_cast<double>(Dot(emitter.velocity, u)), -limit, limit);
    const double ratio = (c + factor * vl) / (c + factor * vs);
    return codec::PitchFromRatio(ratio);
}

}  // namespace snd::spatial
