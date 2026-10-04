// IsoDevice: \Device\CdRom0 served from an XDVDFS disc image (design 4.1, 4.2 item 4; files.md 5.2).
//
// Read-only. Reads are positional on the shared image handle (several outstanding reads per guest
// handle work, files.md 2.4). A read that runs past the end of a file succeeds with the clipped
// length (NiUDSStream rounds the last chunk up to 2048 bytes, files.md 2.4 "bis zu 2047 Byte");
// only a read that STARTS at/after the end returns STATUS_END_OF_FILE, as NT does (XAPI ReadFile
// turns that into TRUE with 0 bytes). Missing names give STATUS_OBJECT_NAME_NOT_FOUND
// (STATUS_OBJECT_PATH_NOT_FOUND if a parent directory is missing), e.g. FutPAL05.img, sysNTSC.img.
#include "vfs/vfs.h"
#include "vfs/vfs_internal.h"

#include "core/log.h"

namespace vfs {
namespace {

using detail::kDiscSectorSize;

std::uint64_t RoundUp(std::uint64_t v, std::uint64_t unit) noexcept {
    return (v + unit - 1) / unit * unit;
}

FileInfo MakeInfo(const XdvdfsImage::Entry& e, std::int64_t timestamp) noexcept {
    FileInfo info;
    info.isDirectory = e.IsDirectory();
    info.size = info.isDirectory ? 0 : e.size;
    info.allocationSize = RoundUp(e.size, kDiscSectorSize);
    // ASSUMPTION: like NT CDFS the disc reports every entry read-only; the stored XDVDFS attribute
    // byte (0x20 archive for files, 0x10 directories on this disc) is kept.
    info.attributes = static_cast<std::uint32_t>(e.attributes) | detail::kAttrReadOnly;
    if (info.isDirectory) {
        info.attributes |= detail::kAttrDirectory;
    }
    info.creationTime = info.lastAccessTime = info.lastWriteTime = info.changeTime = timestamp;
    return info;
}

class IsoFile final : public File {
public:
    IsoFile(std::shared_ptr<XdvdfsImage> image, XdvdfsImage::Entry entry, std::string path)
        : image_(std::move(image)), entry_(std::move(entry)), path_(std::move(path)) {}

    bool IsDirectory() const noexcept override { return entry_.IsDirectory(); }

    NTSTATUS Read(std::uint64_t offset, void* buffer, std::uint32_t length,
                  std::uint32_t& transferred) noexcept override {
        transferred = 0;
        if (entry_.IsDirectory()) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        if (length == 0) {
            return STATUS_SUCCESS;
        }
        if (offset >= entry_.size) {
            return STATUS_END_OF_FILE;
        }
        try {
            if (!image_->Read(entry_, offset, buffer, length, transferred)) {
                return STATUS_UNEXPECTED_IO_ERROR;
            }
        } catch (...) {
            return STATUS_UNEXPECTED_IO_ERROR;
        }
        return STATUS_SUCCESS;
    }

    NTSTATUS Write(std::uint64_t, const void*, std::uint32_t, std::uint32_t& transferred) noexcept override {
        transferred = 0;
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    NTSTATUS QueryInfo(FileInfo& info) noexcept override {
        info = MakeInfo(entry_, image_->VolumeTimestamp());
        return STATUS_SUCCESS;
    }

    NTSTATUS SetEndOfFile(std::uint64_t) noexcept override { return STATUS_MEDIA_WRITE_PROTECTED; }
    NTSTATUS SetTimesAndAttributes(const FileInfo&) noexcept override { return STATUS_MEDIA_WRITE_PROTECTED; }
    NTSTATUS SetDeleteOnClose(bool deleteOnClose) noexcept override {
        return deleteOnClose ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_SUCCESS;
    }
    NTSTATUS Flush() noexcept override { return STATUS_SUCCESS; }

    NTSTATUS NextDirEntry(std::string_view mask, bool restart, DirEntry& out) noexcept override {
        if (!entry_.IsDirectory()) {
            return STATUS_INVALID_PARAMETER;
        }
        try {
            if (restart || !listed_) {
                image_->ListDirectory(path_, listing_);
                listed_ = true;
                cursor_ = 0;
            }
            while (cursor_ < listing_.size()) {
                const XdvdfsImage::Entry& e = listing_[cursor_++];
                if (MatchFileMask(e.name, mask)) {
                    out.name = e.name;
                    out.info = MakeInfo(e, image_->VolumeTimestamp());
                    return STATUS_SUCCESS;
                }
            }
        } catch (...) {
            return STATUS_NO_MEMORY;
        }
        return STATUS_NO_MORE_FILES;
    }

private:
    std::shared_ptr<XdvdfsImage> image_;
    XdvdfsImage::Entry entry_;
    std::string path_;
    std::vector<XdvdfsImage::Entry> listing_;
    std::size_t cursor_ = 0;
    bool listed_ = false;
};

class IsoDevice final : public Device {
public:
    explicit IsoDevice(std::shared_ptr<XdvdfsImage> image) : image_(std::move(image)) {}

    const char* Kind() const noexcept override { return "IsoDevice"; }
    bool IsReadOnly() const noexcept override { return true; }

    NTSTATUS Open(std::string_view relativePath, const OpenRequest& request, std::unique_ptr<File>& file,
                  std::uint32_t& createAction) noexcept override {
        file.reset();
        createAction = detail::kActionOpened;
        try {
            const std::uint32_t disposition = request.createDisposition;
            if (disposition > detail::kFileOverwriteIf) {
                return STATUS_INVALID_PARAMETER;
            }
            const bool createCapable =
                disposition == detail::kFileCreate || disposition == detail::kFileOpenIf ||
                disposition == detail::kFileOverwriteIf || disposition == detail::kFileSupersede;
            XdvdfsImage::Entry entry;
            const std::string path = detail::JoinPath(detail::SplitPath(relativePath));
            if (!image_->Lookup(path, entry)) {
                if (createCapable) {
                    return STATUS_MEDIA_WRITE_PROTECTED;
                }
                return ParentExists(path) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
            }
            if (disposition == detail::kFileCreate) {
                return STATUS_OBJECT_NAME_COLLISION;
            }
            if (disposition != detail::kFileOpen && disposition != detail::kFileOpenIf) {
                return STATUS_MEDIA_WRITE_PROTECTED;
            }
            // ASSUMPTION: write/delete access on the disc is refused like on NT CDFS.
            if ((request.desiredAccess & (detail::kAccessWriteData | detail::kAccessDelete)) != 0 ||
                (request.createOptions & detail::kOptDeleteOnClose) != 0) {
                return STATUS_ACCESS_DENIED;
            }
            if ((request.createOptions & detail::kOptDirectoryFile) != 0 && !entry.IsDirectory()) {
                return STATUS_NOT_A_DIRECTORY;
            }
            if ((request.createOptions & detail::kOptNonDirectoryFile) != 0 && entry.IsDirectory()) {
                return STATUS_FILE_IS_A_DIRECTORY;
            }
            file = std::make_unique<IsoFile>(image_, std::move(entry), path);
            return STATUS_SUCCESS;
        } catch (...) {
            return STATUS_NO_MEMORY;
        }
    }

    NTSTATUS QueryAttributes(std::string_view relativePath, FileInfo& info) noexcept override {
        try {
            XdvdfsImage::Entry entry;
            const std::string path = detail::JoinPath(detail::SplitPath(relativePath));
            if (!image_->Lookup(path, entry)) {
                return ParentExists(path) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
            }
            info = MakeInfo(entry, image_->VolumeTimestamp());
            return STATUS_SUCCESS;
        } catch (...) {
            return STATUS_NO_MEMORY;
        }
    }

    NTSTATUS QueryVolumeSize(VolumeSizeInfo& info) noexcept override {
        // design 4.3: sector 2048 for D: (XGetDiskSectorSizeA("D:\") in the inactive NiTCache).
        info.bytesPerSector = kDiscSectorSize;
        info.sectorsPerAllocationUnit = 1;
        info.totalAllocationUnits = image_->PartitionSize() / kDiscSectorSize;
        info.availableAllocationUnits = 0;
        return STATUS_SUCCESS;
    }

private:
    bool ParentExists(const std::string& path) const {
        std::vector<std::string> parts = detail::SplitPath(path);
        if (parts.empty()) {
            return true;
        }
        parts.pop_back();
        XdvdfsImage::Entry parent;
        return image_->Lookup(detail::JoinPath(parts), parent) && parent.IsDirectory();
    }

    std::shared_ptr<XdvdfsImage> image_;
};

// Device without files (no disc image configured).
class EmptyDirFile final : public File {
public:
    bool IsDirectory() const noexcept override { return true; }
    NTSTATUS Read(std::uint64_t, void*, std::uint32_t, std::uint32_t& transferred) noexcept override {
        transferred = 0;
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    NTSTATUS Write(std::uint64_t, const void*, std::uint32_t, std::uint32_t& transferred) noexcept override {
        transferred = 0;
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    NTSTATUS QueryInfo(FileInfo& info) noexcept override {
        info = FileInfo{};
        info.isDirectory = true;
        info.attributes = detail::kAttrDirectory | detail::kAttrReadOnly;
        return STATUS_SUCCESS;
    }
    NTSTATUS SetEndOfFile(std::uint64_t) noexcept override { return STATUS_MEDIA_WRITE_PROTECTED; }
    NTSTATUS SetTimesAndAttributes(const FileInfo&) noexcept override { return STATUS_MEDIA_WRITE_PROTECTED; }
    NTSTATUS SetDeleteOnClose(bool d) noexcept override {
        return d ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_SUCCESS;
    }
    NTSTATUS Flush() noexcept override { return STATUS_SUCCESS; }
    NTSTATUS NextDirEntry(std::string_view, bool, DirEntry&) noexcept override {
        return STATUS_NO_MORE_FILES;
    }
};

class EmptyDevice final : public Device {
public:
    explicit EmptyDevice(const char* kind) : kind_(kind) {}
    const char* Kind() const noexcept override { return kind_; }
    bool IsReadOnly() const noexcept override { return true; }
    NTSTATUS Open(std::string_view relativePath, const OpenRequest& request, std::unique_ptr<File>& file,
                  std::uint32_t& createAction) noexcept override {
        file.reset();
        createAction = detail::kActionOpened;
        try {
            const auto parts = detail::SplitPath(relativePath);
            if (!parts.empty()) {
                return parts.size() == 1 ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
            }
            if ((request.createOptions & detail::kOptNonDirectoryFile) != 0) {
                return STATUS_FILE_IS_A_DIRECTORY;
            }
            file = std::make_unique<EmptyDirFile>();
            return STATUS_SUCCESS;
        } catch (...) {
            return STATUS_NO_MEMORY;
        }
    }
    NTSTATUS QueryAttributes(std::string_view relativePath, FileInfo& info) noexcept override {
        try {
            const auto parts = detail::SplitPath(relativePath);
            if (!parts.empty()) {
                return parts.size() == 1 ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
            }
        } catch (...) {
            return STATUS_NO_MEMORY;
        }
        info = FileInfo{};
        info.isDirectory = true;
        info.attributes = detail::kAttrDirectory | detail::kAttrReadOnly;
        return STATUS_SUCCESS;
    }
    NTSTATUS QueryVolumeSize(VolumeSizeInfo& info) noexcept override {
        info = VolumeSizeInfo{0, 0, 1, kDiscSectorSize};
        return STATUS_SUCCESS;
    }

private:
    const char* kind_;
};

}  // namespace

std::shared_ptr<Device> CreateIsoDevice(std::shared_ptr<XdvdfsImage> image) {
    if (!image || !image->IsOpen()) {
        return nullptr;
    }
    return std::make_shared<IsoDevice>(std::move(image));
}

namespace detail {
std::shared_ptr<Device> CreateEmptyDevice(const char* kind) {
    return std::make_shared<EmptyDevice>(kind);
}
}  // namespace detail

}  // namespace vfs
