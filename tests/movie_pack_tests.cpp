// M6b unit tests: movie packs (video/movie_pack.h) - index parsing and lookup by the XMV header CRC, the frame
// mapping rules (pts check, reader queue steps, 100-ns -> ms), the texel UV scale of the movie texture
// (gfx/movie_uv.h), the D3D shim's host image, the schema row, and - when a pack built by tools/movie_upscale.py
// exists under <repo or a parent>/run/moviepack_ai_x2 (git-ignored) - the Media Foundation reader on a real file.
#include "doctest.h"

#include "core/config_schema.h"
#include "gfx/d3d_shim.h"
#include "gfx/movie_uv.h"
#include "video/movie_pack.h"
#include "video/movie_pack_reader.h"

#include <windows.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char* const kIndex =
    "# Futurama PC movie pack v1\r\n"
    "# name\theader_crc32\txmv_bytes\tframes\twidth\theight\tduration_ms\tfile\r\n"
    "1-1-00-PAL\t831F5BB4\t2519040\t93\t1280\t960\t7732\t1-1-00-PAL.mp4\r\n"
    "Intro-PAL\t831F5BB4\t2519040\t93\t1280\t960\t7732\t1-1-00-PAL.mp4\r\n"
    "\r\n"
    "SCI-PAL\t708cf314\t2007040\t243\t1280\t960\t10080\tSCI-PAL.mp4\n"
    "odd\t00000001\t1\t1\t1281\t960\t1\todd.mp4\n"
    "evil\t00000002\t1\t1\t1280\t960\t1\t..\\evil.mp4\n"
    "sub\t00000003\t1\t1\t1280\t960\t1\tsub/x.mp4\n"
    "notmp4\t00000004\t1\t1\t1280\t960\t1\tx.avi\n"
    "short\t00000005\t1\t1\t1280\n"
    "badhex\tXYZ\t1\t1\t1280\t960\t1\tb.mp4\n"
    "huge\t00000006\t1\t1\t8192\t6144\t1\th.mp4\n"
    "small\tEFB1276E\t528384\t71\t320\t240\t5532\tsmall.mp4\n"
    "wide\tEFB1276E\t528384\t71\t1280\t720\t5532\twide.mp4\n"
    "uds-PAL\tEFB1276E\t528384\t71\t1280\t960\t5532\tuds-PAL.mp4";  // no final newline

fs::path FindRunPack() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::error_code ec;
    for (fs::path p = fs::path(exe).parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
        if (fs::exists(p / "run/moviepack_ai_x2/index.tsv", ec)) {
            return p / "run/moviepack_ai_x2";
        }
    }
    return {};
}

}  // namespace

TEST_CASE("movie pack: index parsing rejects malformed and unsafe lines") {
    std::uint32_t bad = 0;
    std::string error;
    const std::vector<vid::PackEntry> e = vid::ParsePackIndex(kIndex, bad, error);
    REQUIRE(e.size() == 6);
    CHECK(bad == 7);  // odd, evil, sub, notmp4, short, badhex, huge
    CHECK(error.find("line 7") != std::string::npos);
    CHECK(e[0].name == "1-1-00-PAL");
    CHECK(e[0].headerCrc == 0x831F5BB4u);
    CHECK(e[0].xmvBytes == 2519040u);
    CHECK(e[0].frames == 93u);
    CHECK(e[0].width == 1280u);
    CHECK(e[0].height == 960u);
    CHECK(e[0].durationMs == 7732u);
    CHECK(e[0].file == "1-1-00-PAL.mp4");  // CR stripped
    CHECK(e[1].file == "1-1-00-PAL.mp4");  // identical movies share a file
    CHECK(e[2].headerCrc == 0x708CF314u);  // lower-case hex
    CHECK(e[5].name == "uds-PAL");         // last line without newline
    CHECK(vid::ParsePackIndex("", bad, error).empty());
    CHECK(bad == 0);
}

TEST_CASE("movie pack: lookup by header CRC, picture size and aspect ratio") {
    std::uint32_t bad = 0;
    std::string error;
    const std::vector<vid::PackEntry> e = vid::ParsePackIndex(kIndex, bad, error);
    const vid::PackEntry* intro = vid::FindPackEntry(e, 0x831F5BB4u, 640, 480);
    REQUIRE(intro != nullptr);
    CHECK(intro->name == "1-1-00-PAL");  // the first of identical movies
    const vid::PackEntry* uds = vid::FindPackEntry(e, 0xEFB1276Eu, 640, 480);
    REQUIRE(uds != nullptr);
    CHECK(uds->name == "uds-PAL");  // "small" (smaller than the XMV) and "wide" (other aspect) are skipped
    CHECK(vid::FindPackEntry(e, 0x12345678u, 640, 480) == nullptr);
    CHECK(vid::FindPackEntry(e, 0x831F5BB4u, 0, 480) == nullptr);
    CHECK(vid::FindPackEntry(e, 0x831F5BB4u, 640, 360) == nullptr);  // a 16:9 XMV would not match a 4:3 picture
    CHECK(vid::FindPackEntry({}, 0x831F5BB4u, 640, 480) == nullptr);
}

TEST_CASE("movie pack: frame mapping - pts check, 100-ns conversion, reader queue steps") {
    // Same moment relative to each stream's first frame, within 1 ms.
    CHECK(vid::PackPtsMatch(0, 66, 0, 66));
    CHECK(vid::PackPtsMatch(0, 66, 0, 67));
    CHECK_FALSE(vid::PackPtsMatch(0, 66, 0, 68));
    CHECK(vid::PackPtsMatch(0, 4666, 0, 4666));  // the long gaps of 1-1-02
    CHECK(vid::PackPtsMatch(100, 166, 2000, 2066));  // offsets of the first frames cancel
    CHECK_FALSE(vid::PackPtsMatch(0, 133, 0, 66));   // one frame off
    CHECK(vid::HnsToMs(0) == 0);
    CHECK(vid::HnsToMs(660000) == 66);   // the packs' ms time scale
    CHECK(vid::HnsToMs(666666) == 67);   // a 15-fps CFR timestamp rounds to nearest
    CHECK(vid::HnsToMs(664999) == 66);
    CHECK(vid::HnsToMs(-660000) == -66);
    // The XMV HLE asks for frame n; frames it skipped (late) are discarded, a newer one means a lost frame.
    CHECK(vid::PackStepFor(3, 5) == vid::PackQueueStep::Discard);
    CHECK(vid::PackStepFor(5, 5) == vid::PackQueueStep::Take);
    CHECK(vid::PackStepFor(6, 5) == vid::PackQueueStep::Ahead);
}

TEST_CASE("movie pack: texel UV scale keeps the movie quad on the same screen area") {
    // The game draws its 640x480 movie texture with UVs 0..640 x 0..480.
    const gfx::TexelScale orig = gfx::MovieTexelScale(640, 480, 640, 480);
    const gfx::TexelScale pack = gfx::MovieTexelScale(640, 480, 1280, 960);
    CHECK(orig.sx == doctest::Approx(1.0 / 640.0));
    CHECK(orig.sy == doctest::Approx(1.0 / 480.0));
    // Pack: game texel u -> host texel 2u -> normalised 2u / 1280 = u / 640: the same coordinate.
    CHECK(640.0f * pack.sx == doctest::Approx(1.0));
    CHECK(480.0f * pack.sy == doctest::Approx(1.0));
    CHECK(320.0f * pack.sx == doctest::Approx(0.5));
    CHECK(0.5f * pack.sx * 1280.0f == doctest::Approx(1.0));  // centre of game texel 0 = host texel edge 1
    const gfx::TexelScale x3 = gfx::MovieTexelScale(640, 480, 1920, 1440);
    CHECK(640.0f * x3.sx == doctest::Approx(1.0));
    const gfx::TexelScale none = gfx::MovieTexelScale(0, 480, 1280, 960);
    CHECK(none.sx == 1.0f);
    CHECK(none.sy == 1.0f);
}

TEST_CASE("movie pack: index file loading") {
    const fs::path dir = fs::temp_directory_path() / "fut_movie_pack_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::vector<vid::PackEntry> entries;
    std::string error;
    CHECK_FALSE(vid::LoadPackIndex(dir, entries, error));
    CHECK(error == "not a folder");
    fs::create_directories(dir);
    CHECK_FALSE(vid::LoadPackIndex(dir, entries, error));
    CHECK(error == "no index.tsv");
    {
        std::ofstream(dir / "index.tsv", std::ios::binary) << "# only comments\n";
    }
    CHECK_FALSE(vid::LoadPackIndex(dir, entries, error));
    {
        std::ofstream(dir / "index.tsv", std::ios::binary) << kIndex;
    }
    REQUIRE(vid::LoadPackIndex(dir, entries, error));
    CHECK(entries.size() == 6);
    CHECK(error.find("7 malformed") != std::string::npos);
    fs::remove_all(dir, ec);
}

TEST_CASE("D3D shim: movie pack host image on the movie texture") {
    namespace d3d = gfx::d3d;
    const std::uint32_t tex = d3d::DeviceCreateTexture2(640, 480, 1, 1, 0, d3d::kFormatYuy2, 3);
    REQUIRE(tex != 0);
    const std::uint32_t surf = d3d::TextureGetSurfaceLevel2(tex, 0);
    REQUIRE(surf != 0);
    CHECK(d3d::GetHostImage(tex) == nullptr);
    auto image = std::make_shared<d3d::HostImage>();
    image->width = 1280;
    image->height = 960;
    image->pitch = 2560;
    image->yuy2.assign(static_cast<std::size_t>(image->pitch) * image->height, 0x80);
    CHECK(d3d::SetHostImage(surf, image));  // through the surface: attached to its texture
    CHECK(d3d::GetHostImage(tex) == image);
    CHECK(d3d::GetHostImage(surf) == image);
    d3d::ResourceInfo info;
    REQUIRE(d3d::LookupResource(tex, info));
    CHECK(info.width == 640);  // the guest texture keeps its size (texel UVs)
    CHECK(d3d::SetHostImage(tex, nullptr));
    CHECK(d3d::GetHostImage(tex) == nullptr);
    CHECK_FALSE(d3d::SetHostImage(0x1234, image));
    CHECK(d3d::SetHostImage(tex, image));
    d3d::ResourceRelease(surf);
    d3d::ResourceRelease(tex);  // destroyed with the texture
    CHECK(d3d::GetHostImage(tex) == nullptr);
    CHECK(image.use_count() == 1);
}

TEST_CASE("movie pack: schema row") {
    const core::ConfigKeyDef* key = core::FindConfigKey("render", "movie_pack");
    REQUIRE(key != nullptr);
    CHECK(key->type == core::ConfigKeyType::PathDir);
    CHECK(std::string(key->defaultValue).empty());  // nothing installed: original pictures
    CHECK(key->widget == core::ConfigUiWidget::Dir);
    CHECK(key->tab == core::ConfigUiTab::Graphics);
}

// Optional: the Media Foundation reader on the smallest movie of a real pack (git-ignored run/ folder).
TEST_CASE("movie pack: Media Foundation reader on a real pack file (if present)") {
    const fs::path dir = FindRunPack();
    std::vector<vid::PackEntry> entries;
    std::string error;
    if (dir.empty() || !vid::LoadPackIndex(dir, entries, error)) {
        MESSAGE("run/moviepack_ai_x2 missing - skipped");
        return;
    }
    const vid::PackEntry* smallest = nullptr;
    for (const vid::PackEntry& e : entries) {
        if (fs::exists(dir / e.file) && (smallest == nullptr || e.frames < smallest->frames)) {
            smallest = &e;
        }
    }
    REQUIRE(smallest != nullptr);
    std::vector<std::int64_t> pts;
    {
        vid::PackVideoReader reader;
        REQUIRE(reader.Open(dir / smallest->file, smallest->width, smallest->height, error));
        for (std::uint32_t i = 0;; ++i) {
            vid::PackFrame f;
            const auto r = reader.Get(i, 5000, f, error);
            if (r == vid::PackVideoReader::Result::End) {
                break;
            }
            REQUIRE_MESSAGE(r == vid::PackVideoReader::Result::Ok, error);
            CHECK(f.index == i);
            REQUIRE(f.image != nullptr);
            CHECK(f.image->width == smallest->width);
            CHECK(f.image->height == smallest->height);
            CHECK(f.image->yuy2.size() == static_cast<std::size_t>(f.image->pitch) * f.image->height);
            pts.push_back(f.ptsMs);
        }
        CHECK(reader.Description().find("YUY2") != std::string::npos);
    }
    CHECK(pts.size() == smallest->frames);
    REQUIRE(!pts.empty());
    CHECK(pts.front() == 0);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        CHECK(pts[i] > pts[i - 1]);
    }
    // Skipping like the XMV HLE with late frames: frame 5 after frame 0, the frames between are discarded.
    vid::PackVideoReader reader;
    REQUIRE(reader.Open(dir / smallest->file, smallest->width, smallest->height, error));
    vid::PackFrame f;
    REQUIRE(reader.Get(0, 5000, f, error) == vid::PackVideoReader::Result::Ok);
    REQUIRE(reader.Get(5, 5000, f, error) == vid::PackVideoReader::Result::Ok);
    CHECK(f.index == 5);
    CHECK(f.ptsMs == pts[5]);
    CHECK(reader.Get(3, 100, f, error) == vid::PackVideoReader::Result::Error);  // already gone
    reader.Close();
}
