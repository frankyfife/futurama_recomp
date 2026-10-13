// Movie packs (M6b): upscaled replacement pictures for the game's XMV movies, made by tools/movie_upscale.py.
//
// A pack is a folder ([render] movie_pack) with one H.264 (or HEVC) .mp4 per movie and index.tsv:
//   # comment lines
//   <name> \t <header_crc32 hex> \t <xmv_bytes> \t <frames> \t <width> \t <height> \t <duration_ms> \t <file>
// header_crc32 = zlib CRC-32 of the first 0x1000 bytes of the .xmv - exactly the header the game reads before
// XMVDecoder_CreateDecoderForPackets (Open 0xEE336), so the XMV HLE finds the pack entry of the movie being opened
// without knowing its file name, and a pack made from another disc never matches. Byte-identical movies (Intro =
// 1-1-00) share one entry's file.
//
// Frame mapping: the mp4 holds exactly the XMV's frames in the same order with the same pts (variable frame rate,
// ms time scale, no B-frames). The XMV HLE keeps demuxing and decoding the XMV as before (audio, clock, end of the
// movie, the 640x480 picture as fallback); the n-th XMV frame it shows is replaced by the n-th mp4 frame, after
// checking that both have the same pts relative to their first frame (PackPtsMatch). Any mismatch, a missing frame
// or a decoder error ends the use of the pack for that movie (the XMV picture continues, logged).
//
// Pure code apart from LoadPackIndex (file read); the Media Foundation reader is video/movie_pack_reader.h.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vid {

inline constexpr std::uint32_t kPackHeaderBytes = 0x1000;  // bytes of the .xmv covered by header_crc32
inline constexpr std::uint32_t kPackMaxDimension = 4096;   // largest accepted picture side
inline constexpr std::uint32_t kPackPtsToleranceMs = 1;    // allowed |mp4 - xmv| pts difference (rounding)

struct PackEntry {
    std::string name;            // movie name without extension (e.g. "Intro-PAL")
    std::uint32_t headerCrc = 0;
    std::uint64_t xmvBytes = 0;  // informational
    std::uint32_t frames = 0;
    std::uint32_t width = 0;     // picture size of the mp4
    std::uint32_t height = 0;
    std::uint32_t durationMs = 0;
    std::string file;            // file name inside the pack folder
};

// Parses index.tsv. Comment and empty lines are skipped; a malformed line is skipped and counted in `badLines`
// (first problem in `error`). Rejected: missing columns, bad numbers, odd or zero or too large sizes, file names
// that leave the pack folder (separators, ".."), names not ending in .mp4.
std::vector<PackEntry> ParsePackIndex(std::string_view text, std::uint32_t& badLines, std::string& error);

// The entry for a movie: same header CRC; its picture must be at least the XMV's size and keep its aspect ratio
// (an integer or fractional scale of both sides by the same factor). nullptr = none.
const PackEntry* FindPackEntry(const std::vector<PackEntry>& entries, std::uint32_t headerCrc,
                               std::uint32_t xmvWidth, std::uint32_t xmvHeight) noexcept;

// True when the mp4 frame and the XMV frame of the same index show the same moment: pts relative to the first frame
// of each stream equal within kPackPtsToleranceMs.
inline bool PackPtsMatch(std::uint32_t xmvFirstPts, std::uint32_t xmvPts, std::int64_t packFirstPts,
                         std::int64_t packPts) noexcept {
    const std::int64_t a = static_cast<std::int64_t>(xmvPts) - static_cast<std::int64_t>(xmvFirstPts);
    const std::int64_t b = packPts - packFirstPts;
    const std::int64_t d = a > b ? a - b : b - a;
    return d <= static_cast<std::int64_t>(kPackPtsToleranceMs);
}

// 100-ns Media Foundation time -> ms (rounded to nearest).
inline std::int64_t HnsToMs(std::int64_t hns) noexcept {
    return hns >= 0 ? (hns + 5000) / 10000 : -((-hns + 5000) / 10000);
}

// What the reader does with its oldest decoded frame when frame `wanted` is requested.
enum class PackQueueStep : std::uint8_t {
    Discard,  // older than wanted (the XMV HLE skipped late frames): drop it
    Take,     // the wanted frame
    Ahead,    // newer than wanted: the wanted frame was lost - error
};
inline PackQueueStep PackStepFor(std::uint32_t queued, std::uint32_t wanted) noexcept {
    return queued < wanted ? PackQueueStep::Discard : (queued == wanted ? PackQueueStep::Take : PackQueueStep::Ahead);
}

// Reads and parses <dir>/index.tsv. False (with `error`) when the folder or the index is missing or has no entry.
bool LoadPackIndex(const std::filesystem::path& dir, std::vector<PackEntry>& out, std::string& error);

}  // namespace vid
