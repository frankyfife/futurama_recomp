// xboxkrnl file exports, part 3: device/file-system control and symbolic links (io agent).
#include "kernel/io_exports.h"

#include "kernel/io_common.h"
#include "kernel/kernel_api.h"

#include "core/guest.h"
#include "core/log.h"

#include <mutex>
#include <set>

namespace krnl {
namespace {

// Logs each (api, code) pair once at info level, repeats at debug.
void LogControlOnce(const char* api, const std::string& path, ULONG code, NTSTATUS result) {
    static std::mutex mutex;
    static std::set<std::pair<std::string, ULONG>>* seen = new std::set<std::pair<std::string, ULONG>>();
    bool first;
    {
        std::lock_guard<std::mutex> lock(mutex);
        first = seen->insert({api, code}).second;
    }
    const core::Level level = first ? core::Level::Info : core::Level::Debug;
    if (core::LogEnabled(core::Channel::Vfs, level)) {
        const std::string text = io::StatusText(result);
        core::LogWrite(core::Channel::Vfs, level, "%s(%s, code 0x%05lX) -> %s", api, path.c_str(),
                       static_cast<unsigned long>(code), text.c_str());
    }
}

}  // namespace

// NtDeviceIoControlFile - slot 41, Ord 196 (0x0C4), stdcall, 10 args (purge 40): FileHandle, Event,
// ApcRoutine, ApcContext, IoStatusBlock, IoControlCode, InputBuffer, InputBufferLength, OutputBuffer,
// OutputBufferLength.
// Evidence: kernel_imports.tsv slot 41; callers FUN_000f90e6 (XapiInitProcess DVD check, IOCTL 0x4D014 =
// SCSI pass-through with CDB 0x5A/0xAD on \Device\CdRom0; the result is ignored at 0x0F931E) and
// FUN_000f946d (FATX format via XMountUtilityDrive, 0x70000/0x74004; replaced by the core hook).
// Design 2.1/4.3 + Anhang A: every IOCTL fails with STATUS_INVALID_DEVICE_REQUEST (logged).
NTSTATUS __stdcall NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, io::IoApcRoutine ApcRoutine,
                                         void* /*ApcContext*/, xbox::IO_STATUS_BLOCK* /*IoStatusBlock*/,
                                         ULONG IoControlCode, void* /*InputBuffer*/,
                                         ULONG /*InputBufferLength*/, void* /*OutputBuffer*/,
                                         ULONG /*OutputBufferLength*/) noexcept {
    try {  // FUT_HLE_TRY
        Ref<io::FileObject> file = io::LookupFile(FileHandle);
        if (!file) {
            FUT_LOGW(Krnl, "NtDeviceIoControlFile(%p, 0x%lX): invalid handle", FileHandle,
                     static_cast<unsigned long>(IoControlCode));
            return STATUS_INVALID_HANDLE;
        }
        Ref<EventObject> event;
        if (const NTSTATUS es = io::ReferenceEvent("NtDeviceIoControlFile", Event, event); !NT_SUCCESS(es)) {
            return es;
        }
        // Fails synchronously: no IoStatusBlock, no event, no APC (io_common.h CompleteIo).
        const NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
        LogControlOnce("NtDeviceIoControlFile", file->FullPath(), IoControlCode, status);
        return io::CompleteIo(
            io::IoRequest{"NtDeviceIoControlFile", file.get(), event.get(), ApcRoutine, nullptr, nullptr},
            status, 0);
    }
    FUT_HLE_CATCH("NtDeviceIoControlFile")
}
KRNL_FUNC(NtDeviceIoControlFile, &NtDeviceIoControlFile);

// NtFsControlFile - slot 40, Ord 200 (0x0C8), stdcall, 10 args (purge 40): FileHandle, Event,
// ApcRoutine, ApcContext, IoStatusBlock, FsControlCode, InputBuffer, InputBufferLength, OutputBuffer,
// OutputBufferLength.
// Evidence: kernel_imports.tsv slot 40 (only FUN_000f946d, FSCTL 0x90020 after formatting the utility
// partition - not reached because XMountUtilityDrive is hooked). Design Anhang A "defensiv", 2.1:
// success without doing anything.
NTSTATUS __stdcall NtFsControlFile(HANDLE FileHandle, HANDLE Event, io::IoApcRoutine ApcRoutine,
                                   void* ApcContext, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                   ULONG FsControlCode, void* /*InputBuffer*/, ULONG /*InputBufferLength*/,
                                   void* /*OutputBuffer*/, ULONG /*OutputBufferLength*/) noexcept {
    try {  // FUT_HLE_TRY
        Ref<io::FileObject> file = io::LookupFile(FileHandle);
        if (!file) {
            return STATUS_INVALID_HANDLE;
        }
        Ref<EventObject> event;
        if (const NTSTATUS es = io::ReferenceEvent("NtFsControlFile", Event, event); !NT_SUCCESS(es)) {
            return es;
        }
        LogControlOnce("NtFsControlFile", file->FullPath(), FsControlCode, STATUS_SUCCESS);
        return io::CompleteIo(
            io::IoRequest{"NtFsControlFile", file.get(), event.get(), ApcRoutine, ApcContext, IoStatusBlock},
            STATUS_SUCCESS, 0);
    }
    FUT_HLE_CATCH("NtFsControlFile")
}
KRNL_FUNC(NtFsControlFile, &NtFsControlFile);

// IoCreateSymbolicLink - slot 48, Ord 67 (0x043), stdcall, 2 args (purge 8): SymbolicLinkName,
// DeviceName (both POBJECT_STRING = ANSI).
// Evidence: kernel_imports.tsv slot 48; callers XapiInitProcess ("\??\D:" -> "\Device\CdRom0", only
// 0xC0000035 tolerated), XapiMapLetterToDirectory ("\??\T:"/"\??\U:" ->
// "\Device\Harddisk0\partition1\TDATA|UDATA\46490002"), XMountUtilityDrive (hooked); files.md 1.1,
// design 4.1 (existing name -> STATUS_OBJECT_NAME_COLLISION).
NTSTATUS __stdcall IoCreateSymbolicLink(xbox::OBJECT_STRING* SymbolicLinkName,
                                        xbox::OBJECT_STRING* DeviceName) noexcept {
    try {  // FUT_HLE_TRY
        const std::string link = guest::ReadAnsiString(SymbolicLinkName);
        const std::string target = guest::ReadAnsiString(DeviceName);
        if (link.empty() || target.empty()) {
            FUT_LOGW(Krnl, "IoCreateSymbolicLink: empty name ('%s' -> '%s')", link.c_str(), target.c_str());
            return STATUS_OBJECT_NAME_INVALID;
        }
        const NTSTATUS status = vfs::CreateSymbolicLink(link, target);
        FUT_LOGD(Krnl, "IoCreateSymbolicLink(%s, %s) -> %s", link.c_str(), target.c_str(),
                 io::StatusText(status).c_str());
        return status;
    }
    FUT_HLE_CATCH("IoCreateSymbolicLink")
}
KRNL_FUNC(IoCreateSymbolicLink, &IoCreateSymbolicLink);

}  // namespace krnl
