// NiPixelData -> host image (texconv.h; GFX_CONTRACT.md 7). Evidence for every rule is in the header comment.
#include "gfx/texconv.h"

#include "core/guest.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>

namespace gfx::tex {
namespace {

// ---- packed channel descriptor (FUN_00140910 and its tables in .data) -----------------------------------
constexpr std::uint32_t kCountMask[4] = {0x3F00, 0xFC000, 0x3F00000, 0xFC000000};  // DAT_002D7B68..0x2D7B74
constexpr std::uint32_t kCountShift[4] = {8, 14, 20, 26};                          // DAT_002D7B78..0x2D7B84
constexpr std::uint32_t kShiftMask[4] = {0x1F, 0x3E0, 0x7C00, 0xF8000};            // DAT_002D7B88..0x2D7B94
constexpr std::uint32_t kShiftShift[4] = {0, 5, 10, 15};                           // DAT_002D7B98..0x2D7BA4
constexpr std::uint32_t kFormatFieldMask = 0x1F;  // DAT_002D7B5C (shifted by DAT_0030E8AC = 0)
constexpr std::uint32_t kBytesFieldMask = 0xE0;   // DAT_002D7B60
constexpr std::uint32_t kBytesFieldShift = 5;     // DAT_002D7B64
// DAT_0030EF84: global "mipmap by default" flag of FUN_001572D0 / FUN_0015E010 (BSS, no writer in the XBE).
constexpr bool kGlobalMipmapDefault = false;

struct MaskInfo {
    std::uint32_t shift = 0;  // trailing zero bits
    std::uint32_t bits = 0;   // consecutive one bits after them
};

constexpr MaskInfo AnalyzeMask(std::uint32_t mask) noexcept {
    MaskInfo info;
    if (mask == 0) {
        return info;
    }
    while (((mask >> info.shift) & 1u) == 0) {
        ++info.shift;
    }
    std::uint32_t m = mask >> info.shift;
    while ((m & 1u) != 0) {
        ++info.bits;
        m >>= 1;
    }
    return info;
}

constexpr void AddChannel(ni::NiPixelFormat& f, std::uint32_t mask, int channel) noexcept {
    const MaskInfo info = AnalyzeMask(mask);
    f.channels0 |= (info.bits << kCountShift[channel]) & kCountMask[channel];
    f.channels1 |= (info.shift << kShiftShift[channel]) & kShiftMask[channel];
}

// Bytes-per-pixel field: FUN_001409A0 / FUN_00140C30 set 2, 3 or 4 for 16, 24, 32 bpp (nothing otherwise).
constexpr void AddBytesField(ni::NiPixelFormat& f, std::uint32_t bitsPerPixel) noexcept {
    std::uint32_t bytes = 0;
    if (bitsPerPixel == 16) {
        bytes = 2;
    } else if (bitsPerPixel == 24) {
        bytes = 3;
    } else if (bitsPerPixel == 32) {
        bytes = 4;
    }
    f.channels0 |= (bytes << kBytesFieldShift) & kBytesFieldMask;
}

constexpr ni::NiPixelFormat BuildRgb(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a,
                                     std::uint32_t bpp) noexcept {
    ni::NiPixelFormat f{};
    f.redMask = r;
    f.greenMask = g;
    f.blueMask = b;
    f.alphaMask = a;
    f.bitsPerPixel = static_cast<std::uint8_t>(bpp);
    AddChannel(f, r, 0);
    AddChannel(f, g, 1);
    AddChannel(f, b, 2);
    if (a != 0) {
        AddChannel(f, a, 3);
    }
    f.format = a != 0 ? static_cast<std::uint32_t>(ni::NiPixelLayout::RGBA)
                      : static_cast<std::uint32_t>(ni::NiPixelLayout::RGB);
    f.channels0 |= f.format & kFormatFieldMask;
    AddBytesField(f, bpp);
    return f;
}

constexpr ni::NiPixelFormat BuildPalette(bool alpha) noexcept {
    ni::NiPixelFormat f{};
    f.format = alpha ? static_cast<std::uint32_t>(ni::NiPixelLayout::PALA)
                     : static_cast<std::uint32_t>(ni::NiPixelLayout::PAL);
    f.bitsPerPixel = 8;
    f.channels0 = (f.format & kFormatFieldMask) | ((1u << kBytesFieldShift) & kBytesFieldMask);
    return f;
}

constexpr ni::NiPixelFormat BuildCompressed(std::uint32_t format) noexcept {
    ni::NiPixelFormat f{};
    f.format = format;
    f.channels0 = format & kFormatFieldMask;
    return f;
}

constexpr ni::NiPixelFormat BuildBump(std::uint32_t u, std::uint32_t v, std::uint32_t luma,
                                      std::uint32_t bpp) noexcept {
    ni::NiPixelFormat f{};
    f.redMask = u;
    f.greenMask = v;
    f.alphaMask = luma;
    f.bitsPerPixel = static_cast<std::uint8_t>(bpp);
    AddChannel(f, u, 0);
    AddChannel(f, v, 1);
    if (luma != 0) {
        AddChannel(f, luma, 3);
    }
    f.format = luma != 0 ? static_cast<std::uint32_t>(ni::NiPixelLayout::BUMPLUMA)
                         : static_cast<std::uint32_t>(ni::NiPixelLayout::BUMP);
    f.channels0 |= f.format & kFormatFieldMask;
    AddBytesField(f, bpp);
    return f;
}

// Renderer format table +0x528 (FUN_0014A410 cases 0..13).
constexpr ni::NiPixelFormat kXboxTable[] = {
    BuildRgb(0x7C00, 0x3E0, 0x1F, 0, 16),              // 0 X1R5G5B5
    BuildRgb(0xF800, 0x7E0, 0x1F, 0, 16),              // 1 R5G6B5
    BuildRgb(0xFF0000, 0xFF00, 0xFF, 0, 32),           // 2 X8R8G8B8
    BuildRgb(0x7C00, 0x3E0, 0x1F, 0x8000, 16),         // 3 A1R5G5B5
    BuildRgb(0xF00, 0xF0, 0xF, 0xF000, 16),            // 4 A4R4G4B4
    BuildRgb(0xFF0000, 0xFF00, 0xFF, 0xFF000000, 32),  // 5 A8R8G8B8
    BuildPalette(false),                               // 6 P8
    BuildPalette(true),                                // 7 P8 + alpha
    BuildCompressed(4),                                // 8 DXT1
    BuildCompressed(5),                                // 9 DXT3
    BuildCompressed(6),                                // 10 DXT5
    BuildBump(0xFF, 0xFF00, 0, 16),                    // 11 V8U8
    BuildBump(0x1F, 0x3E0, 0xFC00, 16),                // 12 L6V5U5
    BuildBump(0xFF, 0xFF00, 0xFF0000, 32),             // 13 X8L8V8U8
};
static_assert(std::size(kXboxTable) == static_cast<std::size_t>(XboxFormat::Count));
// Cross-checks against descriptors found in the disc's NIF files (run\m3-texconv\pixeldata.tsv).
static_assert(kXboxTable[1].channels0 == 0x518540 && kXboxTable[1].channels1 == 0xAB);
static_assert(kXboxTable[4].channels0 == 0x10410441 && kXboxTable[4].channels1 == 0x60088);
static_assert(BuildRgb(0xFF, 0xFF00, 0xFF0000, 0xFF000000, 32).channels0 == 0x20820881 &&
              BuildRgb(0xFF, 0xFF00, 0xFF0000, 0xFF000000, 32).channels1 == 0xC4100);
static_assert(kXboxTable[8].channels0 == 4 && kXboxTable[9].channels0 == 5);

// ---- level geometry --------------------------------------------------------------------------------------

enum class Kind : std::uint8_t { Packed, Palette, Dxt1, Dxt3, Dxt5 };

std::uint32_t SourceLevelSize(Kind kind, std::uint32_t bytesPerPixel, std::uint32_t w,
                              std::uint32_t h) noexcept {
    const std::uint64_t blocks = static_cast<std::uint64_t>((w + 3) / 4) * ((h + 3) / 4);
    std::uint64_t size = 0;
    switch (kind) {
    case Kind::Packed:
        size = static_cast<std::uint64_t>(w) * h * bytesPerPixel;
        break;
    case Kind::Palette:
        size = static_cast<std::uint64_t>(w) * h;
        break;
    case Kind::Dxt1:
        size = blocks * 8;  // FUN_00111260: ((w+3)&~3) * ((h+3)&~3) / 16 blocks of 8 (DXT1) or 16 bytes
        break;
    case Kind::Dxt3:
    case Kind::Dxt5:
        size = blocks * 16;
        break;
    }
    return size > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(size);
}

// ---- channel expansion -----------------------------------------------------------------------------------

// n-bit channel value (1 <= n <= 8) -> 8 bits by repeating its bit pattern (565: r << 3 | r >> 2,
// g << 2 | g >> 4; 4444: v * 17). Measured: xemu's NV2A texture path does this - in the flat areas of
// ref_tech_notice.png / ref_legal.png all 376 texels where it differs from round(v * 255 / (2^n - 1)) match
// the replication (run\m3-texconv\cmp_xemu.py); exact UNORM would be off by 1 there.
constexpr std::uint8_t Replicate(std::uint32_t v, std::uint32_t bits) noexcept {
    std::uint32_t result = 0;
    for (int shift = 8 - static_cast<int>(bits);; shift -= static_cast<int>(bits)) {
        result |= shift >= 0 ? v << shift : v >> -shift;
        if (shift <= 0) {
            break;
        }
    }
    return static_cast<std::uint8_t>(result);
}
static_assert(Replicate(31, 5) == 255 && Replicate(16, 5) == 132 && Replicate(48, 6) == 195 &&
              Replicate(1, 1) == 255 && Replicate(2, 2) == 170 && Replicate(5, 3) == 182 &&
              Replicate(9, 4) == 153 && Replicate(0x41, 7) == 0x83 && Replicate(0xA5, 8) == 0xA5);

struct ChannelDecoder {
    std::uint32_t shift = 0;
    std::uint32_t bits = 0;  // 0 = constant
    std::uint32_t rawMask = 0;
    std::uint8_t constant = 0;
    std::uint8_t lut[256] = {};  // bits 1..8: Replicate(v, bits)

    void Init(std::uint32_t mask, std::uint8_t constantValue) noexcept {
        const MaskInfo info = AnalyzeMask(mask);
        shift = info.shift;
        bits = info.bits;
        constant = constantValue;
        rawMask = bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        if (bits >= 1 && bits <= 8) {
            const std::uint32_t max = (1u << bits) - 1u;
            for (std::uint32_t v = 0; v <= max; ++v) {
                lut[v] = Replicate(v, bits);
            }
        }
    }
    std::uint8_t Decode(std::uint32_t texel) const noexcept {
        if (bits == 0) {
            return constant;
        }
        const std::uint32_t raw = (texel >> shift) & rawMask;
        if (bits > 8) {
            return static_cast<std::uint8_t>(raw >> (bits - 8));
        }
        return lut[raw];
    }
};

inline std::uint32_t ReadTexel(const std::uint8_t* p, std::uint32_t bytes) noexcept {
    switch (bytes) {
    case 2: {
        std::uint16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }
    case 3:
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16);
    default: {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    }
}

struct PackedDecoder {
    ChannelDecoder r, g, b, a;
    std::uint32_t bytesPerPixel = 0;
};

void ConvertPackedLevel(const PackedDecoder& d, const std::uint8_t* src, std::uint32_t w, std::uint32_t h,
                        std::uint8_t* dst, std::uint32_t dstPitch) noexcept {
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* s = src + static_cast<std::size_t>(y) * w * d.bytesPerPixel;
        std::uint8_t* o = dst + static_cast<std::size_t>(y) * dstPitch;
        for (std::uint32_t x = 0; x < w; ++x, s += d.bytesPerPixel, o += 4) {
            const std::uint32_t t = ReadTexel(s, d.bytesPerPixel);
            o[0] = d.b.Decode(t);
            o[1] = d.g.Decode(t);
            o[2] = d.r.Decode(t);
            o[3] = d.a.Decode(t);
        }
    }
}

void Copy16Level(const std::uint8_t* src, std::uint32_t w, std::uint32_t h, std::uint16_t orMask,
                 std::uint8_t* dst, std::uint32_t dstPitch) noexcept {
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* s = src + static_cast<std::size_t>(y) * w * 2;
        std::uint8_t* o = dst + static_cast<std::size_t>(y) * dstPitch;
        if (orMask == 0) {
            std::memcpy(o, s, static_cast<std::size_t>(w) * 2);
            continue;
        }
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint16_t v;
            std::memcpy(&v, s + 2 * x, 2);
            v = static_cast<std::uint16_t>(v | orMask);
            std::memcpy(o + 2 * x, &v, 2);
        }
    }
}

// FUN_0015F1B0: entry bytes r, g, b, a -> D3DCOLOR a r g b (a = 0xFF without NiPalette +0x1C) -> BGRA8 bytes.
void ConvertPaletteLevel(const std::uint8_t* src, std::uint32_t w, std::uint32_t h,
                         const std::uint8_t* palette, bool paletteHasAlpha, std::uint8_t* dst,
                         std::uint32_t dstPitch) noexcept {
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* s = src + static_cast<std::size_t>(y) * w;
        std::uint8_t* o = dst + static_cast<std::size_t>(y) * dstPitch;
        for (std::uint32_t x = 0; x < w; ++x, o += 4) {
            const std::uint8_t* e = palette + 4u * s[x];
            o[0] = e[2];
            o[1] = e[1];
            o[2] = e[0];
            o[3] = paletteHasAlpha ? e[3] : 0xFF;
        }
    }
}

// ---- S3TC (DXT1/3/5 = BC1/2/3) decoding -------------------------------------------------------------------

// Decodes the 8-byte color part into 16 BGRA texels (row-major, texel i = y * 4 + x at index bits 2i).
// allowThreeColor: DXT1 semantics (c0 <= c1 -> 3 colors + transparent black); DXT3/5 always use 4 colors.
// Endpoints expand like the 16-bit formats (Replicate); interpolation rounds to nearest.
void DecodeColorBlock(const std::uint8_t* block, bool allowThreeColor, std::uint8_t out[16][4]) noexcept {
    std::uint16_t c0, c1;
    std::uint32_t indices;
    std::memcpy(&c0, block, 2);
    std::memcpy(&c1, block + 2, 2);
    std::memcpy(&indices, block + 4, 4);
    std::uint8_t pal[4][4];  // b, g, r, a
    const auto unpack = [](std::uint16_t c, std::uint8_t* p) {
        p[0] = Replicate(c & 0x1Fu, 5);
        p[1] = Replicate((c >> 5) & 0x3Fu, 6);
        p[2] = Replicate((c >> 11) & 0x1Fu, 5);
        p[3] = 0xFF;
    };
    unpack(c0, pal[0]);
    unpack(c1, pal[1]);
    if (!allowThreeColor || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<std::uint8_t>((2u * pal[0][k] + pal[1][k] + 1u) / 3u);
            pal[3][k] = static_cast<std::uint8_t>((pal[0][k] + 2u * pal[1][k] + 1u) / 3u);
        }
        pal[2][3] = 0xFF;
        pal[3][3] = 0xFF;
    } else {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<std::uint8_t>((pal[0][k] + pal[1][k] + 1u) / 2u);
            pal[3][k] = 0;
        }
        pal[2][3] = 0xFF;
        pal[3][3] = 0;  // transparent black
    }
    for (int i = 0; i < 16; ++i) {
        std::memcpy(out[i], pal[(indices >> (2 * i)) & 3u], 4);
    }
}

void DecodeExplicitAlpha(const std::uint8_t* block, std::uint8_t out[16][4]) noexcept {
    for (int i = 0; i < 16; ++i) {
        const std::uint32_t nibble = (block[i / 2] >> (4 * (i & 1))) & 0xFu;
        out[i][3] = static_cast<std::uint8_t>(nibble * 17u);
    }
}

void DecodeInterpolatedAlpha(const std::uint8_t* block, std::uint8_t out[16][4]) noexcept {
    const std::uint32_t a0 = block[0], a1 = block[1];
    std::uint8_t pal[8];
    pal[0] = static_cast<std::uint8_t>(a0);
    pal[1] = static_cast<std::uint8_t>(a1);
    if (a0 > a1) {
        for (std::uint32_t k = 1; k <= 6; ++k) {
            pal[1 + k] = static_cast<std::uint8_t>(((7u - k) * a0 + k * a1 + 3u) / 7u);
        }
    } else {
        for (std::uint32_t k = 1; k <= 4; ++k) {
            pal[1 + k] = static_cast<std::uint8_t>(((5u - k) * a0 + k * a1 + 2u) / 5u);
        }
        pal[6] = 0;
        pal[7] = 0xFF;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= static_cast<std::uint64_t>(block[2 + i]) << (8 * i);
    }
    for (int i = 0; i < 16; ++i) {
        out[i][3] = pal[(bits >> (3 * i)) & 7u];
    }
}

void DecodeDxtLevel(Kind kind, const std::uint8_t* src, std::uint32_t w, std::uint32_t h, std::uint8_t* dst,
                    std::uint32_t dstPitch) noexcept {
    const std::uint32_t blockBytes = kind == Kind::Dxt1 ? 8 : 16;
    const std::uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::uint8_t texels[16][4];
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            const std::uint8_t* block = src + (static_cast<std::size_t>(by) * bw + bx) * blockBytes;
            if (kind == Kind::Dxt1) {
                DecodeColorBlock(block, true, texels);
            } else {
                DecodeColorBlock(block + 8, false, texels);
                if (kind == Kind::Dxt3) {
                    DecodeExplicitAlpha(block, texels);
                } else {
                    DecodeInterpolatedAlpha(block, texels);
                }
            }
            for (std::uint32_t y = 0; y < 4 && by * 4 + y < h; ++y) {
                for (std::uint32_t x = 0; x < 4 && bx * 4 + x < w; ++x) {
                    std::memcpy(dst + static_cast<std::size_t>(by * 4 + y) * dstPitch + (bx * 4 + x) * 4,
                                texels[y * 4 + x], 4);
                }
            }
        }
    }
}

// ---- format analysis --------------------------------------------------------------------------------------

struct Plan {
    Kind kind = Kind::Packed;
    rhi::Format hostFormat = rhi::Format::Unknown;
    std::uint32_t bytesPerPixel = 0;  // source (packed / palette)
    std::uint16_t orMask16 = 0;       // native 16-bit copy: bits forced to 1 (X1R5G5B5 alpha)
    bool hasAlpha = false;
    bool decompress = false;
};

bool MasksAre(const ni::NiPixelFormat& f, std::uint32_t r, std::uint32_t g, std::uint32_t b) noexcept {
    return f.redMask == r && f.greenMask == g && f.blueMask == b;
}

ConvertStatus MakePlan(const PixelSource& source, const ConvertOptions& options, Plan* plan) noexcept {
    const ni::NiPixelFormat& f = source.format;
    const std::uint32_t bpp = f.bitsPerPixel;
    switch (static_cast<ni::NiPixelLayout>(f.format)) {
    case ni::NiPixelLayout::RGB:
    case ni::NiPixelLayout::RGBA: {
        // FUN_000EDC70: 8 bpp -> -1; 16/24/32 have D3D formats.
        if (bpp != 16 && bpp != 24 && bpp != 32) {
            return ConvertStatus::Unsupported;
        }
        if ((f.redMask | f.greenMask | f.blueMask) == 0) {
            return ConvertStatus::Unsupported;
        }
        if (bpp < 32 && ((f.redMask | f.greenMask | f.blueMask | f.alphaMask) >> bpp) != 0) {
            return ConvertStatus::Unsupported;  // masks outside the texel
        }
        const bool rgba = f.format == static_cast<std::uint32_t>(ni::NiPixelLayout::RGBA);
        plan->kind = Kind::Packed;
        plan->bytesPerPixel = bpp / 8;
        plan->hasAlpha = rgba && f.alphaMask != 0;
        plan->hostFormat = rhi::Format::BGRA8_UNorm;
        if (bpp == 16) {
            const std::uint32_t alpha = rgba ? f.alphaMask : 0;
            if ((options.keep16Bit & kKeepR5G6B5) != 0 && MasksAre(f, 0xF800, 0x7E0, 0x1F) && alpha == 0) {
                plan->hostFormat = rhi::Format::B5G6R5_UNorm;
            } else if ((options.keep16Bit & kKeepA1R5G5B5) != 0 && MasksAre(f, 0x7C00, 0x3E0, 0x1F) &&
                       (alpha == 0 || alpha == 0x8000)) {
                plan->hostFormat = rhi::Format::B5G5R5A1_UNorm;
                plan->orMask16 = alpha == 0 ? 0x8000 : 0;  // X1R5G5B5: alpha 1
            } else if ((options.keep16Bit & kKeepA4R4G4B4) != 0 && MasksAre(f, 0xF00, 0xF0, 0xF) &&
                       (alpha == 0 || alpha == 0xF000)) {
                plan->hostFormat = rhi::Format::B4G4R4A4_UNorm;
                plan->orMask16 = alpha == 0 ? 0xF000 : 0;
            }
        }
        return ConvertStatus::Ok;
    }
    case ni::NiPixelLayout::PAL:
    case ni::NiPixelLayout::PALA:
        if (bpp != 8) {
            return ConvertStatus::Unsupported;  // FUN_001408A0 always builds 8 bpp
        }
        if (source.paletteRgba == nullptr) {
            return ConvertStatus::NoPalette;
        }
        plan->kind = Kind::Palette;
        plan->bytesPerPixel = 1;
        plan->hasAlpha = source.paletteHasAlpha;
        plan->hostFormat = rhi::Format::BGRA8_UNorm;
        return ConvertStatus::Ok;
    case ni::NiPixelLayout::DXT1:
    case ni::NiPixelLayout::DXT3:
    case ni::NiPixelLayout::DXT5: {
        const auto layout = static_cast<ni::NiPixelLayout>(f.format);
        plan->kind = layout == ni::NiPixelLayout::DXT1   ? Kind::Dxt1
                     : layout == ni::NiPixelLayout::DXT3 ? Kind::Dxt3
                                                         : Kind::Dxt5;
        plan->hasAlpha = true;
        const std::uint32_t w0 = source.widths[0], h0 = source.heights[0];
        plan->decompress = !options.keepBlockCompression || (w0 % 4) != 0 || (h0 % 4) != 0;
        if (plan->decompress) {
            plan->hostFormat = rhi::Format::BGRA8_UNorm;
        } else {
            plan->hostFormat = plan->kind == Kind::Dxt1   ? rhi::Format::BC1_UNorm
                               : plan->kind == Kind::Dxt3 ? rhi::Format::BC2_UNorm
                                                          : rhi::Format::BC3_UNorm;
        }
        return ConvertStatus::Ok;
    }
    default:
        return ConvertStatus::Unsupported;  // BUMP, BUMPLUMA, 7, > 9
    }
}

XboxFormat ToXboxFormat(int index) noexcept {
    return index < 0 ? XboxFormat::None : static_cast<XboxFormat>(index);
}

}  // namespace

// ---- names ------------------------------------------------------------------------------------------------

const char* ConvertStatusName(ConvertStatus status) noexcept {
    switch (status) {
    case ConvertStatus::Ok:
        return "ok";
    case ConvertStatus::NoPixelData:
        return "no pixel data";
    case ConvertStatus::Unsupported:
        return "unsupported format";
    case ConvertStatus::NoPalette:
        return "no palette";
    case ConvertStatus::BadData:
        return "bad data";
    case ConvertStatus::OutOfMemory:
        return "out of memory";
    }
    return "?";
}

const char* XboxFormatName(XboxFormat format) noexcept {
    switch (format) {
    case XboxFormat::None:
        return "none";
    case XboxFormat::X1R5G5B5:
        return "X1R5G5B5";
    case XboxFormat::R5G6B5:
        return "R5G6B5";
    case XboxFormat::X8R8G8B8:
        return "X8R8G8B8";
    case XboxFormat::A1R5G5B5:
        return "A1R5G5B5";
    case XboxFormat::A4R4G4B4:
        return "A4R4G4B4";
    case XboxFormat::A8R8G8B8:
        return "A8R8G8B8";
    case XboxFormat::P8:
        return "P8";
    case XboxFormat::P8Alpha:
        return "P8 (alpha)";
    case XboxFormat::DXT1:
        return "DXT1";
    case XboxFormat::DXT3:
        return "DXT3";
    case XboxFormat::DXT5:
        return "DXT5";
    case XboxFormat::V8U8:
        return "V8U8";
    case XboxFormat::L6V5U5:
        return "L6V5U5";
    case XboxFormat::X8L8V8U8:
        return "X8L8V8U8";
    case XboxFormat::Count:
        break;
    }
    return "?";
}

const char* TextureSourceName(TextureSource source) noexcept {
    switch (source) {
    case TextureSource::FileImage:
        return "file";
    case TextureSource::PixelData:
        return "pixel data";
    case TextureSource::DefaultNoImage:
        return "default (no image)";
    case TextureSource::DefaultUnreadable:
        return "default (unreadable file)";
    case TextureSource::DefaultNotLoaded:
        return "default (not loaded)";
    case TextureSource::DefaultNotPowerOfTwo:
        return "default (not a power of two)";
    case TextureSource::DefaultUnsupported:
        return "default (unsupported format)";
    case TextureSource::DefaultNoXboxFormat:
        return "default (no Xbox format)";
    }
    return "?";
}

// ---- conversion -------------------------------------------------------------------------------------------

ConvertStatus ConvertPixels(const PixelSource& source, const ConvertOptions& options,
                            HostImage* out) noexcept {
    if (out == nullptr) {
        return ConvertStatus::BadData;
    }
    *out = HostImage{};
    if (source.pixels == nullptr || source.mipCount == 0 || source.widths == nullptr ||
        source.heights == nullptr || source.offsets == nullptr) {
        return ConvertStatus::NoPixelData;
    }
    if (source.mipCount > kMaxMipLevels) {
        return ConvertStatus::BadData;
    }
    const std::uint32_t w0 = source.widths[0], h0 = source.heights[0];
    if (w0 == 0 || h0 == 0 || w0 > kMaxDimension || h0 > kMaxDimension) {
        return ConvertStatus::BadData;
    }
    Plan plan;
    if (const ConvertStatus s = MakePlan(source, options, &plan); s != ConvertStatus::Ok) {
        return s;
    }

    // Levels to keep: the stored chain (FUN_0015D920 creates exactly NiPixelData +0x40 levels), cut at the
    // option limit, at the full chain length and at the first level that does not halve or whose stored
    // size is inconsistent (level 0 inconsistent -> BadData).
    std::uint32_t levels = std::min(source.mipCount, std::max<std::uint32_t>(1, options.maxMipLevels));
    levels = std::min<std::uint32_t>(levels, static_cast<std::uint32_t>(std::bit_width(std::max(w0, h0))));
    for (std::uint32_t i = 0; i < levels; ++i) {
        const std::uint32_t w = source.widths[i], h = source.heights[i];
        const bool chainOk =
            w == std::max<std::uint32_t>(1, w0 >> i) && h == std::max<std::uint32_t>(1, h0 >> i);
        const std::uint32_t begin = source.offsets[i], end = source.offsets[i + 1];
        const bool sizeOk =
            end >= begin && end - begin == SourceLevelSize(plan.kind, plan.bytesPerPixel, w, h);
        if (!chainOk || !sizeOk) {
            if (i == 0) {
                return ConvertStatus::BadData;
            }
            levels = i;
            break;
        }
    }

    out->format = plan.hostFormat;
    out->width = w0;
    out->height = h0;
    out->mipCount = levels;
    out->hasAlpha = plan.hasAlpha;
    out->sourceFormat = source.format.format;
    out->sourceBpp = source.format.bitsPerPixel;
    out->droppedMipLevels = source.mipCount - levels;
    out->decompressed = plan.decompress;
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < levels; ++i) {
        MipLevel& m = out->mips[i];
        m.width = source.widths[i];
        m.height = source.heights[i];
        m.rowPitch = rhi::RowPitch(plan.hostFormat, m.width);
        m.size = rhi::LevelSize(plan.hostFormat, m.width, m.height);
        m.offset = static_cast<std::uint32_t>(total);
        total += m.size;
    }
    if (total > 0x7FFFFFFFull) {
        *out = HostImage{};
        return ConvertStatus::BadData;
    }
    try {
        out->bytes.resize(static_cast<std::size_t>(total));
    } catch (const std::bad_alloc&) {
        *out = HostImage{};
        return ConvertStatus::OutOfMemory;
    } catch (...) {
        *out = HostImage{};
        return ConvertStatus::OutOfMemory;
    }

    PackedDecoder packed;
    if (plan.kind == Kind::Packed && plan.orMask16 == 0 && plan.hostFormat == rhi::Format::BGRA8_UNorm) {
        const bool rgba = source.format.format == static_cast<std::uint32_t>(ni::NiPixelLayout::RGBA);
        packed.bytesPerPixel = plan.bytesPerPixel;
        packed.r.Init(source.format.redMask, 0);
        packed.g.Init(source.format.greenMask, 0);
        packed.b.Init(source.format.blueMask, 0);
        packed.a.Init(rgba ? source.format.alphaMask : 0, 0xFF);  // RGB (X8 / X1 / no alpha) -> opaque
    }
    const bool native16 = plan.kind == Kind::Packed && plan.hostFormat != rhi::Format::BGRA8_UNorm;
    for (std::uint32_t i = 0; i < levels; ++i) {
        const MipLevel& m = out->mips[i];
        const std::uint8_t* src = source.pixels + source.offsets[i];
        std::uint8_t* dst = out->bytes.data() + m.offset;
        switch (plan.kind) {
        case Kind::Packed:
            if (native16) {
                Copy16Level(src, m.width, m.height, plan.orMask16, dst, m.rowPitch);
            } else {
                ConvertPackedLevel(packed, src, m.width, m.height, dst, m.rowPitch);
            }
            break;
        case Kind::Palette:
            ConvertPaletteLevel(src, m.width, m.height, source.paletteRgba, source.paletteHasAlpha, dst,
                                m.rowPitch);
            break;
        case Kind::Dxt1:
        case Kind::Dxt3:
        case Kind::Dxt5:
            if (plan.decompress) {
                DecodeDxtLevel(plan.kind, src, m.width, m.height, dst, m.rowPitch);
            } else {
                std::memcpy(dst, src, m.size);  // NiPixelData level = rows of 4x4 blocks = BCn layout
            }
            break;
        }
    }
    return ConvertStatus::Ok;
}

// Guest fields read here (ni_guest_layouts.h, re-verified by the texconv agent in the original):
//   NiSourceTexture +0x30 file name (0x15E05B), +0x34 NiPixelData (0x15E07D), +0x14 FormatPrefs (pushed at
//     0x15E0CE, +0x18 read at 0x15E13B)
//   NiPixelData +0x0C NiPixelFormat (0x15E0CF lea = FUN_001572D0 argument; 0x15DCCC reads the format),
//     +0x2C NiPalette (0x15DBBA, 0x15E18F), +0x30 pixels (0x15DBD6, 0x15DCDF), +0x34 widths (0x15E087,
//     0x15DB9F), +0x38 heights (0x15E0A5), +0x3C offsets[mipCount + 1] (0x15DBD0, 0x15E19D),
//     +0x40 mipCount (0x15DB1E, 0x15E19A), +0x44 bytes per pixel (0x15DBA5 imul = row pitch),
//     +0x48 revision (0x15E0F0)
//   NiPalette +0x18 entries (0x15F1B6), +0x1C has alpha (0x15F1E0), +0x10 revision (0x15F2DB)
//   NiPixelFormat +0x00 format / +0x14 bpp byte / +0x18 descriptor (FUN_000EDC70), +0x04..+0x10 masks and
//     +0x18/+0x1C descriptor (FUN_001409A0), descriptor compare (vf3 0x151488/0x151490)
ConvertStatus ConvertPixelData(ni::GuestPtr pixelData, ni::GuestPtr palette, const ConvertOptions& options,
                               HostImage* out) noexcept {
    if (out == nullptr) {
        return ConvertStatus::BadData;
    }
    *out = HostImage{};
    if (pixelData == 0) {
        return ConvertStatus::NoPixelData;
    }
    if (!guest::IsReadable(pixelData, sizeof(ni::NiPixelData))) {
        return ConvertStatus::BadData;
    }
    ni::NiPixelData pd;
    std::memcpy(&pd, guest::Ptr<const void>(pixelData), sizeof(pd));
    if (pd.pixels == 0 || pd.mipCount == 0) {
        return ConvertStatus::NoPixelData;
    }
    if (pd.mipCount > kMaxMipLevels || pd.widths == 0 || pd.heights == 0 || pd.offsets == 0) {
        return ConvertStatus::BadData;
    }
    const std::uint32_t n = pd.mipCount;
    std::uint32_t widths[kMaxMipLevels], heights[kMaxMipLevels], offsets[kMaxMipLevels + 1];
    if (!guest::IsReadable(pd.widths, 4 * n) || !guest::IsReadable(pd.heights, 4 * n) ||
        !guest::IsReadable(pd.offsets, 4 * (n + 1))) {
        return ConvertStatus::BadData;
    }
    std::memcpy(widths, guest::Ptr<const void>(pd.widths), 4 * n);
    std::memcpy(heights, guest::Ptr<const void>(pd.heights), 4 * n);
    std::memcpy(offsets, guest::Ptr<const void>(pd.offsets), 4 * (n + 1));
    // FUN_0015DB00 uses +0x44 as the row pitch factor; it is bpp / 8 for every uncompressed layout.
    if (pd.format.format <= static_cast<std::uint32_t>(ni::NiPixelLayout::PALA) &&
        pd.bytesPerPixel != pd.format.bitsPerPixel / 8u) {
        return ConvertStatus::BadData;
    }
    const std::uint32_t total = offsets[n];
    if (total == 0 || !guest::IsReadable(pd.pixels, total)) {
        return ConvertStatus::BadData;
    }

    PixelSource source;
    source.format = pd.format;
    source.pixels = guest::Ptr<const std::uint8_t>(pd.pixels);
    source.mipCount = n;
    source.widths = widths;
    source.heights = heights;
    source.offsets = offsets;
    const ni::GuestPtr pal = palette != 0 ? palette : pd.palette;
    if (pal != 0 && guest::IsReadable(pal, sizeof(ni::NiPalette))) {
        ni::NiPalette p;
        std::memcpy(&p, guest::Ptr<const void>(pal), sizeof(p));
        if (p.entries != 0 && guest::IsReadable(p.entries, 256 * 4)) {
            source.paletteRgba = guest::Ptr<const std::uint8_t>(p.entries);
            source.paletteHasAlpha = p.hasAlpha != 0;
        }
    }
    return ConvertPixels(source, options, out);
}

rhi::TextureDesc MakeTextureDesc(const HostImage& image, const char* debugName) noexcept {
    rhi::TextureDesc desc;
    desc.width = image.width;
    desc.height = image.height;
    desc.mipLevels = image.mipCount;
    desc.format = image.format;
    desc.usage = rhi::kTextureSampled;
    desc.debugName = debugName;
    return desc;
}

std::uint32_t MakeSubresources(const HostImage& image, rhi::SubresourceData* levels,
                               std::uint32_t capacity) noexcept {
    if (levels == nullptr) {
        return 0;
    }
    const std::uint32_t n = std::min({image.mipCount, capacity, kMaxMipLevels});
    for (std::uint32_t i = 0; i < n; ++i) {
        const MipLevel& m = image.mips[i];
        if (static_cast<std::size_t>(m.offset) + m.size > image.bytes.size()) {
            return i;
        }
        levels[i].data = image.bytes.data() + m.offset;
        levels[i].rowPitch = m.rowPitch;
        levels[i].slicePitch = m.size;
    }
    return n;
}

void MakeDefaultTexture(HostImage* out) noexcept {
    if (out == nullptr) {
        return;
    }
    *out = HostImage{};
    // FUN_0014AB50 (0x14ABE8..): dword index -> A8R8G8B8 value, everything else 0.
    constexpr std::uint32_t kRed = 0xFFFF0000u, kBlue = 0xFF0000FFu, kYellow = 0xFFFFFF00u;
    constexpr std::uint8_t kRedAt[] = {0x18, 0x20, 0x21, 0x22, 0x28, 0x2A, 0x30, 0x32};
    constexpr std::uint8_t kBlueAt[] = {0x0D, 0x15, 0x1D, 0x23, 0x24, 0x25, 0x2B, 0x2D, 0x33, 0x34, 0x35};
    constexpr std::uint8_t kYellowAt[] = {0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x36};
    std::uint32_t texels[64] = {};
    for (std::uint8_t i : kRedAt) {
        texels[i] = kRed;
    }
    for (std::uint8_t i : kBlueAt) {
        texels[i] = kBlue;
    }
    for (std::uint8_t i : kYellowAt) {
        texels[i] = kYellow;
    }
    try {
        out->bytes.resize(sizeof(texels));
    } catch (...) {
        *out = HostImage{};
        return;
    }
    std::memcpy(out->bytes.data(), texels, sizeof(texels));  // little-endian dword = bytes B, G, R, A
    out->format = rhi::Format::BGRA8_UNorm;
    out->width = 8;
    out->height = 8;
    out->mipCount = 1;
    out->mips[0] = MipLevel{8, 8, 32, 256, 0};
    out->hasAlpha = true;  // the 0x00000000 texels
    out->sourceFormat = static_cast<std::uint32_t>(ni::NiPixelLayout::RGBA);
    out->sourceBpp = 32;
}

// ---- Xbox format choice -----------------------------------------------------------------------------------

ni::NiPixelFormat MakeRgbFormat(std::uint32_t redMask, std::uint32_t greenMask, std::uint32_t blueMask,
                                std::uint32_t alphaMask, std::uint32_t bitsPerPixel) noexcept {
    return BuildRgb(redMask, greenMask, blueMask, alphaMask, bitsPerPixel);
}

ni::NiPixelFormat MakePaletteFormat(bool alpha) noexcept {
    return BuildPalette(alpha);
}

ni::NiPixelFormat MakeCompressedFormat(ni::NiPixelLayout layout) noexcept {
    return BuildCompressed(static_cast<std::uint32_t>(layout));
}

ni::NiPixelFormat MakeBumpFormat(std::uint32_t uMask, std::uint32_t vMask, std::uint32_t lumaMask,
                                 std::uint32_t bitsPerPixel) noexcept {
    return BuildBump(uMask, vMask, lumaMask, bitsPerPixel);
}

ni::NiPixelFormat XboxTableFormat(XboxFormat format) noexcept {
    const auto i = static_cast<int>(format);
    if (i < 0 || i >= static_cast<int>(XboxFormat::Count)) {
        return ni::NiPixelFormat{};
    }
    return kXboxTable[i];
}

XboxFormatPrefs ReadFormatPrefs(ni::GuestPtr sourceTexture) noexcept {
    XboxFormatPrefs prefs;
    const ni::GuestPtr p = sourceTexture + kNiSourceTextureFormatPrefs;
    if (sourceTexture == 0 || !guest::IsReadable(p, 12)) {
        return prefs;
    }
    prefs.pixelLayout = guest::Read<std::uint32_t>(p);
    prefs.mipMapped = guest::Read<std::uint32_t>(p + 4);
    prefs.alphaFormat = guest::Read<std::uint32_t>(p + 8);
    return prefs;
}

// FUN_001572D0 (decompiled control flow; entry i "matches" if the image converter's CanConvert vf3 0x151480
// accepts it: equal descriptor dwords +0x18 and +0x1C).
XboxFormat ChooseXboxFormat(const ni::NiPixelFormat& source, const XboxFormatPrefs& prefs) noexcept {
    const auto matches = [&source](int i) {
        return kXboxTable[i].channels0 == source.channels0 && kXboxTable[i].channels1 == source.channels1;
    };
    const auto first = [&matches](std::initializer_list<int> order) {
        for (int i : order) {
            if (matches(i)) {
                return i;
            }
        }
        return -1;
    };
    const std::uint32_t layout = prefs.pixelLayout, alpha = prefs.alphaFormat, format = source.format;
    const auto rgbOrder = [&]() {  // switchD_0015739a_caseD_0 + LAB_0015762a
        return layout == 1 ? first({1, 0, 2, 5, 3, 4}) : first({2, 5, 1, 0, 3, 4});
    };
    if (layout == 4) {  // BUMPMAP
        return ToXboxFormat(format == 9 || format == 1 ? first({13, 12, 11}) : first({11, 13, 12}));
    }
    switch (format) {
    case 0:  // RGB
        return ToXboxFormat(rgbOrder());
    case 1:  // RGBA
        break;
    case 2:  // PAL
    case 3:  // PALA
    {
        const bool notMipmapped = (prefs.mipMapped == 2 && !kGlobalMipmapDefault) || prefs.mipMapped == 0;
        if ((layout == 5 && notMipmapped) || layout == 0) {
            if ((alpha == 2 || alpha == 1) && matches(7)) {
                return XboxFormat::P8Alpha;
            }
            if (matches(6)) {
                return XboxFormat::P8;
            }
        }
        return ToXboxFormat(rgbOrder());
    }
    case 4:  // DXT1
    case 5:  // DXT3
    case 6:  // DXT5
        if (layout != 1 && layout != 2 && matches(static_cast<int>(format) + 4)) {
            return ToXboxFormat(static_cast<int>(format) + 4);
        }
        break;
    default:
        return XboxFormat::None;
    }
    // After the switch (RGBA, DXT): 16-bit with alpha first for HIGH_COLOR_16 (LAB_001575A2), else 32 bit.
    if (layout == 1) {
        return ToXboxFormat(alpha == 1 ? first({3, 4, 5}) : first({4, 5, 3}));
    }
    return ToXboxFormat(alpha == 1 ? first({5, 3, 4}) : first({5, 4, 3}));
}

// ---- slot 65 decision -------------------------------------------------------------------------------------

bool IsPowerOfTwo(std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

TextureSource SelectTextureSource(const TextureSourceInput& input) noexcept {
    TextureSource chosen;
    if (input.hasPath) {
        if (!input.loadTextures) {
            return TextureSource::DefaultNotLoaded;
        }
        if (input.fileImage == 0) {
            return TextureSource::DefaultUnreadable;
        }
        chosen = TextureSource::FileImage;
    } else {
        if (input.pixelData == 0) {
            return TextureSource::DefaultNoImage;
        }
        chosen = TextureSource::PixelData;
    }
    if (!IsPowerOfTwo(input.width) || !IsPowerOfTwo(input.height)) {
        return TextureSource::DefaultNotPowerOfTwo;
    }
    if (input.imageFormat != nullptr &&
        ChooseXboxFormat(*input.imageFormat, input.prefs) == XboxFormat::None) {
        return TextureSource::DefaultNoXboxFormat;
    }
    return chosen;
}

// ---- M6: YUY2 movie textures -------------------------------------------------------------------------------

void YuvToRgbBt601(std::int32_t y, std::int32_t u, std::int32_t v, std::uint8_t& r, std::uint8_t& g,
                   std::uint8_t& b) noexcept {
    const std::int32_t c = 298 * (y - 16) + 128;
    const std::int32_t d = u - 128;
    const std::int32_t e = v - 128;
    auto clamp = [](std::int32_t x) noexcept {
        x >>= 8;
        return static_cast<std::uint8_t>(x < 0 ? 0 : x > 255 ? 255 : x);
    };
    r = clamp(c + 409 * e);
    g = clamp(c - 100 * d - 208 * e);
    b = clamp(c + 516 * d);
}

void ConvertYuy2ToBgra(const std::uint8_t* src, std::uint32_t srcPitch, std::uint32_t width, std::uint32_t height,
                       std::uint8_t* dst, std::uint32_t dstPitch, Yuy2Mode mode) noexcept {
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint8_t* s = src + static_cast<std::size_t>(row) * srcPitch;
        std::uint8_t* o = dst + static_cast<std::size_t>(row) * dstPitch;
        for (std::uint32_t x = 0; x + 1 < width; x += 2, s += 4, o += 8) {
            const std::int32_t y0 = s[0], u = s[1], y1 = s[2], v = s[3];
            if (mode == Yuy2Mode::ConvertBt601) {
                YuvToRgbBt601(y0, u, v, o[2], o[1], o[0]);
                YuvToRgbBt601(y1, u, v, o[6], o[5], o[4]);
            } else {
                o[0] = static_cast<std::uint8_t>(u);
                o[1] = static_cast<std::uint8_t>(y0);
                o[2] = static_cast<std::uint8_t>(v);
                o[4] = static_cast<std::uint8_t>(u);
                o[5] = static_cast<std::uint8_t>(y1);
                o[6] = static_cast<std::uint8_t>(v);
            }
            o[3] = 0xFF;
            o[7] = 0xFF;
        }
    }
}

}  // namespace gfx::tex
