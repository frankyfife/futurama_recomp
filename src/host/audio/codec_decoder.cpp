// snd::CreateDecoder (audio_api.h): guest PCM8 / PCM16 / Xbox ADPCM -> interleaved PCM16 (M5 codec agent).
//
// Evidence: m5_audio.md 4.5 (ADPCM variant), 6.2 (buffers: decode in SetBufferData), 6.3 (streams: decode per
// packet, a partial last block counts as zero-padded), 2.3 (the game's formats). Decoders are stateless
// (every Xbox ADPCM block carries its own predictor and step index), so one instance may decode packets in
// any order.
#include "audio/audio_api.h"
#include "audio/codec_adpcm.h"
#include "audio/codec_format.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace snd {
namespace {

class Pcm16Decoder final : public Decoder {
public:
    explicit Pcm16Decoder(const SourceFormat& f) noexcept : format_(f) {}
    const SourceFormat& Source() const noexcept override { return format_; }
    PcmFormat Output() const noexcept override { return {format_.channels, format_.sampleRate}; }
    std::size_t FramesFor(std::size_t inputBytes) const noexcept override {
        return inputBytes / (2u * format_.channels);
    }
    std::size_t Decode(std::span<const std::uint8_t> input, std::span<std::int16_t> out) noexcept override {
        const std::size_t frames = std::min(FramesFor(input.size()), out.size() / format_.channels);
        // Little-endian int16 on both sides; memcpy also handles unaligned guest data.
        std::memcpy(out.data(), input.data(), frames * 2u * format_.channels);
        return frames;
    }

private:
    SourceFormat format_;
};

class Pcm8Decoder final : public Decoder {
public:
    explicit Pcm8Decoder(const SourceFormat& f) noexcept : format_(f) {}
    const SourceFormat& Source() const noexcept override { return format_; }
    PcmFormat Output() const noexcept override { return {format_.channels, format_.sampleRate}; }
    std::size_t FramesFor(std::size_t inputBytes) const noexcept override {
        return inputBytes / format_.channels;
    }
    std::size_t Decode(std::span<const std::uint8_t> input, std::span<std::int16_t> out) noexcept override {
        const std::size_t frames = std::min(FramesFor(input.size()), out.size() / format_.channels);
        codec::ConvertPcm8(input.data(), frames * format_.channels, out.data());
        return frames;
    }

private:
    SourceFormat format_;
};

class XboxAdpcmDecoder final : public Decoder {
public:
    explicit XboxAdpcmDecoder(const SourceFormat& f) noexcept : format_(f) {}
    const SourceFormat& Source() const noexcept override { return format_; }
    PcmFormat Output() const noexcept override { return {format_.channels, format_.sampleRate}; }
    std::size_t FramesFor(std::size_t inputBytes) const noexcept override {
        return codec::XboxAdpcmFrames(inputBytes, format_.channels, true);
    }
    std::size_t Decode(std::span<const std::uint8_t> input, std::span<std::int16_t> out) noexcept override {
        return codec::DecodeXboxAdpcm(input.data(), input.size(), format_.channels, out.data(),
                                      out.size() / format_.channels, true);
    }

private:
    SourceFormat format_;
};

}  // namespace

std::unique_ptr<Decoder> CreateDecoder(const SourceFormat& format) noexcept {
    if (!codec::IsSupportedFormat(format)) {
        return nullptr;
    }
    SourceFormat f = format;
    f.blockAlign = static_cast<std::uint16_t>(codec::BlockBytes(format));  // never trust the guest's value
    if (f.formatTag == kFormatTagXboxAdpcm) {
        f.bitsPerSample = 4;
        return std::unique_ptr<Decoder>(new (std::nothrow) XboxAdpcmDecoder(f));
    }
    if (f.bitsPerSample == 8) {
        return std::unique_ptr<Decoder>(new (std::nothrow) Pcm8Decoder(f));
    }
    return std::unique_ptr<Decoder>(new (std::nothrow) Pcm16Decoder(f));
}

}  // namespace snd
