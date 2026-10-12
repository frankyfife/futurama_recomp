// Audio capture session: [audio] capture_wav / capture_format / capture_stems / event_log (M5 tooling agent,
// src/INTERFACES.md 4.29; analysis/loader/m5_audio.md 6.8 and 9.2).
//
// Verification without ears: the final mix (and optionally every category submix) is copied unchanged into WAV
// files that tools/audio_compare.py and tools/m5_check.py analyse against the original sounds. At every orderly
// process end (exit reporter "audio capture") the session finalizes the files and logs per-stem statistics:
//
//   audio capture: <file> - 48000 Hz, 2 ch, float32, source xaudio2, 123.456 s (5925888 frames), 0 dropped, ...
//   audio levels mix: rms -24.31 dBFS (ch -24.10 / -24.52), active rms -22.05 dBFS over 95.1 % of 100 ms blocks,
//                     peak -1.20 dBFS at 12.345 s, clipped 0 (0.000000 %), gaps >= 5 ms: 1 (longest 103.4 ms at
//                     113.620 s; 113.620 s 103.4 ms), first sound at 0.512 s, non-finite 0
//   audio levels music: ...           (one line per stem that received data; "no data" otherwise)
//   audio dsound calls: DirectSoundCreateBuffer 123, IDirectSoundBuffer_Play 130, ...   (xdk::CallCount != 0)
//
// Who feeds it: audio/capture_engine.cpp attaches the session to the M5 audio engine (audio/audio_api.h) as a
// snd::EngineObserver - the engine's MixTap delivers every block of the mastering voice (after the limiter) and of
// the category/reverb submixes, with an XAudio2 device and in the software mix alike, and Push copies it into the
// stem's ring (AttachCapture declares every stem up front from AudioEngine::TapFormat, so the mixer thread never
// allocates). CreateTap returns an in-place XAPO for an XAudio2 effect chain that does the same and declares its
// stem's format in LockForProcess - not used by the engine (it has its own taps), kept for other XAudio2 graphs and
// tests; its counters ("audio capture taps: ...") are only logged when one was used. XAudio2 applies a voice's volume BEFORE its effect chain (measured,
// tests/audio_capture_tests.cpp), so a tap includes the volume of the voice it sits on.
//   Push is real-time safe (no locks, no allocation; blocks that do not fit are dropped and counted).
//
// Files: the mix goes to `capture_wav`; with capture_stems = 1 the stems go next to it as <stem>.<name>.wav
// (capture.music.wav, capture.speech.wav, capture.effects.wav, capture.movie.wav, capture.reverb.wav); with
// event_log = 1 RecordEvent lines go to <stem>.events.tsv (columns: sample, seconds, wall_s, tick, kind, object,
// category, name, detail; `sample` = mix frames captured when the event was recorded, i.e. the event happens
// at most one XAudio2 quantum (~10 ms) plus the device latency later in the mix). A file is opened by the
// writer thread when its stem is declared; headers are rewritten about once per second.
//
// Threads: Start/Stop/RecordEvent on any host thread (the guest thread calls RecordEvent from the DSOUND HLE);
// Push/taps on the XAudio2 engine thread or the software mixer; one writer thread ("audio capture") does all file
// I/O and statistics. Never touches guest memory (design E10).
#pragma once

#include "audio/capture_wav.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct IUnknown;

namespace core {
struct Config;
}

namespace snd::capture {

// Category submixes of m5_audio.md 6.1 (+ the mix itself). Order = file/log order.
enum class Stem : std::uint8_t { Mix, Music, Speech, Effects, Movie, Reverb, Count };
inline constexpr std::size_t kStemCount = static_cast<std::size_t>(Stem::Count);
const char* StemName(Stem stem) noexcept;  // "mix", "music", "speech", "effects", "movie", "reverb"
// Game volume category (SoundVoice+0x70: 0 effects, 1 speech, 2 music; 3 = host-side movie) -> stem.
Stem StemForCategory(int category) noexcept;

// Where the mix comes from (log line "source ...", m5_check criterion A1).
enum class Source : std::uint8_t { XAudio2, SoftwareMix, Test };
const char* SourceName(Source source) noexcept;  // "xaudio2", "software-mix", "test"

struct Settings {
    std::filesystem::path wavPath;  // empty = capture off
    SampleFormat format = SampleFormat::Float32;
    bool stems = false;
    bool eventLog = false;
    bool Enabled() const noexcept { return !wavPath.empty(); }
};
// Reads [audio] capture_wav (GetPath: relative to the INI's folder), capture_format, capture_stems, event_log.
Settings SettingsFromConfig(const core::Config& config);
// File of a stem: the mix = wavPath, others "<wavPath without .wav>.<stem>.wav"; events ".events.tsv".
std::filesystem::path StemPath(const std::filesystem::path& wavPath, Stem stem);
std::filesystem::path EventLogPath(const std::filesystem::path& wavPath);

// One line of the event list (strings are copied; tabs/newlines in them are replaced by blanks).
struct Event {
    std::string_view kind;    // short word: create, play, stop, end, release, volume, stream, ...
    std::uint32_t object = 0;  // guest address of the DSOUND buffer/stream (0 = none)
    int category = -1;         // 0 effects, 1 speech, 2 music, 3 movie, -1 unknown
    std::string_view name;     // sound name (probe on SoundVoice::CreateDsObject) or ""
    std::string_view detail;   // free "key=value key=value" text
    std::uint64_t tick = ~0ull;  // logic tick; ~0 = filled from xapi::GetGameTickStats().steps
};

struct StemStatistics {
    bool declared = false;
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t pushedFrames = 0;
    std::uint64_t droppedFrames = 0;      // ring full or a second producer at the same time
    std::uint64_t mismatchedFrames = 0;   // Push with another channel count than declared
    std::uint64_t writtenFrames = 0;      // into the file (0 for stems without capture_stems)
    bool fileFailed = false;
    LevelReport levels;
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // The process-wide session (exit reporter "audio capture" stops it).
    static Session& Instance() noexcept;

    // Starts the writer thread. False if `settings` has no path or the session already ran. `source` only names
    // where the mix comes from (log). Files are opened when the stems are declared.
    bool Start(const Settings& settings, Source source) noexcept;
    // Start(SettingsFromConfig(core::GetConfig()), source) - for the engine.
    bool StartFromConfig(Source source) noexcept;
    bool Active() const noexcept { return state_.load(std::memory_order_acquire) == State::Running; }
    const Settings& settings() const noexcept { return settings_; }
    // The mix source changed (device lost -> software mix, or back): one log line, no format change allowed.
    void NoteSource(Source source) noexcept;

    // Fixes a stem's format (allocates its ring, ~1 s). A second call with the same format is a no-op; another
    // format is refused (false, logged once). Called by the tap in LockForProcess or by the software mixer.
    bool DeclareStem(Stem stem, std::uint32_t channels, std::uint32_t sampleRate,
                     std::uint32_t channelMask = 0) noexcept;
    // Real-time safe: copies `frames` interleaved frames (nullptr = silence) into the stem's ring.
    void Push(Stem stem, const float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept;
    // In-place XAPO that pushes into `stem` (refcount 1, caller releases); nullptr if the session is not active.
    IUnknown* CreateTap(Stem stem) noexcept;

    void RecordEvent(const Event& event) noexcept;

    // Drains the rings, finalizes the files, logs the statistics (unless `log` is false). Idempotent.
    void Stop(bool log = true) noexcept;
    // Mix frames pushed so far (event time base).
    std::uint64_t MixFrames() const noexcept;
    StemStatistics Statistics(Stem stem) const;
    // Waits until the writer thread has drained everything pushed so far (tests).
    void Flush() noexcept;

private:
    enum class State : std::uint8_t { Idle, Running, Stopped };
    struct Stream;

    void WriterLoop() noexcept;
    void DrainAll(bool final) noexcept;
    void DrainStream(Stem stem, Stream& s) noexcept;
    void LogSummary() noexcept;

    std::atomic<State> state_{State::Idle};
    Settings settings_;
    std::atomic<Source> source_{Source::Test};
    std::unique_ptr<Stream> streams_[kStemCount];
    std::thread writer_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    bool stopRequested_ = false;
    std::mutex drainMutex_;  // writer thread vs. Flush/Stop
    std::mutex eventMutex_;
    std::vector<std::string> pendingEvents_;
    std::uint64_t eventsWritten_ = 0;
    void* eventFile_ = nullptr;  // FILE*
    std::int64_t startQpc_ = 0;
    std::int64_t qpf_ = 1;
    std::uint64_t headerUpdateQpc_ = 0;
};

// Logs the non-zero DSOUND entry call counts ("audio dsound calls: ..."), part of the session summary.
void LogDsoundCallCounts() noexcept;

}  // namespace snd::capture
