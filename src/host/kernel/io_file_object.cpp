// Guest file objects (io agent). See io_file_object.h.
#include "kernel/io_file_object.h"

#include "kernel/io_types.h"
#include "vfs/vfs_internal.h"

#include "core/log.h"

namespace krnl::io {

FileObject::FileObject(std::shared_ptr<vfs::Device> device, std::unique_ptr<vfs::File> file,
                       std::string devicePath, std::string relativePath, std::uint32_t desiredAccess,
                       std::uint32_t createOptions) noexcept
    : device_(std::move(device)), file_(std::move(file)), devicePath_(std::move(devicePath)),
      relativePath_(std::move(relativePath)), access_(desiredAccess), options_(createOptions) {}

FileObject::~FileObject() {
    if (core::LogEnabled(core::Channel::Vfs, core::Level::Trace)) {
        try {
            FUT_LOGT(Vfs, "close %s", FullPath().c_str());
        } catch (...) {
        }
    }
    file_.reset();  // closes the host resource (and performs a pending delete-on-close)
}

std::string FileObject::FullPath() const {
    if (relativePath_ == "\\") {
        return devicePath_ + "\\";
    }
    return devicePath_ + relativePath_;
}

bool FileObject::Synchronous() const noexcept {
    return (options_ & (kOptSynchronousIoAlert | kOptSynchronousIoNonAlert)) != 0;
}

bool FileObject::CanWrite() const noexcept {
    return (access_ & (vfs::detail::kAccessWriteData | vfs::detail::kAccessMaximumAllowed)) != 0;
}

std::uint64_t FileObject::Position() const noexcept {
    AcquireSRWLockShared(&lock_);
    const std::uint64_t p = position_;
    ReleaseSRWLockShared(&lock_);
    return p;
}

void FileObject::SetPosition(std::uint64_t position) noexcept {
    AcquireSRWLockExclusive(&lock_);
    position_ = position;
    ReleaseSRWLockExclusive(&lock_);
}

Ref<FileObject> LookupFile(HANDLE handle) noexcept {
    return HandleTable::Instance().LookupAs<FileObject>(handle);
}

}  // namespace krnl::io
