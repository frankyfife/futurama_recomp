// Unit tests of the audio capture (M5 tooling agent, src/INTERFACES.md 4.29): WAV writer, level meter, lock-free
// ring, capture tap XAPO, capture session (files, stems, event list, statistics), the capture attached to the
// software engine and - if this machine has an active audio endpoint - an end-to-end XAudio2 run with the tap on a
// submix voice (inaudible: its send to the mastering voice is 0). Synthetic signals only, never game audio.
#include "doctest.h"

#include "audio/capture_engine.h"
#include "audio/capture_session.h"
#include "audio/capture_tap.h"
#include "audio/capture_wav.h"
#include "core/config.h"

#include <xapo.h>
#include <xaudio2.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace snd::capture;

namespace {

fs::path CaptureTempDir(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const fs::path dir = fs::path(buf).parent_path() / "fut_tests_tmp" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::vector<std::uint8_t> ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::uint32_t U32(const std::vector<std::uint8_t>& b, std::size_t o) {
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
std::uint16_t U16(const std::vector<std::uint8_t>& b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}

// Minimal RIFF reader of what WavFileWriter writes (and what tools/audio_compare.py reads).
struct ParsedWav {
    bool ok = false;
    std::uint16_t tag = 0;
    std::uint16_t subTag = 0;
    std::uint16_t channels = 0;
    std::uint32_t rate = 0;
    std::uint16_t bits = 0;
    std::uint32_t mask = 0;
    std::uint32_t factFrames = 0xFFFFFFFF;
    std::uint32_t riffSize = 0;
    std::size_t dataOffset = 0;
    std::uint32_t dataSize = 0;
};
ParsedWav ParseWav(const std::vector<std::uint8_t>& b) {
    ParsedWav w;
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        return w;
    }
    w.riffSize = U32(b, 4);
    std::size_t o = 12;
    while (o + 8 <= b.size()) {
        const std::uint32_t size = U32(b, o + 4);
        if (std::memcmp(b.data() + o, "fmt ", 4) == 0) {
            w.tag = U16(b, o + 8);
            w.channels = U16(b, o + 10);
            w.rate = U32(b, o + 12);
            w.bits = U16(b, o + 22);
            if (w.tag == 0xFFFE) {
                w.mask = U32(b, o + 28);
                w.subTag = U16(b, o + 32);
            }
        } else if (std::memcmp(b.data() + o, "fact", 4) == 0) {
            w.factFrames = U32(b, o + 8);
        } else if (std::memcmp(b.data() + o, "data", 4) == 0) {
            w.dataOffset = o + 8;
            w.dataSize = size;
            w.ok = true;
            return w;
        }
        o += 8 + size + (size & 1);
    }
    return w;
}

std::vector<float> Sine(std::uint32_t frames, std::uint32_t channels, double hz, double amp, std::uint32_t rate) {
    std::vector<float> v(static_cast<std::size_t>(frames) * channels);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double x = amp * std::sin(2.0 * std::numbers::pi * hz * i / rate);
        for (std::uint32_t c = 0; c < channels; ++c) {
            v[static_cast<std::size_t>(i) * channels + c] = static_cast<float>(c == 1 ? x * 0.5 : x);
        }
    }
    return v;
}

std::vector<float> FloatsOf(const std::vector<std::uint8_t>& b, const ParsedWav& w) {
    std::vector<float> v(w.dataSize / 4);
    std::memcpy(v.data(), b.data() + w.dataOffset, v.size() * 4);
    return v;
}

// Feeds `samples` through a locked tap like XAudio2 does (in place, in blocks of 480 frames).
void RunTap(IUnknown* unk, std::vector<float>& samples, std::uint32_t channels, std::uint32_t rate, bool silent = false) {
    IXAPO* xapo = nullptr;
    REQUIRE(unk->QueryInterface(__uuidof(IXAPO), reinterpret_cast<void**>(&xapo)) == S_OK);
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = static_cast<WORD>(channels);
    fmt.nSamplesPerSec = rate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = static_cast<WORD>(4 * channels);
    fmt.nAvgBytesPerSec = rate * fmt.nBlockAlign;
    XAPO_LOCKFORPROCESS_PARAMETERS lp{&fmt, 480};
    REQUIRE(xapo->LockForProcess(1, &lp, 1, &lp) == S_OK);
    const std::uint32_t frames = static_cast<std::uint32_t>(samples.size() / channels);
    for (std::uint32_t at = 0; at < frames; at += 480) {
        const std::uint32_t n = std::min<std::uint32_t>(480, frames - at);
        XAPO_PROCESS_BUFFER_PARAMETERS in{samples.data() + static_cast<std::size_t>(at) * channels,
                                          silent ? XAPO_BUFFER_SILENT : XAPO_BUFFER_VALID, n};
        XAPO_PROCESS_BUFFER_PARAMETERS out = in;
        out.ValidFrameCount = 0;
        xapo->Process(1, &in, 1, &out, TRUE);
        CHECK(out.ValidFrameCount == n);
        CHECK(out.BufferFlags == in.BufferFlags);
    }
    xapo->UnlockForProcess();
    xapo->Release();
}

}  // namespace

TEST_CASE("audio capture: WAV headers (float, pcm16, extensible)") {
    WavFormat f;
    f.channels = 2;
    f.sampleRate = 48000;
    std::vector<std::uint8_t> h = BuildWavHeader(f, 100);
    CHECK(h.size() == WavHeaderSize(f));
    ParsedWav w = ParseWav(h);
    REQUIRE(w.ok);
    CHECK(w.tag == 3);
    CHECK(w.channels == 2);
    CHECK(w.rate == 48000);
    CHECK(w.bits == 32);
    CHECK(w.factFrames == 100);
    CHECK(w.dataSize == 800);
    CHECK(w.riffSize == h.size() - 8 + 800);

    f.sampleFormat = SampleFormat::Pcm16;
    f.channels = 1;
    h = BuildWavHeader(f, 10);
    w = ParseWav(h);
    REQUIRE(w.ok);
    CHECK(w.tag == 1);
    CHECK(w.bits == 16);
    CHECK(w.factFrames == 0xFFFFFFFF);  // no fact chunk for PCM
    CHECK(w.dataSize == 20);
    CHECK(h.size() == 44);

    f.sampleFormat = SampleFormat::Float32;
    f.channels = 6;
    h = BuildWavHeader(f, 1);
    w = ParseWav(h);
    REQUIRE(w.ok);
    CHECK(w.tag == 0xFFFE);
    CHECK(w.subTag == 3);
    CHECK(w.mask == 0x3F);
    CHECK(w.dataSize == 24);
    CHECK(DefaultChannelMask(2) == 3);
    CHECK(DefaultChannelMask(8) == 0x63F);
}

TEST_CASE("audio capture: float -> pcm16 conversion") {
    CHECK(FloatToPcm16(0.0f) == 0);
    CHECK(FloatToPcm16(1.0f) == 32767);
    CHECK(FloatToPcm16(-1.0f) == -32768);
    CHECK(FloatToPcm16(2.5f) == 32767);
    CHECK(FloatToPcm16(-7.0f) == -32768);
    CHECK(FloatToPcm16(0.5f) == 16384);
    CHECK(FloatToPcm16(std::nanf("")) == 0);
}

TEST_CASE("audio capture: WavFileWriter writes readable files, also before Close") {
    const fs::path dir = CaptureTempDir(L"capture writer (a,b)");
    const std::vector<float> data = Sine(4800, 2, 1000.0, 0.5, 48000);
    WavFileWriter w;
    WavFormat f;
    std::string error;
    REQUIRE(w.Open(dir / "sub dir" / "float.wav", f, &error));
    REQUIRE(w.Write(data.data(), 2400));
    REQUIRE(w.UpdateHeader());
    {
        const std::vector<std::uint8_t> partial = ReadFile(dir / "sub dir" / "float.wav");
        const ParsedWav pw = ParseWav(partial);
        REQUIRE(pw.ok);
        CHECK(pw.dataSize == 2400 * 8);
        CHECK(partial.size() == pw.dataOffset + pw.dataSize);
    }
    REQUIRE(w.Write(data.data() + 4800, 2400));  // continues after the header update
    CHECK(w.FramesWritten() == 4800);
    REQUIRE(w.Close());
    const std::vector<std::uint8_t> b = ReadFile(dir / "sub dir" / "float.wav");
    const ParsedWav pw = ParseWav(b);
    REQUIRE(pw.ok);
    CHECK(pw.factFrames == 4800);
    CHECK(FloatsOf(b, pw) == data);

    WavFileWriter w16;
    f.sampleFormat = SampleFormat::Pcm16;
    REQUIRE(w16.Open(dir / "pcm.wav", f));
    REQUIRE(w16.Write(data.data(), 4800));
    REQUIRE(w16.Close());
    const std::vector<std::uint8_t> b16 = ReadFile(dir / "pcm.wav");
    const ParsedWav p16 = ParseWav(b16);
    REQUIRE(p16.ok);
    CHECK(p16.dataSize == 4800 * 4);
    for (std::size_t i : {0u, 1u, 7u, 333u, 9599u}) {
        CHECK(static_cast<std::int16_t>(U16(b16, p16.dataOffset + 2 * i)) == FloatToPcm16(data[i]));
    }

    WavFileWriter bad;
    WavFormat none;
    none.channels = 0;
    CHECK_FALSE(bad.Open(dir / "x.wav", none, &error));
    CHECK(error.find("unsupported") != std::string::npos);
}

TEST_CASE("audio capture: level meter (rms, peak, clipping, activity, gaps)") {
    LevelMeter m;
    m.Reset(2, 48000);
    // 0.25 s silence (no gap: nothing before), 1 s sine 0.5/0.25, 103.4 ms digital silence, 1 s sine, 0.2 s silence
    std::vector<float> lead(12000 * 2, 0.0f);
    m.Add(lead.data(), 12000);
    const std::vector<float> s = Sine(48000, 2, 440.0, 0.5, 48000);
    m.Add(s.data(), 48000);
    const auto gapFrames = static_cast<std::uint32_t>(std::lround(0.1034 * 48000));
    m.Add(nullptr, gapFrames);
    m.Add(s.data(), 48000);
    m.Add(lead.data(), 9600);
    const LevelReport r = m.Report();
    CHECK(r.frames == 12000u + 96000u + gapFrames + 9600u);
    CHECK(r.gaps == 1);
    CHECK(r.longestGap.lengthMs == doctest::Approx(103.4).epsilon(0.001));
    CHECK(r.longestGap.startSeconds == doctest::Approx(1.25).epsilon(0.0001));
    CHECK(r.firstSoundSeconds == doctest::Approx(12001.0 / 48000.0).epsilon(0.001));
    CHECK(r.peakDb == doctest::Approx(-6.02).epsilon(0.01));
    CHECK(r.clippedSamples == 0);
    // active blocks: the two sine seconds (rms sqrt((0.125 + 0.03125) / 2) = -11.07 dBFS) plus the partly silent
    // 100 ms blocks at their edges, which lower it a little
    CHECK(r.activeRmsDb < -11.0);
    CHECK(r.activeRmsDb > -11.8);
    CHECK(r.channelRmsDb[0] < 20 * std::log10(0.5 / std::sqrt(2.0)));  // the silence counts in the full rms
    CHECK(r.channelRmsDb[0] - r.channelRmsDb[1] == doctest::Approx(6.02).epsilon(0.01));
    CHECK(r.activeShare > 0.8);
    CHECK(r.activeShare < 0.95);

    LevelMeter c;
    c.Reset(1, 1000);
    const float hot[] = {0.2f, 1.0f, -1.5f, 0.9f, std::numeric_limits<float>::infinity()};
    c.Add(hot, 5);
    const LevelReport rc = c.Report();
    CHECK(rc.clippedSamples == 2);
    CHECK(rc.nonFiniteSamples == 1);
    CHECK(rc.peakDb == doctest::Approx(20 * std::log10(1.5)).epsilon(0.001));
    CHECK(rc.peakSeconds == doctest::Approx(0.002));
}

TEST_CASE("audio capture: lock-free ring") {
    FloatRing ring;
    REQUIRE(ring.Allocate(1000));
    CHECK(ring.Capacity() == 1024);
    std::vector<float> in(700);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = static_cast<float>(i);
    }
    CHECK(ring.Write(in.data(), 700));
    CHECK_FALSE(ring.Write(in.data(), 400));  // all or nothing
    std::vector<float> out(1024);
    CHECK(ring.Read(out.data(), 600) == 600);
    CHECK(out[599] == 599.0f);
    CHECK(ring.Write(in.data(), 700));  // wraps
    CHECK(ring.Write(nullptr, 100));    // zeros
    CHECK(ring.Available() == 900);
    CHECK(ring.Read(out.data(), 1024) == 900);
    CHECK(out[0] == 600.0f);
    CHECK(out[99] == 699.0f);
    CHECK(out[100] == 0.0f);
    CHECK(out[799] == 699.0f);
    CHECK(out[800] == 0.0f);
    CHECK(out[899] == 0.0f);
}

TEST_CASE("audio capture: tap XAPO registration and format negotiation") {
    Session session;
    IUnknown* unk = CreateCaptureTap(session, Stem::Mix);
    REQUIRE(unk != nullptr);
    IXAPO* xapo = nullptr;
    REQUIRE(unk->QueryInterface(__uuidof(IXAPO), reinterpret_cast<void**>(&xapo)) == S_OK);
    XAPO_REGISTRATION_PROPERTIES* props = nullptr;
    REQUIRE(xapo->GetRegistrationProperties(&props) == S_OK);
    CHECK((props->Flags & XAPO_FLAG_INPLACE_REQUIRED) != 0);
    CHECK((props->Flags & XAPO_FLAG_CHANNELS_MUST_MATCH) != 0);
    CHECK(props->MaxInputBufferCount == 1);
    XAPOFree(props);

    WAVEFORMATEX flt{WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0};
    WAVEFORMATEX pcm{WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
    WAVEFORMATEX* nearest = nullptr;
    CHECK(xapo->IsInputFormatSupported(&flt, &flt, &nearest) == S_OK);
    CHECK(xapo->IsInputFormatSupported(&flt, &pcm, &nearest) == XAPO_E_FORMAT_UNSUPPORTED);
    REQUIRE(nearest != nullptr);
    CHECK(nearest->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    CHECK(nearest->nChannels == 2);
    XAPOFree(nearest);
    WAVEFORMATEX flt6 = flt;
    flt6.nChannels = 6;
    CHECK(xapo->IsOutputFormatSupported(&flt, &flt6, nullptr) == XAPO_E_FORMAT_UNSUPPORTED);
    XAPO_LOCKFORPROCESS_PARAMETERS a{&flt, 480};
    XAPO_LOCKFORPROCESS_PARAMETERS b{&flt6, 480};
    CHECK(xapo->LockForProcess(1, &a, 1, &b) == XAPO_E_FORMAT_UNSUPPORTED);
    CHECK(xapo->CalcInputFrames(480) == 480);
    xapo->Release();
    CHECK(unk->Release() == 0);
}

TEST_CASE("audio capture: session with taps writes the mix, stems, events and statistics") {
    const fs::path dir = CaptureTempDir(L"capture session");
    Settings settings;
    settings.wavPath = dir / "run (1)" / "capture.wav";
    settings.stems = true;
    settings.eventLog = true;
    CHECK(StemPath(settings.wavPath, Stem::Music).filename() == L"capture.music.wav");
    CHECK(EventLogPath(settings.wavPath).filename() == L"capture.events.tsv");

    Session session;
    CHECK(session.CreateTap(Stem::Mix) == nullptr);  // not active yet
    REQUIRE(session.Start(settings, Source::Test));
    CHECK_FALSE(session.Start(settings, Source::Test));  // once per session

    std::vector<float> mix = Sine(48000, 2, 1000.0, 0.5, 48000);
    const std::vector<float> original = mix;
    IUnknown* tap = session.CreateTap(Stem::Mix);
    REQUIRE(tap != nullptr);
    session.RecordEvent({"play", 0x00123450, 2, "sounds/music/test.wav", "loop=1 mB=-1100", 77});
    RunTap(tap, mix, 2, 48000);
    CHECK(mix == original);  // passed on unchanged
    session.RecordEvent({"stop", 0x00123450, 2, "tab\there", "", 78});
    std::vector<float> silent(4800 * 2, 123.0f);  // garbage content: SILENT flag means zeros
    RunTap(tap, silent, 2, 48000, true);
    tap->Release();

    // a stem through the software-mix path, and a refused second format
    REQUIRE(session.DeclareStem(Stem::Music, 1, 48000));
    CHECK_FALSE(session.DeclareStem(Stem::Music, 2, 48000));
    const std::vector<float> music = Sine(9600, 1, 250.0, 0.25, 48000);
    session.Push(Stem::Music, music.data(), 9600, 1);
    session.Push(Stem::Music, music.data(), 100, 2);  // wrong channel count -> mismatched
    session.Push(Stem::Effects, nullptr, 0, 2);        // nothing
    CHECK(session.MixFrames() == 52800);

    session.Stop(false);
    session.Stop(false);  // idempotent
    CHECK_FALSE(session.Active());
    session.Push(Stem::Mix, original.data(), 10, 2);  // ignored after Stop
    CHECK(session.CreateTap(Stem::Mix) == nullptr);

    const StemStatistics st = session.Statistics(Stem::Mix);
    CHECK(st.pushedFrames == 52800);
    CHECK(st.writtenFrames == 52800);
    CHECK(st.droppedFrames == 0);
    CHECK(st.levels.peakDb == doctest::Approx(-6.02).epsilon(0.01));
    const StemStatistics sm = session.Statistics(Stem::Music);
    CHECK(sm.channels == 1);
    CHECK(sm.mismatchedFrames == 100);
    CHECK(sm.levels.rmsDb == doctest::Approx(20 * std::log10(0.25 / std::sqrt(2.0))).epsilon(0.01));
    CHECK_FALSE(session.Statistics(Stem::Speech).declared);

    const std::vector<std::uint8_t> b = ReadFile(settings.wavPath);
    const ParsedWav w = ParseWav(b);
    REQUIRE(w.ok);
    CHECK(w.channels == 2);
    CHECK(w.rate == 48000);
    const std::vector<float> got = FloatsOf(b, w);
    REQUIRE(got.size() == 52800u * 2);
    CHECK(std::equal(original.begin(), original.end(), got.begin()));
    CHECK(got.back() == 0.0f);
    CHECK(got[96000] == 0.0f);

    const std::vector<std::uint8_t> bm = ReadFile(StemPath(settings.wavPath, Stem::Music));
    const ParsedWav wm = ParseWav(bm);
    REQUIRE(wm.ok);
    CHECK(wm.channels == 1);
    CHECK(wm.dataSize == 9600 * 4);
    CHECK_FALSE(fs::exists(StemPath(settings.wavPath, Stem::Speech)));

    std::ifstream ev(EventLogPath(settings.wavPath));
    std::string header, l1, l2;
    std::getline(ev, header);
    std::getline(ev, l1);
    std::getline(ev, l2);
    CHECK(header == "sample\tseconds\twall_s\ttick\tkind\tobject\tcategory\tname\tdetail");
    CHECK(l1.rfind("0\t0.000000\t", 0) == 0);
    CHECK(l1.find("\t77\tplay\t0x00123450\t2\tsounds/music/test.wav\tloop=1 mB=-1100") != std::string::npos);
    CHECK(l2.rfind("48000\t1.000000\t", 0) == 0);
    CHECK(l2.find("\t78\tstop\t0x00123450\t2\ttab here\t") != std::string::npos);
}

TEST_CASE("audio capture: overflowing ring drops whole blocks and counts them") {
    const fs::path dir = CaptureTempDir(L"capture overflow");
    Settings settings;
    settings.wavPath = dir / "o.wav";
    settings.format = SampleFormat::Pcm16;
    Session session;
    REQUIRE(session.Start(settings, Source::Test));
    REQUIRE(session.DeclareStem(Stem::Mix, 2, 8000));  // ring >= 1 s = 16000 samples -> 16384
    const std::vector<float> big(9000 * 2, 0.1f);
    session.Push(Stem::Mix, big.data(), 9000, 2);  // 18000 samples > 16384 -> dropped
    session.Push(Stem::Mix, big.data(), 4000, 2);  // fits
    session.Stop(false);
    const StemStatistics st = session.Statistics(Stem::Mix);
    CHECK(st.droppedFrames == 9000);
    CHECK(st.writtenFrames == 4000);
    const ParsedWav w = ParseWav(ReadFile(settings.wavPath));
    REQUIRE(w.ok);
    CHECK(w.bits == 16);
    CHECK(w.dataSize == 4000 * 4);
}

TEST_CASE("audio capture: settings from the configuration") {
    core::Config cfg;
    Settings s = SettingsFromConfig(cfg);
    CHECK_FALSE(s.Enabled());
    CHECK(s.format == SampleFormat::Float32);
    cfg.values["audio.capture_wav"] = "C:\\captures\\run.wav";
    cfg.values["audio.capture_format"] = "pcm16";
    cfg.values["audio.capture_stems"] = "1";
    cfg.values["audio.event_log"] = "on";
    s = SettingsFromConfig(cfg);
    CHECK(s.Enabled());
    CHECK(s.wavPath == fs::path(L"C:\\captures\\run.wav"));
    CHECK(s.format == SampleFormat::Pcm16);
    CHECK(s.stems);
    CHECK(s.eventLog);
    CHECK(StemForCategory(0) == Stem::Effects);
    CHECK(StemForCategory(1) == Stem::Speech);
    CHECK(StemForCategory(2) == Stem::Music);
    CHECK(StemForCategory(3) == Stem::Movie);
    CHECK(std::string(StemName(Stem::Reverb)) == "reverb");
    CHECK(std::string(SourceName(Source::SoftwareMix)) == "software-mix");
}

TEST_CASE("audio capture: XAudio2 submix voice tap end to end (skipped without an audio endpoint)") {
    // The tap sits on a submix voice whose send to the mastering voice is 0: inaudible, but processed by XAudio2
    // like the engine's category submixes. The submix volume 0.5 shows where XAudio2 applies a voice's volume relative
    // to its effect chain (measured: before it - so stems and the mix capture include the voice volumes).
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IXAudio2* xa = nullptr;
    if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        MESSAGE("XAudio2Create failed - skipped");
        return;
    }
    IXAudio2MasteringVoice* master = nullptr;
    const HRESULT hm = xa->CreateMasteringVoice(&master, 2, 48000);
    if (FAILED(hm)) {
        MESSAGE("no active audio endpoint (CreateMasteringVoice failed, HRESULT " << static_cast<unsigned long>(hm)
                                                                                 << ") - skipped");
        xa->Release();
        if (SUCCEEDED(co)) {
            CoUninitialize();
        }
        return;
    }
    const fs::path dir = CaptureTempDir(L"capture xaudio2");
    Settings settings;
    settings.wavPath = dir / "xa.wav";
    Session session;
    REQUIRE(session.Start(settings, Source::XAudio2));
    IUnknown* tap = session.CreateTap(Stem::Mix);
    REQUIRE(tap != nullptr);
    XAUDIO2_EFFECT_DESCRIPTOR desc{tap, TRUE, 2};
    XAUDIO2_EFFECT_CHAIN chain{1, &desc};
    IXAudio2SubmixVoice* sub = nullptr;
    REQUIRE(xa->CreateSubmixVoice(&sub, 2, 48000, 0, 0, nullptr, &chain) == S_OK);
    tap->Release();
    const float silent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    REQUIRE(sub->SetOutputMatrix(master, 2, 2, silent) == S_OK);  // nothing reaches the device
    REQUIRE(sub->SetVolume(0.5f) == S_OK);

    WAVEFORMATEX fmt{WAVE_FORMAT_IEEE_FLOAT, 1, 48000, 192000, 4, 32, 0};
    XAUDIO2_SEND_DESCRIPTOR send{0, sub};
    XAUDIO2_VOICE_SENDS sends{1, &send};
    IXAudio2SourceVoice* src = nullptr;
    REQUIRE(xa->CreateSourceVoice(&src, &fmt, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, &sends) == S_OK);
    const std::vector<float> tone = Sine(24000, 1, 1000.0, 0.25, 48000);  // 0.5 s
    const float m[2] = {1.0f, 1.0f};
    REQUIRE(src->SetOutputMatrix(sub, 1, 2, m) == S_OK);
    XAUDIO2_BUFFER buf{};
    buf.Flags = XAUDIO2_END_OF_STREAM;
    buf.AudioBytes = static_cast<UINT32>(tone.size() * 4);
    buf.pAudioData = reinterpret_cast<const BYTE*>(tone.data());
    REQUIRE(src->SubmitSourceBuffer(&buf) == S_OK);
    REQUIRE(src->Start() == S_OK);
    for (int i = 0; i < 200; ++i) {
        XAUDIO2_VOICE_STATE vs{};
        src->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (vs.BuffersQueued == 0) {
            break;
        }
        Sleep(10);
    }
    Sleep(50);
    src->DestroyVoice();
    sub->DestroyVoice();
    master->DestroyVoice();
    xa->Release();
    session.Stop(false);
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    const StemStatistics st = session.Statistics(Stem::Mix);
    CHECK(st.channels == 2);
    CHECK(st.sampleRate == 48000);
    CHECK(st.pushedFrames >= 24000);
    MESSAGE("XAudio2 submix tap: peak " << st.levels.peakDb << " dBFS for a -12.04 dBFS sine and submix volume 0.5");
    CHECK(st.levels.peakDb == doctest::Approx(20 * std::log10(0.125)).epsilon(0.02));  // volume before the effects
    CHECK(st.levels.activeRmsDb == doctest::Approx(20 * std::log10(0.125 / std::sqrt(2.0))).epsilon(0.05));
    // frequency from zero crossings of the left channel
    const std::vector<std::uint8_t> b = ReadFile(settings.wavPath);
    const ParsedWav w = ParseWav(b);
    REQUIRE(w.ok);
    const std::vector<float> got = FloatsOf(b, w);
    int crossings = 0;
    std::size_t first = 0, last = 0;
    for (std::size_t i = 2; i < got.size(); i += 2) {
        if ((got[i - 2] < 0.0f) != (got[i] < 0.0f) && std::fabs(got[i]) + std::fabs(got[i - 2]) > 1e-3f) {
            if (crossings == 0) {
                first = i / 2;
            }
            last = i / 2;
            ++crossings;
        }
    }
    REQUIRE(crossings > 100);
    const double hz = (crossings - 1) / 2.0 / (static_cast<double>(last - first) / 48000.0);
    CHECK(hz == doctest::Approx(1000.0).epsilon(0.002));
}

TEST_CASE("audio capture: attached to the software engine (MixTap stems, event sink)") {
    CHECK(StemForTapPoint(snd::TapPoint::Master) == Stem::Mix);
    CHECK(StemForTapPoint(snd::TapPoint::Effects) == Stem::Effects);
    CHECK(StemForTapPoint(snd::TapPoint::Speech) == Stem::Speech);
    CHECK(StemForTapPoint(snd::TapPoint::Music) == Stem::Music);
    CHECK(StemForTapPoint(snd::TapPoint::Movie) == Stem::Movie);
    CHECK(StemForTapPoint(snd::TapPoint::Reverb) == Stem::Reverb);
    CHECK(StemForTapPoint(snd::TapPoint::Count) == Stem::Count);
    CHECK(std::string(SoundEventKindName(snd::SoundEventKind::OrphanedObject)) == "orphan");

    snd::EngineConfig config;
    config.enabled = false;  // software mixer, rendered on demand
    config.softwareRealtime = false;
    std::unique_ptr<snd::AudioEngine> engine = snd::CreateAudioEngine(config);
    REQUIRE(engine != nullptr);
    CHECK_FALSE(engine->HasDevice());
    const std::uint32_t channels = engine->OutputChannels();
    REQUIRE(channels >= 1);

    const fs::path dir = CaptureTempDir(L"capture engine");
    Settings settings;
    settings.wavPath = dir / "engine.wav";
    settings.stems = true;
    settings.eventLog = true;
    Session session;
    SessionMixTap tap(session);
    SessionEventSink sink(session);
    REQUIRE(AttachCapture(session, settings, *engine, tap, sink));

    snd::VoiceDesc desc;
    desc.format.channels = 1;
    desc.format.sampleRate = engine->OutputSampleRate();
    desc.category = snd::Category::Music;
    std::unique_ptr<snd::SourceVoice> voice = engine->CreateVoice(desc);
    REQUIRE(voice != nullptr);
    const std::uint32_t rate = engine->OutputSampleRate();
    std::vector<std::int16_t> pcm(rate / 2);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::int16_t>(std::lround(8192.0 * std::sin(2.0 * std::numbers::pi * 1000.0 * i / rate)));
    }
    std::vector<float> matrix(channels, 1.0f);
    voice->SetOutputMatrix(matrix);
    snd::SubmitDesc sub;
    sub.data = pcm.data();
    sub.frames = static_cast<std::uint32_t>(pcm.size());
    REQUIRE(voice->Submit(sub));
    voice->Start();
    std::vector<float> out(static_cast<std::size_t>(rate) * channels);
    CHECK(engine->RenderSoftware(rate, out.data()) == rate);

    snd::SoundEvent e;
    e.kind = snd::SoundEventKind::Play;
    e.object = 0x00ABC000;
    e.voiceId = 42;
    e.category = snd::Category::Speech;
    e.isStream = true;
    e.looping = true;
    e.name = "Sounds/Localized/german/x.wav";
    e.millibel = -250;
    e.outputFrame = 4800;
    sink.OnSoundEvent(e);

    voice.reset();
    engine->SetTap(nullptr);
    snd::SetSoundEventSink(nullptr);
    session.Stop(false);

    const StemStatistics mix = session.Statistics(Stem::Mix);
    CHECK(mix.channels == channels);
    CHECK(mix.writtenFrames == rate);
    CHECK(mix.levels.firstSoundSeconds >= 0.0);
    const StemStatistics music = session.Statistics(Stem::Music);
    REQUIRE(music.declared);
    CHECK(music.writtenFrames == rate);
    CHECK(music.levels.peakDb > -20.0);  // the 0.25 sine (-12 dBFS) through the music submix
    CHECK(music.levels.peakDb < -3.0);
    CHECK(music.levels.activeShare == doctest::Approx(0.5).epsilon(0.1));  // 0.5 s of 1 s
    CHECK(fs::exists(StemPath(settings.wavPath, Stem::Music)));
    std::ifstream ev(EventLogPath(settings.wavPath));
    std::string header, line;
    std::getline(ev, header);
    std::getline(ev, line);
    CHECK(line.find("\tplay\t0x00ABC000\t1\tSounds/Localized/german/x.wav\tstream loop voice=42 mB=-250 hz=0 "
                    "pos=0.000,0.000,0.000 frame=4800") != std::string::npos);
}
