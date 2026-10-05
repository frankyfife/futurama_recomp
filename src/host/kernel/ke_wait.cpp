// Alertable waits and guest event objects: KeDelayExecutionThread, NtWaitForSingleObjectEx,
// NtWaitForSingleObject (slots 21, 20, 33; plan M2). Owned by the io agent since M2 (moved from the
// kernel agent with the APC work, see src/INTERFACES.md Changelog); see ke_wait.h.
//
// Design 1.7 "APCs": every guest thread has its own guest APC queue (kernel/io_apc.h). I/O APCs are
// delivered only in KeDelayExecutionThread(UserMode, Alertable = TRUE, ..) and
// NtWaitForSingleObjectEx(.., UserMode, Alertable = TRUE, ..); afterwards the wait returns
// STATUS_USER_APC (0xC0), which XAPI SleepEx 0xF64BE turns into WAIT_IO_COMPLETION. Alertable waits of
// the game: SleepEx(0, TRUE) at 0x13F4C0, 0x13F590 and 0xE0030 (files.md 2.4).
// With a single guest thread nothing can queue an APC while the thread sleeps (only the thread itself
// issues I/O), so delivery before the wait is sufficient; the queue is checked again afterwards anyway.
#include "kernel/ke_wait.h"

#include "core/guest.h"
#include "core/log.h"
#include "kernel/io_apc.h"
#include "kernel/io_common.h"
#include "kernel/ke_thread.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"
#include "kernel/kernel_hle.h"
#include "kernel/ob_handles.h"

namespace krnl {
namespace {

// Delivers queued I/O APCs of the calling thread if the wait may deliver user APCs.
bool DeliverApcs(CHAR waitMode, BOOLEAN alertable) noexcept {
    return DeliversUserApcs(waitMode, alertable) && io::DeliverIoApcs() > 0;
}

NTSTATUS StatusFromWin32Wait(DWORD r) noexcept {
    return r == WAIT_OBJECT_0 ? STATUS_SUCCESS : r == WAIT_TIMEOUT ? STATUS_TIMEOUT : STATUS_UNSUCCESSFUL;
}

NTSTATUS WaitOnHandle(HANDLE handle, CHAR waitMode, BOOLEAN alertable, const xbox::LARGE_INTEGER* timeout,
                      const char* api) noexcept {
    // NT order: the handle is referenced first (ObReferenceObjectByHandle), then the wait may deliver.
    Ref<KObject> object = HandleTable::Instance().Lookup(handle);
    if (!object) {
        FUT_LOGW(Krnl, "%s(%p): invalid handle -> STATUS_INVALID_HANDLE", api, handle);
        return STATUS_INVALID_HANDLE;
    }
    if (DeliverApcs(waitMode, alertable)) {
        return STATUS_USER_APC;
    }
    const std::uint32_t ms = TimeoutToMilliseconds(timeout);
    NTSTATUS status = STATUS_SUCCESS;
    if (auto* thread = dynamic_cast<ThreadObject*>(object.get())) {
        status = StatusFromWin32Wait(WaitForSingleObject(thread->HostHandle(), ms));
    } else if (auto* event = dynamic_cast<EventObject*>(object.get())) {
        if (ms == INFINITE && !event->TryWait()) {
            FUT_LOGW(Krnl, "%s(%p Event): waiting without timeout on an unsignaled event", api, handle);
            status = StatusFromWin32Wait(WaitForSingleObject(event->HostHandle(), INFINITE));
        } else if (ms != INFINITE) {
            status = StatusFromWin32Wait(WaitForSingleObject(event->HostHandle(), ms));
        }
    } else {
        // File objects: the HLE completes every transfer before NtReadFile/NtWriteFile return, so the
        // file object is always signaled (XAPI ReadFile/WriteFile 0xF723B/0xF731C only wait after
        // STATUS_PENDING on synchronous handles, which the io layer never returns).
        FUT_LOGT(Krnl, "%s(%p %s): object is always signaled in the HLE", api, handle, object->TypeName());
    }
    if (status == STATUS_SUCCESS || status == STATUS_TIMEOUT) {
        if (DeliverApcs(waitMode, alertable)) {
            status = STATUS_USER_APC;
        }
    }
    return status;
}

}  // namespace

// ---- EventObject ------------------------------------------------------------------------------------

EventObject::EventObject(Type type, bool signaled) noexcept
    : type_(type), host_(CreateEventW(nullptr, type == Type::Notification ? TRUE : FALSE,
                                      signaled ? TRUE : FALSE, nullptr)) {}

EventObject::~EventObject() {
    if (host_ != nullptr) {
        CloseHandle(host_);
    }
}

void EventObject::Set() noexcept {
    if (host_ != nullptr) {
        SetEvent(host_);
    }
}

void EventObject::Reset() noexcept {
    if (host_ != nullptr) {
        ResetEvent(host_);
    }
}

bool EventObject::TryWait() noexcept {
    return host_ != nullptr && WaitForSingleObject(host_, 0) == WAIT_OBJECT_0;
}

HANDLE CreateEventHandle(EventObject::Type type, bool signaled) noexcept {
    try {
        Ref<EventObject> event = MakeObject<EventObject>(type, signaled);
        if (event->HostHandle() == nullptr) {
            FUT_LOGE(Krnl, "CreateEventHandle: CreateEventW failed (%lu)", GetLastError());
            return nullptr;
        }
        return HandleTable::Instance().Insert(event);
    } catch (...) {
        return nullptr;
    }
}

Ref<EventObject> LookupEvent(HANDLE handle) noexcept {
    return HandleTable::Instance().LookupAs<EventObject>(handle);
}

// ---- wait exports ------------------------------------------------------------------------------------

// KeDelayExecutionThread - slot 21, Ord 99, stdcall, 3 args (purge 12):
// NTSTATUS (KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval).
// Evidence: kernel_imports.tsv; XAPI SleepEx 0xF64BE: KeDelayExecutionThread(1 = UserMode, bAlertable,
// &interval) at 0xF64E3..0xF64E9, repeats on 0x101 and maps 0xC0 to WAIT_IO_COMPLETION (startup.md
// 4.3); the two XPP callers (replaced library) pass KernelMode, FALSE (files.md 2.3). Design 1.7.
// Interval {0, 0x80000000} = INFINITE. Returns STATUS_USER_APC after delivering APCs, else
// STATUS_SUCCESS.
NTSTATUS __stdcall KeDelayExecutionThread(CHAR WaitMode, BOOLEAN Alertable,
                                          xbox::LARGE_INTEGER* Interval) noexcept {
    if (DeliverApcs(WaitMode, Alertable)) {
        FUT_LOGT(Krnl, "KeDelayExecutionThread(%d, %u): APCs delivered -> STATUS_USER_APC",
                 static_cast<int>(WaitMode), static_cast<unsigned>(Alertable));
        return STATUS_USER_APC;
    }
    const std::uint32_t ms = TimeoutToMilliseconds(Interval);
    if (ms == INFINITE) {
        FUT_LOGW(Krnl, "KeDelayExecutionThread(%d, %u, INFINITE): sleeping forever",
                 static_cast<int>(WaitMode), static_cast<unsigned>(Alertable));
    }
    Sleep(ms);
    if (DeliverApcs(WaitMode, Alertable)) {
        FUT_LOGT(Krnl, "KeDelayExecutionThread(%d, %u, %u ms): APCs delivered -> STATUS_USER_APC",
                 static_cast<int>(WaitMode), static_cast<unsigned>(Alertable), ms);
        return STATUS_USER_APC;
    }
    FUT_LOGT(Krnl, "KeDelayExecutionThread(%d, %u, %u ms) -> STATUS_SUCCESS", static_cast<int>(WaitMode),
             static_cast<unsigned>(Alertable), ms);
    return STATUS_SUCCESS;
}
KRNL_FUNC(KeDelayExecutionThread, &KeDelayExecutionThread);

// NtWaitForSingleObjectEx - slot 20, Ord 234, stdcall, 4 args (purge 16):
// NTSTATUS (HANDLE Handle, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout).
// Evidence: kernel_imports.tsv; XAPI WaitForSingleObjectEx 0xF647A: NtWaitForSingleObjectEx(h, 1 =
// UserMode, bAlertable, timeout) at 0xF648E..0xF6497 (repeats on 0x101); callers GetOverlappedResult
// (XMV, replaced) and D3D only; design 1.7. Thread handles wait on the host thread, event handles on
// the host event, file handles are always signaled.
NTSTATUS __stdcall NtWaitForSingleObjectEx(HANDLE Handle, CHAR WaitMode, BOOLEAN Alertable,
                                           xbox::LARGE_INTEGER* Timeout) noexcept {
    const NTSTATUS status = WaitOnHandle(Handle, WaitMode, Alertable, Timeout, "NtWaitForSingleObjectEx");
    FUT_LOGT(Krnl, "NtWaitForSingleObjectEx(%p, %d, %u) -> %s", Handle, static_cast<int>(WaitMode),
             static_cast<unsigned>(Alertable), io::StatusString(status).c_str());
    return status;
}
KRNL_FUNC(NtWaitForSingleObjectEx, &NtWaitForSingleObjectEx);

// NtWaitForSingleObject - slot 33, Ord 233, stdcall, 3 args (purge 12):
// NTSTATUS (HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout).
// Evidence: kernel_imports.tsv; XAPI ReadFile/WriteFile wait on the file handle after STATUS_PENDING
// with (h, 0, NULL) (0xF7236..0xF723B, 0xF731C; kernel.md 7 group 2); design 1.7.
// ASSUMPTION: the Xbox kernel forwards to NtWaitForSingleObjectEx(Handle, KernelMode, Alertable,
// Timeout) - there is no user mode on the Xbox, XAPI asks for user APCs explicitly with WaitMode 1 -
// so this wait never delivers I/O APCs (design 1.7: delivery only in KeDelayExecutionThread and
// NtWaitForSingleObjectEx). Both XAPI callers pass Alertable = FALSE anyway.
NTSTATUS __stdcall NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable,
                                         xbox::LARGE_INTEGER* Timeout) noexcept {
    const NTSTATUS status = WaitOnHandle(Handle, kKernelMode, Alertable, Timeout, "NtWaitForSingleObject");
    FUT_LOGT(Krnl, "NtWaitForSingleObject(%p, %u) -> %s", Handle, static_cast<unsigned>(Alertable),
             io::StatusString(status).c_str());
    return status;
}
KRNL_FUNC(NtWaitForSingleObject, &NtWaitForSingleObject);

}  // namespace krnl
