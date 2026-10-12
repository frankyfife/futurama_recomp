// M6 unit tests: XMV demuxer on synthetic packets, movie core, I420 -> YUY2 packing, YUY2 -> BGRA conversion (texconv),
// the movie clock (A/V timing by pts), [game] skip_videos parsing and the skip decision, the D3D shim content
// generation, and - when the samples cut from the disc exist (analysis/loader/samples, git-ignored) - a real decode.
#include "doctest.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "gfx/d3d_shim.h"
#include "gfx/texconv.h"
#include "video/movie_clock.h"
#include "video/movie_core.h"
#include "video/movie_skip.h"
#include "video/wmv_decoder.h"
#include "video/xmv_demux.h"
#include "video/xmv_test_mode.h"
#include "video/yuv_pack.h"

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void Put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) {
        v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
    }
}
void Put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}

struct SynthFrame {
    std::uint32_t delta;
    std::vector<std::uint8_t> bytes;  // as stored (little-endian words), multiple of 4
};

// Packet body: next size, video header, unknown, audio headers, [extradata], frames, audio.
std::vector<std::uint8_t> Body(std::uint32_t next, const std::vector<SynthFrame>& frames, bool extradata,
                               std::uint32_t extradataValue, const std::vector<std::vector<std::uint8_t>>& audio) {
    std::vector<std::uint8_t> video;
    if (extradata) {
        Put32(video, extradataValue);
    }
    for (const SynthFrame& f : frames) {
        Put32(video, (static_cast<std::uint32_t>(f.bytes.size()) / 4u - 1u) | (f.delta << 17));
        video.insert(video.end(), f.bytes.begin(), f.bytes.end());
    }
    const std::uint32_t tracks = static_cast<std::uint32_t>(audio.size());
    std::vector<std::uint8_t> b;
    Put32(b, next);
    // Video size field = data + 4 per track (the demuxer subtracts them again, FFmpeg's rule).
    Put32(b, (static_cast<std::uint32_t>(video.size()) + 4u * tracks) |
                 (static_cast<std::uint32_t>(frames.size()) << 23) | (extradata ? 0x80000000u : 0u));
    Put32(b, 0);
    for (const auto& a : audio) {
        Put32(b, static_cast<std::uint32_t>(a.size()));
    }
    b.insert(b.end(), video.begin(), video.end());
    for (const auto& a : audio) {
        b.insert(b.end(), a.begin(), a.end());
    }
    return b;
}

std::vector<std::uint8_t> Header(std::uint32_t next, std::uint32_t firstSize, std::uint16_t tracks) {
    std::vector<std::uint8_t> h;
    Put32(h, next);
    Put32(h, firstSize);
    Put32(h, 0x8000);
    Put32(h, vid::xmv::kMagic);
    Put32(h, 2);
    Put32(h, 16);
    Put32(h, 8);
    Put32(h, 1234);
    Put16(h, tracks);
    Put16(h, 0);
    for (std::uint16_t t = 0; t < tracks; ++t) {
        Put16(h, 0x69);
        Put16(h, 2);
        Put32(h, 22050);
        Put16(h, 4);
        Put16(h, 0);
    }
    return h;
}

// A two-packet movie: packet 1 = header + extradata 0x75 + two frames (deltas 0, 66) + 2 ADPCM blocks per track,
// packet 2 = one frame (delta 67) + 1 block per track, next size 0.
std::vector<std::uint8_t> SynthMovie(std::uint16_t tracks = 2) {
    const std::vector<std::uint8_t> block(72, 0x11);  // one stereo Xbox ADPCM block
    std::vector<std::vector<std::uint8_t>> audio1(tracks), audio2(tracks);
    for (std::uint16_t t = 0; t < tracks; ++t) {
        audio1[t].insert(audio1[t].end(), block.begin(), block.end());
        audio1[t].insert(audio1[t].end(), block.begin(), block.end());
        audio1[t][0] = static_cast<std::uint8_t>(t);
        audio2[t] = block;
    }
    const std::vector<SynthFrame> frames1 = {{0, {0x04, 0x03, 0x02, 0x00, 0xAA, 0xBB, 0xCC, 0xDD}},
                                             {66, {0x01, 0x02, 0x03, 0x80}}};
    const std::vector<SynthFrame> frames2 = {{67, {0x05, 0x06, 0x07, 0x40}}};
    const std::vector<std::uint8_t> body2 = Body(0, frames2, false, 0, audio2);
    const std::uint32_t headerBytes = 0x24u + 12u * tracks;
    const std::vector<std::uint8_t> body1Probe = Body(0, frames1, true, 0x75, audio1);
    const std::uint32_t first = headerBytes + static_cast<std::uint32_t>(body1Probe.size());
    std::vector<std::uint8_t> file = Header(static_cast<std::uint32_t>(body2.size()), first, tracks);
    const std::vector<std::uint8_t> body1 = Body(static_cast<std::uint32_t>(body2.size()), frames1, true, 0x75, audio1);
    file.insert(file.end(), body1.begin(), body1.end());
    file.insert(file.end(), body2.begin(), body2.end());
    return file;
}

}  // namespace

TEST_CASE("XMV demux: header, extradata, frames with accumulated pts, per-track audio, end of chain") {
    const std::vector<std::uint8_t> file = SynthMovie();
    vid::xmv::FileHeader h;
    std::string error;
    REQUIRE(vid::xmv::ParseFileHeader(file, h, error));
    CHECK(h.width == 16);
    CHECK(h.height == 8);
    CHECK(h.durationMs == 1234);
    REQUIRE(h.tracks.size() == 2);
    CHECK(h.tracks[1].sampleRate == 22050);
    CHECK(h.tracks[1].BlockAlign() == 72);

    vid::xmv::Demuxer d;
    vid::xmv::PacketOutput out;
    REQUIRE(d.PushPacket(std::span<const std::uint8_t>(file.data(), h.firstPacketSize), out));
    REQUIRE(d.HasExtradata());
    const std::array<std::uint8_t, 4> wmv2 = {0x00, 0x00, 0xAC, 0x80};  // FFmpeg's conversion of 0x75
    CHECK(d.Extradata() == wmv2);
    REQUIRE(out.frames.size() == 2);
    CHECK(out.frames[0].ptsMs == 0);
    CHECK(out.frames[1].ptsMs == 66);
    // Words are byte-swapped to the big-endian WMV2 order; picture type bit 0 = I-frame.
    const std::vector<std::uint8_t> f0 = {0x00, 0x02, 0x03, 0x04, 0xDD, 0xCC, 0xBB, 0xAA};
    CHECK(out.frames[0].data == f0);
    CHECK(out.frames[0].keyframe);
    CHECK_FALSE(out.frames[1].keyframe);
    REQUIRE(out.audio.size() == 2);
    CHECK(out.audio[0].track == 0);
    CHECK(out.audio[1].track == 1);
    CHECK(out.audio[1].data.size() == 144);
    CHECK(out.audio[1].data[0] == 1);
    CHECK(out.audio[0].firstFrame == 0);
    CHECK_FALSE(d.AtEnd());

    const std::size_t rest = file.size() - h.firstPacketSize;
    CHECK(d.NextPacketSize() == rest);
    REQUIRE(d.PushPacket(std::span<const std::uint8_t>(file.data() + h.firstPacketSize, rest), out));
    REQUIRE(out.frames.size() == 1);
    CHECK(out.frames[0].ptsMs == 133);
    CHECK(out.frames[0].index == 2);
    REQUIRE(out.audio.size() == 2);
    CHECK(out.audio[0].firstFrame == 128);  // 2 blocks of 64 frames before
    CHECK(d.AtEnd());
    CHECK_FALSE(d.PushPacket(std::span<const std::uint8_t>(file.data(), 16), out));  // nothing after the end
    CHECK(d.Failed());
}

TEST_CASE("XMV demux: malformed input is rejected") {
    std::vector<std::uint8_t> file = SynthMovie(1);
    vid::xmv::FileHeader h;
    std::string error;
    std::vector<std::uint8_t> bad = file;
    bad[0x0C] = 0;  // magic
    CHECK_FALSE(vid::xmv::ParseFileHeader(bad, h, error));
    CHECK(error.find("xobX") != std::string::npos);
    CHECK_FALSE(vid::xmv::ParseFileHeader(std::span<const std::uint8_t>(file.data(), 0x20), h, error));

    REQUIRE(vid::xmv::ParseFileHeader(file, h, error));
    vid::xmv::Demuxer d;
    vid::xmv::PacketOutput out;
    CHECK_FALSE(d.PushPacket(std::span<const std::uint8_t>(file.data(), h.firstPacketSize - 1), out));  // short
    CHECK(d.Failed());

    // A frame size beyond the video data.
    bad = file;
    const std::size_t frameHeader = 0x24 + 12 + 12 + 4 + 4;  // header, fixed packet part, 1 audio size, extradata
    bad[frameHeader] = 0xFF;
    vid::xmv::Demuxer d2;
    CHECK_FALSE(d2.PushPacket(std::span<const std::uint8_t>(bad.data(), h.firstPacketSize), out));
    CHECK(d2.Error().find("exceeds") != std::string::npos);
}

TEST_CASE("XMV movie core: memory packet source, queues per kept track, EOF") {
    vid::MemoryPacketSource source(SynthMovie());
    vid::xmv::FileHeader h;
    std::string error;
    const std::vector<std::uint8_t> file = SynthMovie();
    REQUIRE(vid::xmv::ParseFileHeader(file, h, error));
    vid::MovieCore core;
    core.Open(&source, h.firstPacketSize, 1u << 1);  // keep track 1 only
    REQUIRE(core.ReadPacket());
    CHECK(core.Video().size() == 2);
    CHECK(core.Audio(0).empty());
    CHECK(core.Audio(1).size() == 1);
    CHECK(core.AudioFramesDemuxed(0) == 128);  // counted even when dropped
    REQUIRE(core.ReadPacket());
    CHECK(core.Eof());
    CHECK_FALSE(core.ReadPacket());
    CHECK_FALSE(core.Failed());
    CHECK(core.Video().size() == 3);
    CHECK(core.Stats().lastPts == 133);
    CHECK(core.Stats().lastFrameGapMs == 67);
    CHECK(core.Stats().maxFrameGapMs == 67);
}

TEST_CASE("XMV extradata conversion matches FFmpeg xmv_read_extradata") {
    CHECK(vid::xmv::ConvertExtradata(0x75) == std::array<std::uint8_t, 4>{0x00, 0x00, 0xAC, 0x80});
    CHECK(vid::xmv::ConvertExtradata(0) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
    CHECK(vid::xmv::ConvertExtradata(0x01) == std::array<std::uint8_t, 4>{0, 0, 0x80, 0x00});  // mspel -> bit 15
    CHECK(vid::xmv::ConvertExtradata(0x1C0) == std::array<std::uint8_t, 4>{0, 0, 0x03, 0x80});  // 7 slices
}

TEST_CASE("YUY2: I420 packing with interstitial chroma, BT.601 conversion and raw mode") {
    // 4x4 picture: luma 0..15, U rows {10, 20}/{30, 40}... chroma 2x2.
    std::uint8_t y[16];
    for (int i = 0; i < 16; ++i) {
        y[i] = static_cast<std::uint8_t>(i);
    }
    const std::uint8_t u[4] = {100, 110, 200, 210};
    const std::uint8_t v[4] = {50, 60, 150, 160};
    std::uint8_t out[4 * 8] = {};
    vid::PackI420ToYuy2(y, u, v, 4, 4, out, 8);
    // Row 0 (even, top edge): chroma row 0 only.
    CHECK(out[0] == 0);
    CHECK(out[1] == 100);
    CHECK(out[2] == 1);
    CHECK(out[3] == 50);
    CHECK(out[5] == 110);
    // Row 1 (odd): 3/4 row 0 + 1/4 row 1 = (300 + 200 + 2) / 4 = 125.
    CHECK(out[8 + 0] == 4);
    CHECK(out[8 + 1] == 125);
    CHECK(out[8 + 3] == (3 * 50 + 150 + 2) / 4);
    // Row 2 (even): 3/4 row 1 + 1/4 row 0 = (600 + 100 + 2) / 4 = 175.
    CHECK(out[16 + 1] == 175);
    // Row 3 (odd, bottom edge): chroma row 1 only.
    CHECK(out[24 + 1] == 200);
    CHECK(out[24 + 2] == 13);

    std::uint8_t r, g, b;
    gfx::tex::YuvToRgbBt601(16, 128, 128, r, g, b);
    CHECK((r == 0 && g == 0 && b == 0));
    gfx::tex::YuvToRgbBt601(235, 128, 128, r, g, b);
    CHECK((r == 255 && g == 255 && b == 255));
    gfx::tex::YuvToRgbBt601(81, 90, 240, r, g, b);  // BT.601 red
    CHECK(r >= 253);
    CHECK(g <= 2);
    CHECK(b <= 2);
    gfx::tex::YuvToRgbBt601(145, 54, 34, r, g, b);  // BT.601 green
    CHECK(r <= 2);
    CHECK(g >= 253);
    CHECK(b <= 2);

    const std::uint8_t yuy2[8] = {16, 128, 235, 128, 81, 90, 82, 240};  // black, white | red, red-ish
    std::uint8_t bgra[16] = {};
    gfx::tex::ConvertYuy2ToBgra(yuy2, 4, 2, 2, bgra, 8, gfx::tex::Yuy2Mode::ConvertBt601);
    CHECK((bgra[0] == 0 && bgra[1] == 0 && bgra[2] == 0 && bgra[3] == 255));
    CHECK((bgra[4] == 255 && bgra[5] == 255 && bgra[6] == 255 && bgra[7] == 255));
    CHECK(bgra[8 + 2] >= 253);  // second row: red (B G R A)
    gfx::tex::ConvertYuy2ToBgra(yuy2, 4, 2, 1, bgra, 8, gfx::tex::Yuy2Mode::Raw);
    CHECK((bgra[0] == 128 && bgra[1] == 16 && bgra[2] == 128 && bgra[5] == 235));  // B = Cb, G = Y, R = Cr
}

TEST_CASE("Movie clock: audio clock, wall clock fallback after the audio, monotonic, frames due by pts") {
    vid::MovieClock c;
    CHECK(c.NowMs(1000, false, 0) == 0);  // not started
    c.Start(44100, 1000, 1000);           // ticks in ms for the test
    CHECK(c.UsesAudio());
    CHECK(c.NowMs(0, false, 1000) == 0);
    CHECK(c.NowMs(44100, false, 1500) == 1000);  // the audio decides, not the wall clock
    CHECK(c.NowMs(22050, false, 2000) == 1000);  // never backwards
    // Audio played out at 2000 ms: from then on the wall clock continues from the audio end.
    CHECK(c.NowMs(88200, true, 3000) == 2000);
    CHECK_FALSE(c.UsesAudio());
    CHECK(c.NowMs(88200, true, 3250) == 2250);

    vid::MovieClock w;
    w.Start(0, 500, 1000);  // no audio: wall clock from the start
    CHECK(w.NowMs(0, false, 500) == 0);
    CHECK(w.NowMs(0, false, 567) == 67);

    // Irregular gaps (avi.md 3.3): 0, 66, 133, 933 (an 800 ms gap), 1000.
    const std::uint32_t pts[] = {0, 66, 133, 933, 1000};
    CHECK(vid::FramesDue(pts, 0) == 1);
    CHECK(vid::FramesDue(pts, 65) == 1);
    CHECK(vid::FramesDue(pts, 66) == 2);
    CHECK(vid::FramesDue(pts, 900) == 3);  // the 133 ms frame stays up during the gap
    CHECK(vid::FramesDue(pts, 5000) == 5);
    // 25-fps files: 40 ms per frame -> 25 frames in the first second.
    std::vector<std::uint32_t> pal;
    for (std::uint32_t i = 0; i < 50; ++i) {
        pal.push_back(i * 40);
    }
    CHECK(vid::FramesDue(pal, 999) == 25);
    CHECK(vid::MovieEndMs(2933, 67, 2969) == 3000);
    CHECK(vid::MovieEndMs(2933, 67, 3100) == 3100);
    CHECK(vid::MovieEndMs(100, 0, 0) == 101);
}

TEST_CASE("[game] skip_videos: parsing, schema default and the skip decision") {
    vid::SkipVideos m = vid::SkipVideos::Original;
    CHECK(vid::ParseSkipVideos("always", m));
    CHECK(m == vid::SkipVideos::Always);
    CHECK(vid::ParseSkipVideos("Original", m));
    CHECK(m == vid::SkipVideos::Original);
    CHECK_FALSE(vid::ParseSkipVideos("never", m));
    CHECK_FALSE(vid::ParseSkipVideos("", m));
    CHECK(std::string(vid::SkipVideosName(vid::SkipVideos::Always)) == "always");
    CHECK(core::IsKnownConfigKey("game", "skip_videos"));
    CHECK(core::ConfigKeyDefault("game", "skip_videos") == "always");
    CHECK(std::string(core::ConfigKeyChoices("game", "skip_videos")) == "always|original");
    // always: every press skips (first viewing of 1-1-01/02, < 2.5 s); original: the game's answer.
    CHECK(vid::CutsceneSkipAllowed(vid::SkipVideos::Always, false));
    CHECK(vid::CutsceneSkipAllowed(vid::SkipVideos::Always, true));
    CHECK_FALSE(vid::CutsceneSkipAllowed(vid::SkipVideos::Original, false));
    CHECK(vid::CutsceneSkipAllowed(vid::SkipVideos::Original, true));
}

TEST_CASE("D3D shim: YUY2 movie texture, content generation per write, unique serials") {
    const std::uint32_t tex = gfx::d3d::DeviceCreateTexture2(8, 4, 1, 1, 0, gfx::d3d::kFormatYuy2, 3);
    REQUIRE(tex != 0);
    const std::uint32_t surf = gfx::d3d::TextureGetSurfaceLevel2(tex, 0);
    REQUIRE(surf != 0);
    gfx::d3d::ResourceInfo t, s;
    REQUIRE(gfx::d3d::LookupResource(tex, t));
    REQUIRE(gfx::d3d::LookupResource(surf, s));
    CHECK(t.format == 0x24);
    CHECK(t.pitch == 16);
    CHECK(s.pixels == t.pixels);
    CHECK(t.serial != s.serial);
    CHECK(t.generation == 0);
    CHECK(gfx::d3d::NoteResourceWritten(surf) == 1);
    CHECK(gfx::d3d::NoteResourceWritten(tex) == 2);
    REQUIRE(gfx::d3d::LookupResource(tex, t));
    REQUIRE(gfx::d3d::LookupResource(surf, s));
    CHECK(t.generation == 2);
    CHECK(s.generation == 2);  // a surface reports its texture's generation
    CHECK(gfx::d3d::NoteResourceWritten(0x1234) == 0);
    gfx::d3d::ResourceRelease(surf);
    const std::uint32_t oldSerial = t.serial;
    gfx::d3d::ResourceRelease(tex);
    const std::uint32_t tex2 = gfx::d3d::DeviceCreateTexture2(8, 4, 1, 1, 0, gfx::d3d::kFormatYuy2, 3);
    REQUIRE(tex2 != 0);
    REQUIRE(gfx::d3d::LookupResource(tex2, t));
    CHECK(t.serial != oldSerial);  // a texture at a reused header address is a new resource
    gfx::d3d::ResourceRelease(tex2);
}

TEST_CASE("CRC-32 matches zlib") {
    CHECK(vid::Crc32("123456789", 9) == 0xCBF43926u);
    CHECK(vid::Crc32("", 0) == 0u);
    CHECK(vid::Crc32("6789", 4, vid::Crc32("12345", 5)) == 0xCBF43926u);
}

// Real files (optional): the two samples avi.md 3.4 decoded with FFmpeg (45 frames / 130944 samples per track and
// 71 frames / 260928 samples). Skipped when the git-ignored samples are missing.
TEST_CASE("XMV decode of the disc samples through the Windows WMV decoder (if present)") {
    // The repository is a parent of the test executable (build/<dir>/bin/fut_tests.exe).
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path dir;
    std::error_code ec;
    for (std::filesystem::path p = std::filesystem::path(exe).parent_path(); !p.empty() && p != p.root_path();
         p = p.parent_path()) {
        if (std::filesystem::exists(p / "analysis/loader/samples/uds-PAL.xmv", ec)) {
            dir = p / "analysis/loader/samples";
            break;
        }
    }
    if (dir.empty() || !vid::WmvDecoderAvailable()) {
        MESSAGE("samples or wmvdecod.dll missing - skipped");
        return;
    }
    struct Expect {
        const char* name;
        std::uint32_t frames;
        std::uint64_t samples;
        std::uint32_t tracks;
    };
    for (const Expect& e : {Expect{"2-1-03-PAL.xmv", 45, 130944, 5}, Expect{"uds-PAL.xmv", 71, 260928, 1}}) {
        std::ifstream f(dir / e.name, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        vid::xmv::FileHeader h;
        std::string error;
        REQUIRE(vid::xmv::ParseFileHeader(bytes, h, error));
        vid::MemoryPacketSource source(bytes);
        vid::MovieCore core;
        core.Open(&source, h.firstPacketSize, 0xFFFF);
        while (core.ReadPacket()) {
        }
        CHECK_FALSE(core.Failed());
        CHECK(core.Eof());
        CHECK(core.Video().size() == e.frames);
        CHECK(h.tracks.size() == e.tracks);
        CHECK(core.AudioFramesDemuxed(0) == e.samples);
        vid::I420Frame pic;
        std::uint32_t produced = 0;
        while (!core.Video().empty()) {
            bool p = false;
            CHECK(core.Decode(core.Video().front(), pic, p));
            produced += p ? 1u : 0u;
            core.Video().pop_front();
        }
        CHECK(produced == e.frames);
        CHECK(pic.width == 640);
        CHECK(pic.height == 480);
    }
}
