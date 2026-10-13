// Unit tests of the G2 texture packs (gfx/texture_pack.h): XXH64 reference vectors, the "FTX1" content hash (also
// from guest memory), hash file names, DDS parsing (BC1/BC3/BC7, DX10, legacy BGRA, rejects), mip generation for PNG
// replacements, the sampler extension, the pack index and its asynchronous loader, and the schema rows.
#include "doctest.h"

#include "core/config_schema.h"
#include "gfx/guest_memory.h"
#include "gfx/texture_pack.h"
#include "platform/screenshot.h"

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace gfx;
using namespace gfx::texpack;

namespace {

fs::path PackTempDir(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const fs::path dir = fs::path(buf).parent_path() / "fut_tests_tmp" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void Put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    std::memcpy(b.data() + at, &v, 4);
}

// A DDS file: header (+ DX10 header if dxgi != 0) + `payload` bytes.
std::vector<std::uint8_t> MakeDds(std::uint32_t width, std::uint32_t height, std::uint32_t mips, const char* fourCc,
                                  std::uint32_t dxgi, std::size_t payload, std::uint32_t caps2 = 0) {
    std::vector<std::uint8_t> b(128 + (dxgi != 0 ? 20 : 0) + payload, 0);
    std::memcpy(b.data(), "DDS ", 4);
    Put32(b, 4, 124);
    Put32(b, 8, 0x1007 | 0x20000);  // caps, height, width, pixel format, mip count
    Put32(b, 12, height);
    Put32(b, 16, width);
    Put32(b, 28, mips);
    Put32(b, 76, 32);
    if (fourCc != nullptr) {
        Put32(b, 80, 0x4);
        std::memcpy(b.data() + 84, fourCc, 4);
    } else {
        Put32(b, 80, 0x41);  // RGB + alpha pixels
        Put32(b, 88, 32);
        Put32(b, 92, 0x00FF0000);
        Put32(b, 96, 0x0000FF00);
        Put32(b, 100, 0x000000FF);
        Put32(b, 104, 0xFF000000);
    }
    Put32(b, 112, caps2);
    if (dxgi != 0) {
        Put32(b, 128, dxgi);
        Put32(b, 132, 3);  // TEXTURE2D
        Put32(b, 140, 1);  // array size
    }
    for (std::size_t i = 0; i < payload; ++i) {
        b[b.size() - payload + i] = static_cast<std::uint8_t>(i * 7 + 1);
    }
    return b;
}

void WriteFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

HashSource Dxt1Source(const std::vector<std::uint8_t>& level0, std::uint32_t w, std::uint32_t h) {
    HashSource s;
    s.format = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT1);
    s.width = w;
    s.height = h;
    s.level0 = level0.data();
    s.level0Size = static_cast<std::uint32_t>(level0.size());
    return s;
}

}  // namespace

TEST_CASE("texture pack: XXH64 reference vectors and streaming") {
    CHECK(Xxh64Hash("", 0) == 0xEF46DB3751D8E999ull);
    CHECK(Xxh64Hash("a", 1) == 0xD24EC4F1A98C6E5Bull);
    CHECK(Xxh64Hash("abc", 3) == 0x44BC2CF5AD770999ull);
    const char* text = "Nobody inspects the spammish repetition";  // 39 bytes: the 32-byte stripe path
    CHECK(Xxh64Hash(text, std::strlen(text)) == 0xFBCEA83C8A378BF1ull);
    std::vector<std::uint8_t> data(1000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i * 31 + (i >> 3));
    }
    const std::uint64_t whole = Xxh64Hash(data.data(), data.size());
    for (std::size_t split : {1u, 7u, 31u, 32u, 33u, 500u, 999u}) {
        Xxh64 h;
        h.Update(data.data(), split);
        h.Update(data.data() + split, data.size() - split);
        CHECK(h.Digest() == whole);
    }
    CHECK(Xxh64Hash(data.data(), data.size(), 1) != whole);
}

TEST_CASE("texture pack: FTX1 content hash") {
    std::vector<std::uint8_t> level0(32);  // 8x8 DXT1 = 4 blocks
    for (std::size_t i = 0; i < level0.size(); ++i) {
        level0[i] = static_cast<std::uint8_t>(i);
    }
    const std::uint64_t base = TextureHash(Dxt1Source(level0, 8, 8));
    // Pinned value of scheme FTX1: changing the scheme invalidates every pack - only on purpose.
    {
        std::uint32_t header[10] = {kHashTag, 4, 0, 8, 8, 0, 0, 0, 0, 32};
        const ni::NiPixelFormat f = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT1);
        header[2] = f.bitsPerPixel;
        header[5] = f.redMask;
        header[6] = f.greenMask;
        header[7] = f.blueMask;
        header[8] = f.alphaMask;
        Xxh64 h;
        h.Update(header, sizeof(header));
        h.Update(level0.data(), level0.size());
        CHECK(base == h.Digest());
    }
    std::vector<std::uint8_t> other = level0;
    other[17] ^= 1;
    CHECK(TextureHash(Dxt1Source(other, 8, 8)) != base);
    CHECK(TextureHash(Dxt1Source(level0, 16, 4)) != base);
    HashSource dxt3 = Dxt1Source(level0, 8, 8);
    dxt3.format = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT3);
    CHECK(TextureHash(dxt3) != base);
    // A palette only counts for PAL/PALA.
    std::vector<std::uint8_t> palette(1024, 9);
    HashSource withPalette = Dxt1Source(level0, 8, 8);
    withPalette.paletteRgba = palette.data();
    CHECK(TextureHash(withPalette) == base);
    HashSource pal = Dxt1Source(level0, 8, 4);
    pal.format = tex::MakePaletteFormat(false);
    const std::uint64_t noPalette = TextureHash(pal);
    pal.paletteRgba = palette.data();
    const std::uint64_t p1 = TextureHash(pal);
    palette[5] = 1;
    CHECK(TextureHash(pal) != p1);
    CHECK(p1 != noPalette);
    pal.paletteHasAlpha = true;
    CHECK(TextureHash(pal) != p1);
}

TEST_CASE("texture pack: hash of a guest NiPixelData = host hash of level 0 (mips ignored)") {
    auto* base = static_cast<std::uint8_t*>(AllocGuestPages(0x10000));
    REQUIRE(base != nullptr);
    const ni::GuestPtr g = static_cast<ni::GuestPtr>(reinterpret_cast<std::uintptr_t>(base));
    // pixels: 8x8 DXT1 (32 bytes) + 4x4 (8 bytes); widths/heights/offsets arrays behind the object.
    auto* pd = reinterpret_cast<ni::NiPixelData*>(base);
    pd->format = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT1);
    pd->pixels = g + 0x1000;
    pd->widths = g + 0x100;
    pd->heights = g + 0x110;
    pd->offsets = g + 0x120;
    pd->mipCount = 2;
    pd->revision = 1;
    const std::uint32_t widths[2] = {8, 4}, heights[2] = {8, 4}, offsets[3] = {0, 32, 40};
    std::memcpy(base + 0x100, widths, 8);
    std::memcpy(base + 0x110, heights, 8);
    std::memcpy(base + 0x120, offsets, 12);
    std::vector<std::uint8_t> level0(32);
    for (std::size_t i = 0; i < 40; ++i) {
        base[0x1000 + i] = static_cast<std::uint8_t>(i * 3);
        if (i < 32) {
            level0[i] = base[0x1000 + i];
        }
    }
    std::uint64_t hash = 0;
    ImageInfo info;
    REQUIRE(HashGuestPixelData(g, &hash, &info));
    CHECK(hash == TextureHash(Dxt1Source(level0, 8, 8)));
    CHECK(info.width == 8);
    CHECK(info.height == 8);
    CHECK(info.mipCount == 2);
    CHECK(std::string(SourceFormatName(info.format, info.bitsPerPixel)) == "DXT1");
    base[0x1000 + 35] ^= 0xFF;  // level 1 is not hashed
    std::uint64_t again = 0;
    REQUIRE(HashGuestPixelData(g, &again, nullptr));
    CHECK(again == hash);
    pd->pixels = 0;
    CHECK_FALSE(HashGuestPixelData(g, &again, nullptr));
    CHECK_FALSE(HashGuestPixelData(0, &again, nullptr));
    FreeGuestPages(base);
}

TEST_CASE("texture pack: hash file names") {
    CHECK(HashName(0x0123456789ABCDEFull) == "0123456789abcdef");
    std::uint64_t h = 0;
    CHECK(ParseHashName("0123456789abcdef", &h));
    CHECK(h == 0x0123456789ABCDEFull);
    CHECK(ParseHashName("0123456789ABCDEF_512x512", &h));
    CHECK(h == 0x0123456789ABCDEFull);
    CHECK(ParseHashName("0123456789abcdef.4x", &h));
    CHECK_FALSE(ParseHashName("0123456789abcdef0", &h));  // 17 hex digits
    CHECK_FALSE(ParseHashName("0123456789abcdeg", &h));
    CHECK_FALSE(ParseHashName("0123456789abc", &h));
    CHECK_FALSE(ParseHashName("index", &h));
}

TEST_CASE("texture pack: DDS parsing") {
    tex::HostImage img;
    std::string detail;
    SUBCASE("DXT1 with 4 levels (8, 4, 2, 1)") {
        const auto b = MakeDds(8, 8, 4, "DXT1", 0, 32 + 8 + 8 + 8);
        REQUIRE(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::Ok);
        CHECK(img.format == rhi::Format::BC1_UNorm);
        CHECK(img.mipCount == 4);
        CHECK(img.mips[1].offset == 32);
        CHECK(img.mips[3].width == 1);
        CHECK(img.mips[3].size == 8);
        CHECK(img.bytes.size() == 56);
        CHECK(img.bytes[0] == 1);
    }
    SUBCASE("DXT5, mip count 0 = one level, more stored levels than the chain are clamped") {
        auto b = MakeDds(4, 4, 0, "DXT5", 0, 16);
        REQUIRE(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::Ok);
        CHECK(img.format == rhi::Format::BC3_UNorm);
        CHECK(img.mipCount == 1);
        b = MakeDds(4, 4, 9, "DXT5", 0, 16 * 3);
        REQUIRE(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::Ok);
        CHECK(img.mipCount == 3);
    }
    SUBCASE("DX10 BC7 (and its sRGB variant as UNORM)") {
        auto b = MakeDds(8, 4, 1, "DX10", 98, 32);
        REQUIRE(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::Ok);
        CHECK(img.format == rhi::Format::BC7_UNorm);
        CHECK(img.bytes.size() == 32);
        CHECK(img.bytes[0] == 1);
        b = MakeDds(8, 4, 1, "DX10", 99, 32);
        REQUIRE(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::Ok);
        CHECK(img.format == rhi::Format::BC7_UNorm);
        b = MakeDds(8, 4, 1, "DX10", 2, 512);  // R32G32B32A32_FLOAT
        CHECK(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::Unsupported);
    }
    SUBCASE("legacy 32-bit BGRA, non-power-of-two") {
        const auto b = MakeDds(6, 3, 2, nullptr, 0, 6 * 3 * 4 + 3 * 1 * 4);
        REQUIRE(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::Ok);
        CHECK(img.format == rhi::Format::BGRA8_UNorm);
        CHECK(img.mips[1].width == 3);
        CHECK(img.mips[1].height == 1);
        CHECK(img.mips[0].rowPitch == 24);
    }
    SUBCASE("rejects") {
        auto b = MakeDds(8, 8, 1, "DXT1", 0, 31);
        CHECK(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::Truncated);
        CHECK(img.bytes.empty());
        b = MakeDds(6, 8, 1, "DXT1", 0, 64);
        CHECK(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::BadSize);
        b = MakeDds(8, 8, 1, "DXT1", 0, 32, 0x200 | 0xFC00);
        CHECK(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::Unsupported);
        b = MakeDds(8, 8, 1, "ATI2", 0, 64);
        CHECK(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::Unsupported);
        b = MakeDds(8, 8, 1, "DXT1", 0, 32);
        b[0] = 'X';
        CHECK(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::NotDds);
        CHECK(ParseDds(b.data(), 10, &img, &detail) == LoadStatus::NotDds);
        b = MakeDds(0, 8, 1, "DXT1", 0, 32);
        CHECK(ParseDds(b.data(), b.size(), &img, &detail) == LoadStatus::BadSize);
    }
}

TEST_CASE("texture pack: generated mip chain of a PNG replacement") {
    // 3x2: opaque red/green/blue on top, a transparent black texel and two white ones below.
    const std::uint8_t px[] = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255,
                               0, 0, 0, 0,     255, 255, 255, 255, 255, 255, 255, 255};
    tex::HostImage img;
    REQUIRE(MakeMippedBgra(px, 3, 2, &img));
    CHECK(img.format == rhi::Format::BGRA8_UNorm);
    CHECK(img.mipCount == 2);
    CHECK(img.mips[1].width == 1);
    CHECK(img.mips[1].height == 1);
    CHECK(img.hasAlpha);
    const std::uint8_t* top = img.bytes.data() + img.mips[1].offset;
    // Alpha-weighted: the transparent black texel does not darken the colour; alpha = 5 * 255 / 6.
    CHECK(top[3] == 213);
    CHECK(top[0] == 153);  // (0 + 0 + 255 + 255 + 255) / 5 per channel: the black texel has weight 0
    CHECK(top[1] == 153);
    CHECK(top[2] == 153);
    std::vector<std::uint8_t> big(256 * 64 * 4, 200);
    REQUIRE(MakeMippedBgra(big.data(), 256, 64, &img));
    CHECK(img.mipCount == 9);  // 256x64 .. 1x1
    CHECK(img.mips[8].width == 1);
    CHECK(img.mips[6].height == 1);
    CHECK(img.bytes[img.mips[8].offset] == 200);
    CHECK_FALSE(MakeMippedBgra(big.data(), 0, 64, &img));
}

TEST_CASE("texture pack: sampler extension levels") {
    CHECK(ExtraLevels(256, 256, 256, 256) == 0);
    CHECK(ExtraLevels(256, 256, 512, 512) == 1);
    CHECK(ExtraLevels(256, 128, 1024, 512) == 2);
    CHECK(ExtraLevels(256, 128, 1024, 300) == 1);  // smaller axis decides
    CHECK(ExtraLevels(256, 256, 128, 128) == 0);
    CHECK(ExtraLevels(0, 256, 128, 128) == 0);
    rhi::SamplerDesc d;
    d.mipFilter = rhi::MipFilter::None;
    d.maxLod = 0.0f;
    AdjustSampler(rhi::TextureHandle{12345}, &d);  // not a replacement
    CHECK(d.mipFilter == rhi::MipFilter::None);
    CHECK(d.maxLod == 0.0f);
}

TEST_CASE("texture pack: index and asynchronous loader") {
    const fs::path dir = PackTempDir(L"texture pack");
    fs::create_directories(dir / "sub");
    // PNG for hash A, PNG + DDS for hash B (DDS wins), a broken DDS for C, files that are not pack entries.
    rhi::Image png;
    png.format = rhi::Format::RGBA8_UNorm;
    png.width = 5;
    png.height = 3;
    png.rowPitch = 20;
    png.pixels.assign(60, 0);
    for (std::size_t i = 0; i < 60; i += 4) {
        png.pixels[i] = 255;       // R
        png.pixels[i + 3] = 128;   // A
    }
    std::string error;
    REQUIRE(platform::WritePngFileRgba(dir / "00000000000000aa.png", png, &error));
    REQUIRE(platform::WritePngFileRgba(dir / "sub" / "00000000000000BB_2x.png", png, &error));
    WriteFile(dir / "00000000000000bb.dds", MakeDds(8, 8, 2, "DXT1", 0, 40));
    WriteFile(dir / "00000000000000cc.dds", MakeDds(8, 8, 1, "DXT1", 0, 4));
    WriteFile(dir / "readme.png", {1, 2, 3});
    WriteFile(dir / "00000000000000dd.txt", {1, 2, 3});

    Pack pack;
    REQUIRE(pack.Open(dir, &error));
    CHECK(pack.FileCount() == 3);
    const Pack::Stats st = pack.GetStats();
    CHECK(st.files == 3);
    CHECK(st.dds == 2);
    CHECK(st.png == 1);
    CHECK(st.duplicates == 1);
    CHECK(pack.FileOf(0xBB).extension() == ".dds");
    CHECK(pack.FileOf(0xDD).empty());

    CHECK_FALSE(pack.Request(0xDD));
    CHECK(pack.Fetch(0xAA, nullptr) == Pack::State::Missing);
    CHECK(pack.Request(0xAA));
    CHECK(pack.Request(0xBB));
    CHECK(pack.Request(0xCC));
    REQUIRE(pack.WaitIdle(10000));
    std::shared_ptr<const tex::HostImage> img;
    REQUIRE(pack.Fetch(0xAA, &img) == Pack::State::Ready);
    CHECK(img->format == rhi::Format::BGRA8_UNorm);
    CHECK(img->width == 5);
    CHECK(img->mipCount == 3);  // 5x3, 2x1, 1x1
    const std::uint8_t* t = img->bytes.data();
    CHECK(t[2] == 255);  // R (BGRA order)
    CHECK(t[3] == 128);
    REQUIRE(pack.Fetch(0xBB, &img) == Pack::State::Ready);
    CHECK(img->format == rhi::Format::BC1_UNorm);
    CHECK(img->mipCount == 2);
    CHECK(pack.Fetch(0xCC, &img) == Pack::State::Failed);
    CHECK(pack.Request(0xAA));  // cached
    CHECK(pack.GetStats().cacheHits == 1);
    CHECK(pack.GetStats().decoded == 2);
    CHECK(pack.GetStats().failed == 1);

    tex::HostImage direct;
    CHECK(LoadReplacementFile(dir / "missing.png", &direct, &error) == LoadStatus::NotFound);
    CHECK(LoadReplacementFile(dir / "readme.png", &direct, &error) == LoadStatus::DecodeFailed);
    Pack none;
    CHECK_FALSE(none.Open(dir / "missing", &error));
}

TEST_CASE("texture pack: schema rows and format table") {
    const core::ConfigKeyDef* pack = core::FindConfigKey("render", "texture_pack");
    REQUIRE(pack != nullptr);
    CHECK(pack->type == core::ConfigKeyType::PathDir);
    CHECK(std::string(pack->defaultValue).empty());
    CHECK(pack->widget == core::ConfigUiWidget::Dir);
    CHECK(pack->tab == core::ConfigUiTab::Graphics);
    const core::ConfigKeyDef* dump = core::FindConfigKey("render", "texture_pack_dump");
    REQUIRE(dump != nullptr);
    CHECK(dump->type == core::ConfigKeyType::Bool);
    CHECK(std::string(dump->defaultValue) == "0");
    const rhi::FormatInfo& bc7 = rhi::GetFormatInfo(rhi::Format::BC7_UNorm);
    CHECK(bc7.compressed);
    CHECK(bc7.bytesPerBlock == 16);
    CHECK(rhi::LevelSize(rhi::Format::BC7_UNorm, 8, 4) == 32);
}
