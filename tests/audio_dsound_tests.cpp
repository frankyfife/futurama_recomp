// Unit tests of the DSOUND HLE and the audio engine (M5 DSOUND core agent; analysis/loader/m5_audio.md 8 traps,
// 9.1 test plan). Synthetic signals only - no game audio. The DSOUND entries are called directly with host memory
// standing in for guest memory (same address space); the engine is the software mixer rendered on demand, so every
// packet completion and level is deterministic.
#include "doctest.h"

#include "audio/audio_api.h"
#include "audio/audio_engine.h"
#include "audio/dsound_hle.h"
#include "audio/dsound_mix.h"
#include "audio/xdk_entries.h"
#include "core/guest.h"
#include "kernel/ke_wait.h"

#include <windows.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using StreamProcessFn = std::int32_t(__stdcall*)(void*, const snd::XMediaPacket*, const snd::XMediaPacket*);
using StreamGetStatusFn = std::int32_t(__stdcall*)(void*, std::uint32_t*);
using StreamRefFn = std::uint32_t(__stdcall*)(void*);

template <class Fn>
Fn VtableSlot(const void* object, std::uint32_t slot) {
    const std::uint32_t vtable = guest::Read<std::uint32_t>(guest::Addr(object));
    return reinterpret_cast<Fn>(static_cast<std::uintptr_t>(guest::Read<std::uint32_t>(vtable + 4 * slot)));
}

double Db(float gain) {
    return 20.0 * std::log10(static_cast<double>(gain));
}

// A software engine (stereo, 48 kHz, rendered only by Render) installed as the process engine, a fresh DSOUND state
// and a device handle.
struct Fixture {
    std::unique_ptr<snd::AudioEngine> engine;
    void* ds = nullptr;
    std::vector<float> out;

    explicit Fixture(std::uint32_t speakers = 2) {
        snd::EngineConfig config;
        config.enabled = false;
        config.softwareRealtime = false;
        config.speakers = speakers;
        engine = snd::CreateSoftwareEngine(config);
        REQUIRE(engine != nullptr);
        snd::SetEngineForTesting(engine.get());
        snd::ResetDsoundForTesting();
        REQUIRE(snd::DirectSoundCreateHle(nullptr, &ds, nullptr) == snd::kS_OK);
    }
    ~Fixture() {
        snd::ResetDsoundForTesting();
        snd::SetEngineForTesting(nullptr);
    }
    // Renders `frames` output frames; returns them interleaved.
    const std::vector<float>& Render(std::uint32_t frames) {
        out.assign(static_cast<std::size_t>(frames) * engine->OutputChannels(), 0.0f);
        engine->RenderSoftware(frames, out.data());
        return out;
    }
};

std::uint32_t BufferStatus(void* buffer) {
    std::uint32_t st = 0xDEADBEEF;
    REQUIRE(snd::IDirectSoundBuffer_GetStatusHle(buffer, &st) == snd::kS_OK);
    return st;
}

void* CreateBuffer(const snd::XWaveFormatEx& wfx, std::uint32_t flags = 0x100) {
    snd::XDsBufferDesc desc{0x18, flags, 0, guest::Addr(&wfx), 0, 0};
    void* buffer = nullptr;
    REQUIRE(snd::DirectSoundCreateBufferHle(&desc, &buffer) == snd::kS_OK);
    REQUIRE(buffer != nullptr);
    return buffer;
}

constexpr snd::XWaveFormatEx kPcm16Mono48k{snd::kWaveFormatPcm, 1, 48000, 96000, 2, 16, 0x12};
constexpr snd::XWaveFormatEx kPcm16Stereo48k{snd::kWaveFormatPcm, 2, 48000, 192000, 4, 16, 0x12};

// Mono ramp: sample i = (i % 20000) - 10000 (unique positions within a 20000-frame window).
std::vector<std::int16_t> Ramp(std::uint32_t frames) {
    std::vector<std::int16_t> v(frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        v[i] = static_cast<std::int16_t>(static_cast<int>(i % 20000) - 10000);
    }
    return v;
}

// Sets the 2D levels the game sets every tick (m5_audio.md 3.4) and commits them like SoundManager::Update.
void SetGameLevels2D(void* ds, void* buffer, std::int32_t mb, std::int32_t bin0, std::int32_t bin1) {
    snd::XDsMixBinVolumePair pairs[2] = {{0, bin0}, {1, bin1}};
    snd::XDsMixBins mixbins{2, guest::Addr(pairs)};
    REQUIRE(snd::IDirectSoundBuffer_SetVolumeHle(buffer, mb) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundBuffer_SetMixBinsHle(buffer, &mixbins) == snd::kS_OK);
    REQUIRE(snd::IDirectSound_CommitDeferredSettingsHle(ds) == snd::kS_OK);
}

// The end of the game's SoundManager::Update: CommitDeferredSettings (0xCBE95) - applies the tick's values and starts
// the voices played in it.
void Commit(void* ds) {
    REQUIRE(snd::IDirectSound_CommitDeferredSettingsHle(ds) == snd::kS_OK);
}

}  // namespace

// ---- level model (m5_gain_model.out) -----------------------------------------------------------------------------

TEST_CASE("dsound mix: hardware mixbin register in 1/64 dB, clamped to 63.98 dB") {
    CHECK(snd::MixbinGain(0) == doctest::Approx(1.0f));
    CHECK(snd::MixbinGain(500) == doctest::Approx(1.0f));  // positive totals clamp to 0 dB
    CHECK(Db(snd::MixbinGain(-600)) == doctest::Approx(-6.0).epsilon(1e-6));
    CHECK(Db(snd::MixbinGain(-2398)) == doctest::Approx(-1534.0 / 64.0).epsilon(1e-6));  // (2398 << 6) / 100
    CHECK(Db(snd::MixbinGain(-20600)) == doctest::Approx(-4095.0 / 64.0).epsilon(1e-6));
    CHECK(snd::MillibelToGain(-10000) == 0.0f);
    CHECK(Db(snd::MillibelToGain(-250)) == doctest::Approx(-2.5).epsilon(1e-6));
}

TEST_CASE("dsound mix: 2D levels of the game (m5_gain_model.out table) incl. pan and 6 dB headroom") {
    struct Row {
        std::int32_t mb, bin0, bin1;
        double left, right;
    };
    // v -> mB; pan 0: bins = mB; pan -1: bin0 = 0, bin1 = mB.
    const Row rows[] = {
        {0, 0, 0, -6.00, -6.00},           {-50, -50, -50, -7.00, -7.00},     {-250, -250, -250, -11.00, -11.00},
        {-250, 0, -250, -8.50, -11.00},    {-500, -500, -500, -16.00, -16.00}, {-899, -899, -899, -23.97, -23.97},
        {-899, 0, -899, -14.98, -23.97},   {-10000, -10000, -10000, -63.98, -63.98},
    };
    for (const Row& r : rows) {
        snd::ds::MixBinList bins;
        bins.count = 2;
        bins.entries[0] = {0, r.bin0};
        bins.entries[1] = {1, r.bin1};
        snd::ds::Matrix2D m;
        snd::ds::Compute2D(r.mb, snd::kHeadroom2DMillibel, bins, 1, 2, 0x3, m);
        INFO("mB " << r.mb << " bins " << r.bin0 << "/" << r.bin1);
        CHECK(Db(m.levels[0]) == doctest::Approx(r.left).epsilon(0.0005));
        CHECK(Db(m.levels[1]) == doctest::Approx(r.right).epsilon(0.0005));
        CHECK(m.reverbSend == 0.0f);  // 2D voices stay dry (trap 9)
    }
    // Stereo source: left channel -> bin 0 -> FL only, right -> bin 1 -> FR only.
    snd::ds::Matrix2D st;
    snd::ds::Compute2D(-250, snd::kHeadroom2DMillibel, snd::ds::DefaultMixBins(2), 2, 2, 0x3, st);
    CHECK(Db(st.levels[0]) == doctest::Approx(-8.5).epsilon(0.0005));
    CHECK(st.levels[1] == 0.0f);
    CHECK(st.levels[2] == 0.0f);
    CHECK(Db(st.levels[3]) == doctest::Approx(-8.5).epsilon(0.0005));
    // 5.1 output: front bins land on FL/FR; a centre bin on FC; LFE only where present.
    snd::ds::MixBinList c;
    c.count = 2;
    c.entries[0] = {snd::kMixBinFrontCenter, 0};
    c.entries[1] = {snd::kMixBinLowFrequency, 0};
    snd::ds::Matrix2D m51;
    snd::ds::Compute2D(0, 0, c, 1, 6, 0x3F, m51);
    CHECK(m51.levels[2] == doctest::Approx(1.0f));
    CHECK(m51.levels[3] == doctest::Approx(1.0f));
    snd::ds::Matrix2D m20;
    snd::ds::Compute2D(0, 0, c, 1, 2, 0x3, m20);
    CHECK(m20.levels[0] == doctest::Approx(0.70710678f));  // centre -> L+R at -3 dB, LFE dropped
    CHECK(m20.levels[1] == doctest::Approx(0.70710678f));
    CHECK(snd::ds::SpeakerIndex(0x3F, snd::ds::kSpeakerBackRight) == 5);
    CHECK(snd::ds::SpeakerIndex(0x3, snd::ds::kSpeakerBackRight) == -1);
}

TEST_CASE("dsound engine helpers: [audio] speakers parser and default masks") {
    std::uint32_t ch = 99;
    CHECK(snd::ParseSpeakers("auto", ch));
    CHECK(ch == 0);
    CHECK(snd::ParseSpeakers("Stereo", ch));
    CHECK(ch == 2);
    CHECK(snd::ParseSpeakers("5.1", ch));
    CHECK(ch == 6);
    CHECK_FALSE(snd::ParseSpeakers("7.1", ch));
    CHECK(snd::DefaultChannelMask(2) == 0x3);
    CHECK(snd::DefaultChannelMask(6) == 0x3F);
    CHECK(std::string(snd::CategoryName(snd::Category::Music)) == "music");
}

// ---- buffers -------------------------------------------------------------------------------------------------------

TEST_CASE("dsound buffer: Play/Stop/resume at the remembered cursor, GetStatus 0 at once (traps 3, 4)") {
    Fixture fx;
    void* b = CreateBuffer(kPcm16Mono48k);
    std::vector<std::int16_t> pcm = Ramp(9600);
    CHECK(BufferStatus(b) == 0);  // never started
    REQUIRE(snd::IDirectSoundBuffer_SetBufferDataHle(b, pcm.data(), 9600 * 2) == snd::kS_OK);
    std::memset(pcm.data(), 0, pcm.size() * 2);  // DSOUND copied at SetBufferData: the game may reuse its memory
    SetGameLevels2D(fx.ds, b, 0, 0, 0);
    CHECK(BufferStatus(b) == 0);
    REQUIRE(snd::IDirectSoundBuffer_PlayHle(b, 0, 0, 0) == snd::kS_OK);
    CHECK(BufferStatus(b) == snd::kBufferStatusPlaying);  // at once, although the voice starts at the commit
    Commit(fx.ds);
    const float g = snd::MixbinGain(-600);  // 0 mB, bins 0 mB, 6 dB headroom
    const std::vector<float>& a = fx.Render(1000);
    CHECK(a[2 * 100] == doctest::Approx((100.0f - 10000.0f) / 32768.0f * g).epsilon(1e-4));
    CHECK(a[2 * 100 + 1] == doctest::Approx(a[2 * 100]));
    // Stop: status 0 at once, cursor remembered (1000 frames played at ratio 1).
    REQUIRE(snd::IDirectSoundBuffer_StopHle(b) == snd::kS_OK);
    CHECK(BufferStatus(b) == 0);
    CHECK(snd::InspectObject(b).cursor == 1000);
    const std::vector<float>& silent = fx.Render(500);
    CHECK(silent[0] == 0.0f);
    CHECK(silent[999] == 0.0f);
    // Play resumes at frame 1000 (CMcpxBuffer_Stop 0x24E04E keeps the position).
    REQUIRE(snd::IDirectSoundBuffer_PlayHle(b, 0, 0, 0) == snd::kS_OK);
    CHECK(snd::InspectObject(b).cursor == 1000);
    Commit(fx.ds);
    const std::vector<float>& r = fx.Render(100);
    CHECK(r[0] == doctest::Approx((1000.0f - 10000.0f) / 32768.0f * g).epsilon(1e-4));
    // End of the one-shot: GetStatus 0 without any further call, cursor back at 0.
    fx.Render(9600);
    CHECK(BufferStatus(b) == 0);
    snd::DirectSoundDoWorkHle();
    CHECK(snd::InspectObject(b).cursor == 0);
    // SetBufferData resets the cursor; (0, 0) leaves an empty buffer that never plays (dtor 0xCC06F + spin 0xCC0BF).
    REQUIRE(snd::IDirectSoundBuffer_PlayHle(b, 0, 0, snd::kPlayLooping) == snd::kS_OK);
    Commit(fx.ds);
    fx.Render(300);
    REQUIRE(snd::IDirectSoundBuffer_StopHle(b) == snd::kS_OK);
    CHECK(snd::InspectObject(b).cursor == 300);
    REQUIRE(snd::IDirectSoundBuffer_SetBufferDataHle(b, nullptr, 0) == snd::kS_OK);
    CHECK(BufferStatus(b) == 0);
    REQUIRE(snd::IDirectSoundBuffer_PlayHle(b, 0, 0, 0) == snd::kS_OK);
    CHECK(BufferStatus(b) == 0);
    CHECK(snd::IDirectSoundBuffer_ReleaseHle(b) == 0);
    CHECK_FALSE(snd::IsDsoundBuffer(b));
}

TEST_CASE("dsound buffer: looping reports 5 and wraps over the whole buffer; SetBufferData while playing stops") {
    Fixture fx;
    void* b = CreateBuffer(kPcm16Mono48k);
    std::vector<std::int16_t> pcm = Ramp(1000);
    REQUIRE(snd::IDirectSoundBuffer_SetBufferDataHle(b, pcm.data(), 2000) == snd::kS_OK);
    SetGameLevels2D(fx.ds, b, 0, 0, 0);
    REQUIRE(snd::IDirectSoundBuffer_PlayHle(b, 0, 0, snd::kPlayLooping) == snd::kS_OK);
    CHECK(BufferStatus(b) == (snd::kBufferStatusPlaying | snd::kBufferStatusLooping));
    Commit(fx.ds);
    const std::vector<float>& a = fx.Render(2500);
    const float g = snd::MixbinGain(-600);
    CHECK(a[2 * 1000] == doctest::Approx(-10000.0f / 32768.0f * g).epsilon(1e-4));  // wrapped to frame 0
    CHECK(a[2 * 2100] == doctest::Approx((100.0f - 10000.0f) / 32768.0f * g).epsilon(1e-4));
    snd::DirectSoundDoWorkHle();
    CHECK(BufferStatus(b) == (snd::kBufferStatusPlaying | snd::kBufferStatusLooping));
    CHECK(snd::InspectObject(b).cursor == 500);
    REQUIRE(snd::IDirectSoundBuffer_SetBufferDataHle(b, pcm.data(), 2000) == snd::kS_OK);
    CHECK(BufferStatus(b) == 0);
    CHECK(snd::InspectObject(b).cursor == 0);
    CHECK(snd::IDirectSoundBuffer_ReleaseHle(b) == 0);
}

TEST_CASE("dsound buffer: SetVolume/SetMixBins levels reach the output; SetFrequency in Xbox pitch units") {
    Fixture fx;
    void* b = CreateBuffer(kPcm16Mono48k);
    std::vector<std::int16_t> dc(48000, 16384);
    REQUIRE(snd::IDirectSoundBuffer_SetBufferDataHle(b, dc.data(), 96000) == snd::kS_OK);
    SetGameLevels2D(fx.ds, b, -250, 0, -250);  // slider 0.75, full left
    REQUIRE(snd::IDirectSoundBuffer_PlayHle(b, 0, 0, 0) == snd::kS_OK);
    Commit(fx.ds);
    const std::vector<float>& a = fx.Render(480);
    CHECK(Db(a[2 * 10] / 0.5f) == doctest::Approx(-8.5).epsilon(0.001));
    CHECK(Db(a[2 * 10 + 1] / 0.5f) == doctest::Approx(-11.0).epsilon(0.001));
    const snd::ObjectView v = snd::InspectObject(b);
    CHECK(v.outputChannels == 2);
    CHECK(Db(v.matrix[0]) == doctest::Approx(-8.5).epsilon(0.001));
    // The tick's new values only apply at the commit (CommitDeferredSettings / DoWork), like a batch.
    REQUIRE(snd::IDirectSoundBuffer_SetVolumeHle(b, -10000) == snd::kS_OK);
    CHECK(Db(snd::InspectObject(b).matrix[0]) == doctest::Approx(-8.5).epsilon(0.001));
    snd::DirectSoundDoWorkHle();
    CHECK(snd::InspectObject(b).matrix[0] < 0.0011f);  // -10000 mB -> -63.98 dB, not 0 (register clamp)
    CHECK(snd::InspectObject(b).matrix[0] > 0.0f);
    // 24000 Hz on a 48 kHz buffer = pitch -4096 = ratio 0.5: 1000 output frames advance the cursor by 500.
    REQUIRE(snd::IDirectSoundBuffer_StopHle(b) == snd::kS_OK);
    const std::uint32_t c0 = snd::InspectObject(b).cursor;
    REQUIRE(snd::IDirectSoundBuffer_SetFrequencyHle(b, 24000) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundBuffer_PlayHle(b, 0, 0, 0) == snd::kS_OK);
    snd::DirectSoundDoWorkHle();  // DoWork commits as well (XMV ticks have no CommitDeferredSettings)
    fx.Render(1000);
    CHECK(snd::InspectObject(b).frequencyRatio == doctest::Approx(0.5f));
    CHECK(snd::InspectObject(b).cursor == c0 + 500);
    // SetFrequency(0) = the buffer's own rate (0x247C42).
    REQUIRE(snd::IDirectSoundBuffer_SetFrequencyHle(b, 0) == snd::kS_OK);
    snd::DirectSoundDoWorkHle();
    CHECK(snd::InspectObject(b).frequencyRatio == doctest::Approx(1.0f));
    CHECK(snd::IDirectSoundBuffer_ReleaseHle(b) == 0);
}

TEST_CASE("dsound buffer/stream: a voice starts at the commit with the levels set after Play / Pause(0)") {
    // SoundVoice::vf11 calls Play (0xCC72E) / Pause(0) (0xCC711) BEFORE SetVolume/SetMixBins/SetFrequency of the same
    // update and commits at 0xCBE95. The first sample must already have the new level (review finding M5-1: the
    // first 20 ms of a menu sound played 15 dB too loud with the effects slider at 0.25).
    Fixture fx;
    void* b = CreateBuffer(kPcm16Mono48k);
    std::vector<std::int16_t> dc(4800, 16384);
    REQUIRE(snd::IDirectSoundBuffer_SetBufferDataHle(b, dc.data(), 9600) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundBuffer_PlayHle(b, 0, 0, 0) == snd::kS_OK);  // defaults: 0 mB, bins 0, original rate
    CHECK(BufferStatus(b) == snd::kBufferStatusPlaying);
    CHECK(snd::InspectObject(b).startPending);
    snd::XDsMixBinVolumePair pairs[2] = {{0, -750}, {1, -750}};
    snd::XDsMixBins mixbins{2, guest::Addr(pairs)};
    REQUIRE(snd::IDirectSoundBuffer_SetVolumeHle(b, -750) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundBuffer_SetMixBinsHle(b, &mixbins) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundBuffer_SetFrequencyHle(b, 24000) == snd::kS_OK);
    const std::vector<float>& before = fx.Render(10);  // not started before the commit
    CHECK(before[0] == 0.0f);
    CHECK(snd::InspectObject(b).cursor == 0);
    Commit(fx.ds);
    CHECK_FALSE(snd::InspectObject(b).startPending);
    const std::vector<float>& a = fx.Render(480);
    const double expected = Db(0.5f * snd::MixbinGain(-750 - 600 - 750));  // -21.0 dB below the DC of 0.5
    CHECK(Db(a[0]) == doctest::Approx(expected).epsilon(0.001));
    CHECK(Db(a[1]) == doctest::Approx(expected).epsilon(0.001));
    CHECK(Db(a[2 * 479]) == doctest::Approx(expected).epsilon(0.001));
    CHECK(snd::InspectObject(b).cursor == 240);  // pitch 0.5 from the first frame
    // Play and Stop before the commit: nothing plays, the cursor stays.
    REQUIRE(snd::IDirectSoundBuffer_StopHle(b) == snd::kS_OK);
    const std::uint32_t c0 = snd::InspectObject(b).cursor;
    REQUIRE(snd::IDirectSoundBuffer_PlayHle(b, 0, 0, 0) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundBuffer_StopHle(b) == snd::kS_OK);
    Commit(fx.ds);
    fx.Render(100);
    CHECK(snd::InspectObject(b).cursor == c0);
    CHECK(BufferStatus(b) == 0);

    // A stream resumed with Pause(0) and its volume set afterwards: the first sample at the new level (DoWork).
    snd::XWaveFormatEx wfx{snd::kWaveFormatPcm, 1, 48000, 96000, 2, 16, 0x12};
    snd::XDsStreamDesc sd{0, 4, guest::Addr(&wfx), 0, 0, 0};
    void* stream = nullptr;
    REQUIRE(snd::DirectSoundCreateStreamHle(&sd, &stream) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundStream_PauseHle(stream, snd::kStreamPause) == snd::kS_OK);
    snd::DirectSoundDoWorkHle();
    const auto process = VtableSlot<StreamProcessFn>(stream, xdk::stream::Process::slot);
    std::uint32_t status = 0;
    snd::XMediaPacket pkt{guest::Addr(dc.data()), 4800 * 2, 0, guest::Addr(&status), 0, 0};
    REQUIRE(process(stream, &pkt, nullptr) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundStream_PauseHle(stream, snd::kStreamResume) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundStream_SetVolumeHle(stream, -500) == snd::kS_OK);
    snd::DirectSoundDoWorkHle();
    const std::vector<float>& sa = fx.Render(10);
    CHECK(Db(sa[0]) == doctest::Approx(Db(0.5f * snd::MixbinGain(-500 - 600))).epsilon(0.001));
    CHECK(VtableSlot<StreamRefFn>(stream, xdk::stream::Release::slot)(stream) == 0);
    CHECK(snd::IDirectSoundBuffer_ReleaseHle(b) == 0);
}

TEST_CASE("dsound buffer: Xbox ADPCM and PCM8 are decoded at SetBufferData (64 frames per 36-byte block)") {
    Fixture fx;
    const snd::XWaveFormatEx adpcm{snd::kWaveFormatXboxAdpcm, 1, 22050, 3, 36, 4, 2};  // nAvgBytesPerSec wrong
    void* b = CreateBuffer(adpcm);
    std::vector<std::uint8_t> blocks(36 * 10, 0);
    REQUIRE(snd::IDirectSoundBuffer_SetBufferDataHle(b, blocks.data(), 360) == snd::kS_OK);
    CHECK(snd::InspectObject(b).frames == 640);
    const snd::XWaveFormatEx pcm8{snd::kWaveFormatPcm, 1, 11025, 11025, 1, 8, 0};
    void* b8 = CreateBuffer(pcm8);
    std::vector<std::uint8_t> bytes(100, 0x80);
    REQUIRE(snd::IDirectSoundBuffer_SetBufferDataHle(b8, bytes.data(), 100) == snd::kS_OK);
    CHECK(snd::InspectObject(b8).frames == 100);
    // Unsupported formats: *pp = NULL (every use of voice+0xA4 tolerates it).
    const snd::XWaveFormatEx bad{2, 1, 22050, 0, 2, 16, 0};  // MS ADPCM
    snd::XDsBufferDesc desc{0x18, 0x100, 0, guest::Addr(&bad), 0, 0};
    void* none = reinterpret_cast<void*>(0x1234);
    CHECK(snd::DirectSoundCreateBufferHle(&desc, &none) != snd::kS_OK);
    CHECK(none == nullptr);
}

TEST_CASE("dsound buffer: category tag from the CreateDsObject probe, one object per call; orphan counter") {
    Fixture fx;
    snd::VoiceTag tag;
    tag.category = snd::Category::Music;
    tag.voiceId = 42;
    std::strcpy(tag.name, "music/01 - front-end.wav");
    snd::SetPendingVoiceTag(tag);
    void* b1 = CreateBuffer(kPcm16Stereo48k);
    void* b2 = CreateBuffer(kPcm16Stereo48k);
    snd::ClearPendingVoiceTag();
    CHECK(snd::InspectObject(b1).category == snd::Category::Music);
    CHECK(snd::InspectObject(b2).category == snd::Category::Movie);  // untagged
    const std::uint32_t before = snd::GetDsoundStatistics().orphans;
    snd::NoteOrphanedObject(b1, 42);
    CHECK(snd::GetDsoundStatistics().orphans == before + 1);
    CHECK(snd::GetDsoundStatistics().tagged >= 1);
}

// ---- streams ---------------------------------------------------------------------------------------------------------

namespace {

struct StreamFixture : Fixture {
    void* stream = nullptr;
    StreamProcessFn process = nullptr;
    StreamGetStatusFn getStatus = nullptr;
    StreamRefFn addRef = nullptr;
    StreamRefFn release = nullptr;
    snd::XWaveFormatEx wfx{snd::kWaveFormatPcm, 2, 48000, 192000, 4, 16, 0x12};

    void Create(std::uint32_t maxPackets, std::uint32_t callback = 0, std::uint32_t context = 0) {
        snd::XDsStreamDesc sd{0, maxPackets, guest::Addr(&wfx), callback, context, 0};
        REQUIRE(snd::DirectSoundCreateStreamHle(&sd, &stream) == snd::kS_OK);
        REQUIRE(stream != nullptr);
        CHECK(static_cast<std::uint64_t>(guest::Addr(stream)) < guest::kGuestMemoryLimit);
        CHECK(snd::InspectObject(stream).startPending);  // running, the voice starts at the next commit
        snd::DirectSoundDoWorkHle();
        CHECK_FALSE(snd::InspectObject(stream).startPending);
        process = VtableSlot<StreamProcessFn>(stream, xdk::stream::Process::slot);
        getStatus = VtableSlot<StreamGetStatusFn>(stream, xdk::stream::GetStatus::slot);
        addRef = VtableSlot<StreamRefFn>(stream, xdk::stream::AddRef::slot);
        release = VtableSlot<StreamRefFn>(stream, xdk::stream::Release::slot);
    }
    std::uint32_t Status() {
        std::uint32_t st = 0;
        REQUIRE(getStatus(stream, &st) == snd::kS_OK);
        return st;
    }
};

// One packet of 480 stereo frames (10 ms at 48 kHz) with slot memory like the game's voice+0x7C status DWORDs.
struct Slots {
    static constexpr std::uint32_t kFrames = 480;
    std::vector<std::int16_t> data[12];
    std::uint32_t status[12] = {};
    std::uint32_t completed[12] = {};
    snd::XMediaPacket pkt[12] = {};
    Slots() {
        for (int i = 0; i < 12; ++i) {
            data[i].assign(kFrames * 2, static_cast<std::int16_t>(1000 * (i + 1)));
            pkt[i].pvBuffer = guest::Addr(data[i].data());
            pkt[i].dwMaxSize = kFrames * 4;
            pkt[i].pdwStatus = guest::Addr(&status[i]);
            pkt[i].pdwCompletedSize = guest::Addr(&completed[i]);
            completed[i] = 0x77777777;
        }
    }
};

}  // namespace

TEST_CASE("dsound stream: E_PENDING synchronously, completion only in DoWork, full queue 0x88780032 (trap 1)") {
    StreamFixture fx;
    fx.Create(10);
    Slots s;
    CHECK((fx.Status() & snd::kStreamStatusReady) != 0);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(fx.process(fx.stream, &s.pkt[i], nullptr) == snd::kS_OK);
        CHECK(s.status[i] == snd::kPacketPending);
        CHECK(s.completed[i] == 0);
    }
    // 11th packet: DSERR_INVALIDCALL, status untouched (0x247676).
    s.status[10] = 0x12345678;
    CHECK(fx.process(fx.stream, &s.pkt[10], nullptr) == snd::kDsErrInvalidCall);
    CHECK(s.status[10] == 0x12345678);
    CHECK((fx.Status() & snd::kStreamStatusReady) == 0);
    CHECK((fx.Status() & snd::kStreamStatusPlaying) != 0);
    // The mixer plays two packets; the guest sees nothing until DirectSoundDoWork (E10: guest writes on the guest
    // thread only).
    fx.Render(2 * Slots::kFrames + 1);
    CHECK(s.status[0] == snd::kPacketPending);
    snd::DirectSoundDoWorkHle();
    CHECK(s.status[0] == snd::kPacketSuccess);
    CHECK(s.completed[0] == Slots::kFrames * 4);
    CHECK(s.status[1] == snd::kPacketSuccess);
    CHECK(s.status[2] == snd::kPacketPending);
    CHECK(snd::DsoundStreamQueuedPackets(fx.stream) == 8);
    CHECK((fx.Status() & snd::kStreamStatusReady) != 0);
    // The packet data were copied at Process: the mixer output is packet 3's constant 3000 (scaled), not garbage.
    REQUIRE(fx.process(fx.stream, &s.pkt[10], nullptr) == snd::kS_OK);
    s.data[2].assign(s.data[2].size(), 0);
    const std::vector<float>& out = fx.Render(10);
    CHECK(out[0] == doctest::Approx(3000.0f / 32768.0f * snd::MixbinGain(-600)).epsilon(1e-4));
    CHECK(fx.release(fx.stream) == 0);
}

TEST_CASE("dsound stream: Pause keeps the queue, FlushEx aborts it and drops PLAYING at once (XMV close)") {
    StreamFixture fx;
    fx.Create(4);
    Slots s;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(fx.process(fx.stream, &s.pkt[i], nullptr) == snd::kS_OK);
    }
    REQUIRE(snd::IDirectSoundStream_PauseHle(fx.stream, snd::kStreamPause) == snd::kS_OK);
    CHECK((fx.Status() & snd::kStreamStatusPaused) != 0);
    CHECK((fx.Status() & snd::kStreamStatusPlaying) == 0);
    fx.Render(5 * Slots::kFrames);
    snd::DirectSoundDoWorkHle();
    CHECK(s.status[0] == snd::kPacketPending);
    REQUIRE(snd::IDirectSoundStream_PauseHle(fx.stream, snd::kStreamResume) == snd::kS_OK);
    snd::DirectSoundDoWorkHle();
    fx.Render(Slots::kFrames + 1);
    snd::DirectSoundDoWorkHle();
    CHECK(s.status[0] == snd::kPacketSuccess);
    CHECK(s.status[1] == snd::kPacketPending);
    REQUIRE(snd::IDirectSoundStream_FlushExHle(fx.stream, 0, snd::kFlushExAsync) == snd::kS_OK);
    CHECK(s.status[1] == snd::kPacketFlushed);
    CHECK(s.status[2] == snd::kPacketFlushed);
    CHECK(s.completed[1] == 0);
    CHECK((fx.Status() & snd::kStreamStatusPlaying) == 0);
    // Late mixer events of the flushed packets change nothing.
    fx.Render(4 * Slots::kFrames);
    snd::DirectSoundDoWorkHle();
    CHECK(s.status[1] == snd::kPacketFlushed);
    // The stream keeps working after a flush.
    REQUIRE(fx.process(fx.stream, &s.pkt[3], nullptr) == snd::kS_OK);
    fx.Render(Slots::kFrames + 1);
    snd::DirectSoundDoWorkHle();
    CHECK(s.status[3] == snd::kPacketSuccess);
    CHECK(fx.release(fx.stream) == 0);
}

TEST_CASE("dsound stream: Release aborts queued packets inside the call and never writes afterwards (trap 2)") {
    StreamFixture fx;
    fx.Create(10);
    Slots s;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(fx.process(fx.stream, &s.pkt[i], nullptr) == snd::kS_OK);
    }
    fx.Render(Slots::kFrames + 1);  // packet 0 played, its event still queued
    CHECK(fx.addRef(fx.stream) == 2);
    CHECK(fx.release(fx.stream) == 1);
    CHECK(snd::IsDsoundStream(fx.stream));
    CHECK(fx.release(fx.stream) == 0);
    CHECK_FALSE(snd::IsDsoundStream(fx.stream));
    for (int i = 0; i < 3; ++i) {
        CHECK(s.status[i] == snd::kPacketFlushed);
    }
    // Guard pattern: the game frees the memory right after Release (0xCC121); nothing may be written there.
    for (int i = 0; i < 3; ++i) {
        s.status[i] = 0xA5A5A5A5;
        s.completed[i] = 0xA5A5A5A5;
    }
    fx.Render(10 * Slots::kFrames);
    snd::DirectSoundDoWorkHle();
    snd::DirectSoundDoWorkHle();
    for (int i = 0; i < 3; ++i) {
        CHECK(s.status[i] == 0xA5A5A5A5);
        CHECK(s.completed[i] == 0xA5A5A5A5);
    }
}

TEST_CASE("dsound stream: Pause(2) + SynchPlayback starts two streams in the same block (XMV)") {
    StreamFixture a;
    a.Create(2);
    void* b = nullptr;
    snd::XDsStreamDesc sd{0, 2, guest::Addr(&a.wfx), 0, 0, 0};
    REQUIRE(snd::DirectSoundCreateStreamHle(&sd, &b) == snd::kS_OK);
    const auto processB = VtableSlot<StreamProcessFn>(b, xdk::stream::Process::slot);
    const auto releaseB = VtableSlot<StreamRefFn>(b, xdk::stream::Release::slot);
    REQUIRE(snd::IDirectSoundStream_PauseHle(a.stream, snd::kStreamPauseSynch) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundStream_PauseHle(b, snd::kStreamPauseSynch) == snd::kS_OK);
    Slots s;
    REQUIRE(a.process(a.stream, &s.pkt[0], nullptr) == snd::kS_OK);
    REQUIRE(processB(b, &s.pkt[1], nullptr) == snd::kS_OK);
    a.Render(3 * Slots::kFrames);
    snd::DirectSoundDoWorkHle();
    CHECK(s.status[0] == snd::kPacketPending);
    CHECK(s.status[1] == snd::kPacketPending);
    REQUIRE(snd::IDirectSound_SynchPlaybackHle(a.ds) == snd::kS_OK);
    a.Render(Slots::kFrames + 1);
    snd::DirectSoundDoWorkHle();
    CHECK(s.status[0] == snd::kPacketSuccess);
    CHECK(s.status[1] == snd::kPacketSuccess);
    CHECK(a.release(a.stream) == 0);
    CHECK(releaseB(b) == 0);
}

namespace {
struct CallbackLog {
    std::uint32_t calls = 0;
    std::uint32_t streamContext = 0;
    std::uint32_t packetContext = 0;
    std::uint32_t status = 0xFFFFFFFF;
};
CallbackLog g_callbackLog;
// LPFNXMEDIAOBJECTCALLBACK: stdcall(pStreamContext, pPacketContext, dwStatus), ret 0xC (XMV 0x264775).
void __stdcall TestStreamCallback(std::uint32_t streamContext, std::uint32_t packetContext,
                                  std::uint32_t status) noexcept {
    ++g_callbackLog.calls;
    g_callbackLog.streamContext = streamContext;
    g_callbackLog.packetContext = packetContext;
    g_callbackLog.status = status;
}
}  // namespace

TEST_CASE("dsound stream: callback streams (XMV) get (context, packet context, status) from DoWork; events") {
    StreamFixture fx;
    g_callbackLog = {};
    fx.Create(2, guest::Addr(reinterpret_cast<const void*>(&TestStreamCallback)), 0xC0FFEE);
    Slots s;
    s.pkt[0].hCompletionEvent = 1234;  // packet context (time stamp) of a callback stream
    REQUIRE(fx.process(fx.stream, &s.pkt[0], nullptr) == snd::kS_OK);
    fx.Render(Slots::kFrames + 1);
    CHECK(g_callbackLog.calls == 0);  // never from the mixer thread
    snd::DirectSoundDoWorkHle();
    CHECK(g_callbackLog.calls == 1);
    CHECK(g_callbackLog.streamContext == 0xC0FFEE);
    CHECK(g_callbackLog.packetContext == 1234);
    CHECK(g_callbackLog.status == snd::kPacketSuccess);
    REQUIRE(fx.process(fx.stream, &s.pkt[1], nullptr) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundStream_FlushExHle(fx.stream, 0, snd::kFlushExAsync) == snd::kS_OK);
    CHECK(g_callbackLog.calls == 2);
    CHECK(g_callbackLog.status == snd::kPacketFlushed);
    CHECK(fx.release(fx.stream) == 0);

    // Without a callback, hCompletionEvent is an event handle signalled at completion.
    StreamFixture ev;
    ev.Create(2);
    const HANDLE h = krnl::CreateEventHandle(krnl::EventObject::Type::Notification, false);
    REQUIRE(h != nullptr);
    Slots t;
    t.pkt[0].hCompletionEvent = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(h));
    REQUIRE(ev.process(ev.stream, &t.pkt[0], nullptr) == snd::kS_OK);
    CHECK_FALSE(krnl::LookupEvent(h)->TryWait());
    ev.Render(Slots::kFrames + 1);
    snd::DirectSoundDoWorkHle();
    CHECK(krnl::LookupEvent(h)->TryWait());
    CHECK(ev.release(ev.stream) == 0);
}

TEST_CASE("dsound stream: Xbox ADPCM packets decoded per packet (19.2 s speech packets keep 64 frames/block)") {
    Fixture fx;
    const snd::XWaveFormatEx adpcm{snd::kWaveFormatXboxAdpcm, 1, 48000, 0, 0x12, 4, 0x12};  // game's wrong fields
    snd::XDsStreamDesc sd{0, 10, guest::Addr(&adpcm), 0, 0, 0};
    void* stream = nullptr;
    REQUIRE(snd::DirectSoundCreateStreamHle(&sd, &stream) == snd::kS_OK);
    REQUIRE(stream != nullptr);
    snd::DirectSoundDoWorkHle();
    const auto process = VtableSlot<StreamProcessFn>(stream, xdk::stream::Process::slot);
    std::vector<std::uint8_t> blocks(36 * 15, 0);  // 960 frames
    std::uint32_t status = 0;
    snd::XMediaPacket pkt{guest::Addr(blocks.data()), 36 * 15, 0, guest::Addr(&status), 0, 0};
    REQUIRE(process(stream, &pkt, nullptr) == snd::kS_OK);
    fx.Render(959);
    snd::DirectSoundDoWorkHle();
    CHECK(status == snd::kPacketPending);
    fx.Render(2);
    snd::DirectSoundDoWorkHle();
    CHECK(status == snd::kPacketSuccess);
    CHECK(VtableSlot<StreamRefFn>(stream, xdk::stream::Release::slot)(stream) == 0);
}

// ---- device-wide state -----------------------------------------------------------------------------------------------

TEST_CASE("dsound listener and I3DL2: deferred values apply at CommitDeferredSettings, immediate ones at once") {
    Fixture fx;
    REQUIRE(snd::IDirectSound_SetPositionHle(fx.ds, 1.0f, 2.0f, 3.0f, 1) == snd::kS_OK);
    CHECK(snd::CommittedListener().position.x == 0.0f);
    REQUIRE(snd::IDirectSound_SetOrientationHle(fx.ds, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1) == snd::kS_OK);
    REQUIRE(snd::IDirectSound_CommitDeferredSettingsHle(fx.ds) == snd::kS_OK);
    CHECK(snd::CommittedListener().position.z == 3.0f);
    CHECK(snd::CommittedListener().front.x == -1.0f);
    REQUIRE(snd::IDirectSound_SetDopplerFactorHle(fx.ds, 2.0f, 0) == snd::kS_OK);  // immediate (init 0xCCD73)
    CHECK(snd::CommittedListener().dopplerFactor == 2.0f);
    // Padded Cell (table 0x2D6818 index 2), deferred like the init 0xCCDC7.
    const snd::XDsI3dl2Listener padded{-1000, -6000, 0.0f, 0.17f, 0.1f, -1204, 0.001f, 207, 0.002f, 100.0f, 100.0f,
                                       5000.0f};
    REQUIRE(snd::IDirectSound_SetI3DL2ListenerHle(fx.ds, &padded, 1) == snd::kS_OK);
    CHECK(snd::CommittedReverb().roomHF == -100);  // still the default
    REQUIRE(snd::IDirectSound_CommitDeferredSettingsHle(fx.ds) == snd::kS_OK);
    CHECK(snd::CommittedReverb().roomHF == -6000);
    CHECK(snd::CommittedReverb().decayTime == doctest::Approx(0.17f));
    CHECK(snd::CommittedReverb().reverb == 207);
}

TEST_CASE("dsound 3D buffer: voice gets the reverb send, 3D values commit with the listener; no headroom") {
    Fixture fx;
    void* b = CreateBuffer(kPcm16Mono48k, 0x60110);
    CHECK(snd::InspectObject(b).is3D);
    std::vector<std::int16_t> dc(4800, 16384);
    REQUIRE(snd::IDirectSoundBuffer_SetBufferDataHle(b, dc.data(), 9600) == snd::kS_OK);
    static const float curve[6] = {1.0f, 0.9f, 0.8f, 0.5f, 0.25f, 0.0f};  // 0x2EAD3C
    REQUIRE(snd::IDirectSoundBuffer_SetRolloffCurveHle(b, curve, 6, 0) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundBuffer_SetMinDistanceHle(b, 5.0f, 1) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundBuffer_SetMaxDistanceHle(b, 80.0f, 1) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundBuffer_SetPositionHle(b, 0.0f, 0.0f, 1.0f, 1) == snd::kS_OK);
    const snd::XDsI3dl2Buffer src{0, 0, -600, 0, 0.0f, 0, 0.0f, 0, 0.0f};
    REQUIRE(snd::IDirectSoundBuffer_SetI3DL2SourceHle(b, &src, 1) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundBuffer_SetVolumeHle(b, -250) == snd::kS_OK);
    REQUIRE(snd::IDirectSound_SetOrientationHle(fx.ds, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1) == snd::kS_OK);
    REQUIRE(snd::IDirectSound_CommitDeferredSettingsHle(fx.ds) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundBuffer_PlayHle(b, 0, 0, 0) == snd::kS_OK);
    Commit(fx.ds);
    const snd::ObjectView v = snd::InspectObject(b);
    // Inside MinDistance at -250 mB: the direct path is at most -2.5 dB per channel (no 6 dB headroom for 3D; the
    // Xbox feeds both ears at the bin level, m5_gain_model.out) and the reverb send (lRoom -600) is -8.5 dB.
    const float loudest = std::max(v.matrix[0], v.matrix[1]);
    CHECK(loudest > 0.0f);
    CHECK(Db(loudest) <= -2.5 + 0.01);
    CHECK(Db(loudest) > -6.0);
    CHECK(Db(v.reverbSend) == doctest::Approx(-8.5).epsilon(0.01));
    CHECK(snd::IDirectSoundBuffer_ReleaseHle(b) == 0);
}

// ---- engine --------------------------------------------------------------------------------------------------------

namespace {
struct RecordingEvents final : snd::VoiceEvents {
    std::vector<std::uint32_t> ended;
    void OnBufferEnd(std::uint32_t context) noexcept override { ended.push_back(context); }
};
struct CountingTap final : snd::MixTap {
    std::uint64_t frames[static_cast<int>(snd::TapPoint::Count)] = {};
    void OnMix(snd::TapPoint point, const float*, std::uint32_t n, std::uint32_t, std::uint32_t) noexcept override {
        frames[static_cast<int>(point)] += n;
    }
};
}  // namespace

TEST_CASE("audio engine (software mixer): flush reports buffers asynchronously, frames played, taps per category") {
    snd::EngineConfig config;
    config.enabled = false;
    config.softwareRealtime = false;
    std::unique_ptr<snd::AudioEngine> engine = snd::CreateSoftwareEngine(config);
    REQUIRE(engine != nullptr);
    CHECK_FALSE(engine->HasDevice());
    CHECK(std::string(engine->BackendName()) == "software");
    CHECK(engine->OutputChannels() == 2);
    CHECK(engine->OutputSampleRate() == 48000);
    CountingTap tap;
    engine->SetTap(&tap);
    RecordingEvents events;
    snd::VoiceDesc d;
    d.format = {1, 24000};
    d.category = snd::Category::Speech;
    d.events = &events;
    {
        std::unique_ptr<snd::SourceVoice> v = engine->CreateVoice(d);
        REQUIRE(v != nullptr);
        std::vector<std::int16_t> pcm(240, 1000);
        snd::SubmitDesc s;
        s.data = pcm.data();
        s.frames = 240;
        s.context = 7;
        REQUIRE(v->Submit(s));
        s.context = 8;
        REQUIRE(v->Submit(s));
        v->Start();
        engine->RenderSoftware(480, nullptr);  // 24 kHz source: 240 frames per 480 output frames
        REQUIRE(events.ended.size() == 1);
        CHECK(events.ended[0] == 7);
        CHECK(v->FramesPlayed() == 240);
        CHECK(v->BuffersQueued() == 1);
        v->Flush();
        CHECK(events.ended.size() == 1);  // asynchronous, like XAudio2
        engine->RenderSoftware(10, nullptr);
        REQUIRE(events.ended.size() == 2);
        CHECK(events.ended[1] == 8);
        CHECK(engine->GetStats().voicesLive == 1);
    }
    CHECK(engine->GetStats().voicesLive == 0);
    CHECK(engine->GetStats().voicesCreated == 1);
    CHECK(engine->OutputFramesRendered() == 490);
    CHECK(tap.frames[static_cast<int>(snd::TapPoint::Master)] == 490);
    CHECK(tap.frames[static_cast<int>(snd::TapPoint::Speech)] == 490);
    engine->SetTap(nullptr);
}

TEST_CASE("audio engine (5.1): the LFE channel is low-passed at 120 Hz like Dolby Digital's LFE, others untouched") {
    const auto levelDb = [](double hz, std::uint32_t outputChannel) {
        snd::EngineConfig config;
        config.enabled = false;
        config.softwareRealtime = false;
        config.reverb = false;
        config.limiter = false;
        config.speakers = 6;
        std::unique_ptr<snd::AudioEngine> engine = snd::CreateSoftwareEngine(config);
        REQUIRE(engine != nullptr);
        REQUIRE(engine->OutputChannels() == 6);
        snd::VoiceDesc d;
        d.format = {1, 48000};
        std::unique_ptr<snd::SourceVoice> v = engine->CreateVoice(d);
        REQUIRE(v != nullptr);
        std::vector<std::int16_t> sine(48000);
        for (std::size_t i = 0; i < sine.size(); ++i) {
            sine[i] = static_cast<std::int16_t>(16384.0 * std::sin(2.0 * 3.14159265358979 * hz * i / 48000.0));
        }
        float matrix[6] = {};
        matrix[outputChannel] = 1.0f;
        v->SetOutputMatrix(matrix);
        snd::SubmitDesc s;
        s.data = sine.data();
        s.frames = 48000;
        REQUIRE(v->Submit(s));
        v->Start();
        std::vector<float> out(48000 * 6);
        engine->RenderSoftware(48000, out.data());
        double sum = 0;
        for (std::size_t f = 24000; f < 48000; ++f) {  // after the filter settled
            sum += static_cast<double>(out[f * 6 + outputChannel]) * out[f * 6 + outputChannel];
        }
        return 20.0 * std::log10(std::sqrt(sum / 24000.0) / (0.5 / std::sqrt(2.0)) + 1e-12);
    };
    CHECK(levelDb(40.0, 3) > -0.5);    // passband
    CHECK(levelDb(120.0, 3) == doctest::Approx(-3.0).epsilon(0.1));  // -3 dB corner (Butterworth)
    CHECK(levelDb(1000.0, 3) < -60.0);  // 4th order: about -73 dB a decade above
    CHECK(levelDb(1000.0, 2) == doctest::Approx(0.0).epsilon(0.05));  // centre channel untouched
}

TEST_CASE("audio engine: a backend switch continues the playing buffers at their position (no restart)") {
    snd::EngineConfig config;
    config.enabled = false;
    config.softwareRealtime = false;
    config.reverb = false;
    std::unique_ptr<snd::AudioEngine> engine = snd::CreateSoftwareEngine(config);
    REQUIRE(engine != nullptr);
    // 48 kHz mono at a 48 kHz output: the resampler reads integer positions, so out[f] = sample[pos] exactly.
    std::vector<std::int16_t> ramp(1000);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<std::int16_t>(i * 16);
    }
    const auto sampleAt = [](const std::vector<float>& out, std::size_t frame) {
        return static_cast<int>(std::lround(out[frame * 2] * 32768.0f / 16.0f));  // left channel, ramp index
    };
    RecordingEvents events;
    snd::VoiceDesc d;
    d.format = {1, 48000};
    d.events = &events;
    std::unique_ptr<snd::SourceVoice> oneShot = engine->CreateVoice(d);
    REQUIRE(oneShot != nullptr);
    snd::SubmitDesc a;
    a.data = ramp.data();
    a.frames = 1000;
    a.playBegin = 100;
    a.context = 1;
    REQUIRE(oneShot->Submit(a));
    snd::SubmitDesc b = a;
    b.playBegin = 0;
    b.context = 2;
    REQUIRE(oneShot->Submit(b));
    oneShot->Start();
    std::vector<float> out(2 * 2000);
    engine->RenderSoftware(300, out.data());
    CHECK(sampleAt(out, 0) == 100);
    CHECK(sampleAt(out, 299) == 399);
    snd::SwitchToNewSoftwareBackendForTesting(*engine);
    CHECK(oneShot->FramesPlayed() == 300);
    engine->RenderSoftware(700, out.data());
    CHECK(sampleAt(out, 0) == 400);  // continues (a restart would give 100)
    CHECK(sampleAt(out, 599) == 999);
    CHECK(sampleAt(out, 600) == 0);  // buffer 2 follows seamlessly
    REQUIRE(events.ended.size() == 1);
    CHECK(events.ended[0] == 1);
    CHECK(oneShot->FramesPlayed() == 1000);
    // Two switches in a row and a flush in between keep the bookkeeping right.
    snd::SwitchToNewSoftwareBackendForTesting(*engine);
    engine->RenderSoftware(50, out.data());
    CHECK(sampleAt(out, 0) == 100);
    oneShot->Stop();
    oneShot->Flush();
    engine->RenderSoftware(10, nullptr);
    REQUIRE(events.ended.size() == 2);
    snd::SubmitDesc c = a;
    c.playBegin = 0;
    c.context = 3;
    REQUIRE(oneShot->Submit(c));
    oneShot->Start();
    engine->RenderSoftware(250, out.data());
    CHECK(sampleAt(out, 0) == 0);
    snd::SwitchToNewSoftwareBackendForTesting(*engine);
    engine->RenderSoftware(10, out.data());
    CHECK(sampleAt(out, 0) == 250);
    oneShot.reset();

    // A looping buffer continues modulo its length.
    std::unique_ptr<snd::SourceVoice> loop = engine->CreateVoice(d);
    REQUIRE(loop != nullptr);
    snd::SubmitDesc l;
    l.data = ramp.data();
    l.frames = 500;
    l.loop = true;
    l.context = 9;
    REQUIRE(loop->Submit(l));
    loop->Start();
    engine->RenderSoftware(1200, out.data());
    CHECK(sampleAt(out, 1199) == 199);
    snd::SwitchToNewSoftwareBackendForTesting(*engine);
    engine->RenderSoftware(400, out.data());
    CHECK(sampleAt(out, 0) == 200);
    CHECK(sampleAt(out, 299) == 499);
    CHECK(sampleAt(out, 300) == 0);
}

TEST_CASE("audio engine (software mixer): resampler 22050 -> 48000 Hz keeps a 1 kHz sine at > 60 dB SNR") {
    snd::EngineConfig config;
    config.enabled = false;
    config.softwareRealtime = false;
    config.masterVolume = 0.5f;  // applied after the taps, to the output only
    std::unique_ptr<snd::AudioEngine> engine = snd::CreateSoftwareEngine(config);
    REQUIRE(engine != nullptr);
    CountingTap tap;
    engine->SetTap(&tap);
    snd::VoiceDesc d;
    d.format = {1, 22050};
    d.category = snd::Category::Music;
    std::unique_ptr<snd::SourceVoice> v = engine->CreateVoice(d);
    REQUIRE(v != nullptr);
    constexpr double kPi = 3.14159265358979323846;
    std::vector<std::int16_t> pcm(22050);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::int16_t>(std::lround(16384.0 * std::sin(2.0 * kPi * 1000.0 * i / 22050.0)));
    }
    snd::SubmitDesc s;
    s.data = pcm.data();
    s.frames = static_cast<std::uint32_t>(pcm.size());
    REQUIRE(v->Submit(s));
    const float unity[2] = {1.0f, 1.0f};
    v->SetOutputMatrix(unity);
    v->Start();
    std::vector<float> out(2 * 24000);
    engine->RenderSoftware(24000, out.data());
    double signal = 0.0, error = 0.0;
    for (std::size_t k = 100; k < 23000; ++k) {
        const double ideal = 0.5 * 0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(k) / 48000.0);
        signal += ideal * ideal;
        error += (out[2 * k] - ideal) * (out[2 * k] - ideal);
    }
    const double snr = 10.0 * std::log10(signal / error);
    MESSAGE("software mixer resampler SNR at 1 kHz: " << snr << " dB");
    CHECK(snr > 60.0);
    CHECK(out[2 * 1000] == doctest::Approx(out[2 * 1000 + 1]));
    engine->SetTap(nullptr);
}

TEST_CASE("audio engine (software mixer): limiter keeps the output below 0 dBFS, the Master tap sees the plain mix") {
    struct PeakTap final : snd::MixTap {
        float peak = 0.0f;
        void OnMix(snd::TapPoint p, const float* s, std::uint32_t frames, std::uint32_t ch,
                   std::uint32_t) noexcept override {
            if (p == snd::TapPoint::Master) {
                for (std::uint32_t i = 0; i < frames * ch; ++i) {
                    peak = std::max(peak, std::fabs(s[i]));
                }
            }
        }
    };
    for (const bool limiter : {true, false}) {
        snd::EngineConfig config;
        config.enabled = false;
        config.softwareRealtime = false;
        config.limiter = limiter;
        std::unique_ptr<snd::AudioEngine> engine = snd::CreateSoftwareEngine(config);
        REQUIRE(engine != nullptr);
        PeakTap tap;
        engine->SetTap(&tap);
        snd::VoiceDesc d;
        d.format = {1, 48000};
        std::unique_ptr<snd::SourceVoice> v = engine->CreateVoice(d);
        REQUIRE(v != nullptr);
        std::vector<std::int16_t> pcm(4800);
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            pcm[i] = (i / 50) % 2 == 0 ? 30000 : -30000;  // two loud voices' worth: 2 x 0.92 after the matrix
        }
        snd::SubmitDesc s;
        s.data = pcm.data();
        s.frames = 4800;
        REQUIRE(v->Submit(s));
        const float twice[2] = {2.0f, 2.0f};
        v->SetOutputMatrix(twice);
        v->Start();
        std::vector<float> out(2 * 4800);
        engine->RenderSoftware(4800, out.data());
        float peak = 0.0f;
        for (const float x : out) {
            peak = std::max(peak, std::fabs(x));
        }
        CHECK(tap.peak > 1.5f);  // the capture keeps the mix before the limiter (m5_audio.md 9.2 R9)
        if (limiter) {
            CHECK(peak <= 0.99f);
            CHECK(engine->GetStats().limitedFrames > 0);
            CHECK(engine->GetStats().limiterMaxReductionDb > 5.0f);
        } else {
            CHECK(peak > 1.5f);
            CHECK(engine->GetStats().limitedFrames == 0);
        }
        engine->SetTap(nullptr);
    }
}

TEST_CASE("audio engine: CreateAudioEngine always returns an engine (XAudio2 device or the software mixer)") {
    snd::EngineConfig config;
    config.enabled = true;
    std::unique_ptr<snd::AudioEngine> engine = snd::CreateAudioEngine(config);
    REQUIRE(engine != nullptr);
    const std::string backend = engine->BackendName();
    CHECK((backend == "xaudio2" || backend == "software"));
    CHECK(engine->OutputChannels() >= 1);
    MESSAGE("audio engine backend on this machine: " << backend << ", " << engine->OutputChannels() << " ch "
                                                    << engine->OutputSampleRate() << " Hz");
}

TEST_CASE("audio engine: device lost -> software mixer -> device back; streams keep completing (trap 15)") {
    snd::EngineConfig config;
    config.enabled = true;
    std::unique_ptr<snd::AudioEngine> engine = snd::CreateAudioEngine(config);
    REQUIRE(engine != nullptr);
    if (!engine->HasDevice()) {
        MESSAGE("no audio output device on this machine: device switching not tested");
        return;
    }
    snd::SetEngineForTesting(engine.get());
    snd::ResetDsoundForTesting();
    void* ds = nullptr;
    REQUIRE(snd::DirectSoundCreateHle(nullptr, &ds, nullptr) == snd::kS_OK);
    const snd::XWaveFormatEx wfx{snd::kWaveFormatPcm, 2, 48000, 192000, 4, 16, 0x12};
    snd::XDsStreamDesc sd{0, 10, guest::Addr(&wfx), 0, 0, 0};
    void* stream = nullptr;
    REQUIRE(snd::DirectSoundCreateStreamHle(&sd, &stream) == snd::kS_OK);
    const auto process = VtableSlot<StreamProcessFn>(stream, xdk::stream::Process::slot);
    std::vector<std::int16_t> silence(4800 * 2, 0);  // 0.1 s per packet, inaudible
    std::uint32_t status[10] = {};
    std::uint32_t refills = 0;
    // Keeps the queue full like the game's pump; returns the packets completed since the last call.
    const auto pump = [&] {
        snd::DirectSoundDoWorkHle();
        std::uint32_t done = 0;
        for (std::uint32_t& st : status) {
            if (st != snd::kPacketPending) {
                done += st == snd::kPacketSuccess && refills >= 10 ? 1u : 0u;
                snd::XMediaPacket pkt{guest::Addr(silence.data()), 4800 * 4, 0, guest::Addr(&st), 0, 0};
                REQUIRE(process(stream, &pkt, nullptr) == snd::kS_OK);
                ++refills;
            }
        }
        return done;
    };
    pump();
    Sleep(300);
    CHECK(pump() >= 1);
    CHECK(std::string(engine->BackendName()) == "xaudio2");
    snd::SimulateDeviceLossForTesting(*engine);
    pump();
    CHECK(std::string(engine->BackendName()) == "software");
    CHECK(engine->GetStats().deviceLosses == 1);
    Sleep(400);
    CHECK(pump() >= 2);  // the software mixer plays the re-submitted packets in real time
    snd::RetryDeviceNowForTesting(*engine);
    pump();
    CHECK(std::string(engine->BackendName()) == "xaudio2");
    Sleep(400);
    CHECK(pump() >= 2);
    CHECK(VtableSlot<StreamRefFn>(stream, xdk::stream::Release::slot)(stream) == 0);
    snd::ResetDsoundForTesting();
    snd::SetEngineForTesting(nullptr);
}


TEST_CASE("dsound stream: an emptied queue while running counts one underrun; flush and pause do not") {
    StreamFixture fx;
    fx.Create(4);
    Slots s;
    REQUIRE(fx.process(fx.stream, &s.pkt[0], nullptr) == snd::kS_OK);
    REQUIRE(fx.process(fx.stream, &s.pkt[1], nullptr) == snd::kS_OK);
    fx.Render(Slots::kFrames + 1);
    snd::DirectSoundDoWorkHle();
    CHECK(snd::GetDsoundStatistics().starvations == 0);
    fx.Render(2 * Slots::kFrames);
    snd::DirectSoundDoWorkHle();
    snd::DirectSoundDoWorkHle();
    CHECK(snd::GetDsoundStatistics().starvations == 1);  // one episode, reported once
    REQUIRE(fx.process(fx.stream, &s.pkt[2], nullptr) == snd::kS_OK);
    snd::DirectSoundDoWorkHle();
    REQUIRE(snd::IDirectSoundStream_FlushExHle(fx.stream, 0, snd::kFlushExAsync) == snd::kS_OK);
    snd::DirectSoundDoWorkHle();
    CHECK(snd::GetDsoundStatistics().starvations == 1);
    REQUIRE(fx.process(fx.stream, &s.pkt[3], nullptr) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundStream_PauseHle(fx.stream, snd::kStreamPause) == snd::kS_OK);
    REQUIRE(snd::IDirectSoundStream_FlushExHle(fx.stream, 0, snd::kFlushExAsync) == snd::kS_OK);
    snd::DirectSoundDoWorkHle();
    CHECK(snd::GetDsoundStatistics().starvations == 1);
    CHECK(fx.release(fx.stream) == 0);
}

// ---- M5 review fixes: engine ------------------------------------------------------------------------------------------

TEST_CASE("audio engine (software mixer): without a listener only positions advance - same frames, same buffer ends") {
    // A mixer nobody listens to (no tap, no output buffer: no device and no capture_wav) skips resampling, reverb and
    // the limiter (review finding M5-15); the game must not notice: OnBufferEnd at the same render call, the same
    // FramesPlayed, also with a pitch and a flush.
    struct Trace {
        std::vector<std::pair<int, std::uint32_t>> ends;  // (render call, context)
        std::vector<std::uint64_t> played;
    };
    const auto run = [](bool listen) {
        snd::EngineConfig config;
        config.enabled = false;
        config.softwareRealtime = false;
        std::unique_ptr<snd::AudioEngine> engine = snd::CreateSoftwareEngine(config);
        REQUIRE(engine != nullptr);
        CountingTap tap;
        if (listen) {
            engine->SetTap(&tap);
        }
        struct Events final : snd::VoiceEvents {
            Trace* trace = nullptr;
            int call = 0;
            void OnBufferEnd(std::uint32_t context) noexcept override { trace->ends.emplace_back(call, context); }
        };
        Trace trace;
        Events events;
        events.trace = &trace;
        snd::VoiceDesc d;
        d.format = {1, 22050};
        d.events = &events;
        std::unique_ptr<snd::SourceVoice> v = engine->CreateVoice(d);
        REQUIRE(v != nullptr);
        std::vector<std::int16_t> pcm(3001, 1000);
        for (std::uint32_t c = 1; c <= 6; ++c) {
            snd::SubmitDesc s;
            s.data = pcm.data();
            s.frames = 1000 + 333 * c;
            s.context = c;
            REQUIRE(v->Submit(s));
        }
        v->SetFrequencyRatio(1.37f);
        v->Start();
        for (int call = 0; call < 40; ++call) {
            events.call = call;
            if (call == 20) {
                v->Flush();
            }
            engine->RenderSoftware(480, nullptr);
            trace.played.push_back(v->FramesPlayed());
        }
        engine->SetTap(nullptr);
        return trace;
    };
    const Trace full = run(true);
    const Trace light = run(false);
    CHECK(full.ends.size() == 6);
    CHECK(full.ends == light.ends);
    CHECK(full.played == light.played);
}

TEST_CASE("audio engine: tap formats for the capture's rings (allocated up front, not on the mixer thread)") {
    snd::EngineConfig config;
    config.enabled = false;
    config.softwareRealtime = false;
    config.speakers = 6;
    std::unique_ptr<snd::AudioEngine> engine = snd::CreateSoftwareEngine(config);
    REQUIRE(engine != nullptr);
    std::uint32_t channels = 0, rate = 0;
    REQUIRE(engine->TapFormat(snd::TapPoint::Master, channels, rate));
    CHECK(channels == 6);
    CHECK(rate == 48000);
    REQUIRE(engine->TapFormat(snd::TapPoint::Music, channels, rate));
    CHECK(channels == 6);
    // The reverb tap delivers the reverb effect's channels; the block that reaches the tap has that format.
    struct FormatTap final : snd::MixTap {
        std::uint32_t reverbChannels = 0;
        void OnMix(snd::TapPoint p, const float*, std::uint32_t, std::uint32_t ch, std::uint32_t) noexcept override {
            if (p == snd::TapPoint::Reverb) {
                reverbChannels = ch;
            }
        }
    };
    FormatTap tap;
    engine->SetTap(&tap);
    engine->RenderSoftware(480, nullptr);
    REQUIRE(engine->TapFormat(snd::TapPoint::Reverb, channels, rate));
    CHECK(channels == tap.reverbChannels);
    CHECK_FALSE(engine->TapFormat(snd::TapPoint::Count, channels, rate));
    engine->SetTap(nullptr);
}

TEST_CASE("audio engine: without an output device the device worker answers off the guest thread (no stall)") {
    // Device handling happens on the "audio device worker" thread (review finding M5-6): Pump only hands over. On a
    // machine without an active endpoint (TV off) the engine stays on the software mixer; with one it opens XAudio2.
    snd::EngineConfig config;
    config.enabled = true;
    std::unique_ptr<snd::AudioEngine> engine = snd::CreateAudioEngine(config);
    REQUIRE(engine != nullptr);
    const bool device = engine->HasDevice();
    snd::RetryDeviceNowForTesting(*engine);  // asks the worker and waits for its answer
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        engine->Pump();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("100 Pump calls: " << ms << " ms, backend " << std::string(engine->BackendName()));
    CHECK(ms < 200.0);
    if (!device) {
        // No endpoint: a device that appeared meanwhile would have been handed over; otherwise still software.
        CHECK((std::string(engine->BackendName()) == "software" || engine->HasDevice()));
    } else {
        CHECK(engine->HasDevice());
    }
}

namespace {
// Active render endpoints of this machine (IMMDevice ids, the format the launcher stores in [audio] device).
std::vector<std::wstring> ActiveRenderEndpoints() {
    std::vector<std::wstring> ids;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* e = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&e)))) {
        IMMDeviceCollection* c = nullptr;
        if (SUCCEEDED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &c))) {
            UINT n = 0;
            c->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                IMMDevice* d = nullptr;
                LPWSTR id = nullptr;
                if (SUCCEEDED(c->Item(i, &d)) && SUCCEEDED(d->GetId(&id))) {
                    ids.emplace_back(id);
                    CoTaskMemFree(id);
                }
                if (d != nullptr) {
                    d->Release();
                }
            }
            c->Release();
        }
        e->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return ids;
}
}  // namespace

TEST_CASE("audio engine (XAudio2): [audio] device takes the launcher's IMMDevice id; a missing one falls back") {
    // Review finding M5-8: the id format "{0.0.0.00000000}.{guid}" must open that device (not silently the default).
    const std::vector<std::wstring> ids = ActiveRenderEndpoints();
    if (ids.empty()) {
        MESSAGE("no active audio output device on this machine: [audio] device not tested");
        return;
    }
    for (const std::wstring& id : ids) {
        snd::EngineConfig config;
        config.enabled = true;
        config.deviceId = id;
        std::unique_ptr<snd::AudioEngine> engine = snd::CreateAudioEngine(config);
        REQUIRE(engine != nullptr);
        REQUIRE(engine->HasDevice());
        const std::string description = snd::EngineDescriptionForTesting(*engine);
        MESSAGE("[audio] device opened: " << description);
        CHECK(description.find("device from [audio] device") != std::string::npos);
        std::uint32_t channels = 0, rate = 0;
        CHECK(engine->TapFormat(snd::TapPoint::Master, channels, rate));
        CHECK(channels == engine->OutputChannels());
    }
    snd::EngineConfig missing;
    missing.enabled = true;
    missing.deviceId = L"{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}";
    std::unique_ptr<snd::AudioEngine> fallback = snd::CreateAudioEngine(missing);
    REQUIRE(fallback != nullptr);
    CHECK(fallback->HasDevice());  // the default device
    CHECK(snd::EngineDescriptionForTesting(*fallback).find("default device") != std::string::npos);
}

TEST_CASE("audio engine (XAudio2): FramesPlayed stays monotonic over Discontinuity; a switch keeps the position") {
    // Review finding M5-9: XAudio2 restarts SamplesPlayed after an END_OF_STREAM buffer (XMV's Discontinuity).
    snd::EngineConfig config;
    config.enabled = true;
    std::unique_ptr<snd::AudioEngine> engine = snd::CreateAudioEngine(config);
    REQUIRE(engine != nullptr);
    if (!engine->HasDevice()) {
        MESSAGE("no audio output device on this machine: XAudio2 Discontinuity not tested");
        return;
    }
    engine->SetMasterVolume(0.0f);  // silent
    RecordingEvents events;
    snd::VoiceDesc d;
    d.format = {1, 48000};
    d.events = &events;
    std::unique_ptr<snd::SourceVoice> v = engine->CreateVoice(d);
    REQUIRE(v != nullptr);
    std::vector<std::int16_t> silence(4800, 0);
    snd::SubmitDesc s;
    s.data = silence.data();
    s.frames = 4800;
    s.context = 1;
    REQUIRE(v->Submit(s));
    v->Discontinuity();
    v->Start();
    for (int i = 0; i < 100 && events.ended.empty(); ++i) {
        Sleep(10);
    }
    REQUIRE(events.ended.size() == 1);
    Sleep(30);  // the base grows at the end of the XAudio2 pass that ended the stream
    const std::uint64_t first = v->FramesPlayed();
    CHECK(first <= 4800);
    CHECK(first >= 4800 - 480);  // at most one pass (10 ms) short: the value of the pass start (audio_engine.cpp)
    s.context = 2;
    REQUIRE(v->Submit(s));
    for (int i = 0; i < 100 && events.ended.size() < 2; ++i) {
        Sleep(10);
    }
    REQUIRE(events.ended.size() == 2);
    const std::uint64_t second = v->FramesPlayed();
    CHECK(second >= first);
    s.context = 3;
    REQUIRE(v->Submit(s));
    Sleep(30);
    const std::uint64_t before = v->FramesPlayed();
    snd::SwitchToNewSoftwareBackendForTesting(*engine);
    CHECK(v->FramesPlayed() >= before);
    v.reset();
}
