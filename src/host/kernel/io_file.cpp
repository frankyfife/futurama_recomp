// xboxkrnl file exports, part 1: open, read, write, flush (io agent; design 4.3, 1.7).
//
// Callers (all XAPI, files.md 2.2/2.3; kernel_imports.tsv `callers`):
//   NtCreateFile  CreateFileA 0xF76E0, CopyFileExA 0xF786B, CreateDirectoryA 0xF85AA, FUN_000f8b5a
//                 (TitleImage.xbx), XapiMapLetterToDirectory 0xF8C41, XCreateSaveGame 0xF6987
//   NtOpenFile    GetDiskFreeSpaceExA 0xF7C5D, XGetDiskClusterSizeA 0xF8081, FUN_000f8ae2 (partition1
//                 cluster check), FindFirstFileA 0xF8961, recursive delete 0xF8709/0xF8886, DVD check
//                 FUN_000f90e6, SetFileAttributesA 0xF7B6C, RemoveDirectoryA 0xF8616, CopyFileExA
//   NtReadFile    ReadFile 0xF7195, ReadFileEx 0xF7603 (APC), CopyFileExA
//   NtWriteFile   WriteFile 0xF7282, WriteFileEx 0xF7654 (APC), CopyFileExA, XapiMapLetterToDirectory
//                 (TitleMeta.xbx), FUN_000f8b5a (TitleImage.xbx)
#include "kernel/io_exports.h"

#include "kernel/io_common.h"
#include "kernel/kernel_api.h"
#include "vfs/vfs_internal.h"

#include "core/log.h"

namespace krnl {
namespace {

using io::FileObject;

bool IsNotFound(NTSTATUS s) noexcept {
    return s == STATUS_OBJECT_NAME_NOT_FOUND || s == STATUS_OBJECT_PATH_NOT_FOUND ||
           s == STATUS_OBJECT_NAME_COLLISION;
}

const char* ActionName(std::uint32_t action) noexcept {
    switch (action) {
    case vfs::detail::kActionSuperseded:
        return "FILE_SUPERSEDED";
    case vfs::detail::kActionOpened:
        return "FILE_OPENED";
    case vfs::detail::kActionCreated:
        return "FILE_CREATED";
    case vfs::detail::kActionOverwritten:
        return "FILE_OVERWRITTEN";
    default:
        return "?";
    }
}

// Common body of NtCreateFile / NtOpenFile.
NTSTATUS CreateOrOpen(const char* api, HANDLE* FileHandle, xbox::ACCESS_MASK DesiredAccess,
                      xbox::OBJECT_ATTRIBUTES* ObjectAttributes, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                      xbox::LARGE_INTEGER* AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
                      ULONG CreateDisposition, ULONG CreateOptions) {
    if (FileHandle == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    io::OpenTarget target;
    NTSTATUS status = io::ResolveObjectAttributes(ObjectAttributes, target);
    std::uint32_t action = 0;
    HANDLE handle = nullptr;
    if (NT_SUCCESS(status)) {
        vfs::OpenRequest request;
        request.desiredAccess = DesiredAccess;
        request.fileAttributes = FileAttributes;
        request.shareAccess = ShareAccess;
        request.createDisposition = CreateDisposition;
        request.createOptions = CreateOptions;
        request.allocationSize =
            AllocationSize != nullptr ? static_cast<std::uint64_t>(AllocationSize->QuadPart) : 0;
        std::unique_ptr<vfs::File> file;
        status = target.device->Open(target.relativePath, request, file, action);
        if (NT_SUCCESS(status)) {
            Ref<FileObject> object =
                MakeObject<FileObject>(target.device, std::move(file), target.devicePath, target.relativePath,
                                       DesiredAccess, CreateOptions);
            handle = HandleTable::Instance().Insert(object);
            if (handle == nullptr) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                *FileHandle = handle;
            }
        }
    }
    if (NT_SUCCESS(status)) {
        io::WriteIoStatus(IoStatusBlock, status, action);
    }

    // VFS trace (design 5.4): guest path -> device -> result.
    const core::Level level =
        NT_SUCCESS(status) || IsNotFound(status) ? core::Level::Debug : core::Level::Warn;
    if (core::LogEnabled(core::Channel::Vfs, level)) {
        const std::string text = io::StatusText(status);
        core::LogWrite(
            core::Channel::Vfs, level, "%s %s -> %s%s : %s %s h=%p (access 0x%lX disp %lu opt 0x%lX)", api,
            target.objectName.empty() ? "<invalid>" : target.objectName.c_str(), target.devicePath.c_str(),
            target.relativePath.c_str(), text.c_str(), NT_SUCCESS(status) ? ActionName(action) : "", handle,
            static_cast<unsigned long>(DesiredAccess), static_cast<unsigned long>(CreateDisposition),
            static_cast<unsigned long>(CreateOptions));
    }
    return status;
}

// ByteOffset handling of NtReadFile/NtWriteFile. Synchronous handles use and advance the file pointer
// when ByteOffset is NULL or FILE_USE_FILE_POINTER_POSITION; asynchronous ones must pass an offset
// (XAPI ReadFileEx/WriteFileEx always do: &OVERLAPPED.Offset).
bool ResolveOffset(const char* api, FileObject& file, const xbox::LARGE_INTEGER* byteOffset, bool forWrite,
                   std::uint64_t& offset) noexcept {
    if (byteOffset != nullptr && byteOffset->HighPart == -1) {
        if (forWrite && byteOffset->LowPart == io::kFileWriteToEndOfFileLow) {
            vfs::FileInfo info;
            if (!NT_SUCCESS(file.File().QueryInfo(info))) {
                return false;
            }
            offset = info.size;
            return true;
        }
        if (byteOffset->LowPart == io::kFileUseFilePointerPositionLow) {
            offset = file.Position();
            return true;
        }
    }
    if (byteOffset != nullptr) {
        if (byteOffset->QuadPart < 0) {
            return false;
        }
        offset = static_cast<std::uint64_t>(byteOffset->QuadPart);
        return true;
    }
    if (!file.Synchronous()) {
        // NT returns STATUS_INVALID_PARAMETER here; be lenient and use the file pointer.
        FUT_LOGW(Vfs, "%s(%s): no ByteOffset on an asynchronous handle - using the file pointer", api,
                 file.FullPath().c_str());
    }
    offset = file.Position();
    return true;
}

}  // namespace

// NtCreateFile - slot 25, Ord 190 (0x0BE), stdcall, 9 args (purge 36): FileHandle, DesiredAccess,
// ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition,
// CreateOptions (no EaBuffer/EaLength on the Xbox).
// Evidence: kernel_imports.tsv slot 25 (9 pushes at 0x0F6B09); files.md 2.3/2.5 (dispositions
// CREATE_NEW->2, CREATE_ALWAYS->5, OPEN_EXISTING->1, OPEN_ALWAYS->3, TRUNCATE_EXISTING->4 in
// CreateFileA 0xF76E0, which reads IoStatusBlock.Information == FILE_OPENED/FILE_OVERWRITTEN to set
// ERROR_ALREADY_EXISTS); design 4.2/4.3.
NTSTATUS __stdcall NtCreateFile(HANDLE* FileHandle, xbox::ACCESS_MASK DesiredAccess,
                                xbox::OBJECT_ATTRIBUTES* ObjectAttributes,
                                xbox::IO_STATUS_BLOCK* IoStatusBlock, xbox::LARGE_INTEGER* AllocationSize,
                                ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition,
                                ULONG CreateOptions) noexcept {
    try {  // FUT_HLE_TRY
        return CreateOrOpen("NtCreateFile", FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                            AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions);
    }
    FUT_HLE_CATCH("NtCreateFile")
}
KRNL_FUNC(NtCreateFile, &NtCreateFile);

// NtOpenFile - slot 43, Ord 202 (0x0CA), stdcall, 6 args (purge 24): FileHandle, DesiredAccess,
// ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions. = NtCreateFile(.., NULL, 0, Share,
// FILE_OPEN, OpenOptions).
// Evidence: kernel_imports.tsv slot 43 (13 call sites, 6 pushes); e.g. FUN_000f8ae2 opens
// "\Device\Harddisk0\partition1\" with options 0x800021, FUN_000f90e6 opens "\Device\CdRom0".
NTSTATUS __stdcall NtOpenFile(HANDLE* FileHandle, xbox::ACCESS_MASK DesiredAccess,
                              xbox::OBJECT_ATTRIBUTES* ObjectAttributes, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                              ULONG ShareAccess, ULONG OpenOptions) noexcept {
    try {  // FUT_HLE_TRY
        return CreateOrOpen("NtOpenFile", FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, nullptr,
                            0, ShareAccess, vfs::detail::kFileOpen, OpenOptions);
    }
    FUT_HLE_CATCH("NtOpenFile")
}
KRNL_FUNC(NtOpenFile, &NtOpenFile);

// NtReadFile - slot 34, Ord 219 (0x0DB), stdcall, 8 args (purge 32): FileHandle, Event, ApcRoutine,
// ApcContext, IoStatusBlock, Buffer, Length, ByteOffset (no Key on the Xbox).
// Evidence: kernel_imports.tsv slot 34 (8 pushes, ReadFileEx 0x0F7630); files.md 2.4 (NiUDSStream reads
// 2048-aligned chunks that may run up to 2047 bytes past the end -> success with a smaller
// Information; reads must never fail), 5.1 item 2 (ReadFileEx: ApcRoutine = NtUserIoApcDispatcher,
// ApcContext = completion routine, IoStatusBlock = OVERLAPPED); design 1.7 (APC path).
// M1: the transfer is always synchronous; with an ApcRoutine the completion is queued for the calling
// thread and STATUS_PENDING returned (io_apc.h) - delivery needs the kernel agent's alertable waits.
NTSTATUS __stdcall NtReadFile(HANDLE FileHandle, HANDLE Event, io::IoApcRoutine ApcRoutine, void* ApcContext,
                              xbox::IO_STATUS_BLOCK* IoStatusBlock, void* Buffer, ULONG Length,
                              xbox::LARGE_INTEGER* ByteOffset) noexcept {
    try {  // FUT_HLE_TRY
        Ref<FileObject> file = io::LookupFile(FileHandle);
        if (!file) {
            FUT_LOGW(Krnl, "NtReadFile(%p): invalid handle", FileHandle);
            return STATUS_INVALID_HANDLE;
        }
        if (file->File().IsDirectory()) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        std::uint64_t offset = 0;
        if (!ResolveOffset("NtReadFile", *file, ByteOffset, false, offset)) {
            return STATUS_INVALID_PARAMETER;
        }
        std::uint32_t transferred = 0;
        NTSTATUS status = STATUS_SUCCESS;
        if (Length != 0) {
            if (Buffer == nullptr) {
                return STATUS_INVALID_PARAMETER;
            }
            status = file->File().Read(offset, Buffer, Length, transferred);
        }
        if (NT_SUCCESS(status) && file->Synchronous()) {
            file->SetPosition(offset + transferred);
        }
        FUT_LOGT(Vfs, "read %s @0x%llX len 0x%lX -> %s 0x%X%s", file->FullPath().c_str(),
                 static_cast<unsigned long long>(offset), static_cast<unsigned long>(Length),
                 io::StatusText(status).c_str(), transferred, ApcRoutine != nullptr ? " (APC)" : "");
        if (!NT_SUCCESS(status) && status != STATUS_END_OF_FILE) {
            FUT_LOGW(Vfs, "read %s @0x%llX len 0x%lX failed: %s", file->FullPath().c_str(),
                     static_cast<unsigned long long>(offset), static_cast<unsigned long>(Length),
                     io::StatusText(status).c_str());
        }
        return io::CompleteIo("NtReadFile", Event, ApcRoutine, ApcContext, IoStatusBlock, status,
                              transferred);
    }
    FUT_HLE_CATCH("NtReadFile")
}
KRNL_FUNC(NtReadFile, &NtReadFile);

// NtWriteFile - slot 35, Ord 236 (0x0EC), stdcall, 8 args (purge 32): FileHandle, Event, ApcRoutine,
// ApcContext, IoStatusBlock, Buffer, Length, ByteOffset (no Key).
// Evidence: kernel_imports.tsv slot 35 (8 pushes); files.md 2.2 (WriteFileEx 0xF7654 with
// NtUserIoApcDispatcher for the 4096-byte save containers, completion 0xE01A0), 1.1 (TitleMeta.xbx /
// TitleImage.xbx written with ByteOffset NULL on a FILE_SYNCHRONOUS_IO_NONALERT handle); design 1.7.
NTSTATUS __stdcall NtWriteFile(HANDLE FileHandle, HANDLE Event, io::IoApcRoutine ApcRoutine, void* ApcContext,
                               xbox::IO_STATUS_BLOCK* IoStatusBlock, const void* Buffer, ULONG Length,
                               xbox::LARGE_INTEGER* ByteOffset) noexcept {
    try {  // FUT_HLE_TRY
        Ref<FileObject> file = io::LookupFile(FileHandle);
        if (!file) {
            FUT_LOGW(Krnl, "NtWriteFile(%p): invalid handle", FileHandle);
            return STATUS_INVALID_HANDLE;
        }
        if (file->File().IsDirectory()) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        if (!file->CanWrite()) {
            FUT_LOGW(Vfs, "write %s: handle has no write access (0x%lX)", file->FullPath().c_str(),
                     static_cast<unsigned long>(file->DesiredAccess()));
            return STATUS_ACCESS_DENIED;
        }
        std::uint64_t offset = 0;
        if (!ResolveOffset("NtWriteFile", *file, ByteOffset, true, offset)) {
            return STATUS_INVALID_PARAMETER;
        }
        std::uint32_t transferred = 0;
        NTSTATUS status = STATUS_SUCCESS;
        if (Length != 0) {
            if (Buffer == nullptr) {
                return STATUS_INVALID_PARAMETER;
            }
            status = file->File().Write(offset, Buffer, Length, transferred);
        }
        if (NT_SUCCESS(status) && file->Synchronous()) {
            file->SetPosition(offset + transferred);
        }
        FUT_LOGT(Vfs, "write %s @0x%llX len 0x%lX -> %s 0x%X%s", file->FullPath().c_str(),
                 static_cast<unsigned long long>(offset), static_cast<unsigned long>(Length),
                 io::StatusText(status).c_str(), transferred, ApcRoutine != nullptr ? " (APC)" : "");
        if (!NT_SUCCESS(status)) {
            FUT_LOGW(Vfs, "write %s @0x%llX len 0x%lX failed: %s", file->FullPath().c_str(),
                     static_cast<unsigned long long>(offset), static_cast<unsigned long>(Length),
                     io::StatusText(status).c_str());
        }
        return io::CompleteIo("NtWriteFile", Event, ApcRoutine, ApcContext, IoStatusBlock, status,
                              transferred);
    }
    FUT_HLE_CATCH("NtWriteFile")
}
KRNL_FUNC(NtWriteFile, &NtWriteFile);

// NtFlushBuffersFile - slot 38, Ord 198 (0x0C6), stdcall, 2 args (purge 8): FileHandle, IoStatusBlock.
// Evidence: kernel_imports.tsv slot 38 (only XAPI FlushFileBuffers 0xF759E, called by the CRT
// FUN_0016F8DE); files.md 2.3 (2 pushes at 0xF75A6/0xF75A7).
NTSTATUS __stdcall NtFlushBuffersFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock) noexcept {
    try {  // FUT_HLE_TRY
        Ref<FileObject> file = io::LookupFile(FileHandle);
        if (!file) {
            return STATUS_INVALID_HANDLE;
        }
        const NTSTATUS status = file->File().Flush();
        if (NT_SUCCESS(status)) {
            io::WriteIoStatus(IoStatusBlock, status, 0);
        }
        FUT_LOGT(Vfs, "flush %s -> %s", file->FullPath().c_str(), io::StatusText(status).c_str());
        return status;
    }
    FUT_HLE_CATCH("NtFlushBuffersFile")
}
KRNL_FUNC(NtFlushBuffersFile, &NtFlushBuffersFile);

}  // namespace krnl
