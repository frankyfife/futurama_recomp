// I3DL2 reverb: guest structures, conversion to the XAudio2 reverb, the DSP effects image (M5 reverb agent;
// m5_audio.md 2.2, 3.6, 3.10, 6.1, 6.6; avi.md 2.1).
//
// The game sets the listener environment with IDirectSound_SetI3DL2Listener(ds, &DSI3DL2LISTENER, apply):
// once at init with "Padded Cell" (table 0x2D6818 entry 2, 0xCCDC7) and never again (no level script calls
// FsSetReverb, m5_audio.md 3.10); SoundManager::SetEnvironment(0..4) would pick Padded Cell, Room, Living
// Room, Stone Room, Concert Hall. The values are the standard I3DL2 presets (identical to
// XAUDIO2FX_I3DL2_PRESET_* of the SDK). Per voice, IDirectSoundBuffer/Stream_SetI3DL2Source(DSI3DL2BUFFER)
// sets the send (lRoom -600, 0xCC9DF), applied by the Spatializer (spatial_x3d.h).
//
// XAudio2 mapping (m5_audio.md 6.6): one reverb submix voice, ONE input channel (the Xbox has a single I3DL2
// mixbin, bin 10) and the XAudio2 reverb XAPO (XAudio2CreateReverb) as its effect, 100 % wet. The reverb
// supports mono->mono and mono->5.1 only (xaudio2fx.h), so the effect outputs 5.1 for every multi-channel
// output and the submix is sent to the master with ReverbOutputMatrix (5.1 as is, stereo = front + 0.7071 *
// (center + rear), LFE dropped). The submix runs at 20000..48000 Hz (ReverbSampleRate). Parameters come from
// ReverbConvertI3DL2ToNative with WetDryMix 100 after clamping the guest values to the I3DL2 ranges.
//
// The DSP effects image `XBOX\dsstdfx.bin` (25256 bytes on the disc, FutPAL00.img) reaches the host at
// runtime through IDirectSound_DownloadEffectsImage(ds, image, size, &loc{0, 1}, &desc) (0xCBF67). It holds
// the DSP code and state of 10 effects; loc names the I3DL2 reverb (index 0) and crosstalk (index 1) effects.
// The host does not run DSP code; ParseEffectsImage validates the image (layout of CMcpxAPU's loader
// FUN_0024A35F / FUN_0024A0F2) so the HLE can report whether the game's reverb would exist on the Xbox.
// Nothing of the image is stored in the repo.
#pragma once

#include "audio/audio_api.h"

#include <cstddef>
#include <cstdint>

#include <xaudio2fx.h>

namespace snd::reverb {

// ---- guest structures (packed little-endian layouts as the game passes them)
// --------------------------------------
#pragma pack(push, 1)
struct GuestI3dl2Listener {  // DSI3DL2LISTENER, 48 bytes (table 0x2D6818, m5_i3dl2.out)
    std::int32_t lRoom;
    std::int32_t lRoomHF;
    float flRoomRolloffFactor;
    float flDecayTime;
    float flDecayHFRatio;
    std::int32_t lReflections;
    float flReflectionsDelay;
    std::int32_t lReverb;
    float flReverbDelay;
    float flDiffusion;
    float flDensity;
    float flHFReference;
};
struct GuestI3dl2Buffer {  // DSI3DL2BUFFER, 36 bytes (0xCC9DF..0xCCA0E; DSOUND copies 9 DWORDs, 0x247DB5)
    std::int32_t lDirect;
    std::int32_t lDirectHF;
    std::int32_t lRoom;
    std::int32_t lRoomHF;
    float flRoomRolloffFactor;
    std::int32_t lObstructionHF;
    float flObstructionLFRatio;
    std::int32_t lOcclusionHF;
    float flOcclusionLFRatio;
};
#pragma pack(pop)
static_assert(sizeof(GuestI3dl2Listener) == 48);
static_assert(sizeof(GuestI3dl2Buffer) == 36);

// Copies a guest structure (any alignment). NULL -> false, `out` unchanged.
bool ReadI3dl2Listener(const void* guest, I3dl2Listener& out) noexcept;
bool ReadI3dl2Source(const void* guest, I3dl2Source& out) noexcept;

// The five environments of SoundManager::SetEnvironment (0xCBFB0..0xCC007; init = 0 Padded Cell) with the
// values of table 0x2D6818 (m5_i3dl2.out; equal to the SDK's XAUDIO2FX_I3DL2_PRESET_*). nullptr for other
// indices.
struct GameEnvironment {
    const char* name;
    I3dl2Listener listener;
};
const GameEnvironment* GameEnvironmentPreset(std::uint32_t index) noexcept;
inline constexpr std::uint32_t kGameEnvironmentCount = 5;

// ---- conversion
// -------------------------------------------------------------------------------------------------- Guest
// values clamped to the I3DL2 ranges of XAUDIO2FX_REVERB_I3DL2_PARAMETERS (NaN -> the Generic preset value).
I3dl2Listener ClampI3dl2(const I3dl2Listener& in) noexcept;
// The mapping of the SDK's ReverbConvertI3DL2ToNative (WetDryMix 100) applied to the clamped values, with
// log10 in double precision (the SDK inline gives HighEQGain 4 or 5 for Padded Cell depending on which
// overload of log10 its translation unit sees - use this function instead of the inline); `sevenDotOne` = the
// effect outputs 7.1 (RearDelay 20 ms instead of 5 ms). The result always passes ValidNative.
XAUDIO2FX_REVERB_PARAMETERS ToNative(const I3dl2Listener& listener, bool sevenDotOne = false) noexcept;
// Range check of every native field (the checks XAudio2's reverb applies in SetParameters).
bool ValidNative(const XAUDIO2FX_REVERB_PARAMETERS& p, bool sevenDotOne = false) noexcept;
// lRoom at -10000 mB: the environment is silent (I3DL2 "off", e.g. XAUDIO2FX_I3DL2_PRESET_DEFAULT).
bool IsSilent(const I3dl2Listener& listener) noexcept;

// ---- XAudio2 layout
// ------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kReverbInputChannels = 1;
// Effect output channels for the engine's output: 1 for mono output, 6 otherwise.
std::uint32_t ReverbEffectChannels(std::uint32_t outputChannels) noexcept;
// Submix rate: the output rate clamped to XAUDIO2FX_REVERB_MIN/MAX_FRAMERATE (20000..48000).
std::uint32_t ReverbSampleRate(std::uint32_t outputRate) noexcept;
// Gains from the effect's channels (ReverbEffectChannels) to the output channels, index src * outputChannels
// + dst (SourceVoice convention of audio_api.h; XAudio2's SetOutputMatrix wants [dst * src_count + src]). 6
// -> 6 identity (5.1 order FL FR FC LFE BL BR), 6 -> 2 front + 0.7071 * (center + rear), LFE dropped, 6 -> 8
// (7.1) FL FR FC LFE to the same channels and BL/BR to back and side pairs at 0.7071, 1 -> N to the front
// channels. Returns false for unsupported combinations (matrix zeroed).
bool ReverbOutputMatrix(std::uint32_t effectChannels, std::uint32_t outputChannels, float* matrix,
                        std::size_t matrixSize) noexcept;

// Creates the XAudio2 reverb XAPO (XAudio2CreateReverb) - for the engine's reverb submix effect chain.
HRESULT CreateReverbEffect(IUnknown** effect) noexcept;

// ---- DSP effects image (dsstdfx.bin)
// -------------------------------------------------------------------------------
inline constexpr std::uint32_t kMaxImageEffects = 32;
struct ImageEffect {
    std::uint32_t codeOffset = 0;  // file offsets of the DSP code / state (24-bit DSP words in 32-bit slots)
    std::uint32_t codeSize = 0;
    std::uint32_t stateOffset = 0;
    std::uint32_t stateSize = 0;
    std::uint32_t yMemoryOffset = 0;  // offsets into the DSP Y memory / scratch memory (not in the file)
    std::uint32_t yMemorySize = 0;
    std::uint32_t scratchOffset = 0;
    std::uint32_t scratchSize = 0;
};
struct EffectsImageInfo {
    std::uint32_t codeWords = 0;         // [0x804]
    std::uint32_t stateWords = 0;        // [0x80C]
    std::uint32_t descriptorOffset = 0;  // 0x818 + (codeWords + stateWords) * 4
    std::uint32_t effectCount = 0;
    std::uint32_t totalScratchSize = 0;
    ImageEffect effects[kMaxImageEffects] = {};
};
// DSEFFECTIMAGELOC as the game passes it ({0, 1}: I3DL2 reverb = effect 0, crosstalk = effect 1, 0xCBF67);
// NULL -> both 0xFFFFFFFF (FUN_00246F1C).
struct EffectsImageLoc {
    std::uint32_t i3dl2Index = 0xFFFFFFFFu;
    std::uint32_t crosstalkIndex = 0xFFFFFFFFu;
};
// Validates the image layout; false with a static `error` text if anything is out of range.
bool ParseEffectsImage(const void* image, std::size_t size, EffectsImageInfo& out,
                       const char** error = nullptr) noexcept;
// True if `loc.i3dl2Index` names an effect of the image that has delay-line scratch memory (an I3DL2 reverb).
bool ImageHasReverb(const EffectsImageInfo& info, const EffectsImageLoc& loc) noexcept;

}  // namespace snd::reverb
