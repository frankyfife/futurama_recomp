// HostDirDevice: a device backed by a host directory (design 4.1/4.2 item 5): partition1 =
// <userdata>\hdd\partition1, Z: = <userdata>\cache\Z (XMountUtilityDrive hook), optionally an
// extracted disc (read-only).
//
// - Guest names are ANSI; they map 1:1 to UTF-16 (Latin-1, vfs_internal.h). Host paths always use the
//   "\\?\" prefix, so reserved DOS names (CON, NUL, ...) and trailing dots/spaces are never
//   reinterpreted by Win32; such names and "." / ".." are rejected anyway (no escape from the root).
// - FATX rules (files.md 5.3): a name component has 1..42 characters, none of  " * / : < > ? \ |  and
//   no control characters.
// - Case-insensitive like the Xbox because the host file system (NTFS) is case-insensitive.
// - I/O is synchronous and positional; the kernel layer keeps the file pointer and produces the APC
//   semantics (design 1.7). FILE_FLAG_NO_BUFFERING / OVERLAPPED of the guest are not passed on.
// - ASSUMPTION: share modes are not emulated (host handles always share read/write/delete). There is
//   one guest thread (E10) and nothing in XAPI or the game relies on a sharing violation.
// - Volume geometry: 32 sectors x 512 bytes = 0x4000 per cluster (design 4.3: XapiInitProcess
//   FUN_000f8ae2 demands 0x4000 for partition1, XGetDiskClusterSizeA("U:\") divides by it in _cinit);
//   free space is capped below 4 GiB (files.md 4.4: the game tests `high | low`).
#include "vfs/vfs.h"
#include "vfs/vfs_internal.h"

#include "core/config.h"
#include "core/log.h"

#include <algorithm>
#include <system_error>

namespace vfs {
namespace {

using detail::kHostClusterSize;

std::uint64_t RoundUp(std::uint64_t v, std::uint64_t unit) noexcept {
    return (v + unit - 1) / unit * unit;
}

std::int64_t FileTimeToInt(const FILETIME& ft) noexcept {
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) |
                                     ft.dwLowDateTime);
}

// Host attributes -> guest attributes (FAT bits only; FILE_ATTRIBUTE_NORMAL if nothing else is set).
std::uint32_t GuestAttributes(DWORD host) noexcept {
    const std::uint32_t a = host & (detail::kAttrReadOnly | detail::kAttrHidden | detail::kAttrSystem |
                                    detail::kAttrDirectory | detail::kAttrArchive);
    return a != 0 ? a : detail::kAttrNormal;
}

// Guest attributes for SetFileAttributes/FileBasicInformation -> host value (0 = do not change).
DWORD HostAttributes(std::uint32_t guest) noexcept {
    if (guest == 0) {
        return 0;
    }
    const DWORD a = guest & detail::kAttrSettableMask;
    return a != 0 ? a : FILE_ATTRIBUTE_NORMAL;
}

bool ValidComponent(const std::string& name) noexcept {
    if (name.empty() || name.size() > detail::kFatxMaxNameLength || name == "." || name == "..") {
        return false;
    }
    for (char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' ||
            c == '\\' || c == '|') {
            return false;
        }
    }
    // Win32 strips trailing dots/spaces even below "\\?\" on some APIs; refuse them to avoid aliasing.
    return name.back() != '.' && name.back() != ' ';
}

FileInfo InfoFromFindData(const WIN32_FIND_DATAW& fd) noexcept {
    FileInfo info;
    info.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.size = info.isDirectory ? 0 : (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    info.allocationSize = RoundUp(info.size, kHostClusterSize);
    info.attributes = GuestAttributes(fd.dwFileAttributes);
    info.creationTime = FileTimeToInt(fd.ftCreationTime);
    info.lastAccessTime = FileTimeToInt(fd.ftLastAccessTime);
    info.lastWriteTime = FileTimeToInt(fd.ftLastWriteTime);
    info.changeTime = info.lastWriteTime;
    return info;
}

FileInfo InfoFromAttributeData(const WIN32_FILE_ATTRIBUTE_DATA& ad) noexcept {
    FileInfo info;
    info.isDirectory = (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.size = info.isDirectory ? 0 : (static_cast<std::uint64_t>(ad.nFileSizeHigh) << 32) | ad.nFileSizeLow;
    info.allocationSize = RoundUp(info.size, kHostClusterSize);
    info.attributes = GuestAttributes(ad.dwFileAttributes);
    info.creationTime = FileTimeToInt(ad.ftCreationTime);
    info.lastAccessTime = FileTimeToInt(ad.ftLastAccessTime);
    info.lastWriteTime = FileTimeToInt(ad.ftLastWriteTime);
    info.changeTime = info.lastWriteTime;
    return info;
}

NTSTATUS LastErrorStatus() noexcept {
    return detail::Win32ErrorToStatus(GetLastError());
}

// dir + "\" + name without doubling the separator of a drive root ("\\?\C:\").
std::wstring JoinHost(const std::wstring& dir, std::wstring_view name) {
    std::wstring out = dir;
    if (out.empty() || out.back() != L'\\') {
        out.push_back(L'\\');
    }
    out += name;
    return out;
}

bool DirectoryIsEmpty(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileExW(JoinHost(dir, L"*").c_str(), FindExInfoBasic, &fd,
                                      FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return true;
    }
    bool empty = true;
    do {
        if (std::wcscmp(fd.cFileName, L".") != 0 && std::wcscmp(fd.cFileName, L"..") != 0) {
            empty = false;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

class HostFile final : public File {
public:
    HostFile(HANDLE handle, std::wstring hostPath, bool isDirectory, bool readOnlyDevice)
        : handle_(handle), hostPath_(std::move(hostPath)), isDirectory_(isDirectory),
          readOnly_(readOnlyDevice) {}

    HostFile(const HostFile&) = delete;
    HostFile& operator=(const HostFile&) = delete;

    ~HostFile() override {
        if (find_ != INVALID_HANDLE_VALUE) {
            FindClose(find_);
        }
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        if (deleteOnClose_) {
            const BOOL ok =
                isDirectory_ ? RemoveDirectoryW(hostPath_.c_str()) : DeleteFileW(hostPath_.c_str());
            if (!ok) {
                FUT_LOGW(Vfs, "delete-on-close of %s failed (Win32 error %lu)",
                         core::WideToUtf8(hostPath_).c_str(), GetLastError());
            } else {
                FUT_LOGT(Vfs, "deleted %s", core::WideToUtf8(hostPath_).c_str());
            }
        }
    }

    bool IsDirectory() const noexcept override { return isDirectory_; }

    NTSTATUS Read(std::uint64_t offset, void* buffer, std::uint32_t length,
                  std::uint32_t& transferred) noexcept override {
        transferred = 0;
        if (isDirectory_) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        if (length == 0) {
            return STATUS_SUCCESS;
        }
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD n = 0;
        if (!ReadFile(handle_, buffer, length, &n, &ov)) {
            const DWORD err = GetLastError();
            return err == ERROR_HANDLE_EOF ? STATUS_END_OF_FILE : detail::Win32ErrorToStatus(err);
        }
        transferred = n;
        return n == 0 ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    }

    NTSTATUS Write(std::uint64_t offset, const void* buffer, std::uint32_t length,
                   std::uint32_t& transferred) noexcept override {
        transferred = 0;
        if (isDirectory_) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        if (readOnly_) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        if (length == 0) {
            return STATUS_SUCCESS;
        }
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD n = 0;
        if (!WriteFile(handle_, buffer, length, &n, &ov)) {
            return LastErrorStatus();
        }
        transferred = n;
        return STATUS_SUCCESS;
    }

    NTSTATUS QueryInfo(FileInfo& info) noexcept override {
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandleEx(handle_, FileBasicInfo, &basic, sizeof(basic))) {
            return LastErrorStatus();
        }
        info = FileInfo{};
        info.isDirectory = isDirectory_;
        info.attributes = GuestAttributes(basic.FileAttributes);
        info.creationTime = basic.CreationTime.QuadPart;
        info.lastAccessTime = basic.LastAccessTime.QuadPart;
        info.lastWriteTime = basic.LastWriteTime.QuadPart;
        info.changeTime = basic.ChangeTime.QuadPart;
        if (!isDirectory_) {
            FILE_STANDARD_INFO standard{};
            if (!GetFileInformationByHandleEx(handle_, FileStandardInfo, &standard, sizeof(standard))) {
                return LastErrorStatus();
            }
            info.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
            info.allocationSize = RoundUp(info.size, kHostClusterSize);
        }
        return STATUS_SUCCESS;
    }

    NTSTATUS SetEndOfFile(std::uint64_t size) noexcept override {
        if (isDirectory_) {
            return STATUS_INVALID_PARAMETER;
        }
        if (readOnly_) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        FILE_END_OF_FILE_INFO eof{};
        eof.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
        return SetFileInformationByHandle(handle_, FileEndOfFileInfo, &eof, sizeof(eof)) ? STATUS_SUCCESS
                                                                                         : LastErrorStatus();
    }

    NTSTATUS SetTimesAndAttributes(const FileInfo& info) noexcept override {
        if (readOnly_) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        // FILE_BASIC_INFO: 0 = leave unchanged (same convention as the guest's FileBasicInformation).
        FILE_BASIC_INFO basic{};
        basic.CreationTime.QuadPart = info.creationTime;
        basic.LastAccessTime.QuadPart = info.lastAccessTime;
        basic.LastWriteTime.QuadPart = info.lastWriteTime;
        basic.ChangeTime.QuadPart = info.changeTime;
        basic.FileAttributes = HostAttributes(info.attributes);
        if (isDirectory_ && basic.FileAttributes != 0) {
            basic.FileAttributes =
                (basic.FileAttributes == FILE_ATTRIBUTE_NORMAL ? 0 : basic.FileAttributes) |
                FILE_ATTRIBUTE_DIRECTORY;
        }
        return SetFileInformationByHandle(handle_, FileBasicInfo, &basic, sizeof(basic)) ? STATUS_SUCCESS
                                                                                         : LastErrorStatus();
    }

    NTSTATUS SetDeleteOnClose(bool deleteOnClose) noexcept override {
        if (!deleteOnClose) {
            deleteOnClose_ = false;
            return STATUS_SUCCESS;
        }
        if (readOnly_) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        const DWORD attrs = GetFileAttributesW(hostPath_.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0) {
            return STATUS_CANNOT_DELETE;  // NT semantics; XAPI clears the bit first (FUN_000f8709)
        }
        try {
            if (isDirectory_ && !DirectoryIsEmpty(hostPath_)) {
                return STATUS_DIRECTORY_NOT_EMPTY;
            }
        } catch (...) {
            return STATUS_NO_MEMORY;
        }
        deleteOnClose_ = true;
        return STATUS_SUCCESS;
    }

    NTSTATUS Flush() noexcept override {
        if (isDirectory_ || readOnly_) {
            return STATUS_SUCCESS;
        }
        return FlushFileBuffers(handle_) ? STATUS_SUCCESS : LastErrorStatus();
    }

    NTSTATUS NextDirEntry(std::string_view mask, bool restart, DirEntry& out) noexcept override {
        if (!isDirectory_) {
            return STATUS_INVALID_PARAMETER;
        }
        try {
            if (restart || !started_) {
                if (find_ != INVALID_HANDLE_VALUE) {
                    FindClose(find_);
                    find_ = INVALID_HANDLE_VALUE;
                }
                started_ = true;
                exhausted_ = false;
                find_ = FindFirstFileExW(JoinHost(hostPath_, L"*").c_str(), FindExInfoBasic, &findData_,
                                         FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (find_ == INVALID_HANDLE_VALUE) {
                    const DWORD err = GetLastError();
                    if (err != ERROR_FILE_NOT_FOUND && err != ERROR_NO_MORE_FILES) {
                        return detail::Win32ErrorToStatus(err);
                    }
                    exhausted_ = true;
                }
                pending_ = !exhausted_;
            }
            while (!exhausted_) {
                if (!pending_) {
                    if (!FindNextFileW(find_, &findData_)) {
                        exhausted_ = true;
                        break;
                    }
                }
                pending_ = false;
                if (std::wcscmp(findData_.cFileName, L".") == 0 ||
                    std::wcscmp(findData_.cFileName, L"..") == 0) {
                    continue;
                }
                std::string name;
                if (!detail::WideToGuest(findData_.cFileName, name)) {
                    FUT_LOGD(Vfs, "skipping host name not representable in the guest code page: %s",
                             core::WideToUtf8(findData_.cFileName).c_str());
                    continue;
                }
                if (!MatchFileMask(name, mask)) {
                    continue;
                }
                out.name = std::move(name);
                out.info = InfoFromFindData(findData_);
                return STATUS_SUCCESS;
            }
        } catch (...) {
            return STATUS_NO_MEMORY;
        }
        return STATUS_NO_MORE_FILES;
    }

private:
    HANDLE handle_;
    std::wstring hostPath_;
    bool isDirectory_;
    bool readOnly_;
    bool deleteOnClose_ = false;
    // directory enumeration
    HANDLE find_ = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW findData_{};
    bool started_ = false;
    bool exhausted_ = false;
    bool pending_ = false;  // findData_ holds an entry that was not returned yet
};

class HostDirDevice final : public Device {
public:
    HostDirDevice(std::wstring rootNative, bool readOnly)
        : root_(std::move(rootNative)), readOnly_(readOnly) {}

    const char* Kind() const noexcept override { return readOnly_ ? "HostDirDevice(ro)" : "HostDirDevice"; }
    bool IsReadOnly() const noexcept override { return readOnly_; }

    NTSTATUS Open(std::string_view relativePath, const OpenRequest& request, std::unique_ptr<File>& file,
                  std::uint32_t& createAction) noexcept override {
        file.reset();
        createAction = detail::kActionOpened;
        try {
            return OpenImpl(relativePath, request, file, createAction);
        } catch (...) {
            return STATUS_NO_MEMORY;
        }
    }

    NTSTATUS QueryAttributes(std::string_view relativePath, FileInfo& info) noexcept override {
        try {
            std::wstring host;
            bool isRoot = false;
            const NTSTATUS s = MapPath(relativePath, host, isRoot);
            if (!NT_SUCCESS(s)) {
                return s;
            }
            WIN32_FILE_ATTRIBUTE_DATA ad{};
            if (!GetFileAttributesExW(host.c_str(), GetFileExInfoStandard, &ad)) {
                return LastErrorStatus();
            }
            info = InfoFromAttributeData(ad);
            return STATUS_SUCCESS;
        } catch (...) {
            return STATUS_NO_MEMORY;
        }
    }

    NTSTATUS QueryVolumeSize(VolumeSizeInfo& info) noexcept override {
        info.sectorsPerAllocationUnit = detail::kHostSectorsPerCluster;
        info.bytesPerSector = detail::kHostBytesPerSector;
        ULARGE_INTEGER freeToCaller{}, total{}, totalFree{};
        std::uint64_t freeBytes = detail::kHostMaxFreeBytes;
        std::uint64_t totalBytes = detail::kHostMaxFreeBytes;
        if (GetDiskFreeSpaceExW(JoinHost(root_, L"").c_str(), &freeToCaller, &total, &totalFree)) {
            freeBytes = freeToCaller.QuadPart;
            totalBytes = total.QuadPart;
        } else {
            FUT_LOGW(Vfs, "GetDiskFreeSpaceEx(%s) failed (Win32 error %lu) - reporting a fixed size",
                     core::WideToUtf8(root_).c_str(), GetLastError());
        }
        // files.md 4.4 / design 4.3: free space must stay below 4 GiB (guest computes `high | low`).
        freeBytes = std::min<std::uint64_t>(freeBytes, detail::kHostMaxFreeBytes);
        info.availableAllocationUnits = readOnly_ ? 0 : freeBytes / kHostClusterSize;
        info.totalAllocationUnits =
            std::max<std::uint64_t>(totalBytes / kHostClusterSize, info.availableAllocationUnits);
        return STATUS_SUCCESS;
    }

private:
    // Device-relative guest path -> "\\?\<root>\a\b". Validates every component (FATX rules).
    NTSTATUS MapPath(std::string_view relativePath, std::wstring& host, bool& isRoot) const {
        const std::vector<std::string> parts = detail::SplitPath(relativePath);
        host = root_;
        for (const std::string& part : parts) {
            if (!ValidComponent(part)) {
                return STATUS_OBJECT_NAME_INVALID;
            }
            host = JoinHost(host, detail::GuestToWide(part));
        }
        isRoot = parts.empty();
        return STATUS_SUCCESS;
    }

    NTSTATUS OpenImpl(std::string_view relativePath, const OpenRequest& request, std::unique_ptr<File>& file,
                      std::uint32_t& createAction) {
        std::wstring host;
        bool isRoot = false;
        NTSTATUS s = MapPath(relativePath, host, isRoot);
        if (!NT_SUCCESS(s)) {
            return s;
        }
        const std::uint32_t disposition = request.createDisposition;
        const std::uint32_t options = request.createOptions;
        if (disposition > detail::kFileOverwriteIf) {
            return STATUS_INVALID_PARAMETER;
        }
        const bool wantDir = (options & detail::kOptDirectoryFile) != 0;
        const bool wantNonDir = (options & detail::kOptNonDirectoryFile) != 0;
        if (wantDir && wantNonDir) {
            return STATUS_INVALID_PARAMETER;
        }

        WIN32_FILE_ATTRIBUTE_DATA ad{};
        const bool exists = GetFileAttributesExW(host.c_str(), GetFileExInfoStandard, &ad) != FALSE;
        if (!exists) {
            const DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND) {
                return detail::Win32ErrorToStatus(err);  // e.g. ERROR_PATH_NOT_FOUND: parent missing
            }
        }
        const bool isDir = exists && (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (exists) {
            if (disposition == detail::kFileCreate) {
                return STATUS_OBJECT_NAME_COLLISION;
            }
            if (isDir && wantNonDir) {
                return STATUS_FILE_IS_A_DIRECTORY;
            }
            if (!isDir && wantDir) {
                return STATUS_NOT_A_DIRECTORY;
            }
            if (isDir && (disposition == detail::kFileOverwrite || disposition == detail::kFileOverwriteIf ||
                          disposition == detail::kFileSupersede)) {
                return STATUS_INVALID_PARAMETER;
            }
            createAction =
                disposition == detail::kFileSupersede ? detail::kActionSuperseded
                : (disposition == detail::kFileOverwrite || disposition == detail::kFileOverwriteIf)
                    ? detail::kActionOverwritten
                    : detail::kActionOpened;
        } else {
            if (disposition == detail::kFileOpen || disposition == detail::kFileOverwrite) {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            createAction = detail::kActionCreated;
        }

        const std::uint32_t access = request.desiredAccess;
        const bool modifies = createAction != detail::kActionOpened;
        const bool deleteOnClose = (options & detail::kOptDeleteOnClose) != 0;
        if (readOnly_) {
            if (modifies) {
                return STATUS_MEDIA_WRITE_PROTECTED;
            }
            if ((access & (detail::kAccessWriteData | detail::kAccessDelete)) != 0 || deleteOnClose) {
                return STATUS_ACCESS_DENIED;
            }
        }

        const bool directory = isDir || (!exists && wantDir);
        HANDLE h = INVALID_HANDLE_VALUE;
        if (directory) {
            if (!exists && !CreateDirectoryW(host.c_str(), nullptr)) {
                return LastErrorStatus();
            }
            if (!exists && HostAttributes(request.fileAttributes) != 0 &&
                HostAttributes(request.fileAttributes) != FILE_ATTRIBUTE_NORMAL) {
                SetFileAttributesW(host.c_str(), HostAttributes(request.fileAttributes));
            }
            const DWORD dirAccess = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE |
                                    (readOnly_ ? 0 : FILE_WRITE_ATTRIBUTES);
            h = CreateFileW(host.c_str(), dirAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED && !readOnly_) {
                h = CreateFileW(host.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            }
            if (h == INVALID_HANDLE_VALUE) {
                return LastErrorStatus();
            }
        } else {
            DWORD hostAccess = GENERIC_READ | SYNCHRONIZE;
            if (!readOnly_) {
                if ((access & (detail::kAccessWriteData | detail::kAccessMaximumAllowed)) != 0 || modifies) {
                    hostAccess |= GENERIC_WRITE;
                }
                if ((access & detail::kAccessWriteAttributes) != 0) {
                    hostAccess |= FILE_WRITE_ATTRIBUTES;
                }
                if ((access & detail::kAccessDelete) != 0 || deleteOnClose) {
                    hostAccess |= DELETE;
                }
            }
            DWORD win32Disposition = OPEN_EXISTING;
            if (createAction == detail::kActionCreated) {
                win32Disposition = CREATE_NEW;
            } else if (modifies) {
                win32Disposition = TRUNCATE_EXISTING;
            }
            const DWORD createAttrs = HostAttributes(request.fileAttributes);
            h = CreateFileW(host.c_str(), hostAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, win32Disposition, createAttrs != 0 ? createAttrs : FILE_ATTRIBUTE_NORMAL,
                            nullptr);
            if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED &&
                (access & detail::kAccessMaximumAllowed) != 0 && !modifies) {
                // MAXIMUM_ALLOWED on a read-only file: fall back to read access.
                h = CreateFileW(host.c_str(), GENERIC_READ | SYNCHRONIZE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            if (h == INVALID_HANDLE_VALUE) {
                return LastErrorStatus();
            }
            if (modifies && createAttrs != 0) {
                // NT FILE_OVERWRITE(_IF)/FILE_SUPERSEDE replace the attributes with FileAttributes.
                SetFileAttributesW(host.c_str(), createAttrs);
            }
        }
        auto hostFile = std::make_unique<HostFile>(h, std::move(host), directory, readOnly_);
        if (deleteOnClose) {
            s = hostFile->SetDeleteOnClose(true);
            if (!NT_SUCCESS(s)) {
                return s;
            }
        }
        file = std::move(hostFile);
        return STATUS_SUCCESS;
    }

    std::wstring root_;  // "\\?\D:\...\partition1" (no trailing separator except for a drive root)
    bool readOnly_;
};

// "\\?\" form of an absolute directory path without trailing separator (a drive root keeps "C:\").
std::wstring NativeRoot(const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(root, ec);
    if (ec) {
        abs = root;
    }
    std::wstring s = abs.lexically_normal().make_preferred().wstring();
    while (s.size() > 3 && s.back() == L'\\') {
        s.pop_back();
    }
    if (s.rfind(L"\\\\?\\", 0) == 0) {
        return s;
    }
    if (s.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + s.substr(2);
    }
    return L"\\\\?\\" + s;
}

}  // namespace

std::shared_ptr<Device> CreateHostDirDevice(const std::filesystem::path& root, bool readOnly) {
    try {
        if (!readOnly) {
            std::error_code ec;
            std::filesystem::create_directories(root, ec);
            if (ec) {
                FUT_LOGE(Vfs, "cannot create host directory %s: %s", core::WideToUtf8(root.wstring()).c_str(),
                         ec.message().c_str());
                return nullptr;
            }
        }
        std::wstring native = NativeRoot(root);
        const DWORD attrs = GetFileAttributesW(native.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            FUT_LOGE(Vfs, "host directory %s does not exist", core::WideToUtf8(native).c_str());
            return nullptr;
        }
        return std::make_shared<HostDirDevice>(std::move(native), readOnly);
    } catch (const std::exception& e) {
        FUT_LOGE(Vfs, "CreateHostDirDevice: %s", e.what());
        return nullptr;
    }
}

}  // namespace vfs
