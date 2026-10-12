// Audio capture building blocks (M5 tooling agent, src/INTERFACES.md 4.29): a streaming WAV file writer and a
// level meter. Plain C++ without XAudio2, used by audio/capture_session.cpp on its writer thread and directly by
// the unit tests (tests/audio_capture_tests.cpp, synthetic signals only).
//
// WAV layout written by WavFileWriter (little endian, RIFF):
//   float32, 1-2 channels, mask 0 : fmt  WAVE_FORMAT_IEEE_FLOAT (3), cbSize 0, + fact chunk (frame count)
//   pcm16,   1-2 channels, mask 0 : fmt  WAVE_FORMAT_PCM (1), 16 bytes
//   more channels or a channel mask: fmt  WAVE_FORMAT_EXTENSIBLE (0xFFFE), cbSize 22, wValidBitsPerSample,
//                                    dwChannelMask, SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT / _PCM (+ fact for
//                                    float)
//   then "data". The header sizes are rewritten by UpdateHeader() (the capture session does that about once per
//   second), so a file stays readable up to the last update even if the process is terminated (watchdog, fatal
//   error). Sizes are 32-bit: the writer stops at kMaxDataBytes and reports Truncated().
//
// pcm16 conversion: round(x * 32768) clamped to -32768..32767 (+1.0 -> 32767); NaN -> 0.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace snd::capture {

enum class SampleFormat : std::uint8_t { Float32, Pcm16 };
const char* SampleFormatName(SampleFormat format) noexcept;  // "float32", "pcm16"

inline constexpr std::uint32_t kMaxCaptureChannels = 8;
inline constexpr std::uint64_t kMaxDataBytes = 0xFFFFFFFFull - 128ull;  // RIFF size field limit minus header

struct WavFormat {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;      // 1..kMaxCaptureChannels
    std::uint32_t channelMask = 0;   // SPEAKER_* bits; 0 = none (plain header for 1-2 channels)
    SampleFormat sampleFormat = SampleFormat::Float32;

    std::uint32_t BytesPerSample() const noexcept { return sampleFormat == SampleFormat::Float32 ? 4u : 2u; }
    std::uint32_t BlockAlign() const noexcept { return BytesPerSample() * channels; }
    bool Extensible() const noexcept { return channels > 2 || channelMask != 0; }
};

// Complete header (RIFF .. "data" size) for `frames` sample frames; HeaderSize(format) bytes.
std::vector<std::uint8_t> BuildWavHeader(const WavFormat& format, std::uint64_t frames);
std::size_t WavHeaderSize(const WavFormat& format) noexcept;
// Default SPEAKER_* mask for a channel count (1 FC, 2 FL|FR, 6 = 5.1 (FL FR FC LFE BL BR), 8 = 7.1); 0 otherwise.
std::uint32_t DefaultChannelMask(std::uint32_t channels) noexcept;
// float -> int16 as described above.
std::int16_t FloatToPcm16(float x) noexcept;

class WavFileWriter {
public:
    WavFileWriter() = default;
    ~WavFileWriter();
    WavFileWriter(const WavFileWriter&) = delete;
    WavFileWriter& operator=(const WavFileWriter&) = delete;

    // Creates/truncates `path` (parent directories are created) and writes the header for 0 frames. The file is
    // shared for reading (analysis tools may read it while the game runs).
    bool Open(const std::filesystem::path& path, const WavFormat& format, std::string* error = nullptr);
    bool IsOpen() const noexcept { return file_ != nullptr; }
    // Appends `frames` interleaved float frames (format().channels values each), converted to the file format.
    // Returns false on an I/O error (the writer then stays closed for writing); frames beyond kMaxDataBytes are
    // dropped and counted (Truncated()).
    bool Write(const float* interleaved, std::uint32_t frames);
    // Rewrites the RIFF/fact/data sizes for the frames written so far; the write position is kept.
    bool UpdateHeader();
    // UpdateHeader + close. Idempotent.
    bool Close();

    const WavFormat& format() const noexcept { return format_; }
    std::uint64_t FramesWritten() const noexcept { return frames_; }
    std::uint64_t FramesTruncated() const noexcept { return truncated_; }
    bool Truncated() const noexcept { return truncated_ != 0; }
    bool Failed() const noexcept { return failed_; }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    bool WriteRaw(const void* data, std::size_t bytes);

    void* file_ = nullptr;  // HANDLE
    WavFormat format_{};
    std::filesystem::path path_;
    std::uint64_t frames_ = 0;
    std::uint64_t truncated_ = 0;
    bool failed_ = false;
    std::vector<std::uint8_t> scratch_;
};

// ---- level statistics ------------------------------------------------------------------------------------------
// Accumulates RMS, peak, clipping, activity and gaps of a stream of interleaved float frames.
//   active block : 100 ms block whose RMS over all channels is above kActiveBlockDb (-70 dBFS)
//   silent frame : every channel below kSilenceLevel (1e-6 = -120 dBFS)
//   gap          : a run of silent frames of at least kGapMinMs between two non-silent frames (a dropout or a
//                  stream loop gap; leading/trailing silence is no gap)
//   clipped      : |x| >= 1.0 (the device would clip; in a float mix it is only a measurement)
inline constexpr double kActiveBlockDb = -70.0;
inline constexpr float kSilenceLevel = 1e-6f;
inline constexpr double kGapMinMs = 5.0;
inline constexpr std::size_t kMaxReportedGaps = 8;

struct GapInfo {
    double startSeconds = 0.0;
    double lengthMs = 0.0;
};

struct LevelReport {
    std::uint64_t frames = 0;
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    double seconds = 0.0;
    double rmsDb = -200.0;                                  // over all samples of all channels
    std::array<double, kMaxCaptureChannels> channelRmsDb{};  // per channel
    double activeRmsDb = -200.0;                            // over the active blocks only
    double activeShare = 0.0;                               // active blocks / all blocks
    std::uint64_t activeBlocks = 0;
    double peakDb = -200.0;
    double peakSeconds = 0.0;
    std::uint64_t clippedSamples = 0;
    double clippedShare = 0.0;  // of all samples
    std::uint64_t nonFiniteSamples = 0;
    std::uint32_t gaps = 0;
    GapInfo longestGap{};
    std::vector<GapInfo> firstGaps;  // at most kMaxReportedGaps
    double firstSoundSeconds = -1.0;  // first non-silent frame, -1 = none
};

class LevelMeter {
public:
    void Reset(std::uint32_t channels, std::uint32_t sampleRate) noexcept;
    void Add(const float* interleaved, std::uint32_t frames) noexcept;
    LevelReport Report() const;
    std::uint32_t channels() const noexcept { return channels_; }

private:
    void CloseBlock() noexcept;

    std::uint32_t channels_ = 0;
    std::uint32_t sampleRate_ = 48000;
    std::uint64_t frames_ = 0;
    std::array<double, kMaxCaptureChannels> sumSq_{};
    double peak_ = 0.0;
    std::uint64_t peakFrame_ = 0;
    std::uint64_t clipped_ = 0;
    std::uint64_t nonFinite_ = 0;
    // 100 ms blocks
    std::uint32_t blockFrames_ = 4800;
    std::uint32_t inBlock_ = 0;
    double blockSumSq_ = 0.0;
    std::uint64_t blocks_ = 0;
    std::uint64_t activeBlocks_ = 0;
    double activeSumSq_ = 0.0;
    std::uint64_t activeSamples_ = 0;
    // gaps
    bool heardSound_ = false;
    std::uint64_t firstSoundFrame_ = 0;
    std::uint64_t silentRun_ = 0;
    std::uint32_t gaps_ = 0;
    GapInfo longest_{};
    std::vector<GapInfo> firstGaps_;
};

// 20 * log10(x) with a floor of -200 dB.
double AmplitudeToDb(double amplitude) noexcept;

}  // namespace snd::capture
