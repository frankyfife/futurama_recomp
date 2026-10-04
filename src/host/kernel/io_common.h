// Shared helpers of the io_* kernel exports (io agent; internal).
//
// Every export body is `try { ... } FUT_HLE_CATCH("Name")` - `try` is what FUT_HLE_TRY expands to;
// it is spelled out because clang-format cannot parse `FUT_HLE_TRY { ... }` and mangles the block.
#pragma once

#include "kernel/io_file_object.h"
#include "kernel/io_types.h"
#include "vfs/vfs.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace krnl::io {

struct OpenTarget {
    std::shared_ptr<vfs::Device> device;
    std::string devicePath;    // "\Device\CdRom0"
    std::string relativePath;  // "\SysPAL.img" ("\" = device root)
    std::string objectName;    // the name as the guest passed it (logs)
};

// OBJECT_ATTRIBUTES -> device + device-relative path (design 4.2 item 1, files.md 5.1 item 1):
// RootDirectory (HANDLE)-3 = relative to "\??\" (XAPI CreateFileA and friends), 0 = absolute object
// name, otherwise a directory handle the name is relative to (recursive delete FUN_000f8709).
NTSTATUS ResolveObjectAttributes(const xbox::OBJECT_ATTRIBUTES* objectAttributes, OpenTarget& out);

// Completes a synchronously executed request with NT IopCompleteRequest semantics: an error status
// is returned as is (no IoStatusBlock, no event, no APC); otherwise the IoStatusBlock is written and,
// with an ApcRoutine, the completion is queued (io_apc.h) and STATUS_PENDING returned.
NTSTATUS CompleteIo(const char* api, HANDLE event, IoApcRoutine apcRoutine, void* apcContext,
                    xbox::IO_STATUS_BLOCK* ioStatusBlock, NTSTATUS status,
                    std::uint32_t information) noexcept;

inline void WriteIoStatus(xbox::IO_STATUS_BLOCK* iosb, NTSTATUS status, std::uint32_t information) noexcept {
    if (iosb != nullptr) {
        iosb->Status = status;
        iosb->Information = information;
    }
}

inline xbox::LARGE_INTEGER MakeLargeInteger(std::int64_t v) noexcept {
    xbox::LARGE_INTEGER li;
    li.QuadPart = v;
    return li;
}

// Copies a host-built structure into a (possibly unaligned) guest buffer.
template <class T>
inline void CopyToGuest(void* guest, const T& value, std::size_t bytes = sizeof(T)) noexcept {
    std::memcpy(guest, &value, bytes < sizeof(T) ? bytes : sizeof(T));
}
template <class T>
inline T CopyFromGuest(const void* guest) noexcept {
    T value;
    std::memcpy(&value, guest, sizeof(T));
    return value;
}

GuestFileNetworkOpenInformation MakeNetworkOpenInformation(const vfs::FileInfo& info) noexcept;

std::string StatusText(NTSTATUS status);
bool IsNtUserIoApcDispatcher(IoApcRoutine routine) noexcept;

}  // namespace krnl::io
