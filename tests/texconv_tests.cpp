// Unit tests of gfx/texconv (GFX_CONTRACT.md 7): every NiPixelData format of the conversion table with
// synthetic data (host PixelSource and guest-layout NiPixelData), mip chains, palettes, DXT blocks, the
// default texture, the slot-65 source decision and the Xbox format choice of FUN_001572D0.
#include "doctest.h"

#include "core/guest.h"
#include "gfx/ni_guest_layouts.h"
#include "gfx/texconv.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using gfx::rhi::Format;
using gfx::tex::ConvertStatus;
using gfx::tex::XboxFormat;
namespace ni = gfx::ni;
namespace tex = gfx::tex;

// Host-side image with any number of levels (offsets accumulate like FUN_00111260).
struct Synthetic {
    ni::NiPixelFormat format{};
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint32_t> widths, heights, offsets{0};
    std::vector<std::uint8_t> palette;  // 1024 bytes r, g, b, a
    bool paletteHasAlpha = false;

    void AddLevel(std::uint32_t w, std::uint32_t h, const std::vector<std::uint8_t>& bytes) {
        widths.push_back(w);
        heights.push_back(h);
        pixels.insert(pixels.end(), bytes.begin(), bytes.end());
        offsets.push_back(static_cast<std::uint32_t>(pixels.size()));
    }
    tex::PixelSource Source() const {
        tex::PixelSource s;
        s.format = format;
        s.pixels = pixels.data();
        s.mipCount = static_cast<std::uint32_t>(widths.size());
        s.widths = widths.data();
        s.heights = heights.data();
        s.offsets = offsets.data();
        s.paletteRgba = palette.empty() ? nullptr : palette.data();
        s.paletteHasAlpha = paletteHasAlpha;
        return s;
    }
};

std::vector<std::uint8_t> Words16(std::initializer_list<std::uint16_t> words) {
    std::vector<std::uint8_t> b;
    for (std::uint16_t w : words) {
        b.push_back(static_cast<std::uint8_t>(w));
        b.push_back(static_cast<std::uint8_t>(w >> 8));
    }
    return b;
}

std::vector<std::uint8_t> Words32(std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> b;
    for (std::uint32_t w : words) {
        for (int i = 0; i < 4; ++i) {
            b.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
        }
    }
    return b;
}

// BGRA bytes of texel (x, y) of level `level`.
std::uint32_t Texel(const tex::HostImage& img, std::uint32_t x, std::uint32_t y, std::uint32_t level = 0) {
    const tex::MipLevel& m = img.mips[level];
    const std::uint8_t* p = img.bytes.data() + m.offset + y * m.rowPitch + x * 4;
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint32_t AlphaOf(const tex::HostImage& img, std::uint32_t x, std::uint32_t y) {
    return Texel(img, x, y) >> 24;
}
// D3DCOLOR-style value (a r g b) from components - equals the BGRA byte order read as a little-endian dword.
constexpr std::uint32_t Argb(std::uint32_t a, std::uint32_t r, std::uint32_t g, std::uint32_t b) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}
// n-bit -> 8-bit by bit replication (texconv.h) for n = 5 or 6.
constexpr std::uint32_t Exp(std::uint32_t v, std::uint32_t bits) {
    return bits == 5 ? (v << 3) | (v >> 2) : (v << 2) | (v >> 4);
}

ConvertStatus Convert(const Synthetic& s, tex::HostImage* out, const tex::ConvertOptions& o = {}) {
    return tex::ConvertPixels(s.Source(), o, out);
}

}  // namespace

TEST_CASE("texconv: NiPixelFormat descriptors equal the constructors and the disc data") {
    // Values found in the game's NIF files (run\m3-texconv\pixeldata.tsv).
    const ni::NiPixelFormat rgb565 = tex::MakeRgbFormat(0xF800, 0x7E0, 0x1F, 0, 16);
    CHECK(rgb565.format == 0);
    CHECK(rgb565.channels0 == 0x518540u);
    CHECK(rgb565.channels1 == 0xABu);
    const ni::NiPixelFormat argb4444 = tex::MakeRgbFormat(0xF00, 0xF0, 0xF, 0xF000, 16);
    CHECK(argb4444.format == 1);
    CHECK(argb4444.channels0 == 0x10410441u);
    CHECK(argb4444.channels1 == 0x60088u);
    const ni::NiPixelFormat abgr8888 = tex::MakeRgbFormat(0xFF, 0xFF00, 0xFF0000, 0xFF000000, 32);
    CHECK(abgr8888.channels0 == 0x20820881u);
    CHECK(abgr8888.channels1 == 0xC4100u);
    CHECK(tex::MakeCompressedFormat(ni::NiPixelLayout::DXT1).channels0 == 4u);
    CHECK(tex::MakeCompressedFormat(ni::NiPixelLayout::DXT3).channels0 == 5u);
    CHECK(tex::MakeCompressedFormat(ni::NiPixelLayout::DXT3).channels1 == 0u);
    // FUN_001408A0: format code | 1 byte per pixel.
    CHECK(tex::MakePaletteFormat(false).channels0 == (2u | 0x20u));
    CHECK(tex::MakePaletteFormat(true).channels0 == (3u | 0x20u));
    CHECK(tex::MakePaletteFormat(true).bitsPerPixel == 8);
    // Table entries (FUN_0014A410).
    const ni::NiPixelFormat t5 = tex::XboxTableFormat(XboxFormat::A8R8G8B8);
    CHECK(t5.format == 1);
    CHECK(t5.redMask == 0xFF0000u);
    CHECK(t5.alphaMask == 0xFF000000u);
    CHECK(t5.bitsPerPixel == 32);
    CHECK(tex::XboxTableFormat(XboxFormat::L6V5U5).format == 9);
    CHECK(tex::XboxTableFormat(XboxFormat::V8U8).format == 8);
    CHECK(tex::XboxTableFormat(XboxFormat::None).channels0 == 0u);
    CHECK(tex::XboxTableFormat(XboxFormat::Count).format == 0u);
}

TEST_CASE("texconv: 16-bit RGB 565 / RGBA 4444 / 1555 / X1R5G5B5 to BGRA8") {
    tex::HostImage img;
    SUBCASE("R5G6B5 (105 of the 136 frontend textures)") {
        Synthetic s;
        s.format = tex::MakeRgbFormat(0xF800, 0x7E0, 0x1F, 0, 16);
        s.AddLevel(2, 2, Words16({0xF800, 0x07E0, 0x001F, 0x8410}));
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(img.format == Format::BGRA8_UNorm);
        CHECK(img.width == 2);
        CHECK(img.mipCount == 1);
        CHECK(img.mips[0].rowPitch == 8);
        CHECK(img.bytes.size() == 16);
        CHECK_FALSE(img.hasAlpha);
        CHECK(img.sourceFormat == 0);
        CHECK(img.sourceBpp == 16);
        CHECK(Texel(img, 0, 0) == Argb(255, 255, 0, 0));
        CHECK(Texel(img, 1, 0) == Argb(255, 0, 255, 0));
        CHECK(Texel(img, 0, 1) == Argb(255, 0, 0, 255));
        CHECK(Texel(img, 1, 1) == Argb(255, Exp(16, 5), Exp(32, 6), Exp(16, 5)));
        CHECK(Exp(16, 5) == 132);
        CHECK(Exp(32, 6) == 130);
    }
    SUBCASE("expansion is bit replication like xemu, not exact rounding") {
        Synthetic s;
        s.format = tex::MakeRgbFormat(0xF800, 0x7E0, 0x1F, 0, 16);
        s.AddLevel(1, 1, Words16({(3u << 11) | (48u << 5) | 1u}));
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == Argb(255, 24, 195, 8));  // rounding would give 25, 194, 8
    }
    SUBCASE("A4R4G4B4 (31 of the frontend textures)") {
        Synthetic s;
        s.format = tex::MakeRgbFormat(0xF00, 0xF0, 0xF, 0xF000, 16);
        s.AddLevel(2, 1, Words16({0xF123, 0x08F0}));
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(img.hasAlpha);
        CHECK(img.sourceFormat == 1);
        CHECK(Texel(img, 0, 0) == Argb(255, 0x11, 0x22, 0x33));
        CHECK(Texel(img, 1, 0) == Argb(0, 0x88, 0xFF, 0x00));
    }
    SUBCASE("A1R5G5B5") {
        Synthetic s;
        s.format = tex::MakeRgbFormat(0x7C00, 0x3E0, 0x1F, 0x8000, 16);
        s.AddLevel(2, 1, Words16({0xFC00, 0x03E0}));
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == Argb(255, 255, 0, 0));
        CHECK(Texel(img, 1, 0) == Argb(0, 0, 255, 0));
    }
    SUBCASE("X1R5G5B5: RGB ignores the top bit") {
        Synthetic s;
        s.format = tex::MakeRgbFormat(0x7C00, 0x3E0, 0x1F, 0, 16);
        s.AddLevel(1, 1, Words16({0x001F}));
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK_FALSE(img.hasAlpha);
        CHECK(Texel(img, 0, 0) == Argb(255, 0, 0, 255));
    }
    SUBCASE("RGB with an alpha mask stays opaque (D3D X formats)") {
        Synthetic s;
        s.format = tex::MakeRgbFormat(0x7C00, 0x3E0, 0x1F, 0, 16);
        s.format.alphaMask = 0x8000;
        s.AddLevel(1, 1, Words16({0x7FFF}));
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == Argb(255, 255, 255, 255));
    }
}

TEST_CASE("texconv: 24/32-bit RGB(A) through the masks") {
    tex::HostImage img;
    SUBCASE("A8R8G8B8") {
        Synthetic s;
        s.format = tex::MakeRgbFormat(0xFF0000, 0xFF00, 0xFF, 0xFF000000, 32);
        s.AddLevel(1, 1, Words32({0x80112233}));
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == 0x80112233u);
        CHECK(img.hasAlpha);
    }
    SUBCASE("X8R8G8B8 ignores the X byte") {
        Synthetic s;
        s.format = tex::MakeRgbFormat(0xFF0000, 0xFF00, 0xFF, 0, 32);
        s.AddLevel(1, 1, Words32({0x00112233}));
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == 0xFF112233u);
    }
    SUBCASE("A8B8G8R8 (newshade.nif layout)") {
        Synthetic s;
        s.format = tex::MakeRgbFormat(0xFF, 0xFF00, 0xFF0000, 0xFF000000, 32);
        s.AddLevel(1, 1, Words32({0x40332211}));  // bytes r=11 g=22 b=33 a=40
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == Argb(0x40, 0x11, 0x22, 0x33));
    }
    SUBCASE("24-bit RGB") {
        Synthetic s;
        s.format = tex::MakeRgbFormat(0xFF0000, 0xFF00, 0xFF, 0, 24);
        s.AddLevel(2, 1, {0x33, 0x22, 0x11, 0xCC, 0xBB, 0xAA});
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == Argb(255, 0x11, 0x22, 0x33));
        CHECK(Texel(img, 1, 0) == Argb(255, 0xAA, 0xBB, 0xCC));
    }
}

TEST_CASE("texconv: unsupported formats and bad data") {
    tex::HostImage img;
    Synthetic s;
    s.format = tex::MakeRgbFormat(0xE0, 0x1C, 0x3, 0, 8);
    s.AddLevel(1, 1, {0xFF});
    CHECK(Convert(s, &img) == ConvertStatus::Unsupported);  // FUN_000EDC70: 8 bpp -> -1
    CHECK(img.bytes.empty());
    s.format = tex::MakeBumpFormat(0xFF, 0xFF00, 0, 16);
    CHECK(Convert(s, &img) == ConvertStatus::Unsupported);
    s.format = tex::MakeBumpFormat(0x1F, 0x3E0, 0xFC00, 16);
    CHECK(Convert(s, &img) == ConvertStatus::Unsupported);
    s.format.format = 7;
    CHECK(Convert(s, &img) == ConvertStatus::Unsupported);
    s.format.format = 10;
    CHECK(Convert(s, &img) == ConvertStatus::Unsupported);
    s.format = tex::MakeRgbFormat(0, 0, 0, 0xFF, 32);
    CHECK(Convert(s, &img) == ConvertStatus::Unsupported);  // no color mask

    Synthetic ok;
    ok.format = tex::MakeRgbFormat(0xF800, 0x7E0, 0x1F, 0, 16);
    ok.AddLevel(2, 2, Words16({1, 2, 3, 4}));
    tex::PixelSource src = ok.Source();
    src.pixels = nullptr;
    CHECK(tex::ConvertPixels(src, {}, &img) == ConvertStatus::NoPixelData);
    src = ok.Source();
    src.mipCount = 0;
    CHECK(tex::ConvertPixels(src, {}, &img) == ConvertStatus::NoPixelData);
    src = ok.Source();
    src.mipCount = tex::kMaxMipLevels + 1;
    CHECK(tex::ConvertPixels(src, {}, &img) == ConvertStatus::BadData);
    CHECK(tex::ConvertPixels(ok.Source(), {}, nullptr) == ConvertStatus::BadData);

    Synthetic zero = ok;
    zero.widths[0] = 0;
    CHECK(Convert(zero, &img) == ConvertStatus::BadData);
    Synthetic huge = ok;
    huge.widths[0] = tex::kMaxDimension * 2;
    CHECK(Convert(huge, &img) == ConvertStatus::BadData);
    Synthetic shortLevel = ok;
    shortLevel.offsets[1] = 6;  // 2x2x2 = 8 bytes expected
    CHECK(Convert(shortLevel, &img) == ConvertStatus::BadData);
    Synthetic backwards = ok;
    backwards.offsets[0] = 9;
    CHECK(Convert(backwards, &img) == ConvertStatus::BadData);
}

TEST_CASE("texconv: mip chains are kept as stored, cut where they break") {
    Synthetic s;
    s.format = tex::MakeRgbFormat(0xF800, 0x7E0, 0x1F, 0, 16);
    s.AddLevel(4, 2, Words16({0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800}));
    s.AddLevel(2, 1, Words16({0x07E0, 0x07E0}));
    s.AddLevel(1, 1, Words16({0x001F}));
    tex::HostImage img;
    REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
    CHECK(img.mipCount == 3);
    CHECK(img.droppedMipLevels == 0);
    CHECK(img.mips[1].width == 2);
    CHECK(img.mips[1].height == 1);
    CHECK(img.mips[1].offset == 32);
    CHECK(img.mips[2].offset == 40);
    CHECK(img.bytes.size() == 44);
    CHECK(Texel(img, 3, 1, 0) == Argb(255, 255, 0, 0));
    CHECK(Texel(img, 1, 0, 1) == Argb(255, 0, 255, 0));
    CHECK(Texel(img, 0, 0, 2) == Argb(255, 0, 0, 255));

    SUBCASE("maxMipLevels") {
        tex::ConvertOptions o;
        o.maxMipLevels = 1;
        REQUIRE(Convert(s, &img, o) == ConvertStatus::Ok);
        CHECK(img.mipCount == 1);
        CHECK(img.droppedMipLevels == 2);
        CHECK(img.bytes.size() == 32);
        o.maxMipLevels = 0;  // treated as 1
        REQUIRE(Convert(s, &img, o) == ConvertStatus::Ok);
        CHECK(img.mipCount == 1);
    }
    SUBCASE("a level that does not halve ends the chain") {
        Synthetic t = s;
        t.widths[2] = 2;
        REQUIRE(Convert(t, &img) == ConvertStatus::Ok);
        CHECK(img.mipCount == 2);
        CHECK(img.droppedMipLevels == 1);
    }
    SUBCASE("a level with an inconsistent size ends the chain") {
        Synthetic t = s;
        t.offsets[2] = 35;
        REQUIRE(Convert(t, &img) == ConvertStatus::Ok);
        CHECK(img.mipCount == 1);
        CHECK(img.droppedMipLevels == 2);
    }
    SUBCASE("levels beyond the full chain are dropped") {
        Synthetic t = s;
        t.AddLevel(1, 1, Words16({0x001F}));  // 4x2 has 3 levels
        REQUIRE(Convert(t, &img) == ConvertStatus::Ok);
        CHECK(img.mipCount == 3);
        CHECK(img.droppedMipLevels == 1);
    }
    SUBCASE("RHI descriptors") {
        const gfx::rhi::TextureDesc d = tex::MakeTextureDesc(img, "t");
        CHECK(d.width == 4);
        CHECK(d.height == 2);
        CHECK(d.mipLevels == 3);
        CHECK(d.format == Format::BGRA8_UNorm);
        CHECK(d.usage == gfx::rhi::kTextureSampled);
        gfx::rhi::SubresourceData levels[tex::kMaxMipLevels];
        REQUIRE(tex::MakeSubresources(img, levels, tex::kMaxMipLevels) == 3);
        CHECK(levels[0].data == img.bytes.data());
        CHECK(levels[1].rowPitch == 8);
        CHECK(levels[2].data == img.bytes.data() + 40);
        CHECK(levels[2].slicePitch == 4);
        CHECK(tex::MakeSubresources(img, levels, 2) == 2);
        CHECK(tex::MakeSubresources(img, nullptr, 2) == 0);
    }
}

TEST_CASE("texconv: native 16-bit outputs (keep16Bit)") {
    tex::HostImage img;
    tex::ConvertOptions o;
    o.keep16Bit = tex::kKeepR5G6B5 | tex::kKeepA1R5G5B5 | tex::kKeepA4R4G4B4;
    Synthetic s;
    s.format = tex::MakeRgbFormat(0xF800, 0x7E0, 0x1F, 0, 16);
    s.AddLevel(2, 1, Words16({0x1234, 0xFFFF}));
    REQUIRE(Convert(s, &img, o) == ConvertStatus::Ok);
    CHECK(img.format == Format::B5G6R5_UNorm);
    CHECK(img.mips[0].rowPitch == 4);
    CHECK(img.bytes == Words16({0x1234, 0xFFFF}));

    s.format = tex::MakeRgbFormat(0x7C00, 0x3E0, 0x1F, 0, 16);  // X1R5G5B5 -> alpha bit forced
    REQUIRE(Convert(s, &img, o) == ConvertStatus::Ok);
    CHECK(img.format == Format::B5G5R5A1_UNorm);
    CHECK(img.bytes == Words16({0x9234, 0xFFFF}));

    s.format = tex::MakeRgbFormat(0xF00, 0xF0, 0xF, 0xF000, 16);
    REQUIRE(Convert(s, &img, o) == ConvertStatus::Ok);
    CHECK(img.format == Format::B4G4R4A4_UNorm);
    CHECK(img.bytes == Words16({0x1234, 0xFFFF}));

    o.keep16Bit = tex::kKeepR5G6B5;  // 4444 not allowed -> BGRA8
    REQUIRE(Convert(s, &img, o) == ConvertStatus::Ok);
    CHECK(img.format == Format::BGRA8_UNorm);
}

TEST_CASE("texconv: palettes (FUN_0015F1B0 byte order and alpha rule)") {
    Synthetic s;
    s.format = tex::MakePaletteFormat(false);
    s.palette.assign(1024, 0);
    const std::uint8_t e1[4] = {0x10, 0x20, 0x30, 0x40};  // r g b a
    const std::uint8_t e255[4] = {0xFF, 0x80, 0x01, 0x00};
    std::memcpy(&s.palette[4 * 1], e1, 4);
    std::memcpy(&s.palette[4 * 255], e255, 4);
    s.AddLevel(2, 1, {1, 255});
    tex::HostImage img;
    SUBCASE("palette without alpha -> 0xFF") {
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(img.format == Format::BGRA8_UNorm);
        CHECK_FALSE(img.hasAlpha);
        CHECK(Texel(img, 0, 0) == Argb(0xFF, 0x10, 0x20, 0x30));
        CHECK(Texel(img, 1, 0) == Argb(0xFF, 0xFF, 0x80, 0x01));
    }
    SUBCASE("palette with alpha -> entry alpha, also for a PAL (not PALA) format") {
        s.paletteHasAlpha = true;
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(img.hasAlpha);
        CHECK(Texel(img, 0, 0) == Argb(0x40, 0x10, 0x20, 0x30));
        CHECK(Texel(img, 1, 0) == Argb(0x00, 0xFF, 0x80, 0x01));
    }
    SUBCASE("PALA with a palette without alpha stays opaque") {
        s.format = tex::MakePaletteFormat(true);
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == Argb(0xFF, 0x10, 0x20, 0x30));
    }
    SUBCASE("no palette") {
        s.palette.clear();
        CHECK(Convert(s, &img) == ConvertStatus::NoPalette);
    }
    SUBCASE("palette formats need 8 bpp") {
        s.format.bitsPerPixel = 4;
        CHECK(Convert(s, &img) == ConvertStatus::Unsupported);
    }
}

namespace {

// DXT1 block: c0, c1 (565), 2-bit indices for texels 0..15 (texel i at bits 2i).
std::vector<std::uint8_t> ColorBlock(std::uint16_t c0, std::uint16_t c1, std::uint32_t indices) {
    std::vector<std::uint8_t> b = Words16({c0, c1});
    const std::vector<std::uint8_t> i = Words32({indices});
    b.insert(b.end(), i.begin(), i.end());
    return b;
}

}  // namespace

TEST_CASE("texconv: DXT1/3/5 pass through as BC1/2/3") {
    tex::HostImage img;
    for (int layout = 4; layout <= 6; ++layout) {
        CAPTURE(layout);
        Synthetic s;
        s.format = tex::MakeCompressedFormat(static_cast<ni::NiPixelLayout>(layout));
        const std::uint32_t blockBytes = layout == 4 ? 8 : 16;
        // 8x8, 4x4, 2x2 (one block each below 4), like the disc's chains that stop at 4 or go below.
        std::vector<std::uint8_t> l0(4 * blockBytes), l1(blockBytes), l2(blockBytes);
        for (std::size_t i = 0; i < l0.size(); ++i) {
            l0[i] = static_cast<std::uint8_t>(i * 7 + 1);
        }
        for (std::size_t i = 0; i < l1.size(); ++i) {
            l1[i] = static_cast<std::uint8_t>(0xA0 + i);
            l2[i] = static_cast<std::uint8_t>(0x50 + i);
        }
        s.AddLevel(8, 8, l0);
        s.AddLevel(4, 4, l1);
        s.AddLevel(2, 2, l2);
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(img.format == (layout == 4   ? Format::BC1_UNorm
                             : layout == 5 ? Format::BC2_UNorm
                                           : Format::BC3_UNorm));
        CHECK_FALSE(img.decompressed);
        CHECK(img.hasAlpha);
        CHECK(img.mipCount == 3);
        CHECK(img.mips[0].rowPitch == 2 * blockBytes);
        CHECK(img.mips[2].size == blockBytes);
        CHECK(img.bytes == s.pixels);  // bytes unchanged
    }
}

TEST_CASE("texconv: DXT decoding (level 0 not a multiple of 4, or keepBlockCompression = false)") {
    tex::HostImage img;
    tex::ConvertOptions decode;
    decode.keepBlockCompression = false;
    const std::uint32_t red = Argb(255, 255, 0, 0), blue = Argb(255, 0, 0, 255);
    SUBCASE("DXT1 four-color block") {
        Synthetic s;
        s.format = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT1);
        s.AddLevel(4, 4, ColorBlock(0xF800, 0x001F, 0xE4));  // texels 0..3 = indices 0, 1, 2, 3
        REQUIRE(Convert(s, &img, decode) == ConvertStatus::Ok);
        CHECK(img.format == Format::BGRA8_UNorm);
        CHECK(img.decompressed);
        CHECK(Texel(img, 0, 0) == red);
        CHECK(Texel(img, 1, 0) == blue);
        CHECK(Texel(img, 2, 0) == Argb(255, 170, 0, 85));
        CHECK(Texel(img, 3, 0) == Argb(255, 85, 0, 170));
        CHECK(Texel(img, 3, 3) == red);
    }
    SUBCASE("DXT1 three-color block with transparent black") {
        Synthetic s;
        s.format = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT1);
        s.AddLevel(4, 4, ColorBlock(0x001F, 0xF800, 0xE4));
        REQUIRE(Convert(s, &img, decode) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == blue);
        CHECK(Texel(img, 1, 0) == red);
        CHECK(Texel(img, 2, 0) == Argb(255, 128, 0, 128));
        CHECK(Texel(img, 3, 0) == 0u);
    }
    SUBCASE("DXT3 explicit alpha, color always four-color") {
        Synthetic s;
        s.format = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT3);
        std::vector<std::uint8_t> block = {0x0F, 0x08, 0, 0, 0, 0, 0, 0xF0};  // texel 0 F, 1 0, 2 8, 15 F
        const std::vector<std::uint8_t> color = ColorBlock(0x001F, 0xF800, 0xE4);
        block.insert(block.end(), color.begin(), color.end());
        s.AddLevel(4, 4, block);
        REQUIRE(Convert(s, &img, decode) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == (blue | 0xFF000000u));
        CHECK(Texel(img, 1, 0) == (red & 0x00FFFFFFu));
        CHECK(Texel(img, 2, 0) == Argb(136, 85, 0, 170));
        CHECK(Texel(img, 3, 0) == Argb(0, 170, 0, 85));
        CHECK(Texel(img, 3, 3) == (blue | 0xFF000000u));
    }
    SUBCASE("DXT5 interpolated alpha, 8 and 6 value modes") {
        Synthetic s;
        s.format = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT5);
        // texel 0 -> index 0, 1 -> 1, 2 -> 2, 3 -> 7 (bits 0x0E88)
        std::vector<std::uint8_t> block8 = {200, 100, 0x88, 0x0E, 0, 0, 0, 0};
        std::vector<std::uint8_t> block6 = {50, 150, 0x88, 0x0E, 0, 0, 0, 0};  // a0 <= a1: 6-value mode
        const std::vector<std::uint8_t> color = ColorBlock(0xF800, 0xF800, 0);
        block8.insert(block8.end(), color.begin(), color.end());
        block6.insert(block6.end(), color.begin(), color.end());
        // 8x4 texture = two blocks side by side
        std::vector<std::uint8_t> level = block8;
        level.insert(level.end(), block6.begin(), block6.end());
        s.AddLevel(8, 4, level);
        REQUIRE(Convert(s, &img, decode) == ConvertStatus::Ok);
        CHECK(AlphaOf(img, 0, 0) == 200u);
        CHECK(AlphaOf(img, 1, 0) == 100u);
        CHECK(AlphaOf(img, 2, 0) == 186u);
        CHECK(AlphaOf(img, 3, 0) == 114u);
        CHECK((Texel(img, 0, 0) & 0xFFFFFFu) == 0xFF0000u);
        CHECK(AlphaOf(img, 4, 0) == 50u);
        CHECK(AlphaOf(img, 5, 0) == 150u);
        CHECK(AlphaOf(img, 6, 0) == 70u);   // (4 * 50 + 150 + 2) / 5
        CHECK(AlphaOf(img, 7, 0) == 255u);  // index 7
        CHECK(AlphaOf(img, 4, 1) == 50u);
    }
    SUBCASE("a 2x2 DXT1 texture is decoded even with keepBlockCompression") {
        Synthetic s;
        s.format = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT1);
        s.AddLevel(2, 2, ColorBlock(0xF800, 0x001F, 0x00000110));  // (0,0)=0 (1,0)=0 (2,0)=1 -> clipped
        s.AddLevel(1, 1, ColorBlock(0x001F, 0x001F, 0));
        REQUIRE(Convert(s, &img) == ConvertStatus::Ok);
        CHECK(img.format == Format::BGRA8_UNorm);
        CHECK(img.decompressed);
        CHECK(img.mipCount == 2);
        CHECK(img.bytes.size() == 2 * 2 * 4 + 4);
        CHECK(Texel(img, 0, 0) == red);
        CHECK(Texel(img, 1, 0) == red);
        CHECK(Texel(img, 0, 1) == blue);  // texel 4 = index 1
        CHECK(Texel(img, 0, 0, 1) == blue);
    }
    SUBCASE("block level sizes are checked") {
        Synthetic s;
        s.format = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT3);
        s.AddLevel(4, 4, std::vector<std::uint8_t>(8));  // DXT3 needs 16 bytes
        CHECK(Convert(s, &img) == ConvertStatus::BadData);
    }
}

TEST_CASE("texconv: guest NiPixelData (ni_guest_layouts.h) and NiPalette") {
    // The test process is 32-bit like the guest: host pointers are guest addresses.
    std::vector<std::uint8_t> pixels = {3, 0, 1, 2};
    std::uint32_t widths[2] = {2, 1}, heights[2] = {2, 1}, offsets[3] = {0, 4, 5};
    pixels.push_back(1);
    std::vector<std::uint8_t> entries(1024, 0);
    for (int i = 0; i < 4; ++i) {
        entries[4 * i + 0] = static_cast<std::uint8_t>(0x10 * i);
        entries[4 * i + 1] = 0x01;
        entries[4 * i + 2] = 0x02;
        entries[4 * i + 3] = static_cast<std::uint8_t>(0x40 + i);
    }
    ni::NiPalette palette{};
    palette.entries = guest::Addr(entries.data());
    palette.hasAlpha = 1;
    palette.revision = 1;
    ni::NiPixelData pd{};
    pd.format = tex::MakePaletteFormat(true);
    pd.palette = guest::Addr(&palette);
    pd.pixels = guest::Addr(pixels.data());
    pd.widths = guest::Addr(widths);
    pd.heights = guest::Addr(heights);
    pd.offsets = guest::Addr(offsets);
    pd.mipCount = 2;
    pd.bytesPerPixel = 1;
    pd.revision = 1;
    const ni::GuestPtr addr = guest::Addr(&pd);

    tex::HostImage img;
    REQUIRE(tex::ConvertPixelData(addr, 0, {}, &img) == ConvertStatus::Ok);
    CHECK(img.mipCount == 2);
    CHECK(img.sourceFormat == 3);
    CHECK(Texel(img, 0, 0) == Argb(0x43, 0x30, 0x01, 0x02));
    CHECK(Texel(img, 1, 0) == Argb(0x40, 0x00, 0x01, 0x02));
    CHECK(Texel(img, 0, 0, 1) == Argb(0x41, 0x10, 0x01, 0x02));

    SUBCASE("palette argument overrides NiPixelData +0x2C") {
        ni::NiPalette opaque = palette;
        opaque.hasAlpha = 0;
        REQUIRE(tex::ConvertPixelData(addr, guest::Addr(&opaque), {}, &img) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == Argb(0xFF, 0x30, 0x01, 0x02));
    }
    SUBCASE("missing palette entries") {
        palette.entries = 0;
        CHECK(tex::ConvertPixelData(addr, 0, {}, &img) == ConvertStatus::NoPalette);
        pd.palette = 0;
        CHECK(tex::ConvertPixelData(addr, 0, {}, &img) == ConvertStatus::NoPalette);
    }
    SUBCASE("16-bit guest image") {
        const std::vector<std::uint8_t> words = Words16({0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000});
        pixels = words;
        pd.pixels = guest::Addr(pixels.data());
        pd.format = tex::MakeRgbFormat(0xF800, 0x7E0, 0x1F, 0, 16);
        pd.palette = 0;
        pd.bytesPerPixel = 2;
        offsets[1] = 8;
        offsets[2] = 10;
        REQUIRE(tex::ConvertPixelData(addr, 0, {}, &img) == ConvertStatus::Ok);
        CHECK(Texel(img, 0, 0) == Argb(255, 255, 0, 0));
        CHECK(Texel(img, 1, 1) == 0xFFFFFFFFu);
        CHECK(Texel(img, 0, 0, 1) == Argb(255, 0, 0, 0));
        pd.bytesPerPixel = 4;  // NiPixelData +0x44 must match bpp / 8
        CHECK(tex::ConvertPixelData(addr, 0, {}, &img) == ConvertStatus::BadData);
    }
    SUBCASE("no pixel data") {
        CHECK(tex::ConvertPixelData(0, 0, {}, &img) == ConvertStatus::NoPixelData);
        pd.pixels = 0;
        CHECK(tex::ConvertPixelData(addr, 0, {}, &img) == ConvertStatus::NoPixelData);
        pd.pixels = guest::Addr(pixels.data());
        pd.mipCount = 0;
        CHECK(tex::ConvertPixelData(addr, 0, {}, &img) == ConvertStatus::NoPixelData);
    }
    SUBCASE("unreadable or inconsistent guest data") {
        CHECK(tex::ConvertPixelData(0x10, 0, {}, &img) ==
              ConvertStatus::BadData);  // first page: never mapped
        pd.mipCount = tex::kMaxMipLevels + 1;
        CHECK(tex::ConvertPixelData(addr, 0, {}, &img) == ConvertStatus::BadData);
        pd.mipCount = 2;
        pd.offsets = 0x10;
        CHECK(tex::ConvertPixelData(addr, 0, {}, &img) == ConvertStatus::BadData);
        CHECK(img.bytes.empty());
        CHECK(img.format == Format::Unknown);
    }
}

TEST_CASE("texconv: default texture (FUN_0014AB50)") {
    tex::HostImage img;
    tex::MakeDefaultTexture(&img);
    CHECK(img.format == Format::BGRA8_UNorm);
    CHECK(img.width == 8);
    CHECK(img.height == 8);
    CHECK(img.mipCount == 1);
    CHECK(img.mips[0].rowPitch == 32);
    REQUIRE(img.bytes.size() == 256);
    CHECK(img.hasAlpha);
    int red = 0, blue = 0, yellow = 0, zero = 0;
    for (std::uint32_t i = 0; i < 64; ++i) {
        const std::uint32_t t = Texel(img, i % 8, i / 8);
        red += t == 0xFFFF0000u;
        blue += t == 0xFF0000FFu;
        yellow += t == 0xFFFFFF00u;
        zero += t == 0u;
    }
    CHECK(red == 8);
    CHECK(blue == 11);
    CHECK(yellow == 6);
    CHECK(zero == 64 - 25);
    CHECK(Texel(img, 0, 3) == 0xFFFF0000u);  // dword 0x18
    CHECK(Texel(img, 5, 1) == 0xFF0000FFu);  // dword 0x0D
    CHECK(Texel(img, 6, 6) == 0xFFFFFF00u);  // dword 0x36
    // bytes B, G, R, A of dword 0x18 (red)
    CHECK(img.bytes[0x18 * 4 + 0] == 0x00);
    CHECK(img.bytes[0x18 * 4 + 2] == 0xFF);
    CHECK(img.bytes[0x18 * 4 + 3] == 0xFF);
    tex::MakeDefaultTexture(nullptr);  // no crash
}

TEST_CASE("texconv: Xbox format choice FUN_001572D0") {
    using P = tex::XboxFormatPrefs;
    const ni::NiPixelFormat rgb565 = tex::MakeRgbFormat(0xF800, 0x7E0, 0x1F, 0, 16);
    const ni::NiPixelFormat rgb555 = tex::MakeRgbFormat(0x7C00, 0x3E0, 0x1F, 0, 16);
    const ni::NiPixelFormat rgb888 = tex::MakeRgbFormat(0xFF0000, 0xFF00, 0xFF, 0, 32);
    const ni::NiPixelFormat rgb24 = tex::MakeRgbFormat(0xFF0000, 0xFF00, 0xFF, 0, 24);
    const ni::NiPixelFormat argb4444 = tex::MakeRgbFormat(0xF00, 0xF0, 0xF, 0xF000, 16);
    const ni::NiPixelFormat argb1555 = tex::MakeRgbFormat(0x7C00, 0x3E0, 0x1F, 0x8000, 16);
    const ni::NiPixelFormat argb8888 = tex::MakeRgbFormat(0xFF0000, 0xFF00, 0xFF, 0xFF000000, 32);
    const ni::NiPixelFormat abgr8888 = tex::MakeRgbFormat(0xFF, 0xFF00, 0xFF0000, 0xFF000000, 32);
    const ni::NiPixelFormat dxt1 = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT1);
    const ni::NiPixelFormat dxt3 = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT3);
    const ni::NiPixelFormat dxt5 = tex::MakeCompressedFormat(ni::NiPixelLayout::DXT5);
    const ni::NiPixelFormat pal = tex::MakePaletteFormat(false);
    const ni::NiPixelFormat pala = tex::MakePaletteFormat(true);

    // The combinations on the disc (scan_pixeldata.py).
    CHECK(tex::ChooseXboxFormat(dxt1, P{3, 1, 3}) == XboxFormat::DXT1);
    CHECK(tex::ChooseXboxFormat(dxt1, P{3, 2, 3}) == XboxFormat::DXT1);
    CHECK(tex::ChooseXboxFormat(dxt3, P{3, 1, 3}) == XboxFormat::DXT3);
    CHECK(tex::ChooseXboxFormat(argb4444, P{1, 1, 3}) == XboxFormat::A4R4G4B4);
    CHECK(tex::ChooseXboxFormat(argb4444, P{1, 2, 3}) == XboxFormat::A4R4G4B4);
    CHECK(tex::ChooseXboxFormat(argb4444, P{1, 0, 3}) == XboxFormat::A4R4G4B4);
    CHECK(tex::ChooseXboxFormat(rgb565, P{1, 1, 3}) == XboxFormat::R5G6B5);
    CHECK(tex::ChooseXboxFormat(rgb565, P{1, 2, 3}) == XboxFormat::R5G6B5);
    CHECK(tex::ChooseXboxFormat(rgb565, P{1, 0, 3}) == XboxFormat::R5G6B5);
    CHECK(tex::ChooseXboxFormat(abgr8888, P{5, 2, 3}) == XboxFormat::None);  // newshade.nif -> default

    // Other layouts and prefs.
    CHECK(tex::ChooseXboxFormat(rgb555, P{}) == XboxFormat::X1R5G5B5);
    CHECK(tex::ChooseXboxFormat(rgb888, P{2, 2, 3}) == XboxFormat::X8R8G8B8);
    CHECK(tex::ChooseXboxFormat(rgb24, P{}) == XboxFormat::None);  // no 24-bit table entry
    CHECK(tex::ChooseXboxFormat(argb1555, P{1, 2, 1}) == XboxFormat::A1R5G5B5);
    CHECK(tex::ChooseXboxFormat(argb1555, P{5, 2, 3}) == XboxFormat::A1R5G5B5);
    CHECK(tex::ChooseXboxFormat(argb8888, P{}) == XboxFormat::A8R8G8B8);
    CHECK(tex::ChooseXboxFormat(argb8888, P{1, 2, 3}) == XboxFormat::A8R8G8B8);
    CHECK(tex::ChooseXboxFormat(dxt5, P{}) == XboxFormat::DXT5);
    CHECK(tex::ChooseXboxFormat(dxt1, P{0, 2, 3}) == XboxFormat::DXT1);
    CHECK(tex::ChooseXboxFormat(dxt1, P{1, 2, 3}) == XboxFormat::None);    // HIGH_COLOR_16: only 3/4/5 tried
    CHECK(tex::ChooseXboxFormat(dxt1, P{2, 2, 3}) == XboxFormat::None);    // TRUE_COLOR_32
    CHECK(tex::ChooseXboxFormat(rgb565, P{4, 2, 3}) == XboxFormat::None);  // BUMPMAP
    // Palettes: only unmipmapped (MIP_DEFAULT with DAT_0030EF84 = 0, or NO) with PIX_DEFAULT, or
    // PALETTIZED_8.
    CHECK(tex::ChooseXboxFormat(pal, P{5, 2, 3}) == XboxFormat::P8);
    CHECK(tex::ChooseXboxFormat(pal, P{5, 0, 1}) == XboxFormat::P8);
    CHECK(tex::ChooseXboxFormat(pala, P{5, 0, 2}) == XboxFormat::P8Alpha);
    CHECK(tex::ChooseXboxFormat(pala, P{5, 0, 3}) == XboxFormat::None);  // alpha default: only P8 tried
    CHECK(tex::ChooseXboxFormat(pal, P{5, 1, 3}) == XboxFormat::None);   // mipmapped palette
    CHECK(tex::ChooseXboxFormat(pal, P{0, 1, 3}) == XboxFormat::P8);
    CHECK(tex::ChooseXboxFormat(pal, P{1, 2, 3}) == XboxFormat::None);
    // Bump maps.
    CHECK(tex::ChooseXboxFormat(tex::MakeBumpFormat(0xFF, 0xFF00, 0, 16), P{4, 2, 3}) == XboxFormat::V8U8);
    CHECK(tex::ChooseXboxFormat(tex::MakeBumpFormat(0x1F, 0x3E0, 0xFC00, 16), P{4, 2, 3}) ==
          XboxFormat::L6V5U5);
    CHECK(tex::ChooseXboxFormat(tex::MakeBumpFormat(0xFF, 0xFF00, 0xFF0000, 32), P{4, 2, 3}) ==
          XboxFormat::X8L8V8U8);
    CHECK(tex::ChooseXboxFormat(tex::MakeBumpFormat(0xFF, 0xFF00, 0, 16), P{}) == XboxFormat::None);
    // Every table entry chooses itself under a fitting preference.
    for (int i = 0; i < static_cast<int>(XboxFormat::Count); ++i) {
        CAPTURE(i);
        const ni::NiPixelFormat f = tex::XboxTableFormat(static_cast<XboxFormat>(i));
        const std::uint32_t layout = i >= 11 ? 4u : (i == 6 || i == 7) ? 0u : 5u;
        const std::uint32_t alpha = i == 7 ? 2u : (i == 3 ? 1u : 3u);
        CHECK(tex::ChooseXboxFormat(f, P{layout, 0, alpha}) == static_cast<XboxFormat>(i));
        CHECK(std::strcmp(tex::XboxFormatName(static_cast<XboxFormat>(i)), "?") != 0);
    }
    CHECK(std::strcmp(tex::XboxFormatName(XboxFormat::None), "none") == 0);
}

TEST_CASE("texconv: FormatPrefs read from a guest NiSourceTexture") {
    ni::NiSourceTextureLayout st{};
    const std::uint32_t prefs[3] = {1, 0, 2};
    std::memcpy(reinterpret_cast<std::uint8_t*>(&st) + tex::kNiSourceTextureFormatPrefs, prefs,
                sizeof(prefs));
    const tex::XboxFormatPrefs p = tex::ReadFormatPrefs(guest::Addr(&st));
    CHECK(p.pixelLayout == 1);
    CHECK(p.mipMapped == 0);
    CHECK(p.alphaFormat == 2);
    const tex::XboxFormatPrefs d = tex::ReadFormatPrefs(0);
    CHECK(d.pixelLayout == 5);
    CHECK(d.mipMapped == 2);
    CHECK(d.alphaFormat == 3);
}

TEST_CASE("texconv: slot 65 source decision (FUN_0015E010)") {
    using tex::TextureSource;
    tex::TextureSourceInput in;
    CHECK(tex::SelectTextureSource(in) == TextureSource::DefaultNoImage);
    in.pixelData = 0x1000;
    in.width = 64;
    in.height = 32;
    CHECK(tex::SelectTextureSource(in) == TextureSource::PixelData);
    in.width = 48;
    CHECK(tex::SelectTextureSource(in) == TextureSource::DefaultNotPowerOfTwo);
    in.width = 0;
    CHECK(tex::SelectTextureSource(in) == TextureSource::DefaultNotPowerOfTwo);
    in.width = 64;
    in.height = 1;
    CHECK(tex::SelectTextureSource(in) == TextureSource::PixelData);

    in.hasPath = true;  // the file wins even if +0x34 is set
    CHECK(tex::SelectTextureSource(in) == TextureSource::DefaultUnreadable);
    in.fileImage = 0x2000;
    CHECK(tex::SelectTextureSource(in) == TextureSource::FileImage);
    in.loadTextures = false;
    CHECK(tex::SelectTextureSource(in) == TextureSource::DefaultNotLoaded);
    in.loadTextures = true;

    const ni::NiPixelFormat abgr = tex::MakeRgbFormat(0xFF, 0xFF00, 0xFF0000, 0xFF000000, 32);
    const ni::NiPixelFormat rgb565 = tex::MakeRgbFormat(0xF800, 0x7E0, 0x1F, 0, 16);
    in.imageFormat = &abgr;
    in.prefs = tex::XboxFormatPrefs{5, 2, 3};
    CHECK(tex::SelectTextureSource(in) == TextureSource::DefaultNoXboxFormat);
    in.imageFormat = &rgb565;
    CHECK(tex::SelectTextureSource(in) == TextureSource::FileImage);
    in.width = 3;  // power-of-two test first, like the original
    in.imageFormat = &abgr;
    CHECK(tex::SelectTextureSource(in) == TextureSource::DefaultNotPowerOfTwo);

    CHECK(tex::IsPowerOfTwo(1));
    CHECK(tex::IsPowerOfTwo(0x80000000u));
    CHECK_FALSE(tex::IsPowerOfTwo(0));
    CHECK_FALSE(tex::IsPowerOfTwo(6));
    for (int s = 0; s <= static_cast<int>(TextureSource::DefaultNoXboxFormat); ++s) {
        CHECK(std::strcmp(tex::TextureSourceName(static_cast<TextureSource>(s)), "?") != 0);
    }
    for (int s = 0; s <= static_cast<int>(ConvertStatus::OutOfMemory); ++s) {
        CHECK(std::strcmp(tex::ConvertStatusName(static_cast<ConvertStatus>(s)), "?") != 0);
    }
}
