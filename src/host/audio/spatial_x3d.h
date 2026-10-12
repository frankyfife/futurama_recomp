// The Spatializer of audio_api.h: DSOUND 3D levels (spatial_model.h) + X3DAudio direction (M5 3D agent;
// m5_audio.md 3.6, 3.7, 6.4, 6.5).
//
// Per emitter, Compute() produces (SpatialResult, matrix index = src * outputChannels + dst):
//   direct  dir[dst] * HwMixbinGain(lVolume + min(0, distance + lDirect)) * trim   for every non-LFE output
//   channel LFE     HwMixbinGain(lVolume + min(0, distance)) * trim on the LFE output channel (5.1/7.1 only;
//   the game's 3D
//           mixbins {6, 8, 7, 9, 10, 3} include bin 3, 0xCC35A); stereo output drops it (ASSUMPTION: Xbox
//           stereo downmix without LFE, m5_audio.md 6.4)
//   reverb  HwMixbinGain(lVolume + min(0, distance + lRoom)) * trim (mono send to the reverb submix)
// dir[] is X3DAudio's matrix for the emitter position in listener space (positional mode, spatial_model.h)
// with a flat volume curve (the distance law is DSOUND's own, exact), panned over the layout WITHOUT the front
// centre (the game's 3D mixbins have no centre bin; 5.1 = FL FR BL BR, 7.1 adds SL SR) and scaled to constant
// power with a ceiling: a source straight ahead plays at gain 1 on its loudest channel (stereo and 5.1 front =
// 1/1 on FL/FR, i.e. 0 dB per channel - 6 dB above a centred 2D voice of the same lVolume, which has 600 mB
// headroom: -6/-6 dB), every other direction carries the same total power unless a channel would exceed 1 (the
// 0 dB ceiling of an Xbox mixbin register): stereo hard left/right = 1 on the near channel (total power -3 dB
// against the front). On the Xbox, direction comes from HRTF filters plus the crosstalk DSP effect
// (dsstdfx.bin); that is not emulated, X3DAudio amplitude panning replaces it (m5_audio.md 6.5). A
// multi-channel emitter (never used by the game: 3D on a stereo sample ends the voice, 0xCC631) places every
// channel at the emitter position with 1/sqrt(channels). The emitter's InnerRadius is 0.5 m (0.5 /
// distanceFactor world units, 2.5 units with the game's factor 0.2) so a source passing through the listener
// does not jump from side to side.
//
// Threading: one instance per caller thread (the DSOUND HLE uses it on the guest thread only).
#pragma once

#include "audio/audio_api.h"
#include "audio/spatial_model.h"

#include <memory>

namespace snd::spatial {

struct SpatialSettings {
    PositionalMode positional = PositionalMode::Corrected;  // [audio] positional
    float trim3dDb = 0.0f;                                  // [audio] trim_3d (developer: 3D level vs. 2D)
};
// Reads [audio] positional / trim_3d (core::GetConfig(); defaults from the schema).
SpatialSettings SpatialSettingsFromConfig() noexcept;

std::unique_ptr<Spatializer> CreateSpatializer(const SpatialSettings& settings) noexcept;

// SPEAKER_* mask for an output channel count when the engine reports none (1 FC, 2 stereo, 4 quad, 6 = 5.1
// back, 8 = 7.1); 0 for other counts.
std::uint32_t DefaultSpeakerMask(std::uint32_t channels) noexcept;

}  // namespace snd::spatial
