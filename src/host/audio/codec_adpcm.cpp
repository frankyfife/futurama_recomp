// Xbox ADPCM decoder and PCM8 conversion (see codec_adpcm.h).
#include "audio/codec_adpcm.h"

#include <algorithm>
#include <cstring>

namespace snd::codec {

// IMA ADPCM tables (IMA Digital Audio Focus and Technical Working Groups, 1992; identical in ffmpeg and in
// analysis/loader/scripts/m5_adpcm.py STEP/INDEX).
const std::int16_t kImaStepTable[kAdpcmMaxStepIndex + 1] = {
    7,    8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,   28,
    31,   34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,  118,
    130,  143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,  494,
    544,  598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878, 2066,
    2272, 2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const std::int8_t kImaIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

void DecodeXboxAdpcmBlock(const std::uint8_t* block, std::uint32_t channels, std::int16_t* out) noexcept {
    const std::uint8_t* data = block + kAdpcmHeaderBytesPerChannel * channels;
    for (std::uint32_t c = 0; c < channels; ++c) {
        const std::uint8_t* header = block + kAdpcmHeaderBytesPerChannel * c;
        std::int32_t predictor =
            static_cast<std::int16_t>(static_cast<std::uint16_t>(header[0] | (header[1] << 8)));
        std::int32_t index = std::min<std::int32_t>(header[2], static_cast<std::int32_t>(kAdpcmMaxStepIndex));
        out[c] = static_cast<std::int16_t>(predictor);
        // Nibble j (0..63) of this channel: round j / 8, byte (j % 8) / 2 of the channel's 4 bytes, low
        // nibble first. Nibble j becomes output sample j + 1; the 64th nibble (j = 63) is decoded but not
        // output.
        std::uint32_t j = 0;
        for (std::uint32_t round = 0; round < 8; ++round) {
            const std::uint8_t* bytes = data + (round * channels + c) * 4;
            for (std::uint32_t k = 0; k < 4; ++k) {
                const std::uint32_t byte = bytes[k];
                const std::int16_t lo = AdpcmExpandNibble(predictor, index, byte & 0x0Fu);
                out[(j + 1) * channels + c] = lo;
                ++j;
                const std::int16_t hi = AdpcmExpandNibble(predictor, index, byte >> 4);
                if (j < kAdpcmFramesPerBlock - 1) {
                    out[(j + 1) * channels + c] = hi;
                }
                ++j;
            }
        }
    }
}

std::size_t XboxAdpcmFrames(std::size_t bytes, std::uint32_t channels, bool padPartialBlock) noexcept {
    if (channels == 0) {
        return 0;
    }
    const std::size_t blockBytes = static_cast<std::size_t>(kAdpcmBlockBytesPerChannel) * channels;
    std::size_t blocks = bytes / blockBytes;
    if (padPartialBlock && bytes % blockBytes != 0) {
        ++blocks;
    }
    return blocks * kAdpcmFramesPerBlock;
}

std::size_t DecodeXboxAdpcm(const std::uint8_t* data, std::size_t bytes, std::uint32_t channels,
                            std::int16_t* out, std::size_t outFrames, bool padPartialBlock) noexcept {
    if (channels == 0 || data == nullptr || out == nullptr) {
        return 0;
    }
    const std::size_t blockBytes = static_cast<std::size_t>(kAdpcmBlockBytesPerChannel) * channels;
    const std::size_t fullBlocks = bytes / blockBytes;
    const std::size_t maxBlocks = outFrames / kAdpcmFramesPerBlock;
    const std::size_t blocks = std::min(fullBlocks, maxBlocks);
    for (std::size_t b = 0; b < blocks; ++b) {
        DecodeXboxAdpcmBlock(data + b * blockBytes, channels, out + b * kAdpcmFramesPerBlock * channels);
    }
    std::size_t frames = blocks * kAdpcmFramesPerBlock;
    const std::size_t rest = bytes - fullBlocks * blockBytes;
    if (padPartialBlock && rest != 0 && blocks == fullBlocks && blocks < maxBlocks) {
        // ASSUMPTION (m5_audio.md 6.3): an incomplete last block plays as if the missing bytes were zero. 36
        // * 8 channels = 288 bytes at most, so a stack copy is fine (Process may run on a guest fiber stack).
        std::uint8_t padded[kAdpcmBlockBytesPerChannel * 8] = {};
        if (blockBytes <= sizeof(padded)) {
            std::memcpy(padded, data + fullBlocks * blockBytes, rest);
            DecodeXboxAdpcmBlock(padded, channels, out + frames * channels);
            frames += kAdpcmFramesPerBlock;
        }
    }
    return frames;
}

void ConvertPcm8(const std::uint8_t* in, std::size_t samples, std::int16_t* out) noexcept {
    for (std::size_t i = 0; i < samples; ++i) {
        out[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(in[i]) - 128) * 256);
    }
}

}  // namespace snd::codec
