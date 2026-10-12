// Unit tests of the M5 reverb module (audio/reverb_*.h): guest I3DL2 structures, the five game environments,
// the conversion to XAudio2 reverb parameters (m5_i3dl2.out), the reverb output matrix, the DSP effects image
// parser (synthetic image; the real dsstdfx.bin only from run\ via FUT_AUDIO_DSSTDFX) and the stand-alone
// reverb XAPO.
#include "doctest.h"

#include "audio/audio_api.h"
#include "audio/reverb_i3dl2.h"
#include "audio/reverb_software.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace {

using namespace snd::reverb;

void PutU32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t value) {
    std::memcpy(v.data() + at, &value, 4);
}

// Image with 2 effects: header at 0x800 (code 0x10 words, state 0x8 words), descriptor at 0x878.
std::vector<std::uint8_t> SyntheticImage() {
    std::vector<std::uint8_t> v(0x878 + 8 + 2 * 32 + 2 * 8, 0);
    PutU32(v, 0x804, 0x10);
    PutU32(v, 0x80C, 0x8);
    PutU32(v, 0x878, 2);       // effects
    PutU32(v, 0x87C, 0x1000);  // total scratch
    const std::uint32_t e0[8] = {0x818, 0x20, 0x858, 0x10, 0, 0, 0xC000, 0x800};
    const std::uint32_t e1[8] = {0x838, 0x20, 0x868, 0x10, 0, 0x20, 0xC800, 0};
    for (int i = 0; i < 8; ++i) {
        PutU32(v, 0x880 + 4 * i, e0[i]);
        PutU32(v, 0x8A0 + 4 * i, e1[i]);
    }
    return v;
}

double Energy(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double e = 0;
    for (std::size_t i = from; i < to && i < v.size(); ++i) {
        e += static_cast<double>(v[i]) * v[i];
    }
    return e;
}

// Impulse response of the stand-alone reverb: 1 s at 48 kHz, 6 output channels.
std::vector<float> ImpulseResponse(SoftwareReverb& r, const snd::I3dl2Listener& env) {
    r.Reset();
    r.SetListener(env);
    const std::uint32_t block = r.BlockFrames();
    std::vector<float> in(block, 0.0f);
    std::vector<float> out(static_cast<std::size_t>(48000) * 6, 0.0f);
    for (std::uint32_t done = 0; done < 48000; done += block) {
        in[0] = done == 0 ? 1.0f : 0.0f;
        r.Process(in.data(), out.data() + static_cast<std::size_t>(done) * 6, block);
    }
    return out;
}

}  // namespace

TEST_CASE("reverb: the five game environments equal the SDK's standard I3DL2 presets") {
    const XAUDIO2FX_REVERB_I3DL2_PARAMETERS sdk[kGameEnvironmentCount] = {
        XAUDIO2FX_I3DL2_PRESET_PADDEDCELL, XAUDIO2FX_I3DL2_PRESET_ROOM, XAUDIO2FX_I3DL2_PRESET_LIVINGROOM,
        XAUDIO2FX_I3DL2_PRESET_STONEROOM, XAUDIO2FX_I3DL2_PRESET_CONCERTHALL};
    for (std::uint32_t i = 0; i < kGameEnvironmentCount; ++i) {
        const GameEnvironment* env = GameEnvironmentPreset(i);
        REQUIRE(env != nullptr);
        CAPTURE(env->name);
        const snd::I3dl2Listener& g = env->listener;
        CHECK(g.room == sdk[i].Room);
        CHECK(g.roomHF == sdk[i].RoomHF);
        CHECK(g.roomRolloffFactor == sdk[i].RoomRolloffFactor);
        CHECK(g.decayTime == sdk[i].DecayTime);
        CHECK(g.decayHFRatio == sdk[i].DecayHFRatio);
        CHECK(g.reflections == sdk[i].Reflections);
        CHECK(g.reflectionsDelay == sdk[i].ReflectionsDelay);
        CHECK(g.reverb == sdk[i].Reverb);
        CHECK(g.reverbDelay == sdk[i].ReverbDelay);
        CHECK(g.diffusion == sdk[i].Diffusion);
        CHECK(g.density == sdk[i].Density);
        CHECK(g.hfReference == sdk[i].HFReference);
    }
    CHECK(GameEnvironmentPreset(5) == nullptr);
    CHECK(std::strcmp(GameEnvironmentPreset(0)->name, "Padded Cell") == 0);
}

TEST_CASE("reverb: native parameters of the game environments (m5_i3dl2.out, 5.1 effect: rear delay 5 ms)") {
    struct Expect {
        float roomFilterHF;
        std::uint8_t highEQGain;
        float decay;
        std::uint32_t reflectionsDelay;
        std::uint8_t reverbDelay;
        float reflectionsGain;
        float reverbGain;
    };
    const Expect expect[kGameEnvironmentCount] = {{-60.0f, 5, 0.17f, 1, 2, -12.04f, 2.07f},
                                                  {-4.54f, 8, 0.40f, 2, 3, -16.46f, 0.53f},
                                                  {-60.0f, 5, 0.50f, 3, 4, -13.76f, -11.04f},
                                                  {-3.0f, 8, 2.31f, 12, 17, -7.11f, 0.83f},
                                                  // m5_i3dl2.out (Python doubles) has 19/28 here: 0.02f *
                                                  // 1000.0f is 20.0f in float arithmetic as in the SDK.
                                                  {-5.0f, 8, 3.92f, 20, 29, -12.3f, -0.02f}};
    for (std::uint32_t i = 0; i < kGameEnvironmentCount; ++i) {
        CAPTURE(i);
        const XAUDIO2FX_REVERB_PARAMETERS p = ToNative(GameEnvironmentPreset(i)->listener);
        CHECK(ValidNative(p));
        CHECK(p.WetDryMix == 100.0f);
        CHECK(p.RearDelay == 5);
        CHECK(p.SideDelay == 5);
        CHECK(p.PositionLeft == 6);
        CHECK(p.PositionRight == 6);
        CHECK(p.PositionMatrixLeft == 27);
        CHECK(p.PositionMatrixRight == 27);
        CHECK(p.RoomSize == 100.0f);
        CHECK(p.LowEQCutoff == 4);
        CHECK(p.HighEQCutoff == 6);
        CHECK(p.LowEQGain == 8);
        CHECK(p.RoomFilterMain == doctest::Approx(-10.0f));
        CHECK(p.RoomFilterHF == doctest::Approx(expect[i].roomFilterHF));
        CHECK(p.HighEQGain == expect[i].highEQGain);
        CHECK(p.DecayTime == doctest::Approx(expect[i].decay));
        CHECK(p.ReflectionsDelay == expect[i].reflectionsDelay);
        CHECK(p.ReverbDelay == expect[i].reverbDelay);
        CHECK(p.ReflectionsGain == doctest::Approx(expect[i].reflectionsGain));
        CHECK(p.ReverbGain == doctest::Approx(expect[i].reverbGain));
        CHECK(p.EarlyDiffusion == 15);
        CHECK(p.LateDiffusion == 15);
        CHECK(p.Density == 100.0f);
        CHECK(p.RoomFilterFreq == 5000.0f);
        CHECK(p.DisableLateField == FALSE);
    }
    // 7.1 effect: rear delay 20 ms (the default of ReverbConvertI3DL2ToNative, as in m5_i3dl2.out).
    const XAUDIO2FX_REVERB_PARAMETERS p71 = ToNative(GameEnvironmentPreset(0)->listener, true);
    CHECK(p71.RearDelay == 20);
    CHECK(ValidNative(p71, true));
    CHECK_FALSE(ValidNative(p71, false));
}

TEST_CASE("reverb: out-of-range and NaN guest values are clamped to valid parameters") {
    snd::I3dl2Listener wild;
    wild.room = 5000;
    wild.roomHF = -20000;
    wild.decayTime = 0.0f;
    wild.decayHFRatio = 10.0f;
    wild.reflections = 9999;
    wild.reflectionsDelay = 5.0f;
    wild.reverb = -99999;
    wild.reverbDelay = std::numeric_limits<float>::quiet_NaN();
    wild.diffusion = -5.0f;
    wild.density = 1000.0f;
    wild.hfReference = 1.0f;
    const snd::I3dl2Listener c = ClampI3dl2(wild);
    CHECK(c.room == 0);
    CHECK(c.roomHF == -10000);
    CHECK(c.decayTime == doctest::Approx(0.1f));
    CHECK(c.decayHFRatio == doctest::Approx(2.0f));
    CHECK(c.reflections == 1000);
    CHECK(c.reflectionsDelay == doctest::Approx(0.3f));
    CHECK(c.reverb == -10000);
    CHECK(c.reverbDelay == doctest::Approx(0.011f));  // NaN -> Generic
    CHECK(c.diffusion == 0.0f);
    CHECK(c.density == 100.0f);
    CHECK(c.hfReference == 20.0f);
    CHECK(ValidNative(ToNative(wild)));
    CHECK(ValidNative(ToNative(wild), true));

    snd::I3dl2Listener silent;
    silent.room = -10000;
    CHECK(IsSilent(silent));
    CHECK_FALSE(IsSilent(GameEnvironmentPreset(0)->listener));
}

TEST_CASE("reverb: guest DSI3DL2LISTENER / DSI3DL2BUFFER reads (unaligned)") {
    std::uint8_t buf[1 + 48] = {};
    const GuestI3dl2Listener g = {-1000,  -6000, 0.0f,   0.17f,  0.1f,   -1204,
                                  0.001f, 207,   0.002f, 100.0f, 100.0f, 5000.0f};
    std::memcpy(buf + 1, &g, sizeof(g));
    snd::I3dl2Listener l;
    REQUIRE(ReadI3dl2Listener(buf + 1, l));
    CHECK(l.room == -1000);
    CHECK(l.roomHF == -6000);
    CHECK(l.decayTime == 0.17f);
    CHECK(l.reverb == 207);
    CHECK(l.hfReference == 5000.0f);
    CHECK_FALSE(ReadI3dl2Listener(nullptr, l));

    // The game's per-voice source (0xCC9DF..0xCCA0E): only lRoom = -600.
    const GuestI3dl2Buffer b = {0, 0, -600, 0, 0.0f, 0, 0.0f, 0, 0.0f};
    std::memcpy(buf + 1, &b, sizeof(b));
    snd::I3dl2Source s;
    s.direct = 77;
    REQUIRE(ReadI3dl2Source(buf + 1, s));
    CHECK(s.direct == 0);
    CHECK(s.room == -600);
    CHECK_FALSE(ReadI3dl2Source(nullptr, s));
}

TEST_CASE("reverb: submix layout and output matrices") {
    CHECK(ReverbEffectChannels(1) == 1);
    CHECK(ReverbEffectChannels(2) == 6);
    CHECK(ReverbEffectChannels(6) == 6);
    CHECK(ReverbEffectChannels(8) == 6);
    CHECK(ReverbSampleRate(48000) == 48000);
    CHECK(ReverbSampleRate(96000) == 48000);
    CHECK(ReverbSampleRate(11025) == 20000);
    CHECK(ReverbSampleRate(44100) == 44100);

    float m[6 * 8] = {};
    REQUIRE(ReverbOutputMatrix(6, 2, m, 12));
    // FL -> L, FR -> R, FC -> both at -3 dB, LFE dropped, BL -> L, BR -> R at -3 dB.
    CHECK(m[0 * 2 + 0] == 1.0f);
    CHECK(m[1 * 2 + 1] == 1.0f);
    CHECK(m[2 * 2 + 0] == doctest::Approx(0.70710678f));
    CHECK(m[2 * 2 + 1] == doctest::Approx(0.70710678f));
    CHECK(m[3 * 2 + 0] == 0.0f);
    CHECK(m[3 * 2 + 1] == 0.0f);
    CHECK(m[4 * 2 + 0] == doctest::Approx(0.70710678f));
    CHECK(m[4 * 2 + 1] == 0.0f);
    CHECK(m[5 * 2 + 1] == doctest::Approx(0.70710678f));
    REQUIRE(ReverbOutputMatrix(6, 6, m, 36));
    for (int s = 0; s < 6; ++s) {
        for (int d = 0; d < 6; ++d) {
            CHECK(m[s * 6 + d] == ((s == d && s != 3) ? 1.0f : 0.0f));
        }
    }
    REQUIRE(ReverbOutputMatrix(6, 8, m, 48));
    CHECK(m[4 * 8 + 4] == doctest::Approx(0.70710678f));
    CHECK(m[4 * 8 + 6] == doctest::Approx(0.70710678f));
    REQUIRE(ReverbOutputMatrix(1, 1, m, 1));
    CHECK(m[0] == 1.0f);
    REQUIRE(ReverbOutputMatrix(1, 2, m, 2));
    CHECK(m[0] == doctest::Approx(0.70710678f));
    CHECK_FALSE(ReverbOutputMatrix(6, 3, m, 18));
    CHECK_FALSE(ReverbOutputMatrix(2, 2, m, 4));
    CHECK_FALSE(ReverbOutputMatrix(6, 2, m, 11));
    CHECK_FALSE(ReverbOutputMatrix(6, 2, nullptr, 12));
}

TEST_CASE("reverb: DSP effects image parser (synthetic image)") {
    std::vector<std::uint8_t> img = SyntheticImage();
    EffectsImageInfo info;
    const char* error = "unset";
    REQUIRE(ParseEffectsImage(img.data(), img.size(), info, &error));
    CHECK(error == nullptr);
    CHECK(info.codeWords == 0x10);
    CHECK(info.stateWords == 0x8);
    CHECK(info.descriptorOffset == 0x878);
    CHECK(info.effectCount == 2);
    CHECK(info.totalScratchSize == 0x1000);
    CHECK(info.effects[0].scratchSize == 0x800);
    CHECK(info.effects[1].yMemorySize == 0x20);
    CHECK(ImageHasReverb(info, EffectsImageLoc{0, 1}));
    CHECK_FALSE(ImageHasReverb(info, EffectsImageLoc{1, 0}));  // no scratch: not a reverb
    CHECK_FALSE(ImageHasReverb(info, EffectsImageLoc{}));      // loc NULL (0xFFFFFFFF)
    CHECK_FALSE(ImageHasReverb(info, EffectsImageLoc{2, 1}));

    // Truncated image, broken counts and offsets.
    CHECK_FALSE(ParseEffectsImage(img.data(), img.size() - 1, info, &error));
    CHECK(error != nullptr);
    CHECK_FALSE(ParseEffectsImage(img.data(), 0x810, info, &error));
    CHECK_FALSE(ParseEffectsImage(nullptr, img.size(), info, &error));
    std::vector<std::uint8_t> bad = img;
    PutU32(bad, 0x878, 0);
    CHECK_FALSE(ParseEffectsImage(bad.data(), bad.size(), info, &error));
    bad = img;
    PutU32(bad, 0x880, 0x800);  // code before 0x818
    CHECK_FALSE(ParseEffectsImage(bad.data(), bad.size(), info, &error));
    bad = img;
    PutU32(bad, 0x888, 0x870);  // state reaches into the descriptor
    CHECK_FALSE(ParseEffectsImage(bad.data(), bad.size(), info, &error));
    bad = img;
    PutU32(bad, 0x89C, 0x1001);  // scratch larger than the total
    CHECK_FALSE(ParseEffectsImage(bad.data(), bad.size(), info, &error));
    bad = img;
    PutU32(bad, 0x804, 0x40000000);  // descriptor far outside
    CHECK_FALSE(ParseEffectsImage(bad.data(), bad.size(), info, &error));
}

TEST_CASE("reverb: the game's dsstdfx.bin (FUT_AUDIO_DSSTDFX, optional)") {
    const char* path = std::getenv("FUT_AUDIO_DSSTDFX");
    if (path == nullptr || !std::filesystem::is_regular_file(path)) {
        MESSAGE("skipped: FUT_AUDIO_DSSTDFX not set (a copy of XBOX\\dsstdfx.bin under run\\)");
        return;
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(img.size() == 25256);
    EffectsImageInfo info;
    const char* error = nullptr;
    REQUIRE(ParseEffectsImage(img.data(), img.size(), info, &error));
    CHECK(info.descriptorOffset == 0x6110);
    CHECK(info.effectCount == 10);
    CHECK(info.totalScratchSize == 0xC2000);
    CHECK(info.effects[0].scratchSize == 0x5CC60);       // delay lines of the I3DL2 reverb
    CHECK(ImageHasReverb(info, EffectsImageLoc{0, 1}));  // the game's loc (0xCBF67)
}

TEST_CASE("reverb: stand-alone XAudio2 reverb XAPO (software mix without a device)") {
    SoftwareReverb bad;
    CHECK_FALSE(bad.Initialize(48000, 2));  // the reverb only does mono -> mono / 5.1
    CHECK_FALSE(bad.Initialize(96000, 6));
    CHECK_FALSE(bad.Ready());

    SoftwareReverb r;
    REQUIRE(r.Initialize(48000, 6));
    CHECK(r.Ready());
    CHECK(r.OutputChannels() == 6);
    CHECK(r.SampleRate() == 48000);
    CHECK(r.BlockFrames() == 480);

    const std::vector<float> concert = ImpulseResponse(r, GameEnvironmentPreset(4)->listener);
    const std::vector<float> padded = ImpulseResponse(r, GameEnvironmentPreset(0)->listener);
    for (float v : concert) {
        REQUIRE(std::isfinite(v));
    }
    const double concertEarly = Energy(concert, 0, 4800 * 6);
    const double concertTail = Energy(concert, 24000 * 6, 48000 * 6);
    const double paddedEarly = Energy(padded, 0, 4800 * 6);
    const double paddedTail = Energy(padded, 24000 * 6, 48000 * 6);
    CAPTURE(concertEarly);
    CAPTURE(concertTail);
    CAPTURE(paddedEarly);
    CAPTURE(paddedTail);
    CHECK(concertEarly > 1e-6);
    CHECK(paddedEarly > 1e-6);
    CHECK(concertTail > 1e-6);               // decay 3.92 s: still ringing after 0.5 s
    CHECK(paddedTail < concertTail * 1e-3);  // decay 0.17 s: gone
    // Silence in, silence out after the tail; nullptr input = silence.
    std::vector<float> out(480 * 6, 1.0f);
    for (int i = 0; i < 200; ++i) {
        r.Process(nullptr, out.data(), 480);
    }
    CHECK(Energy(out, 0, out.size()) < 1e-12);
}
