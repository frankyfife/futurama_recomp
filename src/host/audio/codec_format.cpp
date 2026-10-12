// Guest sample formats, cursor arithmetic and the DSOUND pitch model (see codec_format.h).
#include "audio/codec_format.h"

#include "audio/codec_adpcm.h"

#include <cmath>
#include <cstring>

namespace snd::codec {

bool IsSupportedFormat(const SourceFormat& format) noexcept {
    if (format.channels == 0 || format.channels > kMaxSourceChannels) {
        return false;
    }
    if (format.sampleRate == 0 || format.sampleRate > kMaxSourceRate) {
        return false;
    }
    if (format.formatTag == kFormatTagPcm) {
        return format.bitsPerSample == 8 || format.bitsPerSample == 16;
    }
    return format.formatTag == kFormatTagXboxAdpcm;
}

bool ParseWaveFormat(const void* guestWaveFormat, SourceFormat& out, const char** error) noexcept {
    const char* dummy = nullptr;
    const char*& err = error != nullptr ? *error : dummy;
    if (guestWaveFormat == nullptr) {
        err = "no format";
        return false;
    }
    GuestWaveFormat wfx;
    std::memcpy(&wfx, guestWaveFormat, sizeof(wfx));
    SourceFormat f;
    f.formatTag = wfx.wFormatTag;
    f.channels = wfx.nChannels;
    f.sampleRate = wfx.nSamplesPerSec;
    if (wfx.wFormatTag == kFormatTagXboxAdpcm) {
        f.bitsPerSample = 4;
        f.blockAlign = static_cast<std::uint16_t>(kAdpcmBlockBytesPerChannel * wfx.nChannels);
    } else if (wfx.wFormatTag == kFormatTagPcm) {
        f.bitsPerSample = wfx.wBitsPerSample;
        f.blockAlign = static_cast<std::uint16_t>(wfx.nChannels * (wfx.wBitsPerSample / 8u));
    } else {
        err = "unsupported wFormatTag (only 1 = PCM and 0x69 = Xbox ADPCM)";
        return false;
    }
    if (f.channels == 0 || f.channels > kMaxSourceChannels) {
        err = "unsupported channel count (1..8)";
        return false;
    }
    if (f.sampleRate == 0 || f.sampleRate > kMaxSourceRate) {
        err = "unsupported sample rate (1..192000 Hz)";
        return false;
    }
    if (f.formatTag == kFormatTagPcm && f.bitsPerSample != 8 && f.bitsPerSample != 16) {
        err = "unsupported PCM width (8 or 16 bits)";
        return false;
    }
    out = f;
    err = nullptr;
    return true;
}

std::uint32_t BlockBytes(const SourceFormat& format) noexcept {
    if (!IsSupportedFormat(format)) {
        return 0;
    }
    if (format.formatTag == kFormatTagXboxAdpcm) {
        return kAdpcmBlockBytesPerChannel * format.channels;
    }
    return format.channels * (format.bitsPerSample / 8u);
}

std::uint32_t FramesPerBlock(const SourceFormat& format) noexcept {
    if (!IsSupportedFormat(format)) {
        return 0;
    }
    return format.formatTag == kFormatTagXboxAdpcm ? kAdpcmFramesPerBlock : 1u;
}

std::uint64_t BytesToFrames(const SourceFormat& format, std::uint64_t bytes) noexcept {
    const std::uint32_t block = BlockBytes(format);
    return block == 0 ? 0 : (bytes / block) * FramesPerBlock(format);
}

std::uint64_t FramesToBytes(const SourceFormat& format, std::uint64_t frames) noexcept {
    const std::uint32_t perBlock = FramesPerBlock(format);
    return perBlock == 0 ? 0 : (frames / perBlock) * BlockBytes(format);
}

std::int32_t XboxPitchFromFrequency(std::uint32_t hz) noexcept {
    if (hz == kXboxPitchReferenceRate) {
        return 0;
    }
    if (hz == 0) {
        return kXboxPitchMin;  // log2(0): the x87 result is -inf, fistp gives the integer indefinite; never
                               // used
    }
    // The ratio is passed to FUN_0024C022 as a float32 (fVar1 * 2.0833333e-05 at 0x246EF8), log2 and the
    // scaling run in extended precision, fistp rounds to nearest even.
    const float ratio = static_cast<float>(hz) * 2.0833333e-05f;
    return static_cast<std::int32_t>(std::nearbyint(4096.0L * std::log2(static_cast<long double>(ratio))));
}

std::int32_t ClampXboxPitch(std::int64_t pitch) noexcept {
    if (pitch < kXboxPitchMin) {
        return kXboxPitchMin;
    }
    if (pitch > kXboxPitchMax) {
        return kXboxPitchMax;
    }
    return static_cast<std::int32_t>(pitch);
}

double FrequencyFromXboxPitch(std::int32_t pitch) noexcept {
    return static_cast<double>(kXboxPitchReferenceRate) *
           std::exp2(static_cast<double>(ClampXboxPitch(pitch)) / 4096.0);
}

float FrequencyRatio(std::uint32_t requestedHz, std::uint32_t sourceRate, std::int32_t extraPitch) noexcept {
    if (sourceRate == 0) {
        return 1.0f;
    }
    const std::uint32_t hz = requestedHz == 0 ? sourceRate : requestedHz;
    const std::int32_t pitch = ClampXboxPitch(static_cast<std::int64_t>(XboxPitchFromFrequency(hz)) +
                                              static_cast<std::int64_t>(extraPitch));
    return static_cast<float>(FrequencyFromXboxPitch(pitch) / static_cast<double>(sourceRate));
}

float MaxFrequencyRatio(std::uint32_t sourceRate) noexcept {
    if (sourceRate == 0) {
        return 1.0f;
    }
    const double ratio = FrequencyFromXboxPitch(kXboxPitchMax) / static_cast<double>(sourceRate);
    return ratio < 1.0 ? 1.0f : static_cast<float>(ratio);
}

std::int32_t PitchFromRatio(double ratio) noexcept {
    if (!(ratio > 0.0) || !std::isfinite(ratio)) {
        return 0;
    }
    return ClampXboxPitch(static_cast<std::int64_t>(std::nearbyint(4096.0 * std::log2(ratio))));
}

}  // namespace snd::codec
