// Guest file objects (io agent): the handle-table object behind every guest file/directory handle
// returned by NtCreateFile/NtOpenFile (design 4.2 item 6). It owns the vfs::File (the host resource is
// released - and a delete-on-close performed - when the last reference goes, i.e. on NtClose) and the
// NT file-object state the device layer does not know: the file pointer of synchronous handles and
// the NtQueryDirectoryFile mask.
#pragma once

#include "kernel/ob_handles.h"
#include "vfs/vfs.h"

#include <cstdint>
#include <memory>
#include <string>

namespace krnl::io {

class FileObject final : public KObject {
public:
    FileObject(std::shared_ptr<vfs::Device> device, std::unique_ptr<vfs::File> file, std::string devicePath,
               std::string relativePath, std::uint32_t desiredAccess, std::uint32_t createOptions) noexcept;
    ~FileObject() override;

    const char* TypeName() const noexcept override { return "File"; }

    vfs::File& File() const noexcept { return *file_; }
    const std::shared_ptr<vfs::Device>& Device() const noexcept { return device_; }
    const std::string& DevicePath() const noexcept { return devicePath_; }
    const std::string& RelativePath() const noexcept { return relativePath_; }
    std::string FullPath() const;  // devicePath + relativePath (for logs)
    std::uint32_t DesiredAccess() const noexcept { return access_; }
    std::uint32_t CreateOptions() const noexcept { return options_; }
    // FILE_SYNCHRONOUS_IO_(NON)ALERT: the object keeps a file pointer (CurrentByteOffset).
    bool Synchronous() const noexcept;
    bool CanWrite() const noexcept;

    std::uint64_t Position() const noexcept;
    void SetPosition(std::uint64_t position) noexcept;

    // NtQueryDirectoryFile state (guarded by Lock/Unlock together with the vfs::File enumeration).
    void Lock() noexcept { AcquireSRWLockExclusive(&dirLock_); }
    void Unlock() noexcept { ReleaseSRWLockExclusive(&dirLock_); }
    std::string dirMask;
    bool dirMaskSet = false;
    bool dirReturnedAny = false;

private:
    std::shared_ptr<vfs::Device> device_;
    std::unique_ptr<vfs::File> file_;
    std::string devicePath_;
    std::string relativePath_;
    std::uint32_t access_;
    std::uint32_t options_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;  // position_
    SRWLOCK dirLock_ = SRWLOCK_INIT;       // directory enumeration
    std::uint64_t position_ = 0;
};

// File object behind a guest handle (null for unknown handles and other object types).
Ref<FileObject> LookupFile(HANDLE handle) noexcept;

}  // namespace krnl::io
