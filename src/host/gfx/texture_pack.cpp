// G2: texture replacement packs and the texture dump (see texture_pack.h).
#include "gfx/texture_pack.h"

#include "core/guest.h"
#include "core/log.h"
#include "gfx/gfx_config.h"
#include "gfx/nipc_gpu.h"
#include "gfx/nipc_renderer.h"
#include "platform/screenshot.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gfx::texpack {
namespace {

// ---- XXH64 ---------------------------------------------------------------------------------------------------------

constexpr std::uint64_t kP1 = 0x9E3779B185EBCA87ull;
constexpr std::uint64_t kP2 = 0xC2B2AE3D27D4EB4Full;
constexpr std::uint64_t kP3 = 0x165667B19E3779F9ull;
constexpr std::uint64_t kP4 = 0x85EBCA77C2B2AE63ull;
constexpr std::uint64_t kP5 = 0x27D4EB2F165667C5ull;

constexpr std::uint64_t Rotl(std::uint64_t v, int r) noexcept {
    return (v << r) | (v >> (64 - r));
}
std::uint64_t Read64(const std::uint8_t* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;  // x86: little-endian like the reference implementation's XXH_readLE64
}
std::uint32_t Read32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
constexpr std::uint64_t Round(std::uint64_t acc, std::uint64_t input) noexcept {
    return Rotl(acc + input * kP2, 31) * kP1;
}
constexpr std::uint64_t MergeRound(std::uint64_t acc, std::uint64_t value) noexcept {
    return (acc ^ Round(0, value)) * kP1 + kP4;
}

}  // namespace

Xxh64::Xxh64(std::uint64_t seed) noexcept : v_{seed + kP1 + kP2, seed + kP2, seed, seed - kP1}, seed_(seed) {}

void Xxh64::Update(const void* data, std::size_t size) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    total_ += size;
    if (buffered_ + size < 32) {
        if (size != 0) {
            std::memcpy(buffer_ + buffered_, p, size);
        }
        buffered_ += static_cast<std::uint32_t>(size);
        return;
    }
    if (buffered_ != 0) {
        const std::size_t fill = 32 - buffered_;
        std::memcpy(buffer_ + buffered_, p, fill);
        for (int i = 0; i < 4; ++i) {
            v_[i] = Round(v_[i], Read64(buffer_ + 8 * i));
        }
        p += fill;
        size -= fill;
        buffered_ = 0;
    }
    while (size >= 32) {
        for (int i = 0; i < 4; ++i) {
            v_[i] = Round(v_[i], Read64(p + 8 * i));
        }
        p += 32;
        size -= 32;
    }
    if (size != 0) {
        std::memcpy(buffer_, p, size);
        buffered_ = static_cast<std::uint32_t>(size);
    }
}

std::uint64_t Xxh64::Digest() const noexcept {
    std::uint64_t h;
    if (total_ >= 32) {
        h = Rotl(v_[0], 1) + Rotl(v_[1], 7) + Rotl(v_[2], 12) + Rotl(v_[3], 18);
        for (int i = 0; i < 4; ++i) {
            h = MergeRound(h, v_[i]);
        }
    } else {
        h = seed_ + kP5;
    }
    h += total_;
    const std::uint8_t* p = buffer_;
    std::uint32_t left = buffered_;
    while (left >= 8) {
        h = Rotl(h ^ Round(0, Read64(p)), 27) * kP1 + kP4;
        p += 8;
        left -= 8;
    }
    if (left >= 4) {
        h = Rotl(h ^ (static_cast<std::uint64_t>(Read32(p)) * kP1), 23) * kP2 + kP3;
        p += 4;
        left -= 4;
    }
    while (left > 0) {
        h = Rotl(h ^ (*p * kP5), 11) * kP1;
        ++p;
        --left;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

std::uint64_t Xxh64Hash(const void* data, std::size_t size, std::uint64_t seed) noexcept {
    Xxh64 h(seed);
    h.Update(data, size);
    return h.Digest();
}

std::uint64_t TextureHash(const HashSource& s) noexcept {
    const std::uint32_t header[10] = {kHashTag,           s.format.format,  s.format.bitsPerPixel, s.width,
                                      s.height,           s.format.redMask, s.format.greenMask,    s.format.blueMask,
                                      s.format.alphaMask, s.level0Size};
    Xxh64 h;
    h.Update(header, sizeof(header));
    if (s.level0 != nullptr && s.level0Size != 0) {
        h.Update(s.level0, s.level0Size);
    }
    const bool palettized = s.format.format == static_cast<std::uint32_t>(ni::NiPixelLayout::PAL) ||
                            s.format.format == static_cast<std::uint32_t>(ni::NiPixelLayout::PALA);
    if (palettized && s.paletteRgba != nullptr) {
        h.Update(s.paletteRgba, 1024);
        const std::uint8_t alpha = s.paletteHasAlpha ? 1 : 0;
        h.Update(&alpha, 1);
    }
    return h.Digest();
}

bool HashGuestPixelData(ni::GuestPtr pixelData, std::uint64_t* hash, ImageInfo* info) noexcept {
    if (pixelData == 0 || !guest::IsReadable(pixelData, sizeof(ni::NiPixelData))) {
        return false;
    }
    ni::NiPixelData pd;
    std::memcpy(&pd, guest::Ptr<const void>(pixelData), sizeof(pd));
    if (pd.pixels == 0 || pd.mipCount == 0 || pd.widths == 0 || pd.heights == 0 || pd.offsets == 0 ||
        !guest::IsReadable(pd.widths, 4) || !guest::IsReadable(pd.heights, 4) || !guest::IsReadable(pd.offsets, 8)) {
        return false;
    }
    HashSource s;
    s.format = pd.format;
    s.width = guest::Read<std::uint32_t>(pd.widths);
    s.height = guest::Read<std::uint32_t>(pd.heights);
    const std::uint32_t begin = guest::Read<std::uint32_t>(pd.offsets);
    const std::uint32_t end = guest::Read<std::uint32_t>(pd.offsets + 4);
    if (end <= begin || !guest::IsReadable(pd.pixels + begin, end - begin)) {
        return false;
    }
    s.level0 = guest::Ptr<const std::uint8_t>(pd.pixels + begin);
    s.level0Size = end - begin;
    if (pd.palette != 0 && guest::IsReadable(pd.palette, sizeof(ni::NiPalette))) {
        ni::NiPalette p;
        std::memcpy(&p, guest::Ptr<const void>(pd.palette), sizeof(p));
        if (p.entries != 0 && guest::IsReadable(p.entries, 1024)) {
            s.paletteRgba = guest::Ptr<const std::uint8_t>(p.entries);
            s.paletteHasAlpha = p.hasAlpha != 0;
        }
    }
    if (hash != nullptr) {
        *hash = TextureHash(s);
    }
    if (info != nullptr) {
        info->width = s.width;
        info->height = s.height;
        info->mipCount = pd.mipCount;
        info->format = pd.format.format;
        info->bitsPerPixel = pd.format.bitsPerPixel;
    }
    return true;
}

std::string HashName(std::uint64_t hash) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

namespace {
int HexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
}  // namespace

bool ParseHashName(std::string_view stem, std::uint64_t* hash) noexcept {
    if (stem.size() < 16 || (stem.size() > 16 && HexValue(stem[16]) >= 0)) {
        return false;
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        const int d = HexValue(stem[i]);
        if (d < 0) {
            return false;
        }
        v = (v << 4) | static_cast<std::uint64_t>(d);
    }
    if (hash != nullptr) {
        *hash = v;
    }
    return true;
}

const char* SourceFormatName(std::uint32_t format, std::uint32_t bitsPerPixel) noexcept {
    switch (format) {
    case 0:
        return bitsPerPixel == 16 ? "RGB16" : bitsPerPixel == 24 ? "RGB24" : bitsPerPixel == 32 ? "RGB32" : "RGB";
    case 1:
        return bitsPerPixel == 16 ? "RGBA16" : bitsPerPixel == 32 ? "RGBA32" : "RGBA";
    case 2:
        return "PAL8";
    case 3:
        return "PALA8";
    case 4:
        return "DXT1";
    case 5:
        return "DXT3";
    case 6:
        return "DXT5";
    case 8:
        return "BUMP";
    case 9:
        return "BUMPLUMA";
    default:
        return "unknown";
    }
}

// ---- file formats ------------------------------------------------------------------------------------------------

const char* LoadStatusName(LoadStatus status) noexcept {
    switch (status) {
    case LoadStatus::Ok:
        return "ok";
    case LoadStatus::NotFound:
        return "file not found or unreadable";
    case LoadStatus::NotDds:
        return "not a DDS file";
    case LoadStatus::Unsupported:
        return "unsupported format";
    case LoadStatus::Truncated:
        return "file too short";
    case LoadStatus::BadSize:
        return "invalid size";
    case LoadStatus::DecodeFailed:
        return "image could not be decoded";
    case LoadStatus::OutOfMemory:
        return "out of memory";
    }
    return "?";
}

namespace {

LoadStatus Fail(LoadStatus status, std::string* detail, const char* text) noexcept {
    if (detail != nullptr) {
        try {
            *detail = text;
        } catch (...) {
        }
    }
    return status;
}

constexpr std::uint32_t FourCc(char a, char b, char c, char d) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24;
}

// DXGI_FORMAT values of the DX10 header (dxgiformat.h) that the pack accepts.
rhi::Format FormatOfDxgi(std::uint32_t dxgi) noexcept {
    switch (dxgi) {
    case 70:  // BC1_TYPELESS
    case 71:  // BC1_UNORM
    case 72:  // BC1_UNORM_SRGB
        return rhi::Format::BC1_UNorm;
    case 73:
    case 74:
    case 75:
        return rhi::Format::BC2_UNorm;
    case 76:
    case 77:
    case 78:
        return rhi::Format::BC3_UNorm;
    case 97:  // BC7_TYPELESS
    case 98:  // BC7_UNORM
    case 99:  // BC7_UNORM_SRGB
        return rhi::Format::BC7_UNorm;
    case 87:  // B8G8R8A8_UNORM
    case 90:  // B8G8R8A8_TYPELESS
    case 91:  // B8G8R8A8_UNORM_SRGB
        return rhi::Format::BGRA8_UNorm;
    case 88:  // B8G8R8X8_UNORM
    case 92:
    case 93:
        return rhi::Format::BGRX8_UNorm;
    case 27:  // R8G8B8A8_TYPELESS
    case 28:  // R8G8B8A8_UNORM
    case 29:  // R8G8B8A8_UNORM_SRGB
        return rhi::Format::RGBA8_UNorm;
    default:
        return rhi::Format::Unknown;
    }
}

std::uint32_t FullChain(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t levels = 1;
    while ((width > 1 || height > 1) && levels < tex::kMaxMipLevels) {
        width = std::max(1u, width >> 1);
        height = std::max(1u, height >> 1);
        ++levels;
    }
    return levels;
}

}  // namespace

LoadStatus ParseDds(const std::uint8_t* data, std::size_t size, tex::HostImage* out, std::string* detail) noexcept {
    if (out == nullptr) {
        return LoadStatus::OutOfMemory;
    }
    *out = tex::HostImage{};
    constexpr std::size_t kHeader = 4 + 124;
    if (data == nullptr || size < kHeader || Read32(data) != FourCc('D', 'D', 'S', ' ') || Read32(data + 4) != 124) {
        return Fail(LoadStatus::NotDds, detail, "no 'DDS ' magic or header size != 124");
    }
    const std::uint8_t* h = data + 4;
    const std::uint32_t height = Read32(h + 8);
    const std::uint32_t width = Read32(h + 12);
    const std::uint32_t depth = Read32(h + 20);
    std::uint32_t mips = Read32(h + 24);
    const std::uint8_t* pf = h + 72;
    const std::uint32_t pfFlags = Read32(pf + 4);
    const std::uint32_t fourCc = Read32(pf + 8);
    const std::uint32_t bitCount = Read32(pf + 12);
    const std::uint32_t rMask = Read32(pf + 16), gMask = Read32(pf + 20), bMask = Read32(pf + 24),
                        aMask = Read32(pf + 28);
    const std::uint32_t caps2 = Read32(h + 108);
    if ((caps2 & 0x200u) != 0) {
        return Fail(LoadStatus::Unsupported, detail, "cube map");
    }
    if ((caps2 & 0x200000u) != 0 || depth > 1) {
        return Fail(LoadStatus::Unsupported, detail, "volume texture");
    }
    std::size_t offset = kHeader;
    rhi::Format format = rhi::Format::Unknown;
    constexpr std::uint32_t kFourCcFlag = 0x4, kRgbFlag = 0x40, kAlphaPixels = 0x1;
    if ((pfFlags & kFourCcFlag) != 0) {
        if (fourCc == FourCc('D', 'X', 'T', '1')) {
            format = rhi::Format::BC1_UNorm;
        } else if (fourCc == FourCc('D', 'X', 'T', '2') || fourCc == FourCc('D', 'X', 'T', '3')) {
            format = rhi::Format::BC2_UNorm;
        } else if (fourCc == FourCc('D', 'X', 'T', '4') || fourCc == FourCc('D', 'X', 'T', '5')) {
            format = rhi::Format::BC3_UNorm;
        } else if (fourCc == FourCc('D', 'X', '1', '0')) {
            if (size < kHeader + 20) {
                return Fail(LoadStatus::Truncated, detail, "DX10 header missing");
            }
            const std::uint8_t* x = data + kHeader;
            const std::uint32_t dxgi = Read32(x);
            const std::uint32_t dimension = Read32(x + 4);
            const std::uint32_t misc = Read32(x + 8);
            const std::uint32_t arraySize = Read32(x + 12);
            if (dimension != 3) {  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
                return Fail(LoadStatus::Unsupported, detail, "not a 2D texture");
            }
            if ((misc & 0x4u) != 0) {
                return Fail(LoadStatus::Unsupported, detail, "cube map");
            }
            if (arraySize > 1) {
                return Fail(LoadStatus::Unsupported, detail, "texture array");
            }
            format = FormatOfDxgi(dxgi);
            if (format == rhi::Format::Unknown) {
                return Fail(LoadStatus::Unsupported, detail, "DXGI format not supported (BC1/2/3/7, BGRA8, RGBA8)");
            }
            offset += 20;
        } else {
            return Fail(LoadStatus::Unsupported, detail, "FourCC not supported (DXT1/3/5, DX10)");
        }
    } else if ((pfFlags & kRgbFlag) != 0 && bitCount == 32) {
        const bool alpha = (pfFlags & kAlphaPixels) != 0 && aMask == 0xFF000000u;
        if (rMask == 0x00FF0000u && gMask == 0x0000FF00u && bMask == 0x000000FFu) {
            format = alpha ? rhi::Format::BGRA8_UNorm : rhi::Format::BGRX8_UNorm;
        } else if (rMask == 0x000000FFu && gMask == 0x0000FF00u && bMask == 0x00FF0000u && alpha) {
            format = rhi::Format::RGBA8_UNorm;
        } else {
            return Fail(LoadStatus::Unsupported, detail, "32-bit channel masks not supported");
        }
    } else {
        return Fail(LoadStatus::Unsupported, detail, "pixel format not supported");
    }
    if (width == 0 || height == 0 || width > tex::kMaxDimension || height > tex::kMaxDimension) {
        return Fail(LoadStatus::BadSize, detail, "width or height 0 or above 8192");
    }
    const rhi::FormatInfo& info = rhi::GetFormatInfo(format);
    if (info.compressed && (width % 4 != 0 || height % 4 != 0)) {
        return Fail(LoadStatus::BadSize, detail, "block-compressed level 0 not a multiple of 4 (D3D11 rule)");
    }
    mips = std::clamp(mips, 1u, FullChain(width, height));
    try {
        std::size_t total = 0;
        out->format = format;
        out->width = width;
        out->height = height;
        out->mipCount = mips;
        for (std::uint32_t i = 0; i < mips; ++i) {
            tex::MipLevel& level = out->mips[i];
            level.width = std::max(1u, width >> i);
            level.height = std::max(1u, height >> i);
            level.rowPitch = rhi::RowPitch(format, level.width);
            level.size = rhi::LevelSize(format, level.width, level.height);
            level.offset = static_cast<std::uint32_t>(total);
            total += level.size;
        }
        if (size - offset < total) {
            *out = tex::HostImage{};
            return Fail(LoadStatus::Truncated, detail, "fewer bytes than the mip levels need");
        }
        out->bytes.assign(data + offset, data + offset + total);
        out->hasAlpha = info.hasAlpha;
    } catch (...) {
        *out = tex::HostImage{};
        return LoadStatus::OutOfMemory;
    }
    return LoadStatus::Ok;
}

bool MakeMippedBgra(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
                    tex::HostImage* out) noexcept {
    if (out == nullptr || pixels == nullptr || width == 0 || height == 0 || width > tex::kMaxDimension ||
        height > tex::kMaxDimension) {
        return false;
    }
    try {
        *out = tex::HostImage{};
        out->format = rhi::Format::BGRA8_UNorm;
        out->width = width;
        out->height = height;
        out->mipCount = FullChain(width, height);
        std::size_t total = 0;
        for (std::uint32_t i = 0; i < out->mipCount; ++i) {
            tex::MipLevel& level = out->mips[i];
            level.width = std::max(1u, width >> i);
            level.height = std::max(1u, height >> i);
            level.rowPitch = level.width * 4;
            level.size = level.rowPitch * level.height;
            level.offset = static_cast<std::uint32_t>(total);
            total += level.size;
        }
        out->bytes.resize(total);
        std::memcpy(out->bytes.data(), pixels, out->mips[0].size);
        bool alpha = false;
        for (std::uint32_t i = 0; i < out->mips[0].size; i += 4) {
            alpha = alpha || pixels[i + 3] != 255;
        }
        out->hasAlpha = alpha;
        // Each level from the one above: box average over the source rectangle [x * sw / dw, (x + 1) * sw / dw)
        // (2 x 2 for even sizes, 3 texels on an odd axis), colour weighted by alpha so transparent texels do not
        // darken the edges of alpha-blended or alpha-tested textures.
        for (std::uint32_t i = 1; i < out->mipCount; ++i) {
            const tex::MipLevel& s = out->mips[i - 1];
            const tex::MipLevel& d = out->mips[i];
            const std::uint8_t* src = out->bytes.data() + s.offset;
            std::uint8_t* dst = out->bytes.data() + d.offset;
            for (std::uint32_t y = 0; y < d.height; ++y) {
                const std::uint32_t y0 = y * s.height / d.height;
                const std::uint32_t y1 = std::max(y0 + 1, ((y + 1) * s.height + d.height - 1) / d.height);
                for (std::uint32_t x = 0; x < d.width; ++x) {
                    const std::uint32_t x0 = x * s.width / d.width;
                    const std::uint32_t x1 = std::max(x0 + 1, ((x + 1) * s.width + d.width - 1) / d.width);
                    std::uint32_t sum[4] = {0, 0, 0, 0}, weighted[3] = {0, 0, 0}, count = 0;
                    for (std::uint32_t yy = y0; yy < y1 && yy < s.height; ++yy) {
                        const std::uint8_t* row = src + yy * s.rowPitch;
                        for (std::uint32_t xx = x0; xx < x1 && xx < s.width; ++xx) {
                            const std::uint8_t* t = row + xx * 4;
                            for (int c = 0; c < 3; ++c) {
                                sum[c] += t[c];
                                weighted[c] += t[c] * t[3];
                            }
                            sum[3] += t[3];
                            ++count;
                        }
                    }
                    std::uint8_t* o = dst + y * d.rowPitch + x * 4;
                    for (int c = 0; c < 3; ++c) {
                        o[c] = static_cast<std::uint8_t>(sum[3] != 0 ? (weighted[c] + sum[3] / 2) / sum[3]
                                                                     : (sum[c] + count / 2) / count);
                    }
                    o[3] = static_cast<std::uint8_t>((sum[3] + count / 2) / count);
                }
            }
        }
        return true;
    } catch (...) {
        *out = tex::HostImage{};
        return false;
    }
}

LoadStatus LoadReplacementFile(const std::filesystem::path& path, tex::HostImage* out, std::string* detail) noexcept {
    try {
        std::wstring ext = path.extension().wstring();
        for (auto& c : ext) {
            c = static_cast<wchar_t>(towlower(c));
        }
        if (ext == L".dds") {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return Fail(LoadStatus::NotFound, detail, "cannot open the file");
            }
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            return ParseDds(bytes.data(), bytes.size(), out, detail);
        }
        rhi::Image image;
        std::string error;
        if (!platform::ReadImageFile(path, image, &error)) {
            if (detail != nullptr) {
                *detail = error;
            }
            std::error_code ec;
            return std::filesystem::exists(path, ec) ? LoadStatus::DecodeFailed : LoadStatus::NotFound;
        }
        if (image.width > tex::kMaxDimension || image.height > tex::kMaxDimension) {
            return Fail(LoadStatus::BadSize, detail, "width or height above 8192");
        }
        if (image.rowPitch != image.width * 4) {  // repack (WIC normally returns tight rows)
            std::vector<std::uint8_t> tight(static_cast<std::size_t>(image.width) * 4 * image.height);
            for (std::uint32_t y = 0; y < image.height; ++y) {
                std::memcpy(tight.data() + static_cast<std::size_t>(y) * image.width * 4,
                            image.pixels.data() + static_cast<std::size_t>(y) * image.rowPitch, image.width * 4);
            }
            image.pixels.swap(tight);
        }
        return MakeMippedBgra(image.pixels.data(), image.width, image.height, out) ? LoadStatus::Ok
                                                                                   : LoadStatus::OutOfMemory;
    } catch (...) {
        return LoadStatus::OutOfMemory;
    }
}

std::uint32_t ExtraLevels(std::uint32_t originalWidth, std::uint32_t originalHeight, std::uint32_t replacementWidth,
                          std::uint32_t replacementHeight) noexcept {
    if (originalWidth == 0 || originalHeight == 0) {
        return 0;
    }
    std::uint32_t levels = 0;
    while (levels < 15 && replacementWidth >= (originalWidth << (levels + 1)) &&
           replacementHeight >= (originalHeight << (levels + 1))) {
        ++levels;
    }
    return levels;
}

// ---- Pack ---------------------------------------------------------------------------------------------------------

struct Pack::Impl {
    struct Entry {
        State state = State::Missing;
        std::shared_ptr<const tex::HostImage> image;
        std::size_t bytes = 0;
        std::uint64_t lastUse = 0;
    };
    std::unordered_map<std::uint64_t, std::filesystem::path> files;
    mutable std::mutex mutex;
    std::condition_variable work, idle;
    std::unordered_map<std::uint64_t, Entry> entries;
    std::deque<std::uint64_t> queue;
    bool busy = false;
    bool stop = false;
    std::thread worker;
    std::uint64_t clock = 0;
    std::size_t cachedBytes = 0;
    Stats stats;

    void Evict(std::uint64_t keep) {
        while (cachedBytes > kCacheBudgetBytes) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->second.state == State::Ready && it->first != keep &&
                    (oldest == entries.end() || it->second.lastUse < oldest->second.lastUse)) {
                    oldest = it;
                }
            }
            if (oldest == entries.end()) {
                return;
            }
            cachedBytes -= oldest->second.bytes;
            entries.erase(oldest);
            ++stats.evicted;
        }
    }

    void Run() {
        std::unique_lock lock(mutex);
        for (;;) {
            work.wait(lock, [this] { return stop || !queue.empty(); });
            if (stop) {
                return;
            }
            const std::uint64_t hash = queue.front();
            queue.pop_front();
            const auto file = files.find(hash);
            if (file == files.end()) {
                continue;
            }
            const std::filesystem::path path = file->second;
            busy = true;
            lock.unlock();
            const auto start = std::chrono::steady_clock::now();
            auto image = std::make_shared<tex::HostImage>();
            std::string detail;
            const LoadStatus status = LoadReplacementFile(path, image.get(), &detail);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (status != LoadStatus::Ok) {
                FUT_LOGW(Gfx, "texture pack: %s cannot be used: %s%s%s", path.string().c_str(), LoadStatusName(status),
                         detail.empty() ? "" : " - ", detail.c_str());
            }
            lock.lock();
            busy = false;
            Entry& e = entries[hash];
            if (status == LoadStatus::Ok) {
                e.state = State::Ready;
                e.bytes = image->bytes.size();
                e.image = std::move(image);
                e.lastUse = ++clock;
                cachedBytes += e.bytes;
                ++stats.decoded;
                stats.decodedBytes += e.bytes;
                stats.decodeMsTotal += ms;
                stats.decodeMsMax = std::max(stats.decodeMsMax, ms);
                Evict(hash);
            } else {
                e.state = State::Failed;
                ++stats.failed;
            }
            idle.notify_all();
        }
    }
};

Pack::Pack() : impl_(std::make_unique<Impl>()) {}

Pack::~Pack() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stop = true;
    }
    impl_->work.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

bool Pack::Open(const std::filesystem::path& folder, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        if (error != nullptr) {
            *error = "not a folder";
        }
        return false;
    }
    std::unordered_map<std::uint64_t, std::filesystem::path> files;
    Stats stats;
    for (auto it = std::filesystem::recursive_directory_iterator(
             folder, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::filesystem::path& path = it->path();
        std::wstring ext = path.extension().wstring();
        for (auto& c : ext) {
            c = static_cast<wchar_t>(towlower(c));
        }
        const bool dds = ext == L".dds";
        if (!dds && ext != L".png") {
            continue;
        }
        std::uint64_t hash = 0;
        if (!ParseHashName(path.stem().string(), &hash)) {
            continue;
        }
        auto [slot, inserted] = files.try_emplace(hash, path);
        if (!inserted) {
            ++stats.duplicates;
            std::wstring slotExt = slot->second.extension().wstring();
            for (auto& c : slotExt) {
                c = static_cast<wchar_t>(towlower(c));
            }
            if (dds && slotExt != L".dds") {
                slot->second = path;  // .dds wins over .png
                ++stats.dds;
                --stats.png;
            }
            continue;
        }
        ++(dds ? stats.dds : stats.png);
    }
    stats.files = static_cast<std::uint32_t>(files.size());
    std::lock_guard lock(impl_->mutex);
    impl_->files = std::move(files);
    impl_->entries.clear();
    impl_->queue.clear();
    impl_->cachedBytes = 0;
    impl_->stats = stats;
    return true;
}

std::size_t Pack::FileCount() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->files.size();
}

std::filesystem::path Pack::FileOf(std::uint64_t hash) const {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->files.find(hash);
    return it != impl_->files.end() ? it->second : std::filesystem::path{};
}

bool Pack::Request(std::uint64_t hash) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->files.find(hash) == impl_->files.end()) {
        return false;
    }
    Impl::Entry& e = impl_->entries[hash];
    if (e.state == State::Ready) {
        ++impl_->stats.cacheHits;
        e.lastUse = ++impl_->clock;
        return true;
    }
    if (e.state != State::Missing) {
        return true;  // queued or failed before
    }
    e.state = State::Pending;
    impl_->queue.push_back(hash);
    if (!impl_->worker.joinable()) {
        impl_->worker = std::thread([impl = impl_.get()] {
            SetThreadDescription(GetCurrentThread(), L"texture pack loader");
            impl->Run();
        });
    }
    impl_->work.notify_one();
    return true;
}

Pack::State Pack::Fetch(std::uint64_t hash, std::shared_ptr<const tex::HostImage>* image) {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->entries.find(hash);
    if (it == impl_->entries.end()) {
        return State::Missing;
    }
    if (it->second.state == State::Ready) {
        it->second.lastUse = ++impl_->clock;
        if (image != nullptr) {
            *image = it->second.image;
        }
    }
    return it->second.state;
}

bool Pack::WaitIdle(std::uint32_t timeoutMs) {
    std::unique_lock lock(impl_->mutex);
    return impl_->idle.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                [this] { return impl_->queue.empty() && !impl_->busy; });
}

Pack::Stats Pack::GetStats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}

// ---- renderer integration ------------------------------------------------------------------------------------------

namespace {

struct PendingInfo {
    std::uint64_t hash = 0;
    std::uint32_t width = 0, height = 0;  // original level 0
    std::uint32_t format = 0, bitsPerPixel = 0;
};

struct DumpJob {
    std::uint64_t hash = 0;
    ImageInfo info;
    rhi::Image image;  // BGRA8 level 0
    const char* alpha = "none";
};

struct Manager {
    bool packOn = false;
    bool dumpOn = false;
    Pack pack;
    std::filesystem::path packDir, dumpDir;

    std::mutex mutex;  // pending, dumped, the dump queue
    std::unordered_map<std::uint32_t, PendingInfo> pending;  // NiPCTextureData address -> request
    std::unordered_set<std::uint64_t> dumped;
    std::unordered_set<std::uint64_t> reportedFailures;
    std::deque<DumpJob> dumpQueue;
    bool dumpBusy = false;
    std::condition_variable dumpWork, dumpIdle;
    std::thread dumpWorker;

    std::mutex replacedMutex;
    std::unordered_map<std::uint32_t, std::uint8_t> replaced;  // host texture id -> ExtraLevels
    std::atomic<std::uint32_t> replacedCount{0};

    std::mutex hashMutex;
    std::unordered_map<std::uint32_t, std::uint64_t> hashOfTexture;  // NiSourceTexture -> hash of its last upload

    std::atomic<std::uint32_t> seen{0}, nonStatic{0}, hashFailed{0}, hits{0}, misses{0}, applied{0}, failed{0},
        dumpedFiles{0}, dumpFailures{0}, dumpSkipped{0}, droppedPending{0};
    std::atomic<std::uint64_t> replacedBytes{0}, originalBytes{0};

    void RunDump() {
        std::unique_lock lock(mutex);
        for (;;) {
            dumpWork.wait(lock, [this] { return !dumpQueue.empty(); });
            DumpJob job = std::move(dumpQueue.front());
            dumpQueue.pop_front();
            dumpBusy = true;
            lock.unlock();
            const std::string name = HashName(job.hash);
            std::string error;
            const bool ok = platform::WritePngFileRgba(dumpDir / (name + ".png"), job.image, &error);
            if (ok) {
                dumpedFiles.fetch_add(1, std::memory_order_relaxed);
                const std::filesystem::path index = dumpDir / "index.tsv";
                std::error_code ec;
                const bool fresh = !std::filesystem::exists(index, ec);
                if (FILE* f = _wfopen(index.c_str(), L"ab")) {
                    if (fresh) {
                        std::fputs("hash\twidth\theight\tformat\tmips\talpha\n", f);
                    }
                    std::fprintf(f, "%s\t%u\t%u\t%s\t%u\t%s\n", name.c_str(), job.info.width, job.info.height,
                                 SourceFormatName(job.info.format, job.info.bitsPerPixel), job.info.mipCount,
                                 job.alpha);
                    std::fclose(f);
                }
            } else {
                dumpFailures.fetch_add(1, std::memory_order_relaxed);
                FUT_LOGW(Gfx, "texture dump: %s.png not written: %s", name.c_str(), error.c_str());
            }
            lock.lock();
            dumpBusy = false;
            dumpIdle.notify_all();
        }
    }
};

Manager& M() {
    static Manager* manager = new Manager();  // never destroyed: the process ends with TerminateProcess
    return *manager;
}

void Initialize() noexcept {
    Manager& m = M();
    const Settings& s = GetSettings();
    try {
        if (!s.texturePack.empty()) {
            std::string error;
            if (m.pack.Open(s.texturePack, &error)) {
                m.packOn = true;
                m.packDir = s.texturePack;
                const Pack::Stats st = m.pack.GetStats();
                FUT_LOGI(Gfx, "texture pack: '%s': %u replacement files (%u DDS, %u PNG%s)",
                         s.texturePack.string().c_str(), st.files, st.dds, st.png,
                         st.duplicates != 0 ? ", duplicates ignored" : "");
            } else {
                FUT_LOGW(Gfx, "texture pack: [render] texture_pack = '%s' is %s - original textures",
                         s.texturePack.string().c_str(), error.c_str());
            }
        }
        if (s.texturePackDump) {
            std::error_code ec;
            std::filesystem::create_directories(s.texturePackDumpDir, ec);
            if (ec) {
                FUT_LOGW(Gfx, "texture dump: cannot create '%s': %s - no dump", s.texturePackDumpDir.string().c_str(),
                         ec.message().c_str());
            } else {
                m.dumpOn = true;
                m.dumpDir = s.texturePackDumpDir;
                std::uint32_t existing = 0;
                for (const auto& entry : std::filesystem::directory_iterator(m.dumpDir, ec)) {
                    std::uint64_t hash = 0;
                    if (entry.path().extension() == ".png" && ParseHashName(entry.path().stem().string(), &hash)) {
                        m.dumped.insert(hash);
                        ++existing;
                    }
                }
                FUT_LOGI(Gfx, "texture dump: static textures -> '%s' (%u already there)", m.dumpDir.string().c_str(),
                         existing);
            }
        }
    } catch (...) {
        FUT_LOGE(Gfx, "texture pack: initialisation failed (out of memory)");
        m.packOn = false;
        m.dumpOn = false;
    }
}

const char* AlphaClass(const tex::HostImage& image) noexcept {
    bool binary = false;
    const std::uint8_t* p = image.bytes.data() + image.mips[0].offset;
    for (std::uint32_t i = 0; i < image.mips[0].size; i += 4) {
        const std::uint8_t a = p[i + 3];
        if (a != 0 && a != 255) {
            return "smooth";
        }
        binary = binary || a == 0;
    }
    return binary ? "binary" : "none";
}

void QueueDump(Manager& m, std::uint64_t hash, const ImageInfo& info, ni::GuestPtr pixelData) {
    {
        std::lock_guard lock(m.mutex);
        if (!m.dumped.insert(hash).second) {
            return;
        }
    }
    tex::ConvertOptions options;
    options.keepBlockCompression = false;  // DXT -> BGRA8
    options.maxMipLevels = 1;
    tex::HostImage image;
    if (tex::ConvertPixelData(pixelData, 0, options, &image) != tex::ConvertStatus::Ok ||
        image.format != rhi::Format::BGRA8_UNorm) {
        m.dumpSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    DumpJob job;
    job.hash = hash;
    job.info = info;
    job.alpha = AlphaClass(image);
    job.image.format = rhi::Format::BGRA8_UNorm;
    job.image.width = image.mips[0].width;
    job.image.height = image.mips[0].height;
    job.image.rowPitch = image.mips[0].rowPitch;
    job.image.pixels.assign(image.bytes.begin() + image.mips[0].offset,
                            image.bytes.begin() + image.mips[0].offset + image.mips[0].size);
    std::lock_guard lock(m.mutex);
    m.dumpQueue.push_back(std::move(job));
    if (!m.dumpWorker.joinable()) {
        m.dumpWorker = std::thread([&m] {
            SetThreadDescription(GetCurrentThread(), L"texture dump writer");
            m.RunDump();
        });
    }
    m.dumpWork.notify_one();
}

}  // namespace

bool Enabled() noexcept {
    static std::once_flag once;
    std::call_once(once, Initialize);
    return M().packOn || M().dumpOn;
}

void OnTextureUploaded(GpuRenderer& gpu, NiPCTextureData* data, ni::GuestPtr niTexture,
                       ni::GuestPtr pixelData) noexcept {
    if (data == nullptr || !Enabled()) {
        return;
    }
    Manager& m = M();
    try {
        if (guest::Read<std::uint8_t>(niTexture + ni::kNiSourceTextureStatic) == 0) {
            m.nonStatic.fetch_add(1, std::memory_order_relaxed);  // the game rewrites it (FUN_0015E260)
            return;
        }
        std::uint64_t hash = 0;
        ImageInfo info;
        if (!HashGuestPixelData(pixelData, &hash, &info)) {
            m.hashFailed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m.seen.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(m.hashMutex);
            m.hashOfTexture[niTexture] = hash;
        }
        if (m.dumpOn) {
            QueueDump(m, hash, info, pixelData);
        }
        if (!m.packOn) {
            return;
        }
        if (!m.pack.Request(hash)) {
            m.misses.fetch_add(1, std::memory_order_relaxed);
            FUT_LOGD(Gfx, "texture pack: miss %s (%ux%u %s, NiSourceTexture 0x%08X)", HashName(hash).c_str(),
                     info.width, info.height, SourceFormatName(info.format, info.bitsPerPixel), niTexture);
            return;
        }
        m.hits.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(m.mutex);
            m.pending[guest::Addr(data)] = PendingInfo{hash, info.width, info.height, info.format, info.bitsPerPixel};
        }
        data->hostInfo |= kHostReplacementPending;
        ApplyPending(gpu, data);  // already decoded (cache) -> at once
    } catch (...) {
        FUT_LOGE(Gfx, "texture pack: out of memory");
    }
}

bool UploadedHash(ni::GuestPtr niTexture, std::uint64_t* hash) noexcept {
    if (!M().packOn && !M().dumpOn) {
        return false;
    }
    Manager& m = M();
    std::lock_guard lock(m.hashMutex);
    const auto it = m.hashOfTexture.find(niTexture);
    if (it == m.hashOfTexture.end()) {
        return false;
    }
    *hash = it->second;
    return true;
}

void ApplyPending(GpuRenderer& gpu, NiPCTextureData* data) noexcept {
    Manager& m = M();
    try {
        PendingInfo p;
        {
            std::lock_guard lock(m.mutex);
            const auto it = m.pending.find(guest::Addr(data));
            if (it == m.pending.end()) {
                data->hostInfo &= ~kHostReplacementPending;
                return;
            }
            p = it->second;
        }
        std::shared_ptr<const tex::HostImage> image;
        Pack::State state = m.pack.Fetch(p.hash, &image);
        if (state == Pack::State::Missing) {  // evicted from the cache before it was used
            m.pack.Request(p.hash);
            return;
        }
        if (state == Pack::State::Pending) {
            return;
        }
        rhi::TextureHandle handle;
        if (state == Pack::State::Ready) {
            char name[48];
            std::snprintf(name, sizeof(name), "texture pack %s", HashName(p.hash).c_str());
            handle = gpu.CreateTexture(*image, name);
        }
        {
            std::lock_guard lock(m.mutex);
            m.pending.erase(guest::Addr(data));
        }
        data->hostInfo &= ~kHostReplacementPending;
        if (!handle) {
            m.failed.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(m.mutex);
            if (m.reportedFailures.insert(p.hash).second && state == Pack::State::Ready) {
                FUT_LOGW(Gfx, "texture pack: %s (%ux%u %s) could not be created on the device - original texture",
                         HashName(p.hash).c_str(), image->width, image->height,
                         rhi::GetFormatInfo(image->format).name);
            }
            return;
        }
        if ((data->hostInfo & kHostOwnsTexture) != 0 && data->hostTexture != 0) {
            gpu.DestroyTexture(rhi::TextureHandle{data->hostTexture});
        }
        data->hostTexture = handle.id;
        data->hostInfo |= kHostReplaced | kHostOwnsTexture;
        const std::uint32_t extra = ExtraLevels(p.width, p.height, image->width, image->height);
        {
            std::lock_guard lock(m.replacedMutex);
            m.replaced[handle.id] = static_cast<std::uint8_t>(extra);
            m.replacedCount.store(static_cast<std::uint32_t>(m.replaced.size()), std::memory_order_relaxed);
        }
        m.applied.fetch_add(1, std::memory_order_relaxed);
        m.replacedBytes.fetch_add(image->bytes.size(), std::memory_order_relaxed);
        FUT_LOGD(Gfx, "texture pack: hit %s: %ux%u %s -> %ux%u %s, %u levels", HashName(p.hash).c_str(), p.width,
                 p.height, SourceFormatName(p.format, p.bitsPerPixel), image->width, image->height,
                 rhi::GetFormatInfo(image->format).name, image->mipCount);
    } catch (...) {
        data->hostInfo &= ~kHostReplacementPending;
    }
}

void OnDataDestroyed(NiPCTextureData* data) noexcept {
    if (data == nullptr) {
        return;
    }
    Manager& m = M();
    if ((data->hostInfo & kHostReplacementPending) != 0) {
        std::lock_guard lock(m.mutex);
        if (m.pending.erase(guest::Addr(data)) != 0) {
            m.droppedPending.fetch_add(1, std::memory_order_relaxed);  // freed before it was drawn again
        }
    }
    if ((data->hostInfo & kHostReplaced) != 0) {
        std::lock_guard lock(m.replacedMutex);
        m.replaced.erase(data->hostTexture);
        m.replacedCount.store(static_cast<std::uint32_t>(m.replaced.size()), std::memory_order_relaxed);
    }
}

void AdjustSampler(rhi::TextureHandle texture, rhi::SamplerDesc* desc) noexcept {
    if (desc == nullptr || desc->mipFilter != rhi::MipFilter::None || !texture ||
        M().replacedCount.load(std::memory_order_relaxed) == 0) {
        return;
    }
    Manager& m = M();
    std::uint32_t extra = 0;
    {
        std::lock_guard lock(m.replacedMutex);
        const auto it = m.replaced.find(texture.id);
        if (it == m.replaced.end()) {
            return;
        }
        extra = it->second;
    }
    if (extra != 0) {
        desc->mipFilter = rhi::MipFilter::Linear;
        desc->maxLod = static_cast<float>(extra);
    }
}

void LogStatistics() noexcept {
    Manager& m = M();
    if (!m.packOn && !m.dumpOn) {
        return;
    }
    std::size_t pendingNow = 0;
    {
        std::unique_lock lock(m.mutex);
        if (m.dumpOn) {
            m.dumpIdle.wait_for(lock, std::chrono::seconds(5), [&m] { return m.dumpQueue.empty() && !m.dumpBusy; });
        }
        pendingNow = m.pending.size();
    }
    if (m.dumpOn) {
        FUT_LOGI(Gfx, "texture dump: %u PNG files written to '%s', %u failed, %u not convertible",
                 m.dumpedFiles.load(), m.dumpDir.string().c_str(), m.dumpFailures.load(), m.dumpSkipped.load());
    }
    if (m.packOn) {
        const Pack::Stats st = m.pack.GetStats();
        FUT_LOGI(Gfx,
                 "texture pack: %u static textures looked up: %u hits (%u replaced, %u failed, %zu not drawn since "
                 "the upload, %u freed before), "
                 "%u misses; %u non-static skipped, %u unhashable; %u files decoded (%llu MiB, %.1f ms total, "
                 "max %.1f ms), %u failed, %u cache hits, %u evicted; replacements uploaded: %llu MiB",
                 m.seen.load(), m.hits.load(), m.applied.load(), m.failed.load(), pendingNow, m.droppedPending.load(),
                 m.misses.load(),
                 m.nonStatic.load(), m.hashFailed.load(), st.decoded,
                 static_cast<unsigned long long>(st.decodedBytes >> 20), st.decodeMsTotal, st.decodeMsMax, st.failed,
                 st.cacheHits, st.evicted, static_cast<unsigned long long>(m.replacedBytes.load() >> 20));
    }
}

}  // namespace gfx::texpack
