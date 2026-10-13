// Movie packs (M6b, see movie_pack.h): index parsing and lookup.
#include "video/movie_pack.h"

#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>

namespace vid {
namespace {

std::vector<std::string_view> SplitTabs(std::string_view line) {
    std::vector<std::string_view> cols;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        cols.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos) {
            break;
        }
        start = tab + 1;
    }
    return cols;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

template <typename T>
bool Number(std::string_view s, T& out, int base = 10) {
    s = Trim(s);
    if (s.empty()) {
        return false;
    }
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out, base);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool SafeFileName(std::string_view f) {
    if (f.empty() || f.find_first_of("/\\:") != std::string_view::npos || f.find("..") != std::string_view::npos) {
        return false;
    }
    if (f.size() < 5) {
        return false;
    }
    const std::string_view ext = f.substr(f.size() - 4);
    return ext == ".mp4" || ext == ".MP4";
}

}  // namespace

std::vector<PackEntry> ParsePackIndex(std::string_view text, std::uint32_t& badLines, std::string& error) {
    std::vector<PackEntry> entries;
    badLines = 0;
    std::size_t lineNo = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto cols = SplitTabs(line);
        PackEntry e;
        const char* problem = nullptr;
        if (cols.size() < 8) {
            problem = "fewer than 8 columns";
        } else if (!Number(cols[1], e.headerCrc, 16) || !Number(cols[2], e.xmvBytes) || !Number(cols[3], e.frames) ||
                   !Number(cols[4], e.width) || !Number(cols[5], e.height) || !Number(cols[6], e.durationMs)) {
            problem = "bad number";
        } else if (e.width == 0 || e.height == 0 || (e.width & 1u) != 0 || e.width > kPackMaxDimension ||
                   e.height > kPackMaxDimension || e.frames == 0) {
            problem = "bad picture size or frame count";
        } else if (!SafeFileName(Trim(cols[7]))) {
            problem = "file must be a plain .mp4 name inside the pack folder";
        }
        if (problem != nullptr) {
            if (badLines++ == 0) {
                error = "line " + std::to_string(lineNo) + ": " + problem;
            }
            continue;
        }
        e.name = std::string(Trim(cols[0]));
        e.file = std::string(Trim(cols[7]));
        entries.push_back(std::move(e));
    }
    return entries;
}

const PackEntry* FindPackEntry(const std::vector<PackEntry>& entries, std::uint32_t headerCrc, std::uint32_t xmvWidth,
                               std::uint32_t xmvHeight) noexcept {
    for (const PackEntry& e : entries) {
        if (e.headerCrc != headerCrc) {
            continue;
        }
        // Same aspect ratio (w * H == h * W) and not smaller than the XMV picture.
        if (xmvWidth == 0 || xmvHeight == 0 || e.width < xmvWidth || e.height < xmvHeight ||
            static_cast<std::uint64_t>(e.width) * xmvHeight != static_cast<std::uint64_t>(e.height) * xmvWidth) {
            continue;
        }
        return &e;
    }
    return nullptr;
}

bool LoadPackIndex(const std::filesystem::path& dir, std::vector<PackEntry>& out, std::string& error) {
    out.clear();
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
        error = "not a folder";
        return false;
    }
    std::ifstream f(dir / "index.tsv", std::ios::binary);
    if (!f) {
        error = "no index.tsv";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::uint32_t bad = 0;
    std::string lineError;
    out = ParsePackIndex(ss.str(), bad, lineError);
    if (out.empty()) {
        error = bad != 0 ? "index.tsv has no valid entry (" + lineError + ")" : "index.tsv has no entry";
        return false;
    }
    if (bad != 0) {
        error = std::to_string(bad) + " malformed line(s) skipped (" + lineError + ")";
    }
    return true;
}

}  // namespace vid
