// I3DL2 reverb helpers (see reverb_i3dl2.h).
#include "audio/reverb_i3dl2.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace snd::reverb {
namespace {

// Table 0x2D6818 entries used by SetEnvironment(0..4) (m5_i3dl2.out "Presets used by SetEnvironment").
constexpr GameEnvironment kEnvironments[kGameEnvironmentCount] = {
    {"Padded Cell", {-1000, -6000, 0.0f, 0.17f, 0.10f, -1204, 0.001f, 207, 0.002f, 100.0f, 100.0f, 5000.0f}},
    {"Room", {-1000, -454, 0.0f, 0.40f, 0.83f, -1646, 0.002f, 53, 0.003f, 100.0f, 100.0f, 5000.0f}},
    {"Living Room",
     {-1000, -6000, 0.0f, 0.50f, 0.10f, -1376, 0.003f, -1104, 0.004f, 100.0f, 100.0f, 5000.0f}},
    {"Stone Room", {-1000, -300, 0.0f, 2.31f, 0.64f, -711, 0.012f, 83, 0.017f, 100.0f, 100.0f, 5000.0f}},
    {"Concert Hall", {-1000, -500, 0.0f, 3.92f, 0.70f, -1230, 0.020f, -2, 0.029f, 100.0f, 100.0f, 5000.0f}},
};

std::int32_t ClampI(std::int32_t v, std::int32_t lo, std::int32_t hi) noexcept {
    return std::clamp(v, lo, hi);
}
float ClampF(float v, float lo, float hi, float fallback) noexcept {
    if (!std::isfinite(v)) {
        return fallback;
    }
    return std::clamp(v, lo, hi);
}

std::uint32_t ReadU32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

}  // namespace

bool ReadI3dl2Listener(const void* guest, I3dl2Listener& out) noexcept {
    if (guest == nullptr) {
        return false;
    }
    GuestI3dl2Listener g;
    std::memcpy(&g, guest, sizeof(g));
    out.room = g.lRoom;
    out.roomHF = g.lRoomHF;
    out.roomRolloffFactor = g.flRoomRolloffFactor;
    out.decayTime = g.flDecayTime;
    out.decayHFRatio = g.flDecayHFRatio;
    out.reflections = g.lReflections;
    out.reflectionsDelay = g.flReflectionsDelay;
    out.reverb = g.lReverb;
    out.reverbDelay = g.flReverbDelay;
    out.diffusion = g.flDiffusion;
    out.density = g.flDensity;
    out.hfReference = g.flHFReference;
    return true;
}

bool ReadI3dl2Source(const void* guest, I3dl2Source& out) noexcept {
    if (guest == nullptr) {
        return false;
    }
    GuestI3dl2Buffer g;
    std::memcpy(&g, guest, sizeof(g));
    out.direct = g.lDirect;
    out.directHF = g.lDirectHF;
    out.room = g.lRoom;
    out.roomHF = g.lRoomHF;
    out.roomRolloffFactor = g.flRoomRolloffFactor;
    out.obstructionHF = g.lObstructionHF;
    out.obstructionLFRatio = g.flObstructionLFRatio;
    out.occlusionHF = g.lOcclusionHF;
    out.occlusionLFRatio = g.flOcclusionLFRatio;
    return true;
}

const GameEnvironment* GameEnvironmentPreset(std::uint32_t index) noexcept {
    return index < kGameEnvironmentCount ? &kEnvironments[index] : nullptr;
}

I3dl2Listener ClampI3dl2(const I3dl2Listener& in) noexcept {
    const I3dl2Listener generic;  // defaults of audio_api.h = I3DL2 "Generic"
    I3dl2Listener o;
    o.room = ClampI(in.room, -10000, 0);
    o.roomHF = ClampI(in.roomHF, -10000, 0);
    o.roomRolloffFactor = ClampF(in.roomRolloffFactor, 0.0f, 10.0f, generic.roomRolloffFactor);
    o.decayTime = ClampF(in.decayTime, 0.1f, 20.0f, generic.decayTime);
    o.decayHFRatio = ClampF(in.decayHFRatio, 0.1f, 2.0f, generic.decayHFRatio);
    o.reflections = ClampI(in.reflections, -10000, 1000);
    o.reflectionsDelay = ClampF(in.reflectionsDelay, 0.0f, 0.3f, generic.reflectionsDelay);
    o.reverb = ClampI(in.reverb, -10000, 2000);
    o.reverbDelay = ClampF(in.reverbDelay, 0.0f, 0.1f, generic.reverbDelay);
    o.diffusion = ClampF(in.diffusion, 0.0f, 100.0f, generic.diffusion);
    o.density = ClampF(in.density, 0.0f, 100.0f, generic.density);
    o.hfReference = ClampF(in.hfReference, 20.0f, 20000.0f, generic.hfReference);
    return o;
}

XAUDIO2FX_REVERB_PARAMETERS ToNative(const I3dl2Listener& listener, bool sevenDotOne) noexcept {
    const I3dl2Listener c = ClampI3dl2(listener);
    // Same mapping as the SDK's ReverbConvertI3DL2ToNative (xaudio2fx.h, Windows SDK 10.0.26100), written out
    // here because that inline function is not ODR-safe in C++: `log10(pI3DL2->DecayHFRatio)` binds to the
    // float overload in translation units that included <cmath> first and to the double function otherwise,
    // and the linker keeps one of the two copies (Padded Cell: HighEQGain 4 or 5). Here log10 is always
    // evaluated in double, as in C and in m5_i3dl2.out. RoomRolloffFactor is ignored like in the SDK (0 in
    // every game preset).
    XAUDIO2FX_REVERB_PARAMETERS native = {};
    native.RearDelay =
        sevenDotOne ? XAUDIO2FX_REVERB_DEFAULT_7POINT1_REAR_DELAY : XAUDIO2FX_REVERB_DEFAULT_REAR_DELAY;
    native.SideDelay = XAUDIO2FX_REVERB_DEFAULT_7POINT1_SIDE_DELAY;
    native.PositionLeft = XAUDIO2FX_REVERB_DEFAULT_POSITION;
    native.PositionRight = XAUDIO2FX_REVERB_DEFAULT_POSITION;
    native.PositionMatrixLeft = XAUDIO2FX_REVERB_DEFAULT_POSITION_MATRIX;
    native.PositionMatrixRight = XAUDIO2FX_REVERB_DEFAULT_POSITION_MATRIX;
    native.RoomSize = XAUDIO2FX_REVERB_DEFAULT_ROOM_SIZE;
    native.LowEQCutoff = 4;
    native.HighEQCutoff = 6;
    native.RoomFilterMain = static_cast<float>(c.room) / 100.0f;
    native.RoomFilterHF = static_cast<float>(c.roomHF) / 100.0f;
    const double ratioLog = std::log10(static_cast<double>(c.decayHFRatio));
    if (c.decayHFRatio >= 1.0f) {
        std::int32_t index = static_cast<std::int32_t>(-4.0 * ratioLog);
        index = std::max(index, -8);
        native.LowEQGain = static_cast<BYTE>(index < 0 ? index + 8 : 8);
        native.HighEQGain = 8;
        native.DecayTime = c.decayTime * c.decayHFRatio;
    } else {
        std::int32_t index = static_cast<std::int32_t>(4.0 * ratioLog);
        index = std::max(index, -8);
        native.LowEQGain = 8;
        native.HighEQGain = static_cast<BYTE>(index < 0 ? index + 8 : 8);
        native.DecayTime = c.decayTime;
    }
    float reflectionsDelay = c.reflectionsDelay * 1000.0f;
    if (reflectionsDelay >= static_cast<float>(XAUDIO2FX_REVERB_MAX_REFLECTIONS_DELAY)) {
        reflectionsDelay = static_cast<float>(XAUDIO2FX_REVERB_MAX_REFLECTIONS_DELAY - 1);
    } else if (reflectionsDelay <= 1.0f) {
        reflectionsDelay = 1.0f;
    }
    native.ReflectionsDelay = static_cast<UINT32>(reflectionsDelay);
    float reverbDelay = c.reverbDelay * 1000.0f;
    if (reverbDelay >= static_cast<float>(XAUDIO2FX_REVERB_MAX_REVERB_DELAY)) {
        reverbDelay = static_cast<float>(XAUDIO2FX_REVERB_MAX_REVERB_DELAY - 1);
    }
    native.ReverbDelay = static_cast<BYTE>(reverbDelay);
    native.ReflectionsGain = static_cast<float>(c.reflections) / 100.0f;
    native.ReverbGain = static_cast<float>(c.reverb) / 100.0f;
    native.EarlyDiffusion = static_cast<BYTE>(15.0f * c.diffusion / 100.0f);
    native.LateDiffusion = native.EarlyDiffusion;
    native.Density = c.density;
    native.RoomFilterFreq = c.hfReference;
    native.WetDryMix = 100.0f;  // the reverb submix carries only the wet signal (m5_audio.md 6.6)
    native.DisableLateField = FALSE;
    // Guard the few results the conversion does not bound itself.
    native.DecayTime = std::max(native.DecayTime, XAUDIO2FX_REVERB_MIN_DECAY_TIME);
    native.ReflectionsGain = std::clamp(native.ReflectionsGain, XAUDIO2FX_REVERB_MIN_REFLECTIONS_GAIN,
                                        XAUDIO2FX_REVERB_MAX_REFLECTIONS_GAIN);
    native.ReverbGain =
        std::clamp(native.ReverbGain, XAUDIO2FX_REVERB_MIN_REVERB_GAIN, XAUDIO2FX_REVERB_MAX_REVERB_GAIN);
    return native;
}

bool ValidNative(const XAUDIO2FX_REVERB_PARAMETERS& p, bool sevenDotOne) noexcept {
    const auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    const std::uint32_t maxRear =
        sevenDotOne ? XAUDIO2FX_REVERB_MAX_7POINT1_REAR_DELAY : XAUDIO2FX_REVERB_MAX_REAR_DELAY;
    return in(p.WetDryMix, XAUDIO2FX_REVERB_MIN_WET_DRY_MIX, XAUDIO2FX_REVERB_MAX_WET_DRY_MIX) &&
           p.ReflectionsDelay <= XAUDIO2FX_REVERB_MAX_REFLECTIONS_DELAY &&
           p.ReverbDelay <= XAUDIO2FX_REVERB_MAX_REVERB_DELAY && p.RearDelay <= maxRear &&
           p.SideDelay <= XAUDIO2FX_REVERB_MAX_7POINT1_SIDE_DELAY &&
           p.PositionLeft <= XAUDIO2FX_REVERB_MAX_POSITION &&
           p.PositionRight <= XAUDIO2FX_REVERB_MAX_POSITION &&
           p.PositionMatrixLeft <= XAUDIO2FX_REVERB_MAX_POSITION &&
           p.PositionMatrixRight <= XAUDIO2FX_REVERB_MAX_POSITION &&
           p.EarlyDiffusion <= XAUDIO2FX_REVERB_MAX_DIFFUSION &&
           p.LateDiffusion <= XAUDIO2FX_REVERB_MAX_DIFFUSION &&
           p.LowEQGain <= XAUDIO2FX_REVERB_MAX_LOW_EQ_GAIN &&
           p.LowEQCutoff <= XAUDIO2FX_REVERB_MAX_LOW_EQ_CUTOFF &&
           p.HighEQGain <= XAUDIO2FX_REVERB_MAX_HIGH_EQ_GAIN &&
           p.HighEQCutoff <= XAUDIO2FX_REVERB_MAX_HIGH_EQ_CUTOFF &&
           in(p.RoomFilterFreq, XAUDIO2FX_REVERB_MIN_ROOM_FILTER_FREQ,
              XAUDIO2FX_REVERB_MAX_ROOM_FILTER_FREQ) &&
           in(p.RoomFilterMain, XAUDIO2FX_REVERB_MIN_ROOM_FILTER_MAIN,
              XAUDIO2FX_REVERB_MAX_ROOM_FILTER_MAIN) &&
           in(p.RoomFilterHF, XAUDIO2FX_REVERB_MIN_ROOM_FILTER_HF, XAUDIO2FX_REVERB_MAX_ROOM_FILTER_HF) &&
           in(p.ReflectionsGain, XAUDIO2FX_REVERB_MIN_REFLECTIONS_GAIN,
              XAUDIO2FX_REVERB_MAX_REFLECTIONS_GAIN) &&
           in(p.ReverbGain, XAUDIO2FX_REVERB_MIN_REVERB_GAIN, XAUDIO2FX_REVERB_MAX_REVERB_GAIN) &&
           std::isfinite(p.DecayTime) && p.DecayTime >= XAUDIO2FX_REVERB_MIN_DECAY_TIME &&
           in(p.Density, XAUDIO2FX_REVERB_MIN_DENSITY, XAUDIO2FX_REVERB_MAX_DENSITY) &&
           in(p.RoomSize, XAUDIO2FX_REVERB_MIN_ROOM_SIZE, XAUDIO2FX_REVERB_MAX_ROOM_SIZE);
}

bool IsSilent(const I3dl2Listener& listener) noexcept {
    return listener.room <= -10000;
}

std::uint32_t ReverbEffectChannels(std::uint32_t outputChannels) noexcept {
    return outputChannels <= 1 ? 1u : 6u;
}

std::uint32_t ReverbSampleRate(std::uint32_t outputRate) noexcept {
    return std::clamp<std::uint32_t>(outputRate, XAUDIO2FX_REVERB_MIN_FRAMERATE,
                                     XAUDIO2FX_REVERB_MAX_FRAMERATE);
}

bool ReverbOutputMatrix(std::uint32_t effectChannels, std::uint32_t outputChannels, float* matrix,
                        std::size_t matrixSize) noexcept {
    if (matrix == nullptr || matrixSize < static_cast<std::size_t>(effectChannels) * outputChannels) {
        return false;
    }
    std::fill(matrix, matrix + static_cast<std::size_t>(effectChannels) * outputChannels, 0.0f);
    const auto at = [&](std::uint32_t src, std::uint32_t dst) -> float& {
        return matrix[src * outputChannels + dst];
    };
    constexpr float kHalfPower = 0.70710678f;
    if (effectChannels == 1) {
        if (outputChannels == 1) {
            at(0, 0) = 1.0f;
        } else {
            at(0, 0) = kHalfPower;  // mono reverb on the front pair at constant power
            at(0, 1) = kHalfPower;
        }
        return outputChannels >= 1;
    }
    if (effectChannels != 6) {
        return false;
    }
    // Effect channel order (5.1): 0 FL, 1 FR, 2 FC, 3 LFE, 4 BL, 5 BR.
    switch (outputChannels) {
    case 2:
        at(0, 0) = 1.0f;
        at(1, 1) = 1.0f;
        at(2, 0) = kHalfPower;
        at(2, 1) = kHalfPower;
        at(4, 0) = kHalfPower;
        at(5, 1) = kHalfPower;
        return true;
    case 6:
        for (std::uint32_t c = 0; c < 6; ++c) {
            at(c, c) = c == 3 ? 0.0f : 1.0f;
        }
        return true;
    case 8:  // 7.1 (FL FR FC LFE BL BR SL SR)
        at(0, 0) = 1.0f;
        at(1, 1) = 1.0f;
        at(2, 2) = 1.0f;
        at(4, 4) = kHalfPower;
        at(5, 5) = kHalfPower;
        at(4, 6) = kHalfPower;
        at(5, 7) = kHalfPower;
        return true;
    default:
        return false;
    }
}

HRESULT CreateReverbEffect(IUnknown** effect) noexcept {
    if (effect == nullptr) {
        return E_POINTER;
    }
    *effect = nullptr;
    return XAudio2CreateReverb(effect);
}

bool ParseEffectsImage(const void* image, std::size_t size, EffectsImageInfo& out,
                       const char** error) noexcept {
    const char* dummy = nullptr;
    const char*& err = error != nullptr ? *error : dummy;
    out = EffectsImageInfo{};
    // FUN_0024A35F copies the DWORDs at 0x800..0x814 to the APU and the DSP words from 0x818 on; FUN_0024A0F2
    // finds the descriptor at 0x818 + ([0x80C] + [0x804]) * 4: {count, total scratch, count x 8 DWORDs}, then
    // count x 8 bytes of signature data.
    constexpr std::size_t kHeader = 0x800;
    constexpr std::size_t kWords = 0x818;
    constexpr std::uint32_t kScratchBase = 0xC000;
    if (image == nullptr || size < kWords + 8) {
        err = "image too small";
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(image);
    out.codeWords = ReadU32(bytes + kHeader + 4);
    out.stateWords = ReadU32(bytes + kHeader + 0xC);
    const std::uint64_t descriptor =
        kWords +
        (static_cast<std::uint64_t>(out.codeWords) + static_cast<std::uint64_t>(out.stateWords)) * 4u;
    if (descriptor + 8 > size) {
        err = "descriptor outside the image";
        return false;
    }
    out.descriptorOffset = static_cast<std::uint32_t>(descriptor);
    out.effectCount = ReadU32(bytes + descriptor);
    out.totalScratchSize = ReadU32(bytes + descriptor + 4);
    if (out.effectCount == 0 || out.effectCount > kMaxImageEffects) {
        err = "effect count out of range";
        return false;
    }
    if (descriptor + 8 + 32ull * out.effectCount + 8ull * out.effectCount > size) {
        err = "effect table outside the image";
        return false;
    }
    const std::uint64_t codeEnd = kWords + 4ull * out.codeWords;
    for (std::uint32_t i = 0; i < out.effectCount; ++i) {
        const std::uint8_t* e = bytes + descriptor + 8 + 32ull * i;
        ImageEffect& fx = out.effects[i];
        fx.codeOffset = ReadU32(e + 0);
        fx.codeSize = ReadU32(e + 4);
        fx.stateOffset = ReadU32(e + 8);
        fx.stateSize = ReadU32(e + 12);
        fx.yMemoryOffset = ReadU32(e + 16);
        fx.yMemorySize = ReadU32(e + 20);
        fx.scratchOffset = ReadU32(e + 24);
        fx.scratchSize = ReadU32(e + 28);
        if (fx.codeOffset < kWords || static_cast<std::uint64_t>(fx.codeOffset) + fx.codeSize > codeEnd ||
            fx.codeSize == 0) {
            err = "effect code outside the code segment";
            return false;
        }
        if (fx.stateOffset < codeEnd ||
            static_cast<std::uint64_t>(fx.stateOffset) + fx.stateSize > descriptor) {
            err = "effect state outside the state segment";
            return false;
        }
        // Scratch offsets count from 0xC000 (FUN_0024A0F2 relocates them with base - 0xC000).
        if (fx.scratchSize != 0 &&
            (fx.scratchOffset < kScratchBase ||
             static_cast<std::uint64_t>(fx.scratchOffset - kScratchBase) + fx.scratchSize >
                 out.totalScratchSize)) {
            err = "effect scratch outside the scratch memory";
            return false;
        }
    }
    err = nullptr;
    return true;
}

bool ImageHasReverb(const EffectsImageInfo& info, const EffectsImageLoc& loc) noexcept {
    return loc.i3dl2Index < info.effectCount && loc.i3dl2Index < kMaxImageEffects &&
           info.effects[loc.i3dl2Index].scratchSize > 0;
}

}  // namespace snd::reverb
