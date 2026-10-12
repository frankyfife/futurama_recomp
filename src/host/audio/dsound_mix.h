// DSOUND 2D level model: buffer volume + headroom + per-mixbin volumes -> output matrix (M5 DSOUND core agent).
//
// Evidence (analysis/loader/m5_audio.md 3.6, m5_gain_model.out):
//   - DSOUND stores lVolume - headroom per voice (CDirectSoundVoice_SetVolume 0x24719B); the headroom is 600 mB for
//     every voice without DSBCAPS_CTRL3D (0x10) and 0 for 3D voices (0x248A63).
//   - SetMixBins stores the volume per bin number (FUN_00247080). Each channel of a voice serves
//     mixbinCount / channelCount bins in list order (FUN_0024C3CA `uVar6`): mono 2D -> bins 0 and 1, stereo 2D ->
//     left bin 0, right bin 1.
//   - Per bin the hardware attenuation is -(lVolume - headroom + mixbinVolume) in 1/64-dB register steps, clamped to
//     63.98 dB (snd::MixbinGain). So 2D voices are attenuated twice (buffer volume and the equal mixbin volume the
//     game sets from its pan) plus 6 dB: at the default slider 0.75 music plays at -11 dB.
//   - Without SetMixBins (XMV streams) DSOUND's default mixbins apply: mono {0, 1}, stereo {0}, {1}, each 0 mB
//     (0x2638AC / 0x2638B4).
// Speaker mapping of the Xbox's output bins to the host layout (ASSUMPTION: the Xbox's own stereo downmix of the
// centre/LFE/rear bins is not traced; the game's 2D voices only use bins 0 and 1): front L/R -> FL/FR; centre ->
// FC, or FL+FR at -3 dB; LFE -> LFE, dropped without one; back L/R -> BL/BR (or SL/SR), else FL/FR; crosstalk bins
// 6..9 like 0/1/4/5; I3DL2 bin 10 -> reverb send; FX send bins 11.. are not mapped (the game never uses them).
#pragma once

#include "audio/audio_api.h"

#include <cstdint>
#include <span>

namespace snd::ds {

struct MixBinVolume {
    std::uint32_t bin = 0;
    std::int32_t volume = 0;  // mB
};
inline constexpr std::uint32_t kMaxMixBins = 8;  // DSMIXBINS entries per voice that DSOUND accepts (XDK: 8 per voice)

struct MixBinList {
    std::uint32_t count = 0;
    MixBinVolume entries[kMaxMixBins] = {};
};
// DSOUND's defaults for a voice without SetMixBins.
MixBinList DefaultMixBins(std::uint32_t channels) noexcept;

// Output matrix [src][dst] (srcChannels x outChannels) of a non-3D voice and its reverb send (bin 10, normally 0).
// headroom: 600 for non-3D voices (kHeadroom2DMillibel).
struct Matrix2D {
    float levels[kMaxOutputChannels * kMaxOutputChannels] = {};
    float reverbSend = 0;
};
void Compute2D(std::int32_t volumeMillibel, std::int32_t headroomMillibel, const MixBinList& bins,
               std::uint32_t srcChannels, std::uint32_t outChannels, std::uint32_t outChannelMask,
               Matrix2D& out) noexcept;

// WAVEFORMATEXTENSIBLE speaker bits (ksmedia.h SPEAKER_*), defined here to keep mmreg/ksmedia out of the headers.
inline constexpr std::uint32_t kSpeakerFrontLeft = 0x1;
inline constexpr std::uint32_t kSpeakerFrontRight = 0x2;
inline constexpr std::uint32_t kSpeakerFrontCenter = 0x4;
inline constexpr std::uint32_t kSpeakerLowFrequency = 0x8;
inline constexpr std::uint32_t kSpeakerBackLeft = 0x10;
inline constexpr std::uint32_t kSpeakerBackRight = 0x20;
inline constexpr std::uint32_t kSpeakerSideLeft = 0x200;
inline constexpr std::uint32_t kSpeakerSideRight = 0x400;

// Index of a speaker bit (kSpeakerFrontLeft, ...) in the interleaved output of `mask`, -1 if absent.
int SpeakerIndex(std::uint32_t mask, std::uint32_t speakerBit) noexcept;

}  // namespace snd::ds
