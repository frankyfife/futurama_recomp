// Path helpers of the VFS (design 4.2 items 2/3): normalization, case-insensitive comparison,
// wildcard masks, guest ANSI <-> UTF-16 and Win32 error mapping.
#include "vfs/vfs.h"
#include "vfs/vfs_internal.h"

#include "shared/nt_status.h"

#include <cstdio>

namespace vfs {
namespace {

constexpr char FoldAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// Recursive-free wildcard matcher ('*' any run, '?' one char) with backtracking on the last '*'.
bool MatchFolded(std::string_view name, std::string_view mask) noexcept {
    std::size_t n = 0, m = 0;
    std::size_t starM = std::string_view::npos, starN = 0;
    while (n < name.size()) {
        if (m < mask.size() && (mask[m] == '?' || FoldAscii(mask[m]) == FoldAscii(name[n]))) {
            ++n;
            ++m;
        } else if (m < mask.size() && mask[m] == '*') {
            starM = m++;
            starN = n;
        } else if (starM != std::string_view::npos) {
            m = starM + 1;
            n = ++starN;
        } else {
            return false;
        }
    }
    while (m < mask.size() && mask[m] == '*') {
        ++m;
    }
    return m == mask.size();
}

}  // namespace

std::string NormalizePath(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (c == '/') {
            c = '\\';
        }
        if (c == '\\' && !out.empty() && out.back() == '\\') {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (FoldAscii(a[i]) != FoldAscii(b[i])) {
            return false;
        }
    }
    return true;
}

bool IStartsWith(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && IEquals(s.substr(0, prefix.size()), prefix);
}

bool MatchFileMask(std::string_view name, std::string_view mask) noexcept {
    if (mask.empty()) {
        return true;
    }
    // DOS wildcards (NT FsRtl): '<' ~ '*', '>' ~ '?', '"' ~ '.'. XAPI only turns "*.*" into an empty
    // mask (FUN_000f8961), so the plain forms are what the guest passes; the DOS forms are a courtesy.
    std::string m(mask);
    for (char& c : m) {
        if (c == '<') {
            c = '*';
        } else if (c == '>') {
            c = '?';
        } else if (c == '"') {
            c = '.';
        }
    }
    return MatchFolded(name, m);
}

namespace detail {

std::vector<std::string> SplitPath(std::string_view relativePath) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : relativePath) {
        if (c == '\\' || c == '/') {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        parts.push_back(std::move(current));
    }
    return parts;
}

std::string JoinPath(const std::vector<std::string>& components) {
    if (components.empty()) {
        return "\\";
    }
    std::string out;
    for (const std::string& c : components) {
        out.push_back('\\');
        out += c;
    }
    return out;
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = FoldAscii(c);
    }
    return out;
}

std::wstring GuestToWide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

bool WideToGuest(std::wstring_view s, std::string& out) {
    out.clear();
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (static_cast<unsigned>(c) > 0xFF) {
            return false;
        }
        out.push_back(static_cast<char>(static_cast<unsigned char>(c)));
    }
    return true;
}

NTSTATUS Win32ErrorToStatus(unsigned long error) noexcept {
    switch (error) {
    case ERROR_SUCCESS:
        return STATUS_SUCCESS;
    case ERROR_FILE_NOT_FOUND:
        return STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_PATH_NOT_FOUND:
        return STATUS_OBJECT_PATH_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
        return STATUS_ACCESS_DENIED;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return STATUS_OBJECT_NAME_COLLISION;
    case ERROR_SHARING_VIOLATION:
        return STATUS_SHARING_VIOLATION;
    case ERROR_LOCK_VIOLATION:
        return STATUS_FILE_LOCK_CONFLICT;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return STATUS_DISK_FULL;
    case ERROR_HANDLE_EOF:
        return STATUS_END_OF_FILE;
    case ERROR_DIR_NOT_EMPTY:
        return STATUS_DIRECTORY_NOT_EMPTY;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_FILENAME_EXCED_RANGE:
        return STATUS_OBJECT_NAME_INVALID;
    case ERROR_DIRECTORY:
        return STATUS_NOT_A_DIRECTORY;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return STATUS_NO_MEMORY;
    case ERROR_WRITE_PROTECT:
        return STATUS_MEDIA_WRITE_PROTECTED;
    case ERROR_INVALID_PARAMETER:
        return STATUS_INVALID_PARAMETER;
    case ERROR_INVALID_HANDLE:
        return STATUS_INVALID_HANDLE;
    case ERROR_NOT_SUPPORTED:
        return STATUS_NOT_SUPPORTED;
    case ERROR_CANT_ACCESS_FILE:
        return STATUS_ACCESS_DENIED;
    default:
        return STATUS_UNSUCCESSFUL;
    }
}

std::string StatusText(NTSTATUS status) {
    if (const char* name = xbox::NtStatusName(status)) {
        return name;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(status));
    return buf;
}

}  // namespace detail
}  // namespace vfs
