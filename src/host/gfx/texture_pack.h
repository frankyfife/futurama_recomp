// G2: texture replacement packs and the texture dump (ROADMAP G2, design 8.22, src/INTERFACES.md 4.38,
// docs/g2_report.md).
//
// [render] texture_pack = <folder>   replacement files, looked up by the content hash of the original image
//                                    (empty = off: nothing below runs, no hash is computed)
// [render] texture_pack_dump = 1     writes every static game texture once as <userdata>\texture_dump\<hash>.png
//                                    (level 0 decoded to RGBA8, alpha kept) plus a line in index.tsv
//
// ---- content hash (scheme "FTX1") --------------------------------------------------------------------------------
// XXH64 (seed 0) over the ORIGINAL Xbox image as the game hands it to slot 65 (NiPixelData), independent of what the
// port converts it to (keep16Bit, DXT decompression, mip limits):
//   1. a 40-byte header of ten little-endian u32: 'FTX1' (0x31585446), NiPixelFormat +0x00 format (0 RGB .. 9
//      BUMPLUMA), bits per pixel (+0x14 byte), level-0 width, level-0 height, red, green, blue and alpha mask
//      (+0x04..+0x10), byte count of level 0;
//   2. the stored bytes of level 0 (offsets[1] - offsets[0]; DXT blocks / D3D8 texels exactly as on the disc);
//   3. for PAL/PALA only: the 256 NiPalette entries (r, g, b, a = 1024 bytes) and the palette alpha flag (1 byte).
// The stored mip levels are not hashed: they follow from level 0 on the disc, and a replacement brings its own.
// File names: the hash as 16 lowercase hex digits; a pack file is "<hash>.dds" or "<hash>.png", optionally with a
// suffix after a non-hex character ("<hash>_256x256.png"); upper-case hex is accepted. Sub-folders are scanned.
//
// ---- what is replaced ---------------------------------------------------------------------------------------------
// Only NiSourceTextures whose image the GPU path uploads (FillSourceTextureData -> UploadImage) and whose "static"
// byte (+0x38) is set. Excluded: the default 8x8 texture, movie textures (YUY2 shim textures, texel-addressed UVs),
// rendered textures (render targets), non-static textures (the game rewrites their NiPixelData; FUN_0015E260
// re-uploads them on a revision change). The game keeps seeing the original sizes, formats and level counts
// (NiPCTextureData +0x48/+0x4C/+0x50); only the host texture changes. UVs of source textures are normalized, so any
// replacement size works (power of two not required; BCn needs level-0 width/height divisible by 4, a D3D11 rule).
//
// ---- formats ----------------------------------------------------------------------------------------------------
// PNG (any WIC-readable image, decoded to BGRA8): a full mip chain is generated (alpha-weighted box filter).
// DDS: BC1 (DXT1), BC2 (DXT3), BC3 (DXT5), BC7 (DX10 header), B8G8R8A8 / B8G8R8X8 / R8G8B8A8 (legacy masks or DX10),
// with the stored mip levels (no generation for BCn; a DDS without mips stays one level). _SRGB variants are read as
// UNORM (the game renders gamma-encoded, like the Xbox). Cube maps, volumes and arrays are rejected.
//
// ---- loading --------------------------------------------------------------------------------------------------
// The pack folder is indexed once (file names only). A hit uploads the original first and queues the file on a
// worker thread; the texture resolver (ResolveTexture, every draw) swaps in the replacement as soon as it is
// decoded, usually before the loading screen ends. Decoded images stay in a small LRU cache (kCacheBudgetBytes) for
// level restarts. Mip filter NONE (D3DTEXF_NONE: the original samples level 0 only): the sampler of a replacement
// samples levels 0..log2(replacement / original size) instead (AdjustSampler), so the replacement is not minified
// without mips where the original was shown 1:1.
#pragma once

#include "gfx/ni_guest_layouts.h"
#include "gfx/rhi/rhi.h"
#include "gfx/texconv.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace gfx {
class GpuRenderer;
struct NiPCTextureData;
}  // namespace gfx

namespace gfx::texpack {

// ---- hashing ----------------------------------------------------------------------------------------------------

// XXH64 (Yann Collet's xxHash, 64-bit variant) - streaming and one-shot.
class Xxh64 {
public:
    explicit Xxh64(std::uint64_t seed = 0) noexcept;
    void Update(const void* data, std::size_t size) noexcept;
    std::uint64_t Digest() const noexcept;

private:
    std::uint64_t v_[4];
    std::uint64_t seed_;
    std::uint64_t total_ = 0;
    std::uint8_t buffer_[32];
    std::uint32_t buffered_ = 0;
};
std::uint64_t Xxh64Hash(const void* data, std::size_t size, std::uint64_t seed = 0) noexcept;

inline constexpr std::uint32_t kHashTag = 0x31585446u;  // "FTX1" little-endian

// The hashed parts of an original image (host memory; unit tests and HashGuestPixelData).
struct HashSource {
    ni::NiPixelFormat format{};
    std::uint32_t width = 0;   // level 0
    std::uint32_t height = 0;
    const std::uint8_t* level0 = nullptr;
    std::uint32_t level0Size = 0;
    const std::uint8_t* paletteRgba = nullptr;  // 1024 bytes (PAL/PALA only; nullptr = no palette)
    bool paletteHasAlpha = false;
};
std::uint64_t TextureHash(const HashSource& source) noexcept;

// Hash of a guest NiPixelData (palette from +0x2C). False if the object, level 0 or its pixels are unreadable.
struct ImageInfo {
    std::uint32_t width = 0, height = 0, mipCount = 0, format = 0, bitsPerPixel = 0;
};
bool HashGuestPixelData(ni::GuestPtr pixelData, std::uint64_t* hash, ImageInfo* info) noexcept;

// "0123456789abcdef" (16 lowercase hex digits).
std::string HashName(std::uint64_t hash);
// First 16 characters = hex digits (any case) and, if more follow, the 17th is not a hex digit.
bool ParseHashName(std::string_view stem, std::uint64_t* hash) noexcept;
// "DXT1", "RGBA16", "RGB16", "RGBA32", "PAL8", ... for index.tsv and the log.
const char* SourceFormatName(std::uint32_t format, std::uint32_t bitsPerPixel) noexcept;

// ---- file formats ----------------------------------------------------------------------------------------------

enum class LoadStatus : std::uint8_t {
    Ok,
    NotFound,      // file missing or unreadable
    NotDds,        // magic / header size wrong
    Unsupported,   // pixel format, cube map, volume, array
    Truncated,     // fewer bytes than the levels need
    BadSize,       // 0, > tex::kMaxDimension, BCn level 0 not divisible by 4
    DecodeFailed,  // WIC could not read the image
    OutOfMemory,
};
const char* LoadStatusName(LoadStatus status) noexcept;

// Parses a DDS file in memory into `out` (format, levels, bytes; mipCount = stored levels, clamped to the full chain
// and kMaxMipLevels). `detail` (optional) names the reason of a failure.
LoadStatus ParseDds(const std::uint8_t* data, std::size_t size, tex::HostImage* out, std::string* detail) noexcept;

// BGRA8 level 0 (rows tightly packed, `pixels` = width * height * 4 bytes) -> `out` with the full mip chain
// (max(1, w >> i) x max(1, h >> i) down to 1 x 1; each level an alpha-weighted box average of the level above).
bool MakeMippedBgra(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, tex::HostImage* out) noexcept;

// Reads a replacement file: .dds through ParseDds, anything else through WIC (platform::ReadImageFile) +
// MakeMippedBgra.
LoadStatus LoadReplacementFile(const std::filesystem::path& path, tex::HostImage* out, std::string* detail) noexcept;

// Number of mip levels that `replacement` is larger than `original` (log2 of the smaller axis ratio, rounded down,
// 0..15): the sampler extension of AdjustSampler.
std::uint32_t ExtraLevels(std::uint32_t originalWidth, std::uint32_t originalHeight, std::uint32_t replacementWidth,
                          std::uint32_t replacementHeight) noexcept;

// ---- the pack (index + asynchronous loader) ------------------------------------------------------------------

inline constexpr std::size_t kCacheBudgetBytes = 96u << 20;  // decoded images kept for later uses

class Pack {
public:
    enum class State : std::uint8_t { Missing, Pending, Ready, Failed };

    Pack();
    ~Pack();
    Pack(const Pack&) = delete;
    Pack& operator=(const Pack&) = delete;

    // Indexes `folder` (recursive; .dds wins over .png for the same hash). False + reason if it is not a folder.
    bool Open(const std::filesystem::path& folder, std::string* error);
    std::size_t FileCount() const noexcept;
    std::filesystem::path FileOf(std::uint64_t hash) const;  // empty if not in the pack
    // Queues the decode of `hash` (no-op if cached or queued). False if the pack has no file for it.
    bool Request(std::uint64_t hash);
    // Ready -> *image = the decoded image (shared with the cache). Missing = never requested / not in the pack.
    State Fetch(std::uint64_t hash, std::shared_ptr<const tex::HostImage>* image);
    // Blocks until nothing is queued or decoding, at most `timeoutMs` (tests). True if idle.
    bool WaitIdle(std::uint32_t timeoutMs);

    struct Stats {
        std::uint32_t files = 0, dds = 0, png = 0, duplicates = 0;
        std::uint32_t decoded = 0, failed = 0, cacheHits = 0, evicted = 0;
        std::uint64_t decodedBytes = 0;
        double decodeMsTotal = 0.0, decodeMsMax = 0.0;
    };
    Stats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- renderer integration (nipc_renderer.cpp, ff_pipeline.cpp) ----------------------------------------------------

// NiPCTextureData::hostInfo bits (next to kHostOwnsTexture 0x100).
inline constexpr std::uint32_t kHostReplacementPending = 0x200;  // a pack file is being loaded for this data
inline constexpr std::uint32_t kHostReplaced = 0x400;            // hostTexture is the pack's texture

// True if [render] texture_pack or texture_pack_dump is set (reads the settings once; opens the pack and logs).
bool Enabled() noexcept;

// After UploadImage created the original host texture for `data` (owned, static texture): hashes the guest image,
// dumps it ([render] texture_pack_dump) and, on a pack hit, sets kHostReplacementPending and queues the file.
void OnTextureUploaded(GpuRenderer& gpu, NiPCTextureData* data, ni::GuestPtr niTexture, ni::GuestPtr pixelData) noexcept;

// From the texture resolver when kHostReplacementPending is set: swaps in the decoded replacement (creates the host
// texture, destroys the original, sets kHostReplaced) or drops the request after a failure.
void ApplyPending(GpuRenderer& gpu, NiPCTextureData* data) noexcept;

// From the data destructor (before its host texture is destroyed) when either bit is set.
void OnDataDestroyed(NiPCTextureData* data) noexcept;

// Sampler of a stage that samples `texture`: a replacement with mip filter NONE gets levels 0..ExtraLevels
// (linear between them) instead of level 0 only. No-op for every other texture (one relaxed atomic load when no
// replacement exists).
void AdjustSampler(rhi::TextureHandle texture, rhi::SamplerDesc* desc) noexcept;

// Exit statistics (GFX/info) and flush of the queued dump files (at most 5 s). Called by the gfx exit reporter.
void LogStatistics() noexcept;

}  // namespace gfx::texpack
