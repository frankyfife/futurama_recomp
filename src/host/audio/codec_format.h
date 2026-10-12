// Guest sample formats, cursor arithmetic and the DSOUND pitch model (M5 codec agent;
// m5_audio.md 2.1, 2.3, 6.2).
//
// Formats: the game hands DSOUND a WAVEFORMATEX copied from the file's `fmt ` chunk (SoundData+0x34, cbSize
// forced to 0x12) or its own Xbox ADPCM format (SoundData+0x59, nAvgBytesPerSec = 36*ch*ch/64, wrong). Only
// wFormatTag, nChannels, nSamplesPerSec and wBitsPerSample are trusted; nBlockAlign is recomputed (trap 6,
// m5_audio.md 8).
//
// Pitch: DSOUND converts a frequency into an Xbox pitch, XAudioCalculatePitch 0x246ED1:
//   pitch = 0 for 48000 Hz, else round(4096 * log2((float)hz * (1/48000)))   (FUN_0024C022: x87 fistp)
// and the voice processor plays pitch + doppler pitch (3D) clamped to -0x7FFF..0x1FFF (FUN_0024C5C7), i.e.
// 187.5 Hz .. 191967 Hz at 1/4096-octave resolution. SetFrequency(0) means the buffer's own rate
// (CDirectSoundVoice_SetFrequency 0x247C42). XAudio2 resamples: ratio = 48000 * 2^(pitch/4096) / source rate.
#pragma once

#include "audio/audio_api.h"

#include <cstddef>
#include <cstdint>

namespace snd::codec {

// Guest WAVEFORMATEX (18 bytes, packed; may be unaligned in guest memory).
#pragma pack(push, 1)
struct GuestWaveFormat {
    std::uint16_t wFormatTag;
    std::uint16_t nChannels;
    std::uint32_t nSamplesPerSec;
    std::uint32_t nAvgBytesPerSec;  // not trusted
    std::uint16_t nBlockAlign;      // not trusted
    std::uint16_t wBitsPerSample;
    std::uint16_t cbSize;  // not trusted
};
#pragma pack(pop)
static_assert(sizeof(GuestWaveFormat) == 18);

inline constexpr std::uint32_t kMaxSourceChannels = 8;
inline constexpr std::uint32_t kMaxSourceRate = 192000;

// Reads a guest WAVEFORMATEX (copied byte-wise, so any alignment works). Returns false with a static `error`
// text for NULL, unknown tags, 0 or > 8 channels, a rate of 0 or > 192000 Hz, PCM widths other than 8/16
// bits. blockAlign is recomputed (PCM channels * bits / 8, ADPCM 36 * channels).
bool ParseWaveFormat(const void* guestWaveFormat, SourceFormat& out, const char** error = nullptr) noexcept;
// Same checks for an already filled SourceFormat (CreateDecoder uses it).
bool IsSupportedFormat(const SourceFormat& format) noexcept;
// Block size in source bytes (PCM: one frame) and frames per block (PCM 1, ADPCM 64); 0 for invalid formats.
std::uint32_t BlockBytes(const SourceFormat& format) noexcept;
std::uint32_t FramesPerBlock(const SourceFormat& format) noexcept;
// Byte offset <-> decoded frame: whole blocks only (rounded down).
std::uint64_t BytesToFrames(const SourceFormat& format, std::uint64_t bytes) noexcept;
std::uint64_t FramesToBytes(const SourceFormat& format, std::uint64_t frames) noexcept;

// ---- pitch
// ---------------------------------------------------------------------------------------------------------
inline constexpr std::int32_t kXboxPitchMin = -0x7FFF;
inline constexpr std::int32_t kXboxPitchMax = 0x1FFF;
inline constexpr std::uint32_t kXboxPitchReferenceRate = 48000;

// XAudioCalculatePitch (not clamped).
std::int32_t XboxPitchFromFrequency(std::uint32_t hz) noexcept;
std::int32_t ClampXboxPitch(std::int64_t pitch) noexcept;
// 48000 * 2^(pitch / 4096) for a clamped pitch.
double FrequencyFromXboxPitch(std::int32_t pitch) noexcept;
// XAudio2 frequency ratio for IDirectSoundBuffer/Stream_SetFrequency(requestedHz) on a source of `sourceRate`
// Hz, with `extraPitch` added (3D doppler, in Xbox pitch units) before the hardware clamp. requestedHz 0 =
// source rate. Exact Xbox quantization; returns 1 for an invalid source rate.
float FrequencyRatio(std::uint32_t requestedHz, std::uint32_t sourceRate,
                     std::int32_t extraPitch = 0) noexcept;
// The largest ratio the Xbox can play for a source rate (clamped pitch 0x1FFF), at least 1 - a safe
// XAUDIO2 MaxFrequencyRatio for voices whose frequency the guest may set freely (the game itself stays <= 4).
float MaxFrequencyRatio(std::uint32_t sourceRate) noexcept;
// Pitch units for a frequency ratio (doppler): round(4096 * log2(ratio)), 0 for ratio <= 0.
std::int32_t PitchFromRatio(double ratio) noexcept;

}  // namespace snd::codec
