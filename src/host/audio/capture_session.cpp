// Audio capture session (see capture_session.h).
#include "audio/capture_session.h"

#include "audio/capture_tap.h"
#include "audio/xdk_entries.h"
#include "core/config.h"
#include "core/config_schema.h"
#include "core/host_main.h"
#include "core/log.h"
#include "xapi/game_timing.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

namespace snd::capture {

struct Session::Stream {
    std::atomic<bool> declared{false};
    std::mutex declareMutex;
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channelMask = 0;
    FloatRing ring;
    std::atomic<std::uint64_t> pushed{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> mismatched{0};
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    std::atomic<bool> refusalLogged{false};
    // writer thread (drainMutex_)
    WavFileWriter file;
    bool fileTried = false;
    bool fileFailed = false;
    bool meterReady = false;
    LevelMeter meter;
    std::vector<float> scratch;
};

namespace {

constexpr double kRingSeconds = 1.0;
constexpr int kWriterPeriodMs = 20;

std::string Sanitize(std::string_view text) {
    std::string s(text);
    for (char& c : s) {
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return s;
}

std::string PathUtf8(const std::filesystem::path& p) {
    try {
        const std::u8string u = p.u8string();
        return std::string(u.begin(), u.end());
    } catch (...) {
        return "?";
    }
}

std::int64_t QpcNow() noexcept {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

}  // namespace

const char* StemName(Stem stem) noexcept {
    switch (stem) {
    case Stem::Mix:
        return "mix";
    case Stem::Music:
        return "music";
    case Stem::Speech:
        return "speech";
    case Stem::Effects:
        return "effects";
    case Stem::Movie:
        return "movie";
    case Stem::Reverb:
        return "reverb";
    case Stem::Count:
        break;
    }
    return "?";
}

Stem StemForCategory(int category) noexcept {
    switch (category) {
    case 0:
        return Stem::Effects;
    case 1:
        return Stem::Speech;
    case 2:
        return Stem::Music;
    case 3:
        return Stem::Movie;
    default:
        return Stem::Effects;
    }
}

const char* SourceName(Source source) noexcept {
    switch (source) {
    case Source::XAudio2:
        return "xaudio2";
    case Source::SoftwareMix:
        return "software-mix";
    case Source::Test:
        return "test";
    }
    return "?";
}

Settings SettingsFromConfig(const core::Config& config) {
    Settings s;
    if (!config.GetString("audio", "capture_wav").empty()) {
        s.wavPath = config.GetPath("audio", "capture_wav");
    }
    const std::string format =
        config.GetString("audio", "capture_format", std::string(core::ConfigKeyDefault("audio", "capture_format")));
    s.format = (format == "pcm16" || format == "PCM16") ? SampleFormat::Pcm16 : SampleFormat::Float32;
    s.stems = config.GetBool("audio", "capture_stems", false);
    s.eventLog = config.GetBool("audio", "event_log", false);
    return s;
}

std::filesystem::path StemPath(const std::filesystem::path& wavPath, Stem stem) {
    if (stem == Stem::Mix) {
        return wavPath;
    }
    std::filesystem::path p = wavPath;
    std::filesystem::path stemName = p.stem();
    stemName += L".";
    stemName += StemName(stem);
    stemName += L".wav";
    return p.replace_filename(stemName);
}

std::filesystem::path EventLogPath(const std::filesystem::path& wavPath) {
    std::filesystem::path p = wavPath;
    std::filesystem::path name = p.stem();
    name += L".events.tsv";
    return p.replace_filename(name);
}

// ---- Session ---------------------------------------------------------------------------------------------------

Session::Session() {
    for (auto& s : streams_) {
        s = std::make_unique<Stream>();
    }
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    qpf_ = f.QuadPart > 0 ? f.QuadPart : 1;
}

Session::~Session() {
    Stop(false);
}

Session& Session::Instance() noexcept {
    static Session session;
    return session;
}

bool Session::Start(const Settings& settings, Source source) noexcept {
    if (!settings.Enabled()) {
        return false;
    }
    State expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, State::Running)) {
        FUT_LOGW(Snd, "audio capture: Start ignored (the session already %s)",
                 expected == State::Running ? "runs" : "ran in this process");
        return false;
    }
    try {
        settings_ = settings;
        source_.store(source);
        startQpc_ = QpcNow();
        headerUpdateQpc_ = static_cast<std::uint64_t>(startQpc_);
        if (settings_.eventLog) {
            const std::filesystem::path ev = EventLogPath(settings_.wavPath);
            std::error_code ec;
            if (ev.has_parent_path()) {
                std::filesystem::create_directories(ev.parent_path(), ec);
            }
            FILE* f = _wfopen(ev.c_str(), L"wb");
            if (f == nullptr) {
                FUT_LOGW(Snd, "audio capture: cannot create the event list %s", PathUtf8(ev).c_str());
            } else {
                std::fputs("sample\tseconds\twall_s\ttick\tkind\tobject\tcategory\tname\tdetail\n", f);
                eventFile_ = f;
            }
        }
        stopRequested_ = false;
        writer_ = std::thread([this] { WriterLoop(); });
        FUT_LOGI(Snd, "audio capture: started %s (%s%s%s, source %s)", PathUtf8(settings_.wavPath).c_str(),
                 SampleFormatName(settings_.format), settings_.stems ? ", stems" : "",
                 settings_.eventLog ? ", event list" : "", SourceName(source));
        if (this == &Instance()) {
            core::AddExitReporter("audio capture", []() noexcept { Session::Instance().Stop(); });
        }
        return true;
    } catch (...) {
        state_.store(State::Stopped);
        FUT_LOGE(Snd, "audio capture: could not start the writer thread");
        return false;
    }
}

bool Session::StartFromConfig(Source source) noexcept {
    try {
        return Start(SettingsFromConfig(core::GetConfig()), source);
    } catch (...) {
        return false;
    }
}

void Session::NoteSource(Source source) noexcept {
    const Source old = source_.exchange(source);
    if (old != source && Active()) {
        FUT_LOGI(Snd, "audio capture: mix source %s -> %s at %.3f s of the capture", SourceName(old),
                 SourceName(source), static_cast<double>(MixFrames()) / std::max(1u, streams_[0]->sampleRate));
    }
}

bool Session::DeclareStem(Stem stem, std::uint32_t channels, std::uint32_t sampleRate,
                          std::uint32_t channelMask) noexcept {
    if (stem >= Stem::Count || channels == 0 || channels > kMaxCaptureChannels || sampleRate == 0) {
        return false;
    }
    Stream& s = *streams_[static_cast<std::size_t>(stem)];
    // Lock-free fast path (the engine's tap calls this for every block): the format fields are written before the
    // release store of `declared`.
    if (s.declared.load(std::memory_order_acquire) && s.channels == channels && s.sampleRate == sampleRate) {
        return true;
    }
    std::lock_guard<std::mutex> lock(s.declareMutex);
    if (s.declared.load(std::memory_order_acquire)) {
        const bool same = s.channels == channels && s.sampleRate == sampleRate;
        if (!same && !s.refusalLogged.exchange(true)) {
            FUT_LOGW(Snd, "audio capture: stem %s keeps %u ch %u Hz (refused %u ch %u Hz)", StemName(stem), s.channels,
                     s.sampleRate, channels, sampleRate);
        }
        return same;
    }
    const auto samples = static_cast<std::size_t>(kRingSeconds * sampleRate) * channels;
    if (!s.ring.Allocate(samples)) {
        FUT_LOGE(Snd, "audio capture: no memory for the ring of stem %s", StemName(stem));
        return false;
    }
    s.channels = channels;
    s.sampleRate = sampleRate;
    s.channelMask = channelMask;
    s.declared.store(true, std::memory_order_release);
    if (Active()) {
        FUT_LOGD(Snd, "audio capture: stem %s %u ch %u Hz (mask 0x%X)", StemName(stem), channels, sampleRate,
                 channelMask);
    }
    return true;
}

void Session::Push(Stem stem, const float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept {
    if (!Active() || stem >= Stem::Count || frames == 0) {
        return;
    }
    Stream& s = *streams_[static_cast<std::size_t>(stem)];
    if (!s.declared.load(std::memory_order_acquire)) {
        // Software mixer without DeclareStem: declare with the mix rate (allocates once).
        const Stream& mix = *streams_[0];
        const std::uint32_t rate = mix.declared.load(std::memory_order_acquire) ? mix.sampleRate : 48000;
        if (!DeclareStem(stem, channels, rate)) {
            s.mismatched.fetch_add(frames, std::memory_order_relaxed);
            return;
        }
    }
    if (channels != s.channels) {
        s.mismatched.fetch_add(frames, std::memory_order_relaxed);
        return;
    }
    if (s.busy.test_and_set(std::memory_order_acquire)) {
        s.dropped.fetch_add(frames, std::memory_order_relaxed);  // a second producer at the same moment
        return;
    }
    if (s.ring.Write(interleaved, static_cast<std::size_t>(frames) * channels)) {
        s.pushed.fetch_add(frames, std::memory_order_release);
    } else {
        s.dropped.fetch_add(frames, std::memory_order_relaxed);
    }
    s.busy.clear(std::memory_order_release);
}

IUnknown* Session::CreateTap(Stem stem) noexcept {
    if (!Active() || stem >= Stem::Count) {
        return nullptr;
    }
    return CreateCaptureTap(*this, stem);
}

std::uint64_t Session::MixFrames() const noexcept {
    return streams_[0]->pushed.load(std::memory_order_acquire);
}

void Session::RecordEvent(const Event& event) noexcept {
    if (!Active() || eventFile_ == nullptr) {
        return;
    }
    try {
        const std::uint64_t sample = MixFrames();
        const Stream& mix = *streams_[0];
        const double rate = mix.declared.load(std::memory_order_acquire) ? mix.sampleRate : 48000.0;
        const double wall = static_cast<double>(QpcNow() - startQpc_) / static_cast<double>(qpf_);
        const std::uint64_t tick = event.tick != ~0ull ? event.tick : xapi::GetGameTickStats().steps;
        char head[160];
        std::snprintf(head, sizeof(head), "%llu\t%.6f\t%.6f\t%llu\t", static_cast<unsigned long long>(sample),
                      static_cast<double>(sample) / rate, wall, static_cast<unsigned long long>(tick));
        char obj[32];
        std::snprintf(obj, sizeof(obj), "\t0x%08X\t%d\t", event.object, event.category);
        std::string line = head;
        line += Sanitize(event.kind);
        line += obj;
        line += Sanitize(event.name);
        line += '\t';
        line += Sanitize(event.detail);
        line += '\n';
        std::lock_guard<std::mutex> lock(eventMutex_);
        pendingEvents_.push_back(std::move(line));
    } catch (...) {
    }
}

void Session::DrainStream(Stem stem, Stream& s) noexcept {
    if (!s.declared.load(std::memory_order_acquire)) {
        return;
    }
    if (!s.meterReady) {
        s.meter.Reset(s.channels, s.sampleRate);
        s.meterReady = true;
    }
    const bool wantFile = stem == Stem::Mix || settings_.stems;
    if (wantFile && !s.fileTried) {
        s.fileTried = true;
        WavFormat f;
        f.sampleRate = s.sampleRate;
        f.channels = s.channels;
        f.channelMask = s.channels > 2 ? (s.channelMask != 0 ? s.channelMask : DefaultChannelMask(s.channels)) : 0;
        f.sampleFormat = settings_.format;
        std::string error;
        const std::filesystem::path path = StemPath(settings_.wavPath, stem);
        if (!s.file.Open(path, f, &error)) {
            s.fileFailed = true;
            FUT_LOGW(Snd, "audio capture: cannot write %s: %s", PathUtf8(path).c_str(), error.c_str());
        }
    }
    try {
        if (s.scratch.empty()) {
            s.scratch.resize(static_cast<std::size_t>(s.channels) * 4096);
        }
        for (;;) {
            const std::size_t n = s.ring.Read(s.scratch.data(), s.scratch.size());
            if (n == 0) {
                break;
            }
            const auto frames = static_cast<std::uint32_t>(n / s.channels);
            s.meter.Add(s.scratch.data(), frames);
            if (s.file.IsOpen() && !s.file.Write(s.scratch.data(), frames) && !s.fileFailed) {
                s.fileFailed = true;
                FUT_LOGW(Snd, "audio capture: write error on %s", PathUtf8(s.file.path()).c_str());
            }
        }
    } catch (...) {
    }
}

void Session::DrainAll(bool final) noexcept {
    std::lock_guard<std::mutex> lock(drainMutex_);
    for (std::size_t i = 0; i < kStemCount; ++i) {
        DrainStream(static_cast<Stem>(i), *streams_[i]);
    }
    std::vector<std::string> events;
    {
        std::lock_guard<std::mutex> elock(eventMutex_);
        events.swap(pendingEvents_);
    }
    if (eventFile_ != nullptr) {
        for (const std::string& e : events) {
            std::fwrite(e.data(), 1, e.size(), static_cast<FILE*>(eventFile_));
        }
        eventsWritten_ += events.size();
    }
    const auto now = static_cast<std::uint64_t>(QpcNow());
    if (final || now - headerUpdateQpc_ >= static_cast<std::uint64_t>(qpf_)) {
        headerUpdateQpc_ = now;
        for (auto& s : streams_) {
            if (s->file.IsOpen()) {
                s->file.UpdateHeader();
            }
        }
        if (eventFile_ != nullptr) {
            std::fflush(static_cast<FILE*>(eventFile_));
        }
    }
}

void Session::WriterLoop() noexcept {
    core::LogSetThreadName("audio capture");
    std::unique_lock<std::mutex> lock(wakeMutex_);
    while (!stopRequested_) {
        wake_.wait_for(lock, std::chrono::milliseconds(kWriterPeriodMs));
        lock.unlock();
        DrainAll(false);
        lock.lock();
    }
}

void Session::Flush() noexcept {
    DrainAll(false);
}

void Session::Stop(bool log) noexcept {
    State expected = State::Running;
    if (!state_.compare_exchange_strong(expected, State::Stopped)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        stopRequested_ = true;
    }
    wake_.notify_all();
    if (writer_.joinable()) {
        writer_.join();
    }
    DrainAll(true);
    {
        std::lock_guard<std::mutex> lock(drainMutex_);
        for (auto& s : streams_) {
            s->file.Close();
        }
        if (eventFile_ != nullptr) {
            std::fclose(static_cast<FILE*>(eventFile_));
            eventFile_ = nullptr;
        }
    }
    if (log) {
        LogSummary();
    }
}

StemStatistics Session::Statistics(Stem stem) const {
    StemStatistics st;
    if (stem >= Stem::Count) {
        return st;
    }
    const Stream& s = *streams_[static_cast<std::size_t>(stem)];
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(drainMutex_));
    st.declared = s.declared.load();
    st.channels = s.channels;
    st.sampleRate = s.sampleRate;
    st.pushedFrames = s.pushed.load();
    st.droppedFrames = s.dropped.load();
    st.mismatchedFrames = s.mismatched.load();
    st.writtenFrames = s.file.FramesWritten();
    st.fileFailed = s.fileFailed;
    if (s.meterReady) {
        st.levels = s.meter.Report();
    }
    return st;
}

void Session::LogSummary() noexcept {
    try {
        const StemStatistics mix = Statistics(Stem::Mix);
        std::string files;
        for (std::size_t i = 0; i < kStemCount; ++i) {
            const Stream& s = *streams_[i];
            if (s.fileTried && !s.fileFailed) {
                files += files.empty() ? "" : ", ";
                files += StemName(static_cast<Stem>(i));
            }
        }
        if (!mix.declared) {
            FUT_LOGW(Snd, "audio capture: %s - no mix data (the audio engine created no capture tap for the mix), "
                          "source %s",
                     PathUtf8(settings_.wavPath).c_str(), SourceName(source_.load()));
        } else {
            FUT_LOGI(Snd,
                     "audio capture: %s - %u Hz, %u ch, %s, source %s, %.3f s (%llu frames), %llu dropped, %llu "
                     "mismatched, %llu truncated, files: %s, %llu events",
                     PathUtf8(settings_.wavPath).c_str(), mix.sampleRate, mix.channels,
                     SampleFormatName(settings_.format), SourceName(source_.load()), mix.levels.seconds,
                     static_cast<unsigned long long>(mix.levels.frames),
                     static_cast<unsigned long long>(mix.droppedFrames),
                     static_cast<unsigned long long>(mix.mismatchedFrames),
                     static_cast<unsigned long long>(streams_[0]->file.FramesTruncated()),
                     files.empty() ? "none" : files.c_str(), static_cast<unsigned long long>(eventsWritten_));
        }
        for (std::size_t i = 0; i < kStemCount; ++i) {
            const Stem stem = static_cast<Stem>(i);
            const StemStatistics st = Statistics(stem);
            if (!st.declared || st.levels.frames == 0) {
                FUT_LOGI(Snd, "audio levels %s: no data", StemName(stem));
                continue;
            }
            const LevelReport& r = st.levels;
            std::string ch;
            for (std::uint32_t c = 0; c < r.channels; ++c) {
                char b[24];
                std::snprintf(b, sizeof(b), "%s%.2f", c == 0 ? "" : " / ", r.channelRmsDb[c]);
                ch += b;
            }
            std::string gapList;
            for (const GapInfo& g : r.firstGaps) {
                char b[48];
                std::snprintf(b, sizeof(b), "%s%.3f s %.1f ms", gapList.empty() ? "" : ", ", g.startSeconds,
                              g.lengthMs);
                gapList += b;
            }
            FUT_LOGI(Snd,
                     "audio levels %s: %u ch, %.3f s, rms %.2f dBFS (ch %s), active rms %.2f dBFS over %.1f %% of "
                     "100 ms blocks, peak %.2f dBFS at %.3f s, clipped %llu (%.6f %%), gaps >= %.0f ms: %u (longest "
                     "%.1f ms at %.3f s; %s), first sound at %.3f s, non-finite %llu, dropped %llu",
                     StemName(stem), r.channels, r.seconds, r.rmsDb, ch.c_str(), r.activeRmsDb, 100.0 * r.activeShare,
                     r.peakDb, r.peakSeconds, static_cast<unsigned long long>(r.clippedSamples),
                     100.0 * r.clippedShare, kGapMinMs, r.gaps, r.longestGap.lengthMs, r.longestGap.startSeconds,
                     gapList.empty() ? "none" : gapList.c_str(), r.firstSoundSeconds,
                     static_cast<unsigned long long>(r.nonFiniteSamples),
                     static_cast<unsigned long long>(st.droppedFrames));
        }
        // The engine feeds the session through its own MixTap; the capture-tap XAPO (CreateTap) is an alternative
        // for other XAudio2 graphs and tests - its counters only mean something when one was used.
        const TapCounters t = GetTapCounters();
        if (t.processCalls != 0 || t.lockedTaps != 0) {
            FUT_LOGI(Snd, "audio capture taps: %llu process calls, %llu frames, %llu silent blocks",
                     static_cast<unsigned long long>(t.processCalls), static_cast<unsigned long long>(t.frames),
                     static_cast<unsigned long long>(t.silentBlocks));
        }
        LogDsoundCallCounts();
    } catch (...) {
    }
}

void LogDsoundCallCounts() noexcept {
    try {
        std::string text;
        for (const xdk::EntryInfo& e : xdk::Entries()) {
            if (e.library != xdk::Library::Dsound) {
                continue;
            }
            const std::uint32_t n = xdk::CallCount(e.entry);
            if (n == 0) {
                continue;
            }
            const char* name = std::strstr(e.hookName, "::");
            name = name != nullptr ? name + 2 : e.hookName;
            char b[96];
            std::snprintf(b, sizeof(b), "%s%s %u", text.empty() ? "" : ", ", name, n);
            text += b;
        }
        FUT_LOGI(Snd, "audio dsound calls: %s", text.empty() ? "none" : text.c_str());
    } catch (...) {
    }
}

}  // namespace snd::capture
