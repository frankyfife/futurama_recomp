// Xbox ADPCM (WAVE_FORMAT 0x0069) and PCM8 sample conversion (M5 codec agent; m5_audio.md 4.5, 6.2, 6.3).
//
// Xbox ADPCM block (nBlockAlign = 36 * channels), per block:
//   header, per channel (4 bytes): int16 predictor, uint8 step index, uint8 reserved
//   data: 8 rounds; in each round 4 bytes per channel (8 nibbles, low nibble first)
// Output per channel and block: 64 samples = the header predictor + the first 63 decoded nibbles; the 64th
// nibble is decoded but not output (wSamplesPerBlock 64). Step: diff = ((2 * (n & 7) + 1) * step) >> 3, sign
// bit 3, predictor clamped to int16, index += kImaIndexTable[n] clamped to 0..88. This variant is bit-exact
// with ffmpeg adpcm_ima_xbox on 5 game files (analysis/loader/scripts/out/m5_adpcm_check.out); the classic
// bitwise IMA step formula differs by up to 28 LSB. Whether the Xbox hardware rounds like ffmpeg or like the
// shift-add form is not established (no hardware/xemu capture to compare). A header step index > 88 is clamped
// to 88 (ffmpeg rejects the packet; the game's data never has one, m5_assets.out).
//
// Blocks are independent (each carries its own predictor/index), so packets can be decoded separately
// (streams, m5_audio.md 6.3) and a block-aligned cursor needs no decoder state.
#pragma once

#include <cstddef>
#include <cstdint>

namespace snd::codec {

inline constexpr std::uint32_t kAdpcmBlockBytesPerChannel = 36;
inline constexpr std::uint32_t kAdpcmHeaderBytesPerChannel = 4;
inline constexpr std::uint32_t kAdpcmFramesPerBlock = 64;
inline constexpr std::uint32_t kAdpcmMaxStepIndex = 88;

extern const std::int16_t kImaStepTable[kAdpcmMaxStepIndex + 1];
extern const std::int8_t kImaIndexTable[16];

// One IMA nibble with the Xbox (ffmpeg adpcm_ima_xbox) step formula; updates predictor and index.
inline std::int16_t AdpcmExpandNibble(std::int32_t& predictor, std::int32_t& index,
                                      std::uint32_t nibble) noexcept {
    const std::int32_t step = kImaStepTable[index];
    const std::int32_t diff = ((2 * static_cast<std::int32_t>(nibble & 7u) + 1) * step) >> 3;
    predictor += (nibble & 8u) ? -diff : diff;
    if (predictor > 32767) {
        predictor = 32767;
    } else if (predictor < -32768) {
        predictor = -32768;
    }
    index += kImaIndexTable[nibble & 15u];
    if (index < 0) {
        index = 0;
    } else if (index > static_cast<std::int32_t>(kAdpcmMaxStepIndex)) {
        index = static_cast<std::int32_t>(kAdpcmMaxStepIndex);
    }
    return static_cast<std::int16_t>(predictor);
}

// Decodes ONE complete block (36 * channels bytes) into 64 interleaved frames (64 * channels samples).
void DecodeXboxAdpcmBlock(const std::uint8_t* block, std::uint32_t channels, std::int16_t* out) noexcept;

// Decodes `bytes` of Xbox ADPCM data. Complete blocks are decoded; a trailing partial block is decoded as if
// it was zero-padded to a full block when `padPartialBlock` is true and dropped otherwise. Writes at most
// `outFrames` frames (whole blocks only) and returns the frames written.
std::size_t DecodeXboxAdpcm(const std::uint8_t* data, std::size_t bytes, std::uint32_t channels,
                            std::int16_t* out, std::size_t outFrames, bool padPartialBlock) noexcept;

// Output frames DecodeXboxAdpcm produces for `bytes` of input.
std::size_t XboxAdpcmFrames(std::size_t bytes, std::uint32_t channels, bool padPartialBlock) noexcept;

// Unsigned 8-bit PCM -> int16: (x - 128) << 8 (exact, 0x80 -> 0).
void ConvertPcm8(const std::uint8_t* in, std::size_t samples, std::int16_t* out) noexcept;

}  // namespace snd::codec
