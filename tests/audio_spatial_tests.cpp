// Unit tests of the M5 3D module (audio/spatial_*.h): the DSOUND 3D level model (rolloff curve, mixbin
// register, reverb send, LFE), the listener geometry with the game's swapped front vector, X3DAudio direction
// and the Spatializer result. Synthetic data only. Reference values:
// analysis/loader/scripts/out/m5_gain_model.out, recomputed with DSOUND's truncation (cvttss2si at 0x24E2B4)
// in analysis/loader/scripts/m5_codec_golden.py.
#include "doctest.h"

#include "audio/audio_api.h"
#include "audio/codec_format.h"
#include "audio/spatial_model.h"
#include "audio/spatial_x3d.h"
#include "core/config_schema.h"

#include <x3daudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace {

using snd::Emitter3D;
using snd::Listener3D;
using snd::SpatialResult;
using snd::Vec3;
using namespace snd::spatial;

// The game's 3D voice (m5_audio.md 2.2): flags 0x60110, curve {1, 0.9, 0.8, 0.5, 0.25, 0} (0x2EAD3C),
// I3DL2 source lRoom -600, min/max as given.
Emitter3D GameEmitter(float minD, float maxD, std::int32_t volume = 0) {
    Emitter3D e;
    e.bufferFlags = 0x60110;
    e.minDistance = minD;
    e.maxDistance = maxD;
    e.rolloffCount = 6;
    const float curve[6] = {1.0f, 0.9f, 0.8f, 0.5f, 0.25f, 0.0f};
    std::memcpy(e.rolloff, curve, sizeof(curve));
    e.i3dl2.room = -600;
    e.volumeMillibel = volume;
    e.channels = 1;
    return e;
}

// The game's listener set-up (0xCCD73..0xCCDA7): doppler 2, distance factor 0.2, rolloff 1.
Listener3D GameListener() {
    Listener3D l;
    l.dopplerFactor = 2.0f;
    l.distanceFactor = 0.2f;
    l.rolloffFactor = 1.0f;
    return l;
}

float Db(float gain) {
    return 20.0f * std::log10(gain);
}

float Sum(const float* v, int n) {
    float s = 0;
    for (int i = 0; i < n; ++i) {
        s += v[i];
    }
    return s;
}

}  // namespace

TEST_CASE("spatial: DSOUND rolloff curve (FUN_00249893) for min/max 5/80 and 30/40") {
    struct Point {
        float d;
        std::int32_t mb;
    };
    // m5_gain_model.out rounds (2000*log10 0.9 = -91.5 -> -92); DSOUND truncates (-91).
    const Point p580[] = {{0.0f, 0},       {5.0f, 0},       {11.25f, 0},     {17.5f, 0},
                          {30.0f, -91},    {42.5f, -193},   {55.0f, -602},   {67.5f, -1204},
                          {79.99f, -7397}, {80.0f, -10000}, {81.0f, -10000}, {32.75f, -113}};
    const Emitter3D e1 = GameEmitter(5, 80);
    for (const Point& p : p580) {
        CAPTURE(p.d);
        CHECK(DistanceMillibel(p.d, e1, 1.0f) == p.mb);
    }
    const Point p3040[] = {{0.0f, 0},       {30.0f, 0},      {35.0f, -193},   {38.333332f, -1204},
                           {39.99f, -5647}, {40.0f, -10000}, {41.0f, -10000}, {33.7f, -113}};
    const Emitter3D e2 = GameEmitter(30, 40);
    for (const Point& p : p3040) {
        CAPTURE(p.d);
        CHECK(DistanceMillibel(p.d, e2, 1.0f) == p.mb);
    }
    // Without DSBCAPS_MUTE3DATMAXDISTANCE the distance is clamped to Max (curve end 0 -> -10000 at Max
    // anyway).
    Emitter3D noMute = e1;
    noMute.bufferFlags = 0x10;
    CHECK(DistanceMillibel(200.0f, noMute, 1.0f) == -10000);
    noMute.rolloff[5] = 0.1f;
    CHECK(DistanceMillibel(200.0f, noMute, 1.0f) == GainToMillibel(0.1f));
    // Max <= Min: no attenuation without the mute flag, mute with it.
    Emitter3D flat = GameEmitter(30, 30);
    flat.bufferFlags = 0x10;
    CHECK(DistanceMillibel(35.0f, flat, 1.0f) == 0);
    CHECK(DistanceMillibel(35.0f, GameEmitter(30, 30), 1.0f) == -10000);
}

TEST_CASE("spatial: DS3D default rolloff without a curve and gain -> mB truncation") {
    Emitter3D e = GameEmitter(5, 80);
    e.rolloffCount = 0;
    e.bufferFlags = 0x10;
    // 1 / (1 + rolloff * (d / min - 1)): d = 10 -> 0.5 -> -602 mB; rolloff 2 at d = 10 -> 1/3 -> -954 mB.
    CHECK(DistanceMillibel(10.0f, e, 1.0f) == -602);
    CHECK(DistanceMillibel(10.0f, e, 2.0f) == -954);
    CHECK(DistanceMillibel(4.0f, e, 1.0f) == 0);
    CHECK(DistanceMillibel(1000.0f, e, 1.0f) == DistanceMillibel(80.0f, e, 1.0f));  // clamped to Max
    CHECK(DistanceGainExact(10.0f, e, 1.0f) == doctest::Approx(0.5f));

    CHECK(GainToMillibel(1.0f) == 0);
    CHECK(GainToMillibel(2.0f) == 0);
    CHECK(GainToMillibel(0.0f) == -10000);
    CHECK(GainToMillibel(-1.0f) == -10000);
    CHECK(GainToMillibel(0.5f) == -602);
    CHECK(GainToMillibel(0.9f) == -91);
    CHECK(DefaultRolloffMillibel(-1.0f) == 0);
    CHECK(DefaultRolloffMillibel(1.0f) == -602);
}

TEST_CASE("spatial: mixbin register gain (FUN_0024C3CA) and the 3D levels of m5_gain_model.out") {
    // 2D at the default slider 0.75: lVolume -250, headroom 600, mixbin -250 -> -11.00 dB.
    CHECK(Db(HwMixbinGain(-250 - 600 - 250)) == doctest::Approx(-11.0f).epsilon(1e-4));
    CHECK(Db(HwMixbinGain(-50 - 600 - 50)) == doctest::Approx(-7.0f).epsilon(1e-4));
    // 1/64-dB steps: -899 - 600 - 899 = -2398 mB -> (2398*64)/100 = 1534 -> -23.97 dB.
    CHECK(Db(HwMixbinGain(-2398)) == doctest::Approx(-1534.0f / 64.0f).epsilon(1e-5));
    // -10000 mB: register 0xFFF = -63.98 dB, not silence.
    CHECK(Db(HwMixbinGain(-10000)) == doctest::Approx(-4095.0f / 64.0f).epsilon(1e-5));
    CHECK(Db(HwMixbinGain(-30000)) == doctest::Approx(-4095.0f / 64.0f).epsilon(1e-5));
    CHECK(HwMixbinGain(0) == 1.0f);
    CHECK(HwMixbinGain(500) == 1.0f);

    // 3D voice at v = 0.75 (mB -250) inside MinDistance: direct -2.5 dB, reverb send -8.5 dB, LFE -2.5 dB.
    const Listener3D l = GameListener();
    Emitter3D e = GameEmitter(5, 80, -250);
    e.position = {0, 0, 3};
    Levels3D lv = ComputeLevels3D(l, e);
    CHECK(lv.distance == 0);
    CHECK(lv.direct == -250);
    CHECK(lv.reverb == -850);
    CHECK(lv.lfe == -250);
    CHECK(Db(HwMixbinGain(lv.reverb)) == doctest::Approx(-8.5f).epsilon(1e-4));
    // At distance 55 (gain 0.5, -602 mB): every path drops by the distance term.
    e.position = {0, 55, 0};
    lv = ComputeLevels3D(l, e);
    CHECK(lv.distance == -602);
    CHECK(lv.direct == -852);
    CHECK(lv.reverb == -1452);
    CHECK(lv.lfe == -852);
    // A positive lDirect cannot lift the direct path above the voice volume (3D terms clamped to <= 0).
    e.position = {0, 0, 1};
    e.i3dl2.direct = 500;
    CHECK(ComputeLevels3D(l, e).direct == -250);
}

TEST_CASE("spatial: the X3DAudio form of the rolloff curve equals the DSOUND curve") {
    for (const auto& range : {std::pair<float, float>{5.0f, 80.0f}, std::pair<float, float>{30.0f, 40.0f}}) {
        const Emitter3D game = GameEmitter(range.first, range.second);
        CurvePoint pts[kMaxX3dCurvePoints];
        const std::uint32_t n = BuildX3dDistanceCurve(game, pts);
        REQUIRE(n == 8);
        CHECK(pts[0].distance == 0.0f);
        CHECK(pts[1].distance == doctest::Approx(range.first / range.second));
        CHECK(pts[7].distance == doctest::Approx(1.0f));
        CHECK(pts[7].gain == 0.0f);

        X3DAUDIO_HANDLE h;
        REQUIRE(SUCCEEDED(X3DAudioInitialize(SPEAKER_MONO, X3DAUDIO_SPEED_OF_SOUND, h)));
        X3DAUDIO_DISTANCE_CURVE_POINT xp[kMaxX3dCurvePoints];
        for (std::uint32_t i = 0; i < n; ++i) {
            xp[i] = {pts[i].distance, pts[i].gain};
        }
        X3DAUDIO_DISTANCE_CURVE curve = {xp, n};
        for (float d = 0.5f; d < range.second * 1.2f; d += range.second / 37.0f) {
            X3DAUDIO_LISTENER l = {};
            l.OrientFront = {0, 0, 1};
            l.OrientTop = {0, 1, 0};
            X3DAUDIO_EMITTER e = {};
            e.OrientFront = {0, 0, 1};
            e.OrientTop = {0, 1, 0};
            e.Position = {0, 0, d};
            e.ChannelCount = 1;
            e.pVolumeCurve = &curve;
            e.CurveDistanceScaler = range.second;
            float coefficient = 0;
            X3DAUDIO_DSP_SETTINGS dsp = {};
            dsp.pMatrixCoefficients = &coefficient;
            dsp.SrcChannelCount = 1;
            dsp.DstChannelCount = 1;
            X3DAudioCalculate(h, &l, &e, X3DAUDIO_CALCULATE_MATRIX, &dsp);
            CAPTURE(d);
            CHECK(coefficient == doctest::Approx(DistanceGainExact(d, game, 1.0f)).epsilon(1e-3));
        }
    }
    Emitter3D none = GameEmitter(5, 80);
    none.rolloffCount = 0;
    CurvePoint pts[kMaxX3dCurvePoints];
    CHECK(BuildX3dDistanceCurve(none, pts) == 0);
}

TEST_CASE("spatial: listener basis follows DSOUND (right = top x front) and degenerates gracefully") {
    Listener3D l;
    l.front = {0, 0, 1};
    l.top = {0, 1, 0};
    ListenerBasis b = MakeListenerBasis(l);
    CHECK(b.right.x == doctest::Approx(1.0f));
    CHECK(b.right.y == doctest::Approx(0.0f));
    CHECK(b.right.z == doctest::Approx(0.0f));
    // Not orthogonal, not normalized: top is orthogonalized against front.
    l.front = {0, 0, 2};
    l.top = {0, 3, 1};
    b = MakeListenerBasis(l);
    CHECK(b.up.y == doctest::Approx(1.0f));
    CHECK(b.up.z == doctest::Approx(0.0f));
    CHECK(b.front.z == doctest::Approx(1.0f));
    // Degenerate: zero front, top parallel to front.
    l.front = {0, 0, 0};
    l.top = {0, 0, 0};
    b = MakeListenerBasis(l);
    CHECK(b.front.z == doctest::Approx(1.0f));
    CHECK(b.up.y == doctest::Approx(1.0f));
    l.front = {0, 1, 0};
    l.top = {0, 2, 0};
    b = MakeListenerBasis(l);
    CHECK(Dot(b.up, b.front) == doctest::Approx(0.0f));
    CHECK(Length(b.up) == doctest::Approx(1.0f));
    CHECK(Length(b.right) == doctest::Approx(1.0f));

    PositionalMode mode = PositionalMode::Off;
    CHECK(ParsePositionalMode("xbox", mode));
    CHECK(mode == PositionalMode::Xbox);
    CHECK(ParsePositionalMode(" Corrected ", mode));
    CHECK(mode == PositionalMode::Corrected);
    CHECK(ParsePositionalMode("OFF", mode));
    CHECK(mode == PositionalMode::Off);
    CHECK_FALSE(ParsePositionalMode("on", mode));
    CHECK_FALSE(ParsePositionalMode("xbox x", mode));
    CHECK_FALSE(ParsePositionalMode(nullptr, mode));
    CHECK(std::strcmp(PositionalModeName(PositionalMode::Corrected), "corrected") == 0);
}

TEST_CASE("spatial: the game's camera vectors - left/right correct in both modes, front/back per mode") {
    // NetImmerse camera with view V = +x and up U = +y (the trace's identity camera); camera right = V x U =
    // +z. The game passes front = -V, top = U (0xCBE85).
    Listener3D l = GameListener();
    l.front = {-1, 0, 0};
    l.top = {0, 1, 0};
    const ListenerBasis b = MakeListenerBasis(l);
    const Vec3 inFront{10, 0, 0};   // 10 units along the view direction
    const Vec3 camRight{0, 0, 10};  // camera right
    for (PositionalMode m : {PositionalMode::Xbox, PositionalMode::Corrected}) {
        const Vec3 p = ToListenerSpace(camRight, l, b, m);
        CHECK(p.x == doctest::Approx(10.0f));  // right stays right
        CHECK(p.z == doctest::Approx(0.0f));
    }
    CHECK(ToListenerSpace(inFront, l, b, PositionalMode::Xbox).z == doctest::Approx(-10.0f));      // "behind"
    CHECK(ToListenerSpace(inFront, l, b, PositionalMode::Corrected).z == doctest::Approx(10.0f));  // in front
    const Vec3 off = ToListenerSpace(camRight, l, b, PositionalMode::Off);
    CHECK(off.x == 0.0f);
    CHECK(off.z == doctest::Approx(10.0f));

    // Through the Spatializer on 5.1 (FL FR FC LFE BL BR): a source in front of the camera.
    Emitter3D e = GameEmitter(5, 80, 0);
    e.position = inFront;
    SpatialResult r;
    auto corrected = CreateSpatializer(SpatialSettings{PositionalMode::Corrected, 0.0f});
    auto xbox = CreateSpatializer(SpatialSettings{PositionalMode::Xbox, 0.0f});
    REQUIRE(corrected);
    REQUIRE(xbox);
    corrected->Compute(l, e, 6, SPEAKER_5POINT1, r);
    CHECK(r.matrix[0] + r.matrix[1] + r.matrix[2] > 4.0f * (r.matrix[4] + r.matrix[5]));
    xbox->Compute(l, e, 6, SPEAKER_5POINT1, r);
    CHECK(r.matrix[4] + r.matrix[5] > 4.0f * (r.matrix[0] + r.matrix[1] + r.matrix[2]));
    // Camera right on stereo: right channel louder, in both modes.
    e.position = camRight;
    corrected->Compute(l, e, 2, SPEAKER_STEREO, r);
    CHECK(r.matrix[1] > 4.0f * r.matrix[0]);
    xbox->Compute(l, e, 2, SPEAKER_STEREO, r);
    CHECK(r.matrix[1] > 4.0f * r.matrix[0]);
}

TEST_CASE("spatial: Spatializer levels - front normalization, LFE, reverb send, distance, trim, off mode") {
    Listener3D l = GameListener();  // front (0,0,1), top (0,1,0)
    Emitter3D e = GameEmitter(5, 80, -250);
    e.position = {0, 0, 4};  // straight ahead, inside MinDistance (and outside the 2.5-unit inner radius)
    const float direct = std::pow(10.0f, -2.5f / 20.0f);
    SpatialResult r;
    auto s = CreateSpatializer(SpatialSettings{PositionalMode::Xbox, 0.0f});
    REQUIRE(s);
    // Stereo: a source straight ahead plays like a centered voice at the direct level on both channels.
    s->Compute(l, e, 2, SPEAKER_STEREO, r);
    CHECK(r.matrix[0] == doctest::Approx(direct).epsilon(2e-3));
    CHECK(r.matrix[1] == doctest::Approx(direct).epsilon(2e-3));
    CHECK(r.reverbSend == doctest::Approx(std::pow(10.0f, -8.5f / 20.0f)).epsilon(1e-4));
    CHECK(r.lfe == doctest::Approx(direct).epsilon(1e-4));  // reported, but no LFE channel in stereo
    CHECK(r.distanceGain == 1.0f);
    // Hard right (inside MinDistance, outside the inner radius): the near channel at the direct level - constant
    // power would give +3 dB there, above the 0 dB ceiling of an Xbox mixbin (review finding M5-3).
    e.position = {4.9f, 0, 0};
    s->Compute(l, e, 2, SPEAKER_STEREO, r);
    CHECK(r.matrix[0] == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(r.matrix[1] == doctest::Approx(direct).epsilon(2e-3));
    // Stereo: the near channel is at the direct level in every direction, no channel above it; the total power is
    // at most the front's (2 * direct^2).
    for (float az = 0.0f; az < 6.28f; az += 0.3927f) {
        e.position = {4.9f * std::sin(az), 0.0f, 4.9f * std::cos(az)};
        s->Compute(l, e, 2, SPEAKER_STEREO, r);
        CAPTURE(az);
        CHECK(std::max(r.matrix[0], r.matrix[1]) == doctest::Approx(direct).epsilon(2e-3));
        CHECK(r.matrix[0] * r.matrix[0] + r.matrix[1] * r.matrix[1] <= 2.0f * direct * direct * 1.004f);
        CHECK((az < 3.1f ? r.matrix[1] >= r.matrix[0] - 1e-4f : r.matrix[0] >= r.matrix[1] - 1e-4f));
    }
    // 5.1: never on the centre (the game's 3D mixbins have no centre bin, 0xCC35A), no channel above the direct
    // level; straight ahead = FL/FR at the direct level.
    // Both 5.1 masks: back (0x3F) and side surround (0x60F, e.g. an HDMI TV).
    for (const std::uint32_t mask : {static_cast<std::uint32_t>(SPEAKER_5POINT1),
                                     static_cast<std::uint32_t>(SPEAKER_5POINT1_SURROUND)}) {
        CAPTURE(mask);
        for (float az = 0.0f; az < 6.28f; az += 0.3927f) {
            e.position = {4.9f * std::sin(az), 0.0f, 4.9f * std::cos(az)};
            s->Compute(l, e, 6, mask, r);
            CAPTURE(az);
            CHECK(r.matrix[2] == 0.0f);
            float loudest = 0.0f;
            for (int d : {0, 1, 4, 5}) {
                loudest = std::max(loudest, r.matrix[d]);
            }
            CHECK(loudest <= direct * 1.002f);
            CHECK(loudest > 0.5f * direct);
            // Left/right follow the azimuth (no channel stuck on one side).
            CHECK((az > 0.2f && az < 2.9f ? r.matrix[1] + r.matrix[5] > r.matrix[0] + r.matrix[4] : true));
            CHECK((az > 3.4f && az < 6.1f ? r.matrix[0] + r.matrix[4] > r.matrix[1] + r.matrix[5] : true));
        }
        e.position = {0, 0, 4};
        s->Compute(l, e, 6, mask, r);
        CHECK(r.matrix[0] == doctest::Approx(direct).epsilon(2e-3));
        CHECK(r.matrix[1] == doctest::Approx(direct).epsilon(2e-3));
        CHECK(r.matrix[4] + r.matrix[5] < 0.1f * direct);
    }
    e.position = {0, 0, 4};
    s->Compute(l, e, 6, SPEAKER_5POINT1, r);
    // Distance 55: gain 0.5 (-602 mB) on direct, LFE and reverb; distanceGain for the event log.
    e.position = {0, 0, 55};
    s->Compute(l, e, 6, SPEAKER_5POINT1, r);
    const float att = std::pow(10.0f, -(852 * 64 / 100) / 64.0f / 20.0f);  // register 545 = -8.52 dB
    CHECK(r.distanceGain == doctest::Approx(std::pow(10.0f, -602 / 2000.0f)).epsilon(1e-5));
    CHECK(r.matrix[2] == 0.0f);  // front: on FL/FR, never on FC
    CHECK(r.matrix[0] == doctest::Approx(att).epsilon(2e-3));
    CHECK(r.matrix[1] == doctest::Approx(att).epsilon(2e-3));
    CHECK(r.matrix[3] == doctest::Approx(att).epsilon(1e-4));                     // LFE (bin 3)
    CHECK(r.reverbSend == doctest::Approx(std::pow(10.0f, -(1452 * 64 / 100) / 64.0f / 20.0f)).epsilon(1e-4));
    // Beyond MaxDistance: muted to the register floor (-63.98 dB below the voice volume).
    e.position = {0, 0, 81};
    s->Compute(l, e, 2, SPEAKER_STEREO, r);
    CHECK(r.distanceGain == 0.0f);
    CHECK(Db(r.matrix[0]) < -60.0f);
    CHECK(Db(r.reverbSend) < -60.0f);

    // trim_3d +6 dB scales every path.
    e.position = {0, 0, 4};
    auto trimmed = CreateSpatializer(SpatialSettings{PositionalMode::Xbox, 6.0f});
    trimmed->Compute(l, e, 2, SPEAKER_STEREO, r);
    CHECK(r.matrix[0] == doctest::Approx(direct * std::pow(10.0f, 0.3f)).epsilon(2e-3));
    CHECK(r.reverbSend ==
          doctest::Approx(std::pow(10.0f, -8.5f / 20.0f) * std::pow(10.0f, 0.3f)).epsilon(1e-3));

    // positional = off: every position sounds straight ahead (levels still follow the distance).
    auto off = CreateSpatializer(SpatialSettings{PositionalMode::Off, 0.0f});
    e.position = {-30, 0, -5};
    off->Compute(l, e, 2, SPEAKER_STEREO, r);
    CHECK(r.matrix[0] == doctest::Approx(r.matrix[1]));
    CHECK(r.matrix[0] ==
          doctest::Approx(direct * HwMixbinGain(DistanceMillibel(Length(e.position), e, 1.0f)) /
                          HwMixbinGain(0))
              .epsilon(5e-3));

    // Layout without a mask: the default mask of the channel count; 4 channels (quad) have no LFE.
    e.position = {0, 0, 4};
    s->Compute(l, e, 6, 0, r);
    CHECK(r.matrix[3] == doctest::Approx(direct).epsilon(1e-4));
    s->Compute(l, e, 4, 0, r);
    for (int d = 0; d < 4; ++d) {
        CHECK(r.matrix[d] >= 0.0f);
    }
    CHECK(Sum(r.matrix, 4) > 0.5f);

    // A stereo emitter (never used by the game) duplicates the matrix per channel at 1/sqrt(2).
    Emitter3D st = e;
    st.channels = 2;
    s->Compute(l, st, 2, SPEAKER_STEREO, r);
    CHECK(r.matrix[0] == doctest::Approx(direct / std::sqrt(2.0f)).epsilon(2e-3));
    CHECK(r.matrix[2] == doctest::Approx(r.matrix[0]));
    CHECK(r.matrix[3] == doctest::Approx(r.matrix[1]));
}

TEST_CASE("spatial: doppler pitch (DS3D, never audible in this game)") {
    Listener3D l = GameListener();
    Emitter3D e = GameEmitter(5, 80);
    e.position = {0, 0, 10};
    CHECK(DopplerPitch(l, e) == 0);  // the game's velocities are always 0 (0x2F90BC)
    // Source approaching at 10% of the speed of sound in world units (343.5 / 0.2 = 1717.5 units/s), factor
    // 2: ratio = c / (c - 2 * 171.75) = 1.25 -> 4096 * log2(1.25) = 1319.
    e.velocity = {0, 0, -171.75f};
    CHECK(DopplerPitch(l, e) == snd::codec::PitchFromRatio(1.25));
    CHECK(DopplerPitch(l, e) == 1319);
    e.velocity = {0, 0, 171.75f};  // receding
    CHECK(DopplerPitch(l, e) < 0);
    l.dopplerFactor = 0.0f;
    CHECK(DopplerPitch(l, e) == 0);
    // Supersonic values are clamped below the speed of sound (finite result).
    l.dopplerFactor = 2.0f;
    e.velocity = {0, 0, -1.0e9f};
    CHECK(DopplerPitch(l, e) <= snd::codec::kXboxPitchMax);
}

TEST_CASE("spatial: DefaultSpeakerMask") {
    CHECK(DefaultSpeakerMask(1) == SPEAKER_MONO);
    CHECK(DefaultSpeakerMask(2) == SPEAKER_STEREO);
    CHECK(DefaultSpeakerMask(6) == SPEAKER_5POINT1);
    CHECK(DefaultSpeakerMask(8) == SPEAKER_7POINT1_SURROUND);
    CHECK(DefaultSpeakerMask(3) == 0);
}

TEST_CASE("spatial: [audio] positional / trim_3d schema rows agree with the Spatializer") {
    const core::ConfigKeyDef* positional = core::FindConfigKey("audio", "positional");
    REQUIRE(positional != nullptr);
    CHECK(std::string(positional->defaultValue) == "corrected");
    CHECK(SpatialSettings{}.positional == PositionalMode::Corrected);
    for (const core::ConfigChoice& c : positional->choices) {
        PositionalMode m = PositionalMode::Off;
        CAPTURE(c.value);
        CHECK(ParsePositionalMode(c.value, m));
        CHECK(std::string(PositionalModeName(m)) == c.value);
    }
    const core::ConfigKeyDef* trim = core::FindConfigKey("audio", "trim_3d");
    REQUIRE(trim != nullptr);
    double db = 1.0;
    REQUIRE(core::ParseConfigFloat(trim->defaultValue, db));
    CHECK(db == static_cast<double>(SpatialSettings{}.trim3dDb));
    // The configuration of the test process has no [audio] keys: a valid mode and trim.
    const SpatialSettings fromConfig = SpatialSettingsFromConfig();
    CHECK((fromConfig.positional == PositionalMode::Corrected ||
           fromConfig.positional == PositionalMode::Xbox || fromConfig.positional == PositionalMode::Off));
    CHECK(fromConfig.trim3dDb >= -40.0f);
    CHECK(fromConfig.trim3dDb <= 20.0f);
    // speakers: the words the engine parses (audio_engine.cpp ParseSpeakers: auto, stereo, 2.0, 5.1).
    const core::ConfigKeyDef* speakers = core::FindConfigKey("audio", "speakers");
    REQUIRE(speakers != nullptr);
    CHECK(std::string(core::ConfigChoiceList(*speakers)) == "auto|stereo|5.1|2.0");
    const core::ConfigKeyDef* reverb = core::FindConfigKey("audio", "reverb");
    REQUIRE(reverb != nullptr);
    CHECK(std::string(reverb->defaultValue) == "1");
}
