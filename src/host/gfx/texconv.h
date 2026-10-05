// NiPixelData (guest) -> host texture image (GFX_CONTRACT.md 7). Owned by the texconv agent.
//
// ---- The original (NiXBoxRenderer, verified in M3 phase 2) ------------------------------------------
// Slot 65 CreateSourceTextureRendererData = FUN_0015E010:
//   1. image = NiImageConverter [0x30EF64] vf2(path, 0) if NiSourceTexture +0x30 (file name) is set,
//      else NiSourceTexture +0x34 (NiPixelData*);
//   2. level 0 width and height (NiPixelData +0x34/+0x38 [0]) must be non-zero powers of two (bit loops
//      0x15E087..0x15E0C1), else the renderer's default texture (+0x5A0, format +0x598);
//   3. FUN_001572D0(image +0x0C, FormatPrefs = NiSourceTexture +0x14, format table = renderer +0x528)
//      picks a table entry in a FormatPrefs-dependent order (ChooseXboxFormat); 0 -> default texture. An
//      entry only matches if its packed channel descriptor (NiPixelFormat +0x18/+0x1C) EQUALS the
//      source's: the Xbox image converter (vtable 0x2AAC94) converts nothing - CanConvert vf3 0x151480
//      compares the two descriptor dwords, Convert vf4 0x1514B0 (thiscall, ret 0x10) returns its 3rd
//      argument unchanged when that is the source (FUN_0015E010 pushes the source twice, 0x15E167 and
//      0x15E169) and never reads its 4th argument (the mipmap flag). So the texture always holds exactly
//      the stored data and levels: no format conversion, no quality reduction, no mipmap generation.
//   4. FUN_0015D920: D3DDevice_CreateTexture2(w, h, 1, NiPixelData +0x40 levels, 0, FUN_000EDC70(format),
//      3); FUN_0015DB00 copies every level (linear for DXT, CPU swizzle for 8/16/32 bpp - none on the PC);
//      FUN_0015DF60 -> slot 69 NiXBoxPaletteData: FUN_0015F1B0 turns each NiPalette entry (bytes r, g, b,
//      a at NiPalette +0x18) into D3DCOLOR (a << 24 | r << 16 | g << 8 | b) with a = 0xFF unless NiPalette
//      +0x1C (has alpha) is set - the alpha decision is the palette's, not PAL vs PALA.
//   5. Afterwards (0x15E1A6..0x15E1DD) the pixel buffer of a static source texture (NiSourceTexture
//      +0x3C) up to 512x512 is freed (FUN_001511B0, NiPixelData +0x30 = 0) - integration's business.
//
// Renderer format table (+0x528, FUN_0014A410: 14 NiPixelFormats, each checked with D3D_CheckDeviceFormat;
// default texture format +0x598 = entry 5) = XboxFormat. The NiPixelFormat masks follow the D3D8 layouts
// (little-endian words, e.g. A4R4G4B4 = alpha 0xF000, red 0x0F00, green 0x00F0, blue 0x000F).
//
// ---- texconv ----------------------------------------------------------------------------------------
// Converts every stored level of an NiPixelData into one host format (GFX_CONTRACT.md 7 decisions):
//
//   NiPixelFormat (+0x00)  bpp    Xbox D3DFORMAT (FUN_000EDC70)       host format (rhi::Format)
//   0 RGB                  16     R5G6B5 (5) / X1R5G5B5 (3)           BGRA8 via the masks, alpha 255
//                                                                     (keep16Bit: B5G6R5 / B5G5R5A1)
//   0 RGB                  24/32  X8R8G8B8 (7)                        BGRA8 via the masks, alpha 255
//   1 RGBA                 16     R5G6B5 / A4R4G4B4 (4) / A1R5G5B5 (2) BGRA8 via the masks
//                                                                     (keep16Bit: B4G4R4A4 / B5G5R5A1)
//   1 RGBA                 24/32  X8R8G8B8 ("24Bit ILLEGAL!") / A8R8G8B8 (6)  BGRA8 via the masks
//   2 PAL / 3 PALA          8     P8 (0xB) + palette (slot 69)        BGRA8 through the NiPalette entries
//                                                                     (r, g, b, a; alpha = entry alpha if
//                                                                     NiPalette +0x1C, else 255)
//   4 DXT1 / 5 DXT3 / 6 DXT5 0    DXT1/3/5 (0xC/0xE/0xF), linear      BC1/BC2/BC3, bytes unchanged
//                                                                     (standard little-endian S3TC blocks);
//                                                                     decoded to BGRA8 if level 0 is not a
//                                                                     multiple of 4 (D3D11 rule) or
//                                                                     !keepBlockCompression
//   8 BUMP / 9 BUMPLUMA    16/32  V8U8 / L6V5U5 / X8L8V8U8            Unsupported (default texture)
//   0/1 with 8 bpp: FUN_000EDC70 returns -1 -> Unsupported; other formats (7, > 9) -> Unsupported.
// Mip levels: exactly the stored ones (see 3. above). A level whose size does not halve (max(1, w0 >> i))
// ends the chain there (D3D11 needs the standard chain; HostImage::droppedMipLevels); never generated.
// Channel expansion n -> 8 bits: bit replication (565: r << 3 | r >> 2, g << 2 | g >> 4; 4444: v * 17).
// The xemu reference captures match it texel for texel (exact UNORM rounding would differ by 1 in 376
// flat texels of the legal/notice screens, run\m3-texconv\cmp_xemu.py). keep16Bit outputs are sampled as
// exact UNORM by the GPU.
//
// Measured on the disc (run\m3-texconv\scan_pixeldata.py: every NiPixelData of the 820 NIF/UCF files of
// the six archives, 8756 images): DXT1 4003, DXT3 1852, RGBA 4444 16 bit 1647, RGB 565 16 bit 1253, one
// RGBA 32 bit with A8B8G8R8 masks (effects\envmap\newshade.nif: FUN_001572D0 finds no entry -> default
// texture on the Xbox); no palette, no DXT5, no 555/1555, no 24 bit, no file path; all powers of two
// (2x2 .. 1024x512); every DXT level 0 a multiple of 4 (chains stop at 4 texels); stored descriptors ==
// MakeRgbFormat / MakeCompressedFormat.
//
// Slot 65 source order (FUN_0015E010, already in gfx/nipc_renderer.cpp FillSourceTextureInfo; the
// integration keeps it and calls texconv for the chosen image):
//   1. NiSourceTexture +0x30 (file name) set -> NiImageConverter vf2(path, 0) -> NiPixelData (even if
//      +0x34 is set); [render] load_textures = 0 skips the read;
//   2. else NiSourceTexture +0x34 NiPixelData;
//   3. no image, unreadable file, width or height of level 0 not a power of two, no matching Xbox format
//      (step 3 above, when the caller passes the image format) or ConvertPixelData != Ok -> the
//      renderer's default texture (MakeDefaultTexture).
// SelectTextureSource below is the pure decision of steps 1-3 for unit tests and the integration.
#pragma once

#include "gfx/ni_guest_layouts.h"
#include "gfx/rhi/rhi.h"

#include <cstdint>
#include <vector>

namespace gfx::tex {

inline constexpr std::uint32_t kMaxMipLevels = 16;
inline constexpr std::uint32_t kMaxDimension = 8192;  // D3D11 feature level 10_0 limit (NV2A: 4096)

struct MipLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowPitch = 0;  // bytes per row (of 4x4 blocks for BCn) = rhi::RowPitch
    std::uint32_t size = 0;      // bytes of the level = rhi::LevelSize
    std::uint32_t offset = 0;    // into HostImage::bytes
};

struct HostImage {
    rhi::Format format = rhi::Format::Unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipCount = 0;
    MipLevel mips[kMaxMipLevels];
    std::vector<std::uint8_t> bytes;
    bool hasAlpha = false;           // any texel alpha < 255 possible (alpha mask / palette alpha / DXT1/3/5)
    std::uint32_t sourceFormat = 0;  // NiPixelFormat::format (diagnostics)
    std::uint32_t sourceBpp = 0;
    std::uint32_t droppedMipLevels = 0;  // stored levels not converted (inconsistent chain or maxMipLevels)
    bool decompressed = false;           // DXT source decoded to BGRA8
};

enum class ConvertStatus : std::uint8_t {
    Ok,
    NoPixelData,  // NULL NiPixelData / no pixels / mipCount 0
    Unsupported,  // BUMP, BUMPLUMA, 8-bit RGB(A), unknown format or bpp, no color mask
    NoPalette,    // PAL/PALA without NiPalette entries
    BadData,      // level 0: size 0 or > kMaxDimension, offsets[1] - offsets[0] != level size; > 16 levels;
                  // NiPixelData +0x44 != bpp / 8; unreadable guest memory
    OutOfMemory,
};
const char* ConvertStatusName(ConvertStatus status) noexcept;

// ConvertOptions::keep16Bit bits: exact D3D8 16-bit layouts stay 16 bit instead of BGRA8 (half the memory).
// Set only the formats rhi::Device::IsFormatSupported(.., kTextureSampled) reports (D3D11.1 + WDDM 1.2).
enum Keep16Bit : std::uint8_t {
    kKeepR5G6B5 = 1,    // RGB 565 -> B5G6R5_UNorm
    kKeepA1R5G5B5 = 2,  // RGBA 1555 -> B5G5R5A1_UNorm; RGB 555 (X1R5G5B5) too, with the alpha bit set
    kKeepA4R4G4B4 = 4,  // RGBA 4444 -> B4G4R4A4_UNorm
};

struct ConvertOptions {
    bool keepBlockCompression = true;  // false: decompress DXT to BGRA8 (devices without BCn, tests)
    std::uint32_t maxMipLevels = kMaxMipLevels;  // keep at most this many levels of the source (0 = 1)
    std::uint8_t keep16Bit = 0;  // Keep16Bit bits; 0 = every uncompressed format -> BGRA8 (contract)
};

// Converts a guest NiPixelData (ni::NiPixelData layout) including all its mip levels. The palette is
// NiPixelData +0x2C unless `palette` != 0 is given. Reads guest memory only (checked with guest::IsReadable).
ConvertStatus ConvertPixelData(ni::GuestPtr pixelData, ni::GuestPtr palette, const ConvertOptions& options,
                               HostImage* out) noexcept;

// Host-memory form of the same conversion (unit tests, no guest objects): `pixels` holds all levels at
// `offsets[i]` (mipCount + 1 entries, the last = total size); `paletteRgba` = 256 entries of 4 bytes in
// NiPalette byte order r, g, b, a (FUN_0015F1B0) or nullptr; `paletteHasAlpha` = NiPalette +0x1C.
struct PixelSource {
    ni::NiPixelFormat format{};
    const std::uint8_t* pixels = nullptr;
    std::uint32_t mipCount = 0;
    const std::uint32_t* widths = nullptr;
    const std::uint32_t* heights = nullptr;
    const std::uint32_t* offsets = nullptr;  // mipCount + 1 entries
    const std::uint8_t* paletteRgba = nullptr;
    bool paletteHasAlpha = false;  // NiPalette +0x1C
};
ConvertStatus ConvertPixels(const PixelSource& source, const ConvertOptions& options,
                            HostImage* out) noexcept;

// RHI descriptors of a converted image: TextureDesc {width, height, mipCount, format, sampled} and one
// SubresourceData per level (pointing into image.bytes; returns the number written, at most `capacity`).
rhi::TextureDesc MakeTextureDesc(const HostImage& image, const char* debugName = nullptr) noexcept;
std::uint32_t MakeSubresources(const HostImage& image, rhi::SubresourceData* levels,
                               std::uint32_t capacity) noexcept;

// The renderer's 8x8 default texture (FUN_0014AB50: NiPixelData 8x8, one level, A8R8G8B8, all texels
// 0x00000000 except 0xFFFF0000 at dwords 0x18, 0x20-0x22, 0x28, 0x2A, 0x30, 0x32; 0xFF0000FF at 0x0D,
// 0x15, 0x1D, 0x23-0x25, 0x2B, 0x2D, 0x33-0x35; 0xFFFFFF00 at 0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x36) as BGRA8
// (the A8R8G8B8 dwords in memory order; the original swizzles them on upload, so what it displays is
// this linear image).
void MakeDefaultTexture(HostImage* out) noexcept;

// ---- Xbox format choice (FUN_001572D0) ------------------------------------------------------------------

// Entries of the renderer format table (+0x528, index = entry, FUN_0014A410).
// ASSUMPTION: D3D_CheckDeviceFormat accepts all 14 on the Xbox (NV2A texture formats), so no entry is NULL.
enum class XboxFormat : std::int8_t {
    None = -1,  // FUN_001572D0 returned 0 -> default texture
    X1R5G5B5,   // 0: RGB 0x7C00/0x3E0/0x1F, 16 bpp
    R5G6B5,     // 1: RGB 0xF800/0x7E0/0x1F, 16 bpp
    X8R8G8B8,   // 2: RGB 0xFF0000/0xFF00/0xFF, 32 bpp
    A1R5G5B5,   // 3: RGBA 0x7C00/0x3E0/0x1F/0x8000, 16 bpp
    A4R4G4B4,   // 4: RGBA 0xF00/0xF0/0xF/0xF000, 16 bpp
    A8R8G8B8,   // 5: RGBA 0xFF0000/0xFF00/0xFF/0xFF000000, 32 bpp (default texture format, +0x598 = +0x53C)
    P8,         // 6: PAL (FUN_001408A0(0))
    P8Alpha,    // 7: PALA (FUN_001408A0(1))
    DXT1,       // 8: FUN_00140B10(4)
    DXT3,       // 9: FUN_00140B10(5)
    DXT5,       // 10: FUN_00140B10(6)
    V8U8,       // 11: BUMP u 0xFF, v 0xFF00, 16 bpp (FUN_00140C30)
    L6V5U5,     // 12: BUMPLUMA u 0x1F, v 0x3E0, luma 0xFC00, 16 bpp
    X8L8V8U8,   // 13: BUMPLUMA u 0xFF, v 0xFF00, luma 0xFF0000, 32 bpp
    Count
};
const char* XboxFormatName(XboxFormat format) noexcept;

// NiPixelFormat as the NetImmerse constructors build it, including the packed channel descriptor
// (+0x18: bits 0-4 format, 5-7 bytes per pixel (16/24/32 bpp only), 8-13/14-19/20-25/26-31 bit counts of
// channel 0-3; +0x1C: bits 0-4/5-9/10-14/15-19 shifts of channel 0-3; FUN_00140910 with the tables at
// 0x2D7B5C..0x2D7BA4, format shift DAT_0030E8AC = 0 (BSS, never written)).
ni::NiPixelFormat MakeRgbFormat(std::uint32_t redMask, std::uint32_t greenMask, std::uint32_t blueMask,
                                std::uint32_t alphaMask,
                                std::uint32_t bitsPerPixel) noexcept;       // FUN_001409A0
ni::NiPixelFormat MakePaletteFormat(bool alpha) noexcept;                   // FUN_001408A0
ni::NiPixelFormat MakeCompressedFormat(ni::NiPixelLayout layout) noexcept;  // FUN_00140B10 (4-6)
ni::NiPixelFormat MakeBumpFormat(std::uint32_t uMask, std::uint32_t vMask, std::uint32_t lumaMask,
                                 std::uint32_t bitsPerPixel) noexcept;  // FUN_00140C30
// The NiPixelFormat of a table entry (Count/None -> zero format).
ni::NiPixelFormat XboxTableFormat(XboxFormat format) noexcept;

// NiTexture::FormatPrefs at NiSourceTexture +0x14 (FUN_001572D0 param_2). Field order verified: FUN_0015E010
// 0x15E13B sets the converter's mipmap flag from +0x18 == 1 || (+0x18 == 2 && DAT_0030EF84) (= MipFlag YES /
// MIP_DEFAULT with the global default), FUN_001572D0 prefers PALA/1555 when +0x1C is 1 or 2 (AlphaFormat
// BINARY/SMOOTH); the NIF stream order is the same and the disc only has +0x18 in {0, 1, 2}, +0x1C = 3.
// (ni_guest_layouts.h NiTextureFormatPrefs names +0x18 alphaFormat and +0x1C mipMapped - swapped, reported.)
struct XboxFormatPrefs {
    std::uint32_t pixelLayout = 5;  // +0x14: 0 PALETTIZED_8, 1 HIGH_COLOR_16, 2 TRUE_COLOR_32, 3 COMPRESSED,
                                    //        4 BUMPMAP, 5 PIX_DEFAULT
    std::uint32_t mipMapped = 2;    // +0x18: 0 NO, 1 YES, 2 MIP_DEFAULT
    std::uint32_t alphaFormat = 3;  // +0x1C: 0 NONE, 1 BINARY, 2 SMOOTH, 3 ALPHA_DEFAULT
};
inline constexpr std::uint32_t kNiSourceTextureFormatPrefs = 0x14;
// Reads the three dwords at sourceTexture + 0x14 (defaults if sourceTexture is 0 or unreadable).
XboxFormatPrefs ReadFormatPrefs(ni::GuestPtr sourceTexture) noexcept;
// FUN_001572D0 with the full table and DAT_0030EF84 (global "mipmap by default") = 0 (BSS, only read at
// 0x1573AE, 0x15E148, 0x15EF3A): the first entry in the prefs-dependent order whose descriptor equals the
// source's, or None.
XboxFormat ChooseXboxFormat(const ni::NiPixelFormat& source, const XboxFormatPrefs& prefs) noexcept;

// ---- slot 65 source decision ------------------------------------------------------------------------------

// Slot 65 decision (see the header comment). `fileImage` = result of the NiImageConverter read (0 =
// failed / not attempted), `pixelData` = NiSourceTexture +0x34, width/height = level 0 of the chosen image.
enum class TextureSource : std::uint8_t {
    FileImage,
    PixelData,
    DefaultNoImage,
    DefaultUnreadable,
    DefaultNotLoaded,  // [render] load_textures = 0
    DefaultNotPowerOfTwo,
    DefaultUnsupported,   // ConvertPixelData failed (decided by the caller after converting)
    DefaultNoXboxFormat,  // FUN_001572D0 found no table entry (only checked if imageFormat != nullptr)
};
struct TextureSourceInput {
    bool hasPath = false;
    bool loadTextures = true;
    ni::GuestPtr fileImage = 0;
    ni::GuestPtr pixelData = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Optional step 3: NiPixelFormat of the chosen image (NiPixelData +0x0C) and the NiSourceTexture's
    // FormatPrefs; nullptr skips the check (the port then shows formats the Xbox replaced by the default).
    const ni::NiPixelFormat* imageFormat = nullptr;
    XboxFormatPrefs prefs{};
};
TextureSource SelectTextureSource(const TextureSourceInput& input) noexcept;
const char* TextureSourceName(TextureSource source) noexcept;
// FUN_0015E010's test: non-zero and a power of two.
bool IsPowerOfTwo(std::uint32_t value) noexcept;

}  // namespace gfx::tex
