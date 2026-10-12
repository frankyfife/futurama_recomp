// Streaming WAV writer and level meter of the audio capture (see capture_wav.h).
#include "audio/capture_wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <system_error>

namespace snd::capture {
namespace {

constexpr std::uint16_t kWaveFormatPcm = 1;
constexpr std::uint16_t kWaveFormatIeeeFloat = 3;
constexpr std::uint16_t kWaveFormatExtensible = 0xFFFE;
// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT: {0000000x-0000-0010-8000-00aa00389b71}
constexpr std::uint8_t kSubFormatTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                             0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

void Put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void Put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    Put16(out, v & 0xFFFF);
    Put16(out, v >> 16);
}
void PutTag(std::vector<std::uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

bool HasFact(const WavFormat& f) noexcept {
    return f.sampleFormat == SampleFormat::Float32;
}

}  // namespace

const char* SampleFormatName(SampleFormat format) noexcept {
    return format == SampleFormat::Float32 ? "float32" : "pcm16";
}

std::uint32_t DefaultChannelMask(std::uint32_t channels) noexcept {
    switch (channels) {
    case 1:
        return 0x4;  // SPEAKER_FRONT_CENTER
    case 2:
        return 0x3;  // FL | FR
    case 4:
        return 0x33;  // FL FR BL BR
    case 6:
        return 0x3F;  // FL FR FC LFE BL BR (5.1)
    case 8:
        return 0x63F;  // 7.1: 5.1 + SL SR
    default:
        return 0;
    }
}

std::int16_t FloatToPcm16(float x) noexcept {
    if (!(x == x)) {  // NaN
        return 0;
    }
    const double v = std::nearbyint(static_cast<double>(x) * 32768.0);
    return static_cast<std::int16_t>(std::clamp(v, -32768.0, 32767.0));
}

std::size_t WavHeaderSize(const WavFormat& f) noexcept {
    std::size_t size = 12;                     // RIFF size WAVE
    size += 8 + (f.Extensible() ? 40u : (f.sampleFormat == SampleFormat::Float32 ? 18u : 16u));
    if (HasFact(f)) {
        size += 12;
    }
    return size + 8;  // data size
}

std::vector<std::uint8_t> BuildWavHeader(const WavFormat& f, std::uint64_t frames) {
    std::vector<std::uint8_t> h;
    h.reserve(WavHeaderSize(f));
    const std::uint64_t dataBytes64 = std::min<std::uint64_t>(frames * f.BlockAlign(), kMaxDataBytes);
    const auto dataBytes = static_cast<std::uint32_t>(dataBytes64);
    const auto headerBytes = static_cast<std::uint32_t>(WavHeaderSize(f));
    PutTag(h, "RIFF");
    Put32(h, headerBytes - 8 + dataBytes);
    PutTag(h, "WAVE");
    PutTag(h, "fmt ");
    const bool ext = f.Extensible();
    const bool isFloat = f.sampleFormat == SampleFormat::Float32;
    Put32(h, ext ? 40u : (isFloat ? 18u : 16u));
    Put16(h, ext ? kWaveFormatExtensible : (isFloat ? kWaveFormatIeeeFloat : kWaveFormatPcm));
    Put16(h, f.channels);
    Put32(h, f.sampleRate);
    Put32(h, f.sampleRate * f.BlockAlign());
    Put16(h, f.BlockAlign());
    Put16(h, f.BytesPerSample() * 8);
    if (ext) {
        Put16(h, 22);                          // cbSize
        Put16(h, f.BytesPerSample() * 8);      // wValidBitsPerSample
        Put32(h, f.channelMask != 0 ? f.channelMask : DefaultChannelMask(f.channels));
        Put16(h, isFloat ? kWaveFormatIeeeFloat : kWaveFormatPcm);  // Data1 (low word)
        h.insert(h.end(), std::begin(kSubFormatTail), std::end(kSubFormatTail));
    } else if (isFloat) {
        Put16(h, 0);  // cbSize
    }
    if (HasFact(f)) {
        PutTag(h, "fact");
        Put32(h, 4);
        Put32(h, static_cast<std::uint32_t>(std::min<std::uint64_t>(dataBytes64 / f.BlockAlign(), 0xFFFFFFFFull)));
    }
    PutTag(h, "data");
    Put32(h, dataBytes);
    return h;
}

// ---- WavFileWriter ---------------------------------------------------------------------------------------------

WavFileWriter::~WavFileWriter() {
    Close();
}

bool WavFileWriter::Open(const std::filesystem::path& path, const WavFormat& format, std::string* error) {
    Close();
    failed_ = false;
    frames_ = 0;
    truncated_ = 0;
    if (format.channels == 0 || format.channels > kMaxCaptureChannels || format.sampleRate == 0) {
        if (error != nullptr) {
            *error = "unsupported format (" + std::to_string(format.channels) + " channels, " +
                     std::to_string(format.sampleRate) + " Hz)";
        }
        return false;
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = "cannot create the file (Win32 error " + std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    file_ = h;
    format_ = format;
    path_ = path;
    const std::vector<std::uint8_t> header = BuildWavHeader(format_, 0);
    if (!WriteRaw(header.data(), header.size())) {
        if (error != nullptr) {
            *error = "cannot write the header (Win32 error " + std::to_string(GetLastError()) + ")";
        }
        CloseHandle(h);
        file_ = nullptr;
        return false;
    }
    return true;
}

bool WavFileWriter::WriteRaw(const void* data, std::size_t bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (bytes > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes, 1u << 24));
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(file_), p, chunk, &written, nullptr) || written != chunk) {
            failed_ = true;
            return false;
        }
        p += chunk;
        bytes -= chunk;
    }
    return true;
}

bool WavFileWriter::Write(const float* interleaved, std::uint32_t frames) {
    if (file_ == nullptr || failed_) {
        return false;
    }
    if (frames == 0) {
        return true;
    }
    const std::uint64_t block = format_.BlockAlign();
    const std::uint64_t room = (kMaxDataBytes - frames_ * block) / block;
    std::uint32_t take = frames;
    if (take > room) {
        truncated_ += take - room;
        take = static_cast<std::uint32_t>(room);
    }
    if (take == 0) {
        return true;
    }
    const std::size_t samples = static_cast<std::size_t>(take) * format_.channels;
    if (format_.sampleFormat == SampleFormat::Float32) {
        if (!WriteRaw(interleaved, samples * sizeof(float))) {
            return false;
        }
    } else {
        scratch_.resize(samples * 2);
        for (std::size_t i = 0; i < samples; ++i) {
            const auto v = static_cast<std::uint16_t>(FloatToPcm16(interleaved[i]));
            scratch_[2 * i] = static_cast<std::uint8_t>(v & 0xFF);
            scratch_[2 * i + 1] = static_cast<std::uint8_t>(v >> 8);
        }
        if (!WriteRaw(scratch_.data(), scratch_.size())) {
            return false;
        }
    }
    frames_ += take;
    return true;
}

bool WavFileWriter::UpdateHeader() {
    if (file_ == nullptr || failed_) {
        return false;
    }
    HANDLE h = static_cast<HANDLE>(file_);
    LARGE_INTEGER zero{};
    LARGE_INTEGER pos{};
    if (!SetFilePointerEx(h, zero, &pos, FILE_CURRENT)) {
        failed_ = true;
        return false;
    }
    const std::vector<std::uint8_t> header = BuildWavHeader(format_, frames_);
    bool ok = SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) != 0 && WriteRaw(header.data(), header.size());
    ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0 && ok;
    if (!ok) {
        failed_ = true;
    }
    return ok;
}

bool WavFileWriter::Close() {
    if (file_ == nullptr) {
        return !failed_;
    }
    const bool ok = UpdateHeader();
    FlushFileBuffers(static_cast<HANDLE>(file_));
    CloseHandle(static_cast<HANDLE>(file_));
    file_ = nullptr;
    return ok;
}

// ---- LevelMeter ------------------------------------------------------------------------------------------------

double AmplitudeToDb(double amplitude) noexcept {
    if (!(amplitude > 1e-10)) {
        return -200.0;
    }
    return 20.0 * std::log10(amplitude);
}

void LevelMeter::Reset(std::uint32_t channels, std::uint32_t sampleRate) noexcept {
    *this = LevelMeter{};
    channels_ = std::min(channels, kMaxCaptureChannels);
    sampleRate_ = sampleRate != 0 ? sampleRate : 48000;
    blockFrames_ = std::max<std::uint32_t>(1, sampleRate_ / 10);
}

void LevelMeter::CloseBlock() noexcept {
    if (inBlock_ == 0) {
        return;
    }
    const double samples = static_cast<double>(inBlock_) * channels_;
    const double rms = std::sqrt(blockSumSq_ / samples);
    ++blocks_;
    if (AmplitudeToDb(rms) > kActiveBlockDb) {
        ++activeBlocks_;
        activeSumSq_ += blockSumSq_;
        activeSamples_ += static_cast<std::uint64_t>(inBlock_) * channels_;
    }
    inBlock_ = 0;
    blockSumSq_ = 0.0;
}

void LevelMeter::Add(const float* interleaved, std::uint32_t frames) noexcept {
    if (channels_ == 0) {
        return;
    }
    for (std::uint32_t f = 0; f < frames; ++f) {
        const float* frame = interleaved != nullptr ? interleaved + static_cast<std::size_t>(f) * channels_ : nullptr;
        bool silent = true;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            float x = frame != nullptr ? frame[c] : 0.0f;
            if (!std::isfinite(x)) {
                ++nonFinite_;
                x = 0.0f;
            }
            const double ax = std::fabs(static_cast<double>(x));
            const double sq = static_cast<double>(x) * x;
            sumSq_[c] += sq;
            blockSumSq_ += sq;
            if (ax > peak_) {
                peak_ = ax;
                peakFrame_ = frames_;
            }
            if (ax >= 1.0) {
                ++clipped_;
            }
            if (ax >= kSilenceLevel) {
                silent = false;
            }
        }
        if (silent) {
            ++silentRun_;
        } else {
            if (heardSound_ && silentRun_ > 0) {
                const double ms = 1000.0 * static_cast<double>(silentRun_) / sampleRate_;
                if (ms >= kGapMinMs) {
                    GapInfo g{static_cast<double>(frames_ - silentRun_) / sampleRate_, ms};
                    ++gaps_;
                    if (g.lengthMs > longest_.lengthMs) {
                        longest_ = g;
                    }
                    if (firstGaps_.size() < kMaxReportedGaps) {
                        firstGaps_.push_back(g);
                    }
                }
            }
            if (!heardSound_) {
                heardSound_ = true;
                firstSoundFrame_ = frames_;
            }
            silentRun_ = 0;
        }
        ++frames_;
        if (++inBlock_ >= blockFrames_) {
            CloseBlock();
        }
    }
}

LevelReport LevelMeter::Report() const {
    LevelMeter copy = *this;
    copy.CloseBlock();
    LevelReport r;
    r.frames = copy.frames_;
    r.channels = copy.channels_;
    r.sampleRate = copy.sampleRate_;
    r.seconds = static_cast<double>(copy.frames_) / copy.sampleRate_;
    r.channelRmsDb.fill(-200.0);
    double total = 0.0;
    for (std::uint32_t c = 0; c < copy.channels_; ++c) {
        total += copy.sumSq_[c];
        if (copy.frames_ != 0) {
            r.channelRmsDb[c] = AmplitudeToDb(std::sqrt(copy.sumSq_[c] / static_cast<double>(copy.frames_)));
        }
    }
    const double samples = static_cast<double>(copy.frames_) * copy.channels_;
    if (samples > 0) {
        r.rmsDb = AmplitudeToDb(std::sqrt(total / samples));
        r.clippedShare = static_cast<double>(copy.clipped_) / samples;
    }
    if (copy.activeSamples_ != 0) {
        r.activeRmsDb = AmplitudeToDb(std::sqrt(copy.activeSumSq_ / static_cast<double>(copy.activeSamples_)));
    }
    r.activeBlocks = copy.activeBlocks_;
    r.activeShare = copy.blocks_ != 0 ? static_cast<double>(copy.activeBlocks_) / static_cast<double>(copy.blocks_) : 0.0;
    r.peakDb = AmplitudeToDb(copy.peak_);
    r.peakSeconds = static_cast<double>(copy.peakFrame_) / copy.sampleRate_;
    r.clippedSamples = copy.clipped_;
    r.nonFiniteSamples = copy.nonFinite_;
    r.gaps = copy.gaps_;
    r.longestGap = copy.longest_;
    r.firstGaps = copy.firstGaps_;
    r.firstSoundSeconds = copy.heardSound_ ? static_cast<double>(copy.firstSoundFrame_) / copy.sampleRate_ : -1.0;
    return r;
}

}  // namespace snd::capture
