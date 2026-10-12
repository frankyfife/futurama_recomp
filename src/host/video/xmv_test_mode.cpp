// Movie decoder test mode (see xmv_test_mode.h).
#include "video/xmv_test_mode.h"

#include "audio/codec_adpcm.h"
#include "core/cli_tools.h"
#include "core/config.h"
#include "gfx/texconv.h"
#include "vfs/vfs.h"
#include "video/movie_core.h"
#include "video/yuv_pack.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

namespace vid {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

bool EndsWithXmv(const std::string& name) {
    const std::string l = Lower(name);
    return l.size() > 4 && l.compare(l.size() - 4, 4, ".xmv") == 0;
}

struct MovieFile {
    std::string name;
    std::vector<std::uint8_t> bytes;
};

struct TrackResult {
    std::uint32_t rate = 0, channels = 0;
    std::uint64_t frames = 0;
    std::uint32_t crc = 0;
};

struct FileResult {
    std::string name;
    std::uint64_t size = 0;
    bool ok = false;
    std::string error;
    std::uint32_t width = 0, height = 0, durationMs = 0, packets = 0;
    std::uint32_t framesDemuxed = 0, framesDecoded = 0, decodeErrors = 0, extradataChanges = 0, keyframes = 0;
    std::uint32_t maxGapMs = 0, minGapMs = 0;
    double decodeMsPerFrame = 0;
    std::vector<std::uint32_t> pts;
    std::vector<std::uint32_t> lumaCrc;
    std::vector<TrackResult> tracks;
};

bool WriteFile(const std::filesystem::path& path, const void* data, std::size_t size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

FileResult DecodeOne(const MovieFile& file, const std::filesystem::path& dumpDir) {
    FileResult r;
    r.name = file.name;
    r.size = file.bytes.size();
    xmv::FileHeader header;
    std::string error;
    if (!xmv::ParseFileHeader(file.bytes, header, error)) {
        r.error = error;
        return r;
    }
    r.width = header.width;
    r.height = header.height;
    r.durationMs = header.durationMs;
    MemoryPacketSource source(file.bytes);
    MovieCore core;
    core.Open(&source, header.firstPacketSize, 0xFFFFu);
    std::vector<std::uint32_t> trackCrc(header.tracks.size(), 0);
    std::vector<std::uint64_t> trackFrames(header.tracks.size(), 0);
    std::vector<std::int16_t> pcm;
    I420Frame picture;
    std::vector<std::uint8_t> yuy2, bgra;
    std::filesystem::path dir;
    if (!dumpDir.empty()) {
        dir = dumpDir / file.name;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    double decodeSeconds = 0;
    std::uint32_t frameIndex = 0;
    // Pass 1: demux everything (cheap) to know the frame count for the dump positions.
    while (core.ReadPacket()) {
    }
    if (core.Failed()) {
        r.error = core.Error();
    }
    const std::uint32_t total = static_cast<std::uint32_t>(core.Video().size());
    std::array<std::uint32_t, 5> dumpAt = {0, total / 4, total / 2, (3 * total) / 4, total > 0 ? total - 1 : 0};
    // Audio of every track.
    for (std::uint32_t t = 0; t < header.tracks.size(); ++t) {
        const xmv::AudioTrackInfo& info = header.tracks[t];
        for (const xmv::AudioChunk& c : core.Audio(t)) {
            if (info.formatTag != xmv::kFormatXboxAdpcm || info.channels == 0) {
                continue;
            }
            const std::size_t frames = snd::codec::XboxAdpcmFrames(c.data.size(), info.channels, true);
            pcm.resize(frames * info.channels);
            const std::size_t got = snd::codec::DecodeXboxAdpcm(c.data.data(), c.data.size(), info.channels,
                                                                pcm.data(), frames, true);
            trackCrc[t] = Crc32(pcm.data(), got * info.channels * sizeof(std::int16_t), trackCrc[t]);
            trackFrames[t] += got;
        }
        TrackResult tr;
        tr.rate = info.sampleRate;
        tr.channels = info.channels;
        tr.frames = trackFrames[t];
        tr.crc = trackCrc[t];
        r.tracks.push_back(tr);
    }
    // Video: decode every frame in order.
    std::uint32_t lastPts = 0;
    bool first = true;
    while (!core.Video().empty() && !core.Failed()) {
        const xmv::VideoFrame v = std::move(core.Video().front());
        core.Video().pop_front();
        r.pts.push_back(v.ptsMs);
        if (v.keyframe) {
            ++r.keyframes;
        }
        if (!first) {
            const std::uint32_t gap = v.ptsMs - lastPts;
            r.maxGapMs = std::max(r.maxGapMs, gap);
            r.minGapMs = r.minGapMs == 0 ? gap : std::min(r.minGapMs, gap);
        }
        first = false;
        lastPts = v.ptsMs;
        bool produced = false;
        QueryPerformanceCounter(&t0);
        const bool decodedOk = core.Decode(v, picture, produced);
        QueryPerformanceCounter(&t1);
        decodeSeconds += static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
        if (!decodedOk && core.Failed()) {
            r.error = core.Error();
            break;
        }
        r.lumaCrc.push_back(produced ? Crc32(picture.Y(), static_cast<std::size_t>(picture.width) * picture.height)
                                     : 0u);
        if (produced && !dir.empty() && std::find(dumpAt.begin(), dumpAt.end(), frameIndex) != dumpAt.end()) {
            char name[64];
            std::snprintf(name, sizeof(name), "frame_%05u.i420", frameIndex);
            WriteFile(dir / name, picture.data.data(), picture.data.size());
            if (frameIndex == dumpAt[2]) {
                yuy2.resize(static_cast<std::size_t>(picture.width) * picture.height * 2u);
                bgra.resize(static_cast<std::size_t>(picture.width) * picture.height * 4u);
                PackI420ToYuy2(picture, yuy2.data(), picture.width * 2u);
                gfx::tex::ConvertYuy2ToBgra(yuy2.data(), picture.width * 2u, picture.width, picture.height,
                                            bgra.data(), picture.width * 4u, gfx::tex::Yuy2Mode::ConvertBt601);
                std::snprintf(name, sizeof(name), "display_%05u.bgra", frameIndex);
                WriteFile(dir / name, bgra.data(), bgra.size());
            }
        }
        ++frameIndex;
    }
    const MovieCoreStats& s = core.Stats();
    r.packets = s.packets;
    r.framesDemuxed = s.framesDemuxed;
    r.framesDecoded = s.framesDecoded;
    r.decodeErrors = s.decodeErrors;
    r.extradataChanges = s.extradataChanges;
    r.decodeMsPerFrame = r.framesDecoded != 0 ? decodeSeconds * 1000.0 / r.framesDecoded : 0.0;
    r.ok = r.error.empty() && r.decodeErrors == 0 && r.framesDemuxed != 0 && r.framesDecoded == r.framesDemuxed &&
           core.Eof();
    if (r.error.empty() && !r.ok) {
        r.error = "frames demuxed " + std::to_string(r.framesDemuxed) + ", decoded " + std::to_string(r.framesDecoded) +
                  ", decode errors " + std::to_string(r.decodeErrors) + (core.Eof() ? "" : ", chain not finished");
    }
    return r;
}

bool CollectFromImage(const std::filesystem::path& image, const std::string& filter, std::vector<MovieFile>& out,
                      std::string& error) {
    vfs::XdvdfsImage iso;
    if (!iso.Open(image, error)) {
        return false;
    }
    std::vector<vfs::XdvdfsImage::Entry> entries;
    if (!iso.ListDirectory("\\video", entries)) {
        error = "no \\video folder on the disc image";
        return false;
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return Lower(a.name) < Lower(b.name); });
    for (const auto& e : entries) {
        if (e.IsDirectory() || !EndsWithXmv(e.name) ||
            (!filter.empty() && Lower(e.name).find(Lower(filter)) == std::string::npos)) {
            continue;
        }
        MovieFile m;
        m.name = e.name;
        m.bytes.resize(e.size);
        std::uint32_t got = 0;
        if (!iso.Read(e, 0, m.bytes.data(), e.size, got) || got != e.size) {
            error = "read error: \\video\\" + e.name;
            return false;
        }
        out.push_back(std::move(m));
    }
    return true;
}

bool CollectFromFolder(const std::filesystem::path& folder, const std::string& filter, std::vector<MovieFile>& out,
                       std::string& error) {
    std::filesystem::path dir = folder;
    std::error_code ec;
    for (const char* sub : {"video", "Video"}) {
        if (std::filesystem::is_directory(folder / sub, ec)) {
            dir = folder / sub;
            break;
        }
    }
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (e.is_regular_file(ec) && EndsWithXmv(name) &&
            (filter.empty() || Lower(name).find(Lower(filter)) != std::string::npos)) {
            files.push_back(e.path());
        }
    }
    if (ec) {
        error = "cannot list " + dir.string();
        return false;
    }
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        MovieFile m;
        m.name = p.filename().string();
        std::ifstream f(p, std::ios::binary);
        m.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        out.push_back(std::move(m));
    }
    return true;
}

}  // namespace

std::uint32_t Crc32(const void* data, std::size_t size, std::uint32_t crc) noexcept {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    const auto* p = static_cast<const std::uint8_t*>(data);
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

std::string DecodeMoviesJson(const DecodeMoviesOptions& options, bool& allOk) {
    allOk = false;
    std::vector<MovieFile> files;
    std::string error;
    std::error_code ec;
    const bool folder = std::filesystem::is_directory(options.source, ec);
    const bool collected = folder ? CollectFromFolder(options.source, options.filter, files, error)
                                  : CollectFromImage(options.source, options.filter, files, error);
    std::string decoderError;
    const bool decoder = WmvDecoderAvailable(&decoderError);
    core::cli::JsonWriter w;
    w.BeginObject();
    w.Key("source").String(core::WideToUtf8(options.source.wstring()));
    w.Key("decoder").String(decoder ? "Windows WMVideo Decoder DMO (wmvdecod.dll)" : decoderError);
    std::uint32_t ok = 0, failed = 0;
    std::uint64_t frames = 0;
    w.Key("files").BeginArray();
    for (const MovieFile& f : files) {
        const FileResult r = DecodeOne(f, options.dumpDir);
        ok += r.ok ? 1u : 0u;
        failed += r.ok ? 0u : 1u;
        frames += r.framesDecoded;
        std::fprintf(stderr, "%-24s %s: %u frames, %u decode errors%s%s\n", r.name.c_str(), r.ok ? "ok" : "FAILED",
                     r.framesDecoded, r.decodeErrors, r.error.empty() ? "" : " - ", r.error.c_str());
        w.BeginObject();
        w.Key("name").String(r.name);
        w.Key("ok").Bool(r.ok);
        w.Key("error").String(r.error);
        w.Key("size").UInt(r.size);
        w.Key("width").UInt(r.width);
        w.Key("height").UInt(r.height);
        w.Key("duration_ms").UInt(r.durationMs);
        w.Key("packets").UInt(r.packets);
        w.Key("frames_demuxed").UInt(r.framesDemuxed);
        w.Key("frames_decoded").UInt(r.framesDecoded);
        w.Key("decode_errors").UInt(r.decodeErrors);
        w.Key("keyframes").UInt(r.keyframes);
        w.Key("extradata_changes").UInt(r.extradataChanges);
        w.Key("min_gap_ms").UInt(r.minGapMs);
        w.Key("max_gap_ms").UInt(r.maxGapMs);
        w.Key("decode_ms_per_frame").Number(r.decodeMsPerFrame);
        w.Key("tracks").BeginArray();
        for (const TrackResult& t : r.tracks) {
            w.BeginObject(true);
            w.Key("rate").UInt(t.rate);
            w.Key("channels").UInt(t.channels);
            w.Key("frames").UInt(t.frames);
            w.Key("pcm_crc32").UInt(t.crc);
            w.EndObject();
        }
        w.EndArray();
        w.Key("pts").BeginArray(true);
        for (const std::uint32_t p : r.pts) {
            w.UInt(p);
        }
        w.EndArray();
        w.Key("luma_crc32").BeginArray(true);
        for (const std::uint32_t c : r.lumaCrc) {
            w.UInt(c);
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.Key("files_ok").UInt(ok);
    w.Key("files_failed").UInt(failed);
    w.Key("frames_decoded").UInt(frames);
    w.Key("error").String(collected ? (files.empty() ? "no .xmv files found" : "") : error);
    w.EndObject();
    allOk = collected && decoder && !files.empty() && failed == 0;
    return w.Take();
}

}  // namespace vid
