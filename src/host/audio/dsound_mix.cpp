// DSOUND 2D level model (see dsound_mix.h for the evidence). M5 DSOUND core agent.
#include "audio/dsound_mix.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace snd::ds {

MixBinList DefaultMixBins(std::uint32_t channels) noexcept {
    MixBinList l;
    // 0x2638AC / 0x2638B4: {FRONT_LEFT, FRONT_RIGHT} at 0 mB for mono and stereo voices.
    l.count = 2;
    l.entries[0] = {kMixBinFrontLeft, 0};
    l.entries[1] = {kMixBinFrontRight, 0};
    if (channels > 2) {
        // ASSUMPTION: DSOUND's multichannel default is one bin per channel in Xbox order (FL FR FC LFE BL BR).
        l.count = std::min<std::uint32_t>(channels, 6);
        for (std::uint32_t i = 0; i < l.count; ++i) {
            l.entries[i] = {i, 0};
        }
    }
    return l;
}

int SpeakerIndex(std::uint32_t mask, std::uint32_t speakerBit) noexcept {
    if ((mask & speakerBit) == 0) {
        return -1;
    }
    return std::popcount(mask & (speakerBit - 1u));
}

namespace {

// Adds `gain` for one Xbox bin to the matrix row of a source channel.
void AddBin(std::uint32_t bin, float gain, float* row, std::uint32_t outChannels, std::uint32_t mask,
            float& reverb) noexcept {
    const auto add = [&](std::uint32_t speaker, float g) {
        const int i = SpeakerIndex(mask, speaker);
        if (i >= 0 && static_cast<std::uint32_t>(i) < outChannels) {
            row[i] += g;
            return true;
        }
        return false;
    };
    constexpr float kMinus3dB = 0.70710678f;
    switch (bin) {
        case kMixBinFrontLeft:
        case kMixBinXtlkFrontLeft:
            if (!add(kSpeakerFrontLeft, gain) && outChannels == 1) {
                row[0] += gain;
            }
            break;
        case kMixBinFrontRight:
        case kMixBinXtlkFrontRight:
            if (!add(kSpeakerFrontRight, gain) && outChannels == 1) {
                row[0] += gain;
            }
            break;
        case kMixBinFrontCenter:
            if (!add(kSpeakerFrontCenter, gain)) {
                const bool l = add(kSpeakerFrontLeft, gain * kMinus3dB);
                const bool r = add(kSpeakerFrontRight, gain * kMinus3dB);
                if (!l && !r && outChannels == 1) {
                    row[0] += gain;
                }
            }
            break;
        case kMixBinLowFrequency:
            add(kSpeakerLowFrequency, gain);
            break;
        case kMixBinBackLeft:
        case kMixBinXtlkBackLeft:
            if (!add(kSpeakerBackLeft, gain) && !add(kSpeakerSideLeft, gain) && !add(kSpeakerFrontLeft, gain) &&
                outChannels == 1) {
                row[0] += gain;
            }
            break;
        case kMixBinBackRight:
        case kMixBinXtlkBackRight:
            if (!add(kSpeakerBackRight, gain) && !add(kSpeakerSideRight, gain) &&
                !add(kSpeakerFrontRight, gain) && outChannels == 1) {
                row[0] += gain;
            }
            break;
        case kMixBinI3dl2:
            reverb += gain;
            break;
        default:
            break;  // FX sends: not used by the game
    }
}

}  // namespace

void Compute2D(std::int32_t volumeMillibel, std::int32_t headroomMillibel, const MixBinList& bins,
               std::uint32_t srcChannels, std::uint32_t outChannels, std::uint32_t outChannelMask,
               Matrix2D& out) noexcept {
    out = Matrix2D{};
    if (srcChannels == 0 || outChannels == 0 || srcChannels > kMaxOutputChannels ||
        outChannels > kMaxOutputChannels) {
        return;
    }
    const std::uint32_t count = std::min<std::uint32_t>(bins.count, kMaxMixBins);
    if (count == 0) {
        return;
    }
    // FUN_0024C3CA: channel c serves bins [c*per, (c+1)*per) with per = count / channels (at least 1).
    const std::uint32_t per = std::max<std::uint32_t>(1, count / srcChannels);
    float reverb = 0;
    for (std::uint32_t c = 0; c < srcChannels; ++c) {
        float* row = &out.levels[static_cast<std::size_t>(c) * outChannels];
        for (std::uint32_t k = c * per; k < (c + 1) * per && k < count; ++k) {
            const std::int64_t total = static_cast<std::int64_t>(volumeMillibel) - headroomMillibel +
                                       bins.entries[k].volume;
            const std::int32_t clamped =
                static_cast<std::int32_t>(std::clamp<std::int64_t>(total, -1000000, 1000000));
            AddBin(bins.entries[k].bin, MixbinGain(clamped), row, outChannels, outChannelMask, reverb);
        }
    }
    out.reverbSend = srcChannels != 0 ? reverb / static_cast<float>(srcChannels) : 0.0f;
}

}  // namespace snd::ds
