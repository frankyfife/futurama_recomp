// Unit tests of the M5 codec module (audio/codec_*.h, snd::CreateDecoder): Xbox ADPCM, PCM8/PCM16, guest
// formats and the DSOUND pitch model. Synthetic signals only; the optional case on real game files reads them
// from a directory under run\ named by FUT_AUDIO_CODEC_REFS (analysis/loader/scripts/m5_codec_refs.py) and is
// skipped otherwise.
#include "doctest.h"

#include "audio/audio_api.h"
#include "audio/codec_adpcm.h"
#include "audio/codec_format.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Same LCG as analysis/loader/scripts/m5_codec_golden.py (byte = state >> 24 after each step).
std::vector<std::uint8_t> LcgBytes(std::uint32_t seed, std::size_t n) {
    std::vector<std::uint8_t> out(n);
    std::uint32_t x = seed;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        out[i] = static_cast<std::uint8_t>(x >> 24);
    }
    return out;
}

// Random blocks with valid headers (step index % 89 unless clampIndex is false, reserved byte 0).
std::vector<std::uint8_t> RandomBlocks(std::uint32_t seed, std::uint32_t channels, std::uint32_t blocks,
                                       bool limitIndex = true) {
    std::vector<std::uint8_t> raw = LcgBytes(seed, 36u * channels * blocks);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            const std::size_t o = static_cast<std::size_t>(b) * 36u * channels + 4u * c;
            if (limitIndex) {
                raw[o + 2] = static_cast<std::uint8_t>(raw[o + 2] % 89u);
            }
            raw[o + 3] = 0;
        }
    }
    return raw;
}

std::uint64_t Fnv1a(const std::vector<std::int16_t>& samples) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::int16_t s : samples) {
        const auto u = static_cast<std::uint16_t>(s);
        for (std::uint8_t b : {static_cast<std::uint8_t>(u & 0xFF), static_cast<std::uint8_t>(u >> 8)}) {
            h ^= b;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

// Independent straight-line reference of analysis/loader/scripts/m5_adpcm.py decode(variant 64, EXACT): per
// channel all 64 nibbles in order, then the first 63 kept.
std::vector<std::int16_t> ReferenceDecode(const std::vector<std::uint8_t>& data, std::uint32_t channels) {
    static const int kStep[89] = {
        7,    8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,   28,
        31,   34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,  118,
        130,  143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,  494,
        544,  598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878, 2066,
        2272, 2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845, 8630,
        9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    static const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
    const std::size_t blockBytes = 36u * channels;
    const std::size_t blocks = data.size() / blockBytes;
    std::vector<std::vector<int>> perChannel(channels);
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::uint8_t* blk = data.data() + b * blockBytes;
        for (std::uint32_t c = 0; c < channels; ++c) {
            int pred = static_cast<std::int16_t>(blk[4 * c] | (blk[4 * c + 1] << 8));
            int idx = blk[4 * c + 2] > 88 ? 88 : blk[4 * c + 2];
            std::vector<int> nibbles;
            for (int round = 0; round < 8; ++round) {
                for (int k = 0; k < 4; ++k) {
                    const std::uint8_t byte = blk[4 * channels + (round * channels + c) * 4 + k];
                    nibbles.push_back(byte & 15);
                    nibbles.push_back(byte >> 4);
                }
            }
            perChannel[c].push_back(pred);
            for (int j = 0; j < 64; ++j) {
                const int n = nibbles[j];
                const int step = kStep[idx];
                const int diff = ((2 * (n & 7) + 1) * step) >> 3;
                pred = (n & 8) ? pred - diff : pred + diff;
                pred = pred < -32768 ? -32768 : (pred > 32767 ? 32767 : pred);
                idx += kIndex[n];
                idx = idx < 0 ? 0 : (idx > 88 ? 88 : idx);
                if (j < 63) {
                    perChannel[c].push_back(pred);
                }
            }
        }
    }
    std::vector<std::int16_t> out;
    for (std::size_t i = 0; i < blocks * 64; ++i) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            out.push_back(static_cast<std::int16_t>(perChannel[c][i]));
        }
    }
    return out;
}

// Greedy IMA encoder producing Xbox ADPCM blocks (port of m5_adpcm.encode, for round-trip tests).
std::vector<std::uint8_t> Encode(const std::vector<std::vector<int>>& samples) {
    const std::uint32_t channels = static_cast<std::uint32_t>(samples.size());
    const std::size_t n = samples[0].size();
    std::vector<std::uint8_t> out;
    std::vector<std::int32_t> pred(channels), idx(channels, 0);
    for (std::size_t base = 0; base + 64 <= n; base += 64) {
        std::vector<std::vector<std::uint8_t>> nib(channels);
        for (std::uint32_t c = 0; c < channels; ++c) {
            pred[c] = samples[c][base];
            const auto p = static_cast<std::uint16_t>(static_cast<std::int16_t>(samples[c][base]));
            out.push_back(static_cast<std::uint8_t>(p & 0xFF));
            out.push_back(static_cast<std::uint8_t>(p >> 8));
            out.push_back(static_cast<std::uint8_t>(idx[c]));
            out.push_back(0);
        }
        for (std::uint32_t c = 0; c < channels; ++c) {
            for (std::size_t k = 0; k < 64; ++k) {
                const int target = samples[c][base + (k < 63 ? k + 1 : 63)];
                const int step = snd::codec::kImaStepTable[idx[c]];
                int diff = target - pred[c];
                std::uint32_t code = 0;
                if (diff < 0) {
                    code = 8;
                    diff = -diff;
                }
                if (diff >= step) {
                    code |= 4;
                    diff -= step;
                }
                if (diff >= step >> 1) {
                    code |= 2;
                    diff -= step >> 1;
                }
                if (diff >= step >> 2) {
                    code |= 1;
                }
                snd::codec::AdpcmExpandNibble(pred[c], idx[c], code);
                nib[c].push_back(static_cast<std::uint8_t>(code));
            }
        }
        for (int r = 0; r < 8; ++r) {
            for (std::uint32_t c = 0; c < channels; ++c) {
                for (int k = 0; k < 4; ++k) {
                    const int i = r * 8 + 2 * k;
                    out.push_back(static_cast<std::uint8_t>(nib[c][i] | (nib[c][i + 1] << 4)));
                }
            }
        }
    }
    return out;
}

std::vector<std::int16_t> DecodeAll(const snd::SourceFormat& f, const std::vector<std::uint8_t>& data) {
    auto dec = snd::CreateDecoder(f);
    REQUIRE(dec != nullptr);
    std::vector<std::int16_t> out(dec->FramesFor(data.size()) * f.channels);
    const std::size_t frames = dec->Decode(data, out);
    REQUIRE(frames == dec->FramesFor(data.size()));
    return out;
}

snd::SourceFormat AdpcmFormat(std::uint16_t channels, std::uint32_t rate = 22050) {
    snd::SourceFormat f;
    f.formatTag = snd::kFormatTagXboxAdpcm;
    f.channels = channels;
    f.sampleRate = rate;
    f.bitsPerSample = 4;
    f.blockAlign = static_cast<std::uint16_t>(36 * channels);
    return f;
}

}  // namespace

TEST_CASE("codec: Xbox ADPCM matches ffmpeg adpcm_ima_xbox on synthetic random blocks (golden hashes)") {
    // Hashes from analysis/loader/scripts/m5_codec_golden.py: PyAV/ffmpeg adpcm_ima_xbox decoded the same LCG
    // blocks (mono and stereo; ffmpeg's decoder only supports 1-2 channels) and agreed with m5_adpcm.py
    // sample for sample.
    const auto mono = DecodeAll(AdpcmFormat(1), RandomBlocks(1, 1, 40));
    CHECK(mono.size() == 2560);
    CHECK(mono[0] == 24124);
    CHECK(mono[1] == 23745);
    CHECK(mono[5] == 25662);
    CHECK(Fnv1a(mono) == 0x46285B1CCF5778E2ull);

    const auto stereo = DecodeAll(AdpcmFormat(2), RandomBlocks(2, 2, 40));
    CHECK(stereo.size() == 5120);
    CHECK(stereo[0] == 30012);  // L
    CHECK(stereo[1] == 7863);   // R
    CHECK(stereo[4] == 31028);
    CHECK(Fnv1a(stereo) == 0xFA7567F4FAD042B0ull);

    // 6 channels and step indices > 88 (clamped) against the Python reference only.
    CHECK(Fnv1a(DecodeAll(AdpcmFormat(6), RandomBlocks(3, 6, 10))) == 0x57814DC3770D7721ull);
    CHECK(Fnv1a(DecodeAll(AdpcmFormat(1), RandomBlocks(4, 1, 20, false))) == 0x110BDAEAEA05126Eull);
}

TEST_CASE("codec: Xbox ADPCM block decoder equals the straight-line reference for 1..8 channels") {
    for (std::uint32_t ch = 1; ch <= 8; ++ch) {
        const auto data = RandomBlocks(100 + ch, ch, 17, ch % 2 == 0);
        CAPTURE(ch);
        CHECK(DecodeAll(AdpcmFormat(static_cast<std::uint16_t>(ch)), data) == ReferenceDecode(data, ch));
    }
}

TEST_CASE("codec: Xbox ADPCM header sample, clamping and silence") {
    // Header predictor is output sample 0 of every block; zero data with index 0 stays at the predictor
    // (nibble 0: diff = step >> 3 = 0 for step 7).
    std::vector<std::uint8_t> block(36, 0);
    block[0] = 0x34;
    block[1] = 0x12;  // predictor 0x1234
    std::vector<std::int16_t> out(64);
    snd::codec::DecodeXboxAdpcmBlock(block.data(), 1, out.data());
    for (std::int16_t s : out) {
        CHECK(s == 0x1234);
    }
    // All-zero block = digital silence (a zero-padded packet tail plays silent).
    std::vector<std::uint8_t> zero(72, 0);
    std::vector<std::int16_t> z(128, 1);
    CHECK(snd::codec::DecodeXboxAdpcm(zero.data(), zero.size(), 1, z.data(), 128, false) == 128);
    for (std::int16_t s : z) {
        CHECK(s == 0);
    }
    // Saturation: predictor 32767, max index, nibbles 7 (positive) -> stays at 32767; 0xF -> towards -32768.
    std::vector<std::uint8_t> sat(36, 0x77);
    sat[0] = 0xFF;
    sat[1] = 0x7F;
    sat[2] = 88;
    sat[3] = 0;
    snd::codec::DecodeXboxAdpcmBlock(sat.data(), 1, out.data());
    for (std::int16_t s : out) {
        CHECK(s == 32767);
    }
    std::fill(sat.begin() + 4, sat.end(), static_cast<std::uint8_t>(0xFF));
    snd::codec::DecodeXboxAdpcmBlock(sat.data(), 1, out.data());
    CHECK(out[0] == 32767);
    CHECK(out[63] == -32768);
}

TEST_CASE("codec: Xbox ADPCM partial blocks, frame counts and output limits") {
    const auto data = RandomBlocks(7, 2, 3);  // 216 bytes
    CHECK(snd::codec::XboxAdpcmFrames(216, 2, false) == 192);
    CHECK(snd::codec::XboxAdpcmFrames(216 + 10, 2, false) == 192);
    CHECK(snd::codec::XboxAdpcmFrames(216 + 10, 2, true) == 256);
    CHECK(snd::codec::XboxAdpcmFrames(0, 2, true) == 0);

    // Decoder (packets): a trailing partial block counts as zero-padded.
    auto dec = snd::CreateDecoder(AdpcmFormat(2));
    REQUIRE(dec);
    std::vector<std::uint8_t> partial(data.begin(), data.begin() + 72 + 30);
    CHECK(dec->FramesFor(partial.size()) == 128);
    std::vector<std::int16_t> out(128 * 2);
    CHECK(dec->Decode(partial, out) == 128);
    std::vector<std::uint8_t> padded(partial);
    padded.resize(144, 0);
    std::vector<std::int16_t> expect(128 * 2);
    CHECK(dec->Decode(padded, expect) == 128);
    CHECK(out == expect);

    // Output limited to whole blocks.
    std::vector<std::int16_t> small(100 * 2);
    CHECK(dec->Decode(data, small) == 64);
    CHECK(snd::codec::DecodeXboxAdpcm(data.data(), data.size(), 0, small.data(), 100, true) == 0);
}

TEST_CASE("codec: Xbox ADPCM synthetic round trip (encoder from m5_adpcm.py) keeps a sine at > 25 dB SNR") {
    for (std::uint32_t ch : {1u, 2u}) {
        std::vector<std::vector<int>> sig(ch);
        const std::size_t n = 64 * 172;
        for (std::uint32_t c = 0; c < ch; ++c) {
            for (std::size_t i = 0; i < n; ++i) {
                const double f = c == 0 ? 1000.0 : 440.0;
                sig[c].push_back(
                    static_cast<int>(std::sin(2.0 * 3.141592653589793 * f * i / 22050.0) * 12000.0));
            }
        }
        const auto enc = Encode(sig);
        CHECK(enc.size() == 36u * ch * 172u);
        const auto dec = DecodeAll(AdpcmFormat(static_cast<std::uint16_t>(ch)), enc);
        REQUIRE(dec.size() == n * ch);
        double err = 0;
        double pow = 0;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::uint32_t c = 0; c < ch; ++c) {
                const double d = dec[i * ch + c] - sig[c][i];
                err += d * d;
                pow += static_cast<double>(sig[c][i]) * sig[c][i];
            }
        }
        const double snr = 10.0 * std::log10(pow / std::max(err, 1e-9));
        CAPTURE(ch);
        CAPTURE(snr);
        CHECK(snr > 25.0);
    }
}

TEST_CASE("codec: PCM8 and PCM16 decoders") {
    snd::SourceFormat f8;
    f8.formatTag = snd::kFormatTagPcm;
    f8.channels = 1;
    f8.sampleRate = 11025;
    f8.bitsPerSample = 8;
    f8.blockAlign = 99;  // wrong on purpose: never trusted
    auto d8 = snd::CreateDecoder(f8);
    REQUIRE(d8);
    CHECK(d8->Source().blockAlign == 1);
    const std::vector<std::uint8_t> in8 = {0x00, 0x80, 0xFF, 0x81, 0x7F};
    std::vector<std::int16_t> out8(5);
    CHECK(d8->Decode(in8, out8) == 5);
    CHECK(out8 == std::vector<std::int16_t>{-32768, 0, 32512, 256, -256});
    CHECK(d8->Output().channels == 1);
    CHECK(d8->Output().sampleRate == 11025);

    snd::SourceFormat f16 = f8;
    f16.channels = 2;
    f16.bitsPerSample = 16;
    auto d16 = snd::CreateDecoder(f16);
    REQUIRE(d16);
    // Unaligned input, a trailing partial frame (3 bytes) is dropped.
    std::vector<std::uint8_t> raw = {0xAA, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x80, 0x34, 0x12, 0x99};
    std::vector<std::uint8_t> in16(raw.begin() + 1, raw.end());  // 9 bytes: 2 frames + 1 byte
    CHECK(d16->FramesFor(in16.size()) == 2);
    in16.insert(in16.end(), {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});  // 16 bytes = 4 frames
    std::vector<std::int16_t> out16(8);
    CHECK(d16->Decode(std::span<const std::uint8_t>(in16.data(), in16.size()), out16) == 4);
    CHECK(out16[0] == 1);
    CHECK(out16[1] == -1);
    CHECK(out16[2] == static_cast<std::int16_t>(0x8000));
    CHECK(out16[3] == 0x1234);

    snd::SourceFormat bad = f16;
    bad.bitsPerSample = 24;
    CHECK(snd::CreateDecoder(bad) == nullptr);
    bad = f16;
    bad.channels = 0;
    CHECK(snd::CreateDecoder(bad) == nullptr);
    bad = f16;
    bad.formatTag = 2;  // MS ADPCM
    CHECK(snd::CreateDecoder(bad) == nullptr);
}

TEST_CASE("codec: guest WAVEFORMATEX parsing ignores nAvgBytesPerSec, nBlockAlign and cbSize") {
    // The game's Xbox ADPCM format at SoundData+0x59 (unaligned): nAvgBytesPerSec = 36*ch*ch/64 (wrong),
    // cbSize 2.
    std::uint8_t buf[1 + 18] = {};
    snd::codec::GuestWaveFormat g = {0x69, 2, 22050, 36 * 2 * 2 / 64, 72, 4, 2};
    std::memcpy(buf + 1, &g, sizeof(g));
    snd::SourceFormat f;
    const char* error = "unset";
    REQUIRE(snd::codec::ParseWaveFormat(buf + 1, f, &error));
    CHECK(error == nullptr);
    CHECK(f.formatTag == 0x69);
    CHECK(f.channels == 2);
    CHECK(f.sampleRate == 22050);
    CHECK(f.blockAlign == 72);
    CHECK(f.bitsPerSample == 4);
    CHECK(snd::codec::BlockBytes(f) == 72);
    CHECK(snd::codec::FramesPerBlock(f) == 64);
    CHECK(snd::codec::BytesToFrames(f, 72 * 10 + 5) == 640);
    CHECK(snd::codec::FramesToBytes(f, 640 + 63) == 720);

    // The PCM copy of the fmt chunk at SoundData+0x34 with cbSize forced to 0x12 and a bogus block align.
    g = {1, 2, 22050, 88200, 1, 16, 0x12};
    REQUIRE(snd::codec::ParseWaveFormat(&g, f, &error));
    CHECK(f.blockAlign == 4);
    CHECK(snd::codec::BytesToFrames(f, 26460) == 6615);

    g = {0x11, 1, 22050, 0, 0, 4, 2};  // IMA ADPCM (not Xbox)
    CHECK_FALSE(snd::codec::ParseWaveFormat(&g, f, &error));
    CHECK(error != nullptr);
    g = {1, 9, 22050, 0, 0, 16, 0};
    CHECK_FALSE(snd::codec::ParseWaveFormat(&g, f, &error));
    g = {1, 1, 0, 0, 0, 16, 0};
    CHECK_FALSE(snd::codec::ParseWaveFormat(&g, f, &error));
    CHECK_FALSE(snd::codec::ParseWaveFormat(nullptr, f, &error));
}

TEST_CASE("codec: DSOUND pitch model (XAudioCalculatePitch, hardware clamp) and frequency ratios") {
    using namespace snd::codec;
    // Reference values: round(4096 * log2((float)hz / 48000)) (analysis/loader/scripts/m5_codec_golden.py).
    CHECK(XboxPitchFromFrequency(48000) == 0);
    CHECK(XboxPitchFromFrequency(24000) == -4096);
    CHECK(XboxPitchFromFrequency(96000) == 4096);
    CHECK(XboxPitchFromFrequency(22050) == -4597);
    CHECK(XboxPitchFromFrequency(44100) == -501);
    CHECK(XboxPitchFromFrequency(11025) == -8693);
    CHECK(XboxPitchFromFrequency(1000) == -22876);
    CHECK(XboxPitchFromFrequency(191999) == 8192);
    CHECK(ClampXboxPitch(8192) == 0x1FFF);
    CHECK(ClampXboxPitch(-40000) == -0x7FFF);

    // SetFrequency(0) = the buffer's own rate; quantization error below 1/4096 octave.
    CHECK(FrequencyRatio(0, 22050) == doctest::Approx(1.0).epsilon(1.7e-4));
    CHECK(FrequencyRatio(22050, 22050) == doctest::Approx(1.0).epsilon(1.7e-4));
    CHECK(FrequencyRatio(48000, 48000) == 1.0f);
    // pitch 4 (the game's maximum, 0xCA110) of a 22050 Hz sample: 88200 Hz.
    CHECK(FrequencyRatio(88200, 22050) == doctest::Approx(4.0).epsilon(1.7e-4));
    // above the hardware maximum (191967 Hz) the ratio saturates.
    CHECK(FrequencyRatio(400000, 48000) == doctest::Approx(FrequencyFromXboxPitch(0x1FFF) / 48000.0));
    CHECK(FrequencyRatio(400000, 48000) < 4.0f);
    CHECK(MaxFrequencyRatio(48000) == doctest::Approx(3.99932).epsilon(1e-5));
    CHECK(MaxFrequencyRatio(11025) == doctest::Approx(17.412).epsilon(1e-3));
    CHECK(MaxFrequencyRatio(0) == 1.0f);
    // Doppler pitch offset is added before the clamp.
    CHECK(FrequencyRatio(48000, 48000, 4096) == doctest::Approx(2.0).epsilon(1e-6));
    CHECK(FrequencyRatio(48000, 48000, 100000) == doctest::Approx(FrequencyFromXboxPitch(0x1FFF) / 48000.0));
    CHECK(PitchFromRatio(2.0) == 4096);
    CHECK(PitchFromRatio(0.5) == -4096);
    CHECK(PitchFromRatio(1.0) == 0);
    CHECK(PitchFromRatio(0.0) == 0);
}

TEST_CASE("codec: real game files decode bit-exact like ffmpeg (FUT_AUDIO_CODEC_REFS, optional)") {
    const char* dir = std::getenv("FUT_AUDIO_CODEC_REFS");
    if (dir == nullptr || !std::filesystem::is_directory(dir)) {
        MESSAGE("skipped: FUT_AUDIO_CODEC_REFS not set (analysis/loader/scripts/m5_codec_refs.py writes the "
                "files)");
        return;
    }
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".wav") {
            continue;
        }
        std::ifstream wavFile(entry.path(), std::ios::binary);
        std::vector<std::uint8_t> wav((std::istreambuf_iterator<char>(wavFile)),
                                      std::istreambuf_iterator<char>());
        std::filesystem::path pcmPath = entry.path();
        pcmPath.replace_extension(".pcm");
        std::ifstream pcmFile(pcmPath, std::ios::binary);
        std::vector<std::uint8_t> pcm((std::istreambuf_iterator<char>(pcmFile)),
                                      std::istreambuf_iterator<char>());
        CAPTURE(entry.path().filename().string());
        REQUIRE(wav.size() > 12);
        // RIFF walk (fmt + data).
        std::size_t o = 12;
        const std::uint8_t* fmt = nullptr;
        std::vector<std::uint8_t> data;
        while (o + 8 <= wav.size()) {
            std::uint32_t size;
            std::memcpy(&size, wav.data() + o + 4, 4);
            if (std::memcmp(wav.data() + o, "fmt ", 4) == 0) {
                fmt = wav.data() + o + 8;
            } else if (std::memcmp(wav.data() + o, "data", 4) == 0) {
                const std::size_t n = std::min<std::size_t>(size, wav.size() - o - 8);
                data.assign(wav.begin() + static_cast<std::ptrdiff_t>(o + 8),
                            wav.begin() + static_cast<std::ptrdiff_t>(o + 8 + n));
                break;
            }
            o += 8 + size + (size & 1);
        }
        REQUIRE(fmt != nullptr);
        snd::SourceFormat f;
        REQUIRE(snd::codec::ParseWaveFormat(fmt, f));
        auto dec = snd::CreateDecoder(f);
        REQUIRE(dec);
        // ffmpeg drops a trailing partial block; compare the complete blocks.
        const std::size_t whole = static_cast<std::size_t>(snd::codec::BytesToFrames(f, data.size()));
        std::vector<std::int16_t> out(dec->FramesFor(data.size()) * f.channels);
        dec->Decode(data, out);
        std::vector<std::int16_t> ref(pcm.size() / 2);
        std::memcpy(ref.data(), pcm.data(), ref.size() * 2);
        const std::size_t n = std::min(ref.size(), whole * f.channels);
        CHECK(n > 0);
        CHECK(ref.size() >= whole * f.channels);
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < n; ++i) {
            mismatches += out[i] != ref[i] ? 1u : 0u;
        }
        CHECK(mismatches == 0);
        ++files;
    }
    MESSAGE("compared " << files << " game files from " << std::string(dir));
    CHECK(files > 0);
}
