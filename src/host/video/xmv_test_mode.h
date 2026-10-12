// Host test mode of the movie decoder (M6 exit criterion 3: "Ein Testmodus dekodiert alle 47 Dateien vollständig ohne
// Fehler"). Tool mode of futurama.exe (core/cli_tools.h), no game run:
//
//   futurama.exe --decode-movies=<disc image | folder> --result=<file.json> [--movie-dump=<dir>]
//                [--movie-filter=<substring>]
//
// Every .xmv in the image's \video folder (a folder: its *.xmv files, or those in its video subfolder) is demuxed
// and decoded completely: all WMV2 frames through the Windows decoder (video/wmv_decoder.h), every audio track through
// the M5 Xbox ADPCM decoder. Per file the JSON lists the frame pts, the CRC-32 (zlib) of every decoded luma plane and
// of every track's interleaved PCM16, counts and errors, so tools/m6_check.py can compare all of it with FFmpeg
// (PyAV) without dumping gigabytes. --movie-dump writes, per file, five frames (first, 25 %, 50 %, 75 %, last) as raw
// I420 (<name>/frame_<index>.i420) and the middle frame through the display path (I420 -> YUY2 -> BGRA, as the
// renderer shows it: <name>/display_<index>.bgra) for PSNR checks.
// Exit code 0 = every file decoded without an error, 2 = at least one file failed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace vid {

struct DecodeMoviesOptions {
    std::filesystem::path source;   // disc image or folder
    std::filesystem::path dumpDir;  // empty = no dump
    std::string filter;             // substring of the file name (case-insensitive), empty = all
};

std::string DecodeMoviesJson(const DecodeMoviesOptions& options, bool& allOk);

// zlib CRC-32 (polynomial 0xEDB88320), continuing from `crc` (0 to start).
std::uint32_t Crc32(const void* data, std::size_t size, std::uint32_t crc = 0) noexcept;

}  // namespace vid
