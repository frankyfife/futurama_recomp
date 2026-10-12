// Spatializer: DSOUND 3D levels + X3DAudio direction (see spatial_x3d.h).
#include "audio/spatial_x3d.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "core/log.h"

#include <x3daudio.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>
#include <string>

namespace snd {
namespace spatial {
namespace {

constexpr std::uint32_t kMaxDst = kMaxOutputChannels;
constexpr float kInnerRadiusMeters = 0.5f;

// Index of a SPEAKER_* bit among the set bits of `mask` (XAudio2 channel order), -1 if absent.
int ChannelIndexOf(std::uint32_t mask, std::uint32_t speaker) noexcept {
    if ((mask & speaker) == 0) {
        return -1;
    }
    return std::popcount(mask & (speaker - 1u));
}

class X3dSpatializer final : public Spatializer {
public:
    explicit X3dSpatializer(const SpatialSettings& settings) noexcept
        : settings_(settings), trim_(static_cast<float>(std::pow(10.0, settings.trim3dDb / 20.0))) {}

    void Compute(const Listener3D& listener, const Emitter3D& emitter, std::uint32_t outputChannels,
                 std::uint32_t outputChannelMask, SpatialResult& out) noexcept override {
        out = SpatialResult{};
        const std::uint32_t dst = std::clamp<std::uint32_t>(outputChannels, 1u, kMaxDst);
        EnsureLayout(dst, outputChannelMask);

        const Levels3D levels = ComputeLevels3D(listener, emitter);
        out.distanceGain = levels.distance <= kMillibelMin
                               ? 0.0f
                               : static_cast<float>(std::pow(10.0, levels.distance / 2000.0));
        out.reverbSend = HwMixbinGain(levels.reverb) * trim_;
        out.lfe = HwMixbinGain(levels.lfe) * trim_;

        float dir[kMaxDst] = {};
        Direction(listener, emitter, dir);

        const std::uint32_t src = std::clamp<std::uint32_t>(emitter.channels, 1u, kMaxOutputChannels);
        const float perChannel = src == 1 ? 1.0f : 1.0f / std::sqrt(static_cast<float>(src));
        const float direct = HwMixbinGain(levels.direct) * trim_ * perChannel;
        for (std::uint32_t s = 0; s < src; ++s) {
            for (std::uint32_t d = 0; d < dst; ++d) {
                out.matrix[s * dst + d] =
                    static_cast<int>(d) == lfeIndex_ ? out.lfe * perChannel : dir[d] * direct;
            }
        }
    }

private:
    void EnsureLayout(std::uint32_t channels, std::uint32_t mask) noexcept {
        if (channels == channels_ && mask == requestedMask_) {
            return;
        }
        channels_ = channels;
        requestedMask_ = mask;
        mask_ = (mask != 0 && static_cast<std::uint32_t>(std::popcount(mask)) == channels)
                    ? mask
                    : DefaultSpeakerMask(channels);
        lfeIndex_ = mask_ != 0 ? ChannelIndexOf(mask_, SPEAKER_LOW_FREQUENCY) : -1;
        // X3DAudio pans over the four corners (quad) or over front left/right (stereo), never over the front centre:
        // the game's 3D mixbins {6, 8, 7, 9, 10, 3} (0xCC35A) have no centre bin - on the Xbox a 3D voice reaches
        // the speakers through the crosstalk bins (front/back left/right), LFE and I3DL2 only (dsound_mix.h maps XTLK
        // to the corners). The LFE column comes from the DSOUND LFE level, not from the direction. Only standard
        // masks go to X3DAudio: 2.9 returns 1 on the first channel and 0 elsewhere for every direction with
        // non-standard ones such as FL FR LFE SL SR (0x60B) or FL FR SL SR (0x603) - measured; so the rear corners
        // of a side-surround layout (0x60F, HDMI TVs) are panned as quad's back pair and mapped to SL/SR.
        auto index = [this](std::uint32_t speaker) { return ChannelIndexOf(mask_, speaker); };
        const int fl = index(SPEAKER_FRONT_LEFT);
        const int fr = index(SPEAKER_FRONT_RIGHT);
        const int rl = index(SPEAKER_BACK_LEFT) >= 0 ? index(SPEAKER_BACK_LEFT) : index(SPEAKER_SIDE_LEFT);
        const int rr = index(SPEAKER_BACK_RIGHT) >= 0 ? index(SPEAKER_BACK_RIGHT) : index(SPEAKER_SIDE_RIGHT);
        for (int& m : map_) {
            m = -1;
        }
        if (fl >= 0 && fr >= 0 && rl >= 0 && rr >= 0) {
            x3dMask_ = SPEAKER_QUAD;  // X3DAudio order FL FR BL BR
            map_[0] = fl;
            map_[1] = fr;
            map_[2] = rl;
            map_[3] = rr;
        } else if (fl >= 0 && fr >= 0) {
            x3dMask_ = SPEAKER_STEREO;
            map_[0] = fl;
            map_[1] = fr;
        } else {
            x3dMask_ = mask_;  // mono or an unusual layout: as it is
            std::uint32_t k = 0;
            for (std::uint32_t bits = x3dMask_; bits != 0 && k < kMaxDst; bits &= bits - 1u, ++k) {
                map_[k] = index(bits & (~bits + 1u));  // X3DAudio channel k -> output channel
            }
        }
        x3dChannels_ = static_cast<std::uint32_t>(std::popcount(x3dMask_));
        x3dReady_ = mask_ != 0 && x3dChannels_ <= kMaxDst &&
                    SUCCEEDED(X3DAudioInitialize(x3dMask_, X3DAUDIO_SPEED_OF_SOUND, handle_));
        if (!x3dReady_ && !loggedFallback_) {
            loggedFallback_ = true;
            FUT_LOGW(Snd, "spatial: X3DAudio unavailable for %u channels (mask 0x%X) - simple front panning",
                     channels, mask);
        }
        // Reference: a source straight ahead (outside the inner radius) defines gain 1 on the loudest
        // channel.
        for (float& f : front_) {
            f = 0.0f;
        }
        float raw[kMaxDst] = {};
        RawDirection(Vec3{0.0f, 0.0f, 10.0f}, 0.0f, raw);
        float peak = 0.0f;
        for (std::uint32_t d = 0; d < channels_; ++d) {
            if (static_cast<int>(d) != lfeIndex_) {
                peak = std::max(peak, raw[d]);
            }
        }
        const float norm = peak > 1.0e-6f ? 1.0f / peak : 1.0f;
        referencePower_ = 0.0f;
        for (std::uint32_t d = 0; d < channels_; ++d) {
            front_[d] = static_cast<int>(d) == lfeIndex_ ? 0.0f : raw[d] * norm;
            referencePower_ += front_[d] * front_[d];
        }
    }

    // X3DAudio coefficients (mono emitter, flat curve) for a listener-space position; LFE column zeroed.
    void RawDirection(const Vec3& p, float innerRadius, float (&dir)[kMaxDst]) noexcept {
        for (float& v : dir) {
            v = 0.0f;
        }
        if (!x3dReady_) {
            FallbackPan(p, dir);
            return;
        }
        X3DAUDIO_LISTENER l = {};
        l.OrientFront = {0.0f, 0.0f, 1.0f};
        l.OrientTop = {0.0f, 1.0f, 0.0f};
        X3DAUDIO_DISTANCE_CURVE_POINT flatPoints[2] = {{0.0f, 1.0f}, {1.0f, 1.0f}};
        X3DAUDIO_DISTANCE_CURVE flat = {flatPoints, 2};
        X3DAUDIO_EMITTER e = {};
        e.OrientFront = {0.0f, 0.0f, 1.0f};
        e.OrientTop = {0.0f, 1.0f, 0.0f};
        e.Position = {p.x, p.y, p.z};
        e.InnerRadius = innerRadius;
        e.InnerRadiusAngle = X3DAUDIO_PI / 4.0f - 0.001f;
        e.ChannelCount = 1;
        e.pVolumeCurve = &flat;
        e.pLFECurve = &flat;
        e.CurveDistanceScaler = 1.0f;
        e.DopplerScaler = 0.0f;
        float coefficients[kMaxDst] = {};
        X3DAUDIO_DSP_SETTINGS dsp = {};
        dsp.pMatrixCoefficients = coefficients;
        dsp.SrcChannelCount = 1;
        dsp.DstChannelCount = x3dChannels_;
        X3DAudioCalculate(handle_, &l, &e, X3DAUDIO_CALCULATE_MATRIX, &dsp);
        for (std::uint32_t k = 0; k < x3dChannels_; ++k) {
            const float c = coefficients[k];  // [SrcChannelCount * D + S] with one source channel
            const int d = map_[k];
            if (d >= 0 && static_cast<std::uint32_t>(d) < channels_ && d != lfeIndex_ && std::isfinite(c)) {
                dir[d] = std::max(0.0f, c);
            }
        }
    }

    // Constant-power left/right pan on the first two channels (only when X3DAudio cannot be initialized).
    void FallbackPan(const Vec3& p, float (&dir)[kMaxDst]) const noexcept {
        if (channels_ == 1) {
            dir[0] = 1.0f;
            return;
        }
        const float horizontal = std::sqrt(p.x * p.x + p.z * p.z);
        const float s = horizontal > 1.0e-6f ? std::clamp(p.x / horizontal, -1.0f, 1.0f) : 0.0f;
        const float angle = (s + 1.0f) * (X3DAUDIO_PI / 4.0f);  // 0 = left .. pi/2 = right
        dir[0] = std::cos(angle);
        dir[1] = std::sin(angle);
    }

    void Direction(const Listener3D& listener, const Emitter3D& emitter, float (&dir)[kMaxDst]) noexcept {
        if (settings_.positional == PositionalMode::Off) {
            std::memcpy(dir, front_, sizeof(dir));
            return;
        }
        const ListenerBasis basis = MakeListenerBasis(listener);
        const Vec3 p = ToListenerSpace(emitter.position, listener, basis, settings_.positional);
        const float distanceFactor = listener.distanceFactor > 0.0f ? listener.distanceFactor : 1.0f;
        const float inner = kInnerRadiusMeters / distanceFactor;
        RawDirection(p, std::isfinite(inner) ? inner : 0.0f, dir);
        // Constant power: every direction carries the power of the front reference (X3DAudio pans stereo with
        // amplitudes summing to 1, i.e. -6 dB per channel in the middle; the Xbox HRTF keeps the level) - but no
        // channel above the front reference's loudest (gain 1 = 0 dB, the ceiling of an Xbox mixbin register): a
        // source to the side plays at 1 on the near channel, not at sqrt(2) (+3 dB).
        float power = 0.0f;
        float peak = 0.0f;
        for (std::uint32_t d = 0; d < channels_; ++d) {
            power += dir[d] * dir[d];
            peak = std::max(peak, dir[d]);
        }
        if (!(power > 1.0e-12f) || !(peak > 1.0e-6f)) {
            std::memcpy(dir, front_, sizeof(dir));
            return;
        }
        const float scale = std::min(std::sqrt(referencePower_ / power), 1.0f / peak);
        for (std::uint32_t d = 0; d < channels_; ++d) {
            dir[d] *= scale;
        }
    }

    SpatialSettings settings_;
    float trim_ = 1.0f;
    std::uint32_t channels_ = 0;
    std::uint32_t requestedMask_ = 0xFFFFFFFFu;
    std::uint32_t mask_ = 0;
    std::uint32_t x3dMask_ = 0;      // the layout X3DAudio pans over (quad, stereo or mask_), map_ maps it
    std::uint32_t x3dChannels_ = 0;
    int map_[kMaxDst] = {};          // X3DAudio channel -> output channel
    int lfeIndex_ = -1;
    bool x3dReady_ = false;
    bool loggedFallback_ = false;
    float referencePower_ = 1.0f;  // sum of squares of front_ (front reference, loudest channel = 1)
    float front_[kMaxDst] = {};
    X3DAUDIO_HANDLE handle_ = {};
};

}  // namespace

std::uint32_t DefaultSpeakerMask(std::uint32_t channels) noexcept {
    switch (channels) {
    case 1:
        return SPEAKER_MONO;
    case 2:
        return SPEAKER_STEREO;
    case 4:
        return SPEAKER_QUAD;
    case 6:
        return SPEAKER_5POINT1;
    case 8:
        return SPEAKER_7POINT1_SURROUND;
    default:
        return 0;
    }
}

SpatialSettings SpatialSettingsFromConfig() noexcept {
    SpatialSettings s;
    try {
        const core::Config& cfg = core::GetConfig();
        const std::string positional =
            cfg.GetString("audio", "positional", core::ConfigKeyDefault("audio", "positional"));
        if (!ParsePositionalMode(positional.c_str(), s.positional)) {
            s.positional = PositionalMode::Corrected;
        }
        const std::string trim =
            cfg.GetString("audio", "trim_3d", core::ConfigKeyDefault("audio", "trim_3d"));
        double db = 0.0;
        if (!trim.empty() && core::ParseConfigFloat(trim, db)) {
            s.trim3dDb = static_cast<float>(std::clamp(db, -40.0, 20.0));
        }
    } catch (...) {
        s = SpatialSettings{};
    }
    return s;
}

std::unique_ptr<Spatializer> CreateSpatializer(const SpatialSettings& settings) noexcept {
    return std::unique_ptr<Spatializer>(new (std::nothrow) X3dSpatializer(settings));
}

}  // namespace spatial

std::unique_ptr<Spatializer> CreateSpatializer() noexcept {
    const spatial::SpatialSettings settings = spatial::SpatialSettingsFromConfig();
    FUT_LOGI(Snd, "spatial: positional = %s, trim_3d = %.1f dB",
             spatial::PositionalModeName(settings.positional), static_cast<double>(settings.trim3dDb));
    return spatial::CreateSpatializer(settings);
}

}  // namespace snd
