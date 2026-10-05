// Shared helpers of the io_* kernel exports (io agent; internal).
//
// Every export body is `try { ... } FUT_HLE_CATCH("Name")` - `try` is what FUT_HLE_TRY expands to;
// it is spelled out because clang-format cannot parse `FUT_HLE_TRY { ... }` and mangles the block.
#pragma once

#include "kernel/io_file_object.h"
#include "kernel/io_types.h"
#include "kernel/ke_wait.h"
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

// Optional Event parameter of NtReadFile, NtWriteFile, NtQueryDirectoryFile, NtDeviceIoControlFile and
// NtFsControlFile (NT ObReferenceObjectByHandle(Event, EVENT_MODIFY_STATE, ExEventObjectType) +
// KeClearEvent before the request starts): NULL -> no event, STATUS_SUCCESS; an EventObject
// (kernel/ke_wait.h) -> reset now, returned in `out`; an unknown handle -> STATUS_INVALID_HANDLE, another
// object type -> STATUS_OBJECT_TYPE_MISMATCH (logged; the caller must not start the request).
// No guest path passes an event (NtCreateEvent is not imported, startup.md 4.5; XAPI ReadFileEx/
// WriteFileEx push 0 at 0xF762B/0xF767C, ReadFile/WriteFile pass OVERLAPPED.hEvent only for overlapped
// handles, used by the inactive NiTCache only, files.md 2.2).
NTSTATUS ReferenceEvent(const char* api, HANDLE event, Ref<EventObject>& out) noexcept;

// One request of an asynchronous-capable export, completed by CompleteIo.
struct IoRequest {
    const char* api;                       // "NtReadFile" (logs)
    const FileObject* file;                // target (synchronous/asynchronous handle, path for logs)
    EventObject* event;                    // from ReferenceEvent, may be null
    IoApcRoutine apcRoutine;               // may be null
    void* apcContext;                      // XAPI: the completion routine
    xbox::IO_STATUS_BLOCK* ioStatusBlock;  // XAPI ReadFileEx/WriteFileEx: the OVERLAPPED
};

// Completes a synchronously executed request with NT IopCompleteRequest semantics:
//  - an error status (NT_ERROR) is returned as is: no IoStatusBlock, no event, no APC (an APC request
//    is counted by NoteFailedIoApcRequest, io_apc.h - with NtUserIoApcDispatcher it would make the game
//    call its disc error handler);
//  - otherwise the IoStatusBlock is written - with an ApcRoutine on an asynchronous handle and no event
//    only when the APC is delivered (design 1.7) - the event is signalled, the APC queued for the
//    calling thread (io_apc.h). An asynchronous handle with an ApcRoutine returns STATUS_PENDING
//    (XAPI ReadFileEx/WriteFileEx: anything but NT_ERROR is TRUE), all other cases the final status
//    (NT: a synchronous file object waits for the request; the APC is still queued).
NTSTATUS CompleteIo(const IoRequest& request, NTSTATUS status, std::uint32_t information) noexcept;

inline void WriteIoStatus(xbox::IO_STATUS_BLOCK* iosb, NTSTATUS status, std::uint32_t information) noexcept {
    if (iosb != nullptr) {
        iosb->Status = status;
        iosb->Information = information;
    }
}

inline bool IsNtError(NTSTATUS status) noexcept {
    return (static_cast<std::uint32_t>(status) >> 30) == 3;
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
// "STATUS_..." or "0x%08lX" without allocating (for noexcept paths).
class StatusString {
public:
    explicit StatusString(NTSTATUS status) noexcept;
    const char* c_str() const noexcept { return text_; }

private:
    char hex_[12];
    const char* text_;
};
bool IsNtUserIoApcDispatcher(IoApcRoutine routine) noexcept;

// ---- VFS trace (--trace=vfs, core/trace.h; M2 exit criterion 7) -----------------------------------
// One vfs_trace.log line for an open (NtCreateFile/NtOpenFile) or an attribute query: guest path ->
// device path : hit | missing (documented: <reason>) | missing UNDOCUMENTED | error. `action` is the
// create action of a successful open (FILE_OPENED, ...) or nullptr.
void TraceVfsLookup(const char* api, const OpenTarget& target, NTSTATUS status, const char* action,
                    HANDLE handle) noexcept;

}  // namespace krnl::io
