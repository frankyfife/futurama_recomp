// xboxkrnl file exports, part 2: information classes, volume information, attributes and directory
// enumeration (io agent; design 4.3, files.md 2.2/5.1 item 4).
#include "kernel/io_exports.h"

#include "kernel/io_common.h"
#include "kernel/kernel_api.h"
#include "vfs/vfs_internal.h"

#include "core/guest.h"
#include "core/log.h"

#include <algorithm>
#include <cstring>

namespace krnl {
namespace {

using io::FileObject;

// File information classes the guest may use but nothing here implements: log once per class.
NTSTATUS UnsupportedClass(const char* api, const FileObject& file, ULONG infoClass) {
    FUT_LOGW(Krnl, "%s(%s): information class %lu not supported -> STATUS_INVALID_INFO_CLASS", api,
             file.FullPath().c_str(), static_cast<unsigned long>(infoClass));
    return STATUS_INVALID_INFO_CLASS;
}

std::int64_t FromGuestTime(const xbox::LARGE_INTEGER& t) noexcept {
    return t.QuadPart;
}

}  // namespace

// NtQueryInformationFile - slot 36, Ord 211 (0x0D3), stdcall, 5 args (purge 20): FileHandle,
// IoStatusBlock, FileInformation, Length, FileInformationClass.
// Evidence: kernel_imports.tsv slot 36; classes used by XAPI: 0xE (SetFilePointer/SetEndOfFile, 8 bytes)
// and 0x22 (GetFileSizeEx reads EndOfFile at +0x28, FUN_000f8b5a / XapiMapLetterToDirectory test
// EndOfFile == 0 before writing TitleImage.xbx / TitleMeta.xbx) - files.md 1.1, 2.2.
NTSTATUS __stdcall NtQueryInformationFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                          void* FileInformation, ULONG Length,
                                          ULONG FileInformationClass) noexcept {
    try {  // FUT_HLE_TRY
        Ref<FileObject> file = io::LookupFile(FileHandle);
        if (!file) {
            return STATUS_INVALID_HANDLE;
        }
        if (FileInformation == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        NTSTATUS status = STATUS_SUCCESS;
        std::uint32_t written = 0;
        switch (FileInformationClass) {
        case io::kFilePositionInformation: {
            if (Length < sizeof(io::GuestFilePositionInformation)) {
                return STATUS_INFO_LENGTH_MISMATCH;
            }
            io::GuestFilePositionInformation p{};
            p.CurrentByteOffset = io::MakeLargeInteger(static_cast<std::int64_t>(file->Position()));
            io::CopyToGuest(FileInformation, p);
            written = sizeof(p);
            break;
        }
        case io::kFileNetworkOpenInformation:
        case io::kFileBasicInformation:
        case io::kFileStandardInformation: {
            vfs::FileInfo info;
            status = file->File().QueryInfo(info);
            if (!NT_SUCCESS(status)) {
                break;
            }
            if (FileInformationClass == io::kFileNetworkOpenInformation) {
                if (Length < sizeof(io::GuestFileNetworkOpenInformation)) {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }
                io::CopyToGuest(FileInformation, io::MakeNetworkOpenInformation(info));
                written = sizeof(io::GuestFileNetworkOpenInformation);
            } else if (FileInformationClass == io::kFileBasicInformation) {
                if (Length < sizeof(io::GuestFileBasicInformation)) {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }
                io::GuestFileBasicInformation b{};
                b.CreationTime = io::MakeLargeInteger(info.creationTime);
                b.LastAccessTime = io::MakeLargeInteger(info.lastAccessTime);
                b.LastWriteTime = io::MakeLargeInteger(info.lastWriteTime);
                b.ChangeTime = io::MakeLargeInteger(info.changeTime);
                b.FileAttributes = info.attributes;
                io::CopyToGuest(FileInformation, b);
                written = sizeof(b);
            } else {
                if (Length < sizeof(io::GuestFileStandardInformation)) {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }
                io::GuestFileStandardInformation s{};
                s.AllocationSize = io::MakeLargeInteger(static_cast<std::int64_t>(info.allocationSize));
                s.EndOfFile = io::MakeLargeInteger(static_cast<std::int64_t>(info.size));
                s.NumberOfLinks = 1;
                s.DeletePending = 0;
                s.Directory = info.isDirectory ? 1 : 0;
                io::CopyToGuest(FileInformation, s);
                written = sizeof(s);
            }
            break;
        }
        default:
            return UnsupportedClass("NtQueryInformationFile", *file, FileInformationClass);
        }
        if (NT_SUCCESS(status)) {
            io::WriteIoStatus(IoStatusBlock, status, written);
        }
        FUT_LOGT(Krnl, "NtQueryInformationFile(%s, class 0x%lX) -> %s", file->FullPath().c_str(),
                 static_cast<unsigned long>(FileInformationClass), io::StatusText(status).c_str());
        return status;
    }
    FUT_HLE_CATCH("NtQueryInformationFile")
}
KRNL_FUNC(NtQueryInformationFile, &NtQueryInformationFile);

// NtSetInformationFile - slot 22, Ord 226 (0x0E2), stdcall, 5 args (purge 20): FileHandle,
// IoStatusBlock, FileInformation, Length, FileInformationClass.
// Evidence: kernel_imports.tsv slot 22; classes used by XAPI (files.md 2.2, 2.3): 4 Basic
// (SetFileAttributesA, CopyFileExA, XapiMapLetterToDirectory, FUN_000f692b), 0xD Disposition
// (RemoveDirectoryA, recursive delete, CopyFileExA failure path; Length 1), 0xE Position
// (SetFilePointer), 0x13 Allocation and 0x14 EndOfFile (SetEndOfFile, CopyFileExA).
NTSTATUS __stdcall NtSetInformationFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                        void* FileInformation, ULONG Length,
                                        ULONG FileInformationClass) noexcept {
    try {  // FUT_HLE_TRY
        Ref<FileObject> file = io::LookupFile(FileHandle);
        if (!file) {
            return STATUS_INVALID_HANDLE;
        }
        if (FileInformation == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        NTSTATUS status = STATUS_SUCCESS;
        switch (FileInformationClass) {
        case io::kFileBasicInformation: {
            if (Length < sizeof(io::GuestFileBasicInformation)) {
                return STATUS_INFO_LENGTH_MISMATCH;
            }
            const auto b = io::CopyFromGuest<io::GuestFileBasicInformation>(FileInformation);
            vfs::FileInfo info;
            info.creationTime = FromGuestTime(b.CreationTime);
            info.lastAccessTime = FromGuestTime(b.LastAccessTime);
            info.lastWriteTime = FromGuestTime(b.LastWriteTime);
            info.changeTime = FromGuestTime(b.ChangeTime);
            info.attributes = b.FileAttributes;
            status = file->File().SetTimesAndAttributes(info);
            break;
        }
        case io::kFileDispositionInformation: {
            if (Length < sizeof(io::GuestFileDispositionInformation)) {
                return STATUS_INFO_LENGTH_MISMATCH;
            }
            const auto d = io::CopyFromGuest<io::GuestFileDispositionInformation>(FileInformation);
            status = file->File().SetDeleteOnClose(d.DeleteFile != 0);
            break;
        }
        case io::kFilePositionInformation: {
            if (Length < sizeof(io::GuestFilePositionInformation)) {
                return STATUS_INFO_LENGTH_MISMATCH;
            }
            const auto p = io::CopyFromGuest<io::GuestFilePositionInformation>(FileInformation);
            if (p.CurrentByteOffset.QuadPart < 0) {
                return STATUS_INVALID_PARAMETER;
            }
            file->SetPosition(static_cast<std::uint64_t>(p.CurrentByteOffset.QuadPart));
            break;
        }
        case io::kFileEndOfFileInformation: {
            if (Length < sizeof(io::GuestFileEndOfFileInformation)) {
                return STATUS_INFO_LENGTH_MISMATCH;
            }
            const auto e = io::CopyFromGuest<io::GuestFileEndOfFileInformation>(FileInformation);
            if (e.EndOfFile.QuadPart < 0) {
                return STATUS_INVALID_PARAMETER;
            }
            status = file->File().SetEndOfFile(static_cast<std::uint64_t>(e.EndOfFile.QuadPart));
            break;
        }
        case io::kFileAllocationInformation: {
            if (Length < sizeof(io::GuestFileAllocationInformation)) {
                return STATUS_INFO_LENGTH_MISMATCH;
            }
            const auto a = io::CopyFromGuest<io::GuestFileAllocationInformation>(FileInformation);
            if (a.AllocationSize.QuadPart < 0) {
                return STATUS_INVALID_PARAMETER;
            }
            // NT: an allocation below the end of file truncates it; a larger one only reserves space.
            // ASSUMPTION: host files are never preallocated (only SetEndOfFile 0xF7358 uses this, after
            // setting EndOfFile to the same value).
            vfs::FileInfo info;
            status = file->File().QueryInfo(info);
            if (NT_SUCCESS(status) && static_cast<std::uint64_t>(a.AllocationSize.QuadPart) < info.size) {
                status = file->File().SetEndOfFile(static_cast<std::uint64_t>(a.AllocationSize.QuadPart));
            }
            break;
        }
        default:
            return UnsupportedClass("NtSetInformationFile", *file, FileInformationClass);
        }
        if (NT_SUCCESS(status)) {
            io::WriteIoStatus(IoStatusBlock, status, 0);
        }
        const core::Level level = NT_SUCCESS(status) ? core::Level::Trace : core::Level::Debug;
        if (core::LogEnabled(core::Channel::Vfs, level)) {
            const std::string text = io::StatusText(status);
            core::LogWrite(core::Channel::Vfs, level, "set info %s class 0x%lX -> %s",
                           file->FullPath().c_str(), static_cast<unsigned long>(FileInformationClass),
                           text.c_str());
        }
        return status;
    }
    FUT_HLE_CATCH("NtSetInformationFile")
}
KRNL_FUNC(NtSetInformationFile, &NtSetInformationFile);

// NtQueryVolumeInformationFile - slot 37, Ord 218 (0x0DA), stdcall, 5 args (purge 20): FileHandle,
// IoStatusBlock, FsInformation, Length, FsInformationClass.
// Evidence: kernel_imports.tsv slot 37; class 3 (FileFsSizeInformation, 0x18) at all 4 call sites:
// FUN_000f8ae2 (XapiInitProcess: partition1 cluster must be 0x4000, else XapiBootToDash),
// XGetDiskClusterSizeA 0xF8081 (_cinit initializer 0x20EFC0 divides by it), GetDiskFreeSpaceExA
// 0xF7C5D (free space < 4 GiB, files.md 4.4), XGetDiskSectorSizeA 0xF8120. Design 4.3.
NTSTATUS __stdcall NtQueryVolumeInformationFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                                void* FsInformation, ULONG Length,
                                                ULONG FsInformationClass) noexcept {
    try {  // FUT_HLE_TRY
        Ref<FileObject> file = io::LookupFile(FileHandle);
        if (!file) {
            return STATUS_INVALID_HANDLE;
        }
        if (FsInformation == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        vfs::VolumeSizeInfo v;
        NTSTATUS status = STATUS_SUCCESS;
        std::uint32_t written = 0;
        switch (FsInformationClass) {
        case io::kFileFsSizeInformation: {
            if (Length < sizeof(io::GuestFileFsSizeInformation)) {
                return STATUS_INFO_LENGTH_MISMATCH;
            }
            status = file->Device()->QueryVolumeSize(v);
            if (!NT_SUCCESS(status)) {
                break;
            }
            io::GuestFileFsSizeInformation s{};
            s.TotalAllocationUnits = io::MakeLargeInteger(static_cast<std::int64_t>(v.totalAllocationUnits));
            s.AvailableAllocationUnits =
                io::MakeLargeInteger(static_cast<std::int64_t>(v.availableAllocationUnits));
            s.SectorsPerAllocationUnit = v.sectorsPerAllocationUnit;
            s.BytesPerSector = v.bytesPerSector;
            io::CopyToGuest(FsInformation, s);
            written = sizeof(s);
            break;
        }
        case io::kFileFsFullSizeInformation: {
            if (Length < sizeof(io::GuestFileFsFullSizeInformation)) {
                return STATUS_INFO_LENGTH_MISMATCH;
            }
            status = file->Device()->QueryVolumeSize(v);
            if (!NT_SUCCESS(status)) {
                break;
            }
            io::GuestFileFsFullSizeInformation s{};
            s.TotalAllocationUnits = io::MakeLargeInteger(static_cast<std::int64_t>(v.totalAllocationUnits));
            s.CallerAvailableAllocationUnits =
                io::MakeLargeInteger(static_cast<std::int64_t>(v.availableAllocationUnits));
            s.ActualAvailableAllocationUnits = s.CallerAvailableAllocationUnits;
            s.SectorsPerAllocationUnit = v.sectorsPerAllocationUnit;
            s.BytesPerSector = v.bytesPerSector;
            io::CopyToGuest(FsInformation, s);
            written = sizeof(s);
            break;
        }
        default:
            FUT_LOGW(Krnl, "NtQueryVolumeInformationFile(%s): class %lu not supported",
                     file->FullPath().c_str(), static_cast<unsigned long>(FsInformationClass));
            return STATUS_INVALID_INFO_CLASS;
        }
        if (NT_SUCCESS(status)) {
            io::WriteIoStatus(IoStatusBlock, status, written);
        }
        FUT_LOGD(Vfs, "volume info %s class %lu -> %s (%llu/%llu units of %u x %u bytes)",
                 file->FullPath().c_str(), static_cast<unsigned long>(FsInformationClass),
                 io::StatusText(status).c_str(), static_cast<unsigned long long>(v.availableAllocationUnits),
                 static_cast<unsigned long long>(v.totalAllocationUnits), v.sectorsPerAllocationUnit,
                 v.bytesPerSector);
        return status;
    }
    FUT_HLE_CATCH("NtQueryVolumeInformationFile")
}
KRNL_FUNC(NtQueryVolumeInformationFile, &NtQueryVolumeInformationFile);

// NtQueryFullAttributesFile - slot 45, Ord 210 (0x0D2), stdcall, 2 args (purge 8): ObjectAttributes,
// FileInformation (FILE_NETWORK_OPEN_INFORMATION, 0x38).
// Evidence: kernel_imports.tsv slot 45 (only GetFileAttributesA 0xF7C12, reads FileAttributes at +0x30;
// its caller NiFile-Access 0x14F0A0 is inactive, files.md 2.2). Plan M2.
NTSTATUS __stdcall NtQueryFullAttributesFile(xbox::OBJECT_ATTRIBUTES* ObjectAttributes,
                                             io::GuestFileNetworkOpenInformation* FileInformation) noexcept {
    try {  // FUT_HLE_TRY
        if (FileInformation == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        io::OpenTarget target;
        NTSTATUS status = io::ResolveObjectAttributes(ObjectAttributes, target);
        vfs::FileInfo info;
        if (NT_SUCCESS(status)) {
            status = target.device->QueryAttributes(target.relativePath, info);
        }
        if (NT_SUCCESS(status)) {
            io::CopyToGuest(FileInformation, io::MakeNetworkOpenInformation(info));
        }
        FUT_LOGD(Vfs, "attributes %s -> %s%s : %s", target.objectName.c_str(), target.devicePath.c_str(),
                 target.relativePath.c_str(), io::StatusText(status).c_str());
        io::TraceVfsLookup("NtQueryFullAttributesFile", target, status, nullptr, nullptr);
        return status;
    }
    FUT_HLE_CATCH("NtQueryFullAttributesFile")
}
KRNL_FUNC(NtQueryFullAttributesFile, &NtQueryFullAttributesFile);

// NtQueryDirectoryFile - slot 46, Ord 207 (0x0CF), stdcall, 10 args (purge 40): FileHandle, Event,
// ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass,
// FileName (POBJECT_STRING mask), RestartScan (no ReturnSingleEntry on the Xbox).
// Evidence: kernel_imports.tsv slot 46; files.md 2.2 / 5.1 item 4 (FindFirstFileA 0xF8961 passes the
// mask on the first call with RestartScan FALSE, "*.*" as an empty mask; FindNextFileA 0xF8A6E passes
// no mask; the recursive delete 0xF8709 starts with RestartScan TRUE and stops on
// STATUS_NO_MORE_FILES or STATUS_NO_SUCH_FILE; buffers of 0x146/0x148 bytes = one entry per call;
// class 1 only, ANSI names, no "." / ".."). Plan M2.
NTSTATUS __stdcall NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, io::IoApcRoutine ApcRoutine,
                                        void* ApcContext, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                        void* FileInformation, ULONG Length, ULONG FileInformationClass,
                                        xbox::OBJECT_STRING* FileName, BOOLEAN RestartScan) noexcept {
    try {  // FUT_HLE_TRY
        Ref<FileObject> file = io::LookupFile(FileHandle);
        if (!file) {
            return STATUS_INVALID_HANDLE;
        }
        if (!file->File().IsDirectory()) {
            return STATUS_INVALID_PARAMETER;
        }
        if (FileInformationClass != io::kFileDirectoryInformation) {
            return UnsupportedClass("NtQueryDirectoryFile", *file, FileInformationClass);
        }
        if (FileInformation == nullptr || Length < io::kFileDirectoryInformationHeaderSize) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        Ref<EventObject> event;
        if (const NTSTATUS es = io::ReferenceEvent("NtQueryDirectoryFile", Event, event); !NT_SUCCESS(es)) {
            return es;
        }
        vfs::DirEntry entry;
        NTSTATUS status;
        {
            file->Lock();
            // NT: the mask of the first query sticks to the handle; RestartScan rewinds.
            bool restart = RestartScan != FALSE;
            if (!file->dirMaskSet) {
                file->dirMask = FileName != nullptr ? guest::ReadAnsiString(FileName) : std::string();
                file->dirMaskSet = true;
                restart = true;
            }
            if (restart) {
                file->dirReturnedAny = false;
            }
            status = file->File().NextDirEntry(file->dirMask, restart, entry);
            if (status == STATUS_NO_MORE_FILES && !file->dirReturnedAny) {
                status = STATUS_NO_SUCH_FILE;  // nothing matched since the scan started
            }
            if (NT_SUCCESS(status)) {
                file->dirReturnedAny = true;
            }
            file->Unlock();
        }
        std::uint32_t written = 0;
        if (NT_SUCCESS(status)) {
            io::GuestFileDirectoryInformation d{};
            d.NextEntryOffset = 0;
            d.FileIndex = 0;
            d.CreationTime = io::MakeLargeInteger(entry.info.creationTime);
            d.LastAccessTime = io::MakeLargeInteger(entry.info.lastAccessTime);
            d.LastWriteTime = io::MakeLargeInteger(entry.info.lastWriteTime);
            d.ChangeTime = io::MakeLargeInteger(entry.info.changeTime);
            d.EndOfFile = io::MakeLargeInteger(static_cast<std::int64_t>(entry.info.size));
            d.AllocationSize = io::MakeLargeInteger(static_cast<std::int64_t>(entry.info.allocationSize));
            d.FileAttributes = entry.info.attributes;
            d.FileNameLength = static_cast<std::uint32_t>(entry.name.size());
            const std::uint32_t room = Length - io::kFileDirectoryInformationHeaderSize;
            const std::uint32_t copied = std::min<std::uint32_t>(room, d.FileNameLength);
            io::CopyToGuest(FileInformation, d, io::kFileDirectoryInformationHeaderSize);
            std::memcpy(static_cast<char*>(FileInformation) + io::kFileDirectoryInformationHeaderSize,
                        entry.name.data(), copied);
            written = io::kFileDirectoryInformationHeaderSize + copied;
            if (copied < d.FileNameLength) {
                status = STATUS_BUFFER_OVERFLOW;  // entry consumed, name truncated (NT semantics)
            }
        }
        const NTSTATUS result = io::CompleteIo(io::IoRequest{"NtQueryDirectoryFile", file.get(), event.get(),
                                                             ApcRoutine, ApcContext, IoStatusBlock},
                                               status, written);
        FUT_LOGT(Krnl, "NtQueryDirectoryFile(%p %s, class %lu, mask '%s', restart %u) -> %s %s", FileHandle,
                 file->FullPath().c_str(), static_cast<unsigned long>(FileInformationClass),
                 file->dirMask.c_str(), static_cast<unsigned>(RestartScan), io::StatusString(result).c_str(),
                 NT_SUCCESS(status) ? entry.name.c_str() : "");
        return result;
    }
    FUT_HLE_CATCH("NtQueryDirectoryFile")
}
KRNL_FUNC(NtQueryDirectoryFile, &NtQueryDirectoryFile);

}  // namespace krnl
